# Phase 0 findings — Steam swkotor.exe

Build: Steam, SHA-256 `34e6d971c034222a417995d8e1e8fdd9f8781795c9c289bd86c499a439f34c88`, image base 0x400000.
SteamStub-wrapped: `.text` encrypted on disk (entropy 8.00), EP in `.bind`. Decrypted in memory by the
time the game calls `DirectInput8Create`; probe dumps it then (`.text` entropy 6.50). Dumps stay local.

## Rendering
- Fixed-function OpenGL 1.x + ARB/NV vertex programs. Projection via `GLU32!gluPerspective`; view via
  `glMultMatrixf` / `glRotatef` / `glTranslatef`. No `glLoadMatrixf` import.
- System `opengl32`/`glu32` exports are hot-patchable (`mov edi,edi` + 5 bytes int3).
- `GDI32!SwapBuffers` is a `jmp [ptr]` stub; hook `opengl32!wglSwapBuffers` instead.
- Steam overlay (`GameOverlayRenderer.dll`) detours `opengl32!wglSwapBuffers`, so it is not hot-patchable, and
  the exe never calls `SwapBuffers` through its IAT. Frame boundary = swap the `jmp [ptr]` slot inside
  `GDI32!SwapBuffers` (works, v3).
- `glLoadIdentity` / `glPushMatrix` / `glPopMatrix` are not hot-patchable (`mov eax, fs:[18]`); IAT hooks work for them.
- Game renders at 800x600 (4:3). GPU: RTX 5070 Ti (WGL_NV_DX_interop2 path for GL -> D3D11 sharing).

### In-world frame (traces/trace_f66200.txt)
1. `glClear(COLOR|DEPTH|STENCIL)`
2. `PROJECTION: glLoadIdentity; gluPerspective(55, 1.3333, 0.1, 10000)`. fovy from camerastyle; 45 / 34.5 seen in
   dialogue. GUI 3D (portraits, menu model) uses fovy 22.726 with aspect 0.37-2.8, so filter on aspect ~1.333.
3. `MODELVIEW: glLoadIdentity; glMultMatrixf(R); glTranslatef(-camPos)` is **the camera**. Single injection point.
4. `glProgramEnvParameter4fARB(VERTEX, 89..92)` = same camera as rows [R | camPos]; 93..95 constants. Vertex
   programs use it, so per-eye offsets must rewrite these too.
5. Scene: per-object `glPushMatrix; glMultMatrixf(world); glDrawElements; glPopMatrix`; ARB vp 11 for skinned meshes.
6. `glClear(STENCIL)`: stencil shadows.
7. Post: ortho glow/blur chain at 512/256/128/64/256 viewports (render-to-texture).
8. GUI: `glOrtho(0,800,0,600)` and `glOrtho(0,1,0,1)` with per-widget viewports (minimap 6,474 120x120).

## Combat (addresses valid for this Steam build only)
| VA | What |
|---|---|
| `0x5BACA0` | Attack-roll resolver (NWN-lineage `ResolveAttackRoll`). `ecx` = attacker, arg = target. |
| `0x5BAD02` | `RollDice(1, 20)` call — the d20. |
| `0x550B60` | `RollDice(count, sides)`, thiscall on RNG object at `[0x7A3A28]`, `ret 8`, returns sum in `ax`. |
| `0x832874` | Debug forced-roll global: if non-zero after the roll, it replaces the d20 (`0x5BAD1B`). |
| `0x5A7EA0` | Attack modifier vs target (builds `+ %d (Base Attack Bonus)` breakdown). |
| `0x5AD0E0` | Defense vs attacker. |
| `0x5A9050` | Damage roll (`Damage Roll: %d`, crit variant). |
| `0x4D4C10` | `CSWSCombatRound::IncrementTimer`. |
| `0x4D4620` | `CSWSCombatRound::EndCombatRound`. |
| `0x4D3EC0` / `0x4D5120` | Combat round save/load (`RoundLength`). |
| `0x4D2210` / `0x4D2450` | Attack data save/load (`AttackResult`, `AttackDeflected`, `AttackDebugText`). |

Attack data struct (in `esi` inside the resolver): `+0x5C` result code, `+0xC1` roll, `+0xC2` attack bonus,
`+0xF4` defense.

## Open
- Round length constant (no 3000/6000 immediates in the round functions; likely data-driven).
- Scene render entry point in the exe (needed for synchronized two-pass stereo instead of alternate-eye).

## Cutscene movies (found 2026-10-26)
- Bink through binkw32.dll imports: a persistent movie worker thread (proc `0x405B30`, created in `0x405D50`) waits
  on an event, then per movie `0x4053E0` creates the "SW Movie Player Window" (class SWMovieWindow, popup over the
  game window), opens BinkBuffer, waits `while ([0x7A3C9C] == 1) Sleep(1)`, shows the window and runs the decode loop
  `0x404C80` (DoFrame, CopyToBuffer, BufferBlit, NextFrame, `while (BinkWait) Sleep(1)`). The main thread stops
  rendering while it plays.
