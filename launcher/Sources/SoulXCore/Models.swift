import Foundation

/// One instance as `soulx list --json` / `soulx config --json` report it.
public struct Instance: Decodable, Identifiable, Equatable, Sendable {
    public var id: String { name }
    public let number: Int
    public let name: String
    public let state: String              // "running" | "stopped"
    public let image: String
    public let android: String?
    public let cores: Int
    public let ramMB: Int
    public let dataGB: Int
    public let width: Int
    public let height: Int
    public let dpi: Int
    public let landscape: Bool
    public let gpu: String
    public let dns: String
    public let stayAwake: Bool
    public let lean: Bool
    public let fps: Int
    public let audio: Bool
    public let rooted: Bool
    public let rootStatus: String?        // "active", "inactive", "ready", "missing", or nil
    public let serial: String?
    public let pid: Int?
    public let headless: Bool?
    public let windowPid: Int?            // the pid of SoulX's own window for this instance, nil when none is open
    public let runningScript: String?     // full path of the script the window is running there, nil when none
    public let networkCapturePath: String?          // this run's network capture file, nil when not recording
    public let filesDir: String?          // the folder with its disks and settings ($SOULX_HOME/avd/soulx_<name>.avd)

    public var running: Bool { state == "running" }

    enum CodingKeys: String, CodingKey {
        case number = "id", name, state, image, android, cores, ramMB = "ram_mb", dataGB = "data_gb", width, height,
             dpi, landscape, gpu, dns, stayAwake = "stay_awake", lean, fps, audio, rooted,
             rootStatus = "root_status", serial, pid, headless,
             windowPid = "window_pid", runningScript = "running_script", networkCapturePath = "network_capture_path",
             filesDir = "files_dir"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        number = try c.decode(Int.self, forKey: .number)
        name = try c.decode(String.self, forKey: .name)
        state = try c.decode(String.self, forKey: .state)
        image = try c.decode(String.self, forKey: .image)
        android = try c.decodeIfPresent(String.self, forKey: .android)
        cores = try c.decode(Int.self, forKey: .cores)
        ramMB = try c.decode(Int.self, forKey: .ramMB)
        dataGB = try c.decode(Int.self, forKey: .dataGB)
        width = try c.decode(Int.self, forKey: .width)
        height = try c.decode(Int.self, forKey: .height)
        dpi = try c.decode(Int.self, forKey: .dpi)
        landscape = try c.decode(Bool.self, forKey: .landscape)
        gpu = try c.decode(String.self, forKey: .gpu)
        dns = try c.decodeIfPresent(String.self, forKey: .dns) ?? ""
        stayAwake = try c.decode(Bool.self, forKey: .stayAwake)
        lean = try c.decode(Bool.self, forKey: .lean)
        // older CLIs did not report these: their defaults
        fps = try c.decodeIfPresent(Int.self, forKey: .fps) ?? 60
        audio = try c.decodeIfPresent(Bool.self, forKey: .audio) ?? true
        rooted = try c.decodeIfPresent(Bool.self, forKey: .rooted) ?? false
        rootStatus = try c.decodeIfPresent(String.self, forKey: .rootStatus)
        serial = try c.decodeIfPresent(String.self, forKey: .serial)
        pid = try c.decodeIfPresent(Int.self, forKey: .pid)
        headless = try c.decodeIfPresent(Bool.self, forKey: .headless)
        windowPid = try c.decodeIfPresent(Int.self, forKey: .windowPid)
        runningScript = try c.decodeIfPresent(String.self, forKey: .runningScript)
        networkCapturePath = try c.decodeIfPresent(String.self, forKey: .networkCapturePath)
        filesDir = try c.decodeIfPresent(String.self, forKey: .filesDir)
    }

    /// The instance's settings as an editable value.
    public var settings: InstanceSettings {
        InstanceSettings(cores: cores, ramMB: ramMB, dataGB: dataGB,
                         resolution: Resolution.matching(width: width, height: height, dpi: dpi),
                         width: width, height: height,
                         dpi: dpi, landscape: landscape, gpu: GPUMode(rawValue: gpu) ?? .host, dns: dns,
                         stayAwake: stayAwake, lean: lean, fps: fps, audio: audio, rooted: rooted)
    }
}

/// One line of `soulx stats --watch --json`.
public struct StatsSample: Decodable, Sendable {
    public struct Host: Decodable, Sendable { public let cpus: Int; public let memoryMB: Int
        enum CodingKeys: String, CodingKey { case cpus, memoryMB = "memory_mb" } }
    public struct Entry: Decodable, Sendable {
        public let name: String
        public let state: String
        public let cpuPercent: Double?
        public let memoryMB: Int?
        public let uptimeS: Int?
        enum CodingKeys: String, CodingKey {
            case name, state, cpuPercent = "cpu_percent", memoryMB = "memory_mb", uptimeS = "uptime_s"
        }
    }
    public let host: Host
    public let instances: [Entry]
}

/// `soulx usage <name> --json`: the phone's own (in-VM) RAM and storage use, read from inside Android, plus the
/// space its data takes on the Mac. A slower poll than `stats` because each read is an adb round-trip.
public struct UsageInfo: Decodable, Sendable {
    public let name: String
    public let memUsedMB: Int?         // guest RAM in use (MemTotal - MemAvailable)
    public let memTotalMB: Int?        // guest RAM total (a little under the configured amount)
    public let storageUsedMB: Int?     // guest /data used
    public let storageTotalMB: Int?    // guest /data total
    public let hostStorageMB: Int?     // the instance's userdata image on the Mac (host-side footprint)
    enum CodingKeys: String, CodingKey {
        case name, memUsedMB = "mem_used_mb", memTotalMB = "mem_total_mb",
             storageUsedMB = "storage_used_mb", storageTotalMB = "storage_total_mb", hostStorageMB = "host_storage_mb"
    }
}

