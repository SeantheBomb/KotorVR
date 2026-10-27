// kvr_host: x64 OpenXR compositor bridge for the KotOR VR layer.
// Owns the OpenXR session (D3D11). Publishes head/eye poses to shared memory, and each XR frame
// copies the newest eye images the game produced into the swapchains, submitted with the pose
// each eye was actually rendered with. Shows the full game frame on a quad panel for menus.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <d3dcompiler.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <map>
#include <string>

#include "../common/kvr_shared.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "openxr_loader.lib")

#define CHECK_XR(x)                                                         \
    do {                                                                    \
        XrResult r_ = (x);                                                  \
        if (XR_FAILED(r_)) { printf("%s failed: %d\n", #x, r_); return false; } \
    } while (0)

// ---------------------------------------------------------------- math
struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };
static Quat qmul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static Quat qconj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
static Vec3 qrot(Quat q, Vec3 v) {
    Quat p{v.x, v.y, v.z, 0};
    Quat r = qmul(qmul(q, p), qconj(q));
    return {r.x, r.y, r.z};
}
static XrPosef PoseMul(const XrPosef& a, const XrPosef& b) {  // a * b
    Quat qa{a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w};
    Quat qb{b.orientation.x, b.orientation.y, b.orientation.z, b.orientation.w};
    Vec3 t = qrot(qa, {b.position.x, b.position.y, b.position.z});
    Quat q = qmul(qa, qb);
    return {{q.x, q.y, q.z, q.w}, {a.position.x + t.x, a.position.y + t.y, a.position.z + t.z}};
}
static XrPosef PoseInv(const XrPosef& a) {
    Quat qi = qconj({a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w});
    Vec3 t = qrot(qi, {-a.position.x, -a.position.y, -a.position.z});
    return {{qi.x, qi.y, qi.z, qi.w}, {t.x, t.y, t.z}};
}
// Heading of an orientation around +Y (0 = facing -Z), radians, counter-clockwise seen from above.
static float YawOf(const XrQuaternionf& q) {
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));  // forward = q * (0,0,-1)
    return atan2f(-fx, -fz);
}

static XrPosef YawOnly(const XrPosef& p) {
    // keep heading, drop pitch/roll so the recenter origin stays level
    const XrQuaternionf& q = p.orientation;
    float fx = -2 * (q.x * q.z + q.w * q.y), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));  // forward = q * (0,0,-1)
    float yaw = atan2f(-fx, -fz);
    XrPosef r{};
    r.orientation = {0, sinf(yaw / 2), 0, cosf(yaw / 2)};
    r.position = p.position;
    return r;
}

// ---------------------------------------------------------------- shared state
static KvrShared* g_shm;

static bool OpenShared() {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(KvrShared), KVR_SHM_NAME);
    if (!h) return false;
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    g_shm = (KvrShared*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(KvrShared));
    if (!g_shm) return false;
    if (!existed || g_shm->magic != KVR_MAGIC || g_shm->version != KVR_VERSION) {
        ZeroMemory(g_shm, sizeof(KvrShared));
        g_shm->magic = KVR_MAGIC;
        g_shm->version = KVR_VERSION;
    }
    return true;
}

// ---------------------------------------------------------------- D3D
static ID3D11Device1* g_dev;
static ID3D11DeviceContext* g_ctx;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11SamplerState* g_samp;
static ID3D11Buffer* g_cb;

// Keyed mutex protocol (see game/vr.cpp): game writes under KEY_GAME and hands over with KEY_HOST;
// the host copies into its own texture under KEY_HOST and hands back with KEY_GAME.
enum { KEY_GAME = 0, KEY_HOST = 1 };

struct SharedTex {
    const wchar_t* name;
    ID3D11Texture2D* tex = nullptr;    // shared with the game, only touched while holding KEY_HOST
    IDXGIKeyedMutex* mutex = nullptr;
    ID3D11Texture2D* local = nullptr;  // host-owned copy that the compositor blits from
    ID3D11ShaderResourceView* srv = nullptr;
    KvrEyeSlot meta{};                 // slot metadata matching `local`
    bool have = false;
};
static SharedTex g_shared[3] = {{KVR_TEX_NAME_EYE0}, {KVR_TEX_NAME_EYE1}, {KVR_TEX_NAME_PANEL}};
static uint32_t g_sharedW, g_sharedH, g_sharedGen;

static const char* kShader = R"(
cbuffer P : register(b0) { float2 uvScale; float decodeSrgb; float guiKey; float2 uvOffset; float2 pad; };
Texture2D t : register(t0);
SamplerState s : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    float2 c = float2((id << 1) & 2, id & 2);
    pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);
    uv = float2(c.x, 1 - c.y) * uvScale + uvOffset;   // GL images are bottom-up
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = t.Sample(s, uv).rgb;
    // GUI-only frames are drawn over black: key black out so the HUD floats over the stereo world
    float a = guiKey > 1.5 ? t.Sample(s, uv).a : guiKey > 0.5 ? saturate(max(c.r, max(c.g, c.b)) * 4.0) : 1.0;
    if (decodeSrgb > 0.5) c = pow(abs(c), 2.2);
    return float4(c, a);
}
)";

static bool InitD3D(LUID luid) {
    IDXGIFactory1* fac;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (!memcmp(&d.AdapterLuid, &luid, sizeof luid)) {
            wprintf(L"adapter: %s\n", d.Description);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    fac->Release();
    if (!adapter) { printf("no adapter matching OpenXR LUID\n"); return false; }
    ID3D11Device* dev;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    // BGRA support: Direct2D draws the text panels into textures of this device
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &dev, nullptr, &g_ctx);
    adapter->Release();
    if (FAILED(hr)) return false;
    dev->QueryInterface(IID_PPV_ARGS(&g_dev));
    dev->Release();

    ID3DBlob *vsb, *psb, *err;
    if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        printf("shader: %s\n", (char*)err->GetBufferPointer());
        return false;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    g_dev->CreateSamplerState(&sd, &g_samp);
    D3D11_BUFFER_DESC bd{32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    return true;
}

static void ReleaseShared() {
    for (auto& s : g_shared) {
        if (s.srv) s.srv->Release();
        if (s.mutex) s.mutex->Release();
        if (s.tex) s.tex->Release();
        if (s.local) s.local->Release();
        s.srv = nullptr; s.mutex = nullptr; s.tex = nullptr; s.local = nullptr;
        s.have = false;
    }
    g_sharedW = g_sharedH = 0;
}

static bool EnsureShared() {
    if (!g_shm->texWidth) return false;
    if (g_sharedGen == g_shm->texGeneration && g_shared[0].tex) return true;
    ReleaseShared();
    for (auto& s : g_shared) {
        wchar_t name[96];
        swprintf_s(name, L"%s_%u", s.name, g_shm->texGeneration);
        if (FAILED(g_dev->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, IID_PPV_ARGS(&s.tex)))) {
            ReleaseShared();
            return false;
        }
        s.tex->QueryInterface(IID_PPV_ARGS(&s.mutex));
        D3D11_TEXTURE2D_DESC d;
        s.tex->GetDesc(&d);
        d.MiscFlags = 0;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&d, nullptr, &s.local);
        g_dev->CreateShaderResourceView(s.local, nullptr, &s.srv);
    }
    g_sharedW = g_shm->texWidth;
    g_sharedH = g_shm->texHeight;
    g_sharedGen = g_shm->texGeneration;
    printf("opened shared textures %ux%u (gen %u)\n", g_sharedW, g_sharedH, g_sharedGen);
    return true;
}

// ---------------------------------------------------------------- OpenXR
struct Swapchain {
    XrSwapchain handle;
    int32_t w, h;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
};

static XrInstance g_inst;
static XrSystemId g_sys;
static XrSession g_session;
static XrSpace g_local, g_view;
static Swapchain g_eyeSc[2], g_panelSc;
static XrViewConfigurationView g_viewCfg[2];
static DXGI_FORMAT g_scFormat;
static bool g_scSrgb;
static bool g_sessionRunning, g_quit, g_lost;  // g_quit: user asked to exit; g_lost: runtime session/instance lost
static XrPosef g_origin{{0, 0, 0, 1}, {0, 0, 0}};
static bool g_haveOrigin;

static bool CreateSwapchain(Swapchain& sc, int32_t w, int32_t h) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = g_scFormat;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    CHECK_XR(xrCreateSwapchain(g_session, &ci, &sc.handle));
    sc.w = w;
    sc.h = h;
    uint32_t n;
    xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr);
    sc.images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)sc.images.data());
    for (auto& img : sc.images) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = g_scFormat;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv;
        g_dev->CreateRenderTargetView(img.texture, &rd, &rtv);
        sc.rtvs.push_back(rtv);
    }
    return true;
}

static bool InitControls();
static bool InitText();
struct Swapchain;
static void FillSwapchain(Swapchain& sc, const float rgba[4]);

// ---------------------------------------------------------------- hand tracking (XR_EXT_hand_tracking)
static bool g_extHands, g_extHandSource;
static XrHandTrackerEXT g_handTracker[2];
static PFN_xrCreateHandTrackerEXT p_xrCreateHandTrackerEXT;
static PFN_xrDestroyHandTrackerEXT p_xrDestroyHandTrackerEXT;
static PFN_xrLocateHandJointsEXT p_xrLocateHandJointsEXT;

