// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Mario Kart's own object culling, and the VR switch that turns it off
// (vr.object_culling = false; F10 > Camera, or the headset settings panel's
// Camera tab). It applies while VR is enabled, except in the Flat screen race
// view.
//
// The game hides what its chase camera cannot see in two places, both keyed
// to the game camera and not to the headset, so in VR a wide head turn or a
// first-person look over the shoulder reveals missing karts and characters:
//
// 1. nw4r::g3d::ScnObjGather::Add tests every scene object's bounding box
//    against the camera frustum through nw4r::math::FRUSTUM::IntersectAABB_Ex
//    (0x80086610, its only caller) and drops the ones outside.
// 2. ClipInfoMgr::Update tests every ClipInfo (karts, objects, items) against
//    the per-screen ClipScreenInfo that ClipInfoMgr::UpdateScreenInfo
//    (0x8078707C) derives from the camera: a draw distance, the area groups
//    and six side-plane normals. Models outside a plane by more than their
//    radius are flagged clipped and ModelDirector hides them.
//
// mkw_vr_culling.cpp replaces both functions natively with faithful
// reimplementations (the translator drops the translated body of a natively
// registered address, so there is no original left to fall through to). With
// culling off, the frustum test reports every box as partially inside and the
// screen info carries zero plane normals, which no model can be beyond. The
// draw-distance and area-group clipping stay as the game decides them: they do
// not depend on where the player looks.
//
// The pure parts live here so runtime/tests/vr_culling_tests.cpp can check
// them without guest memory. Every offset is specific to PAL RMCP01.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mkw::vr {

// nw4r::math::AABB: minimum corner, then maximum corner.
struct CullingAabb {
    float min[3];
    float max[3];
};

// One nw4r::math::PLANE as FRUSTUM stores its six: an outward normal and a
// distance, so a point is outside when dot(normal, point) + distance > 0.
struct CullingPlane {
    float normal[3];
    float distance;
};

// The two parts of nw4r::math::FRUSTUM that IntersectAABB_Ex reads: the
// frustum's own bounding box (+0x78) and its six planes (+0x90, 16 bytes each).
struct CullingFrustum {
    CullingAabb box;
    CullingPlane planes[6];
};

inline constexpr uint32_t kFrustumBoxOffset = 0x78u;
inline constexpr uint32_t kFrustumPlanesOffset = 0x90u;
inline constexpr uint32_t kFrustumPlaneStride = 0x10u;
inline constexpr size_t kFrustumPlaneCount = 6;

// IntersectAABB_Ex's return value; ScnObjGather::Add keeps the object for
// anything but Outside.
enum class FrustumAabbResult : int32_t {
    Outside = 0,
    Inside = 1,
    Partial = 2,
};

// The PowerPC compares behind IntersectAABB_Ex's branches (fcmpo). Each is
// true only for an ordered result, so a NaN operand makes every one of them
// false, and whether that rejects or keeps a box depends on which way the
// original branches. The NaN test works on the bits, so it holds even in a
// translation unit built with -ffast-math, where a plain float compare may
// assume NaN never occurs.
inline bool CullingIsNan(float value) noexcept {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7FFFFFFFu) > 0x7F800000u;
}

inline bool CullingOrderedGreater(float a, float b) noexcept {
    return !CullingIsNan(a) && !CullingIsNan(b) && a > b;
}

inline bool CullingOrderedLessOrEqual(float a, float b) noexcept {
    return !CullingIsNan(a) && !CullingIsNan(b) && a <= b;
}

inline bool CullingOrderedGreaterOrEqual(float a, float b) noexcept {
    return !CullingIsNan(a) && !CullingIsNan(b) && a >= b;
}

#if defined(__clang__)
// The runtime's unity batches build with -ffast-math. mkw_vr_culling.cpp, the
// only caller, is built with the translated code's precise options instead
// (cmake/PublicProducts.cmake); these pragmas keep the definition's rounding
// exact wherever else the header is compiled.
#pragma float_control(push)
#pragma float_control(precise, on)
#endif

