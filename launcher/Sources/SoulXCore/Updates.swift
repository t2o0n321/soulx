import CryptoKit
import Foundation

// Updates: SoulX.app asks GitHub for the latest release, and installs it when the user agrees (or by itself when
// "update the next time SoulX opens" is on). The release workflow publishes SoulX-v<version>.zip (SoulX.app),
// .dmg and .sha256 for every tag (.github/workflows/release.yml).

/// A SoulX version: "26.10.1", or the tag "v26.10.1" (YY.M.PATCH, see scripts/versioning/).
public struct AppVersion: Comparable, Hashable, CustomStringConvertible, Sendable {
    public let numbers: [Int]

    public init?(_ text: String) {
        var rest = Substring(text.trimmingCharacters(in: .whitespacesAndNewlines))
        if rest.first == "v" || rest.first == "V" { rest = rest.dropFirst() }
        let parts = rest.split(separator: ".", omittingEmptySubsequences: false)
        guard (1...4).contains(parts.count) else { return nil }
        var numbers: [Int] = []
        for part in parts {
            guard !part.isEmpty, part.allSatisfy({ $0.isASCII && $0.isNumber }), let n = Int(part) else { return nil }
            numbers.append(n)
        }
        self.numbers = numbers
    }

    /// "26.10.1", as in VERSION and Info.plist
    public var plain: String { numbers.map(String.init).joined(separator: ".") }
    /// "v26.10.1", as people see it (tags, file names, the UI)
    public var description: String { "v" + plain }

    private var padded: [Int] { numbers + Array(repeating: 0, count: max(0, 4 - numbers.count)) }
    public static func == (a: AppVersion, b: AppVersion) -> Bool { a.padded == b.padded }
    public func hash(into hasher: inout Hasher) { hasher.combine(padded) }
    public static func < (a: AppVersion, b: AppVersion) -> Bool { a.padded.lexicographicallyPrecedes(b.padded) }
}

/// A GitHub release as the REST API returns it (only what SoulX uses).
public struct GitHubRelease: Decodable, Equatable, Sendable {
    public struct Asset: Decodable, Equatable, Sendable {
        public let name: String
        public let size: Int
        public let downloadURL: URL
        /// "sha256:<hex>", computed by GitHub (missing in older API replies)
        public let digest: String?

        enum CodingKeys: String, CodingKey { case name, size, digest, downloadURL = "browser_download_url" }

        public init(name: String, size: Int, downloadURL: URL, digest: String?) {
            self.name = name
            self.size = size
            self.downloadURL = downloadURL
            self.digest = digest
        }
    }

    public let tag: String
    public let title: String?
    public let notes: String?
    public let page: URL
    public let draft: Bool
    public let prerelease: Bool
    public let assets: [Asset]

    enum CodingKeys: String, CodingKey {
        case tag = "tag_name", title = "name", notes = "body", page = "html_url", draft, prerelease, assets
    }
}

/// A newer release that can be installed.
public struct AvailableUpdate: Equatable, Sendable {
    public let version: AppVersion
    public let page: URL
    /// What changed (the release text without the install instructions)
    public let notes: String
    /// SoulX-v<version>.zip: SoulX.app
    public let archive: GitHubRelease.Asset
    /// SoulX-v<version>.sha256 (shasum -a 256 of the .dmg and .zip)
    public let checksums: GitHubRelease.Asset?
}

public enum UpdateError: Error, Equatable, LocalizedError {
    case network(String)
    case notAnApp                       // run from a build folder, not SoulX.app
    case temporaryLocation              // run from a disk image or a quarantined download (App Translocation)
    case notWritable(String)            // the folder that holds SoulX.app
    case checksumMissing
    case checksumMismatch
    case badArchive(String)
    case wrongApp(String)
    case signature(String)
    case replace(String)

    public var errorDescription: String? {
        switch self {
        case .network(let m): return "Network: \(m)"
        case .notAnApp: return "This is not SoulX.app (a development build)."
        case .temporaryLocation: return "SoulX runs from a temporary location (a disk image or Downloads)."
        case .notWritable(let folder): return "SoulX may not replace itself in \(folder)."
        case .checksumMissing: return "The release has no checksum for the download."
        case .checksumMismatch: return "The download is damaged (its checksum does not match)."
        case .badArchive(let m): return "The download is not a SoulX.app archive: \(m)"
        case .wrongApp(let m): return "The downloaded app is not the expected SoulX: \(m)"
        case .signature(let m): return "The downloaded app's signature is broken: \(m)"
        case .replace(let m): return "SoulX could not be replaced: \(m)"
        }
    }
}

public enum Updates {
    public static let repository = "t2o0n321/soulx"
    public static let latestReleaseAPI = URL(string: "https://api.github.com/repos/\(repository)/releases/latest")!

