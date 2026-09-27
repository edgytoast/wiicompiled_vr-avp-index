// SPDX-License-Identifier: GPL-3.0-or-later
//
// The in-headset settings panel's controller handling, tested without a
// headset: the button that opens and closes it in each controller mode, the
// release latch that keeps a closing press out of the game, selection,
// scrolling, and where a hit lands on the panel's canvas.

#include "vr/openxr_settings_panel.h"

#include <cmath>
#include <iostream>

namespace {

using namespace mkw::vr;
using namespace mkw::vr::settings_panel;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void CheckNear(float actual, float expected, const char* what, float tolerance = 1.0e-3f) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++g_failures;
        std::cerr << "FAILED: " << what << " (expected " << expected << ", got " << actual << ")\n";
    }
}

constexpr float kDt = 1.0f / 90.0f;
constexpr OpenXRControllerMode kWiiRemote = OpenXRControllerMode::WiiRemote;
constexpr OpenXRControllerMode kGamepad = OpenXRControllerMode::Gamepad;
constexpr OpenXRControllerMode kNone = OpenXRControllerMode::None;

std::array<HandInputs, 2> Released() {
    return {};
}

std::array<HandInputs, 2> Chord() {
    std::array<HandInputs, 2> hands{};
    hands[0].thumbstick_click = true;
    hands[1].thumbstick_click = true;
    return hands;
}

std::array<HandInputs, 2> LeftY() {
    std::array<HandInputs, 2> hands{};
    hands[0].secondary = true;
    return hands;
}

void LeftYOpensAndClosesOnceAsAWiiRemote() {
    Controls controls;
    bool open = false;

    auto right_b = Released();
    right_b[1].secondary = true;
    Frame frame = controls.Update(right_b, open, kDt, kWiiRemote);
    Check(!open && !frame.withheld, "right B neither opens nor withholds");
    frame = controls.Update(Chord(), open, kDt, kWiiRemote);
    Check(!open && !frame.withheld, "as a Wii Remote the thumbstick chord does not open the panel");
    controls.Update(Released(), open, kDt, kWiiRemote);

    frame = controls.Update(LeftY(), open, kDt, kWiiRemote);
    Check(open && frame.open && frame.withheld, "left Y opens the panel");
    frame = controls.Update(LeftY(), open, kDt, kWiiRemote);
    Check(open, "holding left Y does not toggle again");
    frame = controls.Update(Released(), open, kDt, kWiiRemote);
    Check(open && frame.withheld, "the panel stays open and keeps the controllers after left Y is released");

    frame = controls.Update(LeftY(), open, kDt, kWiiRemote);
    Check(!open && !frame.open, "left Y closes the panel again");
    Check(frame.withheld, "the closing press is still withheld from the game");
    frame = controls.Update(Released(), open, kDt, kWiiRemote);
    Check(!frame.withheld, "the game gets the controllers back once everything is released");
}

void ChordOpensAndClosesOnceAsAGamepad() {
    Controls controls;
    bool open = false;

    Frame frame = controls.Update(LeftY(), open, kDt, kGamepad);
    Check(!open && !frame.withheld, "as a gamepad left Y is GameCube Y, not the panel");
    auto one = Released();
    one[0].thumbstick_click = true;
    frame = controls.Update(one, open, kDt, kGamepad);
    Check(!open && !frame.withheld, "one thumbstick click alone neither opens nor withholds");

    frame = controls.Update(Chord(), open, kDt, kGamepad);
    Check(open && frame.open && frame.withheld, "clicking both thumbsticks opens the panel");
    frame = controls.Update(Chord(), open, kDt, kGamepad);
    Check(open, "holding the chord does not toggle again");
    frame = controls.Update(Released(), open, kDt, kGamepad);
    Check(open && frame.withheld, "the panel stays open and keeps the controllers after the chord is released");

    frame = controls.Update(Chord(), open, kDt, kGamepad);
    Check(!open && !frame.open, "the chord closes the panel again");
    Check(frame.withheld, "the closing chord is still withheld from the game");
    frame = controls.Update(Released(), open, kDt, kGamepad);
    Check(!frame.withheld, "the game gets the controllers back once everything is released");
}

void LeftYOpensThePanelWithoutAGameController() {
    Controls controls;
    bool open = false;

    Frame frame = controls.Update(Chord(), open, kDt, kNone);
    Check(!open, "with no game controller the thumbstick chord does not open the panel");
    controls.Update(Released(), open, kDt, kNone);
    frame = controls.Update(LeftY(), open, kDt, kNone);
    Check(open && frame.open, "with no game controller left Y opens the panel");
    controls.Update(Released(), open, kDt, kNone);
    frame = controls.Update(LeftY(), open, kDt, kNone);
    Check(!open && !frame.open, "and closes it again");
}

void MenuClosesAndItsPressStaysOutOfTheGame() {
    Controls controls;
    bool open = false;
    controls.Update(LeftY(), open, kDt, kWiiRemote);
    controls.Update(Released(), open, kDt, kWiiRemote);

    auto menu = Released();
    menu[0].menu = true;
    Frame frame = controls.Update(menu, open, kDt, kWiiRemote);
    Check(!open, "the left menu button closes an open panel");
    frame = controls.Update(menu, open, kDt, kWiiRemote);
    Check(frame.withheld, "+ held across the close does not reach the game");
    Check(!open, "a held menu button does not reopen the panel");
    frame = controls.Update(Released(), open, kDt, kWiiRemote);
    Check(!frame.withheld, "released, the controllers go back to the game");

    frame = controls.Update(menu, open, kDt, kWiiRemote);
    Check(!open && !frame.withheld, "with the panel closed the menu button is the game's");
}

