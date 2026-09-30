// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Throwing the held item ([vr] cockpit_item_throw). In the first-person cockpit,
// a quick swing forward or backward of the hand that shows the item plays what
// Mario Kart Wii reads as an aimed throw: the stick pushed forward or back while
// the item button is pressed and released. It goes through the left hand's
// controls, which both controller modes read as the stick and the item button
// (the Nunchuk's stick and Z, the GameCube's stick and L), so it works whichever
// hand shows the item and with bare hands too. Nothing here depends on OpenXR,
// so the rules are tested headlessly (tests/vr_item_throw_tests.cpp).

#include "vr/openxr_wii_remote.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mkw::vr::item_throw {

enum class Direction : uint8_t { None, Forward, Backward };

inline const char* DirectionLabel(Direction direction) noexcept {
    switch (direction) {
    case Direction::Forward: return "forward";
    case Direction::Backward: return "backward";
    default: return "none";
    }
}

inline bool IsFinite(float value) noexcept {
    // Bit test: the runtime may be built with -ffast-math.
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7F800000u) != 0x7F800000u;
}

// The item hand this frame, in the seated frame (+X right, +Y up, -Z forward),
// in metres: the grip, or the palm joint when the hand is drawn from joints.
struct HandSample {
    bool tracked = false;
    bool held = false; // holding the wheel or handlebar
    bool bare = false; // tracked by the headset's cameras, no controller in it
    std::array<float, 3> position{};
};

// What counts as a throw for one kind of hand: the travel along the seated
// frame's forward axis within the window, how much of the window's displacement
// must be along that axis, the speed along it that means the swing goes on, and
// how long a loss of tracking is bridged by the positions either side of it.
struct Tuning {
    float window_seconds;
    float forward_travel;  // m
    float backward_travel; // m: throwing back is harder
    float axis_share;
    float moving_speed; // m/s
    float max_gap_seconds;
    size_t min_samples;
};
// A controller is located smoothly every frame: 1.5 m/s forward, 1.25 back.
inline constexpr Tuning kControllerTuning{0.12f, 0.18f, 0.15f, 0.7f, 0.5f, 0.1f, 3};
// The cameras lag and smooth a bare hand and often lose it for a few frames in
// the middle of a fast swing, so its swing reads slower and shorter: a longer
// window, less travel (1 m/s forward, 0.87 back), and the gap bridged.
inline constexpr Tuning kBareHandTuning{0.15f, 0.15f, 0.13f, 0.65f, 0.3f, 0.2f, 2};

// A throw is a swing along the seated frame's forward axis: within the last
// window the hand has covered at least the direction's travel along that axis,
// mostly along it (so a sideways sweep or the upward flick that does a trick
// never counts), and it is still moving that way. Reaching for the wheel or
// bringing a hand back to rest is too slow to cover the travel. The window is
// counted in tracked time, so a bridged gap does not use it up; across a gap
// the swing must still average most of the window's speed, so a slow drift the
// cameras briefly lost never counts. A hand fires once per swing and then waits
// out the cooldown; a hand that just let go of the wheel must be free a moment
// first; a tracking jump, faster than any hand, restarts the track.
class ThrowDetector {
public:
    static constexpr float kJumpSpeed = 8.0f; // m/s: faster than a hand, a tracking jump
    static constexpr float kGapSpeedShare = 0.7f;
    static constexpr float kFreeSeconds = 0.25f;
    static constexpr float kCooldownSeconds = 0.6f;

    // What the last throw measured, for the log.
    struct Measure {
        float travel = 0.0f;  // m along the forward axis, + forward
        float seconds = 0.0f; // the window it covered, gaps included
        bool bare = false;
        bool bridged = false; // the window spans a loss of tracking
    };

