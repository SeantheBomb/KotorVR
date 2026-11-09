# Changelog

## 1.0.2 (2026-11-09)

- **First-person cameras for everyone.** The setup now builds `Override\camerastyle.2da` from your own game files:
  KotOR's cameras at your head with a 170° view. Until now this file only existed on the developer's PC, so players
  got KotOR's third-person camera, objects and characters flickering out of existence as you looked around (the game
  culled with its normal narrow view) and a headset that could freeze on a cutscene frame. Uninstall removes it (and
  puts back one from another mod, if there was one).
- **Windowed mode really applies:** KotOR ignores `FullScreen=0` without `AllowWindowedMode=1`; the setup now sets
  both (1.0.1 only set the first).
- The VR tutorial cards describe the current controls (the strike ring, the block bar, the blaster damage burst).

## 1.0.1 (2026-11-09)

- **Windows 10: the 3D world reaches the headset.** One of the hooks the mod relies on never attached on Windows 10
  (its GLU32 is built slightly differently), so only the menus reached the headset and the world stayed on the monitor.

- **Windowed KotOR by default:** the setup switches KotOR to a window (and at least 1280×960) and restores your
  display settings when you uninstall. Fullscreen KotOR minimizes the moment it loses focus, which left the headset
  black or flashing with only sound (first reported on an AMD + Quest 3 setup).
- **The VR host writes `kvr\host.log`:** the runtime it's using, every session state with a timestamp, and any
  frame-submission errors - attach it to bug reports with `probe.log`.
- The mod logs a clear warning when the game window is minimized instead of silently sending nothing.
- The KotOR VR shortcut replaces a stuck VR host left over from an earlier session.
- Your own held weapons are recognized exactly again (the installer now builds their shapes file).

## 1.0.0 — first release (2026-11)

Played and tested end to end: the whole Endar Spire (and on into Taris) in VR, with an automated playthrough of the
level that checks doors, cutscenes, combat and rendering on every build.

- **Strike cue:** an enemy in reach that isn't attacking you is *open* — a gold ring on its chest and a pulse in your
  sword hand. Cuts above its hips count while it's open; cuts at the legs, too slow or into its attack get a dull grey
  spark and a low buzz.
- **Blasters:** the barrel lies exactly on your pointing ray; every gun sits handle-down in your hand. Bolts on an enemy
  leave a grey puff, and the game's real damage shows as a white-hot burst with a heavy thump; the gun glows while your
  next attack is about to land. One trigger pull, one shot; bolts stop at what they hit.
- **Doors** open as easily as everything else you grip, including the ones the tutorial wants your party member to open.
- **Party switching:** after switching characters nobody's head goes missing and your new body stays out of your eyes.
- **Fights never stall:** a cut on an enemy that hasn't noticed you starts the fight; swinging near a corpse no longer
  opens its loot mid-fight; the stick frees you from a queued attack.
- **Cutscenes** stay in your eyes when someone merely stands in the way (Trask's last stand); shots of somewhere else
  (the Taris duel ring broadcast) cut to the game's camera. The opening is never black.
- Readable hover labels at any distance; your own head never drawn in your eyes.

## 0.1.0 — first public early access (2026-10)

- Head-driven first person in KotOR's own engine, stereo rendering through OpenXR, recentering, smooth / snap turning.
- Cutscenes in your character's eyes when you can see the scene; the game's shot for scenes elsewhere and shots of you
  lying down and terminal camera feeds; movies on a cinema screen.
- Conversations in VR: replies floating in front of the speaker, every line captioned, B to skip.
- Menus, inventory, stores, containers and terminals on a panel with a laser; the HUD and your Force powers on your
  wrist.
- Your character's own hands holding your real equipped weapons; lightsabers ignite in combat.
- Physical melee (per-bone hit detection), blocks and parries on a block bar that follows the enemy's weapon, with
  lined-up feedback and BLOCK / PARRY / MISSED results, guard and posture,
  bolt deflection back at the shooter, cover, attack tokens.
- Blasters aimed by hand with spread, foregrip and fire modes (Sniper Shot, Rapid Shot, Power Blast); thrown grenades.
- Force powers by gesture: gather, then shove / lift / hold / lightning; tiers by gather time; two-handed area powers;
  powers on yourself from your chest; Saber Throw.
- Pointing picks exactly the object your hand's ray hits (near or far); grip acts only on what's marked.
- Combat feedback: haptics per weapon, hit-pause, flinches, sparks, KotOR's own sounds.
- Death camera, Discord overlay protection, one-click installer that builds all VR assets from your own game files.
