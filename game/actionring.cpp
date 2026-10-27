// Target actions in the world (see actionring.h).
//
// KotOR's in-game interface (client app [[0x7A39FC]+4] -> [+4] -> [+0x40] -> [+0x90]) keeps the target action menu at
// +0xBC: for each of the 3 target columns c, a list at +c*0xC (entries pointer, count) of 0x38-byte entries
//   +0x04 kind, +0x08 id (0x1000xxxx feat, 0x2000xxxx Force power, 0x3EB attack, ...), +0x20 icon resref (16 chars)
// and the column's selected entry id at +(c + (byte[+0x1AEA] + 3) * 3) * 4 (-1: the first entry). The cycle buttons
// (0x6865B0 / 0x686680) only rewrite that id; the column's key (1..3) then performs the selected entry on the target.
//
// Holding the right grip on a target with such actions opens a panel (text slot 2) along your hand listing all of
// them by name (kvr\actions.txt, icon resref -> name, from tools/action_names.py); point + trigger performs one.
#include "actionring.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../common/kvr_shared.h"
#include "vr.h"

extern void Log(const char* fmt, ...);

namespace {

const int kSlot = 2;
const int kColumns = 3;
const int kEntrySize = 0x38;
const DWORD kTimeoutMs = 10000;

struct Choice {
    int column;
    int id;
};

std::map<std::string, std::string> g_names;  // icon resref -> display name
bool g_open;
DWORD g_openedAt;
std::vector<Choice> g_choices;  // panel line i+1
bool g_trigWas;
bool g_gripWas;

BYTE* ActionBlock() {
    __try {
        BYTE* app = *(BYTE**)0x7A39FC;
        if (!app) return nullptr;
        BYTE* client = *(BYTE**)(app + 4);
        if (!client) return nullptr;
        BYTE* inner = *(BYTE**)(client + 4);
        if (!inner) return nullptr;
        BYTE* gui = *(BYTE**)(inner + 0x40);
        if (!gui) return nullptr;
        BYTE* mi = *(BYTE**)(gui + 0x90);
        return mi ? mi + 0xBC : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

std::string NameFor(const char* icon) {
    std::string key;
    for (const char* p = icon; *p; p++) key += (char)tolower((unsigned char)*p);
    auto it = g_names.find(key);
    if (it != g_names.end()) return it->second;
    // unknown: the icon resref without its "i_" / "ip_" / "isk_" prefix and rank digits, capitalized
    size_t u = key.find('_');
    std::string s = u != std::string::npos && u <= 3 ? key.substr(u + 1) : key;
    while (!s.empty() && isdigit((unsigned char)s.back())) s.pop_back();
    for (auto& ch : s)
        if (ch == '_') ch = ' ';
    if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
    Log("ring: no name for icon '%s'", icon);
    return s.empty() ? std::string("?") : s;
}

struct Entry {
    int column;
    int id;
    char icon[17];
};

int ReadEntries(BYTE* blk, Entry* out, int cap) {
    int n = 0;
    __try {
        for (int c = 0; c < kColumns; c++) {
            BYTE* ents = *(BYTE**)(blk + c * 0xC);
            int count = *(int*)(blk + c * 0xC + 4);
            for (int i = 0; ents && i < count && i < 12 && n < cap; i++) {
                BYTE* e = ents + i * kEntrySize;
                out[n].column = c;
                out[n].id = *(int*)(e + 8);
                memcpy(out[n].icon, e + 0x20, 16);
                out[n].icon[16] = 0;
                n++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n;
}

BYTE* MainInterface() {
    BYTE* blk = ActionBlock();
    return blk ? blk - 0xBC : nullptr;
}

void Close() {
    if (g_open) VR_HideText(kSlot);
    g_open = false;
    g_choices.clear();
}

}  // namespace

void ActionRing_Init(const char* gameDir) {
    char path[MAX_PATH];
    sprintf_s(path, "%s\\kvr\\actions.txt", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Log("ring: %s missing (tools/action_names.py); icons stand in for names", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        std::string name = tab + 1;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        g_names[line] = name;
    }
    fclose(f);
    Log("ring: %d action names", (int)g_names.size());
}

bool ActionRing_Open(const KvrInput& in) {
    BYTE* blk = ActionBlock();
    if (!blk) return false;
    Entry ents[36];
    int n = ReadEntries(blk, ents, 36);
    if (n == 0) return false;
    std::string text = "#Actions";
    g_choices.clear();
    for (int i = 0; i < n; i++) {
        std::string icon = ents[i].icon;
        for (auto& ch : icon) ch = (char)tolower((unsigned char)ch);
        text += "\n@" + icon + "|" + NameFor(ents[i].icon);
        g_choices.push_back({ents[i].column, ents[i].id});
    }
    // world-locked 0.7 m out along your hand's aim, facing you, tilted back a little
    float head[3];
    VR_HeadPos(head);
    const float* q = in.aim.quat;
    float fx = -2 * (q[0] * q[2] + q[3] * q[1]), fy = -2 * (q[1] * q[2] - q[3] * q[0]),
          fz = -(1 - 2 * (q[0] * q[0] + q[1] * q[1]));
    float hl = sqrtf(fx * fx + fz * fz);
    if (hl < 0.2f) {  // pointing straight up / down: use the head's forward
        float yaw = VR_HeadYaw();
        fx = -sinf(yaw), fz = -cosf(yaw), fy = 0, hl = 1;
    }
    if (fy > 0.3f) fy = 0.3f;
    if (fy < -0.6f) fy = -0.6f;
    float pos[3] = {in.aim.pos[0] + fx / hl * 0.7f, in.aim.pos[1] + fy * 0.7f, in.aim.pos[2] + fz / hl * 0.7f};
    float yaw = atan2f(head[0] - pos[0], head[2] - pos[2]);  // the quad's +Z toward the head
    float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f), sp = sinf(-0.12f), cp = cosf(-0.12f);
    float rot[4] = {cy * sp, sy * cp, -sy * sp, cy * cp};
    VR_ShowText(kSlot, KVR_ANCHOR_WORLD, pos, rot, 0.55f, text.c_str(), -1);
    VR_SetTextPickable(kSlot, true);
    g_open = true;
    g_openedAt = GetTickCount();
    g_trigWas = in.trigR > 0.6f;
    g_gripWas = true;  // the grip that opened it
    Log("ring: open, %d actions", n);
    return true;
}

bool ActionRing_Active() { return g_open; }

bool ActionRing_ClearQueue() {
    typedef void(__fastcall * ClearFn)(void* self, void* edx);  // thiscall, no arguments
    BYTE* mi = MainInterface();
    if (!mi) return false;
    __try {
        ((ClearFn)0x6887D0)(mi, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

BYTE ActionRing_Update(const KvrInput& in) {
    bool trig = in.trigR > 0.6f, grip = in.gripR > 0.7f;
    BYTE key = 0;
    if (g_open) {
        const KvrShared* shm = VR_Shared();
        int line = shm ? shm->text[kSlot].hover : -1;
        if (trig && !g_trigWas && line >= 1 && line <= (int)g_choices.size()) {
            Choice c = g_choices[line - 1];
            BYTE* blk = ActionBlock();
            if (blk) {
                __try {
                    int who = *(signed char*)(blk + 0x1AEA);
                    *(int*)(blk + (c.column + (who + 3) * 3) * 4) = c.id;
                    static const BYTE kKeys[kColumns] = {0x02, 0x03, 0x04};  // DIK_1..DIK_3: target columns
                    key = kKeys[c.column];
                    Log("ring: picked line %d (column %d, id %08X)", line, c.column, c.id);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            Close();
        } else if ((grip && !g_gripWas) || (in.buttons & KVR_BTN_B) || GetTickCount() - g_openedAt > kTimeoutMs) {
            Close();  // another grip, B, or left alone: dismissed
        }
    }
    g_trigWas = trig;
    g_gripWas = grip;
    return key;
}
