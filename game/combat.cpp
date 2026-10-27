#define _CRT_RAND_S
// Skill layer over KotOR's d20 combat (x86, inside swkotor.exe).
//
// How KotOR resolves an attack (Steam build, see docs/FINDINGS.md "Combat timeline"):
//   1. At the start of the attacker's combat round the attack is rolled: 0x5BACA0 (thiscall, ecx = attacker,
//      first stack arg = target object, ret 0xC) rolls the d20 and writes the result into the round's attack record.
//      Right after RollDice(1,20) it checks the debug global 0x832874: non-zero replaces the natural roll.
//   2. When the swing / bolt visibly lands (~500 ms after the roll for kinrath melee), the damage is turned into a
//      damage effect (type 38, 15 damage slots + flags) and queued as an "apply effect" event (type 5) with 0 ms
//      delay: AddEventDeltaTime 0x4B08D0 (thiscall on the AI master: days, ms, caller id, object id, type, data,
//      ret 0x18).
//   3. When the event fires, the damage handler 0x4DFA40 (stdcall: target object, effect, flags; ret 0xC) applies it.
//
// We hook all three:
//   resolver   player attacking -> an attack gesture right before the roll adds +4 (in the rhythm window, 0.6 s)
//              or +2 (within 2 s) to a fresh d20; a natural 1 still misses.
//              player attacked  -> [Combat] ImpactBlock=0 only: guard / dodge at roll time (the old behavior).
//   resolver   also: an enemy roll against the player starts a warning (impact = roll + the attacker's learned
//              roll -> impact delay), which drives the Arkham-style chevrons and the incoming cue.
//   queue      weapon damage queued against the player = the impact: matched to its roll to learn the delay.
//   damage     [Combat] ImpactBlock=1 (default): the block / parry / dodge is decided at the impact itself, when the
//              player can see the swing land. A successful one zeroes the effect's damage before KotOR applies it.

#include "combat.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "cues.h"
#include "gestures.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

const DWORD kResolverVA = 0x5BACA0;
const BYTE kResolverOrig[6] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};  // mov eax, fs:[0]
const DWORD kQueueVA = 0x4B08D0;
const BYTE kQueueOrig[8] = {0xA1, 0xFC, 0x39, 0x7A, 0x00, 0x83, 0xEC, 0x10};  // mov eax, [app]; sub esp, 0x10
const DWORD kDamageVA = 0x4DFA40;
const BYTE kDamageOrig[7] = {0x6A, 0xFF, 0x68, 0x08, 0xAA, 0x71, 0x00};  // push -1; push 0x71AA08
volatile DWORD* const kForcedRoll = (volatile DWORD*)0x832874;
const DWORD kEventApplyEffect = 5;
const WORD kEffectDamage = 38;

BYTE* g_resolverTramp;
BYTE* g_queueTramp;
BYTE* g_damageTramp;
volatile DWORD g_lastAttack;  // GetTickCount of the last attack roll by anyone (0 = none)
bool g_impactBlock = true;

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / f.QuadPart;
}

unsigned D20() {
    unsigned v = 0;
    if (rand_s(&v) != 0) v = (unsigned)rand();
    return 1 + v % 20;
}

