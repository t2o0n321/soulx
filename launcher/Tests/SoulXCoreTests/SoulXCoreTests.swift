import Foundation
import Testing
@testable import SoulXCore

struct ModelsTests {
    // output of `soulx list --json` (one stopped, one running instance)
    let listJSON = """
    [{"id": 1, "name": "default", "state": "stopped", "image": "play", "android": "Android 17", "target": "android-37.0",
      "cores": 4, "ram_mb": 4096, "data_gb": 8, "width": 1080, "height": 2400, "dpi": 420, "landscape": false,
      "gpu": "host", "dns": "", "stay_awake": true, "lean": true, "fps": 60, "audio": true,
      "created": "2026-10-12T07:56:51Z", "serial": null, "console_port": null, "adb_port": null, "pid": null,
      "headless": null, "started": null},
     {"id": 2, "name": "racer", "state": "running", "image": "play", "android": "Android 17", "target": "android-37.0",
      "cores": 6, "ram_mb": 6144, "data_gb": 16, "width": 2400, "height": 1080, "dpi": 420, "landscape": true,
      "gpu": "host-novulkan", "dns": "1.1.1.1", "stay_awake": false, "lean": false, "fps": 30, "audio": false,
      "created": "2026-10-13T00:00:00Z", "serial": "emulator-5602", "console_port": 5602, "adb_port": 5603,
      "pid": 4242, "headless": true, "started": "2026-10-17T01:00:00Z"}]
    """

    @Test func testDecodeList() throws {
        let list = try JSONDecoder().decode([Instance].self, from: Data(listJSON.utf8))
        #expect(list.count == 2)
        #expect(!(list[0].running))
        #expect(list[1].running)
        #expect(list[1].pid == 4242)
        #expect(list[1].fps == 30)
        #expect(!(list[1].audio))
        // landscape panel 2400x1080 -> portrait numbers 1080x2400 in the settings
        let s = list[1].settings
        #expect(s.width == 1080)
        #expect(s.height == 2400)
        #expect(s.landscape)
        #expect(s.resolution == .fhd)
        #expect(s.gpu == .hostNoVulkan)
        #expect(list[0].settings.profile == .balanced)
        #expect(s.profile == .performance)
    }

    @Test func testOlderCLIWithoutFpsAudio() throws {
        let old = listJSON.replacingOccurrences(of: "\"fps\": 60, \"audio\": true,", with: "")
        let list = try JSONDecoder().decode([Instance].self, from: Data(old.utf8))
        #expect(list[0].fps == 60)
        #expect(list[0].audio)
    }

    @Test func testDecodeStatsAndRuntime() throws {
        let stats = """
        {"host": {"cpus": 10, "memory_mb": 16384}, "instances": [{"name": "default", "state": "running", "pid": 9,
         "cpu_percent": 43.5, "memory_mb": 6200, "uptime_s": 120, "cores": 4, "ram_mb": 4096, "fps": 60},
         {"name": "x", "state": "stopped", "pid": null, "cpu_percent": null, "memory_mb": null, "uptime_s": null}]}
        """
        let s = try JSONDecoder().decode(StatsSample.self, from: Data(stats.utf8))
        #expect(s.host.cpus == 10)
        #expect(s.instances[0].cpuPercent == 43.5)
        #expect(s.instances[1].memoryMB == nil)

        let rt = """
        {"home": "/x", "default_image": "play", "ready": false, "packages": [
         {"key": "emulator", "kind": "engine", "version": "37", "download_bytes": 100, "installed": true, "description": "e"},
         {"key": "platform-tools", "kind": "platform-tools", "version": "1", "download_bytes": 10, "installed": false, "description": null},
         {"key": "play", "kind": "image", "version": "r06", "download_bytes": 1000, "installed": false, "description": "p"},
         {"key": "google", "kind": "image", "version": "r06", "download_bytes": 5000, "installed": false, "description": "g"}]}
        """
        let r = try JSONDecoder().decode(RuntimeInfo.self, from: Data(rt.utf8))
        #expect(!(r.ready))
        #expect(r.missingBytes == 1010)          // adb + the default image, not the optional "google"
    }
}

struct SettingsTests {
    @Test func testCreateArguments() {
        var s = InstanceSettings.defaults(profile: .lite)
        s.landscape = true
        s.lean = false
        s.audio = false
        s.dns = "8.8.8.8"
        let a = s.createArguments(name: "bot")
        #expect(Array(a.prefix(2)) == ["create", "bot"])
        #expect(a.contains("--landscape"))
        #expect(a[a.firstIndex(of: "--res")! + 1] == "1600x720")   // landscape: wider than tall
        #expect(a.contains("--no-lean"))
        #expect(a.contains("--no-audio"))
        #expect(a[a.firstIndex(of: "--dns")! + 1] == "8.8.8.8")
        #expect(!(a.contains("--no-stay-awake")))
    }