static bool HasExtension(const char* name) {
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> p(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, p.data());
    for (auto& e : p)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

static void CreateHandTrackers() {
    if (!g_extHands) return;
    xrGetInstanceProcAddr(g_inst, "xrCreateHandTrackerEXT", (PFN_xrVoidFunction*)&p_xrCreateHandTrackerEXT);
    xrGetInstanceProcAddr(g_inst, "xrDestroyHandTrackerEXT", (PFN_xrVoidFunction*)&p_xrDestroyHandTrackerEXT);
    xrGetInstanceProcAddr(g_inst, "xrLocateHandJointsEXT", (PFN_xrVoidFunction*)&p_xrLocateHandJointsEXT);
    if (!p_xrCreateHandTrackerEXT || !p_xrLocateHandJointsEXT) return;
    for (int h = 0; h < 2; h++) {
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = h ? XR_HAND_RIGHT_EXT : XR_HAND_LEFT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        // hands from cameras when free, and the runtime's controller-driven hand pose while holding Touch controllers
        XrHandTrackingDataSourceEXT sources[2] = {XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT,
                                                 XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
        XrHandTrackingDataSourceInfoEXT si{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT};
        si.requestedDataSourceCount = 2;
        si.requestedDataSources = sources;
        if (g_extHandSource) ci.next = &si;
        XrResult r = p_xrCreateHandTrackerEXT(g_session, &ci, &g_handTracker[h]);
        printf("hand tracker %s: %d\n", h ? "right" : "left", r);
        if (XR_FAILED(r)) g_handTracker[h] = XR_NULL_HANDLE;
    }
}

static void DestroyHandTrackers() {
    for (auto& t : g_handTracker) {
        if (t != XR_NULL_HANDLE && p_xrDestroyHandTrackerEXT) p_xrDestroyHandTrackerEXT(t);
        t = XR_NULL_HANDLE;
    }
}

// both hands' joints, relative to the recenter origin, into shared memory
static void PublishHands(XrTime t) {
    KvrHands& hs = g_shm->hands;
    hs.seq++;
    MemoryBarrier();
    hs.valid = 0;
    XrPosef invOrigin = PoseInv(g_origin);
    for (int h = 0; h < 2; h++) {
        if (g_handTracker[h] == XR_NULL_HANDLE) continue;
        XrHandJointLocationEXT loc[XR_HAND_JOINT_COUNT_EXT];
        XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locs.jointLocations = loc;
        XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        li.baseSpace = g_local;
        li.time = t;
        if (XR_FAILED(p_xrLocateHandJointsEXT(g_handTracker[h], &li, &locs)) || !locs.isActive) continue;
        bool ok = true;
        for (int j = 0; j < KVR_HAND_JOINTS && j < XR_HAND_JOINT_COUNT_EXT; j++) {
            if (!(loc[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
                !(loc[j].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                ok = false;
                break;
            }
            XrPosef rel = PoseMul(invOrigin, loc[j].pose);
            memcpy(hs.joint[h][j].pos, &rel.position, 12);
            memcpy(hs.joint[h][j].quat, &rel.orientation, 16);
        }
        if (ok) hs.valid |= 1u << h;
    }
    MemoryBarrier();
    hs.seq++;
}

static bool InitXR() {
    std::vector<const char*> exts = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    g_extHands = HasExtension(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    g_extHandSource = g_extHands && HasExtension(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
    if (g_extHands) exts.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (g_extHandSource) exts.push_back(XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME);
    printf("hand tracking: %s, controller-driven hands: %s\n", g_extHands ? "yes" : "no", g_extHandSource ? "yes" : "no");
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "KotorVR");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.enabledExtensionNames = exts.data();
    CHECK_XR(xrCreateInstance(&ici, &g_inst));
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    for (;;) {
        XrResult r = xrGetSystem(g_inst, &sgi, &g_sys);
        if (XR_SUCCEEDED(r)) break;
        if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) { printf("xrGetSystem failed: %d\n", r); return false; }
        static bool told;
        if (!told) { printf("waiting for headset (connect Air Link)...\n"); told = true; }
        Sleep(2000);
        // the runtime only notices a newly connected headset on a fresh instance
        xrDestroyInstance(g_inst);
        CHECK_XR(xrCreateInstance(&ici, &g_inst));
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq;
    CHECK_XR(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    CHECK_XR(getReq(g_inst, g_sys, &req));
    if (!InitD3D(req.adapterLuid)) return false;

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_sys;
    CHECK_XR(xrCreateSession(g_inst, &sci, &g_session));
    CreateHandTrackers();

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    CHECK_XR(xrCreateReferenceSpace(g_session, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    CHECK_XR(xrCreateReferenceSpace(g_session, &rs, &g_view));

    uint32_t n;
    xrEnumerateSwapchainFormats(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(g_session, n, &n, fmts.data());
    g_scFormat = DXGI_FORMAT_UNKNOWN;
    for (int64_t f : fmts)
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { g_scFormat = (DXGI_FORMAT)f; g_scSrgb = true; break; }
    if (g_scFormat == DXGI_FORMAT_UNKNOWN) { g_scFormat = (DXGI_FORMAT)fmts[0]; g_scSrgb = false; }
    printf("swapchain format %d (srgb=%d)\n", g_scFormat, g_scSrgb);

    uint32_t vc = 2;
    g_viewCfg[0] = g_viewCfg[1] = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
    CHECK_XR(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, g_viewCfg));
    for (int e = 0; e < 2; e++)
        if (!CreateSwapchain(g_eyeSc[e], g_viewCfg[e].recommendedImageRectWidth, g_viewCfg[e].recommendedImageRectHeight)) return false;
    if (!CreateSwapchain(g_panelSc, 1600, 1200)) return false;
    printf("eye swapchains %ux%u\n", g_viewCfg[0].recommendedImageRectWidth, g_viewCfg[0].recommendedImageRectHeight);
    g_shm->host.recommendedW = g_viewCfg[0].recommendedImageRectWidth;
    g_shm->host.recommendedH = g_viewCfg[0].recommendedImageRectHeight;
    return InitControls();
}

// ---------------------------------------------------------------- controllers
static XrActionSet g_actSet;
static XrAction a_move, a_turn, a_trig, a_grip, a_aim, a_gripPose, a_btnA, a_btnB, a_btnX, a_btnY, a_menu, a_stickClick, a_haptic;
static XrPath g_handPath[2];
static XrSpace g_aimSpace[2], g_gripSpace[2];
static Swapchain g_beamSc, g_dotSc;
static Swapchain g_flashSc;  // the rim flash (KvrFlash)
static Swapchain g_wristSc;  // the wrist panel (docs/DIEGETIC_HUD.md)
static uint32_t g_prevButtons;

static XrPath MakePath(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_inst, s, &p);
    return p;
}

static bool MakeAction(XrAction& a, const char* name, XrActionType type, bool perHand) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, name);
    ci.actionType = type;
    if (perHand) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = g_handPath;
    }
    XrResult r = xrCreateAction(g_actSet, &ci, &a);
    if (XR_FAILED(r)) printf("xrCreateAction %s failed: %d\n", name, r);
    return XR_SUCCEEDED(r);
}

static bool InitControls() {
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "kotor");
    strcpy_s(si.localizedActionSetName, "KotOR");
    CHECK_XR(xrCreateActionSet(g_inst, &si, &g_actSet));
    g_handPath[0] = MakePath("/user/hand/left");
    g_handPath[1] = MakePath("/user/hand/right");
    bool ok = MakeAction(a_move, "move", XR_ACTION_TYPE_VECTOR2F_INPUT, false) &&
              MakeAction(a_turn, "turn", XR_ACTION_TYPE_VECTOR2F_INPUT, false) &&
              MakeAction(a_trig, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              MakeAction(a_grip, "grip", XR_ACTION_TYPE_FLOAT_INPUT, true) &&
              MakeAction(a_aim, "aim", XR_ACTION_TYPE_POSE_INPUT, true) &&
              MakeAction(a_gripPose, "grip_pose", XR_ACTION_TYPE_POSE_INPUT, true) &&
              MakeAction(a_btnA, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_btnB, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_btnX, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_btnY, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_stickClick, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true) &&
              MakeAction(a_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    if (!ok) return false;

    XrActionSuggestedBinding b[] = {
        {a_move, MakePath("/user/hand/left/input/thumbstick")},
        {a_turn, MakePath("/user/hand/right/input/thumbstick")},
        {a_trig, MakePath("/user/hand/left/input/trigger/value")},
        {a_trig, MakePath("/user/hand/right/input/trigger/value")},
        {a_grip, MakePath("/user/hand/left/input/squeeze/value")},
        {a_grip, MakePath("/user/hand/right/input/squeeze/value")},
        {a_aim, MakePath("/user/hand/left/input/aim/pose")},
        {a_aim, MakePath("/user/hand/right/input/aim/pose")},
        {a_gripPose, MakePath("/user/hand/left/input/grip/pose")},
        {a_gripPose, MakePath("/user/hand/right/input/grip/pose")},
        {a_btnA, MakePath("/user/hand/right/input/a/click")},
        {a_btnB, MakePath("/user/hand/right/input/b/click")},
        {a_btnX, MakePath("/user/hand/left/input/x/click")},
        {a_btnY, MakePath("/user/hand/left/input/y/click")},
        {a_menu, MakePath("/user/hand/left/input/menu/click")},
        {a_stickClick, MakePath("/user/hand/left/input/thumbstick/click")},
        {a_stickClick, MakePath("/user/hand/right/input/thumbstick/click")},
        {a_haptic, MakePath("/user/hand/left/output/haptic")},
        {a_haptic, MakePath("/user/hand/right/output/haptic")},
    };
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = MakePath("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = (uint32_t)(sizeof b / sizeof b[0]);
    CHECK_XR(xrSuggestInteractionProfileBindings(g_inst, &sb));

    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo as{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        as.action = a_aim;
        as.subactionPath = g_handPath[h];
        as.poseInActionSpace.orientation.w = 1;
        CHECK_XR(xrCreateActionSpace(g_session, &as, &g_aimSpace[h]));
        as.action = a_gripPose;
        CHECK_XR(xrCreateActionSpace(g_session, &as, &g_gripSpace[h]));
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g_actSet;
    CHECK_XR(xrAttachSessionActionSets(g_session, &ai));

    if (!CreateSwapchain(g_beamSc, 4, 4) || !CreateSwapchain(g_dotSc, 16, 16) || !CreateSwapchain(g_flashSc, 128, 128) ||
        !CreateSwapchain(g_wristSc, 1024, 512))
        return false;
    if (!InitText()) printf("text panels unavailable (DirectWrite / Direct2D init failed)\n");
    printf("controls ready\n");
    return true;
}

static float GetFloat(XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = hand >= 0 ? g_handPath[hand] : XR_NULL_PATH;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
static XrVector2f GetVec2(XrAction a) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &st)) && st.isActive) return st.currentState;
    return {0, 0};
}
static bool GetBool(XrAction a, int hand) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = hand >= 0 ? g_handPath[hand] : XR_NULL_PATH;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &st)) && st.isActive && st.currentState;
}

static XrQuaternionf QuatFromBasis(Vec3 x, Vec3 y, Vec3 z) {  // columns of a rotation matrix
    float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    XrQuaternionf q;
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q = {(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(1.0f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s};
    } else if (m11 > m22) {
        float s = sqrtf(1.0f + m11 - m00 - m22) * 2;
        q = {(y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s};
    } else {
        float s = sqrtf(1.0f + m22 - m00 - m11) * 2;
        q = {(z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s};
    }
    return q;
}
static Vec3 V(const XrVector3f& v) { return {v.x, v.y, v.z}; }
static Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 Mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 Norm(Vec3 a) {
    float l = sqrtf(Dot(a, a));
    return l > 1e-6f ? Mul(a, 1.0f / l) : Vec3{0, 0, 1};
}

// ---------------------------------------------------------------- text panels (game -> DirectWrite -> quad layers)
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static IDWriteTextFormat* g_fmtBody;
static IDWriteTextFormat* g_fmtTitle;
static IDWriteTextFormat* g_fmtGameBody;   // KotOR-style (dialogue, action ring): bold sans like the game's GUI font
static IDWriteTextFormat* g_fmtGameTitle;
struct TextPanel {
    Swapchain sc;
    ID3D11Texture2D* tex = nullptr;   // D2D target (UNORM twin of the swapchain format), copied in on change
    ID2D1RenderTarget* rt = nullptr;
    uint32_t serial = 0xFFFFFFFF;
    int drawnHover = -2;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    int lines = 0;
    float lineTop[24], lineBottom[24];  // texture pixels, for pointing at a line
    std::map<std::string, ID2D1Bitmap*> icons;  // GUI icons loaded into this panel's render target (null: missing)
};
static TextPanel g_text[KVR_TEXT_PANELS];
static bool g_lastAimValid;    // this frame's right aim ray (LOCAL), for pointing at text panels
static XrPosef g_lastAim;
static const int kTextW = 1024, kTextH = 512;

static bool InitText() {
    if (!g_d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (!g_dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw))) return false;
    if (!g_fmtBody) g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, 34.0f, L"en-us", &g_fmtBody);
    if (!g_fmtTitle) g_dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 40.0f, L"en-us", &g_fmtTitle);
    if (!g_fmtGameBody) g_dw->CreateTextFormat(L"Verdana", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, 29.0f, L"en-us", &g_fmtGameBody);
    if (!g_fmtGameTitle) {
        g_dw->CreateTextFormat(L"Verdana", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 31.0f, L"en-us", &g_fmtGameTitle);
        if (g_fmtGameTitle) g_fmtGameTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    DXGI_FORMAT unorm = g_scFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? DXGI_FORMAT_B8G8R8A8_UNORM
                      : g_scFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : g_scFormat;
    for (auto& p : g_text) {
        if (g_session && p.sc.handle == XR_NULL_HANDLE && !CreateSwapchain(p.sc, kTextW, kTextH)) return false;
        if (!p.tex) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = kTextW;
            d.Height = kTextH;
            d.MipLevels = d.ArraySize = 1;
            d.Format = unorm;
            d.SampleDesc.Count = 1;
            d.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &p.tex))) return false;
            IDXGISurface* surf = nullptr;
            p.tex->QueryInterface(IID_PPV_ARGS(&surf));
            D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(unorm, D2D1_ALPHA_MODE_PREMULTIPLIED));
            HRESULT hr = g_d2d->CreateDxgiSurfaceRenderTarget(surf, &rp, &p.rt);
            surf->Release();
            if (FAILED(hr)) { printf("text panel: D2D target failed %08lx\n", (unsigned long)hr); return false; }
        }
        p.serial = 0xFFFFFFFF;
    }
    return true;
}

