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

- Preflight refuses to run with Discord open or a real `kvr_host` running (`-Force` stops the host). A 1-thread
  `kvr_host` zombie from a lost Meta runtime session is ignored.
- Menu clicks go through the panel pointer, which moves the real mouse cursor: leave the PC alone during a run.
- Artifacts land in `traces\sim\<script>_<timestamp>\` (gitignored): `*_eye0/eye1/panel.png`, `stats.csv`,
  `simhost.log`, `probe.log`. A timed-out wait saves `timeout_*.png`.
- Scripts live in `tools/sim/*.txt`; command reference is at the top of `host/kvr_simhost.cpp`.

Menu coordinates (1440x1080, panel UV, top-left origin): Load Game (0.559, 0.521); save slot Game 1 (0.410, 0.431);
Load (0.499, 0.671).

What still needs a person in the headset: comfort, scale, locomotion feel, readability, combat feel.
