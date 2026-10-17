// swift-tools-version:5.10
import PackageDescription

// SoulX launcher: a SwiftUI front end for the `soulx` CLI. It talks to SoulX only through the CLI's JSON output
// (`list`, `status`, `config`, `stats`, `volume`, `runtime` with --json), never through SoulX's files.
let package = Package(
    name: "SoulXLauncher",
    platforms: [.macOS(.v14)],
    targets: [
        // CLI access, JSON models and argument building: no UI, unit-tested
        .target(name: "SoulXCore", path: "Sources/SoulXCore"),
        .executableTarget(name: "SoulXLauncher", dependencies: ["SoulXCore"], path: "Sources/SoulXLauncher"),
        .testTarget(name: "SoulXCoreTests", dependencies: ["SoulXCore"], path: "Tests/SoulXCoreTests"),
    ]
)
