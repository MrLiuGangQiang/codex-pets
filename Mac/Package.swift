// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "CodeXPets",
    platforms: [
        .macOS(.v13)
    ],
    products: [
        .executable(name: "CodeXPets", targets: ["CodeXPetsMac"])
    ],
    targets: [
        .target(
            name: "CodeXPetsCore"
        ),
        .executableTarget(
            name: "CodeXPetsMac",
            dependencies: ["CodeXPetsCore"],
            resources: [
                .process("Resources")
            ]
        ),
        .testTarget(
            name: "CodeXPetsCoreTests",
            dependencies: ["CodeXPetsCore"]
        )
    ]
)
