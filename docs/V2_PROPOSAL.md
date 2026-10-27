# KotOR VR v2 proposal: real-time, physical combat

Status: in progress on branch v2 - see "Status" below. v1 keeps KotOR's d20 rounds with VR gestures,
timing bonuses and impact-time blocks layered on top; v2 makes what your hands physically do decide what happens,
with the d20 kept as the damage and defence model underneath.

Reference: d2r-pcvr (github.com/bearstonem/d2r-pcvr) solved the same problem for Diablo II - swept melee contact,
shield blocks from real geometry, parries timed to measured enemy wind-ups, hand-launched projectiles. Its ideas map
onto ours where noted.

## Status (2026-10-27, branch v2)

Built ([Combat] Mode=contact, the default; Mode=rounds restores v1):
- Contact melee against per-bone capsules from each enemy's animated skeleton (melee.cpp, anim.cpp); a contact
  starts your attack; only credited blows land, graded by swing speed; a credited d20 miss is blocked into posture.
- Defence at the impact by blade geometry: block (guard meter) / parry (stagger + posture), bolt deflection and
  return-to-shooter, dodges; roll-time telegraphs (Phase A).
- Posture and openings (the enemy's combat round pauses, your blows become critical threats); attack tokens.
- Ranged: the barrel decides (per-bone, head shots 1.5x); bolts stop at walls; incoming bolts dodged / deflected.
- Reactions: per-creature hit-pause, torso flinch, NPCs looking at you, death camera.
- Force: pushes aimed by the left palm (the enemy it points at becomes the target).

- Force by gesture (force.cpp): gather with the left grip, the release picks the family (shove / lift / hold / left
  trigger), the hold the tier, two hands the area power; chest-cast buffs and a wrist power menu; Saber Throw by
  letting go mid-swing (catch by squeezing again); parries refund Force. Cast through the game's own action menus.
- Blasters: spread (hand motion, foregrip, steadiness), Sniper / Rapid / Power Blast fire modes, thrown grenades with a
  simulated arc, cover from the game's line of sight.

Left out by decision (2026-10-27): death ragdoll, arm IK, slow-motion battle meditation.

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
Rule (as for melee): the dice never contradict what you physically did.

Your shots
- Aim decides hit or miss: each trigger pull casts along the real barrel against per-bone capsules of the target
  (NPC skeletons are readable since the 2026-10-27 node probe, FINDINGS.md "Client animation nodes"). The d20 grades
  the hit - graze / clean / crit - instead of deciding it. Headshot raises crit threat; limb hits react on that limb.
- Feats become fire modes: Rapid Shot = higher rate, more spread; Power Blast = hold the trigger to charge; Sniper
  Shot = rewards holding steady (steadiness shown on the weapon, no scope); Two-Weapon Fighting = dual pistols on
  both triggers, aimed independently, the feat penalties applied as spread.
- Off hand on a rifle's foregrip tightens spread and recoil.
- Feel: recoil kick on the weapon model (never the camera), tracers, impact sparks / scorches, per-weapon haptics,
  short hit-pause on kills.
- Optional aim magnetism, a small cone that grows with Blaster skill (d2r-pcvr Light / Strong: 4/12 degrees,
  visible line only, 750 ms sticky); never a guaranteed hit.

Incoming fire
- Enemy shots are rolled ~0.5 s before impact: muzzle glint on the shooter at roll time, the bolt flies visibly.
- Dodge and cover: head / body out of the bolt's path at impact, or the engine's line of sight blocked by cover,
  turns the hit into a miss (leaning out of a bolt is the ranged parry). Needs the engine's LOS function (Lane's
  labels).
- Saber deflection as in pillar 2.

Enemies and grenades
- Hit reactions where the bolt lands via the bone layer (spine flinch, head snap; knockdown on crit / Power Blast
  through the engine's knockdown effect).
- Grenades: physical throw arc (d2r-pcvr hand-launched projectiles); the engine resolves the blast at the landing
  point.

Not doing: reloads, ammo, heat - KotOR has none, and Until You Fall's lesson is not to add interactions whose only
effect is new ways to fail.

Order: barrel hits + bone reactions, then telegraphed bolts + dodge / cover, then fire modes + foregrip, grenades.

### 4. Reactions and feedback (M)
- Enemy hit reactions on contact (play the game's own damage / knockdown animations), stagger on parries.
- Haptics per contact type and strength (d2r-pcvr `haptic_strength.h`); guard-strain vignette on heavy blocks;
  low-health heartbeat.

### 5. Force powers by gesture (M)
K1 has ~15 power families with up to 3 tiers (40-odd powers). A small gesture vocabulary with depth inside each
gesture, not more gestures - a misread gesture is a new way to fail (Until You Fall).
- Gesture = family: push (palm-out shove; JKXR classifier: > 2.09 m/s, > 0.15 m travel, palm direction), pull
  (palm-in yank; also Saber Throw's catch), grip / choke (close the hand on a target, lift), lightning (fingers
  spread, held), stasis / stun (palm out, held still).
- How you do it = tier: one hand = single target (Push, Lightning, Stun); both hands together = area (Whirlwind ->
  Wave, Storm, Stasis Field, Death Field); holding longer charges up to the highest tier known (B&S More Star Wars
  Repulse). The bigger, two-handed version is the stronger one.
- Aim with the palm, lock before release (Half-Life: Alyx highlight -> lock -> confirm): the faced target is
  highlighted and the power fires only on a locked target - no misfires into walls or companions.
- The rest on the wrist panel: self-buffs, mind and droid powers (Speed, Valor / Armor, Heal, Affect Mind, Destroy
  Droid, Drain Life) on a hand-hover radial (Skyrim VR Spell Wheel); opening it can use KotOR's own pause. Self-buffs
  cast with hand-to-chest (hold = Heal, touch and push out = Speed). The radial also chooses what the "held still"
  and "spread" gestures cast (Stun vs Slow etc.).
- Perfect parries and deflects refund a little Force (Fallen Order), so mixing saber and Force is the efficient play.
- Juice: hand glow tinted by alignment, haptic hum rising while charging, release burst, short hit-pause on the
  target, the engine's own force-pushed / choke / stun reactions; a resisted Will save shows a shimmer.
- Rules stay KotOR's: Force point cost, Will save vs the power's DC, tiers gated by what you know; each family gets a
  short recovery instead of a round lock.
- Risk: cast latency - casting queues KotOR's cast action, whose animation plays before the effect lands. Measure
  first; fallback is applying the effect directly (force-pushed, stun) while charging FP and rolling the save
  ourselves.
- Order: push / pull with palm lock, charge tiers + two-handed AoE, wrist radial + chest-cast buffs, Saber Throw
  with catch.

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
