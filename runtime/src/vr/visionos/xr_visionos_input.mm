// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenXR actions for the visionOS provider, fed by ARKit hand tracking.
//
// Apple Vision Pro has no tracked controllers of its own, so the action
// bindings openxr_input.cpp suggests (the Touch profile, then the simple
// controller) are served from the hands: a pinch of the index finger is the
// trigger and select, the middle, ring and little fingers pinched are the face
// buttons and menu, curling the fingers is the grip squeeze, and the aim and
// grip poses are built from the wrist and knuckles. There is no thumbstick, so
// a race still wants a gamepad (SDL sees those directly); the hands carry the
// menus, the pointer and hand steering. Haptics have nowhere to go.

#include "xr_visionos_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace mkw::vr::visionos {
namespace {

struct ActionSet;

struct Action {
    ActionSet* set = nullptr;
    std::string name;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::vector<XrPath> subactionPaths;
    // Previous booleans per hand, for changedSinceLastSync.
    std::array<bool, 3> lastBoolean{};
    std::array<int64_t, 3> lastChange{};
};

struct ActionSet {
    Instance* instance = nullptr;
    std::string name;
    std::vector<std::unique_ptr<Action>> actions;
    bool attached = false;
};

struct Binding {
    Action* action;
    std::string path; // "/user/hand/left/input/trigger/value"
};

// Suggested bindings by interaction profile, instance-wide.
std::map<std::string, std::vector<Binding>> g_bindings;

// Gesture thresholds, in metres.
constexpr float kPinchFullMeters = 0.015f;
constexpr float kPinchNoneMeters = 0.045f;
constexpr float kCurlOpenMeters = 0.17f;
constexpr float kCurlClosedMeters = 0.08f;
constexpr float kClickThreshold = 0.7f;

// The pointer hand: the one whose aim pose the game's Wii Remote pointer follows.
constexpr uint32_t kPointerHand = 1;

// Look-and-pinch selection (xr_visionos_spatial_event). The pointer is aimed
// along the pinch's gaze ray at once; select follows a beat later, so the game
// sees the pointer arrive over a button before the press, as with a real
// remote, and stays down long enough for a 60 Hz poll to catch a brief pinch.
constexpr int64_t kGazeSelectDelayNanos = 50'000'000;
constexpr int64_t kGazeSelectHoldNanos = 100'000'000;
std::mutex g_gazeMutex;
GazePinch g_gazePinch;

// The pinch as of `nowNanos`: whether select is down and where the pointer aims.
GazePinch GazePinchNow() noexcept {
    std::lock_guard lock(g_gazeMutex);
    return g_gazePinch;
}

bool GazeSelectDown(const GazePinch& pinch, int64_t nowNanos) noexcept {
    if (pinch.beganNanos == 0) {
        return false;
    }
    if (nowNanos - pinch.beganNanos < kGazeSelectDelayNanos) {
        return false;
    }
    if (pinch.active) {
        return true;
    }
    // Ended: held for at least the minimum press, counted from the later of the
    // press's own start and the end of the pinch.
    const int64_t pressStart = pinch.beganNanos + kGazeSelectDelayNanos;
    const int64_t releaseAt = std::max(pinch.endedNanos, pressStart) + kGazeSelectHoldNanos;
    return nowNanos < std::max(releaseAt, pressStart + kGazeSelectHoldNanos);
}

// An OpenXR-style pose looking along `direction` from `origin`: -Z forward, +Y
// as close to the world's up as the direction allows.
simd_float4x4 PoseAlong(simd_float3 origin, simd_float3 direction) noexcept {
    simd_float3 forward = direction;
    if (simd_length(forward) < 1.0e-6f) {
        forward = simd_make_float3(0.0f, 0.0f, -1.0f);
    }
    forward = simd_normalize(forward);
    simd_float3 worldUp = simd_make_float3(0.0f, 1.0f, 0.0f);
    if (std::fabs(simd_dot(forward, worldUp)) > 0.999f) {
        worldUp = simd_make_float3(0.0f, 0.0f, -1.0f);
    }
    const simd_float3 right = simd_normalize(simd_cross(forward, worldUp));
    const simd_float3 up = simd_normalize(simd_cross(right, forward));
    return simd_matrix(simd_make_float4(right, 0.0f), simd_make_float4(up, 0.0f), simd_make_float4(-forward, 0.0f),
                       simd_make_float4(origin, 1.0f));
}

ActionSet* GetActionSet(XrActionSet handle) noexcept {
    Instance* instance = CurrentInstance();
    if (instance == nullptr) {
        return nullptr;
    }
    for (void* candidate : instance->actionSets) {
        if (reinterpret_cast<XrActionSet>(candidate) == handle) {
            return static_cast<ActionSet*>(candidate);
        }
    }
    return nullptr;
}

Action* GetAction(XrAction handle) noexcept {
    Instance* instance = CurrentInstance();
    if (instance == nullptr) {
        return nullptr;
    }
    for (void* candidate : instance->actionSets) {
        for (auto& action : static_cast<ActionSet*>(candidate)->actions) {
            if (reinterpret_cast<XrAction>(action.get()) == handle) {
                return action.get();
            }
        }
    }
    return nullptr;
}

float Clamp01(float value) noexcept { return std::clamp(value, 0.0f, 1.0f); }

float Pinch(const HandJointSample& a, const HandJointSample& b) noexcept {
    if (!a.tracked || !b.tracked) {
        return 0.0f;
    }
    const float distance = simd_length(a.position - b.position);
    return Clamp01((kPinchNoneMeters - distance) / (kPinchNoneMeters - kPinchFullMeters));
}

struct Gestures {
    float pinchIndex = 0.0f;
    float pinchMiddle = 0.0f;
    float pinchRing = 0.0f;
    float pinchLittle = 0.0f;
    float curl = 0.0f;
    bool tracked = false;
};

Gestures GesturesOf(const HandSample& hand) noexcept {
    Gestures g{};
    g.tracked = hand.tracked;
    if (!hand.tracked) {
        return g;
    }
    g.pinchIndex = Pinch(hand.thumbTip, hand.indexTip);
    g.pinchMiddle = Pinch(hand.thumbTip, hand.middleTip);
    g.pinchRing = Pinch(hand.thumbTip, hand.ringTip);
    g.pinchLittle = Pinch(hand.thumbTip, hand.littleTip);
    if (hand.middleTip.tracked && hand.wrist.tracked) {
        const float reach = simd_length(hand.middleTip.position - hand.wrist.position);
        g.curl = Clamp01((kCurlOpenMeters - reach) / (kCurlOpenMeters - kCurlClosedMeters));
    }
    // A pinch is one gesture: the strongest one owns it, the others read as zero,
    // so pinching the middle finger does not also pull the trigger a little.
    const float best = std::max({g.pinchIndex, g.pinchMiddle, g.pinchRing, g.pinchLittle});
    if (best > 0.0f) {
        if (g.pinchIndex < best) g.pinchIndex = 0.0f;
        if (g.pinchMiddle < best) g.pinchMiddle = 0.0f;
        if (g.pinchRing < best) g.pinchRing = 0.0f;
        if (g.pinchLittle < best) g.pinchLittle = 0.0f;
    }
    // Pinching closes the fist a little; a real grab needs the fingers curled
    // without a pinch, and a pinch must not read as a grab.
    if (best > 0.3f) {
        g.curl = 0.0f;
    }
    return g;
}

// The pointer hand's gestures: the system's own pinch recognition (look and
// pinch, either hand) presses its trigger and select, on top of what the hand
// skeleton says. The pointer hand reads as present while a pinch is in flight
// even when ARKit has lost the hand.
Gestures GesturesOfHand(const Session& session, uint32_t hand) noexcept {
    Gestures g = GesturesOf(session.hands[hand]);
    if (hand == kPointerHand) {
        const GazePinch pinch = GazePinchNow();
        if (GazeSelectDown(pinch, NowNanos())) {
            g.pinchIndex = 1.0f;
            g.pinchMiddle = g.pinchRing = g.pinchLittle = 0.0f;
            g.curl = 0.0f;
            g.tracked = true;
        } else if (pinch.hasRay) {
            g.tracked = true;
        }
    }
    return g;
}

// A binding path's hand and component: "/user/hand/left/input/trigger/value" ->
// 0, "trigger/value". False for paths that are not a hand's.
bool ParseHandPath(const std::string& path, uint32_t& hand, std::string& component) noexcept {
    static const std::string left = "/user/hand/left/";
    static const std::string right = "/user/hand/right/";
    std::string rest;
    if (path.rfind(left, 0) == 0) {
        hand = 0;
        rest = path.substr(left.size());
    } else if (path.rfind(right, 0) == 0) {
        hand = 1;
        rest = path.substr(right.size());
    } else {
        return false;
    }
    if (rest.rfind("input/", 0) == 0) {
        component = rest.substr(6);
    } else if (rest.rfind("output/", 0) == 0) {
        component = rest;
    } else {
        return false;
    }
    return true;
}

// The value a component reads from a hand's gestures: a scalar for everything,
// booleans as 0/1; false for components no hand offers.
bool ComponentValue(const Gestures& g, const std::string& component, float& value, bool& boolean) noexcept {
    boolean = false;
    value = 0.0f;
    if (component == "trigger/value" || component == "trigger") {
        value = g.pinchIndex;
        boolean = value > kClickThreshold;
        return true;
    }
    if (component == "trigger/click" || component == "select/click" || component == "select") {
        boolean = g.pinchIndex > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "squeeze/value" || component == "squeeze") {
        value = g.curl;
        boolean = value > kClickThreshold;
        return true;
    }
    if (component == "squeeze/click") {
        boolean = g.curl > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "a/click" || component == "x/click") {
        boolean = g.pinchMiddle > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "b/click" || component == "y/click") {
        boolean = g.pinchRing > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "menu/click") {
        boolean = g.pinchLittle > kClickThreshold;
        value = boolean ? 1.0f : 0.0f;
        return true;
    }
    if (component == "thumbstick" || component == "thumbstick/click" || component == "thumbstick/x" ||
        component == "thumbstick/y") {
        return true; // present, at rest
    }
    return false;
}

// Every binding of `action` for the hands `subaction` names (both when null),
// as (hand, component) pairs.
void BindingsFor(Instance& instance, const Action& action, XrPath subaction,
                 std::vector<std::pair<uint32_t, std::string>>& out) {
    out.clear();
    uint32_t wantedHand = 2;
    if (subaction != XR_NULL_PATH) {
        const std::string* text = PathString(instance, subaction);
        if (text == nullptr) {
            return;
        }
        if (*text == "/user/hand/left") {
            wantedHand = 0;
        } else if (*text == "/user/hand/right") {
            wantedHand = 1;
        } else {
            return;
        }
    }
    for (const auto& [profile, bindings] : g_bindings) {
        for (const Binding& binding : bindings) {
            if (binding.action != &action) {
                continue;
            }
            uint32_t hand = 0;
            std::string component;
            if (!ParseHandPath(binding.path, hand, component)) {
                continue;
            }
            if (wantedHand != 2 && hand != wantedHand) {
                continue;
            }
            out.emplace_back(hand, component);
        }
    }
}

// A hand's frame from its joints, OpenXR style: -Z along the index metacarpal
// (where the finger points when extended), +Y out of the back of the hand, +X
// to the hand's right. Built from joint positions alone so ARKit's own hand
// anchor axes never matter.
bool HandFrame(const HandSample& hand, uint32_t handIndex, simd_float4x4& worldFromAim,
               simd_float4x4& worldFromGrip) noexcept {
    if (!hand.tracked || !hand.wrist.tracked || !hand.indexKnuckle.tracked || !hand.middleKnuckle.tracked) {
        return false;
    }
    simd_float3 forward = hand.indexKnuckle.position - hand.wrist.position;
    if (simd_length(forward) < 1.0e-4f) {
        return false;
    }
    forward = simd_normalize(forward);
    simd_float3 right = handIndex == 1 ? hand.middleKnuckle.position - hand.indexKnuckle.position
                                       : hand.indexKnuckle.position - hand.middleKnuckle.position;
    if (simd_length(right) < 1.0e-4f) {
        return false;
    }
    right = simd_normalize(right);
    simd_float3 up = simd_cross(right, forward);
    if (simd_length(up) < 1.0e-4f) {
        return false;
    }
    up = simd_normalize(up);
    right = simd_normalize(simd_cross(forward, up));
    const simd_float3 back = -forward;
    worldFromAim = simd_matrix(simd_make_float4(right, 0.0f), simd_make_float4(up, 0.0f), simd_make_float4(back, 0.0f),
                               simd_make_float4(hand.indexKnuckle.position, 1.0f));
    const simd_float3 palm = (hand.wrist.position + hand.middleKnuckle.position) * 0.5f;
    worldFromGrip = simd_matrix(simd_make_float4(right, 0.0f), simd_make_float4(up, 0.0f), simd_make_float4(back, 0.0f),
                                simd_make_float4(palm, 1.0f));
    return true;
}

uint32_t HandOfSubaction(Instance& instance, XrPath subaction) noexcept {
    const std::string* text = PathString(instance, subaction);
    if (text == nullptr) {
        return 2;
    }
    if (*text == "/user/hand/left") return 0;
    if (*text == "/user/hand/right") return 1;
    return 2;
}

// Which hand an action space stands for: the subaction path names it, else the
// first hand its bindings mention.
uint32_t HandOfSpace(Instance& instance, const Space& space, const Action& action) noexcept {
    if (space.subactionPath != XR_NULL_PATH) {
        return HandOfSubaction(instance, space.subactionPath);
    }
    std::vector<std::pair<uint32_t, std::string>> bindings;
    BindingsFor(instance, action, XR_NULL_PATH, bindings);
    return bindings.empty() ? 2 : bindings.front().first;
}

bool IsAimAction(Instance& instance, const Action& action) noexcept {
    std::vector<std::pair<uint32_t, std::string>> bindings;
    BindingsFor(instance, action, XR_NULL_PATH, bindings);
    for (const auto& [hand, component] : bindings) {
        if (component == "aim/pose") return true;
    }
    return false;
}

} // namespace

