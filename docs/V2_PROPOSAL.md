# KotOR VR v2 proposal: real-time, physical combat

Status: proposal only (2026-10-27). Nothing here is implemented. v1 keeps KotOR's d20 rounds with VR gestures,
timing bonuses and impact-time blocks layered on top; v2 makes what your hands physically do decide what happens,
with the d20 kept as the damage and defence model underneath.

Reference: d2r-pcvr (github.com/bearstonem/d2r-pcvr) solved the same problem for Diablo II - swept melee contact,
shield blocks from real geometry, parries timed to measured enemy wind-ups, hand-launched projectiles. Its ideas map
onto ours where noted.

## Goals

1. A hit lands because your blade touched the enemy - not because a round came up.
2. Blocks and parries happen because your blade met theirs (or the bolt), not because a guard pose was held.
3. Every contact is felt and seen at the point of contact (haptics, sparks, sound, enemy reaction).
4. KotOR's progression still matters: feats, skills, Force powers, armour and stats scale the outcome.

## Pillars

### 1. Contact-driven melee (L)
- Each frame sweep the weapon's blade segment (previous -> current pose) as a capsule against enemy hit volumes
  (cylinders from creature position + appearance size, later per-bone capsules from the skinned skeleton we already
  read for hands). d2r-pcvr: `melee_contact.h`, segment-to-segment swept contacts per hand.
- A contact with enough blade speed queues a real attack on that enemy *now* (force the attack resolver for this
  target out of round, or inject the damage effect through the damage handler we already hook), with the d20 roll
  still applied (attack bonus vs defence) - so a weak character still misses some contacts, but nothing lands
  without contact.
- Swing speed and edge alignment scale damage (a slap with the flat does little); special-move gestures stay
  (overhead = Power Attack etc.) but resolve on contact.
- Disable KotOR's own auto-attack loop for the player while contact combat is on (no round-driven swings).

### 2. Physical defence (M-L)
- Enemy attacks: we already know when each attack resolves (combat timeline). Add the attacker's weapon pose and
  measured wind-up per creature type (d2r-pcvr records each monster's attack-start -> hit-frame time).
- Block = your blade intersects the incoming blade's path (or a cone between attacker weapon and your body) at the
  hit frame; parry = a swing into it inside a ~200 ms window (+ late grace), which staggers the attacker and opens a
  riposte window (guaranteed clean hit, its own sound). Bosses excluded or harder.
- Blaster bolts become real projectiles toward you (we already draw bolts): a saber that crosses a bolt deflects it
  - back toward the shooter with a Deflect skill check, or away.

### 3. Ranged as real guns (M)
- v1 already fires on the trigger pull. v2 makes the bolt authoritative: it travels, and hits what it reaches (ray
  against hit volumes, line of sight) - then the d20 decides damage. Light aim assist (d2r-pcvr: 4/12 degree cone,
  visible line only, 750 ms sticky) keeps it fair at range.
- Dual wield: each hand's trigger fires its own gun (the HUD removal for v1 frees the left trigger for this).

### 4. Reactions and feedback (M)
- Enemy hit reactions on contact (play the game's own damage / knockdown animations), stagger on parries.
- Haptics per contact type and strength (d2r-pcvr `haptic_strength.h`); guard-strain vignette on heavy blocks;
  low-health heartbeat.

### 5. Force powers by gesture (M)
- v1 maps push / pull gestures to action slots. v2: aim with the hand (target = what the palm faces), power
  chosen by gesture shape (push, lift, lightning = fingers spread), charge by holding.

## Risks

- Taking attacks out of KotOR's round system touches combat AI and action queues: highest regression risk in the
  project. Mitigation: a kvr.ini switch (`[Combat] Mode=rounds|contact`), contact mode off by default until proven.
- Hit volumes from creature size are coarse; per-bone volumes need the animated skeleton of other creatures
  (we only read our own hands today).
- Balance: contact makes the player far stronger (no misses from rounds). Needs tuning per difficulty.

## Rough order and effort

1. Swept-contact melee behind a switch, cylinders, damage via the resolver (L)
2. Projectile bolts + saber deflection (M)
3. Physical blocks / parries with measured wind-ups (M-L)
4. Reactions + feedback pass (M)
5. Gesture Force powers (M)

Estimate: 6-10 working sessions for 1-4, playtested between each.
