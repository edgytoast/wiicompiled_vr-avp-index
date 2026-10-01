// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gx.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace aurora::gx {
struct HiddenModelArray {
  const void* source = nullptr;
  std::array<float, 12> modelView{};
};
inline std::vector<HiddenModelArray> hiddenModelArrays;
inline uint32_t hiddenModelDraws = 0;
inline std::atomic<uint32_t> hiddenModelLastDraws{0};

// Called before uploading vertices or merging draws. Matrix-indexed shapes
// are deliberately excluded: Bullet Bill's arms have their own animated joints.
inline bool model_array_hidden() {
  if (hiddenModelArrays.empty() || g_gxState.projType != GX_PERSPECTIVE ||
      (g_gxState.vtxDesc[GX_VA_POS] != GX_INDEX8 && g_gxState.vtxDesc[GX_VA_POS] != GX_INDEX16) ||
      g_gxState.vtxDesc[GX_VA_PNMTXIDX] != GX_NONE || g_gxState.currentPnMtx >= MaxPnMtx)
    return false;
  const auto* matrix = reinterpret_cast<const float*>(&g_gxState.pnMtx[g_gxState.currentPnMtx].pos);
  for (const auto& hidden : hiddenModelArrays) {
    if (g_gxState.arrays[GX_VA_POS].data != hidden.source) continue;
    bool matches = true;
    for (int i = 0; i < 12; ++i) {
      uint32_t actual, expected;
      std::memcpy(&actual, &matrix[i], 4);
      std::memcpy(&expected, &hidden.modelView[i], 4);
      if ((actual & 0x7f800000u) == 0x7f800000u || (expected & 0x7f800000u) == 0x7f800000u ||
          std::abs(matrix[i] - hidden.modelView[i]) > (i % 4 == 3 ? 0.1f : 0.002f)) {
        matches = false;
        break;
      }
    }
    if (matches) {
      ++hiddenModelDraws;
      return true;
    }
  }
  return false;
}
} // namespace aurora::gx
