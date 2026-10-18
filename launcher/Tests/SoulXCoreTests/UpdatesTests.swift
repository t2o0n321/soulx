import CryptoKit
import Foundation
import Testing
@testable import SoulXCore

struct VersionTests {
    @Test func testParse() {
        #expect(AppVersion("26.10.1")?.numbers == [26, 10, 1])
        #expect(AppVersion("v26.10.1")?.numbers == [26, 10, 1])
        #expect(AppVersion(" v26.10.0\n")?.plain == "26.10.0")
        #expect(AppVersion("26.10")?.numbers == [26, 10])
        #expect(AppVersion("v26.10.1")?.description == "v26.10.1")
        for bad in ["", "v", "26.10.x", "26..1", "-1.0.0", "1.2.3.4.5", "26.10.1-beta", "２６.10.1"] {
            #expect(AppVersion(bad) == nil, "\(bad)")
        }
    }

    @Test func testCompare() throws {
        let v = { (s: String) throws -> AppVersion in try #require(AppVersion(s)) }
        #expect(try v("26.10.1") > v("26.10.0"))
        #expect(try v("26.11.0") > v("26.10.9"))
        #expect(try v("27.1.0") > v("26.12.5"))
        #expect(try v("26.10.10") > v("26.10.9"))          // numbers, not text
        #expect(try v("26.10") == v("26.10.0"))
        #expect(try !(v("26.10.0") < v("v26.10.0")))
    }
}

struct ReleaseTests {
    // trimmed from GET https://api.github.com/repos/t2o0n321/soulx/releases/latest
    func releaseJSON(tag: String = "v26.10.1", draft: Bool = false, prerelease: Bool = false, zip: Bool = true) -> String {
        let v = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
        let zipAsset = zip ? """
          {"name": "SoulX-v\(v).zip", "size": 2116091, "digest": "sha256:A23B1E1B00D07D690EA6551F699750C57AA960899FD9C4986E8CB44468EFBDF4",
           "browser_download_url": "https://github.com/t2o0n321/soulx/releases/download/\(tag)/SoulX-v\(v).zip"},
        """ : ""
        return """
        {"tag_name": "\(tag)", "name": "SoulX v\(v)", "html_url": "https://github.com/t2o0n321/soulx/releases/tag/\(tag)",
         "draft": \(draft), "prerelease": \(prerelease),
         "body": "- **Updates**: SoulX looks for new versions.\\r\\n- Scripts: OCR reads Chinese.\\r\\n\\r\\n## Install\\r\\n\\r\\nRequires a Mac.\\r\\n## 安裝\\r\\n需要 Mac。",
         "assets": [
          {"name": "SoulX-v\(v).dmg", "size": 2262846, "digest": null,
           "browser_download_url": "https://github.com/t2o0n321/soulx/releases/download/\(tag)/SoulX-v\(v).dmg"},
          \(zipAsset)
          {"name": "SoulX-v\(v).sha256", "size": 170,
           "browser_download_url": "https://github.com/t2o0n321/soulx/releases/download/\(tag)/SoulX-v\(v).sha256"}]}
        """
    }

    func decode(_ json: String) throws -> GitHubRelease {
        try JSONDecoder().decode(GitHubRelease.self, from: Data(json.utf8))
    }

    @Test func testNewerRelease() throws {
        let release = try decode(releaseJSON())
        #expect(release.assets.count == 3)
        #expect(release.assets[0].digest == nil)
        let current = try #require(AppVersion("26.10.0"))
        let update = try #require(Updates.update(in: release, current: current))
        #expect(update.version.description == "v26.10.1")
        #expect(update.archive.name == "SoulX-v26.10.1.zip")
        #expect(update.checksums?.name == "SoulX-v26.10.1.sha256")
        #expect(update.page.absoluteString == "https://github.com/t2o0n321/soulx/releases/tag/v26.10.1")
        #expect(update.notes == "- **Updates**: SoulX looks for new versions.\n- Scripts: OCR reads Chinese.")
        #expect(Updates.sha256(fromDigest: update.archive.digest)
                == "a23b1e1b00d07d690ea6551f699750c57aa960899fd9c4986e8cb44468efbdf4")
    }