static void ReleaseText() {
    for (auto& p : g_text) {
        for (auto& kv : p.icons)
            if (kv.second) kv.second->Release();
        p.icons.clear();
        if (p.rt) { p.rt->Release(); p.rt = nullptr; }
        if (p.tex) { p.tex->Release(); p.tex = nullptr; }
        p.serial = 0xFFFFFFFF;
    }
}

// A GUI icon exported by tools/icon_export.py (<game>\kvr\icons\<name>.png), as a bitmap of this panel's target.
static ID2D1Bitmap* PanelIcon(TextPanel& p, const std::string& name) {
    auto it = p.icons.find(name);
    if (it != p.icons.end()) return it->second;
    ID2D1Bitmap* bmp = nullptr;
    static IWICImagingFactory* wic;
    if (!wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    }
    char dir[260];
    memcpy(dir, (const void*)g_shm->gameDir, sizeof dir);
    dir[sizeof dir - 1] = 0;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%hs\\kvr\\icons\\%hs.png", dir, name.c_str());
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (wic && dir[0] && SUCCEEDED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
        p.rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
    if (conv) conv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (!bmp) printf("text panel: icon %s not found (run tools\\icon_export.py)\n", name.c_str());
    p.icons[name] = bmp;
    return bmp;
}

// redraw a panel's texture from the game's text: dark rounded card, an optional '#' title line, the rest wrapped;
// the highlighted line (pickable options) gets a gold bar
static void DrawTextPanel(TextPanel& p, const KvrTextPanel& src, int hover) {
    wchar_t wide[1024];
    char text[sizeof src.text + 1];
    memcpy(text, src.text, sizeof src.text);
    text[sizeof src.text] = 0;
    ID2D1SolidColorBrush *bg = nullptr, *fg = nullptr, *gold = nullptr, *hl = nullptr, *rule = nullptr;
    // Pickable panels (conversations, the action ring) take KotOR's own GUI look: its dialogue blue, the yellow of a
    // highlighted reply, bold sans text over a dark plate with thin blue rules. Others (instruction cards) stay neutral.
    bool game = src.pickable != 0;
    p.rt->BeginDraw();
    p.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    if (game) {
        p.rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.02f, 0.05f, 0.72f), &bg);
        p.rt->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.62f, 0.91f, 1.0f), &fg);    // KotOR dialogue blue
        p.rt->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.90f, 0.12f, 1.0f), &gold);  // KotOR highlighted reply
        p.rt->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.62f, 0.91f, 0.14f), &hl);
        p.rt->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.62f, 0.91f, 0.85f), &rule);
        p.rt->FillRectangle(D2D1::RectF(0, 6, (float)kTextW, kTextH - 6.0f), bg);
        p.rt->FillRectangle(D2D1::RectF(0, 6, (float)kTextW, 9), rule);
        p.rt->FillRectangle(D2D1::RectF(0, kTextH - 9.0f, (float)kTextW, kTextH - 6.0f), rule);
    } else {
        p.rt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.06f, 0.10f, 0.82f), &bg);
        p.rt->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.95f, 1.0f, 1.0f), &fg);
        p.rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.82f, 0.35f, 1.0f), &gold);
        p.rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.82f, 0.35f, 0.25f), &hl);
        p.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(4, 4, kTextW - 4.0f, kTextH - 4.0f), 24, 24), bg);
    }
    IDWriteTextFormat* fmtTitle = game && g_fmtGameTitle ? g_fmtGameTitle : g_fmtTitle;
    IDWriteTextFormat* fmtBody = game && g_fmtGameBody ? g_fmtGameBody : g_fmtBody;
    float y = 24;
    int line = 0;
    p.lines = 0;
    for (char* s = text; s && *s && y < kTextH - 30; line++) {
        char* nl = strchr(s, '\n');
        if (nl) *nl = 0;
        bool title = s[0] == '#';
        ID2D1Bitmap* icon = nullptr;
        if (s[0] == '@') {  // "@icon|text"
            char* bar = strchr(s, '|');
            if (bar) {
                *bar = 0;
                icon = PanelIcon(p, std::string(s + 1));
                s = bar + 1;
            }
        }
        const float iconSize = 58, textX = icon ? 36 + iconSize + 16 : 36;
        int n = MultiByteToWideChar(CP_UTF8, 0, title ? s + 1 : s, -1, wide, 1024);
        IDWriteTextLayout* lay = nullptr;
        g_dw->CreateTextLayout(wide, n > 0 ? n - 1 : 0, title ? fmtTitle : fmtBody, kTextW - textX - 36.0f, (float)kTextH, &lay);
        DWRITE_TEXT_METRICS m{};
        if (lay) lay->GetMetrics(&m);
        float textY = y;
        if (icon && m.height < iconSize) {  // an icon row: as tall as the icon, text centred beside it
            textY = y + (iconSize - m.height) / 2;
            m.height = iconSize;
        }
        if (line < 24) {
            p.lineTop[line] = y - 4;
            p.lineBottom[line] = y + m.height + 4;
            p.lines = line + 1;
        }
        bool lit = line == src.highlight || line == hover;
        if (lit)
            p.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(20, y - 4, kTextW - 20.0f, y + m.height + 4), 12, 12), hl);
        if (lay) {
            // KotOR: the speaker's line in its blue, replies blue, the highlighted reply yellow
            ID2D1SolidColorBrush* br = game ? (lit ? gold : fg) : (title || lit ? gold : fg);
            if (icon) p.rt->DrawBitmap(icon, D2D1::RectF(36, y, 36 + iconSize, y + iconSize), lit ? 1.0f : 0.85f);
            p.rt->DrawTextLayout(D2D1::Point2F(textX, textY), lay, br);
            lay->Release();
        }
        y += m.height + (title ? 14 : 8);
        s = nl ? nl + 1 : nullptr;
    }
    p.rt->EndDraw();
    if (bg) bg->Release();
    if (fg) fg->Release();
    if (gold) gold->Release();
    if (hl) hl->Release();
    if (rule) rule->Release();
    // into the swapchain
    if (!p.sc.handle) return;  // --preview: no XR session
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(p.sc.handle, &ai, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(p.sc.handle, &wi);
    g_ctx->CopyResource(p.sc.images[idx].texture, p.tex);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(p.sc.handle, &ri);
}

// the game's text panels as quad layers (anchor frame -> LOCAL)
static void AddTextLayers(std::vector<XrCompositionLayerBaseHeader*>& layers, const XrPosef& head, bool aimValid,
                          const XrPosef& aim) {
    static XrCompositionLayerQuad tbeam{XR_TYPE_COMPOSITION_LAYER_QUAD}, tdot{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool beamUsed = false;
    for (int i = 0; i < KVR_TEXT_PANELS; i++) {
        TextPanel& p = g_text[i];
        if (!p.rt || p.sc.handle == XR_NULL_HANDLE) continue;
        KvrTextPanel src;
        memcpy(&src, (const void*)&g_shm->text[i], sizeof src);
        if (src.anchor == KVR_ANCHOR_HIDDEN || src.width <= 0) {
            g_shm->text[i].hover = -1;
            continue;
        }
        XrPosef local, rel;
        memcpy(&rel.position, src.pose.pos, 12);
        memcpy(&rel.orientation, src.pose.quat, 16);
        if (src.anchor == KVR_ANCHOR_HEAD) {
            local = PoseMul(head, rel);
        } else if (src.anchor == KVR_ANCHOR_LWRIST) {
            if (!(g_shm->input.gripValid & 1)) continue;
            XrPosef g;
            memcpy(&g.position, g_shm->input.grip[0].pos, 12);
            memcpy(&g.orientation, g_shm->input.grip[0].quat, 16);
            local = PoseMul(g_origin, PoseMul(g, rel));
        } else {
            local = PoseMul(g_origin, rel);
        }
        // pointing: the right aim ray against the quad; the line under it is highlighted and reported to the game
        int hover = -1;
        float hitDist = 0;
        float qw = src.width, qh = src.width * kTextH / kTextW;
        if (src.pickable && aimValid) {
            XrPosef o = PoseMul(PoseInv(local), aim);  // aim in quad space (quad faces +Z)
            Quat q{o.orientation.x, o.orientation.y, o.orientation.z, o.orientation.w};
            Vec3 d = qrot(q, {0, 0, -1});
            if (d.z < -1e-4f || d.z > 1e-4f) {
                float tt = -o.position.z / d.z;
                float hx = o.position.x + d.x * tt, hy = o.position.y + d.y * tt;
                if (tt > 0 && fabsf(hx) <= qw / 2 && fabsf(hy) <= qh / 2) {
                    float py = (0.5f - hy / qh) * kTextH;
                    for (int l = 1; l < p.lines; l++)  // line 0 is the title (the NPC's words)
                        if (py >= p.lineTop[l] && py <= p.lineBottom[l]) hover = l;
                    hitDist = tt;
                }
            }
        }
        g_shm->text[i].hover = hover;
        if (src.serial != p.serial || hover != p.drawnHover) {
            DrawTextPanel(p, src, src.pickable ? hover : -1);
            p.serial = src.serial;
            p.drawnHover = hover;
        }
        if (hitDist > 0 && !beamUsed) {  // the laser, only while it is on a pickable panel
            beamUsed = true;
            const float cyan[4] = {0.2f, 0.8f, 1.0f, 1.0f}, white[4] = {1, 1, 1, 1};
            FillSwapchain(g_beamSc, cyan);
            FillSwapchain(g_dotSc, white);
            Quat aq{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
            Vec3 ao = V(aim.position), ad = qrot(aq, {0, 0, -1});
            Vec3 c = Add(ao, Mul(ad, hitDist * 0.5f));
            Vec3 toHead = Sub(V(head.position), c);
            Vec3 z = Norm(Sub(toHead, Mul(ad, Dot(toHead, ad))));
            tbeam.space = g_local;
            tbeam.pose.orientation = QuatFromBasis(Cross(ad, z), ad, z);
            tbeam.pose.position = {c.x, c.y, c.z};
            tbeam.size = {0.004f, hitDist};
            tbeam.subImage.swapchain = g_beamSc.handle;
            tbeam.subImage.imageRect = {{0, 0}, {g_beamSc.w, g_beamSc.h}};
            Vec3 hit = Add(ao, Mul(ad, hitDist - 0.004f));
            tdot.space = g_local;
            tdot.pose.orientation = local.orientation;
            tdot.pose.position = {hit.x, hit.y, hit.z};
            tdot.size = {0.015f, 0.015f};
            tdot.subImage.swapchain = g_dotSc.handle;
            tdot.subImage.imageRect = {{0, 0}, {g_dotSc.w, g_dotSc.h}};
        }
        p.quad.space = g_local;
        p.quad.pose = local;
        p.quad.size = {qw, qh};
        p.quad.subImage.swapchain = p.sc.handle;
        p.quad.subImage.imageRect = {{0, 0}, {kTextW, kTextH}};
        p.quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers.push_back((XrCompositionLayerBaseHeader*)&p.quad);
    }
    if (beamUsed) {
        layers.push_back((XrCompositionLayerBaseHeader*)&tbeam);
        layers.push_back((XrCompositionLayerBaseHeader*)&tdot);
    }
}

static void FillSwapchain(Swapchain& sc, const float rgba[4]) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc.handle, &ai, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc.handle, &wi);
    g_ctx->ClearRenderTargetView(sc.rtvs[idx], rgba);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
}

struct PointerResult {
    bool aimValid = false;
    XrPosef aimLocal{};
    bool hitPanel = false;
    float u = 0, v = 0, dist = 0;
    int panelHand = 1;       // the hand whose ray is tested against the panel (0 left: the HUD in the world)
    XrPosef panelAim{};      // that hand's aim pose (LOCAL)
};

