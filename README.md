# TSFix+

Smooth motion at your display's full refresh rate (60, 90, 120, 144 Hz...) for the Steam PC
version of **Tales of Symphonia**, plus the fixes the PC version still needs. No other mods
required.

The game updates its world 30 times a second. TSFix+ leaves that alone, so game speed,
battles, cutscene timing and everything else behave exactly as before. What it changes is what
you see between those updates: it draws the game's own scene again for every refresh of your
display, with every object, character and the camera placed partway between where the game had
them in its last two updates. The result is motion as smooth as your monitor can show, drawn by
the game's own renderer. It isn't frame generation: no image is guessed or warped, every frame is
the real scene rendered at an in-between moment.

The cost: what you see runs about one game update (1/30 s, ~33 ms) behind, because an in-between
frame needs the update that comes after it.

Press **F9** in game to turn the smoothing on and off and compare.

## What's included

TSFix+ 1.0 works on its own on the current Steam version of the game. It includes the fixes that
[TSFix](https://wiki.special-k.info/SpecialK/Custom/TSFix) used to provide and that the official
patches never made (TSFix itself doesn't run on the current Steam version):

- the game keeps running when you switch to another window, and **Alt+Tab** works;
- fullscreen is shown as a **borderless window** over your screen, and windowed mode as a
  borderless window;
- the game's own stuttery 30 fps limiter is replaced by TSFix+'s exact pacing;
- the **intro and other videos** play smoothly, without stutter or black blocks;
- the **Zelos title achievement**, which never unlocks in the unmodded game, can be earned;
- optional **texture packs** in TSFix's format (such as the 4x upscale pack) are loaded if you
  have them.

It doesn't change what the official patches already fixed (resolution up to 4K, anti-aliasing,
the old blur).

## Requirements

- Tales of Symphonia, the **Steam version**, kept up to date by Steam.
- Windows 10 or 11. (A Steam Deck guide is below; it hasn't been tested yet.)
- No TSFix, Special K or dgVoodoo: TSFix+ doesn't need them. See
  [Coming from TSFix](#coming-from-tsfix-or-tsfix-09) if you have them installed.

## Installing

### 1. Download

1. Go to the **[latest release](../../releases/latest)**. (You can also find it under
   **Releases** on the right-hand side of this page.)
2. Under **Assets**, click **`tsfixplus-<version>.zip`** to download it.
3. Unzip it (right-click the file, then **Extract All...**).

> Don't use the green **Code → Download ZIP** button at the top of this page: that downloads
> the source code, not the ready-to-use mod.

### 2. Copy two files into the game folder

1. Open the game folder: in Steam, right-click **Tales of Symphonia**, then **Manage → Browse
   local files**. It's the folder with `TOS.exe` in it.
2. Copy **`d3d9.dll`** and **`tsfixplus.ini`** from the zip into that folder. If Windows asks
   whether to replace a `d3d9.dll` that's already there, see
   [Coming from TSFix](#coming-from-tsfix-or-tsfix-09).

That's all. The game loads `d3d9.dll` from its own folder, and that's TSFix+.

### 3. Check it works

Start the game. A file `tsfixplus.log` appears in the game folder; its first lines say TSFix+
started and list the fixes it applied. Walk around and press **F9** a few times: motion switches
between smooth and the original 30 fps.

### Recommended settings

- **Windows' refresh rate:** TSFix+ shows as many frames as your screen refreshes, and a
  borderless window runs at the refresh rate Windows' desktop is set to. Check **Settings →
  System → Display → Advanced display → Choose a refresh rate** and pick the highest.
- **The game's resolution:** set it to your screen's own resolution (for example 3840x2160 on a
  4K screen). A different size has to be scaled by Windows, which stops G-Sync/FreeSync from
  working and can make videos stutter.
- **HDR screens:** turn **Auto HDR** off for the game (**Settings → System → Display → Graphics**,
  choose `TOS.exe`, **Auto HDR: Off**). With it on, the start of the intro video can break up.
- **G-Sync / FreeSync:** fine to leave on (tested with G-Sync).

## Using it

- **F9** turns the smoothing on and off. The game's speed is the same either way.
- **`tsfixplus.ini`** has two settings:
  ```ini
  ; The most frames per second to show. 0 = as many as the display refreshes.
  MaxFPS=0
  ; Load TSFix-format texture packs from TSFix_Res\inject, if there are any. 0 = off.
  TexturePacks=1
  ```
  A lower `MaxFPS`, such as 60 or 90, uses less GPU time.

### Texture packs

TSFix+ loads texture packs made for TSFix: `.7z` archives (or loose `.dds` files) in a
`TSFix_Res\inject` folder inside the game folder, such as the 4x upscale pack linked from the
[TSFix page](https://wiki.special-k.info/SpecialK/Custom/TSFix). If you don't have any, nothing
happens. They're optional: the difference is mostly in close-up detail, and loading is
noticeably slower with the large 4x pack. `TexturePacks=0` turns them off without deleting them.

## Steam Deck (not yet tested)

> This guide hasn't been tried on a Steam Deck yet. If you try it, please
> [open an issue](../../issues) and say whether it worked, with your `tsfixplus.log`.

On the Deck the game runs through Proton, which has its own `d3d9.dll`; one launch option tells
it to use TSFix+'s instead.

1. Switch to **Desktop Mode** (Steam button → **Power → Switch to Desktop**).
2. Download the zip from the [latest release](../../releases/latest) in the browser, open your
   **Downloads** folder, right-click the zip and choose **Extract** (or **Extract archive here**).
3. In Steam, right-click **Tales of Symphonia → Manage → Browse local files**, and copy
   **`d3d9.dll`** and **`tsfixplus.ini`** into that folder.
4. In Steam, right-click **Tales of Symphonia → Properties → General**, and under
   **Launch Options** enter:
   ```
   WINEDLLOVERRIDES="d3d9=n,b" %command%
   ```
5. Switch back to **Gaming Mode** and start the game.
6. Frame rate: TSFix+ follows the screen's refresh rate. In the quick menu (**…** button →
   **Performance**), leave the **frame rate limit** off; on a Steam Deck OLED, set the
   **refresh rate to 90 Hz** for 90 frames a second (the LCD model runs at 60).

To check it's working, look for `tsfixplus.log` in the game folder (step 3). To undo it, delete
the two files and clear the launch option.

## Coming from TSFix, or TSFix+ 0.9

TSFix+ 1.0 replaces both: it needs neither TSFix nor Special K.

1. If you set TSFix up on the game's 2016 launch version, get the current version back: in
   Steam, right-click the game → **Properties → Installed Files → Verify integrity of game files**.
2. In the game folder, delete TSFix's and Special K's files: `d3d9.dll` and `d3d9.ini` (Special
   K), `tsfix.dll` and `tsfix.ini`, and `dgVoodoo.dll` and `dgVoodoo.conf` if they are there. If
   you had TSFix+ 0.9, also delete `tsfixplus.dll` and the `install.bat`, `uninstall.bat` and
   `tsfixplus-setup.ps1` files. Keep the `TSFix_Res` folder if you want its texture packs.
3. Install TSFix+ as above.

(TSFix+ still works as an add-on to TSFix, as version 0.9 did, if you prefer that setup: then
TSFix provides the fixes and TSFix+ only smooths.)

## Uninstalling

Delete `d3d9.dll`, `tsfixplus.ini` and `tsfixplus.log` from the game folder.

## What's smoothed

| On screen | Smoothed? |
|---|---|
| The camera, scenery, buildings, terrain | Yes |
| Characters and enemies, including their animation | Yes |
| Outlines and depth effects | Yes, together with their objects |
| The battle target marker, shadows under characters | Yes, they follow what they belong to |
| Spell effects, particles, grass and other sprites | Yes, each moved as one piece; a sprite that jumps (a new particle) appears in place |
| HUD, menus, skits, text, videos | No, on purpose: they stay exactly as the game draws them, at 30 |

Camera cuts are detected and never blended across.

## Known limitations

- Tested on Windows 11 at 144 Hz and 60 Hz, on a G-Sync monitor with G-Sync on. Not yet tested:
  the Steam Deck, FreeSync, other graphics cards.
- About 33 ms of extra display latency, as explained above.
- Menus, skits and the battle results screen stay at 30 frames a second.

## Reporting a problem

Open an [issue](../../issues) with:

- what you saw, and where in the game;
- your display's refresh rate;
- the `tsfixplus.log` file from the game folder.

Pressing F9 to see whether the problem disappears with the smoothing off is a quick way to tell
whether it is caused by the smoothing.

## Building from source

TSFix+ is plain C++ with no dependencies beyond the Windows SDK (the 7z decoder for texture
packs is included, in `src/lzma`).

1. Install [Visual Studio](https://visualstudio.microsoft.com/) 2019 or later, or its Build
   Tools, with **Desktop development with C++**.
2. Run **`build.bat`**. It builds `build\d3d9.dll` (32-bit, like the game) and copies
   `tsfixplus.ini` next to it.
3. Copy both into the game folder.

**`package.bat <version>`** (for example `package.bat 1.0.0`) builds and packs the release zip,
`release\tsfixplus-<version>.zip`, with the DLL, the settings file, `INSTALL.txt` and the
licence. That zip is what gets attached to a GitHub release.

## How it works, and changing it

[docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) explains the design: how frames are recorded and
redrawn, how objects are paired between updates, what is blended and why, how the pacing works,
and what each game fix does. It is the place to start before changing anything.

| File | What it does |
|---|---|
| `src/main.cpp` | Loads the real Direct3D 9 and hooks the game's device |
| `src/recorder.cpp`, `src/recorder.h` | Records each game frame's drawing and redraws it |
| `src/interpolate.cpp` | Pairs objects between frames, blends them, and paces the game |
| `src/standalone.cpp` | The game fixes (limiter, timer, clock, window, focus, Alt+Tab, achievement) |
| `src/textures.cpp` | TSFix-format texture packs |
| `src/lzma/` | The 7z decoder from the LZMA SDK (public domain) |
| `src/common.h` | Declarations shared by the files above |
| `src/tsfixplus.def` | The DLL's exported functions |
| `tsfixplus.ini` | The user settings |

## Credits

- **TSFix** by Kaldaien ([source](https://github.com/Kaldaien/TSF), GPL-3): it found the problems
  TSFix+ fixes, and TSFix+'s fixes are reimplemented from its approach. Texture packs use its
  format.
- **LZMA SDK** by Igor Pavlov (public domain): the 7z decoder for texture packs.

TSFix+ is an independent project. It isn't made by or affiliated with the authors of TSFix or
Special K, or with Bandai Namco. It contains none of the game's code or data.

## License

[GNU General Public License v3.0](LICENSE). Version 0.9 was released under the MIT licence; from
1.0, which builds on TSFix's GPL-3 work, TSFix+ is GPL-3.
