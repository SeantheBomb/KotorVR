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
    {"Block", true, {"cb_ls_clash1,cb_ls_clash2", "cb_pa_blade1", "cb_pa_bldlrg1", "cb_pa_unarm1", ""}, 1.7f},
    {"PerfectBlock", true, {"cb_ls_clash1,cb_ls_clash2", "cb_pa_blade1", "cb_pa_bldlrg1", "cb_pa_unarm1", ""}, 2.0f},
    {"Dodge", false, {""}, 0.6f},
    {"BlockFailed", false, {""}, 0.6f},
    {"PerfectHit", true, {"cb_ls_swinglong1,cb_ls_swinglong2,cb_ls_swinglong3", "cb_sw_blade1", "cb_sw_bldlrg1",
                          "cb_sw_unarmed1", ""}, 0.9f},
    {"WindowOpen", false, {"gui_actqueue"}, 0.5f},
    {"Incoming", false, {"gui_prompt"}, 0.8f},
    {"HitLanded", true, {"cb_ls_hithard1,cb_ls_hithard2", "", "", "", ""}, 0.7f},  // a saber sizzle over KotOR's own impact
    {"PerfectLanded", true, {"cb_ls_hithard1,cb_ls_hithard2", "", "", "", ""}, 1.0f},
    {"Force", false, {""}, 1.0f},  // KotOR plays the power's own sound
    {"Hurt", false, {""}, 1.0f},   // KotOR plays the character's own pain grunt
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

// finished one-shot voices are destroyed on the next play (they are short; a handful alive at most)
std::vector<IXAudio2SourceVoice*> g_oneShots;
void ReapOneShots() {
    for (size_t i = 0; i < g_oneShots.size();) {
        XAUDIO2_VOICE_STATE st;
        g_oneShots[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) {
            g_oneShots[i]->DestroyVoice();
            g_oneShots[i] = g_oneShots.back();
            g_oneShots.pop_back();
        } else {
            i++;
        }
    }
}

