// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "RecompIA",
    platforms: [.macOS(.v13)],
    products: [.executable(name: "RecompIA", targets: ["RecompIA"])],
    targets: [
        .target(name: "PortKit"),
        .executableTarget(name: "RecompIA", dependencies: ["PortKit"]),
        .testTarget(name: "PortKitTests", dependencies: ["PortKit"]),
    ]
)