// Sync controllers, publish them to the game, and work out where the right-hand laser points.
// panelPose/panelW/panelH describe the panel quad in LOCAL space when it is visible.
static bool HudSolidAt(float u, float v);

// Game -> controller rumble: one pulse per KvrHaptic serial change (combat feedback cues).
static void PlayHaptics() {
    static uint32_t seen[2];
    static bool primed;
    for (int h = 0; h < 2; h++) {
        uint32_t serial = g_shm->haptic[h].serial;
        if (!primed || serial == seen[h]) {
            seen[h] = serial;
            continue;
        }
        seen[h] = serial;
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = g_shm->haptic[h].amplitude;
        v.duration = (XrDuration)(g_shm->haptic[h].duration * 1e9);
        v.frequency = g_shm->haptic[h].frequency > 0 ? g_shm->haptic[h].frequency : XR_FREQUENCY_UNSPECIFIED;
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = a_haptic;
        hi.subactionPath = g_handPath[h];
        xrApplyHapticFeedback(g_session, &hi, (const XrHapticBaseHeader*)&v);
    }
    primed = true;
}

static PointerResult UpdateControls(XrTime t, bool panelVisible, const XrPosef& panelPose, float panelW, float panelH, bool world, bool hud) {
    PointerResult pr;
    XrActiveActionSet aas{g_actSet, XR_NULL_PATH};
    XrActionsSyncInfo syn{XR_TYPE_ACTIONS_SYNC_INFO};
    syn.countActiveActionSets = 1;
    syn.activeActionSets = &aas;
    bool synced = XR_SUCCEEDED(xrSyncActions(g_session, &syn));
    if (synced) PlayHaptics();
    if (synced) PublishHands(t);

    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (synced && XR_SUCCEEDED(xrLocateSpace(g_aimSpace[1], g_local, t, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        pr.aimValid = true;
        pr.aimLocal = loc.pose;
    }
    // Which hand points at the panel: the right one in menus; in the world (the HUD) only the left one, so the right
    // hand's trigger always stays with the weapon (shots through the HUD used to click medpacs).
    pr.panelHand = hud ? 0 : 1;
    bool panelAimValid = pr.aimValid;
    pr.panelAim = pr.aimLocal;
    if (pr.panelHand == 0) {
        XrSpaceLocation ll{XR_TYPE_SPACE_LOCATION};
        panelAimValid = synced && XR_SUCCEEDED(xrLocateSpace(g_aimSpace[0], g_local, t, &ll)) &&
                        (ll.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                        (ll.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        if (panelAimValid) pr.panelAim = ll.pose;
    }
    if (panelAimValid && panelVisible) {
        XrPosef inv = PoseInv(panelPose);
        XrPosef o = PoseMul(inv, pr.panelAim);  // aim in panel space
        Quat q{o.orientation.x, o.orientation.y, o.orientation.z, o.orientation.w};
        Vec3 d = qrot(q, {0, 0, -1});
        if (d.z < -1e-4f || d.z > 1e-4f) {
            float tt = -o.position.z / d.z;
            if (tt > 0) {
                float hx = o.position.x + d.x * tt, hy = o.position.y + d.y * tt;
                if (fabsf(hx) <= panelW / 2 && fabsf(hy) <= panelH / 2) {
                    pr.u = hx / panelW + 0.5f;
                    pr.v = 0.5f - hy / panelH;
                    pr.dist = tt;
                    // on the HUD only visible GUI counts; elsewhere the laser goes through to the world
                    pr.hitPanel = !hud || HudSolidAt(pr.u, pr.v);
                }
            }
        }
    }

    uint32_t buttons = 0;
    if (synced) {
        if (GetBool(a_btnA, -1)) buttons |= KVR_BTN_A;
        if (GetBool(a_btnB, -1)) buttons |= KVR_BTN_B;
        if (GetBool(a_btnX, -1)) buttons |= KVR_BTN_X;
        if (GetBool(a_btnY, -1)) buttons |= KVR_BTN_Y;
        if (GetBool(a_menu, -1)) buttons |= KVR_BTN_MENU;
        if (GetBool(a_stickClick, 0)) buttons |= KVR_BTN_LSTICK;
        if (GetBool(a_stickClick, 1)) buttons |= KVR_BTN_RSTICK;
    }
    uint32_t pressed = buttons & ~g_prevButtons;
    g_prevButtons = buttons;
    if (pressed & KVR_BTN_Y) g_shm->showPanel = !g_shm->showPanel;
    if (pressed & KVR_BTN_RSTICK) g_shm->recenterReq = 1;

    KvrInput& in = g_shm->input;
    in.seq++;
    MemoryBarrier();
    in.active = synced ? 1 : 0;
    XrVector2f mv = synced ? GetVec2(a_move) : XrVector2f{0, 0};
    XrVector2f tn = synced ? GetVec2(a_turn) : XrVector2f{0, 0};
    in.lx = mv.x; in.ly = mv.y; in.rx = tn.x; in.ry = tn.y;
    in.trigL = synced ? GetFloat(a_trig, 0) : 0;
    in.trigR = synced ? GetFloat(a_trig, 1) : 0;
    in.gripL = synced ? GetFloat(a_grip, 0) : 0;
    in.gripR = synced ? GetFloat(a_grip, 1) : 0;
    in.buttons = buttons;
    in.gripValid = 0;
    if (synced) {
        XrPosef invOrigin = PoseInv(g_origin);
        for (int h = 0; h < 2; h++) {
            XrSpaceLocation gl{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(g_gripSpace[h], g_local, t, &gl)) &&
                (gl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (gl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                XrPosef rel = PoseMul(invOrigin, gl.pose);
                memcpy(in.grip[h].pos, &rel.position, 12);
                memcpy(in.grip[h].quat, &rel.orientation, 16);
                in.gripValid |= 1u << h;
            }
        }
    }
    in.time = (double)t * 1e-9;
    if (pr.aimValid) {  // the right hand's aim, whatever the pointer is doing (shots and picks use it)
        XrPosef rel = PoseMul(PoseInv(g_origin), pr.aimLocal);
        memcpy(in.aim.pos, &rel.position, 12);
        memcpy(in.aim.quat, &rel.orientation, 16);
    }
    if (pr.hitPanel) {
        in.pointerMode = pr.panelHand == 0 ? KVR_POINTER_HUD_LEFT : KVR_POINTER_PANEL;
        in.panelU = pr.u;
        in.panelV = pr.v;
    } else if (pr.aimValid && world) {
        in.pointerMode = KVR_POINTER_WORLD;
    } else {
        in.pointerMode = KVR_POINTER_NONE;
    }
    MemoryBarrier();
    in.seq++;
    return pr;
}

static void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = (XrEventDataSessionStateChanged*)&ev;
            printf("session state %d\n", s->state);
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &bi))) g_sessionRunning = true;
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_sessionRunning = false;
            } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                g_lost = true;  // e.g. Air Link dropped: tear down and reconnect
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_lost = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void PublishPoses(const XrView* views) {
    KvrHostState& h = g_shm->host;
    h.seq++;
    MemoryBarrier();
    XrPosef inv = PoseInv(g_origin);
    for (int e = 0; e < 2; e++) {
        XrPosef rel = PoseMul(inv, views[e].pose);
        memcpy(h.eye[e].pos, &rel.position, 12);
        memcpy(h.eye[e].quat, &rel.orientation, 16);
        h.fov[e] = {views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown};
    }
    h.running = 1;
    MemoryBarrier();
    h.seq++;
}

// Take the game's newest frame for this slot, if it has handed one over since the last pull.
static void Pull(SharedTex& s, const volatile KvrEyeSlot& slot) {
    if (!s.mutex || s.mutex->AcquireSync(KEY_HOST, 0) != S_OK) return;
    g_ctx->CopyResource(s.local, s.tex);
    s.meta = const_cast<const KvrEyeSlot&>(slot);
    s.have = true;
    s.mutex->ReleaseSync(KEY_GAME);
}

template <class T> static void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Draw a sub-rectangle of a pulled texture (GL texture coordinates: scale and offset of the 0..1 square, bottom-up)
// into a render target viewport.
static void DrawTexRect(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, SharedTex& src, const float scale[2],
                        const float offset[2], bool guiKey, bool decodeSrgb) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* p = (float*)m.pData;
        p[0] = scale[0];
        p[1] = scale[1];
        p[2] = decodeSrgb ? 1.0f : 0.0f;
        p[3] = guiKey ? 1.0f : 0.0f;
        p[4] = offset[0];
        p[5] = offset[1];
        p[6] = p[7] = 0;
        g_ctx->Unmap(g_cb, 0);
    }
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);  // uvScale is consumed in the vertex shader
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShaderResources(0, 1, &src.srv);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    g_ctx->Flush();
}

// Draw the valid region of a pulled texture into a render target viewport.
static void DrawTex(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, SharedTex& src, uint32_t validW, uint32_t validH,
                    bool guiKey, bool decodeSrgb) {
    const float scale[2] = {(float)validW / g_sharedW, (float)validH / g_sharedH}, offset[2] = {0, 0};
    DrawTexRect(rtv, vp, src, scale, offset, guiKey, decodeSrgb);
}

// Draw a pulled texture into the acquired swapchain image.
static void Blit(Swapchain& sc, uint32_t imageIndex, SharedTex& src, uint32_t validW, uint32_t validH, bool guiKey = false) {
    D3D11_VIEWPORT vp{0, 0, (float)sc.w, (float)sc.h, 0, 1};
    DrawTex(sc.rtvs[imageIndex], vp, src, validW, validH, guiKey, g_scSrgb);
}

// ---------------------------------------------------------------- desktop mirror window (optional)
// The game's own window mirrors the headset (game/mirror.cpp). This extra window shows the host's view of the same
// frames - the left eye with the HUD keyed over it, or the panel (menus, movies) - and only opens with the
// environment variable KVR_MIRROR_WINDOW=1. It never takes the foreground; closing it just stops it.
static HWND g_mirrorWnd;
static bool g_mirrorTried, g_mirrorResize;
static IDXGISwapChain1* g_mirrorSc;
static ID3D11Device1* g_mirrorDev;  // the device the swapchain belongs to (the host recreates its device on reconnect)
static ID3D11BlendState* g_blendAlpha;

static LRESULT CALLBACK MirrorProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE) g_mirrorResize = true;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (msg == WM_DESTROY) g_mirrorWnd = nullptr;
    return DefWindowProcW(w, msg, wp, lp);
}

static void MirrorPump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void MirrorRelease() {
    SafeRelease(g_mirrorSc);
    SafeRelease(g_blendAlpha);
    g_mirrorDev = nullptr;
}

