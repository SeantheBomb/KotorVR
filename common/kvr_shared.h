// Shared-memory contract between the in-game DLL (x86) and kvr_host (x64).
// Fixed-size types only; layout must match across bitness.
#pragma once
#include <stdint.h>

// the layout version is part of the name: a stale process holding an older (smaller) mapping can never block it
#define KVR_SHM_NAME L"Local\\KotorVR_Shared_v12"
#define KVR_TEX_NAME_EYE0 L"Local\\KotorVR_Eye0"
#define KVR_TEX_NAME_EYE1 L"Local\\KotorVR_Eye1"
#define KVR_TEX_NAME_PANEL L"Local\\KotorVR_Panel"
#define KVR_MAGIC 0x4B565231u  // 'KVR1'
#define KVR_VERSION 12

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
    KVR_SLOT_MOVIE = 1u << 1,     // panel holds a cutscene movie frame (width x height): cinema screen
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

// HUD_LEFT: in the world only the left hand points at the HUD (left trigger clicks it); the right hand and trigger
// stay with the weapon / world (aim is still the right hand).
enum : uint32_t { KVR_POINTER_NONE = 0, KVR_POINTER_PANEL = 1, KVR_POINTER_WORLD = 2, KVR_POINTER_HUD_LEFT = 3 };

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
    uint32_t poke;                // 1 while the right fingertip is pushed into a wrist-panel control (a click)
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

// Game -> host text panels (VR instructions, notices, dialogue options): the host draws the text with DirectWrite
// onto its own quad layer. The game fills the fields, then bumps serial.
enum : uint32_t {
    KVR_ANCHOR_HIDDEN = 0,
    KVR_ANCHOR_HEAD = 1,   // pose relative to the head (follows your view)
    KVR_ANCHOR_WORLD = 2,  // pose relative to the recenter origin (stays put)
    KVR_ANCHOR_LWRIST = 3, // pose relative to the left controller grip
};
#define KVR_TEXT_PANELS 4
struct KvrTextPanel {
    volatile uint32_t serial;
    uint32_t anchor;       // KVR_ANCHOR_*
    KvrPose pose;          // quad center + orientation in the anchor's frame (quad faces +Z)
    float width;           // meters (height follows the text's aspect: 2:1)
    int32_t highlight;     // line index drawn highlighted (-1 none)
    uint32_t pickable;     // the right hand can point at its lines (dialogue options): the host shows the laser,
                           // highlights the line under it and reports it in `hover`
    volatile int32_t hover;  // host -> game: line index under the right-hand ray (-1 none)
    char text[504];        // UTF-8; lines separated by \n; a line starting with '#' is a title; "@icon|text" draws
                           // the game's GUI icon <game>\kvr\icons\<icon>.png before the text
};

// Game -> host: a colored flash around the edge of the view (getting hit, blocks), fading out over `duration`.
// The game fills the fields, then bumps serial.
struct KvrFlash {
    volatile uint32_t serial;
    float rgb[3];
    float strength;  // 0..1 peak opacity at the rim
    float duration;  // seconds
};

// Game -> host: screen rectangles (x, y, w, h in panel-texture pixels, top-left origin; w = 0: none) of the flat
// HUD's pieces, for the wrist panel (docs/DIEGETIC_HUD.md). Bumps serial on change.
enum { KVR_HUD_MENU, KVR_HUD_MINIMAP, KVR_HUD_PARTY, KVR_HUD_SLOTS, KVR_HUD_TOGGLES, KVR_HUD_REGIONS };
struct KvrHudLayout {
    volatile uint32_t serial;
    int32_t rect[KVR_HUD_REGIONS][4];
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
    KvrTextPanel text[KVR_TEXT_PANELS];  // game -> host
    KvrFlash flash;                      // game -> host
    char gameDir[260];                   // game -> host: the game folder (ANSI), for kvr\icons
    KvrHudLayout hud;                    // game -> host
};

#pragma pack(pop)
