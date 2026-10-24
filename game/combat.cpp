#define _CRT_RAND_S
// Skill layer over KotOR's d20 combat (x86, inside swkotor.exe).
//
// 0x5BACA0 is the attack-roll resolver: thiscall, ecx = attacking creature, first stack arg = target object,
// ret 0xC. Right after it rolls the d20 (RollDice(1,20) at 0x5BAD02) it checks the debug global 0x832874:
// if non-zero, that value replaces the natural roll. Nothing else in the game reads or writes that global.
//
// We detour the resolver and set the global per attack:
//   player attacking   -> a timed swing / steady shot in the last 2 s adds +2..+4 to a fresh d20 (a natural 1
//                         still misses); KotOR then plays its own hit/miss/parry animation for the result
//   player attacked    -> guarding with a melee weapon (Jedi: saber block / blaster deflect) or a dodge in the
//                         window turns the hit into a miss (natural 1) with a chance, so defense is skill, not
//                         invulnerability
//   anyone else        -> 0 (the game rolls normally)

#include "combat.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>

#include "gestures.h"
#include "weapons.h"

extern void Log(const char* fmt, ...);

namespace {

const DWORD kResolverVA = 0x5BACA0;
const BYTE kResolverOrig[6] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};  // mov eax, fs:[0]
volatile DWORD* const kForcedRoll = (volatile DWORD*)0x832874;

BYTE* g_tramp;

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

}  // namespace

extern "C" void __cdecl Combat_ResolverPre(void* attacker, void* targetObj) {
    *kForcedRoll = 0;
    void* player = Weapon_PlayerCreature();
    if (!player) return;
    void* target = targetObj ? AsCreature(targetObj) : nullptr;

    if (attacker == player) {
        float q = Gestures_ConsumeRecentAttack(2.0);
        if (q <= 0) return;
        unsigned nat = D20();
        int bonus = (int)(2.0f + 2.0f * (q - 1.0f) + 0.5f);  // quality 1..2 -> +2..+4
        unsigned forced = nat == 1 ? 1 : (nat + bonus > 20 ? 20 : nat + bonus);
        *kForcedRoll = forced;
        Log("combat: player attack, quality %.2f: d20 %u +%d -> %u", q, nat, bonus, forced);
        return;
    }
    if (target == player) {
        bool melee = Weapon_Profile(Weapon_Right()).melee && Weapon_Right() != WC_NONE;
        bool guard = melee && Gestures_Guarding(0.6);
        bool dodge = Gestures_Dodged(0.6);
        if (!guard && !dodge) return;
        unsigned roll = D20();
        unsigned chance = guard ? 13 : 11;  // 60% block / 50% dodge, on top of the normal defense roll
        if (roll <= chance) {
            *kForcedRoll = 1;
            Log("combat: incoming attack %s (d20 %u <= %u) -> miss", guard ? "blocked" : "dodged", roll, chance);
        } else {
            Log("combat: incoming attack, %s failed (d20 %u > %u)", guard ? "block" : "dodge", roll, chance);
        }
    }
}

__declspec(naked) static void ResolverStub() {
    __asm {
        push ecx                      // keep the attacker for the real resolver
        push dword ptr [esp + 8]      // target (first stack argument)
        push ecx                      // attacker
        call Combat_ResolverPre
        add esp, 8
        pop ecx
        jmp dword ptr [g_tramp]       // run the original with untouched stack/registers; it returns to our caller
    }
}

bool Combat_Install() {
    BYTE* target = (BYTE*)(ULONG_PTR)kResolverVA;
    if (memcmp(target, kResolverOrig, sizeof kResolverOrig) != 0) {
        Log("combat: unexpected bytes at %08lX, not hooking", kResolverVA);
        return false;
    }
    g_tramp = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(g_tramp, target, 6);
    g_tramp[6] = 0xE9;
    *(INT32*)(g_tramp + 7) = (INT32)((target + 6) - (g_tramp + 11));
    DWORD old;
    VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(INT32*)(target + 1) = (INT32)((BYTE*)ResolverStub - (target + 5));
    target[5] = 0x90;
    VirtualProtect(target, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    Log("combat: hooked attack resolver at %08lX", kResolverVA);
    return true;
}

void Combat_EndFrame() { *kForcedRoll = 0; }
