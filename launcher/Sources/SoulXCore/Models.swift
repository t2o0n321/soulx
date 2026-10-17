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
    public let serial: String?
    public let pid: Int?
    public let headless: Bool?

    public var running: Bool { state == "running" }

    enum CodingKeys: String, CodingKey {
        case number = "id", name, state, image, android, cores, ramMB = "ram_mb", dataGB = "data_gb", width, height,
             dpi, landscape, gpu, dns, stayAwake = "stay_awake", lean, fps, audio, serial, pid, headless
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
        // older CLIs did not report these two: their defaults
        fps = try c.decodeIfPresent(Int.self, forKey: .fps) ?? 60
        audio = try c.decodeIfPresent(Bool.self, forKey: .audio) ?? true
        serial = try c.decodeIfPresent(String.self, forKey: .serial)
        pid = try c.decodeIfPresent(Int.self, forKey: .pid)
        headless = try c.decodeIfPresent(Bool.self, forKey: .headless)
    }

    /// The instance's settings as an editable value.
    public var settings: InstanceSettings {
        InstanceSettings(cores: cores, ramMB: ramMB, dataGB: dataGB,
                         resolution: Resolution.matching(width: width, height: height, dpi: dpi),
                         width: width, height: height,
                         dpi: dpi, landscape: landscape, gpu: GPUMode(rawValue: gpu) ?? .host, dns: dns,
                         stayAwake: stayAwake, lean: lean, fps: fps, audio: audio)
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

/// `soulx volume --json`.
public struct VolumeInfo: Decodable, Sendable {
    public let name: String
    public let volume: Int        // percent
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
