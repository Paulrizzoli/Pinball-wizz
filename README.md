# Pinball Wizz

3D Pinball Space Cadet, upscaled and modernized. Runs natively on Windows 11 and draws the table in
sharp HD (up to 4x the original 640x480) instead of stretching blurry pixels.

Built on the open-source [SpaceCadetPinball](https://github.com/k4zmu2a/SpaceCadetPinball)
reconstruction by Andrey Muzychenko (MIT, see `LICENSE.SpaceCadetPinball`; its original README is in
`README.upstream.md`).

## No game files are included

This repository and its downloads contain no Microsoft game data. Players use their own
`PINBALL.DAT`, sounds and music. The HD artwork is generated on the player's own PC from their own
`PINBALL.DAT` by the included **MakeHDPack** tool, so it never needs to be shared.

## Playing in HD on Windows

1. Download the newest build:
   [PinballWizz-Windows-x64.zip](https://github.com/Paulrizzoli/Pinball-wizz/releases/latest/download/PinballWizz-Windows-x64.zip)
   (every change merged to main republishes it).
2. Copy your `PINBALL.DAT`, `FONT.DAT`, `SOUND*.WAV` and `PINBALL.MID` into the folder.
3. Run `MakeHDPack.exe` once. It writes an `HD` folder next to `PINBALL.DAT`.
4. Run `SpaceCadetPinball.exe`. F4 toggles fullscreen.

Delete or rename the `HD` folder to go back to the original graphics.

To update, copy the new `SpaceCadetPinball.exe` and `MakeHDPack.exe` over the old ones, delete the
`HD` folder and run `MakeHDPack.exe` again.

## How the HD renderer works

The original engine draws into a 1x screen buffer with a matching depth buffer, and all physics,
collision and dirty-rectangle logic stays at 1x, untouched. `SpaceCadetPinball/hdgfx.cpp` mirrors
every draw that lands on the screen, depth buffer or ball save buffers onto Nx "twin" buffers, using
HD versions of each sprite, and the Nx screen is what gets presented.

* HD sprites are loaded from `HD/<group>.png` (group index in the .dat file) with the scale set in
  `HD/pack.ini`.
* Anything without an HD image (like the built-in text font) is upscaled in-game with nearest
  neighbour, so a partial pack still works. This also means you can hand-edit or repaint any PNG.
* Only the Space Cadet 640x480 table data is supported; Full Tilt's other resolutions run in the
  original renderer.

## LED lighting and dot-matrix display

Options > Graphics has these switches (all on by default, with or without an HD pack):

- **LED Lighting**: table lights fade smoothly on and off instead of snapping, lit lamps get brighter,
  more saturated colours, and unlit lamp inserts are dimmed so lit ones stand out. Round lamps are
  shaded like an LED under a glass dome: a bright core when lit, a dark lens with a reflection when off.
- **Light Glow** and **Glow Strength**: a soft bloom around lit lamps and flashing bumpers.
- **Light Spill**: lit lamps cast a soft pool of their colour onto the table and walls around them.
- **Ball Shadow** and **Ball Light Reflections**: the ball casts a soft shadow, and the chrome picks up
  coloured glints from nearby lit lamps.
- **Dot-Matrix Display**: the score and message boxes are drawn as a grid of little round LEDs.

All of it is computed at runtime from the player's own table art (`SpaceCadetPinball/ledfx.cpp`);
game logic is unchanged.

## Settings window and modern effects

In the widescreen layout a big **Settings** button sits in the left panel, under the backglass. In a
narrow window it sits at the bottom of the table on the start screen and during the demo instead. **Options > Settings...** opens the same window at any time. It holds every switch below plus the
lighting ones above (`SpaceCadetPinball/modernfx.cpp`):

- **Ball trail**: a short fading streak behind the moving ball.
- **Bumper flash and sparks**: a flash and a burst of sparks in the bumper's colour on each hit.
- **Shake the table when nudging**: a brief damped shake of the whole picture.
- **Widescreen backglass**: on windows wider than the game, the table fills the height, with the
  backglass art on the left and an enlarged score panel on the right over a blurred copy of the table.
- **CRT / arcade filter** (off by default): scanlines and a soft vignette.
- **Smooth play**: draws at the monitor's refresh rate and runs physics at 240 updates per second.

## The HD pack tool

`tools/hdpack/make_hd_pack.py` reads the .dat file, rebuilds the exact palette the game uses, and
upscales every sprite 4x with Real-ESRGAN (ONNX, CPU only, BSD-3, see
`tools/hdpack/MODEL_LICENSE.txt`):

* The anime 6B model does the artwork: it removes the old dithering and gives crisp edges.
* Each sprite is upscaled together with the table art around it, so its edges blend into the table,
  and its transparency mask follows the original pixel grid so the game's depth sorting lines up.
* The small CINEMATRONICS and MAXIS logos are too few pixels for any upscaler, so the tool paints
  over the old letters and draws the words fresh in Inter ExtraBold Italic (SIL OFL, see
  `tools/hdpack/OFL.txt`), fitted to where the old letters were. The boxes are listed in
  `LOGO_TEXT` at the top of the script.
* A few sprites are redrawn procedurally instead (`tools/hdpack/art_fixes.py`): the blue arrow
  lights get a clean, pointed two-tone arrow fitted to the original's shape, and the ball is
  rendered as a chrome sphere reflecting the room and the table, with a smooth round edge.
  The plunger spring is redrawn as a steel coil round the rod, compressed to match each frame, and
  every screw on the table (and on the lit slingshots) is replaced by the same Phillips pan-head
  screw. Screw spots are listed in `TABLE_SCREWS` and checked against the player's art before
  anything is drawn.

```
pip install -r tools/hdpack/requirements.txt
pip install torch spandrel onnx   # only to create the .onnx models once
python tools/hdpack/export_models.py
python tools/hdpack/make_hd_pack.py path/to/PINBALL.DAT --scale 4
```

## Building

Windows builds run in GitHub Actions (`.github/workflows/windows.yml`), which downloads SDL2 and
SDL2_mixer, builds the game with MSVC and packages `MakeHDPack.exe` with PyInstaller.
On Linux: install SDL2 and SDL2_mixer dev packages, then `cmake -B build && cmake --build build`.
