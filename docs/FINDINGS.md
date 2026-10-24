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
