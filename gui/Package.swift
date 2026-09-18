// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "CleanMetadataGUI",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "CleanMetadataGUI",
            path: "Sources/CleanMetadataGUI"
        )
    ]
)
