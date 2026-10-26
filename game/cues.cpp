// Combat feedback cues: haptics (via the host), weapon glow, and KotOR's own sounds. See cues.h.
//
// Sounds are KotOR's combat / GUI effects (saber clash, parries, swing whooshes, the action-queue blip), exported from
// the player's own game files to <game>\kvr\sounds, so they sit in the game's mix. They play through XAudio2 at the
// game's "Sound Effects Volume" (swkotor.ini) times a per-cue gain. Which sounds play is set in kvr.ini [Sounds]
// (comma-separated resrefs, one picked at random; empty = silent).
#include "cues.h"

#include <windows.h>
#include <xaudio2.h>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "features.h"
#include "vr.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

// ---------------------------------------------------------------- sound
enum Family { FAM_SABER, FAM_BLADE, FAM_HEAVY, FAM_UNARMED, FAM_RANGED, FAM_COUNT };
const char* kFamName[FAM_COUNT] = {"Saber", "Blade", "Heavy", "Unarmed", "Ranged"};

Family FamilyOf(WeaponClass c) {
    switch (c) {
        case WC_SABER: case WC_SABER_SHORT: case WC_SABER_DOUBLE: return FAM_SABER;
        case WC_BLADE: case WC_BLADE_SHORT: case WC_BATON: return FAM_BLADE;
        case WC_BLADE_DOUBLE: case WC_STAFF: return FAM_HEAVY;
        case WC_PISTOL: case WC_RIFLE: case WC_REPEATER: return FAM_RANGED;
        default: return FAM_UNARMED;
    }
}

struct CueSound {
    const char* key;                     // kvr.ini [Sounds] key prefix
    bool perFamily;                      // key is <key><Family>
    const char* defaults[FAM_COUNT];     // default resrefs per family (or [0] for all)
    float gain;
};

// Defaults: parries for blocks (KotOR's own cb_pa_* parry sounds, saber clash for sabers), the weapon's swing whoosh
// for a perfectly timed attack, the action-queue blip for the attack window, the GUI prompt for an incoming attack.
const CueSound kSounds[CUE_COUNT] = {
    {"Block", true, {"cb_ls_clash1,cb_ls_clash2", "cb_pa_blade1", "cb_pa_bldlrg1", "cb_pa_unarm1", ""}, 0.9f},
    {"PerfectBlock", true, {"cb_ls_clash1,cb_ls_clash2", "cb_pa_blade1", "cb_pa_bldlrg1", "cb_pa_unarm1", ""}, 1.0f},
    {"Dodge", false, {""}, 0.6f},
    {"BlockFailed", false, {""}, 0.6f},
    {"PerfectHit", true, {"cb_ls_swinglong1,cb_ls_swinglong2,cb_ls_swinglong3", "cb_sw_blade1", "cb_sw_bldlrg1",
                          "cb_sw_unarmed1", ""}, 0.6f},
    {"WindowOpen", false, {"gui_actqueue"}, 0.3f},
    {"Incoming", false, {"gui_prompt"}, 0.45f},
};

struct Wav {
    WAVEFORMATEX fmt{};
    std::vector<BYTE> data;
};

char g_dir[MAX_PATH];
char g_ini[MAX_PATH];
float g_master = 0.85f;
IXAudio2* g_xa;
IXAudio2MasteringVoice* g_master_voice;
std::map<std::string, Wav> g_wavs;
std::vector<std::string> g_choices[CUE_COUNT][FAM_COUNT];

bool LoadWav(const std::string& resref, Wav& out) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\kvr\\sounds\\%s.wav", g_dir, resref.c_str());
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return false;
    std::vector<BYTE> d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return false;
    size_t o = 12;
    bool haveFmt = false;
    while (o + 8 <= d.size()) {
        DWORD len = *(DWORD*)&d[o + 4];
        if (!memcmp(&d[o], "fmt ", 4) && len >= 16) {
            memcpy(&out.fmt, &d[o + 8], 16);
            out.fmt.cbSize = 0;
            haveFmt = true;
        } else if (!memcmp(&d[o], "data", 4)) {
            size_t n = min((size_t)len, d.size() - o - 8);
            out.data.assign(d.begin() + o + 8, d.begin() + o + 8 + n);
        }
        o += 8 + len + (len & 1);
    }
    return haveFmt && out.fmt.wFormatTag == WAVE_FORMAT_PCM && !out.data.empty();
}

void InitAudio() {
    HMODULE xa = LoadLibraryA("xaudio2_9.dll");
    typedef HRESULT(WINAPI * Create_t)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);
    Create_t create = xa ? (Create_t)GetProcAddress(xa, "XAudio2Create") : nullptr;
    if (!create || FAILED(create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(g_xa->CreateMasteringVoice(&g_master_voice))) {
        Log("cues: XAudio2 unavailable, cue sounds off");
        g_xa = nullptr;
        return;
    }
    // follow the game's own effects volume so the cues sit in its mix
    char gi[MAX_PATH];
    sprintf_s(gi, "%s\\swkotor.ini", g_dir);
    g_master = GetPrivateProfileIntA("Sound Options", "Sound Effects Volume", 85, gi) / 100.0f;

    int loaded = 0, missing = 0;
    for (int e = 0; e < CUE_COUNT; e++) {
        for (int fam = 0; fam < FAM_COUNT; fam++) {
            if (!kSounds[e].perFamily && fam > 0) break;
            char key[64], val[512];
            if (kSounds[e].perFamily) sprintf_s(key, "%s%s", kSounds[e].key, kFamName[fam]);
            else strcpy_s(key, kSounds[e].key);
            const char* def = kSounds[e].defaults[kSounds[e].perFamily ? fam : 0];
            GetPrivateProfileStringA("Sounds", key, def ? def : "", val, sizeof val, g_ini);
            for (char* t = strtok(val, ", "); t; t = strtok(nullptr, ", ")) {
                std::string r(t);
                if (!g_wavs.count(r)) {
                    Wav w;
                    if (!LoadWav(r, w)) {
                        missing++;
                        continue;
                    }
                    g_wavs[r] = std::move(w);
                    loaded++;
                }
                g_choices[e][fam].push_back(r);
            }
        }
    }
    Log("cues: audio up, effects volume %.2f, %d sounds loaded, %d missing (run tools\\sound_export.py)", g_master, loaded,
        missing);
}

