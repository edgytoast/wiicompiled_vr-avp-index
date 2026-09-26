#pragma once

#include "gx.hpp"
// MaxInterpolatedFrames / FrameInterpolationDrawIdentity, and the interpolation API that build_uniform feeds.
#include "frame_interpolation.hpp"

namespace aurora::gx {
struct UniformReplayLayout {
  uint32_t projectionOffset = 0;
  uint32_t positionOffset = 0;
  uint32_t normalOffset = 0;
  uint32_t positionMatrixMask = 0;
  uint8_t positionMatrixCount = 0;
  uint8_t normalMatrixCount = 0;
  bool perspective = false;
  bool indexedMatrices = false;
  // A 2D draw compositing the framebuffer back over itself: bloom, blur and the
  // rest of the native post-processing chain. It belongs to the rendered image,
  // not to the game's 2D layer, so it must stay where the game aimed it.
  bool nativeEfbEffect = false;
  // For a native effect that samples a freshly produced, frame-sized depth copy: that
  // copy. Mario Kart Wii draws a ghost kart alone into the cleared EFB, copies the
  // frame's colour and depth out, draws the race, then blends the copies back with one
  // such quad. Set when the draw is recorded; the pass that produced the copy holds the
  // ghost's own perspective draws.
  const gfx::TextureRef* compositeDepthCopy = nullptr;
  // Index of that pass once the frame is sealed (link_composite_sources), or -1. An eye
  // re-issues its perspective draws in this composite's place instead of stamping the
  // desktop's flat image of the ghost over its own view.
  int32_t compositeSourcePass = -1;
};

struct UniformRanges {
  gfx::Range current;
  std::array<gfx::Range, MaxInterpolatedFrames> interpolated;
  gfx::Range previous;
  UniformReplayLayout replayLayout;
};

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
Light prepare_shader_light(Light light) noexcept;
UniformRanges build_uniform(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges,
                            const FrameInterpolationDrawIdentity& drawIdentity, bool perspective,
                            uint16_t usedPnMtxMask = 1) noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