/// `soulx volume --json`.
public struct VolumeInfo: Decodable, Sendable {
    public let name: String
    public let volume: Int        // percent
    public let level: Int         // Android's own steps: 0...max
    public let max: Int
    public let muted: Bool
    public let audio: Bool        // false: the instance has no sound device
}

/// `soulx runtime --json`.
public struct RuntimeInfo: Decodable, Sendable {
    public struct Package: Decodable, Sendable {
        public let key: String
        public let kind: String
        public let version: String
        public let downloadBytes: Int64
        public let installed: Bool
        public let description: String?
        enum CodingKeys: String, CodingKey {
            case key, kind, version, downloadBytes = "download_bytes", installed, description
        }
    }
    public let home: String
    public let defaultImage: String?
    public let ready: Bool
    public let packages: [Package]
    enum CodingKeys: String, CodingKey { case home, defaultImage = "default_image", ready, packages }

    /// Bytes still to download for the default runtime (engine, adb and the default image).
    public var missingBytes: Int64 {
        packages.filter { !$0.installed && ($0.kind != "image" || $0.key == defaultImage) }
            .reduce(0) { $0 + $1.downloadBytes }
    }
}

/// `soulx scripts --json`: one instance's scripts.
public struct ScriptList: Decodable, Sendable {
    public struct Script: Decodable, Identifiable, Sendable {
        public var id: String { file }
        public let file: String           // "farm.lua" or "pkg/main.lua"
        public let entry: String          // the top-level file or folder it belongs to
        public let path: String
        public let name: String?          // its "-- name:" line
        public let package: Bool
        public let size: Int64
        public let modified: Int64
        /// What to show: its name, else the file name without ".lua".
        public var title: String { name ?? (file as NSString).lastPathComponent.replacingOccurrences(of: ".lua", with: "") }
    }
    public let instance: String
    public let folder: String
    public let scripts: [Script]
}

/// `soulx mcp config --json`: AI control (MCP) settings.
public struct McpConfig: Decodable, Equatable, Sendable {
    public let enabled: Bool
    public let instances: String            // "*" or "a,b"
    public let allowStartStop: Bool
    public let allowInstall: Bool
    public let allowScripts: Bool
    public let allowLua: Bool
    public let command: String              // the soulx executable an AI client should start
    enum CodingKeys: String, CodingKey {
        case enabled, instances, allowStartStop = "allow_start_stop", allowInstall = "allow_install",
             allowScripts = "allow_scripts", allowLua = "allow_lua", command
    }
    public var allInstances: Bool { instances.trimmingCharacters(in: .whitespaces) == "*" }
    public var instanceList: [String] {
        allInstances ? [] : instances.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    }
}

/// One line of `soulx mcp log --json`.
public struct McpLogEntry: Decodable, Identifiable, Sendable {
    public var id: String { time + tool + instance + detail }
    public let time: String
    public let ok: Bool
    public let tool: String
    public let instance: String
    public let detail: String
}

/// One page of the AI activity log, newest first (`soulx mcp log --page P --json`).
public struct McpLogPage: Decodable, Sendable {
    public let total: Int
    public let page: Int
    public let pages: Int
    public let entries: [McpLogEntry]

    public static let empty = McpLogPage(total: 0, page: 1, pages: 1, entries: [])
}

/// One AI app from `soulx mcp clients --json`.
public struct McpClientApp: Decodable, Identifiable, Sendable {
    public let id: String
    public let name: String
    public let installed: Bool
    public let state: String          // "connected", "other", "no", "unreadable"
    public let config: String
}

/// One installed app from `soulx apps --json`; also cached on disk so AppsSection can show the last
/// known list while an instance is stopped.
public struct InstalledApp: Codable, Identifiable, Hashable, Sendable {
    public let package: String
    public let label: String?            // the app's name; nil when it could not be read
    public let running: Bool             // in the foreground right now; always false in a cached (stopped) entry
    public var id: String { package }
    public var title: String { label.flatMap { $0.isEmpty ? nil : $0 } ?? package }

    enum CodingKeys: String, CodingKey { case package, label, running }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        package = try c.decode(String.self, forKey: .package)
        label = try c.decodeIfPresent(String.self, forKey: .label)
        running = try c.decodeIfPresent(Bool.self, forKey: .running) ?? false
    }
}

/// `soulx import FILE --inspect --json`: what a .soulxvm export holds.
public struct VmManifest: Decodable, Sendable {
    public let name: String
    public let suggestedName: String
    public let nameTaken: Bool
    public let image: String
    public let imageKnown: Bool
    public let android: String
    public let soulxVersion: String
    public let exported: String
    public let bytes: Int64
    public let archiveBytes: Int64
    public let scripts: Int
    enum CodingKeys: String, CodingKey {
        case name, suggestedName = "suggested_name", nameTaken = "name_taken", image, imageKnown = "image_known",
             android, soulxVersion = "soulx_version", exported, bytes, archiveBytes = "archive_bytes", scripts
    }
}
