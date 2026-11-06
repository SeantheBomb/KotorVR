# Headset-free testing

`host/kvr_simhost.exe` stands in for `kvr_host.exe`: same shared-memory contract, no OpenXR. It plays a scripted
timeline of head poses and controller input, consumes the game's frames with the same keyed-mutex handoff, and saves
PNGs + `stats.csv`. The game layer can't tell it from the real host, so no game-side code is test-only.

```
host\simhost_build.bat                     # build once
.\tools\run_sim.ps1 -Script spin           # auto-loads Game1, 360 deg + look up/down, 14 shots
.\tools\run_sim.ps1 -Script walk           # forward, strafe, look-left-then-walk
python tools\contact_sheet.py traces\sim\<run> eye0 4
```

- Preflight refuses to run with Discord open (pass `-AllowDiscord`: the overlay is blocked in-game anyway) or a
  real `kvr_host` running (`-Force` stops the host). A 1-thread
  `kvr_host` zombie from a lost Meta runtime session is ignored.
- Menu clicks go through the panel pointer, which moves the real mouse cursor: leave the PC alone during a run.
- Artifacts land in `traces\sim\<script>_<timestamp>\` (gitignored): `*_eye0/eye1/panel.png`, `stats.csv`,
  `simhost.log`, `probe.log`. A timed-out wait saves `timeout_*.png`.
- Scripts live in `tools/sim/*.txt`; command reference is at the top of `host/kvr_simhost.cpp`.

Menu coordinates (1440x1080, panel UV, top-left origin): Load Game (0.559, 0.521); save slot Game 1 (0.410, 0.431);
Load (0.499, 0.671).

What still needs a person in the headset: comfort, scale, locomotion feel, readability, combat feel.

## Combat test (tools/sim/dantooine.txt)

Needs `EnableCheats=1` under `[Game Options]` and `[Autopause Options]` with `Enemy Sighted=0` and `Party Killed=0`
in swkotor.ini (remove them afterwards). It also turns on `invulnerability`: the kinrath swarm kills a Dantooine-level party. Loads Game1, opens the console
(`key grave`, `type warp kas_m22ab`, `key enter`) and warps to the Kashyyyk Upper Shadowlands, which Game1 has never
loaded, so the kinrath ~14 m from the entry are still alive (Game1 already cleared the Dantooine wilderness).
`[Debug] TracePosition=1` in kvr.ini logs `pos:` lines every 2 s for route-finding. Extra simhost commands:
`key <name>`, `type <text>`, `wait_log <substring> <timeout>` (whole log), `wait_new_log <substring> <timeout>`
(only lines written after it starts; a sync point, never fails the run). Haptic requests print as `haptic L/R ...`. Runs drive the real mouse and keyboard focus:
they only work while nobody is using the PC.

## Issue tests with fixtures and checks (2026-11-05)

Fixture saves (`fixtures/saves/<name>`, the only save in the list during a run; scripts load the top row):
`starboard_entry` (Starboard Section entry after Trask's sacrifice, dual short swords), `starboard_pistol` (same,
blaster pistol), `dantooine_ruins` (Game1). Build more with `-SaveFixture <name>` (the run's F4 quicksave).

    .\tools\run_sim.ps1 -Script t_door -Fixture starboard_entry -Cheats -AllowDiscord -Force

`-Cheats` turns the cheat console on and the enemy-sighted autopause off for that run only. Checks print
`CHECK PASS/FAIL <name>`; any FAIL makes the run fail. Tests: `t_door` (issues 1, 10), `t_blaster` (6, 7, 8),
`t_sword` (2, 7), `t_start` (4, new game), `t_floor` / `t_floorscan` / `t_corpse` (3), `t_flail` (9, WIP);
`python tools\check_guns.py "<game dir>"` (5). Routes for `game goto`: `python tools\navmesh.py <module> x0 y0 x1 y1`.

Harness commands to the game (`game <cmd>`, game/simcmd.cpp): `goto x y ...`, `approach d`, `face x y`,
`faceenemy`, `facewall`, `aimhint` (+ simhost `hand_adjust`), `pos`, `posmark` / `moved m`, `pick`, `doors`,
`neareye 1`, `trace`, `stop`. Checks: `check_log`, `check_no_log`, `check_count`, `mark` + `check_since_mark` /
`@mark`, `check_bright`, `check_dark_time` (saves a picture + the game's position per dark episode), `lumatrace`.
A real play session's probe.log is kept in `traces\play\` before a run replaces it.

## The whole Endar Spire, played by the rig (tools/sim/tutorial.txt)

New game to the escape pod, unattended (~18 min of game, ~20 min with loading): Trask's briefing, footlocker and
equipment, the bunk door through Trask (Tab), Trask's Security on the locked door, every fight, level-up, Trask's last
stand, the Starboard Section (footlocker spikes, the security computer's overload by number keys, the squad, Carth),
the escape pod. Checks along the way: every door opens (`opendoor`), the opening is never black (issue 4), Trask's
scene stays first person (11); at the end over the whole level: nothing of your body within 12 cm of the eye (2),
tooltips readable (10), the stick never slow to free you (7), sword hits land.

    .\tools\run_sim.ps1 -Script tutorial -Cheats -AllowDiscord -Force -TimeoutSec 3000

Tools it relies on: `game god 1` (the rig's invulnerability; the console cheat is a toggle that saves and module
changes flip), `fight <timeout> <module>` (closes on the nearest hostile - one that has not swung yet included - and
walks the walkmesh route when something stands between), `autotalk on` / `autotalk prefer <text[:n]> ...`,
`use x y z | npc [ring <text>]`, `opendoor x y z` (grip, then Security, then the party member's), `mark <name>` +
`@<name>` for a check over one stretch. Segments for debugging one part: fixtures `spire_bunk`, `spire_trask`,
`spire_door1`, `spire_room3`, `spire_mid`, `spire_bridge`, `spire_after_trask`, `spire_starboard`; party switch
(issue 2b): `t_switch -Fixture spire_trask`. Runs take the mouse and the foreground: only when the PC is idle.
