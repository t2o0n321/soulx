import Foundation
import SoulXCore

/// The launcher's state. Everything comes from the soulx CLI: `list --json` (every 3 s), `runtime --json`,
/// and a long-running `stats --watch --json` for live CPU and memory.
@MainActor
final class AppModel: ObservableObject {
    @Published var instances: [Instance] = []
    @Published var runtime: RuntimeInfo?
    @Published var selection: String?
    @Published var stats: [String: StatsSample.Entry] = [:]
    @Published var host: StatsSample.Host?
    @Published var busy: [String: String] = [:]          // instance -> what is going on ("Starting…")
    @Published var alert: String?
    @Published var setupLog: [String] = []
    @Published var settingUp = false

    let cli: SoulXCLI?
    private var statsProcess: Process?
    private var pollTask: Task<Void, Never>?

    var hostMemoryGB: Int { Int(ProcessInfo.processInfo.physicalMemory / (1 << 30)) }

    init(cli: SoulXCLI? = SoulXCLI.locate()) {
        self.cli = cli
    }

    func startPolling() {
        guard cli != nil, pollTask == nil else { return }
        startStats()
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.refresh()
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
    }

    func refresh() async {
        guard let cli else { return }
        if let list = try? await cli.json([Instance].self, ["list", "--json"]) {
            instances = list
            if selection == nil || !list.contains(where: { $0.name == selection }) { selection = list.first?.name }
        }
        if runtime == nil || runtime?.ready == false {
            runtime = try? await cli.json(RuntimeInfo.self, ["runtime", "--json"])
        }
    }

    private func startStats() {
        guard let cli else { return }
        statsProcess = cli.stream(["stats", "--watch", "--json", "--interval", "2000"], onLine: { line in
            guard let s = try? JSONDecoder().decode(StatsSample.self, from: Data(line.utf8)) else { return }
            Task { @MainActor [weak self] in
                self?.host = s.host
                self?.stats = Dictionary(s.instances.map { ($0.name, $0) }, uniquingKeysWith: { a, _ in a })
            }
        }, onExit: { _ in
            Task { @MainActor [weak self] in       // restart it if it ends (it should run until we quit)
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                self?.startStats()
            }
        })
    }

    func shutdown() {
        pollTask?.cancel()
        statsProcess?.terminate()
    }

    func instance(_ name: String?) -> Instance? { instances.first { $0.name == name } }

    /// Runs a CLI action for an instance, marking it busy; shows the CLI's message when it fails.
    @discardableResult
    func perform(_ name: String, _ label: String, _ args: [String]) async -> CLIResult? {
        guard let cli else { return nil }
        busy[name] = label
        defer { busy[name] = nil }
        let r = await cli.run(args)
        if !r.ok { alert = r.message.isEmpty ? L("Error") : r.message }
        await refresh()
        return r
    }

    func start(_ name: String, headless: Bool = false) async {
        await perform(name, L("Starting…"), ["start", name] + (headless ? ["--headless"] : []))
    }
    func stop(_ name: String) async { await perform(name, L("Stopping…"), ["stop", name]) }
    func restart(_ name: String) async { await perform(name, L("Restarting…"), ["restart", name]) }
    func openWindow(_ name: String) async { await perform(name, "", ["window", name]) }
    func delete(_ name: String) async { await perform(name, L("Deleting…"), ["delete", name, "--yes"]) }

    func create(_ name: String, _ s: InstanceSettings) async -> Bool {
        let r = await perform(name, L("Creating…"), s.createArguments(name: name))
        if r?.ok == true { selection = name; return true }
        return false
    }

    /// Saves changed settings; returns a note for the user (or nil when nothing changed or it failed).
    func apply(_ name: String, from old: InstanceSettings, to new: InstanceSettings) async -> String? {
        let args = new.configArguments(name: name, from: old)
        guard !args.isEmpty else { return nil }
        let running = instance(name)?.running ?? false
        guard let r = await perform(name, L("Saving…"), args), r.ok else { return nil }
        return running ? L("Saved. The running instance gets the new settings when it restarts.") : L("Saved.")
    }

    func volume(_ name: String) async -> VolumeInfo? {
        try? await cli?.json(VolumeInfo.self, ["volume", "-i", name, "--json"])
    }

    func setVolume(_ name: String, percent: Int? = nil, mute: Bool? = nil) async -> VolumeInfo? {
        let arg = mute.map { $0 ? "mute" : "unmute" } ?? String(max(0, min(100, percent ?? 0)))
        return try? await cli?.json(VolumeInfo.self, ["volume", "-i", name, arg, "--json"])
    }

    /// Installs the Android runtime (`soulx setup --accept-licenses`), streaming its progress into setupLog.
    func installRuntime() {
        guard let cli, !settingUp else { return }
        settingUp = true
        setupLog = []
        cli.stream(["setup", "--accept-licenses"], onLine: { line in
            Task { @MainActor [weak self] in
                guard let self else { return }
                // progress lines repeat with new numbers: replace the previous one of the same package
                if let last = self.setupLog.last, last.hasPrefix(line.prefix(18)), line.contains("%") {
                    self.setupLog[self.setupLog.count - 1] = line
                } else {
                    self.setupLog.append(line)
                }
            }
        }, onExit: { status in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.settingUp = false
                self.setupLog.append(status == 0 ? L("Installed.") : L("Installation failed."))
                self.runtime = nil
                await self.refresh()
            }
        })
    }
}
