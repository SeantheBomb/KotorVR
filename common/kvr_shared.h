// Shared-memory contract between the in-game DLL (x86) and kvr_host (x64).
// Fixed-size types only; layout must match across bitness.
#pragma once
#include <stdint.h>

#define KVR_SHM_NAME L"Local\\KotorVR_Shared"
#define KVR_TEX_NAME_EYE0 L"Local\\KotorVR_Eye0"
#define KVR_TEX_NAME_EYE1 L"Local\\KotorVR_Eye1"
#define KVR_TEX_NAME_PANEL L"Local\\KotorVR_Panel"
#define KVR_MAGIC 0x4B565231u  // 'KVR1'
#define KVR_VERSION 8

#pragma pack(push, 8)

struct KvrPose {
    float pos[3];   // meters, OpenXR LOCAL space relative to recenter origin (+X right, +Y up, -Z forward)
    float quat[4];  // x, y, z, w
};

struct KvrFov {
    float angleLeft, angleRight, angleUp, angleDown;  // radians, OpenXR convention
};

// Host -> game. Written under a seqlock: seq odd while writing.
struct KvrHostState {
    volatile uint32_t seq;
    uint32_t running;          // host session is running and wants frames
    KvrPose eye[2];            // eye poses relative to recenter origin
    KvrFov fov[2];
    uint32_t recommendedW, recommendedH;
};

// Game -> host, one slot per shared texture. Fields valid once frameId > 0.
// The game writes pose/fov then bumps frameId while holding the texture's keyed mutex.
struct KvrEyeSlot {
    volatile uint32_t frameId;
    KvrPose pose;    // pose the eye was rendered with (for the projection layer)
    KvrFov fov;
    uint32_t width, height;  // valid region of the texture
    uint32_t flags;          // KVR_SLOT_*
};

enum : uint32_t {
    KVR_SLOT_GUI_ONLY = 1u << 0,  // panel holds only the GUI drawn over black (world frames): key out black
};

// Controller buttons (KvrInput::buttons)
enum : uint32_t {
    KVR_BTN_A = 1u << 0,
    KVR_BTN_B = 1u << 1,
    KVR_BTN_X = 1u << 2,
    KVR_BTN_Y = 1u << 3,
    KVR_BTN_MENU = 1u << 4,
    KVR_BTN_LSTICK = 1u << 5,
    KVR_BTN_RSTICK = 1u << 6,
};

enum : uint32_t { KVR_POINTER_NONE = 0, KVR_POINTER_PANEL = 1, KVR_POINTER_WORLD = 2 };

// Host -> game controller state. Written under a seqlock like KvrHostState.
struct KvrInput {
    volatile uint32_t seq;
    uint32_t active;              // controllers are being tracked
    float lx, ly, rx, ry;         // thumbsticks, -1..1 (+y forward / +x right)
    float trigL, trigR, gripL, gripR;
    uint32_t buttons;             // KVR_BTN_*
    uint32_t pointerMode;         // KVR_POINTER_*
    float panelU, panelV;         // 0..1, top-left origin, valid for KVR_POINTER_PANEL
    KvrPose aim;                  // right aim pose relative to the recenter origin, for KVR_POINTER_WORLD
    KvrPose grip[2];              // left/right grip poses relative to the recenter origin (-Z through the fist)
    uint32_t gripValid;           // bit 0 left, bit 1 right
    double time;                  // host clock (s) when these poses were sampled, for velocities
};

// Game -> host haptic pulse request for one hand. The game fills the fields, then bumps serial; the host plays one
// pulse per serial change (a newer request replaces one still playing).
struct KvrHaptic {
    volatile uint32_t serial;
    float amplitude;    // 0..1
    float duration;     // seconds
    float frequency;    // Hz, 0 = runtime default
};

// Host -> game tracked hand skeletons (XR_EXT_hand_tracking, OpenXR joint order: palm, wrist, thumb metacarpal..tip,
// index metacarpal..tip, middle, ring, little; 26 per hand), relative to the recenter origin like the grip poses.
// Joint frames follow OpenXR: -Z toward the fingertip along the bone, +Y out of the back of the hand.
// Driven by real hands or, while holding Touch controllers, by the runtime's controller-driven hand poses.
#define KVR_HAND_JOINTS 26
struct KvrHands {
    volatile uint32_t seq;  // seqlock like KvrInput
    uint32_t valid;         // bit 0 left, bit 1 right: joints this frame are tracked
    KvrPose joint[2][KVR_HAND_JOINTS];
};

struct KvrShared {
    uint32_t magic;
    uint32_t version;
    KvrHostState host;
    // game -> host
    volatile uint32_t gameAlive;    // incremented every game frame
    uint32_t texWidth, texHeight;   // allocated size of the shared textures
    uint32_t texGeneration;         // texture names are <base>_<generation>; bumped on resize
    KvrEyeSlot eyes[2];
    KvrEyeSlot panel;               // full composited frame incl. GUI
    volatile uint32_t showPanel;    // toggled in-game (F9)
    volatile uint32_t recenterReq;  // set in-game (F8), cleared by host
    volatile uint32_t lastWorldFrame;  // gameAlive value of the last frame that rendered the 3D world
    KvrInput input;                    // host -> game controller state
    volatile uint32_t cinematic;       // game is on a dialogue/cutscene camera (world FOV != gameplay FOV)
    KvrHaptic haptic[2];               // game -> host, left / right hand
    KvrHands hands;                    // host -> game
};

#pragma pack(pop)
