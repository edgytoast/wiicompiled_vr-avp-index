// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The window the app opens with: where the game files go, whether they are
/// there, and the button that opens the immersive space.
struct LauncherView: View {
    @EnvironmentObject private var model: GameModel
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Text("\(model.gameTitle) for Apple Vision Pro")
                .font(.largeTitle.bold())

            GroupBox("Game files") {
                VStack(alignment: .leading, spacing: 8) {
                    Label(model.discPresent ? "Extracted disc found" : "No extracted disc yet",
                          systemImage: model.discPresent ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                        .foregroundStyle(model.discPresent ? .green : .orange)
                    Text("Copy the extracted Mario Kart Wii (PAL) disc, the folder holding sys/ and files/, into the DATA folder below with the Files app or through Finder file sharing. Settings live in Config.toml next to it.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    PathRow(title: "Disc (DATA)", path: model.discDirectory)
                    PathRow(title: "Config.toml", path: model.configPath)
                    Button("Check again") { model.refreshDisc() }
                        .buttonStyle(.bordered)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            GroupBox("Headset") {
                VStack(alignment: .leading, spacing: 8) {
                    Toggle("Show my room around the menu screen", isOn: Binding(
                        get: { model.wantsRoom },
                        set: { model.wantsRoom = $0 }))
                        .disabled(model.phase == .running)
                    Text("Races are fully immersive either way. In the menus, look at a button and pinch to press it. A Bluetooth game controller is the way to actually race. Dismiss the immersive space (press the Digital Crown) to end the game.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            statusView

            HStack {
                Button {
                    Task { await start() }
                } label: {
                    Label("Play", systemImage: "play.fill")
                        .frame(minWidth: 120)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!model.canStart)

                if model.phase == .running {
                    Button("Quit game", role: .destructive) {
                        model.requestQuit()
                        Task { await dismissImmersiveSpace() }
                    }
                    .buttonStyle(.bordered)
                }
            }
        }
        .padding(28)
        .frame(minWidth: 640)
        .onAppear {
            model.refreshDisc()
            // `simctl launch ... --autoplay` (or an Xcode scheme argument) presses Play for
            // an unattended run on the simulator, where nothing can pinch the button.
            if CommandLine.arguments.contains("--autoplay"), model.canStart {
                Task { await start() }
            }
        }
    }

    @ViewBuilder
    private var statusView: some View {
        switch model.phase {
        case .idle:
            if !model.lastError.isEmpty {
                Label(model.lastError, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
            }
        case .opening:
            Label("Opening the immersive space…", systemImage: "hourglass")
        case .running:
            Label("The game is running in the immersive space.", systemImage: "visionpro")
        case .ended(let code):
            Label(code == 0 ? "The game ended. Relaunch the app to play again."
                            : "The game ended with code \(code); see the Logs folder. Relaunch the app to play again.",
                  systemImage: "flag.checkered")
        case .failed(let message):
            Label(message.isEmpty ? "The game could not start." : message, systemImage: "xmark.octagon.fill")
                .foregroundStyle(.red)
        }
    }

    private func start() async {
        model.markOpening()
        switch await openImmersiveSpace(id: GameModel.immersiveSpaceID) {
        case .opened:
            // Out of the way once the game is on: visionOS only lets the last scene go
            // after the space has opened, which is now. The game's audio is anchored to
            // the listener, not this window (GameModel), so it plays on without it.
            dismissWindow(id: GameModel.launcherWindowID)
        case .userCancelled:
            model.openingFailed("The immersive space was not opened.")
        case .error:
            model.openingFailed("visionOS could not open the immersive space.")
        @unknown default:
            model.openingFailed("visionOS could not open the immersive space.")
        }
    }
}

private struct PathRow: View {
    let title: String
    let path: String

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title).font(.callout.bold()).frame(width: 110, alignment: .leading)
            Text(path)
                .font(.caption.monospaced())
                .textSelection(.enabled)
                .lineLimit(2)
                .truncationMode(.head)
        }
    }
}