void* AsCreature(void* obj) {
    __try {
        typedef void*(__thiscall * Cast_t)(void*);
        return ((Cast_t)((*(void***)obj)[0x30 / 4]))(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return nullptr;
}

DWORD ObjectId(void* obj) {
    __try {
        return obj ? *(DWORD*)((BYTE*)obj + 4) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

// A weapon attack's damage effect (melee 0x5B6F30 / ranged 0x5B75D0 set ints 0x13 and 0x15 to 1; script / Force
// damage does not). Effect: type WORD at +8, int count at +0x30, int array at +0x34.
bool IsWeaponDamage(void* effect, int** ints) {
    __try {
        BYTE* e = (BYTE*)effect;
        if (!e || *(WORD*)(e + 8) != kEffectDamage) return false;
        int n = *(int*)(e + 0x30);
        int* v = *(int**)(e + 0x34);
        if (n <= 0x15 || !v || v[0x13] != 1 || v[0x15] != 1) return false;
        *ints = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

// ---------------------------------------------------------------- incoming-attack timeline
// An enemy attack rolled against the player. KotOR rolls at the start of the attacker's round and fills the attack
// record's ReaxnDelay (ms from the roll to the moment the blow lands) right after; the damage event itself is only
// created at that moment (queued with 0 ms), so the record is the only advance notice.
struct Incoming {
    DWORD attacker;
    BYTE* record;        // the attacker's attack record (round + 4 + index * 0x14C)
    double rolled, due;  // Now() seconds; due < 0 until ReaxnDelay is known
    bool warned;
};
CRITICAL_SECTION g_lock;
std::vector<Incoming> g_incoming;
// Roll -> impact delay learned per attacker (melee records leave ReaxnDelay at 0; kinrath measured 500 +- 6 ms).
std::map<DWORD, double> g_learnedDelay;
const double kDefaultDelay = 0.5;

// ---------------------------------------------------------------- the player's attack rhythm
double g_playerRolls[2] = {-10, -10};  // the last two player attack rolls (separate rounds)
int g_lastPlayerBonus;                  // bonus given to the player's last roll (4 = on the beat)
double g_lastPlayerBonusAt = -10;
bool g_windowOpen;

double PredictedNextRoll() {
    double interval = g_playerRolls[1] - g_playerRolls[0];
    if (g_playerRolls[0] < 0 || interval < 1.0 || interval > 4.0) return -1;
    double next = g_playerRolls[1] + interval;
    while (next < Now() - 0.2) next += interval;  // skipped rounds keep the beat
    return next;
}

const double kPerfectWindow = 0.6;  // seconds before the roll
const double kTimedWindow = 2.0;
const double kIncomingLead = 0.6;  // warning shown / cue fired this long before an attack lands

// Special moves: the attack gesture's move becomes the best rank of that combat feat the character can use, written
// into the attack record's AttackType (+0x64) before the roll; KotOR then applies the feat's own to-hit / damage
// rules (attack bonus 0x5A7EA0, damage 0x5A9050). Feat use is checked with the engine's own test 0x5A6680
// (thiscall on the creature stats [creature + 0xA74], feat id), so a character never gets a move it does not have.
void ApplySpecialMove(void* creature) {
    GestureMove m = Gestures_TakeMove(kTimedWindow);
    if (m == MOVE_NONE) return;
    static const unsigned short kFeats[][3] = {
        {0, 0, 0},
        {83, 17, 28},  // power attack: master, improved, base
        {81, 19, 8},   // critical strike
        {91, 11, 0},   // flurry: improved, base
        {82, 18, 29},  // power blast
        {92, 30, 0},   // rapid shot: improved, base
    };
    typedef bool(__thiscall * CanUseFeat_t)(void*, unsigned short);
    const CanUseFeat_t CanUseFeat = (CanUseFeat_t)0x5A6680;
    __try {
        void* stats = *(void**)((BYTE*)creature + 0xA74);
        BYTE* round = *(BYTE**)((BYTE*)creature + 0x9C8);
        if (!stats || !round) return;
        BYTE* record = round + 4 + *(round + 0x96C) * 0x14C;
        for (unsigned short feat : kFeats[m]) {
            if (!feat || !CanUseFeat(stats, feat)) continue;
            *(WORD*)(record + 0x64) = feat;
            Log("combat: special_move %d -> feat %u on this attack", (int)m, feat);
            Cues_Move((int)m);
            return;
        }
        Log("combat: special move %d, but the character has none of its feats", (int)m);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

}  // namespace

// ---------------------------------------------------------------- 1. the roll
extern "C" void __cdecl Combat_ResolverPre(void* attacker, void* targetObj) {
    *kForcedRoll = 0;
    g_lastAttack = GetTickCount() | 1;
    void* player = Weapon_PlayerCreature();
    if (!player) return;
    void* target = targetObj ? AsCreature(targetObj) : nullptr;

    if (attacker == player) {
        if (!Weapon_Profile(Weapon_Right()).melee && Weapon_Right() != WC_NONE && target) {
            __try {
                float to[3];
                memcpy(to, (BYTE*)target + 0x90, sizeof to);
                to[2] += 1.1f;  // chest height
                Weapons_FireBolt(to, 0.15f);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        double t = Now();
        if (t - g_playerRolls[1] > 0.3) {  // several rolls in one round (flurry, off hand, bursts) = one beat
            g_playerRolls[0] = g_playerRolls[1];
            g_playerRolls[1] = t;
        }
        double ago = Gestures_LastAttackAgo();
        ApplySpecialMove(attacker);
        __try {  // which attack KotOR is making (0 plain, else the feat: Power Attack, Critical Strike, ...)
            BYTE* round = *(BYTE**)((BYTE*)attacker + 0x9C8);
            if (round) Log("combat: player roll, attack type %d", *(int*)(round + 4 + *(round + 0x96C) * 0x14C + 0x64));
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (Gestures_ConsumeRecentAttack(kTimedWindow) <= 0) return;
        unsigned nat = D20();
        int bonus = ago <= kPerfectWindow ? 4 : 2;  // on the beat / roughly in time
        unsigned forced = nat == 1 ? 1 : (nat + bonus > 20 ? 20 : nat + bonus);
        *kForcedRoll = forced;
        g_lastPlayerBonus = bonus;
        g_lastPlayerBonusAt = Now();
        Log("combat: player_attack, gesture %.2f s before the roll: d20 %u +%d -> %u", ago, nat, bonus, forced);
        return;
    }
    if (target == player) {
        __try {
            BYTE* round = *(BYTE**)((BYTE*)attacker + 0x9C8);
            BYTE* record = round ? round + 4 + *(round + 0x96C) * 0x14C : nullptr;
            if (record) {
                EnterCriticalSection(&g_lock);
                g_incoming.push_back({ObjectId(attacker), record, Now(), -1, false});
                LeaveCriticalSection(&g_lock);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (target == player && !g_impactBlock) {
        bool melee = Weapon_Profile(Weapon_Right()).melee && Weapon_Right() != WC_NONE;
        bool guard = melee && Gestures_Guarding(0.6);
        bool dodge = Gestures_Dodged(0.6);
        if (!guard && !dodge) return;
        unsigned roll = D20();
        unsigned chance = guard ? 13 : 11;  // 60% block / 50% dodge, on top of the normal defense roll
        if (roll <= chance) {
            *kForcedRoll = 1;
            Log("combat: incoming attack %s (d20 %u <= %u) -> miss", guard ? "blocked" : "dodged", roll, chance);
            Cues_Fire(guard ? CUE_BLOCK : CUE_DODGE);
        } else {
            Log("combat: incoming attack, %s failed (d20 %u > %u)", guard ? "block" : "dodge", roll, chance);
            Cues_Fire(CUE_BLOCK_FAILED);
        }
    }
}

// ---------------------------------------------------------------- 2. the queue (impact times)
extern "C" void __cdecl Combat_QueuePre(DWORD ms, DWORD caller, DWORD object, DWORD type, void* data) {
    if (type != kEventApplyEffect) return;
    int* ints;
    if (!IsWeaponDamage(data, &ints)) return;
    DWORD playerId = ObjectId(Weapon_PlayerCreature());
    if (!playerId) return;
    if (caller == playerId && object != playerId) {  // the player's blow / bolt just connected
        bool beat = g_lastPlayerBonus == 4 && Now() - g_lastPlayerBonusAt < 3.0;
        if (beat) g_lastPlayerBonus = 0;
        Cues_Fire(beat ? CUE_PERFECT_LANDED : CUE_HIT_LANDED);
        return;
    }
    if (object != playerId || caller == playerId) return;
    // the blow just landed: match it to its roll (oldest pending from this attacker) and log the prediction error
    double t = Now();
    EnterCriticalSection(&g_lock);
    for (size_t i = 0; i < g_incoming.size(); i++)
        if (g_incoming[i].attacker == caller) {
            double d = t - g_incoming[i].rolled;
            Log("combat: incoming_damage from %08lX, %.0f ms after the roll (predicted %.0f ms, queued +%lu ms)",
                caller, d * 1000, (g_incoming[i].due - g_incoming[i].rolled) * 1000, ms);
            if (d > 0.1 && d < 3.0) {
                auto it = g_learnedDelay.find(caller);
                g_learnedDelay[caller] = it == g_learnedDelay.end() ? d : it->second * 0.7 + d * 0.3;
            }
            g_incoming.erase(g_incoming.begin() + i);
            break;
        }
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------- 3. the impact
extern "C" void __cdecl Combat_DamagePre(void* targetObj, void* effect) {
    int* ints;
    if (!IsWeaponDamage(effect, &ints)) return;
    void* player = Weapon_PlayerCreature();
    if (!player || AsCreature(targetObj) != player) return;
    int dealt = 0;
    for (int i = 0; i < 15; i++) dealt += ints[i] > 0 ? ints[i] : 0;
    if (!g_impactBlock) {
        if (dealt > 0) Cues_Fire(CUE_HURT);
        return;
    }

    bool melee = Weapon_Profile(Weapon_Right()).melee && Weapon_Right() != WC_NONE;
    double held = melee ? Gestures_GuardHeldFor() : -1;
    bool guard = held >= 0;
    bool perfect = guard && held <= 0.4;  // raised into the blow: a parry
    bool dodge = !guard && Gestures_Dodged(0.4);
    if (!guard && !dodge) {
        if (dealt > 0) Cues_Fire(CUE_HURT);
        return;
    }
    unsigned roll = D20();
    unsigned chance = perfect ? 19 : guard ? 12 : 10;  // 95% parry / 60% held guard / 50% dodge
    int total = 0;
    for (int i = 0; i < 15; i++) total += ints[i] > 0 ? ints[i] : 0;
    if (roll <= chance) {
        for (int i = 0; i < 15; i++)
            if (ints[i] > 0) ints[i] = 0;
        Log("combat: impact %s (d20 %u <= %u), %d damage stopped (guard held %.2f s)",
            perfect ? "parried" : guard ? "blocked" : "dodged", roll, chance, total, held);
        Cues_Fire(perfect ? CUE_PERFECT_BLOCK : guard ? CUE_BLOCK : CUE_DODGE);
    } else {
        Log("combat: impact, %s failed (d20 %u > %u), %d damage", guard ? "block" : "dodge", roll, chance, total);
        Cues_Fire(CUE_BLOCK_FAILED);
        if (total > 0) Cues_Fire(CUE_HURT);
    }
}

// ---------------------------------------------------------------- stubs / install
__declspec(naked) static void ResolverStub() {
    __asm {
        push ecx                      // keep the attacker for the real resolver
        push dword ptr [esp + 8]      // target (first stack argument)
        push ecx                      // attacker
        call Combat_ResolverPre
        add esp, 8
        pop ecx
        jmp dword ptr [g_resolverTramp]  // the original, untouched stack/registers; it returns to our caller
    }
}

__declspec(naked) static void QueueStub() {
    __asm {
        push ecx
        push dword ptr [esp + 0x1C]   // data      (arg 6)
        push dword ptr [esp + 0x1C]   // type      (arg 5)
        push dword ptr [esp + 0x1C]   // object id (arg 4)
        push dword ptr [esp + 0x1C]   // caller id (arg 3)
        push dword ptr [esp + 0x1C]   // ms        (arg 2)
        call Combat_QueuePre
        add esp, 20
        pop ecx
        jmp dword ptr [g_queueTramp]
    }
}

__declspec(naked) static void DamageStub() {
    __asm {
        push ecx
        push dword ptr [esp + 0xC]    // effect (arg 2)
        push dword ptr [esp + 0xC]    // target (arg 1)
        call Combat_DamagePre
        add esp, 8
        pop ecx
        jmp dword ptr [g_damageTramp]
    }
}

static bool Detour(DWORD va, const BYTE* orig, int len, void* stub, BYTE** tramp, const char* what) {
    BYTE* target = (BYTE*)(ULONG_PTR)va;
    if (memcmp(target, orig, len) != 0) {
        Log("combat: unexpected bytes at %08lX (%s), not hooking", va, what);
        return false;
    }
    *tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(*tramp, target, len);  // the copied instructions use only absolute operands
    (*tramp)[len] = 0xE9;
    *(INT32*)(*tramp + len + 1) = (INT32)((target + len) - (*tramp + len + 5));
    DWORD old;
    VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(INT32*)(target + 1) = (INT32)((BYTE*)stub - (target + 5));
    for (int i = 5; i < len; i++) target[i] = 0x90;
    VirtualProtect(target, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    Log("combat: hooked %s at %08lX", what, va);
    return true;
}

bool Combat_Install() {
    InitializeCriticalSection(&g_lock);
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* s = strrchr(ini, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - ini), "kvr\\kvr.ini");
    g_impactBlock = GetPrivateProfileIntA("Combat", "ImpactBlock", 1, ini) != 0;
    bool ok = Detour(kResolverVA, kResolverOrig, 6, ResolverStub, &g_resolverTramp, "attack resolver");
    if (ok) Detour(kQueueVA, kQueueOrig, 8, QueueStub, &g_queueTramp, "event queue");
    bool dmg = ok && Detour(kDamageVA, kDamageOrig, 7, DamageStub, &g_damageTramp, "damage handler");
    if (!dmg) g_impactBlock = false;
    Log("combat: blocks decided %s", g_impactBlock ? "at impact" : "at the roll");
    return ok;
}

void Combat_EndFrame() { *kForcedRoll = 0; }

bool Combat_AttackWithin(unsigned long ms) {
    DWORD t = g_lastAttack;
    return t && GetTickCount() - t < ms;
}

// Once per frame: warnings for attacks about to land, the player's rhythm window, stale entries.
void Combat_Update() {
    double t = Now();
    bool fighting = Combat_AttackWithin(6000);
    EnterCriticalSection(&g_lock);
    for (size_t i = 0; i < g_incoming.size();) {
        Incoming& in = g_incoming[i];
        if (in.due < 0) {  // ReaxnDelay is filled in after the roll
            short reaxn = 0, anim = 0;
            __try {
                reaxn = *(short*)(in.record + 0x10);
                anim = *(short*)(in.record + 0x08);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            if (reaxn > 0 || t - in.rolled > 0.05) {
                auto it = g_learnedDelay.find(in.attacker);
                double learned = it != g_learnedDelay.end() ? it->second : kDefaultDelay;
                in.due = in.rolled + (reaxn > 0 ? reaxn / 1000.0 : learned);
                Log("combat: incoming_attack from %08lX, reaxn %d ms, anim %d ms", in.attacker, reaxn, anim);
            }
        }
        if (in.due > 0 && t > in.due + 1.0 || t - in.rolled > 5.0) {  // a miss (no damage event) or a dropped attack
            g_incoming.erase(g_incoming.begin() + i);
            continue;
        }
        if (!in.warned && in.due > 0 && t >= in.due - kIncomingLead) {
            in.warned = true;
            Cues_Fire(CUE_INCOMING);
        }
        i++;
    }
    LeaveCriticalSection(&g_lock);

    double next = fighting ? PredictedNextRoll() : -1;
    bool open = next > 0 && t >= next - kPerfectWindow && t <= next;
    if (open && !g_windowOpen) Cues_Fire(CUE_WINDOW_OPEN);
    g_windowOpen = open;
    Cues_SetWindow(open);
}

bool Combat_InPerfectWindow() { return g_windowOpen; }

bool Combat_PlayerAttacking() { return Now() - g_playerRolls[1] < 3.5; }
void Combat_ResetAttacking() { g_playerRolls[0] = g_playerRolls[1] = -100; }

int Combat_IncomingWarnings(CombatWarning* out, int max) {
    double t = Now();
    int n = 0;
    EnterCriticalSection(&g_lock);
    for (const Incoming& in : g_incoming) {
        if (n >= max) break;
        if (in.due < 0) continue;
        double left = in.due - t;
        if (left > kIncomingLead || left < -0.15) continue;
        out[n].attacker = in.attacker;
        out[n].secondsLeft = (float)left;
        n++;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}