    /// The update |release| offers to SoulX |current|: nil when it is not newer, is a draft or a pre-release, or has
    /// no SoulX-v<version>.zip.
    public static func update(in release: GitHubRelease, current: AppVersion) -> AvailableUpdate? {
        guard !release.draft, !release.prerelease, let version = AppVersion(release.tag), version > current,
              let archive = release.assets.first(where: { $0.name == "SoulX-\(version).zip" }) else { return nil }
        let checksums = release.assets.first { $0.name == "SoulX-\(version).sha256" }
        return AvailableUpdate(version: version, page: release.page, notes: changes(in: release.notes),
                               archive: archive, checksums: checksums)
    }

    /// A release's text without the install instructions every release ends with (scripts/release_notes.sh), with
    /// the CHANGELOG's wrapped lines joined again (a line that continues the one above is indented).
    public static func changes(in notes: String?) -> String {
        guard let notes else { return "" }
        let text = "\n" + notes.replacingOccurrences(of: "\r\n", with: "\n")
        let changes = text.range(of: "\n## Install").map { String(text[..<$0.lowerBound]) } ?? text
        var lines: [String] = []
        for line in changes.split(separator: "\n", omittingEmptySubsequences: false) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix(" "), !trimmed.isEmpty, !trimmed.hasPrefix("- "), let last = lines.last, !last.isEmpty {
                lines[lines.count - 1] = last + " " + trimmed
            } else {
                lines.append(String(line))
            }
        }
        return lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// The SHA-256 of |file| in a `shasum -a 256` listing (one "<hex>  <name>" per line), lowercased.
    public static func checksum(of file: String, inListing listing: String) -> String? {
        for line in listing.split(whereSeparator: \.isNewline) {
            let fields = line.split(separator: " ", omittingEmptySubsequences: true)
            guard fields.count >= 2 else { continue }
            var name = String(fields[fields.count - 1])
            if name.hasPrefix("*") { name.removeFirst() }            // shasum's binary-mode marker
            if name == file, isSHA256(fields[0]) { return fields[0].lowercased() }
        }
        return nil
    }

    /// The hex SHA-256 in a GitHub asset digest ("sha256:<hex>").
    public static func sha256(fromDigest digest: String?) -> String? {
        guard let digest, digest.hasPrefix("sha256:") else { return nil }
        let hex = digest.dropFirst("sha256:".count)
        return isSHA256(hex) ? hex.lowercased() : nil
    }

    private static func isSHA256<S: StringProtocol>(_ text: S) -> Bool {
        text.count == 64 && text.allSatisfy(\.isHexDigit)
    }

    /// Asks GitHub for the latest release: one request, nothing about the user is sent.
    public static func latestRelease(session: URLSession = .shared, userAgent: String) async throws -> GitHubRelease {
        var request = URLRequest(url: latestReleaseAPI, timeoutInterval: 20)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        let (data, response) = try await session.data(for: request)
        guard let status = (response as? HTTPURLResponse)?.statusCode else { throw UpdateError.network("no reply") }
        guard status == 200 else { throw UpdateError.network("GitHub replied \(status)") }
        do { return try JSONDecoder().decode(GitHubRelease.self, from: data) } catch {
            throw UpdateError.network("unexpected reply from GitHub")
        }
    }
}

/// Replaces the running SoulX.app with a release's. Each step is checked before anything is replaced: the download's
/// SHA-256 against the release's, the unpacked app's identifier and version, and its signature. The old app is
/// swapped out in one rename, so an interrupted update leaves the old SoulX in place.
public struct UpdateInstaller: Sendable {
    public let appURL: URL
    public let bundleIdentifier: String

    public init(appURL: URL, bundleIdentifier: String) {
        self.appURL = appURL
        self.bundleIdentifier = bundleIdentifier
    }

    /// Why SoulX cannot update itself where it runs, or nil when it can.
    public func problem(fileManager: FileManager = .default) -> UpdateError? {
        guard appURL.pathExtension == "app" else { return .notAnApp }
        let path = appURL.path
        if path.contains("/AppTranslocation/") { return .temporaryLocation }
        let folder = appURL.deletingLastPathComponent().path
        guard fileManager.isWritableFile(atPath: folder), fileManager.isWritableFile(atPath: path) else {
            return path.hasPrefix("/Volumes/") ? .temporaryLocation : .notWritable(folder)
        }
        return nil
    }

    /// Downloads, checks and installs |update|. |progress| gets 0...1 while it downloads. Returns the installed app.
    public func install(_ update: AvailableUpdate, session: URLSession = .shared, userAgent: String,
                        progress: @escaping @Sendable (Double) -> Void) async throws -> URL {
        if let problem = problem() { throw problem }
        let fileManager = FileManager.default
        // a scratch folder on the app's volume, so that the final swap is a rename
        let staging: URL
        do {
            staging = try fileManager.url(for: .itemReplacementDirectory, in: .userDomainMask, appropriateFor: appURL,
                                          create: true)
        } catch { throw UpdateError.replace(error.localizedDescription) }
        defer { try? fileManager.removeItem(at: staging) }

        let archive = staging.appendingPathComponent(update.archive.name)
        try await Self.download(update.archive.downloadURL, to: archive, expectedSize: update.archive.size,
                                session: session, userAgent: userAgent, progress: progress)
        let expected = try await expectedChecksums(update, session: session, userAgent: userAgent)
        let actual = try Self.sha256(of: archive)
        guard expected.allSatisfy({ $0 == actual }) else { throw UpdateError.checksumMismatch }

        let unpacked = staging.appendingPathComponent("unpacked")
        do { try Self.run("/usr/bin/ditto", ["-x", "-k", archive.path, unpacked.path]) } catch {
            throw UpdateError.badArchive(error.localizedDescription)
        }
        let apps = (try? fileManager.contentsOfDirectory(at: unpacked, includingPropertiesForKeys: nil))?
            .filter { $0.pathExtension == "app" } ?? []
        guard apps.count == 1, let newApp = apps.first else { throw UpdateError.badArchive("no single app inside") }
        try validate(newApp, version: update.version)
        do {
            _ = try fileManager.replaceItemAt(appURL, withItemAt: newApp, backupItemName: nil,
                                              options: .usingNewMetadataOnly)
        } catch { throw UpdateError.replace(error.localizedDescription) }
        return appURL
    }

