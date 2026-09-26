// SPDX-License-Identifier: GPL-3.0-or-later

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
        Task { @MainActor in
            self.startGame()
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