bool LocateActionSpaceInWorld(Session& session, const Space& space, int64_t timeNanos, simd_float4x4& worldFromSpace,
                              XrSpaceLocationFlags& flags, simd_float3* linearVelocity) noexcept {
    (void)timeNanos; // the hands are sampled at xrSyncActions; no prediction is attempted
    flags = 0;
    Instance& instance = *session.instance;
    const Action* action = GetAction(space.action);
    if (action == nullptr || action->type != XR_ACTION_TYPE_POSE_INPUT) {
        return false;
    }
    const uint32_t hand = HandOfSpace(instance, space, *action);
    if (hand > 1) {
        return false;
    }
    const bool aim = IsAimAction(instance, *action);
    if (aim && hand == kPointerHand) {
        // The pointer is not the hand's: pointing a hand at a screen a few metres
        // away is too coarse to land on a button. A pinch aims it along the
        // system's gaze ray, where the eyes were looking as the fingers met, and
        // it stays there until the next pinch. Before the first pinch there is no
        // pointer at all: the aim pose is simply not located.
        const GazePinch pinch = GazePinchNow();
        if (!pinch.hasRay) {
            return false;
        }
        worldFromSpace = simd_mul(PoseAlong(pinch.origin, pinch.direction), MatrixFromPose(space.poseInSpace));
        flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if (linearVelocity != nullptr) {
            *linearVelocity = simd_make_float3(0.0f, 0.0f, 0.0f);
        }
        return true;
    }
    const HandSample& sample = session.hands[hand];
    simd_float4x4 worldFromAim;
    simd_float4x4 worldFromGrip;
    if (!HandFrame(sample, hand, worldFromAim, worldFromGrip)) {
        return false;
    }
    const simd_float4x4& worldFromPose = aim ? worldFromAim : worldFromGrip;
    worldFromSpace = simd_mul(worldFromPose, MatrixFromPose(space.poseInSpace));
    flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if (linearVelocity != nullptr) {
        const HandSample& previous = session.previousHands[hand];
        simd_float4x4 previousAim;
        simd_float4x4 previousGrip;
        if (previous.tracked && previous.timeNanos != 0 && sample.timeNanos > previous.timeNanos &&
            HandFrame(previous, hand, previousAim, previousGrip)) {
            const simd_float4x4& before = aim ? previousAim : previousGrip;
            const float dt = static_cast<float>(sample.timeNanos - previous.timeNanos) * 1.0e-9f;
            if (dt > 1.0e-3f) {
                *linearVelocity = (worldFromPose.columns[3].xyz - before.columns[3].xyz) / dt;
            }
        }
    }
    return true;
}

