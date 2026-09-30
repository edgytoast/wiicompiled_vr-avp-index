#pragma once

#include "gx/frame_interpolation.hpp"
#include "gfx/stereo_replay.hpp"
#include <cstdint>
#include <cstring>

namespace aurora::stereo {

inline bool inverse_rigid_view(const Mat3x4<float>& view, Mat3x4<float>& inverse) noexcept {
  const Vec4<float>* rows[] = {&view.m0, &view.m1, &view.m2};
  for (unsigned i = 0; i < 3; ++i) {
    for (unsigned j = 0; j < 4; ++j) {
      // Hosts may build Aurora with finite-math-only.
      uint32_t bits;
      std::memcpy(&bits, reinterpret_cast<const uint8_t*>(rows[i]) + j * sizeof(float), sizeof(bits));
      if ((bits & 0x7f800000u) == 0x7f800000u) return false;
    }
    for (unsigned j = 0; j < 3; ++j) {
      float dot = 0;
      for (unsigned k = 0; k < 3; ++k) dot += (*rows[i])[k] * (*rows[j])[k];
      if (std::abs(dot - (i == j ? 1.f : 0.f)) > 0.002f) return false;
    }
  }
  const float determinant = view.m0[0] * (view.m1[1] * view.m2[2] - view.m1[2] * view.m2[1]) -
                            view.m0[1] * (view.m1[0] * view.m2[2] - view.m1[2] * view.m2[0]) +
                            view.m0[2] * (view.m1[0] * view.m2[1] - view.m1[1] * view.m2[0]);
  if (determinant < 0.99f) return false;
  Vec4<float>* out[] = {&inverse.m0, &inverse.m1, &inverse.m2};
  for (unsigned i = 0; i < 3; ++i) {
    (*out[i])[3] = 0;
    for (unsigned j = 0; j < 3; ++j) {
      (*out[i])[j] = (*rows[j])[i];
      (*out[i])[3] -= (*rows[j])[i] * (*rows[j])[3];
    }
  }
  return true;
}

// Keep both object endpoints in the current recorded camera's coordinates.
// Sample the actual camera pose separately, so held vertices, unmatched draws
// and rejected object animations still follow smooth game-camera movement.
struct SceneCameraMotion {
  Mat3x4<float> currentFromPrevious{};
  Mat3x4<float> previousFromCurrent{};
  Mat3x4<float> worldFromCurrentScene{};
  Mat3x4<float> previousPose{}, currentPose{};
  bool active = false;

  bool prepare(const Mat3x4<float>& previousView, const Mat3x4<float>& currentView,
               const Mat3x4<float>& previousAnchor, const Mat3x4<float>& currentAnchor) noexcept {
    active = false;
    Mat3x4<float> previousCameraPose{}, sample{};
    if (!inverse_rigid_view(previousView, previousCameraPose) ||
        !inverse_rigid_view(currentView, worldFromCurrentScene) ||
        !gx::interpolate_transform(previousCameraPose, worldFromCurrentScene, 0.5f, sample) ||
        !inverse_rigid_view(gfx::stereo_replay::compose_affine(previousAnchor, previousView), previousPose) ||
        !inverse_rigid_view(gfx::stereo_replay::compose_affine(currentAnchor, currentView), currentPose) ||
        !gx::interpolate_transform(previousPose, currentPose, 0.5f, sample)) return false;
    currentFromPrevious = gfx::stereo_replay::compose_affine(currentView, previousCameraPose);
    previousFromCurrent = gfx::stereo_replay::compose_affine(previousView, worldFromCurrentScene);
    active = true;
    return true;
  }

  bool sample(float weight, Mat3x4<float>& anchorFromCurrentScene) const noexcept {
    Mat3x4<float> pose{}, view{};
    if (!active || !gx::interpolate_transform(previousPose, currentPose, weight, pose) ||
        !inverse_rigid_view(pose, view)) return false;
    anchorFromCurrentScene = gfx::stereo_replay::compose_affine(view, worldFromCurrentScene);
    return true;
  }
};
} // namespace aurora::stereo