- `[0x7A3C9C]` is 1 only around the game's SwapBuffers call (`0x4048AE` .. `0x4048D5`). Our SwapBuffers hook runs
  inside that window, so in the world the flag read 1 almost always and the worker never showed its window (the
  new-game hang, 2026-10-26). Fixed by clearing it during our work (kvr_game.cpp h_SwapBuffers).
- Movie window proc `0x405190`: WM_ACTIVATEAPP 0 (app deactivated) or Alt+Enter = interrupted (replayed later from
  the start); Escape, or a click per setting = skipped. We subclass it to drop WM_ACTIVATEAPP 0.
- Queued script movies (k_pend_area01: PlayMovie "01A") play at module start via `0x602650` (client internal +0x1F8
  must be 0), or later when replayed; immediate ones via `0x602AF0`.
- Opening: character creation -> the crawl + dream (640x272, 3605 frames) -> Endar Spire -> Trask (m01aa_c01).

## Target action menu (found 2026-10-26)
- In-game interface: `[[[[0x7A39FC]+4]+4]+0x40]+0x90` (client app -> ... -> GUI -> main interface); action block at +0xBC.
- Per target column c (0..2): list at block+c*0xC (entries pointer, count); entries 0x38 bytes: +0x04 kind, +0x08 id
  (0x1000xxxx feat, 0x2000xxxx Force power, 0x3EB plain attack), +0x0C handler, +0x20 icon resref (16 chars).
- Selected entry id of column c: block+(c + (byte[block+0x1AEA] + 3) * 3)*4; -1 = the first entry.
- Cycle next / previous: `0x6865B0` / `0x686680` (thiscall block, column) only rewrite that id. Keys 1..3 perform the
  column's selected entry on the target (seen: Critical Strike queued against a kinrath).
- Slot widgets: `0x68B9D0` ("BTN_TARGET%d" / UP / DOWN, or ACTION for the personal slots).
- Names: icon resref -> feat.2da / spells.2da / skills.2da name (tools/action_names.py -> kvr/actions.txt).

## Conversation panels (found 2026-10-26; NPC dialog and computer terminal share this code)
- Reset `0x6A7160` thiscall (): first call for every node.
- SetMessage `0x6A7010` thiscall (CExoString*), ret 4 — vtable slot 0x1D4; the computer overrides it (`0x6A9520`) and
  calls this base. Pending text sits in the CExoString at panel+0x1DF0 only until it is shown.
- Subtitle `0x6A7AE0` thiscall (CExoString*), ret 4 — the NPC dialog's line (layout of the subtitle bar).
- SetReplies `0x6A86A0` thiscall (count, CExoString[count], ids, ?, show), ret 0x14 — vtable slot 0x1CC; computer override
  `0x6A94E0` calls it then refreshes its skill readout. Empty reply text = KotOR's "continue" entry.
