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

1. Download the latest `PinballWizz-Windows-x64` build from the Actions tab (or Releases).
2. Copy your `PINBALL.DAT`, `FONT.DAT`, `SOUND*.WAV` and `PINBALL.MID` into the folder.
3. Run `MakeHDPack.exe` once. It writes an `HD` folder next to `PINBALL.DAT`.
4. Run `SpaceCadetPinball.exe`. F4 toggles fullscreen.

Delete or rename the `HD` folder to go back to the original graphics.

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

## The HD pack tool

`tools/hdpack/make_hd_pack.py` reads the .dat file, rebuilds the exact palette the game uses, and
upscales every sprite 4x with Real-ESRGAN (ONNX, CPU only, BSD-3, see
`tools/hdpack/MODEL_LICENSE.txt`):

* The anime 6B model does the artwork: it removes the old dithering and gives crisp edges.
* Each sprite is upscaled together with the table art around it, so its edges blend into the table,
  and its transparency mask follows the original pixel grid so the game's depth sorting lines up.
* Small lettering that the anime model breaks apart (CINEMATRONICS, MAXIS) is redone with the
  general x4plus model, rebuilt as solid smooth letters and blended in. The boxes are listed in
  `TEXT_REGIONS` at the top of the script.

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
