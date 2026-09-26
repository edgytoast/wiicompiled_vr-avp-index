// SPDX-License-Identifier: GPL-3.0-or-later

import CompositorServices
import SwiftUI

/// The app: a small launcher window and the immersive space the game renders
/// into. The game runtime lives in the static library this app links
/// (runtime/cmake/PublicProducts.cmake) and is driven through GameModel.
@main
struct WiiCompiledVisionApp: App {
    @StateObject private var model = GameModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup(id: GameModel.launcherWindowID) {
            LauncherView()
                .environmentObject(model)
        }
        .windowResizability(.contentSize)
        .defaultSize(width: 720, height: 560)

        ImmersiveSpace(id: GameModel.immersiveSpaceID) {
            ImmersiveGame.layer(for: model)
        }
        // Mixed immersion shows the room around the virtual screen and the
        // immersive window when the game's passthrough setting asks for it (the
        // provider clears the frame transparent there); full immersion is the
        // headset with nothing else. The launcher offers the choice.
        .immersionStyle(selection: $model.immersionStyle, in: .mixed, .full)
        .upperLimbVisibility(.visible)
    }
}
