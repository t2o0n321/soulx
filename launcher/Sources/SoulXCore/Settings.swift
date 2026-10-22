import Foundation

/// GPU modes offered by the launcher. `metal` (experimental, corrupts textures) is left out on purpose.
public enum GPUMode: String, CaseIterable, Identifiable, Sendable {
    case host                         // the Mac's GPU (default)
    case hostNoVulkan = "host-novulkan" // compatibility: OpenGL ES only
    case software                     // CPU, slow
    case metal                        // only shown when an instance already uses it
    public var id: String { rawValue }
    public static var offered: [GPUMode] { [.host, .hostNoVulkan, .software] }
}

/// Screen size presets (portrait numbers; the CLI turns them for landscape instances).
public enum Resolution: String, CaseIterable, Identifiable, Sendable {
    case hd = "720p", fhd = "1080p", qhd = "1440p", custom
    public var id: String { rawValue }
    public var size: (width: Int, height: Int, dpi: Int)? {
        switch self {
        case .hd: return (720, 1600, 320)
        case .fhd: return (1080, 2400, 420)
        case .qhd: return (1440, 3200, 560)
        case .custom: return nil
        }
    }
    public static var presets: [Resolution] { [.hd, .fhd, .qhd] }
    public static func matching(width: Int, height: Int, dpi: Int) -> Resolution {
        let w = min(width, height), h = max(width, height)
        return presets.first { $0.size!.width == w && $0.size!.height == h && $0.size!.dpi == dpi } ?? .custom
    }
}

/// The CLI's three profiles (keep in sync with registry.c).
public enum Profile: String, CaseIterable, Identifiable, Sendable {
    case lite, balanced, performance
    public var id: String { rawValue }
    public var cores: Int { [.lite: 2, .balanced: 4, .performance: 6][self]! }
    public var ramMB: Int { [.lite: 4096, .balanced: 4096, .performance: 6144][self]! }
    public var resolution: Resolution { self == .lite ? .hd : .fhd }

    /// A sensible default for this Mac: 8 GB Macs get lite, 32 GB and more performance.
    public static func recommended(hostMemoryGB: Int) -> Profile {
        if hostMemoryGB <= 8 { return .lite }
        if hostMemoryGB >= 32 { return .performance }
        return .balanced
    }
}

/// Everything the launcher lets a user set on an instance.
public struct InstanceSettings: Equatable, Sendable {
    public var cores: Int
    public var ramMB: Int
    public var dataGB: Int
    public var resolution: Resolution
    public var width: Int          // portrait: width <= height
    public var height: Int
    public var dpi: Int
    public var landscape: Bool
    public var gpu: GPUMode
    public var dns: String         // "" = the Mac's
    public var stayAwake: Bool
    public var lean: Bool
    public var fps: Int
    public var audio: Bool
    public var rooted: Bool

    public init(cores: Int, ramMB: Int, dataGB: Int, resolution: Resolution, width: Int, height: Int, dpi: Int,
                landscape: Bool, gpu: GPUMode, dns: String, stayAwake: Bool, lean: Bool, fps: Int, audio: Bool,
                rooted: Bool = false) {
        self.cores = cores; self.ramMB = ramMB; self.dataGB = dataGB; self.resolution = resolution
        self.width = min(width, height); self.height = max(width, height); self.dpi = dpi
        self.landscape = landscape; self.gpu = gpu; self.dns = dns; self.stayAwake = stayAwake; self.lean = lean
        self.fps = fps; self.audio = audio; self.rooted = rooted
    }

    public static func defaults(profile: Profile) -> InstanceSettings {
        let s = profile.resolution.size!
        return InstanceSettings(cores: profile.cores, ramMB: profile.ramMB, dataGB: profile == .performance ? 16 : 8,
                                resolution: profile.resolution, width: s.width, height: s.height, dpi: s.dpi,
                                landscape: false, gpu: .host, dns: "", stayAwake: true, lean: true, fps: 60,
                                audio: true, rooted: false)
    }

    /// The profile these settings match (CPUs, RAM, screen), or nil for custom ones.
    public var profile: Profile? {
        Profile.allCases.first { $0.cores == cores && $0.ramMB == ramMB && $0.resolution == resolution }
    }

    public mutating func apply(_ profile: Profile) {
        cores = profile.cores
        ramMB = profile.ramMB
        setResolution(profile.resolution)
    }

    public mutating func setResolution(_ r: Resolution) {
        resolution = r
        if let s = r.size { width = s.width; height = s.height; dpi = s.dpi }
    }

    var resString: String { landscape ? "\(height)x\(width)" : "\(width)x\(height)" }

    /// Options for `soulx create <name> ...`.
    public func createArguments(name: String) -> [String] {
        var a = ["create", name, "--cores", "\(cores)", "--ram", "\(ramMB)", "--data", "\(dataGB)",
                 "--res", resString, "--dpi", "\(dpi)", "--gpu", gpu.rawValue, "--fps", "\(fps)"]
        if landscape { a.append("--landscape") }
        if !dns.isEmpty { a += ["--dns", dns] }
        if !stayAwake { a.append("--no-stay-awake") }
        if !lean { a.append("--no-lean") }
        if !audio { a.append("--no-audio") }
        if rooted { a.append("--root") }
        return a
    }

    /// Options for `soulx config <name> ... --json` that turn |old| into self (only what changed).
    public func configArguments(name: String, from old: InstanceSettings) -> [String] {
        var a: [String] = []
        if cores != old.cores { a += ["--cores", "\(cores)"] }
        if ramMB != old.ramMB { a += ["--ram", "\(ramMB)"] }
        if dataGB != old.dataGB { a += ["--data", "\(dataGB)"] }
        if width != old.width || height != old.height || landscape != old.landscape { a += ["--res", resString] }
        if landscape != old.landscape { a.append(landscape ? "--landscape" : "--portrait") }
        if dpi != old.dpi { a += ["--dpi", "\(dpi)"] }
        if gpu != old.gpu { a += ["--gpu", gpu.rawValue] }
        if dns != old.dns { a += dns.isEmpty ? ["--no-dns"] : ["--dns", dns] }
        if stayAwake != old.stayAwake { a.append(stayAwake ? "--stay-awake" : "--no-stay-awake") }
        if lean != old.lean { a.append(lean ? "--lean" : "--no-lean") }
        if fps != old.fps { a += ["--fps", "\(fps)"] }
        if audio != old.audio { a.append(audio ? "--audio" : "--no-audio") }
        // rooted is handled separately by AppModel.applyRoot() — it runs 'soulx root' / 'soulx root --unroot'
        return a.isEmpty ? [] : ["config", name] + a + ["--json"]
    }
}

/// Instance names the CLI accepts: 1-32 of a-z, 0-9, '-', '_', starting with a letter or digit, not all digits.
public func isValidInstanceName(_ s: String) -> Bool {
    guard (1...32).contains(s.count), let first = s.unicodeScalars.first else { return false }
    let ok = s.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" || $0 == "_" }
    let firstOK = ("a"..."z").contains(first) || ("0"..."9").contains(first)
    return ok && firstOK && !s.allSatisfy(\.isNumber)
}