// a sound by resref (loaded on first use from kvr\sounds)
IXAudio2SourceVoice* PlayResref(const char* resref, float gain, float ratio = 1.0f, bool loop = false) {
    if (!g_xa) return nullptr;
    auto it = g_wavs.find(resref);
    if (it == g_wavs.end()) {
        Wav w;
        if (!LoadWav(resref, w)) {
            static int warned;
            if (warned++ < 5) Log("cues: sound %s missing (run tools/sound_export.py)", resref);
            g_wavs[resref] = Wav();
            return nullptr;
        }
        it = g_wavs.emplace(resref, std::move(w)).first;
    }
    const Wav& w = it->second;
    if (w.data.empty()) return nullptr;
    IXAudio2SourceVoice* v = nullptr;
    if (FAILED(g_xa->CreateSourceVoice(&v, &w.fmt))) return nullptr;
    XAUDIO2_BUFFER b{};
    b.AudioBytes = (UINT32)w.data.size();
    b.pAudioData = w.data.data();
    b.Flags = XAUDIO2_END_OF_STREAM;
    if (loop) b.LoopCount = XAUDIO2_LOOP_INFINITE;
    v->SetVolume(g_master * gain);
    v->SetFrequencyRatio(ratio);
    v->SubmitSourceBuffer(&b);
    v->Start();
    if (!loop) {
        ReapOneShots();
        g_oneShots.push_back(v);
    }
    return v;
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
    ReapOneShots();
    g_oneShots.push_back(v);
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

double g_forceAt = -10;
float g_tint[3] = {1, 1, 1};
double g_tintUntil = -10;
float g_spark;
double g_sparkAt = -10;
bool g_sparkBlock;  // blue-white (a block) rather than hot orange (a hit)
void Spark(float v, bool block = false) {
    g_spark = v;
    g_sparkAt = Now();
    g_sparkBlock = block;
}

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
                                           "window open", "incoming_warning", "hit landed", "perfect landed", "force",
                                           "hurt"};
    if (e != CUE_WINDOW_OPEN) Log("cue: %s", names[e]);
    PlaySoundFor(e);
    switch (e) {  // haptic: hand 1 = weapon hand (both for two-handed weapons), 2 = both hands
        case CUE_PERFECT_BLOCK:  // crack + ring + a bright blue rim: unmistakable
            Haptic(0, 1, 1.0f, 0.14f, 320); Haptic(0.15, 1, 0.8f, 0.12f, 160); Flash(1.0f); Spark(1.0f, true);
            VR_Flash(0.6f, 0.85f, 1.0f, 0.55f, 0.35f); break;
        case CUE_BLOCK:
            Haptic(0, 1, 1.0f, 0.15f, 250); Flash(0.85f); Spark(0.85f, true); VR_Flash(0.35f, 0.65f, 1.0f, 0.35f, 0.25f);
            break;
        case CUE_DODGE: Haptic(0, 2, 0.7f, 0.12f, 200); VR_Flash(0.8f, 0.8f, 0.8f, 0.2f, 0.2f); break;
        case CUE_BLOCK_FAILED: Haptic(0, 1, 0.6f, 0.08f, 80); Haptic(0.14, 1, 0.6f, 0.08f, 80); break;
        case CUE_HURT:  // a heavy thump in both hands and a red pulse at the edge of the view
            Haptic(0, 2, 1.0f, 0.16f, 55); Haptic(0.18, 2, 0.6f, 0.12f, 45); VR_Flash(0.95f, 0.05f, 0.02f, 0.75f, 0.55f);
            break;
        case CUE_PERFECT_HIT: Haptic(0, 1, 0.8f, 0.10f, 120); Flash(0.6f); break;
        case CUE_WINDOW_OPEN: Haptic(0, 1, 0.25f, 0.03f, 200); break;
        case CUE_INCOMING: Haptic(0, 2, 0.5f, 0.06f, 160); break;
        case CUE_HIT_LANDED:  // a hard knock then a short tail: it connected
            Haptic(0, 1, 1.0f, 0.09f, 90); Haptic(0.1, 1, 0.5f, 0.08f, 60); Flash(0.8f); Spark(1.0f); break;
        case CUE_PERFECT_LANDED:
            Haptic(0, 1, 1.0f, 0.22f, 70); Haptic(0.24, 1, 0.6f, 0.1f, 50); Flash(1.0f); Spark(1.0f);
            VR_Flash(1.0f, 0.75f, 0.2f, 0.25f, 0.3f); break;
        case CUE_FORCE:  // a low surge in the casting (left) hand
            Haptic(0, 0, 0.7f, 0.25f, 50); g_forceAt = Now(); break;
        default: break;
    }
}

void Cues_Update() {
    double t = Now();
    if (Features().cueTest) {  // harness: every cue in turn
        static double next;
        static int i;
        if (t >= next) {
            next = t + 1.5;
            Cues_Fire((CueEvent)(i++ % CUE_COUNT));
        }
    }
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
    float flash = g_flash[hand] * (float)max(0.0, 1.0 - (t - g_flashAt[hand]) / 0.6);
    float pulse = g_window ? 0.12f + 0.1f * (float)sin(t * 2 * 3.14159 * 4) : 0;
    return max(flash, pulse);
}

void Cues_SetWindow(bool open) { g_window = open; }

void Cues_GlowColor(int hand, float rgb[3]) {
    bool tinted = Now() < g_tintUntil;
    for (int i = 0; i < 3; i++) rgb[i] = tinted ? g_tint[i] : 1.0f;
}

