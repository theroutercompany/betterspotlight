// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "BetterSpotlight",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "BetterSpotlight", targets: ["App"]),
        .library(name: "BetterSpotlightCore", targets: ["Core"]),
        .library(name: "BetterSpotlightShared", targets: ["Shared"]),
    ],
    dependencies: [
        .package(url: "https://github.com/stephencelis/SQLite.swift.git", from: "0.15.0"),
    ],
    targets: [
        // MARK: - App Target
        .executableTarget(
            name: "App",
            dependencies: [
                "Core",
                "Shared",
                "Services",
            ],
            path: "App"
        ),

        // MARK: - Core Library
        .target(
            name: "Core",
            dependencies: [
                "Shared",
                .product(name: "SQLite", package: "SQLite.swift"),
            ],
            path: "Core"
        ),

        // MARK: - Services (XPC)
        .target(
            name: "Services",
            dependencies: [
                "Core",
                "Shared",
            ],
            path: "Services"
        ),

        // MARK: - Shared Models & IPC
        .target(
            name: "Shared",
            dependencies: [],
            path: "Shared"
        ),

        // MARK: - Tests
        .testTarget(
            name: "CoreTests",
            dependencies: ["Core"],
            path: "Tests/Unit"
        ),
        .testTarget(
            name: "IntegrationTests",
            dependencies: ["Core", "Services"],
            path: "Tests/Integration"
        ),
    ]
)
