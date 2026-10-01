// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_model_visibility.h"
#include "gx_internal.h"
#include <aurora/aurora.h>
#include <cstring>

namespace GxModelVisibility {
namespace records {
struct HiddenArray {
    const void* source;
    float modelView[12];
};
void Clear(const uint8_t*, uint32_t) { aurora_clear_hidden_model_arrays(); }
void Hide(const uint8_t* payload, uint32_t size) {
    if (size != sizeof(HiddenArray)) return;
    HiddenArray array{};
    std::memcpy(&array, payload, sizeof(array));
    aurora_hide_model_array(array.source, array.modelView);
}
void Post(const void* source, const float modelView[12]) {
    if (!GxThread::Enabled()) {
        aurora_hide_model_array(source, modelView);
        return;
    }
    HiddenArray array{source, {}};
    std::memcpy(array.modelView, modelView, sizeof(array.modelView));
    GxThread::detail::PostRecord(&Hide, &array, sizeof(array));
}
} // namespace records

void PostClear() {
    if (!GxThread::Enabled()) {
        aurora_clear_hidden_model_arrays();
        return;
    }
    GxThread::detail::PostRecord(&records::Clear, nullptr, 0);
}
bool PostHiddenArray(uint32_t guestArray, uint32_t size, const float modelView[12]) {
    if (!guestArray || !size || size > 65536 || !modelView) return false;
    const void* sdk = GuestToHostPtr(guestArray, size);
    const void* cp = GuestToHostPtr(DecodeCpArrayBaseGuestAddress(guestArray), size);
    if (sdk) records::Post(sdk, modelView);
    if (cp && cp != sdk) records::Post(cp, modelView);
    return sdk || cp;
}
uint32_t LastDrawCount() { return aurora_hidden_model_draw_count(); }
} // namespace GxModelVisibility
