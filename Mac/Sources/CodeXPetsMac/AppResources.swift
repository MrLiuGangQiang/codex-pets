import AppKit
import CodeXPetsCore
import Foundation

struct AppInfo {
    static let productName = "CodeXPets"
    static let repository = URL(string: "https://github.com/MrLiuGangQiang/codex-pets")!

    static var version: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "3.0.1"
    }

    static var displayName: String { "\(productName) v\(version)" }
}

enum AppResources {
    static func url(name: String, extension ext: String) -> URL? {
        Bundle.main.url(forResource: name, withExtension: ext) ??
            Bundle.module.url(forResource: name, withExtension: ext)
    }

    static func image(named name: String) -> NSImage? {
        guard let url = url(name: name, extension: "png") else { return nil }
        return NSImage(contentsOf: url)
    }

    static func catImage(state: ReminderState, frame: Int) -> NSImage? {
        let stateName: String
        switch state {
        case .idle: stateName = "idle"
        case .busy: stateName = "busy"
        case .completed: stateName = "completed"
        case .error: stateName = "error"
        }
        return image(named: "cat-\(stateName)-\(max(0, min(7, frame)))")
    }

    static func dockImage(index: Int) -> NSImage? {
        image(named: "dock-\(max(0, min(7, index)))")
    }

    static func missingResources() -> [String] {
        var missing: [String] = []
        for state in ["idle", "completed", "busy", "error"] {
            for frame in 0..<8 where url(name: "cat-\(state)-\(frame)", extension: "png") == nil {
                missing.append("cat-\(state)-\(frame).png")
            }
        }
        for index in 0..<8 where url(name: "dock-\(index)", extension: "png") == nil {
            missing.append("dock-\(index).png")
        }
        for file in ["cloud-bubble.png", "voice-start.mp3", "voice-complete.mp3",
                     "voice-error.mp3", "AppIcon.png"] {
            let parts = file.split(separator: ".", maxSplits: 1).map(String.init)
            if parts.count != 2 || url(name: parts[0], extension: parts[1]) == nil {
                missing.append(file)
            }
        }
        return missing
    }
}