void PlaySoundFor(CueEvent e) {
    if (!g_xa) return;
    Family fam = kSounds[e].perFamily ? FamilyOf(Weapon_Right()) : FAM_SABER;
    const auto& list = g_choices[e][kSounds[e].perFamily ? fam : 0];
    if (list.empty()) return;
    const Wav& w = g_wavs[list[rand() % list.size()]];
    IXAudio2SourceVoice* v = nullptr;
    if (FAILED(g_xa->CreateSourceVoice(&v, &w.fmt))) return;
    XAUDIO2_BUFFER b{};
    b.AudioBytes = (UINT32)w.data.size();
    b.pAudioData = w.data.data();
    b.Flags = XAUDIO2_END_OF_STREAM;
    v->SetVolume(g_master * kSounds[e].gain);
    v->SubmitSourceBuffer(&b);
    v->Start();
    // reap finished voices (one-shots are short; a handful alive at most)
    static std::vector<IXAudio2SourceVoice*> live;
    for (size_t i = 0; i < live.size();) {
        XAUDIO2_VOICE_STATE st;
        live[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) {
            live[i]->DestroyVoice();
            live[i] = live.back();
            live.pop_back();
        } else {
            i++;
        }
    }
    live.push_back(v);
}

// ---------------------------------------------------------------- haptics
struct Pulse {
    double at;
    int hand;
    float amp, dur, freq;
};
std::vector<Pulse> g_pulses;

void Haptic(double delay, int hand, float amp, float dur, float freq) {
    bool both = Weapon_Profile(Weapon_Right()).twoHanded;
    for (int h = 0; h < 2; h++)
        if (h == hand || (hand == 1 && both) || hand == 2) g_pulses.push_back({Now() + delay, h, amp, dur, freq});
}

// ---------------------------------------------------------------- glow
float g_flash[2];
double g_flashAt[2] = {-10, -10};
bool g_window;

void Flash(float v) {
    for (int h = 0; h < 2; h++) {
        g_flash[h] = v;
        g_flashAt[h] = Now();
    }
}

}  // namespace

void Cues_Init(const char* gameDir) {
    strcpy_s(g_dir, gameDir);
    sprintf_s(g_ini, "%s\\kvr\\kvr.ini", gameDir);
    if (!Features().cues) return;
    InitAudio();
}

void Cues_Fire(CueEvent e) {
    if (!Features().cues) return;
    static const char* names[CUE_COUNT] = {"block", "perfect block", "dodge", "block failed", "perfect hit",
                                           "window open", "incoming_warning"};
    if (e != CUE_WINDOW_OPEN) Log("cue: %s", names[e]);
    PlaySoundFor(e);
    switch (e) {  // haptic: hand 1 = weapon hand (both for two-handed weapons), 2 = both hands
        case CUE_PERFECT_BLOCK: Haptic(0, 1, 1.0f, 0.08f, 320); Flash(1.0f); break;
        case CUE_BLOCK: Haptic(0, 1, 0.8f, 0.06f, 250); Flash(0.7f); break;
        case CUE_DODGE: Haptic(0, 2, 0.5f, 0.06f, 200); break;
        case CUE_BLOCK_FAILED: Haptic(0, 1, 0.45f, 0.05f, 90); Haptic(0.12, 1, 0.45f, 0.05f, 90); break;
        case CUE_PERFECT_HIT: Haptic(0, 1, 1.0f, 0.12f, 100); Flash(0.8f); break;
        case CUE_WINDOW_OPEN: Haptic(0, 1, 0.15f, 0.02f, 200); break;
        case CUE_INCOMING: Haptic(0, 2, 0.3f, 0.04f, 160); break;
        default: break;
    }
}

void Cues_Update() {
    double t = Now();
    for (size_t i = 0; i < g_pulses.size();) {
        if (g_pulses[i].at <= t) {
            VR_Haptic(g_pulses[i].hand, g_pulses[i].amp, g_pulses[i].dur, g_pulses[i].freq);
            g_pulses[i] = g_pulses.back();
            g_pulses.pop_back();
        } else {
            i++;
        }
    }
}

float Cues_Glow(int hand) {
    if (!Features().cues || hand < 0 || hand > 1) return 0;
    double t = Now();
    float flash = g_flash[hand] * (float)max(0.0, 1.0 - (t - g_flashAt[hand]) / 0.3);
    float pulse = g_window ? 0.12f + 0.1f * (float)sin(t * 2 * 3.14159 * 4) : 0;
    return max(flash, pulse);
}

void Cues_SetWindow(bool open) { g_window = open; }
