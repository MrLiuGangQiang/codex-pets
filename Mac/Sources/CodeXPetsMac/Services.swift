import AppKit
import AVFoundation
import Foundation
import ServiceManagement

enum SingleInstanceChecker {
    static func hasAnotherInstance() -> Bool {
        guard let identifier = Bundle.main.bundleIdentifier else { return false }
        let currentPID = ProcessInfo.processInfo.processIdentifier
        return NSRunningApplication.runningApplications(withBundleIdentifier: identifier)
            .contains { $0.processIdentifier != currentPID && !$0.isTerminated }
    }
}

enum LaunchAtLoginManager {
    static var isEnabled: Bool {
        let status = SMAppService.mainApp.status
        return status == .enabled || status == .requiresApproval
    }

    static func setEnabled(_ enabled: Bool) throws {
        if enabled {
            if SMAppService.mainApp.status == .notRegistered {
                try SMAppService.mainApp.register()
            }
        } else if SMAppService.mainApp.status != .notRegistered {
            try SMAppService.mainApp.unregister()
        }
    }
}

@MainActor
final class SoundPlayer: NSObject, AVAudioPlayerDelegate {
    enum EventSound: String {
        case start = "voice-start"
        case complete = "voice-complete"
        case error = "voice-error"
    }

    private var queue: [URL] = []
    private var player: AVAudioPlayer?

    func enqueue(_ sound: EventSound) {
        guard let url = AppResources.url(name: sound.rawValue, extension: "mp3") else { return }
        queue.append(url)
        playNextIfNeeded()
    }

    private func playNextIfNeeded() {
        guard player == nil, !queue.isEmpty else { return }
        let url = queue.removeFirst()
        do {
            let next = try AVAudioPlayer(contentsOf: url)
            next.delegate = self
            next.prepareToPlay()
            player = next
            next.play()
        } catch {
            player = nil
            playNextIfNeeded()
        }
    }

    nonisolated func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        Task { @MainActor [weak self] in
            self?.player = nil
            self?.playNextIfNeeded()
        }
    }

    nonisolated func audioPlayerDecodeErrorDidOccur(_ player: AVAudioPlayer, error: Error?) {
        Task { @MainActor [weak self] in
            self?.player = nil
            self?.playNextIfNeeded()
        }
    }
}