    /// The unpacked app must be this SoulX (same identifier), the release's version, and correctly signed.
    public func validate(_ app: URL, version: AppVersion) throws {
        guard let info = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")) as? [String: Any]
        else { throw UpdateError.badArchive("no Info.plist") }
        guard info["CFBundleIdentifier"] as? String == bundleIdentifier else {
            throw UpdateError.wrongApp("identifier \(info["CFBundleIdentifier"] as? String ?? "-")")
        }
        guard let found = (info["CFBundleShortVersionString"] as? String).flatMap(AppVersion.init), found == version else {
            throw UpdateError.wrongApp("version \(info["CFBundleShortVersionString"] as? String ?? "-")")
        }
        // the check the release workflow makes on every build (SoulX is signed ad hoc, see scripts/package_macos.sh)
        do { try Self.run("/usr/bin/codesign", ["--verify", "--strict", "--deep", app.path]) } catch {
            throw UpdateError.signature(error.localizedDescription)
        }
    }

    /// The SHA-256s the release gives for the archive: GitHub's digest and the release's own .sha256 listing.
    private func expectedChecksums(_ update: AvailableUpdate, session: URLSession, userAgent: String) async throws
        -> [String] {
        var sums: [String] = []
        if let digest = Updates.sha256(fromDigest: update.archive.digest) { sums.append(digest) }
        if let listing = update.checksums {
            var request = URLRequest(url: listing.downloadURL, timeoutInterval: 30)
            request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
            if let (data, response) = try? await session.data(for: request),
               (response as? HTTPURLResponse)?.statusCode == 200,
               let sum = Updates.checksum(of: update.archive.name, inListing: String(decoding: data, as: UTF8.self)) {
                sums.append(sum)
            }
        }
        guard !sums.isEmpty else { throw UpdateError.checksumMissing }
        return sums
    }

    static func download(_ url: URL, to file: URL, expectedSize: Int, session: URLSession, userAgent: String,
                         progress: @escaping @Sendable (Double) -> Void) async throws {
        var request = URLRequest(url: url, timeoutInterval: 60)
        request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        let bytes: URLSession.AsyncBytes
        let response: URLResponse
        do { (bytes, response) = try await session.bytes(for: request) } catch {
            throw UpdateError.network(error.localizedDescription)
        }
        guard let status = (response as? HTTPURLResponse)?.statusCode, status == 200 else {
            throw UpdateError.network("download failed")
        }
        let total = max(Int(response.expectedContentLength), expectedSize, 1)
        guard FileManager.default.createFile(atPath: file.path, contents: nil),
              let handle = try? FileHandle(forWritingTo: file) else { throw UpdateError.replace("cannot write \(file.path)") }
        defer { try? handle.close() }
        var buffer = Data()
        buffer.reserveCapacity(1 << 16)
        var received = 0
        do {
            for try await byte in bytes {
                buffer.append(byte)
                if buffer.count == 1 << 16 {
                    try handle.write(contentsOf: buffer)
                    received += buffer.count
                    buffer.removeAll(keepingCapacity: true)
                    progress(min(1, Double(received) / Double(total)))
                }
            }
            if !buffer.isEmpty { try handle.write(contentsOf: buffer); received += buffer.count }
        } catch { throw UpdateError.network(error.localizedDescription) }
        if expectedSize > 0, received != expectedSize { throw UpdateError.network("incomplete download") }
        progress(1)
    }

    static func sha256(of file: URL) throws -> String {
        guard let handle = try? FileHandle(forReadingFrom: file) else { throw UpdateError.badArchive("unreadable") }
        defer { try? handle.close() }
        var hasher = SHA256()
        while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty { hasher.update(data: chunk) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    struct CommandFailure: Error, LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    /// Runs a system tool and waits for it; throws its error output when it fails.
    static func run(_ tool: String, _ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = arguments
        let errors = Pipe()
        process.standardError = errors
        process.standardOutput = FileHandle.nullDevice
        try process.run()
        let output = errors.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            let text = String(decoding: output, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
            throw CommandFailure(message: text.isEmpty ? "\(tool) failed (\(process.terminationStatus))" : text)
        }
    }
}
