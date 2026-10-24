// Shared-memory contract between the in-game DLL (x86) and kvr_host (x64).
// Fixed-size types only; layout must match across bitness.
#pragma once
#include <stdint.h>

#define KVR_SHM_NAME L"Local\\KotorVR_Shared"
#define KVR_TEX_NAME_EYE0 L"Local\\KotorVR_Eye0"
#define KVR_TEX_NAME_EYE1 L"Local\\KotorVR_Eye1"
#define KVR_TEX_NAME_PANEL L"Local\\KotorVR_Panel"
#define KVR_MAGIC 0x4B565231u  // 'KVR1'
#define KVR_VERSION 2

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
};

#pragma pack(pop)