static void MirrorFrame(bool world, bool hud, bool panelShown, float panelAspect) {
    if (!g_mirrorTried) {
        g_mirrorTried = true;
        char flag[8] = "";
        GetEnvironmentVariableA("KVR_MIRROR_WINDOW", flag, sizeof flag);
        if (flag[0] != '1') return;
        WNDCLASSW wc{};
        wc.lpfnWndProc = MirrorProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"KotorVRMirror";
        RegisterClassW(&wc);
        RECT rc{0, 0, 960, 720};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        g_mirrorWnd = CreateWindowExW(WS_EX_NOACTIVATE, L"KotorVRMirror", L"KotOR VR - headset view", WS_OVERLAPPEDWINDOW,
                                      80, 80, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                                      nullptr, wc.hInstance, nullptr);
        if (g_mirrorWnd) ShowWindow(g_mirrorWnd, SW_SHOWNOACTIVATE);
        printf("mirror window %s\n", g_mirrorWnd ? "open" : "failed");
    }
    if (!g_mirrorWnd || !g_dev) return;
    if (g_mirrorDev != g_dev) {
        MirrorRelease();
        IDXGIDevice* dd = nullptr;
        IDXGIAdapter* ad = nullptr;
        IDXGIFactory2* fac = nullptr;
        g_dev->QueryInterface(IID_PPV_ARGS(&dd));
        if (dd) dd->GetAdapter(&ad);
        if (ad) ad->GetParent(IID_PPV_ARGS(&fac));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (fac) fac->CreateSwapChainForHwnd(g_dev, g_mirrorWnd, &d, nullptr, nullptr, &g_mirrorSc);
        if (fac) fac->MakeWindowAssociation(g_mirrorWnd, DXGI_MWA_NO_ALT_ENTER);
        SafeRelease(fac);
        SafeRelease(ad);
        SafeRelease(dd);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &g_blendAlpha);
        g_mirrorDev = g_dev;
        g_mirrorResize = false;
        if (!g_mirrorSc) return;
    }
    if (!g_mirrorSc) return;
    if (g_mirrorResize) {
        g_mirrorResize = false;
        g_mirrorSc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    }
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_mirrorSc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    ID3D11RenderTargetView* rtv = nullptr;
    g_dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();
    if (!rtv) return;
    const float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(rtv, black);
    // letterbox the image's aspect into the window
    float aspect = world ? (float)g_shared[0].meta.width / std::max(1u, g_shared[0].meta.height) : panelAspect;
    float w = (float)bd.Width, h = (float)bd.Height;
    float vw = w, vh = w / aspect;
    if (vh > h) { vh = h; vw = h * aspect; }
    D3D11_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    if (world && g_shared[0].have) {
        DrawTex(rtv, vp, g_shared[0], g_shared[0].meta.width, g_shared[0].meta.height, false, false);
        if (hud && panelShown && g_shared[2].have) {
            const float f[4] = {0, 0, 0, 0};
            g_ctx->OMSetBlendState(g_blendAlpha, f, 0xFFFFFFFF);
            DrawTex(rtv, vp, g_shared[2], g_shared[2].meta.width, g_shared[2].meta.height, true, false);
            g_ctx->OMSetBlendState(nullptr, f, 0xFFFFFFFF);
        }
    } else if (panelShown && g_shared[2].have) {
        DrawTex(rtv, vp, g_shared[2], g_shared[2].meta.width, g_shared[2].meta.height, false, false);
    }
    rtv->Release();
    g_mirrorSc->Present(0, 0);
}

// Debug: write a texture to a 24-bit BMP (RGBA8 / BGRA8 assumed; top-down rows as stored).
static void DumpTex(ID3D11Texture2D* tex, const char* path) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* st;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st))) { printf("dump %s: staging failed (format %d)\n", path, d.Format); return; }
    g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
        FILE* f;
        if (!fopen_s(&f, path, "wb")) {
            int rowBytes = (d.Width * 3 + 3) & ~3;
            BITMAPFILEHEADER fh{0x4D42, (DWORD)(54 + rowBytes * d.Height), 0, 0, 54};
            BITMAPINFOHEADER ih{40, (LONG)d.Width, -(LONG)d.Height, 1, 24};
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&ih, sizeof ih, 1, f);
            std::vector<unsigned char> row(rowBytes);
            for (UINT y = 0; y < d.Height; y++) {
                const unsigned char* src = (const unsigned char*)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < d.Width; x++) {
                    const unsigned char* p = src + x * 4;
                    row[x * 3 + 0] = bgra ? p[0] : p[2];
                    row[x * 3 + 1] = p[1];
                    row[x * 3 + 2] = bgra ? p[2] : p[0];
                }
                fwrite(row.data(), rowBytes, 1, f);
            }
            fclose(f);
            printf("dumped %s (%ux%u fmt %d)\n", path, d.Width, d.Height, d.Format);
        }
        g_ctx->Unmap(st, 0);
    }
    st->Release();
}

static void ClearSwapchain(Swapchain& sc, uint32_t imageIndex) {
    const float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(sc.rtvs[imageIndex], black);
}

static uint32_t AcquireImage(Swapchain& sc) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(sc.handle, &ai, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc.handle, &wi);
    return idx;
}
static void ReleaseImage(Swapchain& sc) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(sc.handle, &ri);
}

static XrPosef ToXr(const KvrPose& p) {
    return {{p.quat[0], p.quat[1], p.quat[2], p.quat[3]}, {p.pos[0], p.pos[1], p.pos[2]}};
}

// Is there visible GUI under the laser? Samples an 8x8 block of the HUD texture; the readback is one frame behind.
static bool HudSolidAt(float u, float v) {
    static ID3D11Texture2D* st[2];
    static ID3D11Device1* stDev;
    static int cur;
    static bool pending[2];
    static bool lastSolid;
    SharedTex& p = g_shared[2];
    if (!p.local || !g_sharedW) return false;
    if (stDev != g_dev) {  // device recreated after a reconnect
        st[0] = st[1] = nullptr;
        pending[0] = pending[1] = false;
        stDev = g_dev;
    }
    if (!st[0]) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = 8;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_dev->CreateTexture2D(&d, nullptr, &st[0]);
        g_dev->CreateTexture2D(&d, nullptr, &st[1]);
    }
    int prev = cur ^ 1;
    D3D11_MAPPED_SUBRESOURCE m;
    if (pending[prev] && SUCCEEDED(g_ctx->Map(st[prev], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        int maxc = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                const unsigned char* px = (const unsigned char*)m.pData + y * m.RowPitch + x * 4;
                maxc = std::max(maxc, (int)std::max(px[0], std::max(px[1], px[2])));
            }
        g_ctx->Unmap(st[prev], 0);
        lastSolid = maxc > 24;
        pending[prev] = false;
    }
    // GL rows are bottom-up in the texture
    int w = (int)p.meta.width, h = (int)p.meta.height;
    int x = std::min(std::max((int)(u * w) - 4, 0), w - 8);
    int y = std::min(std::max((int)((1.0f - v) * h) - 4, 0), h - 8);
    D3D11_BOX box{(UINT)x, (UINT)y, 0, (UINT)x + 8, (UINT)y + 8, 1};
    g_ctx->CopySubresourceRegion(st[cur], 0, 0, 0, 0, p.local, 0, &box);
    pending[cur] = true;
    cur ^= 1;
    return lastSolid;
}

// ---------------------------------------------------------------- recording ([Debug] Record=1 in kvr.ini)
// What the headset shows (left eye with the HUD keyed over it, or the menu / movie panel), 960x720 at 30 fps, piped
// to ffmpeg (on PATH) as H.264 into <game>\kvr\recordings\. For reviewing sessions (pop-in, glitches) and for trailer
// footage. A writer thread feeds ffmpeg so the XR loop never waits on it; frames are dropped if it falls behind.
static const int kRecW = 960, kRecH = 720;
static ID3D11Device1* g_recDev;
static ID3D11Texture2D* g_recRt;
static ID3D11RenderTargetView* g_recRtv;
static ID3D11Texture2D* g_recStage[2];
static bool g_recPending[2];
static int g_recCur;
static ID3D11BlendState* g_recBlend;
static HANDLE g_recWrite, g_recProc, g_recThread, g_recEvent;
static CRITICAL_SECTION g_recLock;
static std::vector<std::vector<uint8_t>> g_recQueue;
static volatile bool g_recStop;
static double g_recStart;
static uint64_t g_recFrames;

static DWORD WINAPI RecWriter(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_recEvent, 200);
        std::vector<std::vector<uint8_t>> batch;
        EnterCriticalSection(&g_recLock);
        batch.swap(g_recQueue);
        bool stop = g_recStop;
        LeaveCriticalSection(&g_recLock);
        for (auto& f : batch) {
            DWORD n = 0;
            if (!WriteFile(g_recWrite, f.data(), (DWORD)f.size(), &n, nullptr)) { stop = true; break; }
        }
        if (stop) return 0;
    }
}

static bool RecStart() {
    char dir[260];
    memcpy(dir, (const void*)g_shm->gameDir, sizeof dir);
    dir[sizeof dir - 1] = 0;
    if (!dir[0]) return false;
    char folder[MAX_PATH], path[MAX_PATH], cmd[1024];
    sprintf_s(folder, "%s\\kvr\\recordings", dir);
    CreateDirectoryA(folder, nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    sprintf_s(path, "%s\\kvr_%04d%02d%02d_%02d%02d%02d.mp4", folder, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
              st.wSecond);
    sprintf_s(cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 30 -i - -c:v libx264 -preset veryfast "
              "-crf 20 -pix_fmt yuv420p \"%s\"", kRecW, kRecH, path);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 8 << 20)) return false;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        printf("record: could not start ffmpeg (is it on PATH?)\n");
        CloseHandle(rd);
        CloseHandle(wr);
        return false;
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);
    g_recProc = pi.hProcess;
    g_recWrite = wr;
    static bool lockInit;
    if (!lockInit) { InitializeCriticalSection(&g_recLock); lockInit = true; }
    g_recStop = false;
    g_recEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_recThread = CreateThread(nullptr, 0, RecWriter, nullptr, 0, nullptr);
    g_recStart = GetTickCount64() / 1000.0;
    g_recFrames = 0;
    printf("record: recording to %s\n", path);
    return true;
}

static void RecStop() {
    if (!g_recProc) return;
    EnterCriticalSection(&g_recLock);
    g_recStop = true;
    LeaveCriticalSection(&g_recLock);
    SetEvent(g_recEvent);
    WaitForSingleObject(g_recThread, 3000);
    CloseHandle(g_recThread);
    CloseHandle(g_recEvent);
    CloseHandle(g_recWrite);  // ffmpeg sees the end of its input and finishes the file
    WaitForSingleObject(g_recProc, 10000);
    CloseHandle(g_recProc);
    g_recProc = g_recWrite = g_recThread = g_recEvent = nullptr;
    printf("record: stopped (%llu frames)\n", (unsigned long long)g_recFrames);
}

static void RecRelease() {
    SafeRelease(g_recRtv);
    SafeRelease(g_recRt);
    SafeRelease(g_recStage[0]);
    SafeRelease(g_recStage[1]);
    SafeRelease(g_recBlend);
    g_recPending[0] = g_recPending[1] = false;
    g_recDev = nullptr;
}

static void RecordFrame(bool world, bool hud, bool panelShown, float panelAspect) {
    // the switch: kvr.ini [Debug] Record, re-read every 2 s
    static DWORD lastCheck;
    static bool want;
    if (GetTickCount() - lastCheck > 2000) {
        lastCheck = GetTickCount();
        char dir[260], ini[MAX_PATH];
        memcpy(dir, (const void*)g_shm->gameDir, sizeof dir);
        dir[sizeof dir - 1] = 0;
        sprintf_s(ini, "%s\\kvr\\kvr.ini", dir);
        want = dir[0] && GetPrivateProfileIntA("Debug", "Record", 0, ini) != 0;
        if (want && !g_recProc && !RecStart()) want = false;
        if (!want && g_recProc) RecStop();
    }
    if (!g_recProc || !g_dev) return;
    double now = GetTickCount64() / 1000.0;
    uint64_t due = (uint64_t)((now - g_recStart) * 30.0);
    if (g_recFrames >= due) return;
    if (g_recDev != g_dev) {
        RecRelease();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = kRecW;
        d.Height = kRecH;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        g_dev->CreateTexture2D(&d, nullptr, &g_recRt);
        if (g_recRt) g_dev->CreateRenderTargetView(g_recRt, nullptr, &g_recRtv);
        d.BindFlags = 0;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_dev->CreateTexture2D(&d, nullptr, &g_recStage[0]);
        g_dev->CreateTexture2D(&d, nullptr, &g_recStage[1]);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &g_recBlend);
        g_recDev = g_dev;
    }
    if (!g_recRtv || !g_recStage[0] || !g_recStage[1]) return;
    // compose this frame
    const float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_recRtv, black);
    float aspect = world ? (float)g_shared[0].meta.width / std::max(1u, g_shared[0].meta.height) : panelAspect;
    float w = (float)kRecW, h = (float)kRecH, vw = w, vh = w / aspect;
    if (vh > h) { vh = h; vw = h * aspect; }
    D3D11_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    if (world && g_shared[0].have) {
        DrawTex(g_recRtv, vp, g_shared[0], g_shared[0].meta.width, g_shared[0].meta.height, false, false);
        if (hud && panelShown && g_shared[2].have) {
            const float f[4] = {0, 0, 0, 0};
            g_ctx->OMSetBlendState(g_recBlend, f, 0xFFFFFFFF);
            DrawTex(g_recRtv, vp, g_shared[2], g_shared[2].meta.width, g_shared[2].meta.height, true, false);
            g_ctx->OMSetBlendState(nullptr, f, 0xFFFFFFFF);
        }
    } else if (panelShown && g_shared[2].have) {
        DrawTex(g_recRtv, vp, g_shared[2], g_shared[2].meta.width, g_shared[2].meta.height, false, false);
    }
    g_ctx->CopyResource(g_recStage[g_recCur], g_recRt);
    g_recPending[g_recCur] = true;
    // read back the previous one (a frame of latency, no stall)
    int prev = g_recCur ^ 1;
    if (g_recPending[prev]) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(g_recStage[prev], 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<uint8_t> px((size_t)kRecW * kRecH * 4);
            for (int y = 0; y < kRecH; y++)
                memcpy(&px[(size_t)y * kRecW * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)kRecW * 4);
            g_ctx->Unmap(g_recStage[prev], 0);
            EnterCriticalSection(&g_recLock);
            // keep up with real time: repeat the frame for any 30 fps slots we missed (at most a second's worth)
            uint64_t n = std::min<uint64_t>(due - g_recFrames, 30);
            if (g_recQueue.size() < 60)
                for (uint64_t i = 0; i < n; i++) g_recQueue.push_back(px);
            LeaveCriticalSection(&g_recLock);
            g_recFrames += n;
            SetEvent(g_recEvent);
        }
        g_recPending[prev] = false;
    }
    g_recCur = prev;
}

