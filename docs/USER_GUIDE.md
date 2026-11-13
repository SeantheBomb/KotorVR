# KotOR VR — User guide

## 1. Setup

### What you need
- KotOR 1 on **Steam** (Windows 10/11).
- A PC VR headset with an **OpenXR runtime**: Meta Quest over **Air Link** or **Link** (tested on Quest 3), or a
  SteamVR headset (untested). The setup tells you which runtime it found.
- Tracked controllers.

### Installing
Run `KotorVR-Setup.exe`, check the KotOR folder it found (the one with `swkotor.exe`), and press **Install**. It:
- puts the mod (`dinput8.dll`) in your KotOR folder (any `dinput8.dll` already there is kept as
  `dinput8.dll.kvr-backup` and put back when you uninstall);
- puts the VR host and the launcher in `<KotOR>\kvr\host`;
- builds the VR weapon models, hands, action icons, sounds and room visibility files from your own game files;
- creates the **KotOR VR** shortcut (desktop and Start menu).

Other mods that replace `dinput8.dll` can't run at the same time. Texture and content mods (Override folder) are fine.

### Starting
1. Put the headset on and connect it (Quest: Quick Settings → **Air Link** or **Link**).
2. Start **KotOR VR** from the shortcut. A desktop window "KotOR VR – headset view" opens (the VR host) and KotOR
   starts. The game's own window stays black — everything is in the headset.
3. Press the **right thumbstick** to recenter (do it once you're standing or seated comfortably).

If you start KotOR from Steam directly, start `<KotOR>\kvr\host\kvr_host.exe` first.

## 2. Controls

| Input | Does |
|---|---|
| Your head | look around (it's KotOR's camera) |
| Left stick | walk where you're looking |
| Right stick left / right | turn (smooth; snap turning in the settings) |
| Right stick click | recenter |
| Right hand, pointing | a faint ring under what you point at, with its name above it |
| **Right grip** | use / talk to / open / attack what you point at |
| **Right grip, held** | that object's other actions (Security, Bash, Force powers, grenades, …) listed by your hand; point + trigger |
| **Right trigger** | in menus and conversations: click / pick; in a fight with a blaster: fire |
| **Left grip** | gather the Force (see Force powers); at your hip: take a grenade |
| Left palm up toward your face | the HUD (menu buttons, map, party, quick slots) and your Force powers on your forearm; right laser + trigger to press |
| A | pause / unpause |
| B | back (Escape); in a conversation: skip the line being spoken |
| X | switch character |
| Y | show / hide the flat HUD |
| Menu | game menu (Escape) |

Menus, the inventory, stores, containers and computer terminals appear on a panel in front of you: point with the right
controller and pull the trigger.

### Conversations
- The conversation floats in front of whoever you're talking to: their line on top, your replies below. Point at a
  reply and pull the right trigger.
- Every line is captioned, including cutscene and alien lines.
- **B** skips the line being spoken.
- Cutscenes stay in your character's eyes when you can see what the scene shows; scenes elsewhere (another room, a
  comm call), shots of you lying down and terminal camera feeds cut to the game's camera. Movies play on a cinema screen.

## 3. Combat

### Melee (lightsabers, swords, vibroblades, staffs)
KotOR already choreographs every exchange: when a round starts it decides each blow, picks the matching pair of
animations for both fighters, and knows when the blades will meet. You play that choreography.

- **Start a fight** by cutting an enemy within reach.
- **Your gates (gold):** each time your round starts, a gold line appears where your character's own blade will land on
  the enemy, with a ring closing on the moment of the blow. **Cut through it as the ring closes.** The enemy's own
  reaction plays in time with your blade.
  - on time and on the line: **clean** (a flash, the enemy freezes for a beat); right on the beat: **perfect**
    (extra damage, a bigger flash);
  - late, early or off the line: **glancing** (half damage, a dull scrape);
  - not swung at all: **no hit**, and the enemy parries instead.
- KotOR's dice still decide whether the blow *can* land. When they say it misses, cutting through the gate meets the
  enemy's parrying blade (**blade on blade**: a clash), which wears down its guard.
- **The breath beat: choosing a move.** Just before each of your rounds the hilt hums and faint **ghost blades**
  appear by your hand, one for each special move your character knows. Move your blade into one and hold it (a click,
  and the hilt glows its colour); the move is used in your next round, with all of KotOR's rules for it:

  | Ghost | Hold your blade | Move |
  |---|---|---|
  | orange, a heavy wedge | raised over your shoulder, tip back | Power Attack |
  | red, a point | drawn back at shoulder height, tip at the enemy | Critical Strike |
  | blue, three strokes | low at your hip, tip forward | Flurry (its extra attack shows as an extra gate) |
  | (none) | in front of you, tip up | a normal attack |

  With two weapons, the right-hand blade counts. A double-bladed staff: overhead and level = Power Attack, one end
  pointed at the enemy = Critical Strike, low and level = Flurry. After a few fights the ghosts fade to a glint
  (`[Combat] StanceGhosts=0` hides them).

