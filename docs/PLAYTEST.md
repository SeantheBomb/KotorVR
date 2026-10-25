# Playtest guide

Start: quit Discord fully, connect Air Link, run `host\build\kvr_host.exe`, launch KotOR from Steam.

One-time (already done on this PC): export the weapon models the hands use, from your own game files:
`python tools\weapon_export.py "<game dir>" "<game dir>\kvr"` (writes `kvr\models` and `kvr\textures`).
Without it the hands fall back to simple stand-in shapes.

## Controls

| Input | Action |
|---|---|
| Head | look (engine renders and culls from your head) |
| Left stick | walk where you look (8-way) |
| Right stick X | turn (smooth, game camera) |
| Right stick click | recenter |
| Right hand pointing | KotOR's own hover highlight (no beam in the world) |
| Right grip | select / use / talk to what your hand points at |
| Right trigger on the HUD / menus | click (laser + white dot) |
| **In combat** — melee swing | attack the current target |
| **In combat** — right trigger | attack the current target (ranged: steady aim = better shot) |
| **In combat** — left hand thrust forward | target action slot 2 (Force powers) |
| **In combat** — left hand yank back | personal power slot (heal, speed, …) |
| A | pause (Space) |
| B / Menu | Escape |
| X | switch character (Tab) |
| Y | hide / show the HUD |

Attack gestures only fire while KotOR is in combat (outside it, KotOR's Default Action would *use* the target).
Start a fight by gripping on an enemy.

## Combat skill layer (d20 untouched otherwise)

- **Your attacks are on a beat.** Once a fight is going, KotOR attacks once per round. Swing (or pull the trigger) in
  the last 0.6 s before your attack rolls: +4 to the d20. Within 2 s: +2. The weapon pulses gently and the hand ticks
  when that window opens; a swing inside it flares the blade with a heavy thump and the weapon's own swing whoosh.
  A natural 1 still misses. Log: `combat: player_attack, gesture X s before the roll`.
- **Blocks happen when the blow lands** (`[Combat] ImpactBlock=1`, default). An amber chevron appears over an enemy
  about to hit you; it turns white-gold in the last 0.4 s. Hold your melee weapon across your body:
  - raised into the blow (guard up for 0.4 s or less at impact): **parry**, 95%: bright flash, sharp buzz, saber clash
    (or the weapon's parry sound)
  - guard held longer: block, 60%; head dodge (sideways / duck 18 cm): 50%, both hands buzz
  - failed: dull double pulse, you take the hit. Log: `combat: impact ...`
  A stopped hit still plays KotOR's hit animation on the enemy side; you just take no damage.
- Weapons in hand are detected from your equipped items (all KotOR classes: sabers single/short/double, swords,
  vibroblades, double blades, batons, staffs, pistols, rifles, repeaters); two-handed melee spans both hands.
- Sounds are KotOR's own (`python tools\sound_export.py "<game dir>"`, already done here), at your Sound Effects
  volume. Swap any in `kvr.ini [Sounds]` (e.g. `BlockSaber=cb_ls_clash2`, empty = silent).
- KotOR's own "Enemy sighted" auto-pause is on in your options: press A to unpause.

## What to judge (feel — the harness covers the mechanics)

1. Swing threshold: too easy / too hard to trigger? (`gesture: swing` lines show peak speed and travel)
2. Does the weapon sit right in the hand (angle, length, scale)?
3. Block/dodge: readable? too strong / too weak?
4. Force thrust / yank: do they fire when meant to, and not by accident?
5. Ranged: does a steady aim feel rewarded?

## Weapons in your hands

The equipped item's real model and textures (its variation, so your saber is your saber: hilt, blade colour).
`[Debug] WeaponGallery=1` in kvr.ini cycles through one weapon of every class every 3 s (for screenshots).

## If something misbehaves

Edit `<game>\kvr\kvr.ini` (written on first launch) and restart the game:

```
[Features]
EngineCamera=1    ; 0 = old GL-level head tracking (culling follows the game camera again)
WeaponProxies=1   ; 0 = no weapons drawn in the hands
Gestures=1        ; 0 = swings / shots / Force gestures do nothing
Combat=1          ; 0 = no timing bonus or block / dodge on the d20
Cues=1            ; 0 = no haptics / weapon glow / cue sounds / warning chevrons

[Combat]
ImpactBlock=1     ; 0 = old behavior: block / dodge decided at the roll (no parry window, warnings only cosmetic)
```

Logs: `<game>\kvr\probe.log` (game layer), the host's terminal window.