// ---------------------------------------------------------------- rim flash (hits taken, blocks)
// A head-locked quad just in front of the eyes, transparent in the middle and tinted toward the edges, fading out.
static ID3D11Texture2D* g_flashTex;
static ID3D11ShaderResourceView* g_flashSrv;
static ID3D11Device1* g_flashDev;

static void AddFlashLayer(std::vector<XrCompositionLayerBaseHeader*>& layers) {
    static uint32_t seen;
    static double start = -10;
    static KvrFlash f;
    double now = GetTickCount64() / 1000.0;
    if (g_shm->flash.serial != seen) {
        seen = g_shm->flash.serial;
        MemoryBarrier();
        f = const_cast<const KvrFlash&>(g_shm->flash);
        start = now;
    }
    double t = now - start;
    if (f.duration <= 0 || t >= f.duration || !g_flashSc.handle) return;
    float k = (float)(1.0 - t / f.duration);
    float s = f.strength * k * k;
    if (s < 0.01f) return;
    const int N = 128;
    if (g_flashDev != g_dev) {
        SafeRelease(g_flashSrv);
        SafeRelease(g_flashTex);
        D3D11_TEXTURE2D_DESC d{};
        d.Width = d.Height = N;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_dev->CreateTexture2D(&d, nullptr, &g_flashTex);
        if (g_flashTex) g_dev->CreateShaderResourceView(g_flashTex, nullptr, &g_flashSrv);
        g_flashDev = g_dev;
    }
    if (!g_flashSrv) return;
    static std::vector<uint32_t> px(N * N);
    uint32_t r = (uint32_t)(std::min(1.0f, f.rgb[0]) * 255), g = (uint32_t)(std::min(1.0f, f.rgb[1]) * 255),
             b = (uint32_t)(std::min(1.0f, f.rgb[2]) * 255);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1;
            float d = sqrtf(dx * dx + dy * dy);
            float m = std::min(1.0f, std::max(0.0f, (d - 0.35f) / 0.5f));  // clear centre, full at the rim
            uint32_t a = (uint32_t)(m * m * s * 255);
            px[y * N + x] = r | (g << 8) | (b << 16) | (a << 24);
        }
    g_ctx->UpdateSubresource(g_flashTex, 0, nullptr, px.data(), N * 4, 0);
    uint32_t idx = AcquireImage(g_flashSc);
    const float clear[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(g_flashSc.rtvs[idx], clear);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* p = (float*)m.pData;
        p[0] = p[1] = 1;
        p[2] = g_scSrgb ? 1.0f : 0.0f;
        p[3] = 2;  // alpha from the texture
        p[4] = p[5] = p[6] = p[7] = 0;
        g_ctx->Unmap(g_cb, 0);
    }
    D3D11_VIEWPORT vp{0, 0, (float)N, (float)N, 0, 1};
    g_ctx->OMSetRenderTargets(1, &g_flashSc.rtvs[idx], nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShaderResources(0, 1, &g_flashSrv);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &nullSrv);
    ReleaseImage(g_flashSc);
    static XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.space = g_view;
    q.pose = {{0, 0, 0, 1}, {0, 0, -0.5f}};
    q.size = {2.4f, 2.4f};  // ~135 deg at 0.5 m: past the edges of the headset's view
    q.subImage.swapchain = g_flashSc.handle;
    q.subImage.imageRect = {{0, 0}, {N, N}};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    layers.push_back((XrCompositionLayerBaseHeader*)&q);
}

// ---------------------------------------------------------------- wrist panel (docs/DIEGETIC_HUD.md)
// The flat HUD's pieces (menu row, minimap, party, quick slots, toggles - rectangles from the game, KvrHudLayout)
// cropped out of the GUI frame onto a panel along the left forearm. It shows while the left palm faces the eyes; the
// right index fingertip hovers and pokes it, which the game receives as the cursor on (and a click of) the real,
// hidden HUD control. The right hand's laser points at it (a beam and a dot, gold on a button) and the right trigger
// clicks (2026-10-27: fingertip pokes were too imprecise).
static const int kWristW = 1024, kWristH = 512;
struct WristZone {
    float dst[4];  // wrist texture px: x, y, w, h (where the crop is drawn)
    int src[4];    // panel px, top-left origin: x, y, w, h (the HUD piece)
};
static WristZone g_wristZones[KVR_HUD_REGIONS];
static int g_wristZoneCount;
static ID3D11BlendState* g_blendKey;  // GUI crops keyed over the wrist's dark plate
static ID3D11Device1* g_blendKeyDev;
struct WristState {
    bool shown = false;
    double since = 0;      // when the palm started / stopped facing the eyes
    XrPosef pose{};        // LOCAL
    float w = 0.26f, h = 0.13f;
    bool hover = false;
    float u = 0, v = 0;    // panel texture coordinates (0..1, top-left) under the fingertip
    bool poke = false;
    XrVector3f tip{};      // LOCAL, for the cursor dot
    bool nearTip = false;  // the fingertip is within reach of the panel: draw the touch ray
    XrVector3f proj{};     // LOCAL, the fingertip projected onto the panel
};
static WristState g_wrist;

static float IniFloat(const char* section, const char* key, float def) {
    char dir[260], ini[MAX_PATH], val[32];
    memcpy(dir, (const void*)g_shm->gameDir, sizeof dir);
    dir[sizeof dir - 1] = 0;
    if (!dir[0]) return def;
    sprintf_s(ini, "%s\\kvr\\kvr.ini", dir);
    GetPrivateProfileStringA(section, key, "", val, sizeof val, ini);
    return val[0] ? (float)atof(val) : def;
}

// Where each HUD piece goes on the wrist texture: menu row on top, minimap + party in the middle, quick slots and the
// toggles at the bottom; each crop letterboxed into its zone.
static void LayoutWrist() {
    static const float zones[KVR_HUD_REGIONS][4] = {
        {12, 8, 1000, 84},     // menu row
        {12, 104, 300, 300},   // minimap
        {324, 104, 688, 300},  // party
        {12, 416, 640, 88},    // quick slots
        {664, 416, 348, 88},   // pause / solo / stealth
    };
    g_wristZoneCount = 0;
    for (int i = 0; i < KVR_HUD_REGIONS; i++) {
        const int32_t* r = g_shm->hud.rect[i];
        if (r[2] <= 0 || r[3] <= 0) continue;
        WristZone& z = g_wristZones[g_wristZoneCount++];
        memcpy(z.src, r, sizeof z.src);
        float zw = zones[i][2], zh = zones[i][3], a = (float)r[2] / r[3];
        float w = zw, h = zw / a;
        if (h > zh) { h = zh; w = zh * a; }
        z.dst[0] = zones[i][0] + (zw - w) / 2;
        z.dst[1] = zones[i][1] + (zh - h) / 2;
        z.dst[2] = w;
        z.dst[3] = h;
    }
}

static void DrawWrist() {
    if (!g_wristSc.handle || !g_shared[2].have || !g_sharedW) return;
    if (g_blendKeyDev != g_dev) {
        SafeRelease(g_blendKey);
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &g_blendKey);
        g_blendKeyDev = g_dev;
    }
    uint32_t idx = AcquireImage(g_wristSc);
    const float plate[4] = {0.01f, 0.03f, 0.07f, 0.88f};
    g_ctx->ClearRenderTargetView(g_wristSc.rtvs[idx], plate);
    const float f[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendKey, f, 0xFFFFFFFF);
    float H = (float)g_sharedH;
    for (int i = 0; i < g_wristZoneCount; i++) {
        const WristZone& z = g_wristZones[i];
        D3D11_VIEWPORT vp{z.dst[0], z.dst[1], z.dst[2], z.dst[3], 0, 1};
        // the crop in GL (bottom-up) texture coordinates
        float scale[2] = {z.src[2] / (float)g_sharedW, z.src[3] / H};
        float offset[2] = {z.src[0] / (float)g_sharedW, (H - z.src[1] - z.src[3]) / H};
        DrawTexRect(g_wristSc.rtvs[idx], vp, g_shared[2], scale, offset, true, g_scSrgb);
    }
    g_ctx->OMSetBlendState(nullptr, f, 0xFFFFFFFF);
    ReleaseImage(g_wristSc);
}

// The right index fingertip (LOCAL): the tracked hand's tip joint, else a point just past the controller's tip.
static bool FingerTip(const PointerResult& pr, XrVector3f* out) {
    const KvrHands& hs = g_shm->hands;
    if (hs.valid & 2) {
        const KvrPose& j = hs.joint[1][10];  // XR_HAND_JOINT_INDEX_TIP_EXT
        XrPosef p = PoseMul(g_origin, XrPosef{{j.quat[0], j.quat[1], j.quat[2], j.quat[3]}, {j.pos[0], j.pos[1], j.pos[2]}});
        *out = p.position;
        return true;
    }
    if (!pr.aimValid) return false;
    Quat q{pr.aimLocal.orientation.x, pr.aimLocal.orientation.y, pr.aimLocal.orientation.z, pr.aimLocal.orientation.w};
    Vec3 tip = Add(V(pr.aimLocal.position), Mul(qrot(q, {0, 0, -1}), 0.02f));
    *out = {tip.x, tip.y, tip.z};
    return true;
}

