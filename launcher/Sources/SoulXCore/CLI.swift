import Foundation

/// Result of one `soulx` invocation.
public struct CLIResult: Sendable {
    public let status: Int32
    public let stdout: String
    public let stderr: String
    public var ok: Bool { status == 0 }
    /// The CLI's own error text ("soulx: error: ..."), without the prefix, for showing to the user.
    public var message: String {
        let text = stderr.isEmpty ? stdout : stderr
        return text.split(separator: "\n")
            .map { $0.replacingOccurrences(of: "soulx: error: ", with: "").replacingOccurrences(of: "soulx: ", with: "") }
            .filter { !$0.hasPrefix("usage:") && !$0.hasPrefix("Run 'soulx help") }
            .joined(separator: "\n")
    }
}

public enum CLIError: Error, LocalizedError {
    case notFound
    case failed(String)
    case badOutput(String)
    public var errorDescription: String? {
        switch self {
        case .notFound: return "The soulx command was not found."
        case .failed(let m): return m
        case .badOutput(let m): return "Unexpected output from soulx: \(m)"
        }
    }
}

/// Runs the `soulx` CLI. All launcher actions go through here.
public final class SoulXCLI: @unchecked Sendable {
    public let executable: URL

    public init(executable: URL) { self.executable = executable }

    /// Where the CLI is: $SOULX_CLI, next to the launcher in the app bundle, the repo's build output (development),
    /// then the usual install locations.
    public static func locate(environment: [String: String] = ProcessInfo.processInfo.environment,
                              bundle: Bundle = .main,
                              fileManager: FileManager = .default) -> SoulXCLI? {
        var candidates: [URL] = []
        if let p = environment["SOULX_CLI"], !p.isEmpty { candidates.append(URL(fileURLWithPath: p)) }
        if let r = bundle.resourceURL { candidates.append(r.appendingPathComponent("soulx")) }
        if let exe = bundle.executableURL {
            // development: launcher/.build/<config>/SoulXLauncher -> ../build/macos/soulx
            var dir = exe.deletingLastPathComponent()
            for _ in 0..<5 {
                candidates.append(dir.appendingPathComponent("build/macos/soulx"))
                dir = dir.deletingLastPathComponent()
            }
        }
        candidates += ["/usr/local/bin/soulx", "/opt/homebrew/bin/soulx"].map(URL.init(fileURLWithPath:))
        for c in candidates where fileManager.isExecutableFile(atPath: c.path) { return SoulXCLI(executable: c) }
        return nil
    }

    private func makeProcess(_ args: [String]) -> Process {
        let p = Process()
        p.executableURL = executable
        p.arguments = args
        var env = ProcessInfo.processInfo.environment
        env["SOULX_FROM_LAUNCHER"] = "1"
        p.environment = env
        p.standardInput = FileHandle.nullDevice
        return p
    }

    /// Runs `soulx args...` to completion off the main thread.
    public func run(_ args: [String]) async -> CLIResult {
        await withCheckedContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                let p = self.makeProcess(args)
                let out = Pipe(), err = Pipe()
                p.standardOutput = out
                p.standardError = err
                do { try p.run() } catch {
                    cont.resume(returning: CLIResult(status: -1, stdout: "", stderr: error.localizedDescription))
                    return
                }
                // read both pipes while the process runs (a full pipe would block it)
                var outData = Data(), errData = Data()
                let group = DispatchGroup()
                group.enter()
                DispatchQueue.global().async { outData = out.fileHandleForReading.readDataToEndOfFile(); group.leave() }
                group.enter()
                DispatchQueue.global().async { errData = err.fileHandleForReading.readDataToEndOfFile(); group.leave() }
                p.waitUntilExit()
                group.wait()
                cont.resume(returning: CLIResult(status: p.terminationStatus,
                                                 stdout: String(decoding: outData, as: UTF8.self),
                                                 stderr: String(decoding: errData, as: UTF8.self)))
            }
        }
    }

    /// Runs a command that prints JSON on stdout and decodes it.
    public func json<T: Decodable>(_ type: T.Type, _ args: [String]) async throws -> T {
        let r = await run(args)
        guard r.ok else { throw CLIError.failed(r.message) }
        do { return try JSONDecoder().decode(T.self, from: Data(r.stdout.utf8)) } catch {
            throw CLIError.badOutput(String(r.stdout.prefix(200)))
        }
    }

    /// Starts a long-running command and calls |onLine| for every line it prints (stdout and stderr), and
    /// |onExit| with its status. Returns the process so the caller can stop it (terminate()).
    @discardableResult
    public func stream(_ args: [String], onLine: @escaping @Sendable (String) -> Void,
                       onExit: @escaping @Sendable (Int32) -> Void) -> Process? {
        let p = makeProcess(args)
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        var buffer = Data()
        let lock = NSLock()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let chunk = h.availableData
            guard !chunk.isEmpty else { return }
            lock.lock()
            buffer.append(chunk)
            var lines: [String] = []
            while let nl = buffer.firstIndex(of: 0x0A) {
                // progress bars rewrite one line with "\r": keep its latest state
                let raw = String(decoding: buffer[..<nl], as: UTF8.self)
                lines.append(raw.split(separator: "\r").last.map(String.init) ?? raw)
                buffer.removeSubrange(...nl)
            }
            lock.unlock()
            lines.forEach(onLine)
        }
        p.terminationHandler = { proc in
            pipe.fileHandleForReading.readabilityHandler = nil
            onExit(proc.terminationStatus)
        }
        do { try p.run() } catch { onExit(-1); return nil }
        return p
    }
}
