#include "stereo_interpolation.hpp"
#include "scene_camera.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

TEST(StereoInterpolation, SeparatesCameraFromHeldGeometryAndFirstPersonAnchor) {
  using aurora::Mat3x4;
  using aurora::gfx::stereo_replay::compose_affine;
  const Mat3x4<float> identity{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  const float angle = 0.08f;
  const float c = std::cos(angle), s = std::sin(angle);
  const Mat3x4<float> currentView{{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}};
  const Mat3x4<float> object{{1, 0, 0, 1000}, {0, 1, 0, 0}, {0, 0, 1, -100000}};
  const auto currentObject = compose_affine(currentView, object);
  aurora::stereo::SceneCameraMotion motion;
  for (bool firstPerson : {false, true}) {
    // A moving seat is distinct from the game's chase camera. Both must be
    // sampled once, and only once, even for geometry with no usable history.
    auto previousAnchor = identity, currentAnchor = identity;
    if (firstPerson) {
      previousAnchor.m1[3] = 200;
      currentAnchor.m1[3] = 230;
    }
    ASSERT_TRUE(motion.prepare(identity, currentView, previousAnchor, currentAnchor));
    for (float weight : {0.f, 1.f / 3, 2.f / 3, 1.f}) {
      Mat3x4<float> sampledAnchor{}, expectedPose{}, expectedView{};
      ASSERT_TRUE(motion.sample(weight, sampledAnchor));
      ASSERT_TRUE(aurora::gx::interpolate_transform(motion.previousPose, motion.currentPose, weight, expectedPose));
      ASSERT_TRUE(aurora::stereo::inverse_rigid_view(expectedPose, expectedView));
      const auto expected = compose_affine(expectedView, object);
      // Identical math covers held particle vertices baked into current view
      // space and an unmatched/rejected billboard using its current matrix.
      const auto actual = compose_affine(sampledAnchor, currentObject);
      EXPECT_NEAR(actual.m0.w(), expected.m0.w(), 0.02f);
      EXPECT_NEAR(actual.m1.w(), expected.m1.w(), 0.02f);
      EXPECT_NEAR(actual.m2.w(), expected.m2.w(), 0.02f);
    }
  }
}

TEST(StereoInterpolation, CameraCutsAndMalformedViewsDisableCameraSeparation) {
  const aurora::Mat3x4<float> identity{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
  aurora::stereo::SceneCameraMotion motion;
  ASSERT_TRUE(motion.prepare(identity, identity, identity, identity));
  auto cut = identity;
  cut.m0[3] = 2000;
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
  EXPECT_FALSE(motion.active);
  cut = identity;
  cut.m0[0] = 2;
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
  cut.m0[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
  cut = {{-1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, -1, 0}};
  EXPECT_FALSE(motion.prepare(identity, cut, identity, identity));
}

TEST(StereoInterpolation, ContinuousMotionAcross60HzScenesAtHeadsetRates) {
  constexpr uint64_t interval = 16'666'667;
  // A camera/object moving one unit per guest frame must advance uniformly,
  // even at 72/90 Hz where many samples are neither midpoints nor endpoints.
  for (uint64_t hz : {72u, 90u, 120u}) {
    double previousPosition = -1;
    for (uint64_t sample = 1; sample <= hz; ++sample) {
      const uint64_t displayTime = 1'000'000'000 + sample * 1'000'000'000 / hz;
      const uint64_t scene = (displayTime - 1'000'000'000) / interval;
      const uint64_t boundary = 1'000'000'000 + scene * interval;
      const float weight = aurora::stereo::interpolation_weight(displayTime, boundary, interval);
      const double position = static_cast<double>(scene) + weight;
      if (sample > 1)
        EXPECT_NEAR(position - previousPosition, 1'000'000'000.0 / hz / interval, 1e-5);
      previousPosition = position;
    }
  }
}

TEST(StereoInterpolation, MissingTimingAndStallsDoNotExtrapolate) {
  using aurora::stereo::interpolation_weight;
  EXPECT_FLOAT_EQ(interpolation_weight(0, 100, 10), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 0, 10), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 100, 0), 1);
  EXPECT_FLOAT_EQ(interpolation_weight(95, 100, 10), 0);
  EXPECT_FLOAT_EQ(interpolation_weight(105, 100, 10), 0.5);
  EXPECT_FLOAT_EQ(interpolation_weight(500, 100, 10), 1);
}

TEST(StereoInterpolation, PlaybackCadenceDoesNotFollowFutureHeadPrediction) {
  constexpr uint64_t start = 1'000'000'000, interval = 16'666'667;
  for (uint64_t hz : {72u, 90u, 120u}) {
    // The producer's desktop presentation boundary can be ahead of, or behind,
    // its actual seal. Neither offset belongs in headset scene playback.
    for (int64_t scheduleOffset : {-5'000'000, 0, 7'000'000}) {
      const uint64_t origin = start + scheduleOffset;
      aurora::stereo::ScenePlaybackClock clock;
      clock.begin_scene(origin, start, false);
      uint64_t sealedScene = 0;
      double previousPosition = 0;
      uint32_t oldClampedSamples = 0;
      for (uint64_t sample = 1; sample <= hz; ++sample) {
        const uint64_t now = start + sample * 1'000'000'000 / hz;
        const uint64_t scene = (now - start) / interval;
        const uint64_t boundary = origin + scene * interval;
        if (scene != sealedScene) {
          // Seal jitter must not re-phase the entire playback clock.
          clock.begin_scene(boundary, start + scene * interval + (scene % 3) * 100'000, true);
          sealedScene = scene;
        }
        // The captured Virtual Desktop session predicted 37-65 ms ahead.
        const uint64_t displayTime = now + (37 + sample % 29) * 1'000'000;
        oldClampedSamples += aurora::stereo::interpolation_weight(displayTime, boundary, interval) == 1.0f;
        const float weight = aurora::stereo::interpolation_weight(clock.sample_time(now), boundary, interval);
        const double position = static_cast<double>(scene) + weight;
        if (sample > 1)
          EXPECT_NEAR(position - previousPosition, 1'000'000'000.0 / hz / interval, 1e-5);
        previousPosition = position;
      }
      EXPECT_EQ(oldClampedSamples, hz); // Regression reproduces the old all-current result.
    }
  }
}

TEST(StereoInterpolation, PlaybackReanchorsOnCutsButNeverExtrapolatesAStalledScene) {
  aurora::stereo::ScenePlaybackClock clock;
  EXPECT_EQ(clock.sample_time(100), 0u);
  clock.begin_scene(1000, 100, false);
  EXPECT_EQ(clock.sample_time(105), 1005u);
  clock.begin_scene(1010, 113, true);
  EXPECT_EQ(clock.sample_time(115), 1015u); // Seal latency is not a new clock origin.
  EXPECT_FLOAT_EQ(aurora::stereo::interpolation_weight(clock.sample_time(500), 1010, 10), 1);
  clock.begin_scene(2000, 500, false); // Stall recovery / scene change.
  EXPECT_EQ(clock.sample_time(505), 2005u);
  clock.begin_scene(100, 510, true); // Reset producer schedule.
  EXPECT_EQ(clock.sample_time(515), 105u);
  clock.begin_scene(110, 20, true); // Reset host clock.
  EXPECT_EQ(clock.sample_time(25), 115u);
  EXPECT_EQ(clock.sample_time(19), 0u);
}

TEST(StereoInterpolation, EarlierProductionAfterWarmupDoesNotClampToPreviousEndpoints) {
  aurora::stereo::ScenePlaybackClock clock;
  clock.begin_scene(1000, 100, false);
  clock.begin_scene(1010, 106, true); // Producer sheds four time units of warm-up latency.
  EXPECT_EQ(clock.sample_time(106), 1010u);
  EXPECT_EQ(clock.sample_time(111), 1015u);
  clock.begin_scene(1020, 118, true); // A subsequent late seal must not move the clock back.
  EXPECT_EQ(clock.sample_time(118), 1022u);
  EXPECT_FLOAT_EQ(aurora::stereo::interpolation_weight(clock.sample_time(118), 1020, 10), 0.2f);
}

TEST(StereoInterpolation, MotionDiagnosticsDistinguishCadenceFromSubmissionCount) {
  aurora::stereo::MotionSamples samples;
  constexpr uint64_t boundary = 1'000'000'000, interval = 16'666'667;
  const auto record = [&](uint64_t display, uint64_t sceneBoundary, bool continuous = true) {
    samples.record(display, sceneBoundary, interval, continuous,
                   aurora::stereo::interpolation_weight(display, sceneBoundary, interval));
  };
  record(boundary, boundary);
  record(boundary + interval / 2, boundary);
  record(boundary + interval, boundary);
  // A future display deadline outruns the retained scene; another submission
  // cannot advance its motion, even though it can apply a fresh head pose.
  record(boundary + 2 * interval, boundary);
  record(boundary + 2 * interval, boundary + interval);
  EXPECT_EQ(samples.samples, 5u);
  EXPECT_EQ(samples.blended, 1u);
  EXPECT_EQ(samples.atPrevious, 1u);
  EXPECT_EQ(samples.atCurrent, 3u);
  EXPECT_EQ(samples.repeated, 1u);
  EXPECT_EQ(samples.backwards, 0u);
  EXPECT_EQ(samples.minStep, 0u);
  EXPECT_EQ(samples.maxStep, interval);
  samples.clear_window();
  record(boundary + 2 * interval, boundary + interval);
  EXPECT_EQ(samples.samples, 1u);
  EXPECT_EQ(samples.repeated, 1u); // Preserve cadence across reporting windows.
  record(boundary + interval / 2, boundary);
  EXPECT_EQ(samples.backwards, 1u);
  record(boundary, boundary, false);
  record(boundary, boundary);
  EXPECT_EQ(samples.discontinuous, 1u);
  EXPECT_EQ(samples.backwards, 1u); // A camera cut starts a new sequence.
}