void Cues_Move(int move) {
    if (!Features().cues) return;
    static const float kColor[6][3] = {{1, 1, 1}, {1.0f, 0.5f, 0.1f}, {1.0f, 0.15f, 0.1f}, {0.3f, 0.6f, 1.0f},
                                       {1.0f, 0.5f, 0.1f}, {0.3f, 0.6f, 1.0f}};
    if (move < 1 || move > 5) return;
    memcpy(g_tint, kColor[move], sizeof g_tint);
    g_tintUntil = Now() + 0.8;
    Flash(1.0f);
    for (int h = 0; h < 2; h++) g_flashAt[h] = Now() + 0.2;  // hold the flare a moment longer than a plain flash
    switch (move) {
        case 1: case 4: Haptic(0, 1, 1.0f, 0.25f, 60); break;  // power: heavy, low
        case 2: Haptic(0, 1, 1.0f, 0.08f, 320); Haptic(0.1, 1, 0.8f, 0.06f, 320); break;  // critical: sharp crack
        case 3: case 5:  // flurry / rapid: quick triple
            Haptic(0, 1, 0.8f, 0.05f, 200); Haptic(0.09, 1, 0.8f, 0.05f, 200); Haptic(0.18, 1, 0.8f, 0.05f, 200); break;
    }
    static const char* names[6] = {"", "power", "critical", "flurry", "power shot", "rapid"};
    Log("cue: move %s", names[move]);
}

float Cues_ForceGlow(int hand) {
    if (!Features().cues || hand != 0) return 0;
    double age = Now() - g_forceAt;
    return age < 0.5 ? (float)(1.0 - age / 0.5) : 0;
}

void Cues_Shot() {
    if (!Features().cues) return;
    WeaponClass c = Weapon_Right();
    const char* snd = c == WC_REPEATER ? "cb_sh_hvybl1" : c == WC_RIFLE ? "cb_sh_medbl1" : "cb_sh_blast1";
    PlayResref(snd, 1.0f, 0.97f + (rand() % 7) * 0.01f);  // a touch of pitch variety
}

float Cues_Sparks(double* age, float rgb[3]) {
    *age = Now() - g_sparkAt;
    rgb[0] = g_sparkBlock ? 0.6f : 1.0f;
    rgb[1] = g_sparkBlock ? 0.85f : 0.6f;
    rgb[2] = g_sparkBlock ? 1.0f : 0.2f;
    if (!Features().cues || *age > 0.45) return 0;
    return g_spark * (float)(1.0 - *age / 0.45);
}

// ---------------------------------------------------------------- lightsaber
static IXAudio2SourceVoice* g_hum;
static double g_lastWhoosh;
static float g_prevSpeed;

void Cues_SaberIgnite(bool on) {
    if (!Features().cues) return;
    PlayResref(on ? "cb_ls_powerup" : "cb_ls_powerdown", 0.8f);
}

void Cues_SaberHum(bool on, float speed) {
    if (!Features().cues || !g_xa) return;
    if (!on) {
        if (g_hum) {
            g_hum->Stop();
            g_hum->DestroyVoice();
            g_hum = nullptr;
        }
        g_prevSpeed = 0;
        return;
    }
    if (!g_hum) g_hum = PlayResref("cb_ls_humgood", 0.35f, 1.0f, true);
    if (!g_hum) return;
    // the movie effect: faster blade = louder, higher hum
    float k = min(speed / 6.0f, 1.0f);
    g_hum->SetVolume(g_master * (0.3f + 0.5f * k));
    g_hum->SetFrequencyRatio(1.0f + 0.35f * k);
    // swing whoosh on a fast stroke (rising edge, rate-limited)
    double t = Now();
    if (speed > 4.5f && g_prevSpeed <= 4.5f && t - g_lastWhoosh > 0.25) {
        static const char* kWhoosh[3] = {"cb_ls_swingshrt1", "cb_ls_swingshrt2", "cb_ls_swingshrt3"};
        PlayResref(kWhoosh[rand() % 3], 0.35f + 0.4f * k, 0.95f + 0.1f * (rand() % 100) / 100.0f);
        g_lastWhoosh = t;
    }
    g_prevSpeed = speed;
}
