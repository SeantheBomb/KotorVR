// kvr_host: x64 OpenXR compositor bridge for the KotOR VR layer.
// Owns the OpenXR session (D3D11). Publishes head/eye poses to shared memory, and each XR frame
// copies the newest eye images the game produced into the swapchains, submitted with the pose
// each eye was actually rendered with. Shows the full game frame on a quad panel for menus.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

#include "../common/kvr_shared.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
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
cbuffer P : register(b0) { float2 uvScale; float decodeSrgb; float guiKey; };
Texture2D t : register(t0);
SamplerState s : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    float2 c = float2((id << 1) & 2, id & 2);
    pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);
    uv = float2(c.x, 1 - c.y) * uvScale;   // GL images are bottom-up
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = t.Sample(s, uv).rgb;
    // GUI-only frames are drawn over black: key black out so the HUD floats over the stereo world
    float a = guiKey > 0.5 ? saturate(max(c.r, max(c.g, c.b)) * 4.0) : 1.0;
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
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &g_ctx);
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
    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
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

static bool InitXR() {
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "KotorVR");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
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
static XrAction a_move, a_turn, a_trig, a_grip, a_aim, a_btnA, a_btnB, a_btnX, a_btnY, a_menu, a_stickClick;
static XrPath g_handPath[2];
static XrSpace g_aimSpace[2];
static Swapchain g_beamSc, g_dotSc;
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
              MakeAction(a_btnA, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_btnB, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_btnX, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_btnY, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false) &&
              MakeAction(a_stickClick, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
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
        {a_btnA, MakePath("/user/hand/right/input/a/click")},
        {a_btnB, MakePath("/user/hand/right/input/b/click")},
        {a_btnX, MakePath("/user/hand/left/input/x/click")},
        {a_btnY, MakePath("/user/hand/left/input/y/click")},
        {a_menu, MakePath("/user/hand/left/input/menu/click")},
        {a_stickClick, MakePath("/user/hand/left/input/thumbstick/click")},
        {a_stickClick, MakePath("/user/hand/right/input/thumbstick/click")},
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
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &g_actSet;
    CHECK_XR(xrAttachSessionActionSets(g_session, &ai));

    if (!CreateSwapchain(g_beamSc, 4, 4) || !CreateSwapchain(g_dotSc, 16, 16)) return false;
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
};

// Sync controllers, publish them to the game, and work out where the right-hand laser points.
// panelPose/panelW/panelH describe the panel quad in LOCAL space when it is visible.
static bool HudSolidAt(float u, float v);

static PointerResult UpdateControls(XrTime t, bool panelVisible, const XrPosef& panelPose, float panelW, float panelH, bool world, bool hud) {
    PointerResult pr;
    XrActiveActionSet aas{g_actSet, XR_NULL_PATH};
    XrActionsSyncInfo syn{XR_TYPE_ACTIONS_SYNC_INFO};
    syn.countActiveActionSets = 1;
    syn.activeActionSets = &aas;
    bool synced = XR_SUCCEEDED(xrSyncActions(g_session, &syn));

    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (synced && XR_SUCCEEDED(xrLocateSpace(g_aimSpace[1], g_local, t, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        pr.aimValid = true;
        pr.aimLocal = loc.pose;
    }
    if (pr.aimValid && panelVisible) {
        XrPosef inv = PoseInv(panelPose);
        XrPosef o = PoseMul(inv, pr.aimLocal);  // aim in panel space
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
    if (pr.hitPanel) {
        in.pointerMode = KVR_POINTER_PANEL;
        in.panelU = pr.u;
        in.panelV = pr.v;
    } else if (pr.aimValid && world) {
        in.pointerMode = KVR_POINTER_WORLD;
        XrPosef rel = PoseMul(PoseInv(g_origin), pr.aimLocal);
        memcpy(in.aim.pos, &rel.position, 12);
        memcpy(in.aim.quat, &rel.orientation, 16);
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

// Draw a pulled texture into the acquired swapchain image.
static void Blit(Swapchain& sc, uint32_t imageIndex, SharedTex& src, uint32_t validW, uint32_t validH, bool guiKey = false) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* p = (float*)m.pData;
        p[0] = (float)validW / g_sharedW;
        p[1] = (float)validH / g_sharedH;
        p[2] = g_scSrgb ? 1.0f : 0.0f;
        p[3] = guiKey ? 1.0f : 0.0f;
        g_ctx->Unmap(g_cb, 0);
    }
    D3D11_VIEWPORT vp{0, 0, (float)sc.w, (float)sc.h, 0, 1};
    g_ctx->OMSetRenderTargets(1, &sc.rtvs[imageIndex], nullptr);
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
        bool world = g_shared[0].have && g_shared[1].have && (alive - g_shm->lastWorldFrame) < 30;
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
        bool panelVisible = !world || (hud && !g_shm->showPanel);
        static DWORD lastHudLog;
        if (GetTickCount() - lastHudLog > 3000) {
            lastHudLog = GetTickCount();
            printf("hud: world=%d hud=%d panelVisible=%d panelFlags=0x%x panelHave=%d hidden(showPanel)=%u cinematic=%u\n", world, hud,
                   panelVisible, g_shared[2].meta.flags, g_shared[2].have, g_shm->showPanel, g_shm->cinematic);
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
        XrPosef panelPose = PoseMul(g_origin, panelLocal);
        float panelW = world ? 2.0f : 2.4f, panelH = panelW * 0.75f;
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

        // controllers: publish state, then draw the right-hand laser and the panel cursor
        PointerResult pr = UpdateControls(fs.predictedDisplayTime, panelVisible, panelPose, panelW, panelH, world, hud);
        static XrCompositionLayerQuad beam{XR_TYPE_COMPOSITION_LAYER_QUAD}, dot{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (pr.aimValid && tracked) {
            const float cyan[4] = {0.2f, 0.8f, 1.0f, 1.0f}, white[4] = {1, 1, 1, 1};
            FillSwapchain(g_beamSc, cyan);
            Quat aq{pr.aimLocal.orientation.x, pr.aimLocal.orientation.y, pr.aimLocal.orientation.z, pr.aimLocal.orientation.w};
            Vec3 o = V(pr.aimLocal.position), d = qrot(aq, {0, 0, -1});
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

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = (uint32_t)layers.size();
    fei.layers = layers.data();
    xrEndFrame(g_session, &fei);
}

template <class T> static void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
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
    // children before parents: destroying a live session through its instance deadlocks the Meta runtime
    for (auto& sp : g_aimSpace) {
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
    SafeRelease(g_ctx);
    SafeRelease(g_dev);
    g_teardownDone = 1;
}

static BOOL WINAPI OnConsoleClose(DWORD) {
    // Ctrl+C or the terminal tab closing: tell the game the host is gone so it stops exporting
    if (g_shm) g_shm->host.running = 0;
    g_quit = true;
    Sleep(200);
    return FALSE;
}

int main() {
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
    g_shm->host.running = 0;
    return 0;
}
