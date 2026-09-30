#pragma once

#include <algorithm>
#include <cstdint>

namespace aurora::stereo {
// Scene playback uses the producer's cadence on a local monotonic clock. The
// compositor may predict head poses several frames ahead; sampling the two known
// scene endpoints at that future time would clamp every image to the newest one.
// Anchor when history starts. If production later arrives earlier on that grid
// (e.g. after shader warm-up), advance the origin enough to reach the new pair.
// Never move playback backwards to follow a late seal or the prediction horizon.
class ScenePlaybackClock {
public:
  void begin_scene(uint64_t boundary, uint64_t now, bool continuous) noexcept {
    if (!continuous || originBoundary_ == 0 || now < originTime_ || boundary <= lastBoundary_ ||
        sample_time(now) < boundary) {
      originBoundary_ = boundary;
      originTime_ = now;
    }
    lastBoundary_ = boundary;
  }

  uint64_t sample_time(uint64_t now) const noexcept {
    if (originBoundary_ == 0 || now < originTime_)
      return 0;
    return originBoundary_ + (now - originTime_);
  }

private:
  uint64_t originBoundary_ = 0;
  uint64_t originTime_ = 0;
  uint64_t lastBoundary_ = 0;
};

// The current scene belongs to boundary, and playback blends from the preceding
// scene for one guest interval. Head tracking is applied afterwards, undelayed.
inline float interpolation_weight(uint64_t sampleTime, uint64_t boundary, uint64_t interval) noexcept {
  if (sampleTime == 0 || boundary == 0 || interval == 0)
    return 1.0f;
  if (sampleTime <= boundary)
    return 0.0f;
  return static_cast<float>(std::min(static_cast<double>(sampleTime - boundary) / static_cast<double>(interval), 1.0));
}

// Worker-owned window. A fresh eye pair can still repeat the same game-scene
// time, or move it backwards when an overdue scene is replaced. Neither is
// visible in the compositor's submission FPS. Discontinuities have no comparable
// scene time, so they break the sequence rather than counting as backwards motion.
struct MotionSamples {
  uint32_t samples = 0;
  uint32_t blended = 0;
  uint32_t atPrevious = 0;
  uint32_t atCurrent = 0;
  uint32_t discontinuous = 0;
  uint32_t repeated = 0;
  uint32_t backwards = 0;
  uint64_t lastSceneTime = 0;
  uint64_t minStep = UINT64_MAX;
  uint64_t maxStep = 0;

  void record(uint64_t displayTime, uint64_t boundary, uint64_t interval,
              bool continuous, float weight) noexcept {
    ++samples;
    if (!continuous || displayTime == 0 || boundary < interval || interval == 0) {
      ++discontinuous;
      lastSceneTime = 0;
      return;
    }
    if (weight <= 0.0f)
      ++atPrevious;
    else if (weight >= 1.0f)
      ++atCurrent;
    else
      ++blended;
    const uint64_t sceneTime = boundary - interval +
        static_cast<uint64_t>(static_cast<double>(interval) * weight);
    if (lastSceneTime != 0) {
      if (sceneTime >= lastSceneTime) {
        const auto step = sceneTime - lastSceneTime;
        minStep = std::min(minStep, step);
        maxStep = std::max(maxStep, step);
      }
      // Float interpolation weights can differ by a few nanoseconds at a boundary.
      if (sceneTime + 1000 < lastSceneTime)
        ++backwards;
      else if (sceneTime <= lastSceneTime + 1000)
        ++repeated;
    }
    lastSceneTime = sceneTime;
  }

  void clear_window() noexcept {
    const auto last = lastSceneTime;
    *this = {};
    lastSceneTime = last;
  }
};
} // namespace aurora::stereo
