# Phase 0 findings — Steam swkotor.exe

Build: Steam, SHA-256 `34e6d971c034222a417995d8e1e8fdd9f8781795c9c289bd86c499a439f34c88`, image base 0x400000.
SteamStub-wrapped: `.text` encrypted on disk (entropy 8.00), EP in `.bind`. Decrypted in memory by the
time the game calls `DirectInput8Create`; probe dumps it then (`.text` entropy 6.50). Dumps stay local.

## Rendering
- Fixed-function OpenGL 1.x + ARB/NV vertex programs. Projection via `GLU32!gluPerspective`; view via
  `glMultMatrixf` / `glRotatef` / `glTranslatef`. No `glLoadMatrixf` import.
- System `opengl32`/`glu32` exports are hot-patchable (`mov edi,edi` + 5 bytes int3).
- `GDI32!SwapBuffers` is a `jmp [ptr]` stub; hook `opengl32!wglSwapBuffers` instead.
- v1 IAT-only hooks produced no frame trace — unconfirmed why; v2 hot-patches exports and logs first calls.

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
