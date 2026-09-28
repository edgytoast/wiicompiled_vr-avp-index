// SPDX-License-Identifier: GPL-3.0-or-later
#include "vr/bullet_bill_model.h"
#include <iostream>
#include <vector>

int main() {
    // Synthetic MDL0: three rigid body arrays and a fourth on the arm joints.
    // No game data is needed to check ownership and offset validation.
    std::vector<uint8_t> mdl(0x600);
    const auto put32 = [&](size_t at, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) mdl[at + i] = uint8_t(value >> ((3 - i) * 8));
    };
    const auto put16 = [&](size_t at, uint32_t value) {
        mdl[at] = uint8_t(value >> 8);
        mdl[at + 1] = uint8_t(value);
    };
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
    };
    put32(0, 0x4d444c30);
    put32(4, uint32_t(mdl.size()));
    put32(0x18, 0x40);
    put32(0x30, 0xa0);
    put32(0x38, 0xa0);
    put32(0x44, 4);
    put32(0xa4, 4);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t array = 0x160 + i * 0x40, shape = 0x280 + i * 0x60, data = 0x480 + i * 24;
        put32(0x40 + 8 + (i + 1) * 16 + 12, array - 0x40);
        put32(array + 8, data - array);
        put32(array + 0x10, i);
        mdl[array + 0x1d] = 12;
        put16(array + 0x1e, 2);
        put32(0xa0 + 8 + (i + 1) * 16 + 12, shape - 0xa0);
        put32(shape + 8, i == 3 ? UINT32_MAX : 0);
        put32(shape + 0xc, i == 3 ? 1 : 0);
        put16(shape + 0x48, i);
    }
    for (uint32_t version : {8u, 9u, 10u, 11u}) {
        put32(8, version);
        const auto arrays = mkw::vr::ReadBulletBillBodyArrays(mdl.data(), mdl.size());
        check(arrays.size() == 3, "body, eyes and cone selected, arms excluded");
        for (size_t i = 0; i < arrays.size(); ++i)
            check(arrays[i].offset == 0x480 + i * 24 && arrays[i].size == 24, "array data range resolved");
    }
    put16(0x3a0 + 0x48, 0);
    auto shared = mkw::vr::ReadBulletBillBodyArrays(mdl.data(), mdl.size());
    check(shared.size() == 2 && shared.front().offset == 0x498, "an array shared with an arm stays visible");
    put16(0x3a0 + 0x48, 3);
    put32(0x280 + 0xc, 1);
    check(mkw::vr::ReadBulletBillBodyArrays(mdl.data(), mdl.size()).size() == 2,
          "an indexed matrix shape cannot be assumed to belong to the body");
    put32(0x280 + 0xc, 0);
    put32(0x160 + 8, UINT32_MAX);
    check(mkw::vr::ReadBulletBillBodyArrays(mdl.data(), mdl.size()).empty(), "escaping array offset rejected");
    put32(0x160 + 8, 0x480 - 0x160);
    put32(0xa0 + 8 + 16 + 12, UINT32_MAX);
    check(mkw::vr::ReadBulletBillBodyArrays(mdl.data(), mdl.size()).empty(), "escaping shape offset rejected");
    check(mkw::vr::ReadBulletBillBodyArrays(mdl.data(), mdl.size() - 1).empty(), "truncated model rejected");
    check(mkw::vr::ReadBulletBillBodyArrays(nullptr, 0).empty(), "missing model rejected");
    if (failures) return 1;
    std::cout << "Bullet Bill model tests passed\n";
}
