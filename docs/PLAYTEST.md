# Playtest guide

Start: connect Air Link, run `host\build\kvr_host.exe`, launch KotOR from Steam.
The host opens a desktop window ("KotOR VR - headset view") mirroring the left eye with the HUD, or the menus /
movie on their own; close it if you don't want it. The game's own window stays black.

One-time (already done on this PC), from your own game files:
- `python tools\weapon_export.py "<game dir>" "<game dir>\kvr"`: weapon models (else simple stand-in shapes)
- `python tools\hand_export.py "<game dir>" "<game dir>\kvr"`: your character's hands, per body model and outfit
- `python tools\sound_export.py "<game dir>"`: the combat cue and lightsaber sounds

## Controls

| Input | Action |
|---|---|
| Head | look (engine renders and culls from your head) |
| Left stick | walk where you look (8-way) |
| Right stick X | turn (smooth, game camera) |
| Right stick click | recenter |
| Right hand pointing | KotOR's own hover highlight (no beam in the world) |
| Right grip | use / talk to / open / attack what your hand points at (no mouse click: never fires a HUD slot) |
| Right grip **held** (0.4 s) | action ring for what you point at: its other actions by name (Security, Bash, Critical Strike, Force Push, grenades, …); point + trigger to do one; grip again / B / 10 s closes it |
| Right trigger in menus | click (laser + white dot) |
| **In the world** — left hand on the HUD + left trigger | click the HUD (laser from the left hand). The right trigger never touches the HUD: it is always your weapon |
| **Conversations** — point at a reply + right trigger | pick it (panel floats in front of you: the speaker's line on top, replies below; the laser shows while you point at it) |
| **In combat** — melee swing | attack the current target (once attacking, swings only count for timing) |
| **In combat** — overhead chop / forward thrust / 2 quick swings | Power Attack / Critical Strike / Flurry (best rank you have) |
| Right trigger with a blaster (anywhere in the world) | fires at once: bolt down the barrel, muzzle flash, recoil, the gun's own sound |
| **In combat** — right trigger | also attacks the target you point at (KotOR's roll decides hits; steady aim = better shot) |
| **In combat** — shot after 1.2 s steady aim / 2 quick shots | Power Blast / Rapid Shot (best rank you have) |
| **In combat** — left hand thrust forward | target action slot 2 (Force powers) |
| **In combat** — left hand yank back | personal power slot (heal, speed, …) |
| A | pause (Space) |
| A / B during a cutscene movie | skip it (movies play on a cinema screen 3 m ahead, where you were looking) |
| B / Menu | Escape |
| X | switch character (Tab) |
| Y | hide / show the HUD |

Cutscenes stay in first person whenever you can see what the shot shows (a hidden probe finds the shot's subject,
then checks your line of sight to it); `[Comfort] CutsceneFirstPerson=1` in kvr.ini never cuts away.

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

## Lightsaber, hands, your body

- The blade is off outside combat (hilt only), ignites when a fight starts and shuts off 4 s after it ends, with
  KotOR's own ignite / shut-down sounds. While lit it hums; the hum rises with blade speed and fast strokes whoosh.
- Your hands are your character's own (cut from their body model), so gloves / skin follow the outfit and the
  character you control (Tab). Grip curls the fingers, trigger the index finger; a held weapon closes the fist.
  A Force gesture makes the off hand surge blue with a low rumble.
- Only your own model is hidden (exactly, not by distance): NPCs next to you, floors and walls stay.
- Cutscenes keep you in your character's head and follow them if the scene walks them somewhere.
- Hits you land: a thump and a spark burst when the blow connects (heavier on an on-beat attack).

## What to judge (feel — the harness covers the mechanics)

1. Swing threshold: too easy / too hard to trigger? (`gesture: swing` lines show peak speed and travel)
2. Does the weapon sit right in the hand (angle, length, scale)?
3. Block/dodge: readable? too strong / too weak?
4. Force thrust / yank: do they fire when meant to, and not by accident?
5. Ranged: does a steady aim feel rewarded?
6. Hands: placement on the controllers, size, finger curl.
7. Special moves: do the chop / thrust / quick swings fire when meant to, and not by accident?
8. Picture: distant faces and textures (16x anisotropic + 4x MSAA now on in swkotor.ini).

## Weapons in your hands

The equipped item's real model and textures (its variation, so your saber is your saber: hilt, blade colour).
`[Debug] WeaponGallery=1` in kvr.ini cycles through one weapon of every class every 3 s (for screenshots).

## v2 Phase A: combat feel (no rule changes)

- Telegraphs from the moment an enemy rolls its attack on you: a chevron over the attacker with a ring that closes
  onto it at the impact, and a diamond in front of your chest on its side (where the blow arrives - put your blade
  there). Blue = block / parry it, red = dodge it (a bolt you can't deflect). Gold, closed ring, filled diamond =
  the parry window.
- A block throws your weapon off your hand for 0.2 s (about 40 degrees; a clean parry about 18). The camera never
  moves.
- Your hits: the struck enemy freezes for a beat (60 ms, 90 on the beat). Saber hits buzz and burn, heavy weapons
  thud, bolts tick; a faint warm flash at the edge of the view.
- After a Power Attack the blade drags behind your hand for a moment (weight).
- kvr.ini `[Feel] HitPause=0` turns the freeze off, `HitPauseMs` sets it.
- To judge: are the telegraphs readable without being a crutch? Does the knock-off feel like resistance or like a
  glitch? Is the hit-pause noticeable, too long, too short?

## 2026-10-27 fixes to check

- Game screens (looting a footlocker, inventory, stores, computer terminals) come up on the flat panel with the
  laser, as menus do; the wrist only shows the HUD pieces.
- Every line in a conversation or cutscene is captioned. A or B skips the line being spoken.
- Pointing in the world: a pulsing ring at the feet of what your right hand points at. Grip (use / talk / open) and
  shots act on exactly that. Nothing walks you to a target: out of reach you get a double tick - step closer.
- Shots no longer hitch (no camera pick); bolts stop at walls and bodies with a spark burst.
- Cutscenes: shots that look back at you stay first person; a shot of something off to your side (Carth's comm
  call) cuts to the game's camera.
- Telegraph chevrons sit at the chest of close attackers; attackers out of view get an arrow at the edge of your view.
- Your hidden body no longer plays its own swing / clash sounds or saber blur.
- NPCs turn their heads to look at you in conversations (`[Feel] LookAtYou=0` turns it off).
- Death: the camera eases out behind your body and you see it fall.
- Wrist: from 10 cm away a short ray and a dot show where your fingertip is (gold on a button); poke it or pull the
  right trigger.

## Wrist panel (no flat HUD)

- Turn your left palm toward your face: menu buttons, party and quick slots appear along the forearm. Poke a button
  with your right index finger (a tick on the controller when it presses).
- The flat HUD is off in the world. `[Features] FlatHud=1` brings it back; Y toggles it in game.
- The pointer laser is only for dialogue and menus; both triggers are free in the world.

## Settings worth knowing (kvr.ini)

- `[Compat] DiscordOverlay=0` (default): Discord's in-game overlay is kept out of KotOR (it froze the game). Discord
  itself (voice, chat) is unaffected, so leave it running. `=1` lets the overlay in.
- `[Debug] Record=1`: the host records the headset view to `kvr\recordings\` (960x720, 30 fps, needs ffmpeg).
  `python tools\popscan.py <file.mp4>` lists the frames where geometry pops.

## If something misbehaves

Edit `<game>\kvr\kvr.ini` (written on first launch) and restart the game:

```
[Features]
EngineCamera=1    ; 0 = old GL-level head tracking (culling follows the game camera again)
WeaponProxies=1   ; 0 = no weapons drawn in the hands
Gestures=1        ; 0 = swings / shots / Force gestures do nothing
Combat=1          ; 0 = no timing bonus or block / dodge on the d20
Cues=1            ; 0 = no haptics / weapon glow / cue sounds / warning chevrons
Hands=1           ; 0 = no hands
ModelCull=1       ; 0 = old distance-based hiding of your body

[Combat]
ImpactBlock=1     ; 0 = old behavior: block / dodge decided at the roll (no parry window, warnings only cosmetic)
```

Logs: `<game>\kvr\probe.log` (game layer), the host's terminal window.