void DestroyInstanceInput(Instance& instance) noexcept {
    for (void* set : instance.actionSets) {
        delete static_cast<ActionSet*>(set);
    }
    instance.actionSets.clear();
    g_bindings.clear();
    std::lock_guard lock(g_gazeMutex);
    g_gazePinch = {};
}

} // namespace mkw::vr::visionos

// App bridge (visionos_host.mm <- the SwiftUI CompositorLayer's onSpatialEvent).
void xr_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                               float origin_y, float origin_z, float direction_x, float direction_y,
                               float direction_z) {
    using namespace mkw::vr::visionos;
    (void)chirality; // either hand's pinch drives the one pointer
    const int64_t now = NowNanos();
    std::lock_guard lock(g_gazeMutex);
    GazePinch& pinch = g_gazePinch;
    if (phase == 0) {
        if (!pinch.active || pinch.eventId != event_id) {
            // A new pinch. Its ray is the gaze at the moment it began; later events of
            // the same pinch repeat it (or carry none) while the hand moves.
            pinch = {};
            pinch.eventId = event_id;
            pinch.active = true;
            pinch.beganNanos = now;
        }
        if (has_ray) {
            pinch.hasRay = true;
            pinch.origin = simd_make_float3(origin_x, origin_y, origin_z);
            pinch.direction = simd_make_float3(direction_x, direction_y, direction_z);
        }
        return;
    }
    if (pinch.eventId == event_id && pinch.active) {
        pinch.active = false;
        pinch.endedNanos = now;
        if (phase == 2) {
            // Cancelled by the system: no press should come of it.
            pinch.beganNanos = 0;
        }
    }
}