    Direction Update(const HandSample& hand, float dt) noexcept {
        if (!IsFinite(dt) || dt <= 0.0f) {
            return Direction::None;
        }
        dt = std::min(dt, 0.1f);
        m_cooldown = std::max(m_cooldown - dt, 0.0f);
        m_free = std::min(m_free + dt, 1.0f);
        m_time += dt;
        const Tuning& tuning = hand.bare ? kBareHandTuning : kControllerTuning;
        const auto& p = hand.position;
        if (!hand.tracked || !IsFinite(p[0]) || !IsFinite(p[1]) || !IsFinite(p[2])) {
            if (m_count > 0 && m_time - At(0).time > tuning.max_gap_seconds) {
                ClearTrack();
            }
            return Direction::None;
        }
        if (hand.held) {
            ClearTrack();
            m_free = 0.0f;
            return Direction::None;
        }
        bool bridged_now = false;
        if (m_count > 0) {
            const Sample& last = At(0);
            const float elapsed = m_time - last.time;
            if (elapsed > tuning.max_gap_seconds + dt ||
                Distance(p, last.position) / std::max(elapsed, 1.0e-4f) > kJumpSpeed) {
                ClearTrack();
            } else {
                bridged_now = elapsed > dt + 1.0e-4f;
            }
        }
        m_tracked_time += dt;
        Push({m_time, m_tracked_time, bridged_now, p});
        if (m_count < 2) {
            return Direction::None;
        }
        // The swing's speed along the axis now (+ forward), from the last step.
        const Sample& previous = At(1);
        const float forward_speed = -(p[2] - previous.position[2]) / std::max(m_time - previous.time, 1.0e-4f);
        if (m_swing != Direction::None && Along(m_swing, forward_speed) < tuning.moving_speed) {
            m_swing = Direction::None; // that swing is over
        }
        // The oldest sample still inside the window, in tracked time.
        size_t oldest = 0;
        bool bridged = false;
        for (size_t i = 1; i < m_count && m_tracked_time - At(i).tracked <= tuning.window_seconds + 1.0e-4f; ++i) {
            bridged = bridged || At(i - 1).bridged;
            oldest = i;
        }
        if (oldest + 1 < tuning.min_samples) {
            return Direction::None;
        }
        const Sample& start = At(oldest);
        const std::array<float, 3> moved{p[0] - start.position[0], p[1] - start.position[1], p[2] - start.position[2]};
        const float length = std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
        const float travel = -moved[2];
        const float seconds = m_time - start.time;
        const Direction direction = travel >= tuning.forward_travel ? Direction::Forward
                                    : -travel >= tuning.backward_travel ? Direction::Backward
                                                                        : Direction::None;
        if (direction == Direction::None) {
            return Direction::None;
        }
        const float needed = direction == Direction::Forward ? tuning.forward_travel : tuning.backward_travel;
        if (std::fabs(travel) < tuning.axis_share * length || Along(direction, forward_speed) < tuning.moving_speed ||
            std::fabs(travel) / std::max(seconds, 1.0e-4f) < kGapSpeedShare * needed / tuning.window_seconds) {
            return Direction::None;
        }
        if (m_free < kFreeSeconds || m_cooldown > 0.0f || m_swing == direction) {
            // Too soon: this whole swing is spent, however long it goes on.
            m_swing = direction;
            return Direction::None;
        }
        m_cooldown = kCooldownSeconds;
        m_swing = direction;
        m_last = {travel, seconds, hand.bare, bridged};
        return direction;
    }

    const Measure& Last() const noexcept { return m_last; }

    void Reset() noexcept { *this = ThrowDetector{}; }

private:
    struct Sample {
        float time = 0.0f;    // since the detector started, gaps included
        float tracked = 0.0f; // tracked time only
        bool bridged = false; // follows a loss of tracking
        std::array<float, 3> position{};
    };
    static constexpr size_t kCapacity = 32; // over either window at any headset's rate

    static float Along(Direction direction, float forward_speed) noexcept {
        return direction == Direction::Forward ? forward_speed : -forward_speed;
    }
    static float Distance(const std::array<float, 3>& a, const std::array<float, 3>& b) noexcept {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return std::sqrt(x * x + y * y + z * z);
    }
    // Newest first: At(0) is the latest sample.
    const Sample& At(size_t age) const noexcept { return m_samples[(m_next + kCapacity - 1 - age) % kCapacity]; }
    void Push(const Sample& sample) noexcept {
        m_samples[m_next] = sample;
        m_next = (m_next + 1) % kCapacity;
        m_count = std::min(m_count + 1, kCapacity);
    }
    void ClearTrack() noexcept {
        m_count = 0;
        m_swing = Direction::None;
    }

    std::array<Sample, kCapacity> m_samples{};
    size_t m_next = 0;
    size_t m_count = 0;
    float m_time = 0.0f;
    float m_tracked_time = 0.0f;
    float m_free = 1.0f; // seconds since the hand last held the wheel, capped
    float m_cooldown = 0.0f;
    Direction m_swing = Direction::None; // the swing that last fired, while it lasts
    Measure m_last{};
};

// The input a throw plays on the left hand's controls. The stick goes first and
// stays until the item button has been pressed and released again, so an item
// used on the press (a Mushroom) and one thrown on the release (a trailed shell
// or banana) both see the aim. A button the player was already holding, to
// trail an item, is released by the throw and then ignored until the player
// lets go of it, so it does not use the next item of a triple.
class ThrowSequence {
public:
    static constexpr float kAimSeconds = 0.05f;     // the stick alone, a few game frames
    static constexpr float kPressSeconds = 0.10f;
    static constexpr float kReleaseSeconds = 0.10f; // the stick still aimed

    void Start(Direction direction) noexcept {
        if (direction == Direction::None) {
            return;
        }
        m_direction = direction;
        m_elapsed = 0.0f;
    }

    bool Active() const noexcept { return m_direction != Direction::None; }

    void Apply(wii_remote::HandInputs& left, float dt) noexcept {
        const bool held = left.trigger > wii_remote::kPressThreshold;
        if (m_ignore_held) {
            if (held) {
                left.trigger = 0.0f;
            } else {
                m_ignore_held = false;
            }
        }
        if (!Active()) {
            return;
        }
        left.stick_y = m_direction == Direction::Forward ? 1.0f : -1.0f;
        if (m_elapsed >= kAimSeconds + kPressSeconds) {
            m_ignore_held = m_ignore_held || held;
            left.trigger = 0.0f;
        } else if (m_elapsed >= kAimSeconds) {
            left.trigger = 1.0f;
        }
        m_elapsed += IsFinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f;
        if (m_elapsed >= kAimSeconds + kPressSeconds + kReleaseSeconds) {
            m_direction = Direction::None;
        }
    }

    void Reset() noexcept { *this = ThrowSequence{}; }

private:
    Direction m_direction = Direction::None;
    float m_elapsed = 0.0f;
    bool m_ignore_held = false;
};

} // namespace mkw::vr::item_throw