    @Test func testConfigArgumentsOnlyChanges() {
        let old = InstanceSettings.defaults(profile: .balanced)
        #expect(old.configArguments(name: "default", from: old).isEmpty)     // nothing changed: no call
        var new = old
        new.apply(.performance)
        new.fps = 30
        new.dns = ""
        let a = new.configArguments(name: "default", from: old)
        #expect(Array(a.prefix(2)) == ["config", "default"])
        #expect(a.last == "--json")
        #expect(a[a.firstIndex(of: "--cores")! + 1] == "6")
        #expect(a[a.firstIndex(of: "--ram")! + 1] == "6144")
        #expect(a[a.firstIndex(of: "--fps")! + 1] == "30")
        #expect(!a.contains("--res"))                                   // same screen
        var turned = old
        turned.landscape = true
        let b = turned.configArguments(name: "default", from: old)
        #expect(b[b.firstIndex(of: "--res")! + 1] == "2400x1080")
        #expect(b.contains("--landscape"))
        var nodns = old
        nodns.dns = "1.1.1.1"
        #expect(old.configArguments(name: "d", from: nodns).contains("--no-dns"))
    }

    @Test func testProfilesAndNames() {
        #expect(Profile.recommended(hostMemoryGB: 8) == .lite)
        #expect(Profile.recommended(hostMemoryGB: 16) == .balanced)
        #expect(Profile.recommended(hostMemoryGB: 64) == .performance)
        #expect(Resolution.matching(width: 2400, height: 1080, dpi: 420) == .fhd)
        #expect(Resolution.matching(width: 1000, height: 2000, dpi: 420) == .custom)
        #expect(isValidInstanceName("my-game_2"))
        #expect(!(isValidInstanceName("123")))
        #expect(!(isValidInstanceName("Bad")))
        #expect(!(isValidInstanceName("-x")))
        #expect(!(isValidInstanceName(String(repeating: "a", count: 33))))
    }
}

struct CLITests {
    /// A fake soulx: prints its arguments, writes to stderr and exits with the status in $FAKE_STATUS.
    func makeFake() throws -> URL {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("soulx-fake-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("soulx")
        try """
        #!/bin/sh
        if [ "$1" = "fail" ]; then echo "soulx: error: no such instance 'x'" >&2; echo "usage: soulx x" >&2; exit 1; fi
        if [ "$1" = "json" ]; then echo '{"name": "d", "volume": 40, "level": 6, "max": 15, "muted": false, "audio": true}'; exit 0; fi
        if [ "$1" = "lines" ]; then printf 'one\\ntwo\\rtwo-final\\nthree\\n'; exit 3; fi
        echo "args: $*"
        """.write(to: url, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: url.path)
        return url
    }

    @Test func testRunJsonAndErrors() async throws {
        let cli = SoulXCLI(executable: try makeFake())
        let r = await cli.run(["start", "default"])
        #expect(r.ok)
        #expect(r.stdout == "args: start default\n")
        let v = try await cli.json(VolumeInfo.self, ["json"])
        #expect(v.volume == 40)
        let f = await cli.run(["fail"])
        #expect(!(f.ok))
        #expect(f.message == "no such instance 'x'")
        do {
            _ = try await cli.json(VolumeInfo.self, ["fail"])
            Issue.record("expected an error")
        } catch CLIError.failed(let m) {
            #expect(m == "no such instance 'x'")
        }
    }

    final class Box: @unchecked Sendable {
        var lines: [String] = []; var status: Int32 = 0; let lock = NSLock()
        func snapshot() -> ([String], Int32) { lock.lock(); defer { lock.unlock() }; return (lines, status) }
    }

    @Test func testStreamLines() async throws {
        let cli = SoulXCLI(executable: try makeFake())
        let box = Box()
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            cli.stream(["lines"], onLine: { l in box.lock.lock(); box.lines.append(l); box.lock.unlock() },
                       onExit: { s in box.status = s; done.resume() })
        }
        try await Task.sleep(nanoseconds: 200_000_000)
        let (lines, status) = box.snapshot()
        #expect(lines == ["one", "two-final", "three"])
        #expect(status == 3)
    }

    @Test func testLocate() throws {
        let fake = try makeFake()
        let cli = SoulXCLI.locate(environment: ["SOULX_CLI": fake.path], bundle: Bundle.main)
        #expect(cli?.executable.path == fake.path)
        // no SOULX_CLI: found from the source tree (any SwiftPM output layout)
        let repoRoot = fake.deletingLastPathComponent()
        let src = repoRoot.appendingPathComponent("launcher/Sources/SoulXCore/CLI.swift").path
        try FileManager.default.createDirectory(at: repoRoot.appendingPathComponent("build/macos"), withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: fake, to: repoRoot.appendingPathComponent("build/macos/soulx"))
        let found = SoulXCLI.locate(environment: [:], bundle: Bundle.main, sourceFile: src)
        #expect(found?.executable.path == repoRoot.appendingPathComponent("build/macos/soulx").path)
    }
}