    @Test func testNoUpdate() throws {
        let current = try #require(AppVersion("26.10.1"))
        #expect(Updates.update(in: try decode(releaseJSON(tag: "v26.10.1")), current: current) == nil)   // same
        #expect(Updates.update(in: try decode(releaseJSON(tag: "v26.10.0")), current: current) == nil)   // older
        #expect(Updates.update(in: try decode(releaseJSON(tag: "v26.11.0", draft: true)), current: current) == nil)
        #expect(Updates.update(in: try decode(releaseJSON(tag: "v26.11.0", prerelease: true)), current: current) == nil)
        #expect(Updates.update(in: try decode(releaseJSON(tag: "v26.11.0", zip: false)), current: current) == nil)
        #expect(Updates.update(in: try decode(releaseJSON(tag: "nightly")), current: current) == nil)
    }

    @Test func testChecksums() {
        let listing = """
        64d645f75d91a3c501c5777ada12f65ba7745e3e6d1dcb0938a77b342d7af253  SoulX-v26.10.0.dmg
        A23B1E1B00D07D690EA6551F699750C57AA960899FD9C4986E8CB44468EFBDF4 *SoulX-v26.10.0.zip
        """
        #expect(Updates.checksum(of: "SoulX-v26.10.0.zip", inListing: listing)
                == "a23b1e1b00d07d690ea6551f699750c57aa960899fd9c4986e8cb44468efbdf4")
        #expect(Updates.checksum(of: "SoulX-v26.10.0.dmg", inListing: listing)?.hasPrefix("64d645") == true)
        #expect(Updates.checksum(of: "SoulX-v26.10.1.zip", inListing: listing) == nil)
        #expect(Updates.checksum(of: "x", inListing: "nothing here\nshort x") == nil)
        #expect(Updates.sha256(fromDigest: "sha1:abc") == nil)
        #expect(Updates.sha256(fromDigest: "sha256:1234") == nil)
        #expect(Updates.changes(in: "## Install\nRequires a Mac.") == "")
        // CHANGELOG lines wrap at 120 columns, continued with an indent
        #expect(Updates.changes(in: "First release.\n\n- **SoulX.app**: open it, start (with or without a\n  window), stop.\n  - nested\n- Two")
                == "First release.\n\n- **SoulX.app**: open it, start (with or without a window), stop.\n  - nested\n- Two")
        #expect(Updates.changes(in: nil) == "")
    }
}

/// Serves canned replies to URLSession, so the installer can be tested without the network.
final class StubProtocol: URLProtocol {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var replies: [String: Data] = [:]

    static func serve(_ replies: [String: Data]) {
        lock.lock(); defer { lock.unlock() }
        self.replies = replies
    }

    static func session() -> URLSession {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [StubProtocol.self]
        return URLSession(configuration: configuration)
    }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func stopLoading() {}
    override func startLoading() {
        guard let url = request.url else { return }
        Self.lock.lock()
        let data = Self.replies[url.absoluteString]
        Self.lock.unlock()
        let response = HTTPURLResponse(url: url, statusCode: data == nil ? 404 : 200, httpVersion: "HTTP/1.1",
                                       headerFields: ["Content-Length": "\(data?.count ?? 0)"])!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        if let data { client?.urlProtocol(self, didLoad: data) }
        client?.urlProtocolDidFinishLoading(self)
    }
}

@Suite(.serialized) struct InstallerTests {
    let identifier = "io.github.t2o0n321.soulx"

    func scratch() throws -> URL {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("soulx-update-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    func shell(_ tool: String, _ args: [String]) throws {
        try UpdateInstaller.run(tool, args)
    }

    /// A tiny signed SoulX.app: Info.plist, an executable (a copy of /usr/bin/true) and an ad hoc signature.
    func makeApp(in folder: URL, version: String, identifier: String? = nil, sign: Bool = true) throws -> URL {
        let app = folder.appendingPathComponent("SoulX.app")
        let macOS = app.appendingPathComponent("Contents/MacOS")
        try FileManager.default.createDirectory(at: macOS, withIntermediateDirectories: true)
        let info: [String: Any] = ["CFBundleIdentifier": identifier ?? self.identifier, "CFBundleExecutable": "SoulX",
                                   "CFBundleShortVersionString": version, "CFBundlePackageType": "APPL",
                                   "CFBundleName": "SoulX"]
        let plist = try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0)
        try plist.write(to: app.appendingPathComponent("Contents/Info.plist"))
        try FileManager.default.copyItem(at: URL(fileURLWithPath: "/usr/bin/true"), to: macOS.appendingPathComponent("SoulX"))
        if sign { try shell("/usr/bin/codesign", ["--force", "--sign", "-", app.path]) }
        return app
    }

    func installedVersion(_ app: URL) -> String? {
        (NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")) as? [String: Any])?[
            "CFBundleShortVersionString"] as? String
    }

