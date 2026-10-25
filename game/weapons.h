// Equipped-weapon detection, weapon profiles, in-hand proxy rendering and swing detection.
#pragma once
#include <windows.h>

enum WeaponClass {
    WC_NONE,          // unarmed / nothing equipped
    WC_SABER,         // lightsaber
    WC_SABER_SHORT,   // short lightsaber
    WC_SABER_DOUBLE,  // double-bladed lightsaber (two-handed)
    WC_BLADE,         // swords, vibroblades
    WC_BLADE_SHORT,   // short sword, vibroblade
    WC_BLADE_DOUBLE,  // double-bladed sword, vibro double-blade (two-handed)
    WC_BATON,         // stun baton (one-handed blunt)
    WC_STAFF,         // quarterstaff (two-handed blunt)
    WC_PISTOL,        // all blaster/ion/disruptor/sonic pistols
    WC_RIFLE,         // rifles, carbines, bowcaster (two-handed)
    WC_REPEATER,      // repeating / heavy repeating blasters (two-handed, heavy)
    WC_COUNT
};

struct WeaponProfile {
    const char* name;
    bool melee;
    bool twoHanded;
    float swingSpeed;  // m/s at the hand for a committed melee swing (0 = n/a)
    float swingTravel; // m of hand travel within the swing window
};

const WeaponProfile& Weapon_Profile(WeaponClass c);
WeaponClass Weapon_Right();
WeaponClass Weapon_Left();
// Server-side player creature (party leader), or null; refreshed by Weapons_Update.
void* Weapon_PlayerCreature();
// The player's combat round is running (KotOR is in combat with something).
bool Weapon_PlayerInCombat();

// Once per frame (game thread).
void Weapons_Update();
// Draw the proxies for both hands into the world, before the eye image is captured. `invEye` is the inverse eye
// pose in the recenter-origin frame (column-major), `zn` the near plane; the projection must already be the eye's.
void Weapons_Draw(const float invEye[16]);
// Arkham-style warning glyphs over enemies whose attack on the player is about to land (combat.cpp timeline).
// `worldView` is the eye's world modelview (KotOR meters); the projection must already be the eye's.
void Weapons_DrawWarnings(const float worldView[16]);
// Server creature for an object id, or null.
void* Weapon_CreatureById(DWORD id);