// Once per frame after the controls are synced: show / hide, place, and hit-test the fingertip. Returns the layer.
static XrCompositionLayerQuad* UpdateWrist(const XrPosef& head, const PointerResult& pr, bool world) {
    static XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    g_wrist.hover = false;
    g_wrist.nearTip = false;
    const KvrInput& in = g_shm->input;
    static uint32_t layoutSerial = 0xFFFFFFFF;
    if (g_shm->hud.serial != layoutSerial) {
        layoutSerial = g_shm->hud.serial;
        LayoutWrist();
    }
    bool can = world && (in.gripValid & 1) && g_wristZoneCount > 0;
    XrPosef grip{};
    float facing = -1;
    if (can) {
        XrPosef rel{{in.grip[0].quat[0], in.grip[0].quat[1], in.grip[0].quat[2], in.grip[0].quat[3]},
                    {in.grip[0].pos[0], in.grip[0].pos[1], in.grip[0].pos[2]}};
        grip = PoseMul(g_origin, rel);
        Quat gq{grip.orientation.x, grip.orientation.y, grip.orientation.z, grip.orientation.w};
        Vec3 palm = qrot(gq, {1, 0, 0});  // left hand: +X points out of the palm
        Vec3 toHead = Norm(Sub(V(head.position), V(grip.position)));
        facing = Dot(palm, toHead);
    }
    // the "check your watch" gesture, with hysteresis
    double now = GetTickCount64() / 1000.0;
    bool want = g_wrist.shown ? facing > 0.35f : facing > 0.6f;
    if (want != g_wrist.shown) {
        if (g_wrist.since == 0) g_wrist.since = now;
        if (now - g_wrist.since > (want ? 0.15 : 0.3)) {
            g_wrist.shown = want;
            g_wrist.since = 0;
        }
    } else {
        g_wrist.since = 0;
    }
    if (!g_wrist.shown || !can) {
        if (!can) g_wrist.shown = false;
        g_wrist.poke = false;
        return nullptr;
    }
    // along the forearm just above the wrist, facing out of the palm, its top toward the fingers
    Quat gq{grip.orientation.x, grip.orientation.y, grip.orientation.z, grip.orientation.w};
    Vec3 X = qrot(gq, {1, 0, 0}), Z = qrot(gq, {0, 0, 1});
    Vec3 c = Add(Add(V(grip.position), Mul(Z, 0.10f)), Mul(X, 0.035f));
    Vec3 qz = X, qy = Mul(Z, -1.0f), qx = Cross(qy, qz);
    g_wrist.pose.orientation = QuatFromBasis(qx, qy, qz);
    g_wrist.pose.position = {c.x, c.y, c.z};
    // the right hand's laser (2026-10-27: fingertip pokes were too imprecise on buttons this small): the aim ray
    // against the panel; the trigger clicks what the dot is on (pointer mode "panel": the game's own click path)
    if (pr.aimValid) {
        Vec3 o = V(pr.aimLocal.position);
        Quat aq{pr.aimLocal.orientation.x, pr.aimLocal.orientation.y, pr.aimLocal.orientation.z, pr.aimLocal.orientation.w};
        Vec3 d = qrot(aq, {0, 0, -1});
        float den = Dot(d, qz);
        if (den < -0.05f) {  // facing the panel
            float t = Dot(Sub(c, o), qz) / den;
            if (t > 0.02f && t < 1.2f) {
                Vec3 hit = Add(o, Mul(d, t));
                Vec3 rel = Sub(hit, c);
                float px = Dot(rel, qx), py = Dot(rel, qy);
                float u = px / g_wrist.w + 0.5f, v = 0.5f - py / g_wrist.h;
                if (u >= -0.05f && u <= 1.05f && v >= -0.1f && v <= 1.1f) {
                    g_wrist.nearTip = true;
                    g_wrist.tip = {o.x, o.y, o.z};
                    g_wrist.proj = {hit.x, hit.y, hit.z};
                    float wx = u * kWristW, wy = v * kWristH;
                    static int lastZone = -1;
                    int zone = -1;
                    for (int i = 0; i < g_wristZoneCount; i++) {
                        const WristZone& z = g_wristZones[i];
                        if (wx < z.dst[0] || wy < z.dst[1] || wx > z.dst[0] + z.dst[2] || wy > z.dst[1] + z.dst[3]) continue;
                        float sx = z.src[0] + (wx - z.dst[0]) / z.dst[2] * z.src[2];
                        float sy = z.src[1] + (wy - z.dst[1]) / z.dst[3] * z.src[3];
                        g_wrist.hover = true;
                        g_wrist.u = sx / g_sharedW;
                        g_wrist.v = sy / g_sharedH;
                        zone = i;
                        break;
                    }
                    if (zone >= 0 && zone != lastZone) {  // a light tick entering a piece of the HUD
                        KvrHaptic& hp = g_shm->haptic[1];
                        hp.amplitude = 0.25f;
                        hp.duration = 0.015f;
                        hp.frequency = 200;
                        MemoryBarrier();
                        hp.serial = hp.serial + 1;
                    }
                    lastZone = zone;
                }
            }
        }
    }
    g_wrist.poke = false;
    DrawWrist();
    q.space = g_local;
    q.pose = g_wrist.pose;
    q.size = {g_wrist.w, g_wrist.h};
    q.subImage.swapchain = g_wristSc.handle;
    q.subImage.imageRect = {{0, 0}, {kWristW, kWristH}};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    // the wrist is what the fingertip is on: the game's cursor goes there, a poke is its click
    if (g_wrist.hover) {
        KvrInput& w = g_shm->input;
        w.seq++;
        MemoryBarrier();
        w.pointerMode = KVR_POINTER_PANEL;
        w.panelU = g_wrist.u;
        w.panelV = g_wrist.v;
        w.poke = g_wrist.poke ? 1 : 0;
        MemoryBarrier();
        w.seq++;
    } else if (g_shm->input.poke) {
        KvrInput& w = g_shm->input;
        w.seq++;
        MemoryBarrier();
        w.poke = 0;
        MemoryBarrier();
        w.seq++;
    }
    return &q;
}

static void Frame() {
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session, &fwi, &fs))) return;
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(g_session, &fbi);

    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t nv = 2;
    xrLocateViews(g_session, &vli, &vs, 2, &nv, views);
    bool tracked = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;

    if (tracked && (!g_haveOrigin || g_shm->recenterReq)) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g_view, g_local, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            g_origin = YawOnly(loc.pose);
            g_haveOrigin = true;
            g_shm->recenterReq = 0;
            printf("recentered\n");
        }
    }
    if (tracked && g_haveOrigin) PublishPoses(views);

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

    if (fs.shouldRender && EnsureShared()) {
        Pull(g_shared[0], g_shm->eyes[0]);
        Pull(g_shared[1], g_shm->eyes[1]);
        Pull(g_shared[2], g_shm->panel);
        uint32_t alive = g_shm->gameAlive;
        // a cutscene movie: the panel carries its frames (the world frames are stale while it plays)
        bool movie = g_shared[2].have && (g_shared[2].meta.flags & KVR_SLOT_MOVIE);
        bool world = !movie && g_shared[0].have && g_shared[1].have && (alive - g_shm->lastWorldFrame) < 30;
        if (world) {
            for (int e = 0; e < 2; e++) {
                const KvrEyeSlot& slot = g_shared[e].meta;
                uint32_t idx = AcquireImage(g_eyeSc[e]);
                Blit(g_eyeSc[e], idx, g_shared[e], slot.width, slot.height);
                ReleaseImage(g_eyeSc[e]);
                pv[e].pose = PoseMul(g_origin, ToXr(slot.pose));
                pv[e].fov = {slot.fov.angleLeft, slot.fov.angleRight, slot.fov.angleUp, slot.fov.angleDown};
                pv[e].subImage.swapchain = g_eyeSc[e].handle;
                pv[e].subImage.imageRect = {{0, 0}, {g_eyeSc[e].w, g_eyeSc[e].h}};
            }
            proj.space = g_local;
            proj.viewCount = 2;
            proj.views = pv;
            layers.push_back((XrCompositionLayerBaseHeader*)&proj);
        }
        // Menus (no world): opaque theater screen 2.4 m ahead. In the world the panel carries only the GUI, keyed
        // over the stereo view as a HUD 1.6 m ahead covering ~64 deg; Y (or F9) hides it.
        bool hud = world && (g_shared[2].meta.flags & KVR_SLOT_GUI_ONLY);
        // a pickable text panel (our conversation / action ring) replaces the game's flat HUD while it is up
        bool pickableUp = false;
        for (int i = 0; i < KVR_TEXT_PANELS; i++)
            pickableUp |= g_shm->text[i].pickable && g_shm->text[i].anchor != KVR_ANCHOR_HIDDEN;
        // The flat HUD stays hidden in the world (its pieces are on the wrist); [Features] FlatHud=1 in kvr.ini shows
        // it as before. Y flips whichever is the default.
        static DWORD flatCheck;
        static bool flatDefault;
        if (GetTickCount() - flatCheck > 2000) {
            flatCheck = GetTickCount();
            flatDefault = IniFloat("Features", "FlatHud", 0) != 0;
        }
        bool flatHud = flatDefault ? !g_shm->showPanel : g_shm->showPanel != 0;
        // a game menu over the world (container, inventory, store, computer): the flat panel with the right-hand
        // laser, as in the menus - never the wrist
        bool menuUp = hud && g_shm->menuOpen != 0;
        bool panelVisible = movie || !world || menuUp || (hud && flatHud && !pickableUp);
        static DWORD lastHudLog;
        if (GetTickCount() - lastHudLog > 3000) {
            lastHudLog = GetTickCount();
            printf("hud: world=%d hud=%d menu=%d panelVisible=%d panelFlags=0x%x panelHave=%d hidden(showPanel)=%u cinematic=%u\n", world, hud,
                   menuUp, panelVisible, g_shared[2].meta.flags, g_shared[2].have, g_shm->showPanel, g_shm->cinematic);
        }
        // HUD lazily follows head yaw: once you have looked more than 35 deg away for 0.6 s it swings round to face you
        static float hudYaw;
        static DWORD awaySince;
        if (tracked) {
            XrPosef headRel = PoseMul(PoseInv(g_origin), views[0].pose);
            float headYaw = YawOf(headRel.orientation);
            float diff = remainderf(headYaw - hudYaw, 6.2831853f);
            if (fabsf(diff) > 0.61f) {
                if (!awaySince) awaySince = GetTickCount();
            } else {
                awaySince = 0;
            }
            if (awaySince && GetTickCount() - awaySince > 600) hudYaw += diff * 0.15f;  // ease toward the head
            if (awaySince && fabsf(diff) < 0.05f) awaySince = 0;
        }
        XrPosef hudYawPose{{0, sinf(hudYaw / 2), 0, cosf(hudYaw / 2)}, {0, 0, 0}};
        XrPosef panelLocal = world ? PoseMul(hudYawPose, XrPosef{{0, 0, 0, 1}, {0, -0.1f, -1.6f}}) : XrPosef{{0, 0, 0, 1}, {0, 0, -2.4f}};
        float panelW = world ? 2.0f : 2.4f, panelH = panelW * 0.75f;
        // movies: a cinema screen 3.6 m wide, 3 m out where you were looking when it started, at the movie's aspect
        static bool wasMovie;
        static float movieYaw;
        if (movie && !wasMovie && tracked) movieYaw = YawOf(PoseMul(PoseInv(g_origin), views[0].pose).orientation);
        wasMovie = movie;
        float panelAspect = g_shared[2].meta.height ? (float)g_shared[2].meta.width / g_shared[2].meta.height : 4.0f / 3.0f;
        if (movie) {
            XrPosef yawPose{{0, sinf(movieYaw / 2), 0, cosf(movieYaw / 2)}, {0, 0, 0}};
            panelLocal = PoseMul(yawPose, XrPosef{{0, 0, 0, 1}, {0, 0.1f, -3.0f}});
            panelW = 3.6f;
            panelH = panelW / panelAspect;
        }
        XrPosef panelPose = PoseMul(g_origin, panelLocal);
        if (panelVisible) {
            const KvrEyeSlot& slot = g_shared[2].meta;
            uint32_t idx = AcquireImage(g_panelSc);
            if (g_shared[2].have) Blit(g_panelSc, idx, g_shared[2], slot.width, slot.height, hud);
            else ClearSwapchain(g_panelSc, idx);
            ReleaseImage(g_panelSc);
            quad.space = g_local;
            quad.pose = panelPose;
            quad.size = {panelW, panelH};
            quad.subImage.swapchain = g_panelSc.handle;
            quad.subImage.imageRect = {{0, 0}, {g_panelSc.w, g_panelSc.h}};
            quad.layerFlags = hud ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) : 0;
            layers.push_back((XrCompositionLayerBaseHeader*)&quad);
        }

        MirrorFrame(world, hud, panelVisible, panelAspect);
        RecordFrame(world, hud, panelVisible, panelAspect);

        // controllers: publish state, then draw the right-hand laser and the panel cursor
        PointerResult pr = UpdateControls(fs.predictedDisplayTime, panelVisible, panelPose, panelW, panelH, world, hud && !menuUp);
        g_lastAimValid = pr.aimValid;
        g_lastAim = pr.aimLocal;
        static XrCompositionLayerQuad* wristLayer;
        wristLayer = tracked ? UpdateWrist(views[0].pose, pr, world && !flatHud && !menuUp) : nullptr;
        if (wristLayer) layers.push_back((XrCompositionLayerBaseHeader*)wristLayer);
        static XrCompositionLayerQuad beam{XR_TYPE_COMPOSITION_LAYER_QUAD}, dot{XR_TYPE_COMPOSITION_LAYER_QUAD};
        // the laser only exists for flat GUI; in the world your hand points through the game's own hover feedback
        static XrCompositionLayerQuad wbeam{XR_TYPE_COMPOSITION_LAYER_QUAD}, wdot{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (tracked && wristLayer && g_wrist.nearTip) {  // the wrist touch ray: fingertip -> where it is over
            const float cyan[4] = {0.2f, 0.8f, 1.0f, 1.0f}, gold[4] = {1.0f, 0.85f, 0.3f, 1.0f}, white[4] = {1, 1, 1, 1};
            Vec3 o = V(g_wrist.tip), p = V(g_wrist.proj);
            Vec3 d = Sub(p, o);
            float len = sqrtf(Dot(d, d));
            if (len > 0.003f) {
                d = Mul(d, 1.0f / len);
                FillSwapchain(g_beamSc, cyan);
                Vec3 c = Add(o, Mul(d, len * 0.5f));
                Vec3 toHead = Sub(V(views[0].pose.position), c);
                Vec3 z = Norm(Sub(toHead, Mul(d, Dot(toHead, d))));
                Vec3 x = Cross(d, z);
                wbeam.space = g_local;
                wbeam.pose.orientation = QuatFromBasis(x, d, z);
                wbeam.pose.position = {c.x, c.y, c.z};
                wbeam.size = {0.003f, len};
                wbeam.subImage.swapchain = g_beamSc.handle;
                wbeam.subImage.imageRect = {{0, 0}, {g_beamSc.w, g_beamSc.h}};
                layers.push_back((XrCompositionLayerBaseHeader*)&wbeam);
            }
            FillSwapchain(g_dotSc, g_wrist.hover ? gold : white);
            Quat wq{g_wrist.pose.orientation.x, g_wrist.pose.orientation.y, g_wrist.pose.orientation.z, g_wrist.pose.orientation.w};
            Vec3 n = qrot(wq, {0, 0, 1});
            Vec3 dp = Add(p, Mul(n, 0.002f));
            wdot.space = g_local;
            wdot.pose.orientation = g_wrist.pose.orientation;
            wdot.pose.position = {dp.x, dp.y, dp.z};
            float ds = 0.008f;
            wdot.size = {ds, ds};
            wdot.subImage.swapchain = g_dotSc.handle;
            wdot.subImage.imageRect = {{0, 0}, {g_dotSc.w, g_dotSc.h}};
            layers.push_back((XrCompositionLayerBaseHeader*)&wdot);
        }
        if (tracked && pr.hitPanel) {
            const float cyan[4] = {0.2f, 0.8f, 1.0f, 1.0f}, white[4] = {1, 1, 1, 1};
            FillSwapchain(g_beamSc, cyan);
            Quat aq{pr.panelAim.orientation.x, pr.panelAim.orientation.y, pr.panelAim.orientation.z, pr.panelAim.orientation.w};
            Vec3 o = V(pr.panelAim.position), d = qrot(aq, {0, 0, -1});
            float len = pr.hitPanel ? pr.dist : 3.0f;
            Vec3 c = Add(o, Mul(d, len * 0.5f));
            Vec3 head = V(views[0].pose.position);
            Vec3 toHead = Sub(head, c);
            Vec3 z = Norm(Sub(toHead, Mul(d, Dot(toHead, d))));  // face the viewer, perpendicular to the beam
            Vec3 x = Cross(d, z);
            beam.space = g_local;
            beam.pose.orientation = QuatFromBasis(x, d, z);
            beam.pose.position = {c.x, c.y, c.z};
            beam.size = {0.004f, len};
            beam.subImage.swapchain = g_beamSc.handle;
            beam.subImage.imageRect = {{0, 0}, {g_beamSc.w, g_beamSc.h}};
            layers.push_back((XrCompositionLayerBaseHeader*)&beam);
            if (pr.hitPanel) {
                FillSwapchain(g_dotSc, white);
                Vec3 hit = Add(o, Mul(d, pr.dist));
                Quat pq{panelPose.orientation.x, panelPose.orientation.y, panelPose.orientation.z, panelPose.orientation.w};
                Vec3 n = qrot(pq, {0, 0, 1});
                Vec3 p = Add(hit, Mul(n, 0.004f));
                dot.space = g_local;
                dot.pose.orientation = panelPose.orientation;
                dot.pose.position = {p.x, p.y, p.z};
                dot.size = {0.015f, 0.015f};
                dot.subImage.swapchain = g_dotSc.handle;
                dot.subImage.imageRect = {{0, 0}, {g_dotSc.w, g_dotSc.h}};
                layers.push_back((XrCompositionLayerBaseHeader*)&dot);
            }
        }
    }

    if (tracked) AddTextLayers(layers, views[0].pose, g_lastAimValid, g_lastAim);
    if (tracked) AddFlashLayer(layers);

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = (uint32_t)layers.size();
    fei.layers = layers.data();
    xrEndFrame(g_session, &fei);
}


