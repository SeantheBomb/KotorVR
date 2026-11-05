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
#include "guard.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "actionring.h"
#include "anim.h"
#include "cues.h"
#include "features.h"
#include "melee.h"
#include "force.h"
#include "gestures.h"
#include "vr.h"
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

double Now();
double NowImpl() {
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

double Now() { return NowImpl(); }

// ---------------------------------------------------------------- v2 contact combat (docs/V2_PROPOSAL.md)
// [Combat] Mode=contact (default): your hands decide, the d20 grades. Mode=rounds: the v1 rules (gesture timing
// bonuses on KotOR's rounds, guard / dodge gestures at the impact).
bool g_contact = true;
float g_parryZone = 0.38f;   // [Combat] ParryZone: strike-zone radius (m) - the difficulty knob for defence
int g_tokens = 2;            // [Combat] AttackTokens: enemies whose attacks on you may be live at once
float g_guard = 100;         // your guard: held blocks drain it, parries don't; at 0 a block breaks
double g_lastBlockAt = -10;

struct Posture {
    float value = -1, max = 60;
    double lastHit = -10, openUntil = -10;
};
std::map<DWORD, Posture> g_posture;
std::map<void*, std::pair<DWORD, bool>> g_effectFrom;  // damage effect -> (attacker id, ranged)
struct PendingRoll {
    BYTE* record;
    DWORD target;
    double at;
};
std::vector<PendingRoll> g_playerPending;  // the player's rolls, to see a credited miss

DWORD ObjectIdOf(void* obj);
Posture& PostureOf(DWORD id) {
    Posture& p = g_posture[id];
    if (p.value < 0) {
        short hp = 0;
        __try {
            BYTE* c = (BYTE*)Weapon_CreatureById(id);
            if (c) hp = *(short*)(c + 0xDC);  // current hit points (CSWSObject::GetCurrentHitPoints)
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        p.max = hp > 0 && hp <= 20 ? 20.0f : 60.0f;  // rank-and-file go down fast (Until You Fall's lesser husks)
        p.value = p.max;
    }
    return p;
}

void SetRoundPaused(DWORD id, bool paused) {
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(id);
        BYTE* round = c ? *(BYTE**)(c + 0x9C8) : nullptr;
        if (round) *(int*)(round + 0x958) = paused ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void HitPosture(DWORD id, float amount, const char* why) {
    Posture& p = PostureOf(id);
    double t = Now();
    p.lastHit = t;
    if (t < p.openUntil) return;
    p.value -= amount;
    if (p.value <= 0) {
        p.value = 0;
        p.openUntil = t + 2.2;
        SetRoundPaused(id, true);  // its combat round stops: no attacks while it is open
        Anim_HitPause(id, 0.3f);
        Cues_Fire(CUE_PERFECT_LANDED);
        Log("combat: %08lX guard broken (%s) - open for 2.2 s", id, why);
    }
}

bool IsOpen(DWORD id) {
    auto it = g_posture.find(id);
    return it != g_posture.end() && Now() < it->second.openUntil;
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
    bool ranged;
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
const double kIncomingLead = kCombatWarnLead;  // incoming cue fired this long before an attack lands

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
        {77, 20, 31},  // sniper shot: master, improved, base
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

// distinct enemies with an attack on the player in flight, other than this one (attack tokens)
static int LiveAttackersBesides(DWORD aid) {
    EnterCriticalSection(&g_lock);
    std::set<DWORD> live;
    double t = Now();
    for (const Incoming& in : g_incoming)
        if (in.attacker != aid && (in.due < 0 || in.due > t - 0.1)) live.insert(in.attacker);
    LeaveCriticalSection(&g_lock);
    return (int)live.size();
}

// ---------------------------------------------------------------- 1. the roll
// One trigger pull = one shot: when the queued ranged attack has rolled, clear the game's attack queue this long after
// (the bolt and its damage play out first; the next round would otherwise fire on its own).
static const double kShotClearDelay = 0.9;
static double g_clearShotAt = -1;
extern "C" void __cdecl Combat_ResolverPre(void* attacker, void* targetObj) {
    *kForcedRoll = 0;
    g_lastAttack = GetTickCount() | 1;
    void* player = Weapon_PlayerCreature();
    if (!player) return;
    void* target = targetObj ? AsCreature(targetObj) : nullptr;
    Melee_NoteAttack(ObjectId(attacker), ObjectId(targetObj));
    if (attacker == player) {
        Log("combat: player attack roll on %08lX", ObjectId(targetObj));  // every one (tests count)
        // one trigger pull, one shot (2026-11-05): a ranged attack queued by a shot would otherwise repeat round after
        // round with the gun idle; Combat_Update clears the queue once this attack has played out
        if (!Weapon_Profile(Weapon_Right()).melee && Weapon_Right() != WC_NONE) g_clearShotAt = Now() + kShotClearDelay;
    }
    if (g_contact) {
        DWORD aid = ObjectId(attacker), tid = ObjectId(targetObj);
        if (attacker == player) {
            double tnow = Now();  // your attack rounds (the stick clears a queued attack while one runs)
            if (tnow - g_playerRolls[1] > 0.3) {
                g_playerRolls[0] = g_playerRolls[1];
                g_playerRolls[1] = tnow;
            }
            // an open enemy (guard broken): your blow is a critical threat
            if (IsOpen(tid)) {
                *kForcedRoll = 20;
                Log("combat: player attack on an open enemy -> natural 20");
            }
            __try {
                BYTE* round = *(BYTE**)((BYTE*)attacker + 0x9C8);
                if (round) g_playerPending.push_back({round + 4 + *(round + 0x96C) * 0x14C, tid, Now()});
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            ApplySpecialMove(attacker);
            return;
        }
        if (target == player) {
            if (IsOpen(aid)) {  // staggered: it can't follow through
                *kForcedRoll = 1;
                return;
            }
            // attack tokens: only a few enemies' attacks on you are live at once; the rest whiff (no telegraph)
            int live = LiveAttackersBesides(aid);
            if (live >= g_tokens) {
                *kForcedRoll = 1;
                static int logged;
                if (logged++ < 20) Log("combat: %08lX waits its turn (%d attackers already live) -> whiff", aid, live);
                return;
            }
        }
    }

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
    // [Debug] TelegraphTest=1: every attack (e.g. a kinrath on a companion) is telegraphed as if aimed at the player
    if (target == player || (Features().telegraphTest && target && attacker != player)) {
        __try {
            BYTE* round = *(BYTE**)((BYTE*)attacker + 0x9C8);
            BYTE* record = round ? round + 4 + *(round + 0x96C) * 0x14C : nullptr;
            if (record) {
                EnterCriticalSection(&g_lock);
                bool ranged = false;
                __try {
                    ranged = *(int*)(record + 0x40) != 0;
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                g_incoming.push_back({ObjectId(attacker), record, Now(), -1, false, ranged});
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
    if (caller == playerId && object != playerId && g_contact) {  // the player's blow / bolt lands now
        float speed = 1;
        bool head = false;
        int dealt = 0;
        for (int i = 0; i < 15; i++) dealt += ints[i] > 0 ? ints[i] : 0;
        float age = Melee_CreditAge(object);
        if (!Melee_TakeCredit(object, &speed, &head)) {
            Log("combat: (credit for %08lX: %s %.2f s)", object, age < 0 ? "none," : "aged", age);
            for (int i = 0; i < 15; i++)
                if (ints[i] > 0) ints[i] = 0;
            Log("combat: player blow on %08lX without contact -> no damage (%d stopped)", object, dealt);
            return;
        }
        // graded by how you hit: swing speed (a slap does little, a full swing more), a head shot 1.5x
        float k = head ? 1.5f : (speed >= 1.5f ? 0.75f + speed * 0.07f : 1.0f);
        if (k > 1.35f && !head) k = 1.35f;
        for (int i = 0; i < 15; i++)
            if (ints[i] > 0) ints[i] = (int)(ints[i] * k + 0.5f);
        HitPosture(object, 12, "hits");
        Anim_HitPause(object, head ? 0.09f : 0);
        Cues_Fire(CUE_HIT_LANDED);
        Log("combat: player blow on %08lX lands: %d damage x%.2f%s", object, dealt, k, head ? " (head)" : "");
        return;
    }
    if (caller == playerId && object != playerId) {  // the player's blow / bolt just connected
        bool beat = g_lastPlayerBonus == 4 && Now() - g_lastPlayerBonusAt < 3.0;
        if (beat) g_lastPlayerBonus = 0;
        int dealt = 0;
        for (int i = 0; i < 15; i++) dealt += ints[i] > 0 ? ints[i] : 0;
        if (dealt > 0) Anim_HitPause(object, beat ? 0.09f : 0);  // the struck enemy freezes a beat (Phase A feel)
        Cues_Fire(beat ? CUE_PERFECT_LANDED : CUE_HIT_LANDED);
        return;
    }
    if (Features().telegraphTest && object != playerId) Anim_HitPause(object, 0);  // any landed blow freezes its target
    if (object != playerId || caller == playerId) return;
    // the blow just landed: match it to its roll (oldest pending from this attacker) and log the prediction error
    double t = Now();
    EnterCriticalSection(&g_lock);
    for (size_t i = 0; i < g_incoming.size(); i++)
        if (g_incoming[i].attacker == caller) {
            double d = t - g_incoming[i].rolled;
            Log("combat: incoming_damage from %08lX, %.0f ms after the roll (predicted %.0f ms, queued +%lu ms)",
                caller, d * 1000, (g_incoming[i].due - g_incoming[i].rolled) * 1000, ms);
            g_effectFrom[data] = {caller, g_incoming[i].ranged};
            if (g_effectFrom.size() > 64) g_effectFrom.erase(g_effectFrom.begin());
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
// v2 defence at the impact: where your blade is decides (Until You Fall's block line, JKA's parry cancel)
static bool ContactDefence(void** targetSlot, void* effect, int* ints, int dealt) {
    auto from = g_effectFrom.find(effect);
    DWORD attacker = from != g_effectFrom.end() ? from->second.first : 0;
    bool ranged = from != g_effectFrom.end() && from->second.second;
    float me[3], them[3];
    if (!Weapon_PlayerPos(me) || !attacker) return false;
    __try {
        BYTE* c = (BYTE*)Weapon_CreatureById(attacker);
        if (!c) return false;
        memcpy(them, c + 0x90, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    float dx = them[0] - me[0], dy = them[1] - me[1], dl = sqrtf(dx * dx + dy * dy);
    if (dl < 0.05f) return false;
    dx /= dl;
    dy /= dl;
    float chest[3] = {me[0], me[1], me[2] + 1.2f};
    bool saber = Weapon_Right() == WC_SABER || Weapon_Right() == WC_SABER_SHORT || Weapon_Right() == WC_SABER_DOUBLE;
    bool melee = Weapon_Right() != WC_NONE && Weapon_Profile(Weapon_Right()).melee;
    double t = Now();
    if (ranged) {
        // cover: the shooter sees neither your head nor your chest (the game's own line of sight) - the wall takes it
        float head[3] = {me[0], me[1], me[2] + 1.6f}, from[3] = {them[0], them[1], them[2] + 1.4f};
        DWORD meId = ObjectId(Weapon_PlayerCreature());
        if (!Melee_LineOfSight(from, head, attacker, meId) && !Melee_LineOfSight(from, chest, attacker, meId)) {
            for (int i = 0; i < 15; i++)
                if (ints[i] > 0) ints[i] = 0;
            VR_Haptic(1, 0.3f, 0.03f, 250);
            Log("combat: bolt from %08lX hit your cover", attacker);
            return true;
        }
        // the bolt's last 1.5 m before your chest: a saber across it deflects it; a fast one sends it back
        float a[3] = {chest[0] + dx * 1.5f, chest[1] + dy * 1.5f, them[2] + 1.3f};
        float speed = 0;
        if (saber && Melee_BladeCrossesLine(a, chest, 0.22f, &speed)) {
            for (int i = 0; i < 15; i++)
                if (ints[i] > 0) ints[i] = 0;
            if (speed >= 1.6f) {
                void* shooter = Weapon_CreatureById(attacker);
                if (shooter) {
                    // the same bolt now strikes the shooter (its damage restored)
                    for (int i = 0; i < 15; i++) ints[i] = 0;
                    ints[0] = dealt > 0 ? dealt : 1;  // physical slot 0 carries the returned damage
                    *targetSlot = shooter;
                    float to[3] = {them[0], them[1], them[2] + 1.2f};
                    Weapons_FireBolt(to, 0.0f);
                    Anim_HitPause(attacker, 0.08f);
                    Anim_Flinch(attacker, a);
                }
                Cues_Fire(CUE_PERFECT_BLOCK);
            Guard_Result(attacker, 2);
                Log("combat: bolt from %08lX deflected back at it (%d damage, blade %.1f m/s)", attacker, dealt, speed);
            } else {
                Cues_Fire(CUE_BLOCK);
                Log("combat: bolt from %08lX deflected (blade %.1f m/s)", attacker, speed);
            }
            return true;
        }
        if (Gestures_Dodged(0.45)) {  // leaned / ducked out of its path
            for (int i = 0; i < 15; i++)
                if (ints[i] > 0) ints[i] = 0;
            Cues_Fire(CUE_DODGE);
            Log("combat: bolt from %08lX dodged", attacker);
            return true;
        }
        if (dealt > 0) Cues_Fire(CUE_HURT);
        return true;
    }
    // a blow: your blade on its block bar (guard.cpp); without a bar (no fresh data) the old strike zone in front of
    // your chest on its side
    float zone[3] = {me[0] + dx * 0.55f, me[1] + dy * 0.55f, me[2] + 1.2f};
    float speed = 0;
    bool parry = false, haveBar = false, onBar = melee && Guard_Judge(attacker, &parry, &speed, &haveBar);
    if (melee && !haveBar && Melee_BladeNear(zone, g_parryZone, &speed)) {
        onBar = true;
        parry = speed >= 1.6f;  // moved into the blow, not just held there
    }
    if (onBar) {
        if (!parry && g_guard <= 0) {
            Cues_Fire(CUE_BLOCK_FAILED);
            if (dealt > 0) Cues_Fire(CUE_HURT);
            Guard_Result(attacker, 3);
            Log("combat: block broken (no guard left), %d damage", dealt);
            return true;
        }
        for (int i = 0; i < 15; i++)
            if (ints[i] > 0) ints[i] = 0;
        if (parry) {
            HitPosture(attacker, 35, "parried");
            Anim_HitPause(attacker, 0.2f);
            Anim_Flinch(attacker, zone);
            g_guard = g_guard + 15 > 100 ? 100 : g_guard + 15;
            Force_OnParry();
            Cues_Fire(CUE_PERFECT_BLOCK);
            Log("combat: parried %08lX (blade %.1f m/s), %d damage stopped", attacker, speed, dealt);
        } else {
            g_guard -= 25;
            g_lastBlockAt = t;
            HitPosture(attacker, 8, "blocked");
            Cues_Fire(CUE_BLOCK);
            Guard_Result(attacker, 1);
            Log("combat: blocked %08lX, %d damage stopped, guard %.0f", attacker, dealt, g_guard);
        }
        return true;
    }
    if (!melee && Gestures_Dodged(0.45)) {
        for (int i = 0; i < 15; i++)
            if (ints[i] > 0) ints[i] = 0;
        Cues_Fire(CUE_DODGE);
        return true;
    }
    if (dealt > 0) Cues_Fire(CUE_HURT);
    if (melee) Guard_Result(attacker, 3);
    Log("combat: blow from %08lX lands, %d damage (no blade on the bar)", attacker, dealt);
    return true;
}

extern "C" void __cdecl Combat_DamagePre(void** targetSlot, void* effect) {
    void* targetObj = *targetSlot;
    int* ints;
    if (!IsWeaponDamage(effect, &ints)) return;
    void* player = Weapon_PlayerCreature();
    if (!player || AsCreature(targetObj) != player) return;
    int dealt = 0;
    for (int i = 0; i < 15; i++) dealt += ints[i] > 0 ? ints[i] : 0;
    if (g_contact) {
        ContactDefence(targetSlot, effect, ints, dealt);
        g_effectFrom.erase(effect);
        return;
    }
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
        lea eax, [esp + 0xC]          // &target (arg 1): a deflected bolt is re-aimed at its shooter
        push eax
        call Combat_DamagePre
        add esp, 8
        pop ecx
        jmp dword ptr [g_damageTramp]
    }
}

bool Combat_Detour(unsigned long va, const unsigned char* orig, int len, void* stub, unsigned char** tramp,
                   const char* what) {
    BYTE* target = (BYTE*)(ULONG_PTR)va;
    if (memcmp(target, orig, len) != 0) {
        Log("hook: unexpected bytes at %08lX (%s), not hooking", va, what);
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
    Log("hook: %s at %08lX", what, va);
    return true;
}

bool Combat_Install() {
    InitializeCriticalSection(&g_lock);
    char ini[MAX_PATH];
    GetModuleFileNameA(nullptr, ini, MAX_PATH);
    if (char* s = strrchr(ini, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - ini), "kvr\\kvr.ini");
    g_impactBlock = GetPrivateProfileIntA("Combat", "ImpactBlock", 1, ini) != 0;
    char mode[32];
    GetPrivateProfileStringA("Combat", "Mode", "contact", mode, sizeof mode, ini);
    g_contact = _stricmp(mode, "rounds") != 0;
    char buf[32];
    GetPrivateProfileStringA("Combat", "ParryZone", "0.38", buf, sizeof buf, ini);
    g_parryZone = (float)atof(buf);
    g_tokens = GetPrivateProfileIntA("Combat", "AttackTokens", 2, ini);
    Log("combat: mode %s (parry zone %.2f m, %d attack tokens)", g_contact ? "contact" : "rounds", g_parryZone, g_tokens);
    bool ok = Combat_Detour(kResolverVA, kResolverOrig, 6, ResolverStub, &g_resolverTramp, "attack resolver");
    if (ok) Combat_Detour(kQueueVA, kQueueOrig, 8, QueueStub, &g_queueTramp, "event queue");
    bool dmg = ok && Combat_Detour(kDamageVA, kDamageOrig, 7, DamageStub, &g_damageTramp, "damage handler");
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
    if (g_clearShotAt > 0 && t >= g_clearShotAt) {
        g_clearShotAt = -1;
        if (ActionRing_ClearQueue()) {
            Combat_ResetAttacking();
            Log("combat: shot done - the game's attack cleared (one pull, one shot)");
        }
    }
    bool fighting = Combat_AttackWithin(6000);
    if (g_contact) {
        static double last = t;
        float dt = (float)(t - last);
        last = t;
        if (t - g_lastBlockAt > 1.0) g_guard = g_guard + 20 * dt > 100 ? 100 : g_guard + 20 * dt;
        for (auto& kv : g_posture) {
            Posture& p = kv.second;
            if (p.openUntil > 0 && t >= p.openUntil) {  // the opening closes: its round runs again
                p.openUntil = -10;
                p.value = p.max * 0.5f;
                SetRoundPaused(kv.first, false);
            } else if (t >= p.openUntil && t - p.lastHit > 1.5 && p.value < p.max) {
                p.value = p.value + 15 * dt > p.max ? p.max : p.value + 15 * dt;
            }
        }
        // the player's rolls: a miss on an enemy your blade did touch = it blocked you -> its posture takes it
        for (size_t i = 0; i < g_playerPending.size();) {
            PendingRoll& pr = g_playerPending[i];
            if (t - pr.at < 0.08) {
                i++;
                continue;
            }
            int result = 0;
            __try {
                result = *(int*)(pr.record + 0x5C);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            // (melee only: a blade can be blocked; for a shot this ate the hit's credit and the blow that then landed
            // did no damage - 2026-11-05)
            bool melee = Weapon_Profile(Weapon_Right()).melee || Weapon_Right() == WC_NONE;
            if (melee && result == 4 && Melee_HasCredit(pr.target)) {
                Melee_TakeCredit(pr.target, nullptr, nullptr);
                HitPosture(pr.target, 20, "blocked your blow");
                Cues_Fire(CUE_BLOCK);
                Log("combat: %08lX blocked your blow (d20 miss) -> posture", pr.target);
            }
            g_playerPending.erase(g_playerPending.begin() + i);
        }
    }
    EnterCriticalSection(&g_lock);
    if (Features().telegraphTest) {  // harness: keep re-arming a 1.2 s telegraph from the last real attacker
        static DWORD lastAttacker;
        static double lastFake;
        static bool ranged;
        if (!g_incoming.empty() && g_incoming.back().record) lastAttacker = g_incoming.back().attacker;
        if (lastAttacker && g_incoming.empty() && t - lastFake > 2.0) {
            lastFake = t;
            ranged = !ranged;
            g_incoming.push_back({lastAttacker, nullptr, t, t + 1.2, false, ranged});
            Log("combat: telegraph_test from %08lX (%s)", lastAttacker, ranged ? "ranged" : "melee");
        }
    }
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

bool Combat_ContactMode() { return g_contact; }

bool Combat_PlayerAttacking() { return Now() - g_playerRolls[1] < 3.5; }
void Combat_ResetAttacking() { g_playerRolls[0] = g_playerRolls[1] = -100; }

// The player's combat round holds the character in place until its timer runs out (KotOR: an attack plays out its
// round), even with the action queue cleared (2026-11-05: 0.5-1.3 s with the stick pushed away after a strike).
// Run the timer out so the round ends now (round: [creature + 0x9C8], Timer +0x944, RoundLength +0x94C). A round waiting on the opponent's (Paused +0x958) keeps its timer at 0 regardless.
void Combat_EndPlayerRound() {
    __try {
        BYTE* p = (BYTE*)Weapon_PlayerCreature();
        BYTE* round = p ? *(BYTE**)(p + 0x9C8) : nullptr;
        if (!round || !*(round + 0x934)) return;
        long len = *(long*)(round + 0x94C), timer = *(long*)(round + 0x944);
        if (timer < len) {
            *(long*)(round + 0x944) = len;
            static DWORD lastLog;
            if (GetTickCount() - lastLog > 500) Log("combat: the player's round ended early (%ld of %ld ms) - movement is yours", timer, len);
            lastLog = GetTickCount();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int Combat_IncomingWarnings(CombatWarning* out, int max) {
    double t = Now();
    int n = 0;
    EnterCriticalSection(&g_lock);
    for (const Incoming& in : g_incoming) {
        if (n >= max) break;
        if (in.due < 0) continue;
        double left = in.due - t;
        if (left > 3.0 || left < -0.15) continue;  // shown from the roll (Phase A telegraph)
        out[n].attacker = in.attacker;
        out[n].secondsLeft = (float)left;
        out[n].total = (float)(in.due - in.rolled);
        out[n].ranged = in.ranged;
        n++;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}