    @Test func testProblem() throws {
        let dir = try scratch()
        defer { try? FileManager.default.removeItem(at: dir) }
        let app = try makeApp(in: dir, version: "26.10.0", sign: false)
        #expect(UpdateInstaller(appURL: app, bundleIdentifier: identifier).problem() == nil)
        #expect(UpdateInstaller(appURL: dir, bundleIdentifier: identifier).problem() == .notAnApp)
        let moved = URL(fileURLWithPath: "/private/var/folders/x/AppTranslocation/ABC/d/SoulX.app")
        #expect(UpdateInstaller(appURL: moved, bundleIdentifier: identifier).problem() == .temporaryLocation)
    }

    @Test func testValidate() throws {
        let dir = try scratch()
        defer { try? FileManager.default.removeItem(at: dir) }
        let installer = UpdateInstaller(appURL: dir.appendingPathComponent("Installed/SoulX.app"), bundleIdentifier: identifier)
        let v = try #require(AppVersion("26.10.1"))
        let good = try makeApp(in: dir.appendingPathComponent("good"), version: "26.10.1")
        try installer.validate(good, version: v)
        let older = try makeApp(in: dir.appendingPathComponent("older"), version: "26.10.0")
        #expect(throws: UpdateError.wrongApp("version 26.10.0")) { try installer.validate(older, version: v) }
        let other = try makeApp(in: dir.appendingPathComponent("other"), version: "26.10.1", identifier: "com.example.other")
        #expect(throws: UpdateError.wrongApp("identifier com.example.other")) { try installer.validate(other, version: v) }
        let unsigned = try makeApp(in: dir.appendingPathComponent("unsigned"), version: "26.10.1", sign: false)
        #expect(throws: (any Error).self) { try installer.validate(unsigned, version: v) }
    }

    @Test func testInstall() async throws {
        let dir = try scratch()
        defer { try? FileManager.default.removeItem(at: dir) }
        let installed = try makeApp(in: dir.appendingPathComponent("Applications"), version: "26.10.0")
        let newApp = try makeApp(in: dir.appendingPathComponent("build"), version: "26.10.1")
        let zip = dir.appendingPathComponent("SoulX-v26.10.1.zip")
        try shell("/usr/bin/ditto", ["-c", "-k", "--keepParent", newApp.path, zip.path])
        let data = try Data(contentsOf: zip)
        let sum = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()

        let base = "https://github.com/t2o0n321/soulx/releases/download/v26.10.1/"
        let archive = GitHubRelease.Asset(name: "SoulX-v26.10.1.zip", size: data.count,
                                          downloadURL: URL(string: base + "SoulX-v26.10.1.zip")!, digest: "sha256:" + sum)
        let listing = GitHubRelease.Asset(name: "SoulX-v26.10.1.sha256", size: 100,
                                          downloadURL: URL(string: base + "SoulX-v26.10.1.sha256")!, digest: nil)
        let update = AvailableUpdate(version: try #require(AppVersion("26.10.1")),
                                     page: URL(string: "https://github.com/t2o0n321/soulx/releases/tag/v26.10.1")!,
                                     notes: "", archive: archive, checksums: listing)
        StubProtocol.serve([archive.downloadURL.absoluteString: data,
                            listing.downloadURL.absoluteString: Data("\(sum)  SoulX-v26.10.1.zip\n".utf8)])
        let installer = UpdateInstaller(appURL: installed, bundleIdentifier: identifier)
        let progress = Progress()
        let result = try await installer.install(update, session: StubProtocol.session(), userAgent: "test") { f in
            progress.completedUnitCount = Int64(f * 100)
        }
        #expect(result == installed)
        #expect(installedVersion(installed) == "26.10.1")
        #expect(progress.completedUnitCount == 100)

        // a damaged download: nothing changes
        let older = try makeApp(in: dir.appendingPathComponent("Applications2"), version: "26.10.0")
        StubProtocol.serve([archive.downloadURL.absoluteString: data + Data("x".utf8)])
        let damaged = AvailableUpdate(version: update.version, page: update.page, notes: "",
                                      archive: GitHubRelease.Asset(name: archive.name, size: data.count + 1,
                                                                   downloadURL: archive.downloadURL, digest: "sha256:" + sum),
                                      checksums: nil)
        await #expect(throws: UpdateError.checksumMismatch) {
            _ = try await UpdateInstaller(appURL: older, bundleIdentifier: identifier)
                .install(damaged, session: StubProtocol.session(), userAgent: "test") { _ in }
        }
        #expect(installedVersion(older) == "26.10.0")
    }
}
