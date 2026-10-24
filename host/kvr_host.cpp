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
    if (!existed || g_shm->magic != KVR_MAGIC) {
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
cbuffer P : register(b0) { float2 uvScale; float decodeSrgb; float pad; };
Texture2D t : register(t0);
SamplerState s : register(s0);
void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    float2 c = float2((id << 1) & 2, id & 2);
    pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);
    uv = float2(c.x, 1 - c.y) * uvScale;   // GL images are bottom-up
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 c = t.Sample(s, uv).rgb;
    if (decodeSrgb > 0.5) c = pow(abs(c), 2.2);
    return float4(c, 1);
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
static bool g_sessionRunning, g_quit;
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
        Sleep(1000);
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
    return true;
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
                g_quit = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_quit = true;
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
static void Blit(Swapchain& sc, uint32_t imageIndex, SharedTex& src, uint32_t validW, uint32_t validH) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* p = (float*)m.pData;
        p[0] = (float)validW / g_sharedW;
        p[1] = (float)validH / g_sharedH;
        p[2] = g_scSrgb ? 1.0f : 0.0f;
        p[3] = 0;
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
        if (!world || g_shm->showPanel) {
            const KvrEyeSlot& slot = g_shared[2].meta;
            uint32_t idx = AcquireImage(g_panelSc);
            if (g_shared[2].have) Blit(g_panelSc, idx, g_shared[2], slot.width, slot.height);
            else ClearSwapchain(g_panelSc, idx);
            static DWORD lastDump;
            if (g_shared[2].have && GetTickCount() - lastDump > 10000) {
                lastDump = GetTickCount();
                DumpTex(g_shared[2].local, "C:\\Users\\SeanF\\Documents\\KotorVR\\traces\\host_panel_src.bmp");
                DumpTex(g_panelSc.images[idx].texture, "C:\\Users\\SeanF\\Documents\\KotorVR\\traces\\host_panel_swap.bmp");
            }
            ReleaseImage(g_panelSc);
            // theater screen 2.4 m ahead when there is no world; smaller and low when overlaid on the world
            XrPosef local = world ? XrPosef{{-0.2588f, 0, 0, 0.9659f}, {0, -0.55f, -1.2f}}   // pitched 30 deg toward the viewer
                                  : XrPosef{{0, 0, 0, 1}, {0, 0, -2.4f}};
            quad.space = g_local;
            quad.pose = PoseMul(g_origin, local);
            float wM = world ? 0.8f : 2.4f;
            quad.size = {wM, wM * 0.75f};
            quad.subImage.swapchain = g_panelSc.handle;
            quad.subImage.imageRect = {{0, 0}, {g_panelSc.w, g_panelSc.h}};
            layers.push_back((XrCompositionLayerBaseHeader*)&quad);
        }
    }

    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = (uint32_t)layers.size();
    fei.layers = layers.data();
    xrEndFrame(g_session, &fei);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!OpenShared()) { printf("shared memory failed\n"); return 1; }
    if (!InitXR()) { printf("OpenXR init failed\n"); return 1; }
    printf("kvr_host running. In game: F8 recenter, F9 toggle panel. Ctrl+C to quit.\n");
    DWORD lastReport = GetTickCount();
    uint32_t lastAlive = 0, lastE0 = 0, lastE1 = 0, xrFrames = 0;
    while (!g_quit) {
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
    g_shm->host.running = 0;
    return 0;
}
