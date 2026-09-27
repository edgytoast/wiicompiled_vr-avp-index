// SPDX-License-Identifier: GPL-3.0-or-later

import AVFAudio
import CompositorServices
import Foundation
import SwiftUI

/// The game's state as the launcher sees it: its files (GameStorage) and, once loaded at
/// Play, the runtime's C bridge (GameLibrary, runtime/include/platform/visionos/visionos_host.h).
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
        GameStorage.exportDirectories()
        if let message = GameStorage.prepare() {
            lastError = message
        }
        gameDirectory = GameStorage.gameDirectory.path
        configPath = GameStorage.configFile.path
        discDirectory = GameStorage.discDirectory.path
        refreshDisc()
    }

    func refreshDisc() {
        discPresent = GameStorage.discStatus == .ready
    }

    /// Loads the game framework: its static initialisers read Config.toml now, with whatever
    /// the Settings page wrote. Before the immersive space opens, so the layer's arrival can
    /// start the game at once. False with the phase set to failed.
    func loadGame() -> Bool {
        if let message = GameStorage.prepare() {
            phase = .failed(message: message)
            return false
        }
        do {
            let library = try GameLibrary.load()
            library.setDirectories(data: GameStorage.documents.path, resources: GameStorage.resources.path)
            return true
        } catch {
            phase = .failed(message: error.localizedDescription)
            return false
        }
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
        GameLibrary.shared?.setLayerRenderer(pointer)
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
        let ray = event.selectionRay.map {
            (origin: SIMD3<Float>(Float($0.origin.x), Float($0.origin.y), Float($0.origin.z)),
             direction: SIMD3<Float>(Float($0.direction.x), Float($0.direction.y), Float($0.direction.z)))
        }
        // The pinching hand, wherever it goes while the pinch lasts: the provider moves the
        // pointer with it from where the gaze put it, and presses when the pinch ends.
        let pose = event.inputDevicePose.map {
            SIMD3<Float>(Float($0.pose3D.position.x), Float($0.pose3D.position.y), Float($0.pose3D.position.z))
        }
        GameLibrary.shared?.spatialEvent(id: id, phase: phase, chirality: chirality, ray: ray, pose: pose)
    }

    private func startGame() {
        guard phase == .opening || phase == .idle, let library = GameLibrary.shared else { return }
        if library.startGame() {
            phase = .running
            startWatchdog()
        } else {
            phase = .failed(message: library.lastError)
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
        guard let library = GameLibrary.shared else { return }
        if !library.gameRunning {
            watchdog?.invalidate()
            watchdog = nil
            phase = .ended(exitCode: library.exitCode)
            return
        }
        if library.layerInvalidated {
            library.requestQuit()
        }
    }

    func requestQuit() {
        GameLibrary.shared?.requestQuit()
    }

    var wantsRoom: Bool {
        get { immersionStyle is MixedImmersionStyle }
        set { immersionStyle = newValue ? .mixed : .full }
    }
}
