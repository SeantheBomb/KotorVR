// Camera-yaw finder (development tool).
//
// To drive the game camera from the headset we need the field that holds its heading. Rather than reverse it
// statically, snapshot every writable value that could encode the current camera yaw, then keep only those that
// follow the yaw as the player turns with the stick, then nudge each survivor to see which one the camera obeys.
//
// Encodings tracked:
//   angle:  v = s*yaw + k*pi/2 (radians) or the same in degrees, s = +-1, k = 0..3
//   pair:   two adjacent floats of unit length (direction vectors, yaw-only quaternions). Their own angle
//           theta = atan2(b, a) must move by m * dyaw, with m in {1, -1, 0.5, -0.5} learned on the first turn.
//
// Scanning and filtering run on a worker thread; a sample is only taken once the heading has been steady for
// 300 ms, so the real field is guaranteed to hold the sampled value.

#include "scan.h"

#include <windows.h>
#include <cmath>
#include <vector>

extern void Log(const char* fmt, ...);

namespace {

const float kPi = 3.14159265f;
float Wrap(float a) { return remainderf(a, 2 * kPi); }

enum Kind : unsigned char { K_RAD, K_DEG, K_PAIR };
struct Cand {
    float* addr;
    Kind kind;
    signed char sign;   // angle: +-1
    unsigned char k;    // angle: quarter-turn offset
    float mult;         // pair: theta change per yaw change (0 = not learned yet)
    float theta;        // pair: angle at the last sample
};

std::vector<Cand> g_cands;  // worker-owned
volatile LONG g_sampleSeq;
volatile float g_sampleYaw;
volatile float g_liveYaw;
volatile LONG g_done;
HANDLE g_worker;

// game-thread state
DWORD g_firstGameplay, g_steadySince;
float g_steadyYaw = 1000.0f, g_lastSampled = 1000.0f;

// write test
std::vector<Cand> g_testList;
volatile LONG g_testPhase;  // 0 idle, 1 testing, 2 finished
size_t g_testIdx;
int g_testWait;
float g_testBefore;

bool AngleRel(float v, float yaw, bool deg, signed char& s, unsigned char& k) {
    if (v == 0.0f || !std::isfinite(v)) return false;
    float r = deg ? v * kPi / 180.0f : v;
    if (deg ? (fabsf(v) > 720.0f || fabsf(v) < 1.0f) : fabsf(v) > 7.0f) return false;
    for (int sg = -1; sg <= 1; sg += 2)
        for (int q = 0; q < 4; q++)
            if (fabsf(Wrap(r - (sg * yaw + q * kPi / 2))) < (deg ? 0.01f : 0.005f)) {
                s = (signed char)sg;
                k = (unsigned char)q;
                return true;
            }
    return false;
}

bool IsUnitPair(const float* p) {
    float a = p[0], b = p[1];
    if (!std::isfinite(a) || !std::isfinite(b)) return false;
    if (fabsf(a) < 0.05f || fabsf(b) < 0.05f) return false;  // (0,1)-style pairs are everywhere
    return fabsf(a * a + b * b - 1.0f) < 0.002f;
}

void ScanRegion(BYTE* base, SIZE_T size, float yaw) {
    __try {
        float* p = (float*)base;
        size_t n = size / 4;
        for (size_t i = 0; i + 1 < n; i++) {
            Cand c{p + i};
            if (AngleRel(p[i], yaw, false, c.sign, c.k)) { c.kind = K_RAD; g_cands.push_back(c); continue; }
            if (AngleRel(p[i], yaw, true, c.sign, c.k)) { c.kind = K_DEG; g_cands.push_back(c); continue; }
            if (IsUnitPair(p + i)) {
                c.kind = K_PAIR;
                c.theta = atan2f(p[i + 1], p[i]);
                g_cands.push_back(c);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void FullScan(float yaw) {
    SIZE_T scanned = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (BYTE* a = (BYTE*)0x10000; a < (BYTE*)0x7FFE0000; a = (BYTE*)mbi.BaseAddress + mbi.RegionSize) {
        if (!VirtualQuery(a, &mbi, sizeof mbi)) break;
        bool rw = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD);
        bool sysImage = mbi.Type == MEM_IMAGE && (ULONG_PTR)mbi.AllocationBase != 0x400000;
        if (rw && !sysImage && mbi.RegionSize < (256u << 20)) {
            ScanRegion((BYTE*)mbi.BaseAddress, mbi.RegionSize, yaw);
            scanned += mbi.RegionSize;
        }
    }
    Log("yawscan: full scan at yaw %.4f rad: %zu MB scanned, %zu candidates", yaw, scanned >> 20, g_cands.size());
}

// Does the candidate still agree with `yaw`? dyaw is the heading change since the previous sample.
bool Keep(Cand& c, float yaw, float dyaw) {
    __try {
        if (c.kind == K_RAD || c.kind == K_DEG) {
            signed char s;
            unsigned char k;
            return AngleRel(*c.addr, yaw, c.kind == K_DEG, s, k) && s == c.sign && k == c.k;
        }
        if (!IsUnitPair(c.addr)) return false;
        float th = atan2f(c.addr[1], c.addr[0]);
        float d = Wrap(th - c.theta);
        if (c.mult == 0.0f) {
            const float mults[4] = {1.0f, -1.0f, 0.5f, -0.5f};
            for (float m : mults)
                if (fabsf(Wrap(d - m * dyaw)) < 0.01f) {
                    c.mult = m;
                    c.theta = th;
                    return true;
                }
            return false;
        }
        if (fabsf(Wrap(d - c.mult * dyaw)) >= 0.01f) return false;
        c.theta = th;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

const char* Describe(const Cand& c, char* buf, size_t n) {
    if (c.kind == K_PAIR) sprintf_s(buf, n, "pair x%.1f (%.4f, %.4f)", c.mult, c.addr[0], c.addr[1]);
    else sprintf_s(buf, n, "%s %+d*yaw%+d*90 = %.4f", c.kind == K_RAD ? "rad" : "deg", c.sign, c.k, *c.addr);
    return buf;
}

DWORD WINAPI Worker(LPVOID) {
    LONG seen = 0;
    bool scanned = false;
    int filters = 0;
    float prevYaw = 0;
    while (!g_done) {
        LONG seq = g_sampleSeq;
        if (seq == seen) { Sleep(20); continue; }
        seen = seq;
        float yaw = g_sampleYaw;
        if (fabsf(Wrap(g_liveYaw - yaw)) > 0.005f) continue;  // turned again before we got here
        if (!scanned) {
            FullScan(yaw);
            scanned = true;
            prevYaw = yaw;
            continue;
        }
        float dyaw = Wrap(yaw - prevYaw);
        size_t before = g_cands.size();
        std::vector<Cand> keep;
        keep.reserve(before);
        for (Cand& c : g_cands)
            if (Keep(c, yaw, dyaw)) keep.push_back(c);
        if (fabsf(Wrap(g_liveYaw - yaw)) > 0.005f) { Log("yawscan: heading moved during filter, discarded"); continue; }
        g_cands.swap(keep);
        prevYaw = yaw;
        filters++;
        Log("yawscan: filter %d at yaw %.4f (d %.3f): %zu -> %zu", filters, yaw, dyaw, before, g_cands.size());
        if (g_cands.size() <= 40 && filters >= 4) {
            char buf[96];
            for (const Cand& c : g_cands) Log("yawscan: survivor %p %s", c.addr, Describe(c, buf, sizeof buf));
            g_testList.clear();
            MEMORY_BASIC_INFORMATION self{};
            for (const Cand& c : g_cands) {
                bool stack = (ULONG_PTR)c.addr > (ULONG_PTR)&self - 0x100000 && (ULONG_PTR)c.addr < (ULONG_PTR)&self + 0x100000;
                if (!stack) g_testList.push_back(c);
            }
            Log("yawscan: filtering done, write-testing %zu candidates", g_testList.size());
            g_testPhase = 1;
            g_done = 1;
        }
    }
    return 0;
}

// Log statics in the exe image that point at (or just before) the object holding `field`: a stable path to it.
void FindPointerPaths(float* field) {
    ULONG_PTR f = (ULONG_PTR)field;
    const DWORD* p = (const DWORD*)0x78D000;  // exe .data .. end of image
    const DWORD* end = (const DWORD*)(0x400000 + 0x4C3000);
    int found = 0;
    __try {
        for (; p < end && found < 20; p++) {
            DWORD v = *p;
            if (v <= f && f - v < 0x2000) {
                Log("yawscan: static %p -> %08lX (+0x%lX to field)", p, v, (unsigned long)(f - v));
                found++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!found) Log("yawscan: no direct static pointer to %p's object; needs a two-level search", field);
}

void Nudge(const Cand& c, float by) {
    __try {
        if (c.kind == K_PAIR) {
            float th = atan2f(c.addr[1], c.addr[0]) + c.mult * by;
            c.addr[0] = cosf(th);
            c.addr[1] = sinf(th);
        } else if (c.kind == K_RAD) {
            *c.addr += c.sign * by;
        } else {
            *c.addr += c.sign * by * 180.0f / kPi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

}  // namespace

void Scan_OnGameplayCamera(const float R[16]) {
    // camera forward in world = -(row 2 of the world->camera rotation); world is Z-up
    float yaw = atan2f(-R[6], -R[2]);
    if (g_testPhase == 1) {
        if (g_testIdx >= g_testList.size()) {
            Log("yawscan: write test finished");
            g_testPhase = 2;
            return;
        }
        const Cand& c = g_testList[g_testIdx];
        if (g_testWait == 0) {
            g_testBefore = yaw;
            Nudge(c, 0.5f);
            g_testWait = 1;
        } else if (++g_testWait >= 4) {
            float moved = Wrap(yaw - g_testBefore);
            bool drives = fabsf(moved - 0.5f) < 0.1f || fabsf(moved + 0.5f) < 0.1f;
            char buf[96];
            Log("yawscan: write test %p %s: camera moved %.3f rad -> %s", c.addr, Describe(c, buf, sizeof buf), moved,
                drives ? "DRIVES THE CAMERA" : "derived/ignored");
            if (drives) FindPointerPaths(c.addr);
            g_testWait = 0;
            g_testIdx++;
        }
        return;
    }
    if (g_done) return;
    g_liveYaw = yaw;
    DWORD now = GetTickCount();
    if (!g_firstGameplay) g_firstGameplay = now;
    if (now - g_firstGameplay < 10000) return;
    if (!g_worker) g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (fabsf(Wrap(yaw - g_steadyYaw)) > 0.002f) {
        g_steadyYaw = yaw;
        g_steadySince = now;
        return;
    }
    bool distinct = fabsf(Wrap(yaw - g_lastSampled)) > 0.15f;
    bool usable = fabsf(sinf(yaw)) > 0.1f && fabsf(cosf(yaw)) > 0.1f;
    if (now - g_steadySince > 300 && distinct && usable) {
        g_sampleYaw = yaw;
        g_lastSampled = yaw;
        InterlockedIncrement(&g_sampleSeq);
    }
}
