// SPDX-License-Identifier: GPL-3.0-or-later

import AVFAudio
import CompositorServices
import Foundation
import SwiftUI

/// The game's state as the launcher sees it, over the C bridge
/// (runtime/include/platform/visionos/visionos_host.h).
@MainActor
final class GameModel: ObservableObject {
    static let launcherWindowID = "launcher"
    static let immersiveSpaceID = "game"

    enum Phase: Equatable {
        /// Nothing started yet.
        case idle
        /// The immersive space is being opened; the game starts once its layer exists.
        case opening
        case running
        /// The runtime returned; the process keeps the guest memory it reserved, so a
        /// second run needs a relaunch of the app.
        case ended(exitCode: Int32)
        case failed(message: String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var discPresent = false
    @Published var immersionStyle: any ImmersionStyle = .mixed
    @Published private(set) var lastError = ""

    /// The game folder in the app's Documents directory, as the Files app shows it.
    let gameDirectory: String
    let configPath: String
    let discDirectory: String
    /// The game this build carries (MKW_VISIONOS_PRODUCT in visionos/CMakeLists.txt).
    #if MKW_VISIONOS_RETRO_REWIND
    let gameTitle = "Retro Rewind"
    #else
    let gameTitle = "WiiCompiled"
    #endif

    private var watchdog: Timer?

    init() {
        // visionOS anchors an app's audio to its window scene by default, so the game fell
        // silent when the launcher window closed. The game's sound belongs to the player,
        // not to a window: anchor it to the listener's front, which outlives every scene.
        do {
            try AVAudioSession.sharedInstance().setIntendedSpatialExperience(
                .headTracked(soundStageSize: .automatic, anchoringStrategy: .front))
        } catch {
            lastError = "Could not set up the game's audio: \(error.localizedDescription)"
        }
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        let resources = Bundle.main.resourcePath ?? Bundle.main.bundlePath
        mkw_visionos_set_directories(documents.path, resources)
        if !mkw_visionos_prepare_game_directory() {
            lastError = String(cString: mkw_visionos_last_error())
        }
        gameDirectory = String(cString: mkw_visionos_game_directory())
        configPath = String(cString: mkw_visionos_config_path())
        discDirectory = String(cString: mkw_visionos_disc_directory())
        refreshDisc()
    }

    func refreshDisc() {
        discPresent = mkw_visionos_disc_present()
    }

    var canStart: Bool {
        if case .idle = phase { return discPresent }
        return false
    }

    func markOpening() {
        if case .idle = phase { phase = .opening }
    }

    func openingFailed(_ message: String) {
        phase = .failed(message: message)
    }

    /// Called from the CompositorLayer closure once the immersive space has a layer renderer.
    /// The provider keeps the renderer; the runtime creates its OpenXR session against it.
    nonisolated func attach(_ layerRenderer: LayerRenderer) {
        // An OS object: the pointer the provider bridges back to cp_layer_renderer_t and retains.
        let pointer = Unmanaged.passUnretained(layerRenderer).toOpaque()
        mkw_visionos_set_layer_renderer(pointer)
        // visionOS's look-and-pinch selection. Apps never see the gaze itself, but a
        // pinch carries the ray from the eyes to where the user was looking when it
        // began, which the provider turns into the game's pointer and its press.
        nonisolated(unsafe) let renderer = layerRenderer
        Task { @MainActor in
            renderer.onSpatialEvent = { events in
                for event in events {
                    Self.forward(event)
                }
            }
            self.startGame()
        }
    }

    private nonisolated static func forward(_ event: SpatialEventCollection.Event) {
        let phase: Int32
        switch event.phase {
        case .active: phase = 0
        case .ended: phase = 1
        case .cancelled: phase = 2
        @unknown default: phase = 2
        }
        let chirality: Int32
        switch event.chirality {
        case .left?: chirality = 1
        case .right?: chirality = 2
        default: chirality = 0
        }
        // Stable per pinch: the same event id arrives for every phase of one gesture.
        let id = UInt64(bitPattern: Int64(event.id.hashValue))
        if let ray = event.selectionRay {
            mkw_visionos_spatial_event(id, phase, chirality, true,
                                       Float(ray.origin.x), Float(ray.origin.y), Float(ray.origin.z),
                                       Float(ray.direction.x), Float(ray.direction.y), Float(ray.direction.z))
        } else {
            mkw_visionos_spatial_event(id, phase, chirality, false, 0, 0, 0, 0, 0, 0)
        }
    }

    private func startGame() {
        guard phase == .opening || phase == .idle else { return }
        if mkw_visionos_start_game() {
            phase = .running
            startWatchdog()
        } else {
            phase = .failed(message: String(cString: mkw_visionos_last_error()))
        }
    }

    /// The game runs on its own thread; the launcher polls the bridge for its end and
    /// for a dismissed immersive space, after which the runtime would carry on unseen.
    private func startWatchdog() {
        watchdog?.invalidate()
        watchdog = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.poll()
            }
        }
    }

    private func poll() {
        if !mkw_visionos_game_running() {
            watchdog?.invalidate()
            watchdog = nil
            phase = .ended(exitCode: mkw_visionos_exit_code())
            return
        }
        if mkw_visionos_layer_invalidated() {
            mkw_visionos_request_quit()
        }
    }

    func requestQuit() {
        mkw_visionos_request_quit()
    }

    var wantsRoom: Bool {
        get { immersionStyle is MixedImmersionStyle }
        set { immersionStyle = newValue ? .mixed : .full }
    }
}