static void ResetSwapchain(Swapchain& sc) {
    for (auto* r : sc.rtvs) r->Release();
    if (sc.handle) xrDestroySwapchain(sc.handle);
    sc = Swapchain{};
}

// If the runtime wedges during teardown (seen after Air Link drops), start a fresh host and kill this one.
static volatile LONG g_teardownDone;
static DWORD WINAPI TeardownWatchdog(LPVOID) {
    for (int i = 0; i < 80 && !g_teardownDone; i++) Sleep(100);
    if (g_teardownDone) return 0;
    printf("teardown stuck in the runtime - restarting the host process\n");
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    STARTUPINFOA si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

// Drop everything tied to the OpenXR instance and D3D device so InitXR can start over.
static void Teardown() {
    g_shm->host.running = 0;
    g_teardownDone = 0;
    CloseHandle(CreateThread(nullptr, 0, TeardownWatchdog, nullptr, 0, nullptr));
    ReleaseShared();
    g_sharedGen = 0;
    for (auto& sc : g_eyeSc) ResetSwapchain(sc);
    ResetSwapchain(g_panelSc);
    ResetSwapchain(g_beamSc);
    ResetSwapchain(g_dotSc);
    ResetSwapchain(g_flashSc);
    ResetSwapchain(g_wristSc);
    SafeRelease(g_blendKey);
    g_blendKeyDev = nullptr;
    SafeRelease(g_flashSrv);
    SafeRelease(g_flashTex);
    g_flashDev = nullptr;
    ReleaseText();
    for (auto& p : g_text) ResetSwapchain(p.sc);
    // children before parents: destroying a live session through its instance deadlocks the Meta runtime
    DestroyHandTrackers();
    for (auto& sp : g_aimSpace) {
        if (sp) xrDestroySpace(sp);
        sp = XR_NULL_HANDLE;
    }
    for (auto& sp : g_gripSpace) {
        if (sp) xrDestroySpace(sp);
        sp = XR_NULL_HANDLE;
    }
    if (g_local) xrDestroySpace(g_local);
    if (g_view) xrDestroySpace(g_view);
    g_local = g_view = XR_NULL_HANDLE;
    if (g_actSet) xrDestroyActionSet(g_actSet);
    g_actSet = XR_NULL_HANDLE;
    if (g_session) xrDestroySession(g_session);
    if (g_inst) xrDestroyInstance(g_inst);
    g_inst = XR_NULL_HANDLE;
    g_session = XR_NULL_HANDLE;
    g_sessionRunning = false;
    g_haveOrigin = false;
    g_prevButtons = 0;
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_samp);
    SafeRelease(g_cb);
    MirrorRelease();
    RecRelease();
    SafeRelease(g_ctx);
    SafeRelease(g_dev);
    g_teardownDone = 1;
}

static BOOL WINAPI OnConsoleClose(DWORD) {
    // Ctrl+C or the terminal tab closing: tell the game the host is gone so it stops exporting
    if (g_shm) g_shm->host.running = 0;
    g_quit = true;
    RecStop();  // finish the recording's file before the console goes away
    Sleep(200);
    return FALSE;
}

// --preview <text file> <out.bmp> [pickable 0|1] [game dir]: draw one text panel offscreen (no headset) and dump
// it, to check styling and icons. The text file is the panel text (UTF-8, '\n' lines, '#' title, "@icon|name").
static int Preview(int argc, char** argv) {
    if (argc < 4) { printf("usage: kvr_host --preview <text.txt> <out.bmp> [pickable] [game dir]\n"); return 2; }
    static KvrShared shm{};
    g_shm = &shm;
    strcpy_s(shm.gameDir, argc > 5 ? argv[5] : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\swkotor");
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                 D3D11_SDK_VERSION, &dev, nullptr, &g_ctx))) return 1;
    dev->QueryInterface(IID_PPV_ARGS(&g_dev));
    dev->Release();
    g_scFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!InitText()) { printf("InitText failed\n"); return 1; }
    KvrTextPanel src{};
    FILE* f = nullptr;
    if (fopen_s(&f, argv[2], "rb") || !f) return 1;
    size_t n = fread(src.text, 1, sizeof src.text - 1, f);
    fclose(f);
    src.text[n] = 0;
    src.pickable = argc > 4 ? atoi(argv[4]) : 1;
    src.highlight = -1;
    DrawTextPanel(g_text[0], src, 2);  // line 2 hovered
    DumpTex(g_text[0].tex, argv[3]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--preview")) return Preview(argc, argv);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleCtrlHandler(OnConsoleClose, TRUE);
    if (!OpenShared()) { printf("shared memory failed\n"); return 1; }
    while (!g_quit) {
    if (!InitXR()) {
        printf("OpenXR init failed; retrying in 3 s\n");
        Teardown();
        Sleep(3000);
        continue;
    }
    printf("kvr_host running. In game: F8 recenter, F9 toggle panel. Ctrl+C to quit.\n");
    g_lost = false;
    DWORD lastReport = GetTickCount();
    uint32_t lastAlive = 0, lastE0 = 0, lastE1 = 0, xrFrames = 0;
    while (!g_quit && !g_lost) {
        MirrorPump();
        PollEvents();
        if (!g_sessionRunning) { Sleep(10); continue; }
        Frame();
        xrFrames++;
        DWORD now = GetTickCount();
        if (now - lastReport >= 5000) {
            float s = (now - lastReport) / 1000.0f;
            printf("xr %.0f fps | game %.0f fps | eye0 %.0f/s eye1 %.0f/s | panel %u\n", xrFrames / s,
                   (g_shm->gameAlive - lastAlive) / s, (g_shm->eyes[0].frameId - lastE0) / s, (g_shm->eyes[1].frameId - lastE1) / s,
                   g_shm->panel.frameId);
            lastAlive = g_shm->gameAlive; lastE0 = g_shm->eyes[0].frameId; lastE1 = g_shm->eyes[1].frameId;
            xrFrames = 0;
            lastReport = now;
        }
    }
    if (g_lost && !g_quit) {
        printf("OpenXR session lost (Air Link dropped?) - reconnecting\n");
        Teardown();
        Sleep(2000);
    }
    }
    RecStop();
    g_shm->host.running = 0;
    return 0;
}
