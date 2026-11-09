# Known issues

What we know about, roughly from most to least noticeable. Found something else?
[Report it](https://github.com/SeantheBomb/KotorVR/issues/new?template=bug_report.yml).

## Focus
- **KotOR pauses whenever its window isn't the active one.** A pop-up, a notification or another app taking the
  foreground freezes the headset view until the game is active again. Close pop-ups before you play; turn on Windows'
  Do Not Disturb.

## Platform
- **Virtual Desktop:** its OpenXR runtime (VDXR) ends the session a few seconds after it starts; use Meta Link /
  Air Link for now.
- **AMD GPUs** have only been tested by one player so far; NVIDIA is the tested platform.
- **Steam version only.** GOG, disc and modified executables aren't supported yet (the mod detects a mismatch and
  stays off rather than crashing).
- **Tested on Meta Quest 3 over Air Link with an NVIDIA GPU.** Other headsets, SteamVR runtimes and AMD / Intel GPUs
  are untested.
- Other mods that replace `dinput8.dll` can't be used together with KotOR VR.

## Rendering
- KotOR renders one eye per frame, alternately, so each eye updates at half the game's frame rate. Keep the game's
  frame rate high (it usually runs well above 100 fps).
- Occasionally a piece of geometry pops in or out for a frame, or a nearby object is hidden briefly when it stands
  right against you.
- Your own body is hidden: you see your hands and weapons, not a full body.

## Cutscenes and conversations
- The camera rules (stay in your eyes vs. cut to the game's shot) are heuristics; some shots still cut when they
  shouldn't, or stay first person when the game's shot would be better. `[Comfort] CutsceneFirstPerson=1` never cuts.
- A conversation line can occasionally show without its replies for a moment; they appear when the voice line ends
  (B skips it).

## Combat
- Physical combat sits on top of KotOR's turn-based rules: enemies still attack on their own 3-second rhythm, and
  your attack "rounds" run in the background. Most of the time this is invisible; sometimes a hit you expected waits
  for the next round. With blasters this is visible on purpose: only the white-hot burst is damage.
- Now and then the stick takes half a second to free you from an attack that has just started.
- Blocks and parries are judged against the block bar when the blow lands. KotOR decides an attack only about half a
  second before it lands, so the bar's warning is short; several enemies at once can be hard to read.
  `BlockTolerance` makes it more forgiving.
- The block bar takes its angle from the enemy's weapon hand; creatures without one (beasts) always show an overhead
  bar.
- Enemies react to hits with a flinch and the game's own hit animations — no ragdoll physics.
- Force powers are cast through the game's own action menus: a power you don't know, or one that can't target that
  enemy, doesn't cast (the next lower rank you know is used instead).
- Gesture thresholds (swing speed, Force shoves, throws) are tuned on one player; tell us if they fire too easily or
  not at all.

## Interface
- The game's full-screen menus (inventory, character, map) are flat panels, not native VR interfaces.
- Some tutorial texts still mention the mouse and keyboard.
