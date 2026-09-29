# TSFix+

Smooth motion at your display's full refresh rate (120 Hz, 144 Hz, and so on) for the Steam PC
version of **Tales of Symphonia**, as an add-on to [TSFix](https://wiki.special-k.info/SpecialK/Custom/TSFix).

The game updates its world 30 times a second. TSFix+ leaves that alone, so game speed,
physics, cutscene timing and everything else behave exactly as before. What it changes is what
you see between those updates: it draws the game's own scene again for every refresh of your
display, with every object, character and the camera placed partway between where the game had
them in its last two updates. The result is motion as smooth as your monitor can show, drawn by
the game's own renderer. It isn't frame generation: no image is guessed or warped, every frame is
the real scene rendered at an in-between moment.

The cost: what you see runs about one game update (1/30 s, ~33 ms) behind, because an in-between
frame needs the update that comes after it.

Press **F9** in game to turn TSFix+ on and off and compare.

## Requirements

- Tales of Symphonia, the Steam version for Windows.
- **TSFix 0.10.5**, installed and working. It includes Special K, which loads TSFix+.
- The game in borderless fullscreen (TSFix's default: it fills the screen) or a window. Exclusive
  fullscreen hasn't been tested.
- Windows 10 or 11.
- dgVoodoo is optional: if `dgVoodoo.dll` is next to the game, TSFix+ uses it; otherwise it
  uses Windows' own Direct3D 9.

## Installing

### 1. Download

1. Go to the **[latest release](../../releases/latest)**. (You can also find it under
   **Releases** on the right-hand side of this page.)
2. Under **Assets**, click **`tsfixplus-<version>.zip`** to download it.
3. Unzip it (right-click the file, then **Extract All...**).

> Don't use the green **Code → Download ZIP** button at the top of this page: that downloads
> the source code, not the ready-to-use mod.

The zip contains `tsfixplus.dll`, `tsfixplus.ini`, `INSTALL.txt` (these instructions, for
Notepad) and the licence.

### 2. Install

1. Open the game folder: in Steam, right-click **Tales of Symphonia**, then **Manage → Browse
   local files**. It's the folder with `TOS.exe` in it.
2. Copy **`tsfixplus.dll`** and **`tsfixplus.ini`** from the zip into that folder.
3. Tell Special K to load TSFix+. Open **`d3d9.ini`** in the game folder with Notepad:
   - If it has an `[Import.dgvoodoo]` section, change that section's `Filename=dgVoodoo.dll` to
     `Filename=tsfixplus.dll`. (TSFix+ loads dgVoodoo itself.)
   - Otherwise, add this section at the top of the file:
     ```ini
     [Import.TSFixPlus]
     Architecture=Win32
     Role=d3d9
     When=Proxy
     Filename=tsfixplus.dll
     ```
   Save the file.
4. Take TSFix's frame limit out of the way. Open **`tsfix.ini`** and, under `[TSFix.Window]`,
   change the two `30.0` values to
   ```ini
   ForegroundFPS=1000.0
   BackgroundFPS=1000.0
   ```
   and save the file. TSFix+ keeps the game at exactly 30 updates a second itself. TSFix's
   own limiter has to stay out of its way: at 60 it still holds a frame back now and then, which
   shows as a small hitch.

> **Do step 4 only together with step 3.** Without TSFix+, a limit above 30 makes the whole
> game run too fast.

### 3. Check it works

Start the game and press **F9** a few times while walking around: motion switches between
TSFix+ and the original 30 fps. A file `tsfixplus.log` also appears in the game folder; its
first lines say TSFix+ started and which Direct3D 9 it is using.

## Using it

- **F9** turns the smoothing on and off. The game's speed is the same either way.
- **`tsfixplus.ini`** has one setting:
  ```ini
  ; The most frames per second to show. 0 = as many as the display refreshes.
  MaxFPS=0
  ```
  A lower number, such as 60 or 90, uses less GPU time.

## Uninstalling

1. Delete `tsfixplus.dll`, `tsfixplus.ini` and `tsfixplus.log` from the game folder.
2. In `d3d9.ini`, change `Filename=tsfixplus.dll` back to `Filename=dgVoodoo.dll`, or delete the
   `[Import.TSFixPlus]` section if you added it.
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

- Tested on the first hours of the game at 144 Hz, and briefly at 60 Hz, on a G-Sync monitor
  (G-Sync on), in borderless fullscreen. Exclusive fullscreen and FreeSync haven't been tested.
- About 33 ms of extra display latency, as explained above.

## Reporting a problem

Open an issue with:

- what you saw, and where in the game;
- your display's refresh rate;
- the `tsfixplus.log` file from the game folder.

Pressing F9 to see whether the problem disappears with TSFix+ off is a quick way to tell
whether it is caused by TSFix+.

## Building from source

TSFix+ is plain C++ with no dependencies beyond the Windows SDK.

1. Install [Visual Studio](https://visualstudio.microsoft.com/) 2019 or later, or its Build
   Tools, with **Desktop development with C++**.
2. Run **`build.bat`**. It builds `build\tsfixplus.dll` (32-bit, like the game) and copies
   `tsfixplus.ini` next to it.
3. Install the files from `build\` as described above.

**`package.bat <version>`** (for example `package.bat 1.0.0`) builds and packs the release zip,
`release\tsfixplus-<version>.zip`, with the DLL, the settings file, `INSTALL.txt` and the
licence. That zip is what gets attached to a GitHub release.

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
| `src/tsfixplus.def` | The DLL's exported functions |
| `tsfixplus.ini` | The user setting |

## Credits

- **TSFix** and **Special K** by Kaldaien: TSFix+ depends on them and is loaded by Special K.
- **dgVoodoo** by Dege, used when installed.

TSFix+ is an independent project. It isn't made by or affiliated with the authors of TSFix or
Special K, or with Bandai Namco. It contains none of the game's code or data.

## License

[MIT](LICENSE)
