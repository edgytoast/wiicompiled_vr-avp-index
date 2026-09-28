// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mkw::vr {

struct BulletBillBodyArray {
    uint32_t offset = 0;
    uint32_t size = 0;
};

// kart_killer's body, eye and exhaust shapes are rigid on matrix 0. Its arms
// use other matrices. Select only position arrays used exclusively by rigid
// body shapes, so even a mod sharing positions with an arm keeps those draws.
// Offsets are relative to the MDL0; no names or game assets are embedded here.
inline std::vector<BulletBillBodyArray> ReadBulletBillBodyArrays(const uint8_t* mdl, size_t size) {
    if (!mdl || size < 0x40) return {};
    const auto contains = [size](size_t at, size_t length) { return at <= size && length <= size - at; };
    const auto read32 = [mdl](size_t at) {
        return (uint32_t(mdl[at]) << 24) | (uint32_t(mdl[at + 1]) << 16) |
               (uint32_t(mdl[at + 2]) << 8) | mdl[at + 3];
    };
    const auto read16 = [mdl](size_t at) { return (uint32_t(mdl[at]) << 8) | mdl[at + 1]; };
    const uint32_t version = read32(8);
    if (read32(0) != 0x4d444c30 || read32(4) != size || version < 8 || version > 11) return {};
    const size_t positions = read32(0x18), shapes = read32(version >= 10 ? 0x38 : 0x30);
    if (!positions || !shapes || !contains(positions, 8) || !contains(shapes, 8)) return {};
    const uint32_t positionCount = read32(positions + 4), shapeCount = read32(shapes + 4);
    if (positionCount > 64 || shapeCount > 4096 ||
        !contains(positions + 8, size_t(positionCount + 1) * 16) ||
        !contains(shapes + 8, size_t(shapeCount + 1) * 16)) return {};
    std::vector<BulletBillBodyArray> result;
    for (uint32_t i = 1; i <= positionCount; ++i) {
        const size_t array = positions + size_t(read32(positions + 8 + i * 16 + 12));
        if (!contains(array, 0x38)) return {};
        const uint32_t id = read32(array + 0x10);
        bool body = false, other = false;
        for (uint32_t j = 1; j <= shapeCount; ++j) {
            const size_t shape = shapes + size_t(read32(shapes + 8 + j * 16 + 12));
            if (!contains(shape, 0x60)) return {};
            if (read16(shape + 0x48) != id) continue;
            const bool rigidBody = read32(shape + 8) == 0 && (read32(shape + 0xc) & 1u) == 0;
            body |= rigidBody;
            other |= !rigidBody;
        }
        if (!body || other) continue;
        const size_t data = array + size_t(read32(array + 8));
        const uint32_t bytes = uint32_t(mdl[array + 0x1d]) * read16(array + 0x1e);
        if (!bytes || !contains(data, bytes)) return {};
        result.push_back({static_cast<uint32_t>(data), bytes});
    }
    return result;
}

} // namespace mkw::vr