// nw4r::math::FRUSTUM::IntersectAABB_Ex, operation for operation. Every
// arithmetic step is a separate single-precision statement, and the one fused
// multiply-add (ps_madd, which the translated code runs as one fused
// single-precision FMA per lane) is an explicit fmaf, so the result is
// bit-identical to the translated original whatever the host's contraction
// setting. Each test names the branch the original takes.
inline FrustumAabbResult FrustumIntersectAabb(const CullingFrustum& frustum,
                                              const CullingAabb& aabb) noexcept {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const CullingAabb& box = frustum.box;
    // The frustum's bounding box first: bgt rejects, except for the last
    // compare, where ble keeps (so a NaN there rejects).
    for (size_t axis = 0; axis < 3; ++axis) {
        if (CullingOrderedGreater(aabb.min[axis], box.max[axis])) {
            return FrustumAabbResult::Outside;
        }
        if (axis < 2) {
            if (CullingOrderedGreater(box.min[axis], aabb.max[axis])) {
                return FrustumAabbResult::Outside;
            }
        } else if (!CullingOrderedLessOrEqual(box.min[axis], aabb.max[axis])) {
            return FrustumAabbResult::Outside;
        }
    }
    FrustumAabbResult result = FrustumAabbResult::Inside;
    for (size_t p = 0; p < kFrustumPlaneCount; ++p) {
        const CullingPlane& plane = frustum.planes[p];
        // The corner the normal points away from (the box's least value along
        // the plane) and the corner it points at (its greatest). A NaN normal
        // component fails the ordered >= and takes the second choice.
        float least[3];
        float greatest[3];
        for (size_t axis = 0; axis < 3; ++axis) {
            const bool non_negative = CullingOrderedGreaterOrEqual(plane.normal[axis], 0.0f);
            least[axis] = non_negative ? aabb.min[axis] : aabb.max[axis];
            greatest[axis] = non_negative ? aabb.max[axis] : aabb.min[axis];
        }
        // ps_mul (y, z), ps_madd (x onto y), ps_sum0 (+ z), fadds (+ distance).
        const auto signed_distance = [&plane](const float corner[3]) noexcept {
            const float yy = plane.normal[1] * corner[1];
            const float zz = plane.normal[2] * corner[2];
            const float xy = std::fmaf(plane.normal[0], corner[0], yy);
            const float sum = xy + zz;
            return plane.distance + sum;
        };
        // Both branches are ble over "keep going": anything but an ordered
        // <= 0, NaN included, rejects the box or marks it partial.
        if (!CullingOrderedLessOrEqual(signed_distance(least), 0.0f)) {
            return FrustumAabbResult::Outside;
        }
        if (!CullingOrderedLessOrEqual(signed_distance(greatest), 0.0f)) {
            result = FrustumAabbResult::Partial;
        }
    }
    return result;
}

#if defined(__clang__)
#pragma float_control(pop)
#endif

// ClipScreenInfo, 0x60 bytes per screen, as ClipInfoMgr::UpdateScreenInfo
// fills it and ClipInfoMgr::Update reads it.
inline constexpr uint32_t kClipScreenInfoBytes = 0x60u;
// The camera position (+0x00) and the camera's own distance value (+0x0C).
inline constexpr uint32_t kClipScreenCameraOffset = 0x00u;
inline constexpr uint32_t kClipScreenDistanceOffset = 0x0Cu;
// Six side-plane normals: left and right, their widened copies, top and
// bottom, six vectors from +0x10 to +0x57.
inline constexpr uint32_t kClipScreenPlanesOffset = 0x10u;
inline constexpr uint32_t kClipScreenPlanesBytes = 0x48u;
// The squared draw-distance scale (+0x58) and the area 8/9 group bits (+0x5C).
inline constexpr uint32_t kClipScreenDrawScaleOffset = 0x58u;
inline constexpr uint32_t kClipScreenAreaGroupsOffset = 0x5Cu;

// Whether the game's culling is in force. Off only while VR is enabled,
// vr.object_culling is false and the race view is not Flat screen; the
// natives read it on every call.
void MkwVRSetObjectCulling(bool enabled) noexcept;
bool MkwVRObjectCullingEnabled() noexcept;

// Applies vr.object_culling for the given VR state, and remembers that state
// so the settings overlay can re-apply a changed value or race view.
void MkwVRObjectCullingApplyConfiguredSettings(bool vr_enabled) noexcept;
void MkwVRObjectCullingApplyConfiguredSettings() noexcept;

} // namespace mkw::vr
