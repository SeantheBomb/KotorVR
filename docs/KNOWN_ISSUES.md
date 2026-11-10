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


## Settings Issues
Some users are reporting quality issues that can be attributed to misconfigured settings. The installer should be automatically configuring everything correctly, but if that fails, you should be able to reuse my personal INI as a reference:
```
[Sound Options]
Sound Init=0
Music Volume=85
Voiceover Volume=85
Sound Effects Volume=85
Movie Volume=100
Force Software=0
EAX=0
Disable Sound=0
Number 3D Voices=16
Number 2D Voices=24
Environment Effects Level=0.60
2D3D Bias=1.50
Environment Effects=1
Environment Effects Nonstreaming=1
Environment Effects Streaming=1

[Game Options]
GUIsInScreenShot=1
EnableScreenShot=0
Reverse Mouse Buttons=0
Hide InGame GUI=0
Use Small Fonts=0
Keyboard Camera Deceleration=2000.000000
Keyboard Camera Acceleration=500.000000
Keyboard Camera DPS=200.000000
Hide Unequippable=0
Tutorial Popups=0
Subtitles=1
Mini Map=1
Floating Numbers=1
Status Summary=0
Enable Mouse Teleporting To Buttons=1
Mouse Sensitivity=33
Auto Level Up NPCs=0
Mouse Look=0
AutoSave=1
Reverse Minigame YAxis=0
Combat Movement=1
Enable Tooltips=1
Difficulty Level=1
TooltipDelay Sec=1.000000
Memory Access=1
Memory Level=1

[Movies Shown]
Movie 0=112
Movie 1=0
Movie 2=0
Movie 3=0
Movie 4=0
Movie 5=0
Movie 6=0
Movie 7=0
Movie 8=0
Movie 9=0
Movie10=0

[Alias]
HD0=.\
OVERRIDE=.\Override
TEMP=.\Temp
MODULES=.\Modules
LOGS=.\Logs
LOCALVAULT=.\LocalVault
DMVAULT=.\DMVault
SERVERVAULT=.\ServerVault
STREAMMUSIC=.\StreamMusic
TEMPCLIENT=.\Tempclient
SAVES=.\saves
CURRENTGAME=.\currentgame
TEXTUREPACKS=.\texturepacks
STREAMWAVES=.\StreamWaves

[Graphics Options]
V-Sync=0
Anisotropy=16
Frame Buffer=1
Anti Aliasing=4
Texture Quality=2
Emitters=1
Grass=1
Soft Shadows=0
Shadows=1
Brightness=57
EnableHardwareMouse=1
FullScreen=0
Disable Vertex Buffer Objects=1
Width=1440
Height=1080
RefreshRate=60
AllowWindowedMode=1

[Autopause Options]
Mine Sighted=1
End Of Combat Round=0
Enemy Sighted=1
Party Killed=1
Action Menu=0
New Target Selected=1

[config]
firstrun=1

[Keymapping]
Action286B=8
Action286A=7
Action285B=10
Action285A=9
Action284B=54
Action284A=51
Action283B=54
Action283A=51
Action282B=69
Action282A=73
Action281B=53
Action281A=76
Action280B=69
Action280A=73
Action265=52
Action264=57
Action263=43
Action262=85
Action261=84
Action260=83
Action259=82
Action258=81
Action257=80
Action256=79
Action255=78
Action254=77
Action253=31
Action245=75
Action244=55
Action243=67
Action242=74
Action241=87
Action240=56
Action239=68
Action238=82
Action236=83
Action234=81
Action232=80
Action230=79
Action228=78
Action226=77
Action225=70
Action224=90
Action223=31
Action222=25
Action221=24
Action220=29
Action219=28
Action218=42
Action217=87
Action216=65
Action215=63
Action214=62
Action213=60
Action212=61
Action211=66
Action210=59
Action209=71
Action208=89
Action207=72
Action206=30
Action205=55
Action204=67

```

## Interface
- The game's full-screen menus (inventory, character, map) are flat panels, not native VR interfaces.
- Some tutorial texts still mention the mouse and keyboard.