namespace mkw::vr::visionos {

} // namespace mkw::vr::visionos

using namespace mkw::vr::visionos;

namespace {

XrResult XRAPI_CALL CreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || actionSet == nullptr || createInfo->type != XR_TYPE_ACTION_SET_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (createInfo->actionSetName[0] == '\0') {
        return XR_ERROR_NAME_INVALID;
    }
    std::lock_guard lock(object->mutex);
    auto* set = new ActionSet();
    set->instance = object;
    set->name = createInfo->actionSetName;
    object->actionSets.push_back(set);
    *actionSet = reinterpret_cast<XrActionSet>(set);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroyActionSet(XrActionSet actionSet) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    ActionSet* set = GetActionSet(actionSet);
    if (set == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    for (auto& [profile, bindings] : g_bindings) {
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                      [&](const Binding& binding) { return binding.action->set == set; }),
                       bindings.end());
    }
    if (object->session != nullptr) {
        auto& spaces = object->session->spaces;
        spaces.erase(std::remove_if(spaces.begin(), spaces.end(),
                                    [&](const std::unique_ptr<Space>& space) {
                                        const Action* action = GetAction(space->action);
                                        return space->kind == Space::Kind::Action && action != nullptr &&
                                               action->set == set;
                                    }),
                     spaces.end());
        auto& attached = object->session->attachedActionSets;
        attached.erase(std::remove(attached.begin(), attached.end(), actionSet), attached.end());
    }
    object->actionSets.erase(std::remove(object->actionSets.begin(), object->actionSets.end(), set),
                             object->actionSets.end());
    delete set;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    ActionSet* set = GetActionSet(actionSet);
    if (set == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || action == nullptr || createInfo->type != XR_TYPE_ACTION_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (set->attached) {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    if (createInfo->actionName[0] == '\0') {
        return XR_ERROR_NAME_INVALID;
    }
    auto created = std::make_unique<Action>();
    created->set = set;
    created->name = createInfo->actionName;
    created->type = createInfo->actionType;
    for (uint32_t i = 0; i < createInfo->countSubactionPaths; ++i) {
        created->subactionPaths.push_back(createInfo->subactionPaths[i]);
    }
    *action = reinterpret_cast<XrAction>(created.get());
    set->actions.push_back(std::move(created));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL DestroyAction(XrAction action) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Action* target = GetAction(action);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    for (auto& [profile, bindings] : g_bindings) {
        bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
                                      [&](const Binding& binding) { return binding.action == target; }),
                       bindings.end());
    }
    auto& actions = target->set->actions;
    actions.erase(std::remove_if(actions.begin(), actions.end(),
                                 [&](const std::unique_ptr<Action>& candidate) { return candidate.get() == target; }),
                  actions.end());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL SuggestInteractionProfileBindings(XrInstance instance,
                                                      const XrInteractionProfileSuggestedBinding* suggestedBindings) {
    Instance* object = GetInstance(instance);
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (suggestedBindings == nullptr || suggestedBindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::lock_guard lock(object->mutex);
    const std::string* profile = PathString(*object, suggestedBindings->interactionProfile);
    if (profile == nullptr) {
        return XR_ERROR_PATH_INVALID;
    }
    std::vector<Binding> bindings;
    for (uint32_t i = 0; i < suggestedBindings->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& suggested = suggestedBindings->suggestedBindings[i];
        Action* action = GetAction(suggested.action);
        const std::string* path = PathString(*object, suggested.binding);
        if (action == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (path == nullptr) {
            return XR_ERROR_PATH_INVALID;
        }
        if (action->set->attached) {
            return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        }
        bindings.push_back({action, *path});
    }
    g_bindings[*profile] = std::move(bindings);
    Log("bindings suggested for %s (%u)", profile->c_str(), suggestedBindings->countSuggestedBindings);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL AttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (attachInfo == nullptr || attachInfo->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (target->actionSetsAttached) {
        return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    }
    for (uint32_t i = 0; i < attachInfo->countActionSets; ++i) {
        ActionSet* set = GetActionSet(attachInfo->actionSets[i]);
        if (set == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        set->attached = true;
        target->attachedActionSets.push_back(attachInfo->actionSets[i]);
    }
    target->actionSetsAttached = true;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                 XrInteractionProfileState* interactionProfile) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    if (GetSession(session) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (interactionProfile == nullptr || interactionProfile->type != XR_TYPE_INTERACTION_PROFILE_STATE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    (void)topLevelUserPath;
    // The hands answer the Touch bindings when suggested, else the simple ones.
    for (const char* profile : {"/interaction_profiles/oculus/touch_controller", "/interaction_profiles/khr/simple_controller"}) {
        if (g_bindings.count(profile) != 0) {
            interactionProfile->interactionProfile = InternPath(*object, profile);
            return XR_SUCCESS;
        }
    }
    interactionProfile->interactionProfile = XR_NULL_PATH;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL SyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (syncInfo == nullptr || syncInfo->type != XR_TYPE_ACTIONS_SYNC_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (!target->actionSetsAttached) {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    for (uint32_t i = 0; i < syncInfo->countActiveActionSets; ++i) {
        if (GetActionSet(syncInfo->activeActionSets[i].actionSet) == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
    }
    if (target->state != XR_SESSION_STATE_FOCUSED) {
        return XR_SESSION_NOT_FOCUSED;
    }
    std::array<HandSample, 2> hands{};
    Compositor::Get().Hands(hands);
    for (uint32_t hand = 0; hand < 2; ++hand) {
        if (hands[hand].tracked) {
            target->previousHands[hand] = target->hands[hand];
            target->hands[hand] = hands[hand];
        } else if (target->hands[hand].tracked && NowNanos() - target->hands[hand].timeNanos > 250'000'000) {
            // A hand ARKit has not seen for a quarter second is gone, not paused.
            target->hands[hand] = {};
            target->previousHands[hand] = {};
        }
    }
    target->lastSyncNanos = NowNanos();
    return XR_SUCCESS;
}

// Common lookup for the three state getters: the action, the hands it maps to
// for the requested subaction, and their gestures.
XrResult ResolveState(Session*& target, Action*& action, const XrActionStateGetInfo* getInfo,
                      XrActionType expected, std::vector<std::pair<uint32_t, std::string>>& bindings) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (getInfo == nullptr || getInfo->type != XR_TYPE_ACTION_STATE_GET_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    action = GetAction(getInfo->action);
    if (action == nullptr || target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (action->type != expected) {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    if (!action->set->attached) {
        return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    }
    BindingsFor(*object, *action, getInfo->subactionPath, bindings);
    return XR_SUCCESS;
}

uint32_t HistorySlot(Instance& instance, XrPath subaction) noexcept {
    const uint32_t hand = HandOfSubaction(instance, subaction);
    return hand > 1 ? 2 : hand;
}

XrResult XRAPI_CALL GetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                          XrActionStateBoolean* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_BOOLEAN_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_BOOLEAN) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    bool current = false;
    for (const auto& [hand, component] : bindings) {
        const Gestures g = GesturesOfHand(*target, hand);
        float value = 0.0f;
        bool boolean = false;
        if (!g.tracked || !ComponentValue(g, component, value, boolean)) {
            continue;
        }
        active = true;
        current = current || boolean;
    }
    const uint32_t slot = HistorySlot(*object, getInfo->subactionPath);
    state->isActive = active ? XR_TRUE : XR_FALSE;
    state->currentState = current ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = (active && current != action->lastBoolean[slot]) ? XR_TRUE : XR_FALSE;
    if (state->changedSinceLastSync) {
        action->lastChange[slot] = target->lastSyncNanos;
    }
    action->lastBoolean[slot] = active && current;
    state->lastChangeTime = action->lastChange[slot];
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateFloat* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_FLOAT_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_FLOAT) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    float current = 0.0f;
    for (const auto& [hand, component] : bindings) {
        const Gestures g = GesturesOfHand(*target, hand);
        float value = 0.0f;
        bool boolean = false;
        if (!g.tracked || !ComponentValue(g, component, value, boolean)) {
            continue;
        }
        active = true;
        current = std::max(current, value);
    }
    state->isActive = active ? XR_TRUE : XR_FALSE;
    state->currentState = current;
    state->changedSinceLastSync = XR_TRUE;
    state->lastChangeTime = target->lastSyncNanos;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                           XrActionStateVector2f* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_VECTOR2F_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_VECTOR2F) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    for (const auto& [hand, component] : bindings) {
        if (target->hands[hand].tracked && component.rfind("thumbstick", 0) == 0) {
            active = true;
        }
    }
    // The hands have no stick; it reads as centred.
    state->isActive = active ? XR_TRUE : XR_FALSE;
    state->currentState = {0.0f, 0.0f};
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = target->lastSyncNanos;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStatePose* state) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    Action* action = nullptr;
    std::vector<std::pair<uint32_t, std::string>> bindings;
    if (const XrResult result = ResolveState(target, action, getInfo, XR_ACTION_TYPE_POSE_INPUT, bindings);
        XR_FAILED(result)) {
        return result;
    }
    if (state == nullptr || state->type != XR_TYPE_ACTION_STATE_POSE) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    bool active = false;
    for (const auto& [hand, component] : bindings) {
        active = active || target->hands[hand].tracked;
    }
    state->isActive = active ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    Session* target = GetSession(session);
    if (target == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (createInfo == nullptr || space == nullptr || createInfo->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    Action* action = GetAction(createInfo->action);
    if (action == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (action->type != XR_ACTION_TYPE_POSE_INPUT) {
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    auto created = std::make_unique<Space>();
    created->session = target;
    created->kind = Space::Kind::Action;
    created->action = createInfo->action;
    created->subactionPath = createInfo->subactionPath;
    created->poseInSpace = createInfo->poseInActionSpace;
    *space = reinterpret_cast<XrSpace>(created.get());
    target->spaces.push_back(std::move(created));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL ApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                        const XrHapticBaseHeader* hapticFeedback) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    if (GetSession(session) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr || hapticFeedback == nullptr ||
        hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (GetAction(hapticActionInfo->action) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    return XR_SUCCESS; // hands have no motor
}

XrResult XRAPI_CALL StopHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo) {
    Instance* object = CurrentInstance();
    if (object == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::lock_guard lock(object->mutex);
    if (GetSession(session) == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    if (hapticActionInfo == nullptr || hapticActionInfo->type != XR_TYPE_HAPTIC_ACTION_INFO) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    return XR_SUCCESS;
}

struct Entry {
    const char* name;
    PFN_xrVoidFunction function;
};

const std::array<Entry, 14> kEntries{{
    {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(&CreateActionSet)},
    {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(&DestroyActionSet)},
    {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(&CreateAction)},
    {"xrDestroyAction", reinterpret_cast<PFN_xrVoidFunction>(&DestroyAction)},
    {"xrSuggestInteractionProfileBindings", reinterpret_cast<PFN_xrVoidFunction>(&SuggestInteractionProfileBindings)},
    {"xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction>(&AttachSessionActionSets)},
    {"xrGetCurrentInteractionProfile", reinterpret_cast<PFN_xrVoidFunction>(&GetCurrentInteractionProfile)},
    {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(&SyncActions)},
    {"xrGetActionStateBoolean", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStateBoolean)},
    {"xrGetActionStateFloat", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStateFloat)},
    {"xrGetActionStateVector2f", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStateVector2f)},
    {"xrGetActionStatePose", reinterpret_cast<PFN_xrVoidFunction>(&GetActionStatePose)},
    {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(&CreateActionSpace)},
    {"xrApplyHapticFeedback", reinterpret_cast<PFN_xrVoidFunction>(&ApplyHapticFeedback)},
}};

} // namespace

namespace mkw::vr::visionos {

PFN_xrVoidFunction LookupInputFunction(const char* name) noexcept {
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.function;
        }
    }
    if (std::strcmp(name, "xrStopHapticFeedback") == 0) {
        return reinterpret_cast<PFN_xrVoidFunction>(&StopHapticFeedback);
    }
    return nullptr;
}

} // namespace mkw::vr::visionos

// The action entry points are also exported under their OpenXR names, because
// openxr_input.cpp calls them directly rather than through xrGetInstanceProcAddr.
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo,
                                                 XrActionSet* actionSet) {
    return CreateActionSet(instance, createInfo, actionSet);
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet) { return DestroyActionSet(actionSet); }
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo,
                                              XrAction* action) {
    return CreateAction(actionSet, createInfo, action);
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction action) { return DestroyAction(action); }
XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) {
    return SuggestInteractionProfileBindings(instance, suggestedBindings);
}
XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session,
                                                         const XrSessionActionSetsAttachInfo* attachInfo) {
    return AttachSessionActionSets(session, attachInfo);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                                              XrInteractionProfileState* interactionProfile) {
    return GetCurrentInteractionProfile(session, topLevelUserPath, interactionProfile);
}
XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo) {
    return SyncActions(session, syncInfo);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo,
                                                       XrActionStateBoolean* state) {
    return GetActionStateBoolean(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo,
                                                     XrActionStateFloat* state) {
    return GetActionStateFloat(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo,
                                                        XrActionStateVector2f* state) {
    return GetActionStateVector2f(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo,
                                                    XrActionStatePose* state) {
    return GetActionStatePose(session, getInfo, state);
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo,
                                                   XrSpace* space) {
    return CreateActionSpace(session, createInfo, space);
}
XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo,
                                                     const XrHapticBaseHeader* hapticFeedback) {
    return ApplyHapticFeedback(session, hapticActionInfo, hapticFeedback);
}
XRAPI_ATTR XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* hapticActionInfo) {
    return StopHapticFeedback(session, hapticActionInfo);
}
