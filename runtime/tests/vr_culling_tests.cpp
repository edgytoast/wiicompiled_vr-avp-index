// SPDX-License-Identifier: GPL-3.0-or-later
//
// The frustum test behind Mario Kart's scene culling (vr/mkw_vr_culling.h):
// the native replacement of nw4r::math::FRUSTUM::IntersectAABB_Ex has to
// classify boxes exactly as the PowerPC original, since with vr.object_culling
// on it runs for every scene object of every frame.
#include "vr/mkw_vr_culling.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>

using mkw::vr::CullingAabb;
using mkw::vr::CullingFrustum;
using mkw::vr::CullingPlane;
using mkw::vr::FrustumAabbResult;
using mkw::vr::FrustumIntersectAabb;

namespace {

// nw4r's return codes, which ScnObjGather::Add tests by value.
static_assert(static_cast<int32_t>(FrustumAabbResult::Outside) == 0, "");
static_assert(static_cast<int32_t>(FrustumAabbResult::Inside) == 1, "");
static_assert(static_cast<int32_t>(FrustumAabbResult::Partial) == 2, "");

// The layout the native reads from guest memory.
static_assert(mkw::vr::kFrustumPlanesOffset - mkw::vr::kFrustumBoxOffset == sizeof(CullingAabb), "");
static_assert(sizeof(CullingPlane) == mkw::vr::kFrustumPlaneStride, "");
static_assert(mkw::vr::kClipScreenPlanesOffset + mkw::vr::kClipScreenPlanesBytes ==
                  mkw::vr::kClipScreenDrawScaleOffset,
              "six plane normals fill the screen info up to the draw scale");

// NaN and infinity from their bits: a literal one is undefined behaviour when
// the test is built with the runtime's -ffast-math, which it is worth trying.
float FloatFromBits(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

CullingAabb Box(float x0, float y0, float z0, float x1, float y1, float z1) {
    return CullingAabb{{x0, y0, z0}, {x1, y1, z1}};
}

// An axis-aligned "frustum": six outward planes at +-half on each axis, and
// the frustum's own bounding box at +-box_half.
CullingFrustum AxisFrustum(float half, float box_half) {
    CullingFrustum f{};
    f.box = Box(-box_half, -box_half, -box_half, box_half, box_half, box_half);
    size_t p = 0;
    for (size_t axis = 0; axis < 3; ++axis) {
        for (float sign : {1.0f, -1.0f}) {
            CullingPlane& plane = f.planes[p++];
            plane.normal[axis] = sign;
            plane.distance = -half;
        }
    }
    return f;
}

} // namespace

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAILED: " << message << '\n'; ++failures; }
    };
    const auto expect = [&](FrustumAabbResult got, FrustumAabbResult want, const char* message) {
        if (got != want) {
            std::cerr << "FAILED: " << message << " (got " << static_cast<int>(got) << ", want "
                      << static_cast<int>(want) << ")\n";
            ++failures;
        }
    };

    const CullingFrustum f = AxisFrustum(10.0f, 10.0f);
    expect(FrustumIntersectAabb(f, Box(-1, -1, -1, 1, 1, 1)), FrustumAabbResult::Inside,
           "a box within every plane is inside");
    expect(FrustumIntersectAabb(f, Box(5, -1, -1, 15, 1, 1)), FrustumAabbResult::Partial,
           "a box across the +x plane is partial");
    expect(FrustumIntersectAabb(f, Box(-15, -1, -1, -5, 1, 1)), FrustumAabbResult::Partial,
           "a box across the -x plane is partial (the negative normal picks the other corner)");
    expect(FrustumIntersectAabb(f, Box(11, -1, -1, 12, 1, 1)), FrustumAabbResult::Outside,
           "a box past the frustum's bounding box is rejected");
    expect(FrustumIntersectAabb(f, Box(-1, -1, -12, 1, 1, -11)), FrustumAabbResult::Outside,
           "the bounding-box rejection covers z as well");
    expect(FrustumIntersectAabb(f, Box(-10, -10, -10, 10, 10, 10)), FrustumAabbResult::Inside,
           "touching every plane exactly is still inside (outside needs strictly greater)");

    // Planes tighter than the frustum's bounding box: only the plane test can
    // reject.
    const CullingFrustum tight = AxisFrustum(10.0f, 100.0f);
    expect(FrustumIntersectAabb(tight, Box(20, -1, -1, 30, 1, 1)), FrustumAabbResult::Outside,
           "a box beyond the +x plane is outside");
    expect(FrustumIntersectAabb(tight, Box(-1, -30, -1, 1, -20, 1)), FrustumAabbResult::Outside,
           "a box beyond the -y plane is outside");
    expect(FrustumIntersectAabb(tight, Box(-50, -50, -50, 50, 50, 50)), FrustumAabbResult::Partial,
           "a box enclosing the whole frustum is partial");

    // NaN follows each branch of the original. The plane tests keep going only
    // on an ordered "<= 0", so a NaN distance rejects the box.
    const float nan = FloatFromBits(0x7FC00000u);
    expect(FrustumIntersectAabb(f, Box(nan, -1, -1, nan, 1, 1)), FrustumAabbResult::Outside,
           "a NaN coordinate makes a plane distance NaN, which rejects");
    CullingFrustum nan_plane = f;
    nan_plane.planes[0].normal[0] = nan;
    expect(FrustumIntersectAabb(nan_plane, Box(-1, -1, -1, 1, 1, 1)), FrustumAabbResult::Outside,
           "a NaN normal gives a NaN distance, which rejects");
    // The bounding-box compares reject on an ordered ">" (bgt), except the last
    // one, which keeps on an ordered "<=" (ble). Planes that accept everything
    // isolate those compares.
    CullingFrustum open{};
    open.box = Box(-10, -10, -10, 10, 10, 10);
    for (CullingPlane& plane : open.planes) plane.distance = -1.0f;
    expect(FrustumIntersectAabb(open, Box(-1, -1, -1, 1, 1, 1)), FrustumAabbResult::Inside,
           "zero-normal planes accept a box inside the bounding box");
    open.box.min[0] = nan;
    expect(FrustumIntersectAabb(open, Box(-1, -1, -1, 1, 1, 1)), FrustumAabbResult::Inside,
           "a NaN frustum min.x fails bgt and keeps the box");
    open.box.min[0] = -10.0f;
    open.box.min[2] = nan;
    expect(FrustumIntersectAabb(open, Box(-1, -1, -1, 1, 1, 1)), FrustumAabbResult::Outside,
           "a NaN frustum min.z fails ble and rejects the box");
    open.box.min[2] = -10.0f;
    open.box.max[2] = nan;
    expect(FrustumIntersectAabb(open, Box(-1, -1, -1, 1, 1, 1)), FrustumAabbResult::Inside,
           "a NaN frustum max.z fails bgt and keeps the box");
    check(mkw::vr::CullingIsNan(nan) && !mkw::vr::CullingIsNan(1.0f) &&
              !mkw::vr::CullingIsNan(FloatFromBits(0x7F800000u)),
          "the bit-level NaN test");

    // The plane distance is one fused multiply-add for x (ps_madd) after a
    // rounded product for y: choose values where fusing decides the sign.
    // nx * vx = 1 + 2^-22 + 2^-46 exactly, which rounds to 1 + 2^-22 on its
    // own; yy cancels that rounded value, so only a fused step keeps 2^-46.
    {
        const float one_plus = 1.0f + std::ldexp(1.0f, -23);
        CullingFrustum fused = AxisFrustum(1000.0f, 1000.0f);
        fused.planes[0].normal[0] = one_plus;
        fused.planes[0].normal[1] = 1.0f;
        fused.planes[0].normal[2] = 0.0f;
        fused.planes[0].distance = -std::ldexp(1.0f, -47);
        const float yy = -(1.0f + std::ldexp(1.0f, -22));
        expect(FrustumIntersectAabb(fused, Box(one_plus, yy, 0, one_plus, yy, 0)),
               FrustumAabbResult::Outside, "the x term is fused like ps_madd, so 2^-46 survives");
        fused.planes[0].distance = -std::ldexp(1.0f, -45);
        expect(FrustumIntersectAabb(fused, Box(one_plus, yy, 0, one_plus, yy, 0)),
               FrustumAabbResult::Inside, "and a larger distance still keeps the point inside");
    }
    check(mkw::vr::kFrustumPlaneCount == 6, "six frustum planes");

    if (failures == 0) {
        std::cout << "vr_culling_tests: all passed\n";
    }
    return failures == 0 ? 0 : 1;
}
