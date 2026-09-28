// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace GxModelVisibility {
// Game thread. These requests are ordered with the frame's GX commands.
void PostClear();
// Hide rigid draws using this array and this instance's model-view matrix.
// Shared models at other transforms and indexed joints are kept visible.
bool PostHiddenArray(uint32_t guestArray, uint32_t size, const float modelView[12]);
uint32_t LastDrawCount();
} // namespace GxModelVisibility
