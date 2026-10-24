# Playtest guide

Start: quit Discord fully, connect Air Link, run `host\build\kvr_host.exe`, launch KotOR from Steam.

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

- A swing (melee) or trigger pull (ranged) within 2 s before your attack resolves gives +2..+4 to a fresh d20
  (harder swing / steadier aim = more). A natural 1 still misses. Log line: `combat: player attack ...`.
- When an enemy attacks you: holding a melee weapon raised across your body (block / deflect) gives a 60% chance
  to turn the hit into a miss; a sharp dodge (head sideways or ducking ≥ 18 cm) gives 50%. Log: `combat: incoming`.
- Weapons in hand are detected from your equipped items (all KotOR classes: sabers single/short/double, swords,
  vibroblades, double blades, batons, staffs, pistols, rifles, repeaters); two-handed melee spans both hands.

## What to judge (feel — the harness covers the mechanics)

1. Swing threshold: too easy / too hard to trigger? (`gesture: swing` lines show peak speed and travel)
2. Does the weapon sit right in the hand (angle, length, scale)?
3. Block/dodge: readable? too strong / too weak?
4. Force thrust / yank: do they fire when meant to, and not by accident?
5. Ranged: does a steady aim feel rewarded?

## If something misbehaves

Edit `<game>\kvr\kvr.ini` (written on first launch) and restart the game:

```
[Features]
EngineCamera=1    ; 0 = old GL-level head tracking (culling follows the game camera again)
WeaponProxies=1   ; 0 = no weapons drawn in the hands
Gestures=1        ; 0 = swings / shots / Force gestures do nothing
Combat=1          ; 0 = no timing bonus or block / dodge on the d20
```

Logs: `<game>\kvr\probe.log` (game layer), the host's terminal window.