- SelectReply `0x6A7110` thiscall (index), ret 4 — keys 1..9 in the panel's input handler `0x6A7230` end here.
- panel+0x1C non-null exactly while the panel is up (0 before it opens and after the conversation ends).
- Node order: Reset, SetMessage, SetReplies — at once, or after a scripted pause / voice line (the reply list is empty
  meanwhile; Space does not skip a computer's pause).
- Constructors: NPC dialog `0x6A8B40`, computer `0x6A8EB0` (LB_MESSAGE +0x2CFC, LB_REPLIES +0x19C4); computer vtable
  `0x755730`.

## Combat timeline (found 2026-10-25 with Ghidra 12.2 headless, `tools/ghidra/q.ps1`)

Rolled at round start, applied at the visible impact. Measured in the harness (kinrath melee vs the player): the damage
event is created *at* the impact and queued with 0 ms delay, ~500 ms (+- 6) after the roll; the record's ReaxnDelay
stays 0 for melee (set for ranged by `0x5B6DB0`). So the roll is the only advance notice: warnings are timed from the
roll plus a per-attacker learned roll -> impact delay.

| Step | Where | Notes |
|---|---|---|
| Roll | `0x5BACA0` thiscall (ecx attacker, target obj, ...), ret 0xC | writes the attack record; d20 replaced by `[0x832874]` when non-zero |
| Attack record | `round + 4 + index * 0x14C` (`0x4D2B50`), index = byte `[round + 0x96C]` | see fields below |
| Damage effect | melee `0x5B6F30`, ranged `0x5B75D0` | `new` 0x8C bytes, type WORD `+8` = 38 (damage); ints at `[+0x34]`, count `+0x30`; ints 0..14 = damage per type, 0x10 = delay, 0x13 / 0x15 = 1 for weapon attacks |
| Queue | `0x4B08D0` AddEventDeltaTime, thiscall (days, ms, caller id, object id, event type, data), ret 0x18 | event 5 = apply effect; weapon damage is queued at the impact with 0 ms |
| Impact | `0x4DFA40` damage handler, stdcall (target obj, effect, flags), ret 0xC | reads the 15 damage ints, runs damage reduction (vtable `+0xA4`) / resistance (`+0xA8`), applies HP loss |

Attack record (`CSWSCombatAttackData`, from its save code `0x4D2210`): `+0x00` AttackGroup, `+0x08` AnimationLength,
`+0x0C` ReactObject, `+0x10` ReaxnDelay (ms, roll -> impact), `+0x12` ReaxnAnimation, `+0x14` ReaxnAnimLength,
`+0x18` MissedBy, `+0x1C` DamageList (15 shorts), `+0x3A` WeaponAttackType, `+0x40` RangedAttack, `+0x48` KillingBlow,
`+0x50` CriticalThreat, `+0x58` AttackDeflected, `+0x5C` AttackResult (1 hit, 4 miss, 8..10 deflect/parry variants),
`+0x64` AttackType, `+0x68` RangedTarget xyz.

Objects: id at `+4`, world position (KotOR meters, z up) at `+0x90 / +0x94 / +0x98`.
Combat round clock: `CSWSCombatRound::IncrementTimer` `0x4D4C10` (Timer `+0x944` vs RoundLength `+0x94C`).

## Engine objects (Steam build) — found 2026-10-24

| What | How |
|---|---|
| App / server | `app = [0x7A39FC]`, `server = [app+8]`, `internal = [server+4]`, client `[app+4]` |
| Creature by id | `0x4AE770(server, id)` thiscall -> server creature (type-checked, vtable+0x30 cast) |
| Object by id | `0x4AE750(server, id)` -> any object |
| Party table | `internal + 0x1B770`; leader id `+0xEC` (-1 = the PC leads) |
| Player list | `[internal + 0x10064]` CExoLinkedList; `0x5E9B60` GetAtPos; element vtable[1] cast -> player; PC id `[player+0x38]` |
| Inventory | `[creature + 0xA2C]`; `0x5A4C20(inv, slot)` -> item; slot 0x10 right hand, 0x20 left |
| Item base type | `[item + 0x0C]` = baseitems.2da row |
| Combat round | `[creature + 0x9C8]`; RoundStarted byte `+0x934`, Timer `+0x944`, RoundLength `+0x94C`, RoundPaused `+0x958` |
| Scene camera setup | `0x45C540` virtual thiscall; pos `+0x7C`, quat (w,x,y,z) `+0x88`, fovy `+0x1D0`, frustum planes `[+0x1F8]` |
| GUI click gate | WndProc builds WM_*BUTTON* -> GUI via `0x5DF590`; input ignored while the app is inactive |
| Resolution whitelist | `0x5F0C60` (patched to accept any mode) |

## Weapon classes (baseitems.2da `weaponwield`)

1 baton · 2 one-hand blades/sabers (saber when damageflags 4096) · 3 two-handed melee (staff, double blades,
warblade, ghaffi stick) · 4 pistols · 5 rifles/carbines/bowcaster · 6 repeaters. Full row map in game/weapons.cpp.

## Client animation nodes (v2 spike, 2026-10-27)

Names from Lane's K1 Ghidra labels (GOG build; every address checked so far is identical on Steam). Verified at runtime
by `[Debug] AnimProbe=1` (game/animprobe.cpp): all 51 drawn skeleton nodes of the player model matched the GL-recorded
bones to < 1 mm; only undrawn hooks (LightsaberHook, headconjure, camerahook...) differ.

| What | Where |
|---|---|
| Player client creature | `CClientExoApp::GetPlayerCreature` `0x5ED540` (ecx = `[app+4]`) |
| Body model (Gob) | `CSWCCreature::GetModel(part 0xFF, create 0)` `0x6179C0` -> `[creature+0x68]` anim base `vtbl[2]` |
| Gob root Part | `[gob + 0x6C]`; `Gob::Animate(dt)` `0x486670`, `Gob::PlayAnimation(name, speed, flags, start)` `0x485BD0`, `Gob::BeginLookAtAnimate` `0x487190` |
| Part | `+0x04` MdlNode, `+0x08` local pos, `+0x14` local quat (w,x,y,z), `+0x24` scale, `+0x30/+0x34` children array/count, `+0x3C` parent, `+0x40` gob (world pos `+0x78`, quat `+0x84`, scale `+0x1B4`) |
| Part methods | `GetWorld(pos*, quat*)` `0x43E4A0`, `GetLocal` `0x43E6A0`, `Name()` `0x440200` (fastcall) |

Body nodes are the `*_g` bones (pelvis_g, torso_g, torsoUpr_g, r/lbicep_g, R/Lforearm_g, R/Lhand_g, fingers, thighs,
shins, feet) plus hooks: rhand / lhand (weapon roots), headhook, Impact, LightsaberHook, DeflectHook, camerahook.
