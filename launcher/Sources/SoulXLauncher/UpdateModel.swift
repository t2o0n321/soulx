import AppKit
import CoreServices
import SoulXCore

/// Looks for a new SoulX release on GitHub when SoulX opens (and every 6 hours while it stays open), offers it, and
/// installs it: by itself at start when "update automatically the next time SoulX opens" is on. Only SoulX.app has a
/// version to compare; a build run from source never checks.
@MainActor
final class UpdateModel: ObservableObject {
    enum Phase: Equatable {
        case idle                        // nothing going on (also: no update, after an automatic check)
        case checking                    // a check the user asked for
        case upToDate                    // the answer to that check
        case available                   // `update` is newer than this SoulX
        case downloading(Double)
        case installing
        case relaunching
        case failed(String)
    }

    static let autoUpdateKey = "autoUpdateOnLaunch"
    private static let checkInterval: UInt64 = 6 * 3600 * 1_000_000_000

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var update: AvailableUpdate?
    @Published var showSheet = false
    @Published var autoUpdate: Bool {
        didSet { UserDefaults.standard.set(autoUpdate, forKey: Self.autoUpdateKey) }
    }

    /// This SoulX's version; nil when not running as SoulX.app (development)
    let current: AppVersion?
    private let installer: UpdateInstaller?
    private var checks: Task<Void, Never>?
    private var announced: AppVersion?          // the sheet opens by itself once per new version

    init(bundle: Bundle = .main) {
        autoUpdate = UserDefaults.standard.bool(forKey: Self.autoUpdateKey)
        let version = (bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String).flatMap(AppVersion.init)
        if bundle.bundleURL.pathExtension == "app", let version {
            current = version
            installer = UpdateInstaller(appURL: bundle.bundleURL,
                                        bundleIdentifier: bundle.bundleIdentifier ?? "io.github.t2o0n321.soulx")
        } else {
            current = nil
            installer = nil
        }
    }

    private var userAgent: String { "SoulX/\(current?.plain ?? "development")" }
    var busy: Bool {
        switch phase {
        case .downloading, .installing, .relaunching: return true
        default: return false
        }
    }

    /// Checks now, then every 6 hours. Call once, when the window appears.
    func start() {
        guard current != nil, checks == nil else { return }
        checks = Task { [weak self] in
            await self?.check(userAsked: false, atStart: true)
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: Self.checkInterval)
                await self?.check(userAsked: false)
            }
        }
    }

    /// Asks GitHub for the latest release. A check the user asked for shows its answer; automatic ones stay quiet
    /// unless there is something new (offline or GitHub busy: the next check tries again).
    func check(userAsked: Bool, atStart: Bool = false) async {
        guard let current, !busy else { return }
        if userAsked {
            phase = .checking
            showSheet = true
        }
        do {
            let release = try await Updates.latestRelease(userAgent: userAgent)
            guard let found = Updates.update(in: release, current: current) else {
                update = nil
                phase = userAsked ? .upToDate : .idle
                return
            }
            update = found
            phase = .available
            if atStart && autoUpdate {
                showSheet = true
                await install()
            } else if userAsked || announced != found.version {
                announced = found.version
                showSheet = true
            }
        } catch {
            if userAsked { phase = .failed(String(format: L("Could not check for updates: %@"), describe(error))) }
        }
    }

    /// Downloads, checks and installs the update, then opens the new SoulX and quits this one.
    func install() async {
        guard let update, let installer, !busy else { return }
        if let problem = installer.problem() {
            phase = .failed(describe(problem))
            return
        }
        phase = .downloading(0)
        do {
            let app = try await installer.install(update, userAgent: userAgent) { fraction in
                Task { @MainActor [weak self] in
                    guard let self, case .downloading = self.phase else { return }
                    self.phase = fraction >= 1 ? .installing : .downloading(fraction)
                }
            }
            phase = .relaunching
            relaunch(app)
        } catch {
            phase = .failed(String(format: L("The update failed: %@"), describe(error)))
        }
    }

    /// "Later": hide the sheet; the sidebar keeps showing that an update is there.
    func dismiss() {
        guard !busy else { return }
        showSheet = false
        if case .failed = phase { phase = update == nil ? .idle : .available }
        if phase == .upToDate || phase == .checking { phase = .idle }
    }

    /// Opens the new SoulX once this one has quit. A small shell waits for this process to end (at most 10 s) and
    /// then opens the app, so the two never run side by side. The sheet closes first: with it still open (it cannot
    /// be dismissed while updating) the old SoulX was seen to stay open after terminate. A quit that still hangs ends
    /// with exit().
    private func relaunch(_ app: URL) {
        LSRegisterURL(app as CFURL, true)                    // Launch Services: the new version's Info.plist
        let opener = Process()
        opener.executableURL = URL(fileURLWithPath: "/bin/sh")
        opener.arguments = ["-c", "for i in $(seq 1 100); do kill -0 \"$1\" 2>/dev/null || break; sleep 0.1; done; "
                                  + "exec /usr/bin/open -n \"$2\"",
                            "soulx-relaunch", String(ProcessInfo.processInfo.processIdentifier), app.path]
        opener.standardInput = FileHandle.nullDevice
        opener.standardOutput = FileHandle.nullDevice
        opener.standardError = FileHandle.nullDevice
        do { try opener.run() } catch {
            phase = .failed(String(format: L("SoulX was updated but could not reopen: %@"), error.localizedDescription))
            return
        }
        showSheet = false
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { NSApp.terminate(nil) }
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { exit(0) }
    }

    private func describe(_ error: Error) -> String {
        guard let problem = error as? UpdateError else { return error.localizedDescription }
        switch problem {
        case .network(let detail): return String(format: L("Network problem (%@)."), detail)
        case .notAnApp: return L("Updates work in SoulX.app only.")
        case .temporaryLocation: return L("Move SoulX to your Applications folder first, then open it from there.")
        case .notWritable(let folder):
            return String(format: L("SoulX may not replace itself in %@. Download the new version from GitHub instead."), folder)
        case .checksumMissing, .checksumMismatch: return L("The download is damaged; nothing was changed.")
        case .badArchive, .wrongApp, .signature: return L("The download is not a valid SoulX; nothing was changed.")
        case .replace(let detail): return String(format: L("SoulX could not be replaced (%@)."), detail)
        }
    }
}
