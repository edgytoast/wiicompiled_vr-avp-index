#pragma once

#include "gx.hpp"

#include "aurora/gfx.h"

#include <atomic>

// Frame interpolation: generates intermediate presentation frames between two consecutive 60 Hz
// guest frames by re-staging each perspective draw's uniform block with interpolated transforms.
namespace aurora::gx {
constexpr uint32_t MaxInterpolatedFrames = 3;

// Identifies a draw across frames. Match exact `combined` first, then `pipeline`/`geometry`
// across texture animation, then `pipeline`/`texture` for meshes whose vertex data changes.
struct FrameInterpolationDrawIdentity {
  HashType combined = 0;
  HashType pipeline = 0;
  HashType texture = 0;
  // Hash of the per-vertex PNMTXIDX stream. Matrix values animate, but this topology decides which
  // absolute palette slot each vertex reads, so it must match across a pair.
  HashType matrixTopology = 0;
  // Exact geometry without its texture binding. Texture-pattern animations may
  // swap images while the same mesh still needs continuous camera/object motion.
  // Zero means unavailable; do not use it as a wildcard geometry match.
  HashType geometry = 0;
};

// CPU-authored particle quads move inside their vertex stream, often with an
// identity position matrix. Track their center while retaining the current
// shape, UVs and colour; no renderer thread needs access to guest particles.
struct DrawVertexMotion {
  std::array<float, 3> center{};
  bool enabled = false;
};

// Matching-only shape of such a quad: the two edges leaving its first corner.
// Emitters draw many look-alike quads, and a speed line moves further per
// frame than the gap to its neighbours; its size and orientation still tell
// them apart. Replay never needs it, so it stays out of UniformReplayLayout.
struct DrawVertexShape {
  std::array<float, 3> edge0{}, edge1{};
};

inline Mat3x4<float> offset_transform_origin(Mat3x4<float> matrix,
                                            const std::array<float, 3>& center, float sign = 1.f) noexcept {
  for (auto* row : {&matrix.m0, &matrix.m1, &matrix.m2})
    (*row)[3] += sign * ((*row)[0] * center[0] + (*row)[1] * center[1] + (*row)[2] * center[2]);
  return matrix;
}

// Where the transforms live inside a draw's staged uniform block. Offsets are
// relative to the start of the mapped range.
struct InterpolatedUniformLayout {
  const uint8_t* sourceUniformData = nullptr;
  size_t uniformSize = 0;
  size_t projectionOffset = 0;
  size_t positionOffset = 0;
  size_t normalOffset = 0;
  // Slot the live matrix occupies; a compacted position region holds it at 0.
  size_t currentMatrix = 0;
  bool indexedMatrices = false;
  DrawVertexMotion vertexMotion{};
  DrawVertexShape vertexShape{};
};

namespace detail {
// Defined in frame_interpolation.cpp, exposed so the early-outs below stay inline: the shipped
// build compiles shards without LTO, so a cross-TU call would land on every draw in the frame.
extern std::atomic_uint32_t g_frameInterpolationFps;
extern std::atomic_bool g_stereoFrameInterpolation;
} // namespace detail

// 0 when interpolation is disabled; otherwise the configured target (120/180/240).
inline uint32_t frame_interpolation_fps() noexcept {
  return detail::g_frameInterpolationFps.load(std::memory_order_acquire);
}
inline bool stereo_frame_interpolation_active() noexcept {
  return detail::g_stereoFrameInterpolation.load(std::memory_order_acquire);
}
inline bool frame_interpolation_active() noexcept {
  return frame_interpolation_fps() != 0 || stereo_frame_interpolation_active();
}

void set_frame_interpolation_fps(uint32_t targetFps) noexcept;

// Feedback for the adaptive slot controller: whether the producer met its
// retrace boundary for the frame that just presented.
void report_producer_paced(bool paced) noexcept;

void begin_frame_interpolation() noexcept;
void finalize_frame_interpolation() noexcept;
// Before seal, re-express previous VR endpoints in the current recorded camera.
// Both null disables the operation. begin_frame_interpolation clears it.
void set_frame_interpolation_view_rebase(const Mat3x4<float>* currentFromPrevious,
                                         const Mat3x4<float>* previousFromCurrent) noexcept;
// Fills the observability snapshot behind aurora_get_frame_interpolation_diagnostics.
void get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics& diagnostics) noexcept;
bool has_interpolated_frame() noexcept;
uint32_t interpolated_frame_count() noexcept;
void mark_frame_interpolation_replay_unsafe() noexcept;
// Drops the interpolation tasks staged into the currently mapped uniform range. Required of
// anything that unmaps or rotates that range before the frame is finalized.
void drop_pending_frame_interpolation_uniforms() noexcept;
bool frame_interpolation_replay_safe() noexcept;

// Records one perspective draw and maps its intermediate uniform copies, returning the mapped
// range per slot (empty when the draw has no counterpart). Called by build_uniform.
std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(const FrameInterpolationDrawIdentity& identity,
                                                                        const Mat4x4<float>& projection,
                                                                        uint16_t usedPnMtxMask,
                                                                        const InterpolatedUniformLayout& uniformLayout,
                                                                        gfx::Range* previousUniform = nullptr) noexcept;

// Folds a merged draw back into the snapshot of the draw it joined. aurora renders merged
// primitives through the first one's uniform block, so without this the merged-in bones tear.
void extend_interpolation_draw(uint16_t usedPnMtxMask) noexcept;

bool interpolate_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                           float weight, Mat3x4<float>& output) noexcept;
bool interpolate_transform_midpoint(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                    Mat3x4<float>& output) noexcept;
// Matched rigid draws can spin more than 90 degrees per guest frame (kart tires), and
// a draw matrix may shear (Lakitu's sway tilts his whole body). Camera/seat anchors
// keep the conservative rotation and rigidity guards above.
bool interpolate_draw_transform(const Mat3x4<float>& previous, const Mat3x4<float>& current,
                                float weight, Mat3x4<float>& output) noexcept;
// Matrix palettes are already-composed skinning transforms, so interpolating their coefficients
// keeps shared boundaries intact. Ordinary one-matrix draws keep the rigid TRS path above.
bool interpolate_indexed_transform(const Mat3x4<float>& previous,
                                   const Mat3x4<float>& current, float weight,
                                   Mat3x4<float>& output) noexcept;
float transform_match_distance_squared(const Mat3x4<float>& previous,
                                       const Mat3x4<float>& current) noexcept;
} // namespace aurora::gx