void SelectWaitsForAReleaseAndFollowsTheTrigger() {
    Controls controls;
    bool open = false;
    // Opened from the settings bar while a trigger is held for the game.
    auto trigger = Released();
    trigger[1].trigger = 1.0f;
    controls.Update(trigger, open, kDt, kWiiRemote);
    open = true;
    Frame frame = controls.Update(trigger, open, kDt, kWiiRemote);
    Check(frame.open && !frame.select, "a trigger held from before the panel opened does not click");
    frame = controls.Update(Released(), open, kDt, kWiiRemote);
    Check(!frame.select, "nothing held, nothing selected");
    frame = controls.Update(trigger, open, kDt, kWiiRemote);
    Check(frame.select && frame.pointing_hand == 1, "a fresh right trigger selects and points with the right hand");

    auto left = Released();
    left[0].trigger = 0.9f;
    controls.Update(Released(), open, kDt, kWiiRemote);
    frame = controls.Update(left, open, kDt, kWiiRemote);
    Check(frame.select && frame.pointing_hand == 0, "pulling the left trigger hands the pointer to the left hand");

    auto button = Released();
    button[1].primary = true;
    controls.Update(Released(), open, kDt, kWiiRemote);
    frame = controls.Update(button, open, kDt, kWiiRemote);
    Check(frame.select && frame.pointing_hand == 0, "A selects without moving the pointer to another hand");
}

void ThumbstickScrolls() {
    Controls controls;
    bool open = true;
    controls.Update(Released(), open, kDt, kWiiRemote);

    auto small = Released();
    small[1].stick_y = 0.2f;
    Check(controls.Update(small, open, kDt, kWiiRemote).wheel == 0.0f, "a resting thumbstick does not scroll");

    auto up = Released();
    up[1].stick_y = 1.0f;
    CheckNear(controls.Update(up, open, 0.5f, kWiiRemote).wheel, kScrollStepsPerSecond * 0.1f,
              "full deflection scrolls up at the full rate, with a long frame clamped");
    auto down = Released();
    down[0].stick_y = -1.0f;
    down[1].stick_y = 0.3f;
    Check(controls.Update(down, open, kDt, kWiiRemote).wheel < 0.0f, "the more deflected stick decides the direction");

    bool closed = false;
    Controls idle;
    Check(idle.Update(up, closed, kDt, kWiiRemote).wheel == 0.0f, "a closed panel does not scroll");
}

void HitsMapOntoTheCanvas() {
    wii_remote::ScreenHit hit{};
    hit.valid = true;
    hit.u = -1.0f;
    hit.v = 1.0f;
    auto point = CanvasPoint(hit);
    CheckNear(point[0], 0.0f, "the panel's left edge is canvas x 0");
    CheckNear(point[1], 0.0f, "the panel's top edge is canvas y 0");
    hit.u = 1.0f;
    hit.v = -1.0f;
    point = CanvasPoint(hit);
    CheckNear(point[0], kSettingsPanelWidthPixels, "the right edge is the canvas width");
    CheckNear(point[1], kSettingsPanelHeightPixels, "the bottom edge is the canvas height");

    const auto extents = HalfExtents(2.4f);
    CheckNear(extents[0], 0.9f, "three quarters of a 2.4 m screen is 1.8 m across");
    CheckNear(extents[1], 0.675f, "and keeps the canvas's 4:3");
}

void BridgeAccumulatesWheelUntilTaken() {
    OpenXRSetSettingsPanelOpen(true);
    Check(OpenXRSettingsPanelOpen(), "the open flag is shared");
    OpenXRPublishSettingsPanelPointer(true, 10.0f, 20.0f, false, 0.5f);
    OpenXRPublishSettingsPanelPointer(true, 11.0f, 21.0f, true, 0.25f);
    OpenXRSettingsPanelPointer pointer = OpenXRTakeSettingsPanelPointer();
    Check(pointer.valid && pointer.select, "the latest pointer state is read");
    CheckNear(pointer.x, 11.0f, "the latest x is read");
    CheckNear(pointer.wheel, 0.75f, "wheel steps from every XR frame are kept");
    pointer = OpenXRTakeSettingsPanelPointer();
    CheckNear(pointer.wheel, 0.0f, "taking the pointer consumes its wheel steps");
    OpenXRSetSettingsPanelOpen(false);
    Check(!OpenXRSettingsPanelOpen(), "and can be cleared again");
}

} // namespace

int main() {
    LeftYOpensAndClosesOnceAsAWiiRemote();
    ChordOpensAndClosesOnceAsAGamepad();
    LeftYOpensThePanelWithoutAGameController();
    MenuClosesAndItsPressStaysOutOfTheGame();
    SelectWaitsForAReleaseAndFollowsTheTrigger();
    ThumbstickScrolls();
    HitsMapOntoTheCanvas();
    BridgeAccumulatesWheelUntilTaken();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "vr_settings_panel_tests: all checks passed\n";
    return 0;
}
