# TOSFIXPLUS

Smooth motion at your display's full refresh rate (120 Hz, 144 Hz, and so on) for the Steam PC
version of **Tales of Symphonia**, as an add-on to [TSFix](https://wiki.special-k.info/SpecialK/Custom/TSFix).

The game updates its world 30 times a second. TOSFIXPLUS leaves that alone, so game speed,
physics, cutscene timing and everything else behave exactly as before. What it changes is what
you see between those updates: it draws the game's own scene again for every refresh of your
display, with every object, character and the camera placed partway between where the game had
them in its last two updates. The result is motion as smooth as your monitor can show, drawn by
the game's own renderer. It isn't frame generation: no image is guessed or warped, every frame is
the real scene rendered at an in-between moment.

The cost: what you see runs about one game update (1/30 s, ~33 ms) behind, because an in-between
frame needs the update that comes after it.

Press **F9** in game to turn TOSFIXPLUS on and off and compare.

## Requirements

- Tales of Symphonia, the Steam version for Windows.
- **TSFix 0.10.5**, installed and working. It includes Special K, which loads TOSFIXPLUS.
- The game in a window or TSFix's borderless mode (TSFix's default). Exclusive fullscreen hasn't
  been tested.
- Windows 10 or 11.
- dgVoodoo is optional: if `dgVoodoo.dll` is next to the game, TOSFIXPLUS uses it; otherwise it
  uses Windows' own Direct3D 9.

## Installing

1. Download `tosfixplus.zip` from the [Releases](../../releases) page and unzip it.
2. Copy **`tosfixplus.dll`** and **`tosfixplus.ini`** into the game folder, next to `TOS.exe`
   (by default `C:\Program Files (x86)\Steam\steamapps\common\Tales of Symphonia`).
3. Tell Special K to load TOSFIXPLUS. Open **`d3d9.ini`** in the game folder with Notepad:
   - If it has an `[Import.dgvoodoo]` section, change that section's `Filename=dgVoodoo.dll` to
     `Filename=tosfixplus.dll`. (TOSFIXPLUS loads dgVoodoo itself.)
   - Otherwise, add this section at the top of the file:
     ```ini
     [Import.TOSFIXPLUS]
     Architecture=Win32
     Role=d3d9
     When=Proxy
     Filename=tosfixplus.dll
     ```
4. Take TSFix's frame limit out of the way. Open **`tsfix.ini`** and, under `[TSFix.Window]`, set
   ```ini
   ForegroundFPS=1000.0
   BackgroundFPS=1000.0
   ```
   TOSFIXPLUS keeps the game at exactly 30 updates a second itself. TSFix's own limiter has to
   stay out of its way: at 60 it still holds a frame back now and then, which shows as a small
   hitch.

> **Do step 4 only together with step 3.** Without TOSFIXPLUS, a limit above 30 makes the whole
> game run too fast.

Start the game. A file `tosfixplus.log` appears in the game folder; its first lines say
TOSFIXPLUS started and which Direct3D 9 it is using.

## Using it

- **F9** turns the smoothing on and off. The game's speed is the same either way.
- **`tosfixplus.ini`** has one setting:
  ```ini
  ; The most frames per second to show. 0 = as many as the display refreshes.
  MaxFPS=0
  ```
  A lower number, such as 60 or 90, uses less GPU time.

## Uninstalling

1. Delete `tosfixplus.dll`, `tosfixplus.ini` and `tosfixplus.log` from the game folder.
2. In `d3d9.ini`, change `Filename=tosfixplus.dll` back to `Filename=dgVoodoo.dll`, or delete the
   `[Import.TOSFIXPLUS]` section if you added it.
3. In `tsfix.ini`, set `ForegroundFPS` and `BackgroundFPS` back to `30.0`.

## What's smoothed

| On screen | Smoothed? |
|---|---|
| The camera, scenery, buildings, terrain | Yes |
| Characters and enemies, including their animation | Yes |
| Outlines and depth effects | Yes, together with their objects |
| The battle target marker, shadows under characters | Yes, they follow what they belong to |
| Spell effects, particles, grass and other sprites | Yes, each moved as one piece; a sprite that jumps (a new particle) appears in place |
| HUD, menus, text, pre-rendered videos | No, on purpose: they stay exactly as the game draws them |

Camera cuts are detected and never blended across.

## Known limitations

- Tested on the first hours of the game. Exclusive fullscreen, variable refresh (G-Sync or
  FreeSync) and 60 Hz displays haven't been tested.
- About 33 ms of extra display latency, as explained above.

## Reporting a problem

Open an issue with:

- what you saw, and where in the game;
- your display's refresh rate;
- the `tosfixplus.log` file from the game folder.

Pressing F9 to see whether the problem disappears with TOSFIXPLUS off is a quick way to tell
whether it is caused by TOSFIXPLUS.

## Building from source

TOSFIXPLUS is plain C++ with no dependencies beyond the Windows SDK.

1. Install [Visual Studio](https://visualstudio.microsoft.com/) 2019 or later, or its Build
   Tools, with **Desktop development with C++**.
2. Run **`build.bat`**. It builds `build\tosfixplus.dll` (32-bit, like the game) and copies
   `tosfixplus.ini` next to it.
3. Install the files from `build\` as described above.

## How it works, and changing it

[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) explains the design: how frames are recorded and
redrawn, how objects are paired between updates, what is blended and why, and how the pacing
works. It is the place to start before changing anything.

| File | What it does |
|---|---|
| `src/main.cpp` | Loads the real Direct3D 9 and hooks the game's device |
| `src/recorder.cpp`, `src/recorder.h` | Records each game frame's drawing and redraws it |
| `src/interpolate.cpp` | Pairs objects between frames, blends them, and paces the game |
| `src/common.h` | Declarations shared by the files above |
| `src/tosfixplus.def` | The DLL's exported functions |
| `tosfixplus.ini` | The user setting |

## Credits

- **TSFix** and **Special K** by Kaldaien: TOSFIXPLUS depends on them and is loaded by Special K.
- **dgVoodoo** by Dege, used when installed.

TOSFIXPLUS is an independent project. It isn't made by or affiliated with the authors of TSFix or
Special K, or with Bandai Namco. It contains none of the game's code or data.

## License

[MIT](LICENSE)
