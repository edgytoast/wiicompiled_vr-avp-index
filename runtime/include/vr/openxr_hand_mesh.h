// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from heurazy's mario-kart-wii-VR-port (GPL-3.0-or-later).
#pragma once
#include "vr/openxr_runtime.h"
#include <aurora/aurora.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace mkw::vr {
// AnimalCrossing-VR-MR-Standalone obtains its white hands from this Meta
// runtime extension, not from a distributable model asset. Use the same API,
// with a procedural fallback on PC runtimes that do not expose Meta meshes.
// `existing` are the tracked hands' trackers (OpenXRInput::HandTracker), used
// where there is one; otherwise a tracker is made just for the mesh.
inline bool LoadRuntimeHandMeshes(OpenXRRuntime& runtime, const XrHandTrackerEXT* existing = nullptr) {
    for (uint32_t h = 0; h < 2; ++h)
        aurora_set_vr_hand_mesh(h, nullptr, 0, nullptr, 0, nullptr, nullptr, 0);
    const auto& extensions = runtime.EnabledExtensions();
    if (std::find(extensions.begin(), extensions.end(), XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME) == extensions.end())
        return false;
    PFN_xrCreateHandTrackerEXT create = nullptr;
    PFN_xrDestroyHandTrackerEXT destroy = nullptr;
    PFN_xrGetHandMeshFB meshFn = nullptr;
    xrGetInstanceProcAddr(runtime.Instance(), "xrCreateHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&create));
    xrGetInstanceProcAddr(runtime.Instance(), "xrDestroyHandTrackerEXT", reinterpret_cast<PFN_xrVoidFunction*>(&destroy));
    xrGetInstanceProcAddr(runtime.Instance(), "xrGetHandMeshFB", reinterpret_cast<PFN_xrVoidFunction*>(&meshFn));
    if (!create || !destroy || !meshFn) return false;
    bool any = false;
    for (uint32_t h = 0; h < 2; ++h) {
        XrHandTrackerEXT tracker = existing != nullptr ? existing[h] : XR_NULL_HANDLE;
        const bool owned = tracker == XR_NULL_HANDLE;
        if (owned) {
            XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            info.hand = h ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
            info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            if (XR_FAILED(create(runtime.Session(), &info, &tracker))) continue;
        }
        struct Guard {
            XrHandTrackerEXT tracker; PFN_xrDestroyHandTrackerEXT destroy; bool owned;
            ~Guard() { if (owned) destroy(tracker); }
        } guard{tracker, destroy, owned};
        XrHandTrackingMeshFB mesh{XR_TYPE_HAND_TRACKING_MESH_FB};
        if (XR_FAILED(meshFn(tracker, &mesh)) || mesh.jointCountOutput != 26 ||
            !mesh.vertexCountOutput || mesh.vertexCountOutput > 65535 || !mesh.indexCountOutput ||
            mesh.indexCountOutput > 100000 || mesh.indexCountOutput % 3) continue;
        std::vector<XrPosef> poses(mesh.jointCountOutput);
        std::vector<float> radii(mesh.jointCountOutput);
        std::vector<XrHandJointEXT> parents(mesh.jointCountOutput);
        std::vector<XrVector3f> positions(mesh.vertexCountOutput), normals(mesh.vertexCountOutput);
        std::vector<XrVector2f> uv(mesh.vertexCountOutput);
        std::vector<XrVector4sFB> joints(mesh.vertexCountOutput);
        std::vector<XrVector4f> weights(mesh.vertexCountOutput);
        std::vector<int16_t> indices(mesh.indexCountOutput);
        mesh.jointCapacityInput = poses.size(); mesh.jointBindPoses = poses.data();
        mesh.jointRadii = radii.data(); mesh.jointParents = parents.data();
        mesh.vertexCapacityInput = positions.size(); mesh.vertexPositions = positions.data();
        mesh.vertexNormals = normals.data(); mesh.vertexUVs = uv.data();
        mesh.vertexBlendIndices = joints.data(); mesh.vertexBlendWeights = weights.data();
        mesh.indexCapacityInput = indices.size(); mesh.indices = indices.data();
        if (XR_FAILED(meshFn(tracker, &mesh))) continue;
        std::vector<AuroraVRHandVertex> vertices(positions.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            auto& v = vertices[i];
            std::memcpy(v.position, &positions[i], sizeof(v.position));
            std::memcpy(v.joints, &joints[i], sizeof(v.joints));
            std::memcpy(v.weights, &weights[i], sizeof(v.weights));
        }
        std::array<float, 26 * 7> bind{};
        std::array<int32_t, 26> parentIds{};
        for (size_t j = 0; j < poses.size(); ++j) {
            std::memcpy(bind.data() + j * 7, &poses[j], 7 * sizeof(float));
            parentIds[j] = static_cast<int32_t>(parents[j]);
        }
        aurora_set_vr_hand_mesh(h, vertices.data(), vertices.size(),
            reinterpret_cast<const uint16_t*>(indices.data()), indices.size(), bind.data(), parentIds.data(), poses.size());
        any = true;
    }
    return any;
}
} // namespace mkw::vr