### Defence
- **A beat early:** an orange ring tightens on an enemy's weapon hand just before it commits to a blow, so you can see
  who goes next, even in a crowd.
- **Its line (blue, then gold):** when it commits, a line appears where its blade will be when the blow lands, with a
  ring closing on that moment. It turns gold for the last moments. (Beasts and droids without a matching animation
  show the block bar in front of your chest instead.)
  - **Parry:** swing your blade into its line just before the blow lands: a white flash, the attacker staggers, it
    costs nothing and gives back a little Force. A parry right on the beat opens a **riposte**: a white line on the
    attacker for half a beat. Cut through it for a big hit to its guard.
  - **Block:** hold your blade across the line when the blow lands. It costs **guard** (the four pips by your hilt);
    with no guard left a block breaks.
  - Two blows at once can both be parried by one blade across both lines.
  - Red means you can't block it (no melee weapon in hand): dodge it.
- **Bolts:** a lightsaber across a blaster bolt's path deflects it; swing into it and it goes back at the shooter.
  Without a saber, lean or duck out of its way as it arrives. Behind cover (the shooter can't see your head or chest),
  bolts hit the wall.
- Over each enemy you're fighting, a **band** shows its health (red) and its guard (gold segments). Parries, blocks and
  hits wear the guard down; when it breaks, the band flashes gold and the enemy is **open** for two seconds: it can't
  attack and your blows are critical threats.
- Your own health: the edge of your view pulses red when it runs low.
- Every enemy can attack you, as in the original game (you can hold a crowd while your companions fight).
- `[Combat] FightGrade=1` shows a short card after each fight: parries, gates met, hits taken.
- **Force on the beat:** a power released in the breath beat or on a blow's moment gives back some Force.

### Blasters
- The barrel lies along your pointing ray: what the ring under your pointer marks is what you hit; head shots count
  extra. Each trigger pull is one shot.
- **When a shot hurts:** KotOR deals your damage once per combat round (about every 3 seconds), whatever the trigger
  does. Bolts that strike an enemy between rounds leave a small **grey puff**; when the game actually deals your damage
  you see a **white-hot burst** on the enemy and feel a heavy **thump**. Your gun **glows** while your next attack is
  about to land.
- Shots spread a little: less with your **left hand on the barrel** (foregrip) or after **holding still for a
  second** (which also makes it a **Sniper Shot**). Two quick pulls = **Rapid Shot**; hold the trigger about a second
  and let go = **Power Blast** (a heavier extra bolt).
- The gun only fires in a fight.

### Force powers
Hold the **left grip** to gather the Force — a hum rises in your hand; the longer you gather, the stronger the rank
(up to the best you know). Then let go:

| Let go by… | Power (tier 1 / 2 / 3) |
|---|---|
| shoving your hand forward | Push / Whirlwind / Wave (shove with **both hands** for Wave at once) |
| lifting your hand | Choke / Kill / Death Field |
| holding still, then letting go | Slow / Stun / Hold (droids: Droid Stun / Disable / Destroy) |
| squeezing the **left trigger** while gathering | Shock / Lightning / Storm |

The power goes where your **left palm points** (or what your right hand points at, else the nearest enemy ahead). A
quick shove without gathering is a plain Push.

**Powers on yourself:** gather with your **left hand on your chest**, then let go: hold = your chosen power (or the best
healing / protection you know), push out = Speed, lift = Armour. Choose the chest power on your wrist: turn your palm up
toward your face; the powers you know are listed along the forearm — point the right laser and pull the trigger.

**Saber Throw:** let go of the right grip in the middle of a fast swing; squeeze again to catch the saber.

### Grenades
Squeeze the **left grip at your hip** (a click, and a grenade in your hand), throw, and let go. The enemy nearest to
where it lands gets it. You need grenades in your inventory.

## 4. Settings

`<KotOR>\kvr\kvr.ini` is written the first time you play. Edit it with any text editor while the game is closed.

| Section / key | Default | What it does |
|---|---|---|
| `[Combat] Mode` | `choreo` | `contact` = the 1.0 version (cuts land on contact, a block bar in front of you); `rounds` = the classic version (KotOR's rounds decide hits, your swings only time them) |
| `[Combat] GateWindow` | `0.35` | how early a cut through your gate still counts as on time (s); bigger is easier |
| `[Combat] GateRadius` | `0.22` | how close to the gate your blade must pass (m); bigger is easier, and your attack bonus widens it as you level |
| `[Combat] BlockTolerance` | `0.12` | how close your blade must be to an enemy's line to block or parry (m) |
| `[Combat] ParryWindow` | `0.3` | how long before the blow a swing onto its line counts as a parry (s); your Defense widens it |
| `[Combat] IncomingDamage` | `1.0` | multiplies the weapon damage you take (`0.5` = half) |
| `[Combat] AttackTokens` | `0` | how many enemies may attack you at once (`0` = no limit, as in the original) |
| `[Combat] StanceGhosts` | `1` | `0` = no ghost blades in the breath beat |
| `[Combat] FightGrade` | `0` | `1` = a short card after each fight |
| `[Comfort] SnapTurn` | `0` | `1` = turn in steps instead of smoothly |
| `[Comfort] SnapAngle` | `30` | snap step (degrees) |
| `[Comfort] TurnSpeed` | `120` | smooth turn speed (degrees / second) |
| `[Comfort] CutsceneFirstPerson` | `0` | `1` = never cut away from your character's eyes in cutscenes |
| `[Feel] HitPause` | `1` | the brief freeze on a struck enemy |
| `[Feel] HitPauseMs` | `60` | its length |
| `[Feel] LookAtYou` | `0` | `1` = NPCs turn their heads to you in conversations (experimental: bystanders turn too) |
| `[Aim] Pitch` | `0` | tilts guns away from the controller's pointing ray (degrees; negative = down) |
| `[Features] StrikeCue` | `1` | `0` = no openings / strike zone: any blade touch counts |
| `[Features] FlatHud` | `0` | `1` = the flat HUD in front of you as in the original (Y toggles it) |
| `[Render] EyeBuffer` | `1` | the 3D world is drawn at your headset's own resolution; `0` = at KotOR's window size (blurrier, a little faster) |
| `[Render] Samples` | `4` | anti-aliasing of the headset picture (`0`, `2`, `4`, `8`); lower it if your frame rate drops |
| `[Features] Hands` / `WeaponProxies` / `Cues` | `1` | your hands / weapons in hand / combat feedback (haptics, telegraphs) |
| `[Compat] DiscordOverlay` | `0` | Discord's in-game overlay is kept out (it froze the game); Discord itself is unaffected |
| `[WeaponPose] <class>` | — | adjust how a weapon class sits in your hand: `roll pitch yaw forward` (degrees, metres), e.g. `short_blade=180 0 0 0`; applied live |
| `[Sounds] …` | — | swap the combat feedback sounds for other KotOR sounds (`resref`, comma-separated; empty = silent) |

The game's own **Options** still apply (graphics, sound volumes, difficulty, auto-pause). Turn off **Auto-pause →
Enemy sighted** if the game keeps pausing when a fight starts (A unpauses).

## 5. Troubleshooting

| Problem | Try |
|---|---|
| Nothing in the headset, the game runs on the monitor | Start the headset's Link / Air Link **before** KotOR VR. Check the "KotOR VR – headset view" window is open. |
| Blurry, jagged or pixelated picture | Since 1.0.4 the 3D world is drawn at your headset's own resolution (check `[Render] EyeBuffer=1` in `kvr\kvr.ini`); menus and the HUD still come from KotOR's window. On older versions each eye got exactly KotOR's window size. In `<KotOR>\swkotor.ini` `[Graphics Options]` set `Anti Aliasing=4`, `Anisotropy=16` and the biggest 4:3 `Width` / `Height` your monitor lists that fits under your desktop's height (e.g. `1920` / `1440` on a 4K screen; at 125% display scaling a 1440p screen tops out at `1440` / `1080`). The setup does this since 1.0.3. On Link / Air Link a higher encode bitrate in the Meta app helps too. |
| Sound but a black (or flashing) headset; the monitor switches to 640×480 | KotOR is running fullscreen: it minimizes when it loses focus and has nothing to send. Set `FullScreen=0` and `AllowWindowedMode=1` in `<KotOR>\swkotor.ini` `[Graphics Options]` (the setup does this since 1.0.1). |
| Virtual Desktop: black, the session keeps restarting | Use Meta Link / Air Link for now (Virtual Desktop's OpenXR runtime ends the session - being looked into). |
| The view is off-centre or too high / low | Right thumbstick click to recenter. |
| The game freezes in the main menu with Discord open | Make sure `[Compat] DiscordOverlay=0` (default). |
| Clicks in menus don't land | Point with the **right** controller; menus are on the panel in front of you. |
| My character won't move after a fight | Push the left stick once (it clears a queued attack). |
| Combat feels wrong / you'd rather play it the 1.0 way or the classic way | `[Combat] Mode=contact` or `Mode=rounds`. |
| Weapons show as simple shapes, no hands | Run the setup again (Install) — the assets weren't built. |
| The game freezes for a moment, or stops | Another window took the foreground (a pop-up, a notification, another app). KotOR pauses when it isn't the active window: click its window or close the pop-up. |
| Something else | `<KotOR>\kvr\probe.log` is the mod's log; include it with a bug report. |

## 6. Getting help, reporting bugs, suggesting features

- **Found a bug?** Open a [bug report](https://github.com/SeantheBomb/KotorVR/issues/new?template=bug_report.yml).
  Attach `<KotOR>\kvr\probe.log` and `<KotOR>\kvr\host.log` from the session where it happened (it's replaced each time you start the game, so
  copy it before you play again) and, if you can, a short clip.
- **Have an idea?** Open a [feature request](https://github.com/SeantheBomb/KotorVR/issues/new?template=feature_request.yml).
- **Questions, impressions, "how do I…":** [Discussions](https://github.com/SeantheBomb/KotorVR/discussions).
