# Diegetic HUD: the flat HUD moves onto your left forearm

Goal for v1: no flat HUD floating in the world, no laser outside menus and conversations, and both triggers free for
weapons (dual pistols). Started 2026-10-27.

## Approach: relocate, don't rebuild

KotOR's own in-game interface keeps running exactly as before - we only stop showing it as a sheet in front of you.
Its pieces are cropped out of the GUI frame we already capture (the panel texture) and shown on a panel attached to
your left forearm; touching a piece with your right index finger clicks the real (hidden) control underneath. Every
HUD function keeps working without re-implementing any of them.

| Flat HUD element                | Where it goes                                           |
|---------------------------------|---------------------------------------------------------|
| Menu buttons (equipment, inventory, character, abilities, messages, journal, map, options) | wrist panel, top row: poke to open the menu (the menu itself is a world panel with the laser, as now) |
| Minimap                         | wrist panel                                             |
| Party portraits + vitality / Force bars | wrist panel; poke a portrait to switch character (X still works) |
| Personal quick-use slots (medpacs, stims, self powers) | wrist panel, bottom row: poke to use, poke the arrows to cycle |
| Pause / solo / stealth toggles  | wrist panel (A still pauses)                            |
| Target action menu (over the target) | the action ring (hold grip on the target) - already done |
| Combat action queue, clear      | gone: moving the stick clears a queued attack - already done |
| Feedback / notifications text   | hidden for now; quiet world-anchored cards later        |

## Showing it

- The panel appears when you turn your left palm toward your face (the "check your watch" gesture: the palm normal
  points at the head within ~35 deg) and fades after you turn it away.
- Anchored to the left controller grip, lying along the forearm just above the wrist, facing the eyes; ~24 cm wide.

## Touching it

- Right index fingertip: the tracked hand's index tip joint when hand tracking is live, else a point just past the
  right controller's tip.
- Within 2 cm in front of the panel: the cursor hovers that spot (the game's own hover highlight shows on the
  crop). Pushing through the surface = mouse down, pulling back out = mouse up (a click). A short haptic tick on
  contact.
- No trigger involved anywhere, so both triggers stay with the weapons.

## Where the crops come from

The game publishes the screen rectangles of the HUD controls (shared memory, `KvrHudLayout`) from the main interface
object (`[[[[0x7A39FC]+4]+4]+0x40]+0x90`), whose controls keep their extent (x, y, w, h) 4 bytes in:

| Control                         | Offset in the main interface                  |
|---------------------------------|-----------------------------------------------|
| BTN_EQU .. BTN_OPT (menu row)   | +0xB00C, +0xB1D0, +0xB394, +0xB558, +0xB71C, +0xB8E0, +0xBAA4, +0xBC68 |
| LBL_MAPBORDER / LBL_MAP (minimap) | +0x5CC0 / +0x5E00                           |
| party member widgets (3)        | +0x1F88, stride 0xEA8; inside: LBL_BACK +0x28, LBL_CHAR +0x668, PB_VIT +0x7A8, PB_FORCE +0x8F8, BTN_CHAR +0xCC8 |
| personal action slots (6)       | +0x772C, stride 0x71C; inside: BTN +0x0, LBL +0x1C4, UP +0x388, DOWN +0x54C |
| TB_PAUSE / TB_SOLO / TB_STEALTH | +0xC0AC / +0xC360 / +0xC614                   |

## Steps

1. Publish the layout (game) and verify the rectangles against a GUI frame (harness).
2. Host: wrist panel composed from the crops, palm-toward-face gesture, fingertip poke -> clicks.
3. Hide the flat HUD in the world by default (`[Features] FlatHud=1` brings it back); laser only in menus and
   conversations; left trigger no longer clicks anything in the world.
4. Playtest; then quiet notice cards for the feedback text.
