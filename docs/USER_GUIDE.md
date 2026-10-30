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
- **Your blade hits what it touches.** Swing for real (about 2 m/s at the tip — a deliberate swing, not a flick); a lazy
  touch does nothing and a full swing hits harder. The first cut on an enemy starts the fight with it.
- KotOR's rules still decide how well a hit lands: a cut the enemy's defence beats becomes **blocked into its guard**.
- Special moves: an overhead chop = Power Attack, a forward thrust = Critical Strike, two quick swings = Flurry (the
  best rank you have).

### Defence
When an enemy commits to an attack on you:
- a **chevron** appears over it (an arrow at the edge of your view if it's behind you);
- a glowing **block bar** appears about half a metre in front of your chest, on its side. The bar follows the enemy's
  weapon hand while it winds up (a faint line joins them): a level bar above centre = an overhead blow, an upright bar
  to one side = a side blow. For the last moment it holds still;
- a **ring** closes onto the bar at the moment the blow lands; the bar turns **gold** just before.

Blue means block or parry it; red means dodge it (a bolt you can't deflect).
- **Block:** lay your blade along the bar. When it's lined up the bar lights up solid, with a soft tone and a tick in
  your hand. Keep it there when the blow lands: **BLOCK**. It costs **guard**; block too many in a row and your guard
  breaks.
- **Parry:** swing your blade *onto* the bar just before the blow lands: **PARRY** (a white flash). The attacker
  staggers, it costs nothing, and it gives back a little Force.
- Not on the bar when it lands: the bar breaks apart in red — **MISSED**, or **TOO EARLY** if you left it before the
  blow arrived.
- **Bolts:** a lightsaber across a blaster bolt's path deflects it; swing into it and it goes back at the shooter.
  Without a saber, lean or duck out of its way as it arrives. Behind cover (the shooter can't see your head or chest),
  bolts hit the wall.

Enemies have a guard too: parries, blocked blows and hits wear it down. When it breaks, the enemy staggers for two
seconds — it can't attack and your blows become critical threats. Ordinary troopers and beasts break quickly.

At most two enemies attack you at once; the rest wait their turn.

### Blasters
- Where the barrel points is what you hit; head shots count extra. Each trigger pull is one shot.
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
| `[Combat] Mode` | `contact` | `rounds` = the classic version: KotOR's rounds decide hits; your swings only time them |
| `[Combat] BlockTolerance` | `0.12` | how close your blade must be to the block bar (m) — bigger is easier |
| `[Combat] ParryWindow` | `0.3` | how long before the blow a swing onto the bar counts as a parry (s) |
| `[Combat] AttackTokens` | `2` | how many enemies may attack you at once |
| `[Comfort] SnapTurn` | `0` | `1` = turn in steps instead of smoothly |
| `[Comfort] SnapAngle` | `30` | snap step (degrees) |
| `[Comfort] TurnSpeed` | `120` | smooth turn speed (degrees / second) |
| `[Comfort] CutsceneFirstPerson` | `0` | `1` = never cut away from your character's eyes in cutscenes |
| `[Feel] HitPause` | `1` | the brief freeze on a struck enemy |
| `[Feel] HitPauseMs` | `60` | its length |
| `[Feel] LookAtYou` | `0` | `1` = NPCs turn their heads to you in conversations (experimental: bystanders turn too) |
| `[Aim] Pitch` | `-20` | how far guns tip down from the controller's pointing ray (degrees); raise it if shots go low, lower it if they go high |
| `[Features] FlatHud` | `0` | `1` = the flat HUD in front of you as in the original (Y toggles it) |
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
| The view is off-centre or too high / low | Right thumbstick click to recenter. |
| The game freezes in the main menu with Discord open | Make sure `[Compat] DiscordOverlay=0` (default). |
| Clicks in menus don't land | Point with the **right** controller; menus are on the panel in front of you. |
| My character won't move after a fight | Push the left stick once (it clears a queued attack). |
| Combat feels wrong / you'd rather play it the classic way | `[Combat] Mode=rounds`. |
| Weapons show as simple shapes, no hands | Run the setup again (Install) — the assets weren't built. |
| Something else | `<KotOR>\kvr\probe.log` is the mod's log; include it with a bug report. |
