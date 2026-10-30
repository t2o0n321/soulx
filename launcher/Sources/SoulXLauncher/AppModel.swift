import AppKit
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
    @Published var usage: [String: UsageInfo] = [:]       // guest RAM/storage, polled slowly (adb reads)
    @Published var guestIP: [String: String] = [:]        // each instance's wlan0 address, polled slowly
    @Published var publicIP: String?                      // the Mac's public (egress) IP — what NAT/Bridged guests appear as
    @Published var signInGPU: [String: GPUMode] = [:]     // instance -> GPU to restore after the Google sign-in helper
    @Published var busy: [String: String] = [:]          // instance -> what is going on ("Starting…")
    @Published var alert: String?
    @Published var setupLog: [String] = []
    @Published var settingUp = false
    @Published var repairing = false          // a forced reinstall (soulx setup --force) is running
    private var setupProcess: Process?
    @Published var transfer: VmTransfer?           // an export or import in progress (or just finished / failed)
    private var transferProcess: Process?
    private var setupCancelled = false
    private var refreshCount = 0
    @Published var language: LanguageChoice = .system   // shared with SoulX's window (soulx language)
    /// Instance name -> apps, as last seen while it was running. Lets AppsSection show a stopped
    /// instance's apps instead of just a "start it" message; cleared when the instance is deleted.
    @Published private(set) var appsCache: [String: [InstalledApp]] = [:]

    // Host-only / Bridged networking: the privileged helper's install state (nil = unknown) and, for Bridged,
    // the Mac network interfaces an instance can join.
    @Published var netHelperInstalled: Bool?
    @Published var macInterfaces: [String] = []
    @Published var installingNet = false

    let cli: SoulXCLI?
    private var statsProcess: Process?
    private var pollTask: Task<Void, Never>?
    private static let appsCacheKey = "appsCache"

    var hostMemoryGB: Int { Int(ProcessInfo.processInfo.physicalMemory / (1 << 30)) }

    init(cli: SoulXCLI? = SoulXCLI.locate()) {
        self.cli = cli
        if let data = UserDefaults.standard.data(forKey: Self.appsCacheKey),
           let decoded = try? JSONDecoder().decode([String: [InstalledApp]].self, from: data) {
            appsCache = decoded
        }
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

    /// Reads the saved language once at start (it also decides the window's language).
    func loadLanguage() async {
        struct Reply: Decodable { let language: String }
        if let r = try? await cli?.json(Reply.self, ["language", "--json"]), let c = LanguageChoice(rawValue: r.language) {
            applyLanguage(c)
            language = c
        }
    }

    func setLanguage(_ c: LanguageChoice) async {
        guard let cli else { return }
        let r = await cli.run(["language", c.rawValue])
        if !r.ok { alert = r.message; return }
        applyLanguage(c)
        language = c
    }

    func refresh() async {
        guard let cli else { return }
        if let list = try? await cli.json([Instance].self, ["list", "--json"]) {
            let wasRunningWithCapture = Dictionary(uniqueKeysWithValues:
                instances.filter { $0.running && $0.networkCapturePath != nil }.map { ($0.name, $0.networkCapturePath!) })
            instances = list
            // pick the first instance only once the runtime is known to be ready (else setup is the only page)
            if runtime?.ready == true, !repairing, selection != mcpSelection,
               selection == nil || !list.contains(where: { $0.name == selection }) {
                selection = list.first?.name
            }
            // A capture just finished (the instance stopped): show the .pcap in Finder.
            for (name, path) in wasRunningWithCapture where list.first(where: { $0.name == name })?.running != true {
                NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: path)])
            }
        }
        // Re-check a ready runtime now and then (about every 30 s), so files deleted by hand show the repair page.
        refreshCount += 1
        if runtime == nil || runtime?.ready == false || (!settingUp && refreshCount % 10 == 0) {
            if let r = try? await cli.json(RuntimeInfo.self, ["runtime", "--json"]) { runtime = r }
        }
        // while the runtime needs installing or repairing, nothing is selected (the setup page is the only page)
        if repairing || runtime?.ready == false { selection = nil }
    }

    private func startStats() {
        guard let cli else { return }
        statsProcess = cli.stream(["stats", "--watch", "--json", "--interval", "2000"], onLine: { [weak self] line in
            guard let s = try? JSONDecoder().decode(StatsSample.self, from: Data(line.utf8)) else { return }
            Task { @MainActor [weak self] in
                self?.host = s.host
                self?.stats = Dictionary(s.instances.map { ($0.name, $0) }, uniquingKeysWith: { a, _ in a })
            }
        }, onExit: { [weak self] _ in
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
        let r = await perform(name, L("Starting…"), ["start", name] + (headless ? ["--headless"] : []))
        if r?.ok == true, !headless { await activateWindowSoon(name) }
    }
    func stop(_ name: String) async { await perform(name, L("Stopping…"), ["stop", name]) }
    func restart(_ name: String) async {
        let r = await perform(name, L("Restarting…"), ["restart", name])
        if r?.ok == true { await activateWindowSoon(name) }     // a restart keeps whatever window mode it had
    }

    // MARK: Google sign-in helper
    // The Google / Play sign-in page is a Chromium WebView whose GPU process crashes under host graphics on this
    // emulator. Sign in once under software (compatibility) graphics, then switch back — no root, no tamper, just a
    // render mode; the account is saved for good.
    func beginGoogleSignIn(_ name: String) async {
        guard signInGPU[name] == nil, let inst = instance(name) else { return }
        let original = inst.settings.gpu
        signInGPU[name] = original
        if original != .software {
            if inst.running { await stop(name) }
            await perform(name, L("Switching to compatibility graphics…"), ["config", name, "--gpu", "software", "--json"])
        }
        await start(name)
        Task { await watchGoogleSignIn(name) }
    }

    func finishGoogleSignIn(_ name: String) async {
        guard let original = signInGPU.removeValue(forKey: name) else { return }
        if original != .software {
            if instance(name)?.running == true { await stop(name) }
            await perform(name, L("Switching graphics back…"), ["config", name, "--gpu", original.rawValue, "--json"])
        }
        await start(name)
    }

    /// While the helper is active, watch for a Google account and switch graphics back automatically once one is
    /// added, so the user does not have to come back and click.
    private func watchGoogleSignIn(_ name: String) async {
        while signInGPU[name] != nil {
            try? await Task.sleep(nanoseconds: 4_000_000_000)
            guard signInGPU[name] != nil else { return }
            if await hasGoogleAccount(name) { await finishGoogleSignIn(name); return }
        }
    }

    private func hasGoogleAccount(_ name: String) async -> Bool {
        guard let cli else { return false }
        let r = await cli.run(["-i", name, "shell", "dumpsys account"])
        return r.ok && r.stdout.contains("type=com.google")
    }
    /// Opens the instance's window, or brings it to the front if it is already open (behind the launcher,
    /// most often). The CLI's own "bring to front" runs as a background helper process, which macOS is prone to
    /// ignore; asking directly from the launcher (the app the user just clicked in) is what actually works.
    func openWindow(_ name: String) async {
        if let pid = instance(name)?.windowPid {
            activateWindow(pid: pid)
            return
        }
        await perform(name, "", ["window", name])
        await activateWindowSoon(name)
    }

    /// Polls briefly for the instance's window pid (freshly spawned, it can take a moment to register itself)
    /// and fronts it as soon as it shows up; gives up quietly if none appears (e.g. a headless instance).
    private func activateWindowSoon(_ name: String) async {
        for _ in 0..<10 {
            if let pid = instance(name)?.windowPid {
                activateWindow(pid: pid)
                return
            }
            try? await Task.sleep(nanoseconds: 300_000_000)
            await refresh()
        }
    }

    private func activateWindow(pid: Int) {
        guard pid > 0, let app = NSRunningApplication(processIdentifier: pid_t(pid)) else { return }
        if #available(macOS 14.0, *) {
            app.activate(options: [.activateAllWindows])
        } else {
            app.activate(options: [.activateAllWindows, .activateIgnoringOtherApps])
        }
    }
    func delete(_ name: String) async {
        let r = await perform(name, L("Deleting…"), ["delete", name, "--yes"])
        if r?.ok == true, appsCache.removeValue(forKey: name) != nil { saveAppsCache() }
    }

    func create(_ name: String, _ s: InstanceSettings) async -> Bool {
        let r = await perform(name, L("Creating…"), s.createArguments(name: name))
        guard r?.ok == true else { return false }
        selection = name
        // Root needs a boot + ramdisk patch, so set it up after the instance exists. Run it detached so the
        // create sheet closes right away and the step-by-step progress shows on the instance page (busy[name]).
        if s.rooted {
            Task { await applyRoot(name, enable: true) }
        }
        return true
    }

    /// Saves changed settings; returns a note for the user (or nil when nothing changed or it failed).
    func apply(_ name: String, from old: InstanceSettings, to new: InstanceSettings) async -> String? {
        let args = new.configArguments(name: name, from: old)
        guard !args.isEmpty else { return nil }
        let running = instance(name)?.running ?? false
        guard let r = await perform(name, L("Saving…"), args), r.ok else { return nil }
        return running ? L("Saved. The running instance gets the new settings when it restarts.") : L("Saved.")
    }

    private static let rootStepLabels: [Int: String] = [
        1: L("Downloading Magisk tools"),
        2: L("Starting instance"),
        3: L("Waiting for boot"),
        4: L("Pushing tools to device"),
        5: L("Patching ramdisk"),
        6: L("Pulling patched ramdisk"),
        7: L("Stopping instance"),
    ]

    /// Patches or unpatches the instance's ramdisk with Magisk. Called directly from the Root toggle.
    @discardableResult
    func applyRoot(_ name: String, enable: Bool) async -> String? {
        guard let cli else { return nil }
        if enable {
            let running = instance(name)?.running ?? false
            if running {
                await perform(name, L("Stopping…"), ["stop", name])
            }
            busy[name] = L("Setting up root…")
            let result: String? = await withCheckedContinuation { cont in
                var errors: [String] = []
                cli.stream(["root", name], onLine: { [weak self] line in
                    Task { @MainActor [weak self] in
                        guard let self else { return }
                        if line.hasPrefix("[root] ") {
                            let parts = line.dropFirst(7).split(separator: " ", maxSplits: 1)
                            if let frac = parts.first, let slash = frac.firstIndex(of: "/"),
                               let step = Int(frac[..<slash]), let total = Int(frac[frac.index(after: slash)...]) {
                                let label = Self.rootStepLabels[step] ?? String(parts.last ?? "")
                                self.busy[name] = "Root (\(step)/\(total)) \(label)…"
                            }
                        } else if line.hasPrefix("soulx: error: ") {
                            errors.append(String(line.dropFirst(14)))
                        }
                    }
                }, onExit: { [weak self] status in
                    Task { @MainActor [weak self] in
                        guard let self else { return }
                        self.busy[name] = nil
                        await self.refresh()
                        if status != 0 {
                            self.alert = errors.last ?? L("Error")
                            cont.resume(returning: nil)
                        } else {
                            cont.resume(returning: L("Root is ready. Start the instance to use it."))
                        }
                    }
                })
            }
            return result
        } else {
            guard let r = await perform(name, L("Removing root…"), ["root", name, "--unroot"]), r.ok else { return nil }
            return L("Root removed.")
        }
    }

    // --- apps (listing, installing and acting on them needs the instance running; the list is
    // also cached so a stopped instance can still show what was installed last time it ran) ---

    func apps(_ name: String) async -> [InstalledApp]? {
        guard let list = try? await cli?.json([InstalledApp].self, ["apps", "-i", name, "--json"]) else { return nil }
        appsCache[name] = list
        saveAppsCache()
        return list
    }

    private func saveAppsCache() {
        guard let data = try? JSONEncoder().encode(appsCache) else { return }
        UserDefaults.standard.set(data, forKey: Self.appsCacheKey)
    }

    /// Installs .apk/.xapk/.apks/.apkm files; true when every one went in.
    func installApps(_ name: String, _ urls: [URL]) async -> Bool {
        guard let cli, !urls.isEmpty else { return false }
        let r = await cli.run(["install", "-i", name] + urls.map(\.path))
        if !r.ok { alert = r.message }
        return r.ok
    }

    func appAction(_ name: String, _ command: String, _ package: String) async {
        guard let cli else { return }
        let r = await cli.run([command, "-i", name, package])
        if !r.ok { alert = r.message }
    }

    /// Starts recording an instance's network traffic live (no restart needed); `list --json`'s networkCapturePath
    /// reflects it once the next poll lands. Analysis: open the resulting .pcap in Wireshark.
    func startNetworkCapture(_ name: String) async {
        guard let cli else { return }
        let r = await cli.run(["network-capture-start", "-i", name])
        if !r.ok { alert = r.message }
        await refresh()
    }

    /// Stops the capture and reveals the finished .pcap in Finder right away (refresh()'s own diff also
    /// catches a capture that ended because the whole instance was stopped, without this being called first).
    func stopNetworkCapture(_ name: String) async {
        struct NetworkCapturePathReply: Decodable { let path: String }
        guard let cli else { return }
        do {
            let reply = try await cli.json(NetworkCapturePathReply.self, ["network-capture-stop", "-i", name, "--json"])
            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: reply.path)])
        } catch {
            alert = error.localizedDescription
        }
        await refresh()
    }

    // --- scripts (managed here, run from SoulX's window or `soulx run`) ---

    func scripts(_ name: String) async -> ScriptList? {
        try? await cli?.json(ScriptList.self, ["scripts", "-i", name, "--json"])
    }

    func importScripts(_ name: String, _ urls: [URL]) async -> ScriptList? {
        guard let cli, !urls.isEmpty else { return nil }
        do { return try await cli.json(ScriptList.self, ["scripts", "-i", name, "import"] + urls.map(\.path) + ["--json"]) }
        catch { alert = error.localizedDescription; return nil }
    }

    func removeScript(_ name: String, entry: String) async -> ScriptList? {
        guard let cli else { return nil }
        do { return try await cli.json(ScriptList.self, ["scripts", "-i", name, "remove", entry, "--json"]) }
        catch { alert = error.localizedDescription; return nil }
    }

    func renameScript(_ name: String, file: String, to title: String) async -> ScriptList? {
        guard let cli else { return nil }
        do { return try await cli.json(ScriptList.self, ["scripts", "-i", name, "rename", file, title, "--json"]) }
        catch { alert = error.localizedDescription; return nil }
    }

    private static let runNotification = NSNotification.Name("com.soulx.window.run")
    private static let stopNotification = NSNotification.Name("com.soulx.window.stop")

    /// Asks the instance's window to run one of its scripts — the same as picking "Run Script…" there, so the
    /// live parameter panel shows up too (a plain `soulx run` from here would not: SOULX_UI_DIR is only set up
    /// by the window itself). Opens the window first when none is open yet; gives up quietly if it never shows
    /// up (same as openWindow's own best-effort retry).
    func runScript(_ name: String, path: String) async {
        if instance(name)?.windowPid == nil {
            // `soulx window` opens one, or fronts a window that is already open. That window may not be reported
            // as this CLI's (a window started by another copy of SoulX), so the request is sent either way: the
            // window listens by instance name. A window that has just started gets a moment to listen.
            let r = await perform(name, "", ["window", name])
            guard r?.ok == true else { return }
            await activateWindowSoon(name)
            if instance(name)?.windowPid == nil { try? await Task.sleep(nanoseconds: 1_500_000_000) }
        }
        DistributedNotificationCenter.default().postNotificationName(Self.runNotification, object: name,
                                                                      userInfo: ["path": path], deliverImmediately: true)
    }

    /// Stops the script currently running on an instance (same as the window's own Stop / Cmd-.).
    func stopScript(_ name: String) {
        DistributedNotificationCenter.default().postNotificationName(Self.stopNotification, object: name,
                                                                      userInfo: nil, deliverImmediately: true)
    }

    /// Opens a macOS terminal running an interactive Android shell for the instance — `soulx shell` (uid 2000),
    /// or a root shell (`su`) when |root| is true. The instance must be running (adb needs a live device).
    /// We cannot run this through SoulXCLI (its process has no tty); instead we hand Terminal a tiny script that
    /// execs the CLI, so the user gets a real interactive shell.
    func openShell(_ name: String, root: Bool) {
        guard let cli else { return }
        // Single-quote for /bin/zsh so paths or names with spaces survive; '\'' escapes an embedded quote.
        func shq(_ s: String) -> String { "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'" }

        // `-t` forces adb to allocate a pty so `su` opens an interactive root shell rather than exiting at once.
        var command = "exec \(shq(cli.executable.path)) -i \(shq(name)) shell"
        if root { command += " -t su" }

        var script = "#!/bin/zsh\n"
        // Match the launcher's own SOULX_HOME (a fresh Terminal login shell would otherwise use the default home).
        if let home = ProcessInfo.processInfo.environment["SOULX_HOME"], !home.isEmpty {
            script += "export SOULX_HOME=\(shq(home))\n"
        }
        // Land the user straight in the Android shell: name the window, then wipe Terminal's login banner, the
        // echoed .command path and the user's prompt (screen + scrollback) so the session opens clean.
        let title = root ? "SoulX · \(name) · root" : "SoulX · \(name)"
        script += "printf '\\033]0;%s\\007' \(shq(title))\n"
        script += "clear; printf '\\033[3J'\n"
        script += command + "\n"

        let safe = String(name.map { ($0.isLetter || $0.isNumber || $0 == "-" || $0 == "_") ? $0 : "_" })
        let file = FileManager.default.temporaryDirectory
            .appendingPathComponent("soulx-shell-\(safe)\(root ? "-root" : "").command")
        do {
            try script.write(to: file, atomically: true, encoding: .utf8)
            try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: file.path)
        } catch {
            alert = error.localizedDescription
            return
        }
        let open = Process()
        open.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        open.arguments = ["-a", "Terminal", file.path]
        do { try open.run() } catch { alert = error.localizedDescription }
    }

    // --- Host-only / Bridged network helper (soulx_netd) ---

    private struct NetStatus: Decodable { let installed: Bool }

    /// Refresh whether the privileged network helper is installed, and the Mac interfaces Bridged can join.
    func refreshNetHelper() async {
        macInterfaces = Self.listMacInterfaces()
        netHelperInstalled = (try? await cli?.json(NetStatus.self, ["net", "status", "--json"]))?.installed
    }

    /// Install the network helper (one administrator-password prompt). Returns true on success.
    @discardableResult
    func installNetHelper() async -> Bool {
        guard let cli, !installingNet else { return false }
        installingNet = true
        defer { installingNet = false }
        let r = await cli.run(["net", "install"])
        await refreshNetHelper()
        if netHelperInstalled != true {
            alert = r.message.isEmpty ? L("Could not set up the network helper.") : r.message
        }
        return netHelperInstalled == true
    }

    /// Up, non-loopback `en*` interfaces that have an IPv4 address — the ones Bridged networking can join.
    static func listMacInterfaces() -> [String] {
        var out: [String] = [], seen = Set<String>()
        var ifap: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifap) == 0, let first = ifap else { return [] }
        defer { freeifaddrs(ifap) }
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard let sa = p.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            let flags = Int32(p.pointee.ifa_flags)
            guard flags & IFF_UP != 0, flags & IFF_LOOPBACK == 0 else { continue }
            let name = String(cString: p.pointee.ifa_name)
            if name.hasPrefix("en"), !seen.contains(name) { seen.insert(name); out.append(name) }
        }
        return out.sorted()
    }

    // --- AI control (MCP) ---

    @Published var mcp: McpConfig?

    func loadMcp() async {
        mcp = try? await cli?.json(McpConfig.self, ["mcp", "config", "--json"])
    }

    /// Changes AI control settings (`soulx mcp config ...`); the running servers follow on their next tool call.
    func setMcp(_ args: [String]) async {
        guard let cli else { return }
        do { mcp = try await cli.json(McpConfig.self, ["mcp", "config"] + args + ["--json"]) }
        catch { alert = error.localizedDescription }
    }

    func mcpClients() async -> [McpClientApp] {
        (try? await cli?.json([McpClientApp].self, ["mcp", "clients", "--json"])) ?? []
    }

    /// One click: add SoulX to the app's settings (also turns AI control on). Returns what to do next, or nil.
    func connectClient(_ id: String, connect: Bool) async -> String? {
        guard let cli else { return nil }
        let r = await cli.run(["mcp", connect ? "connect" : "disconnect", id])
        await loadMcp()
        if !r.ok { alert = r.message; return nil }
        return r.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    func mcpLog(page: Int) async -> McpLogPage? {
        try? await cli?.json(McpLogPage.self, ["mcp", "log", "--page", String(page), "--per-page", "20", "--json"])
    }

    func volume(_ name: String) async -> VolumeInfo? {
        try? await cli?.json(VolumeInfo.self, ["volume", "-i", name, "--json"])
    }

    func setVolume(_ name: String, percent: Int? = nil, mute: Bool? = nil) async -> VolumeInfo? {
        let arg = mute.map { $0 ? "mute" : "unmute" } ?? String(max(0, min(100, percent ?? 0)))
        return try? await cli?.json(VolumeInfo.self, ["volume", "-i", name, arg, "--json"])
    }

    /// The phone's own RAM and storage use (an adb read; poll it slowly, unlike the per-second `stats`).
    func usageInfo(_ name: String) async -> UsageInfo? {
        try? await cli?.json(UsageInfo.self, ["usage", "-i", name, "--json"])
    }

    /// The instance's wlan0 address (an adb read). It reflects the network mode: a 10.0.2.x NAT address,
    /// a 192.168.x host-only address, or a real LAN address when bridged.
    func fetchGuestIP(_ name: String) async -> String? {
        guard let cli else { return nil }
        let r = await cli.run(["-i", name, "shell", "ip -4 addr show wlan0"])
        guard r.ok else { return nil }
        return Self.firstIPv4(in: r.stdout)
    }

    /// The Mac's public (egress) IP — what a NAT or bridged guest appears as on the internet. Host-only has none.
    func refreshPublicIP() async {
        guard let url = URL(string: "https://api.ipify.org") else { return }
        let req = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 6)
        guard let (data, _) = try? await URLSession.shared.data(for: req) else { return }
        let s = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        if !s.isEmpty, Self.firstIPv4(in: s) == s { publicIP = s }
    }

    /// The first dotted-quad in some text (optionally on an `inet` line), e.g. "10.0.2.16".
    private static func firstIPv4(in text: String) -> String? {
        let re = try! NSRegularExpression(pattern: #"(?:inet\s+)?(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"#)
        let ns = text as NSString
        guard let m = re.firstMatch(in: text, range: NSRange(location: 0, length: ns.length)) else { return nil }
        return ns.substring(with: m.range(at: 1))
    }

    /// Installs the Android runtime (`soulx setup --accept-licenses`), streaming its progress into setupLog.
    // MARK: Export / import (.soulxvm)

    /// Saves a stopped instance as one file: `soulx export -i NAME FILE --force` (the save panel asked already).
    func exportInstance(_ name: String, to url: URL) {
        runTransfer(VmTransfer(kind: .export, name: name, file: url),
                    ["export", "-i", name, url.path, "--force"])
    }

    /// What an export holds, read before importing it.
    func inspectArchive(_ url: URL) async -> VmManifest? {
        guard let cli else { return nil }
        do {
            return try await cli.json(VmManifest.self, ["import", url.path, "--inspect", "--json"])
        } catch {
            let r = await cli.run(["import", url.path, "--inspect"])
            alert = r.message.isEmpty ? L("This file is not a SoulX export.") : r.message
            return nil
        }
    }

    func importArchive(_ url: URL, as name: String) {
        runTransfer(VmTransfer(kind: .import, name: name, file: url), ["import", url.path, "--name", name])
    }

    func cancelTransfer() {
        guard let p = transferProcess, p.isRunning else { transfer = nil; return }
        transfer?.cancelled = true
        p.interrupt()
    }

    private func runTransfer(_ t: VmTransfer, _ args: [String]) {
        guard let cli, transfer == nil || transfer?.finished == true else { return }
        transfer = t
        var errors: [String] = []
        transferProcess = cli.stream(args, onLine: { [weak self] line in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if line.hasPrefix("soulx: error: ") {
                    errors.append(String(line.dropFirst(14)))
                } else if let (phase, pct) = VmTransfer.parse(line) {
                    self.transfer?.phase = phase
                    self.transfer?.fraction = pct
                }
            }
        }, onExit: { [weak self] status in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.transferProcess = nil
                self.transfer?.finished = true
                if self.transfer?.cancelled == true {
                    self.transfer = nil
                } else if status != 0 {
                    self.transfer?.error = errors.isEmpty ? L("Error") : errors.joined(separator: "\n")
                } else {
                    self.transfer?.fraction = 1
                    await self.refresh()
                    if t.kind == .import { self.selection = t.name }
                }
            }
        })
    }

    /// Stops a running install or repair with SIGINT, which `soulx setup` handles (it stops between steps; a partial
    /// download is kept and resumed next time). Stopped while a package was being unpacked, the runtime checks as
    /// incomplete and the repair page offers to finish it.
    func cancelRuntimeInstall() {
        guard let p = setupProcess, p.isRunning else { return }
        setupCancelled = true
        p.interrupt()
    }

    /// `force`: reinstall every package, also ones that look complete (the manual "Repair runtime").
    func installRuntime(force: Bool = false) {
        guard let cli, !settingUp else { return }
        settingUp = true
        repairing = force
        if force { selection = nil }
        setupLog = []
        setupCancelled = false
        setupProcess = cli.stream(["setup", "--accept-licenses"] + (force ? ["--force"] : []), onLine: { [weak self] line in
            Task { @MainActor [weak self] in
                guard let self else { return }
                // progress lines repeat with new numbers: replace the previous one of the same package
                if let last = self.setupLog.last, last.contains("%"), last.hasPrefix(line.prefix(18)), line.contains("%") {
                    self.setupLog[self.setupLog.count - 1] = line
                } else {
                    self.setupLog.append(line)
                }
            }
        }, onExit: { [weak self] status in
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.settingUp = false
                self.repairing = false
                self.setupProcess = nil
                self.setupLog.append(self.setupCancelled ? L("Cancelled. The download resumes next time.")
                                     : status == 0 ? L("Installed.") : L("Installation failed."))
                self.runtime = nil
                await self.refresh()
            }
        })
    }
}

/// One export or import, as the progress sheet shows it.
struct VmTransfer {
    enum Kind { case export, `import` }
    let kind: Kind
    let name: String
    let file: URL
    var phase = ""
    var fraction: Double?
    var finished = false
    var cancelled = false
    var error: String?

    /// "Packing... 42%" -> (localized phase, 0.42); "Exporting Preparing..." -> (phase, nil).
    static func parse(_ line: String) -> (String, Double?)? {
        let phases: [(String, String)] = [
            ("Checking the files", "Checking the files"), ("Packing", "Packing"), ("Unpacking", "Unpacking"),
            ("Verifying", "Verifying the files"), ("Preparing", "Preparing"), ("Adding the instance", "Adding the instance"),
        ]
        for (key, label) in phases where line.contains(key) {
            var pct: Double?
            if let r = line.range(of: #"(\d{1,3})%"#, options: .regularExpression), let v = Double(line[r].dropLast()) {
                pct = min(v, 100) / 100
            }
            return (L(label), pct)
        }
        return nil
    }
}
