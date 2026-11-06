# Test Drive Off-Road 3 native

An unofficial native Linux version of **Test Drive Off-Road 3** (Accolade / Infogrames, 1999,
PC), made by static recompilation: the game's own code (TDOR3.exe, its SurRender 3D engine
sr.dll and plug-ins, and the Microsoft C runtimes it shipped with) is translated into C on your
computer and built against a new stand-in for Windows on SDL3 and Vulkan, with a native Vulkan
display driver in place of the game's Direct3D 6 one. The game plays as it did; everything new is
an option.

> **Alpha.** Played only on the author's own laptop, and started (not yet played) on a Steam
> Deck. Expect problems on other computers, and please report them in Issues.

**Bring your own game.** This repository holds only the translator and the runtime, never any of
the game's code or data. You need your own copy of the PC CD version of Test Drive Off-Road 3.
The build checks your files against the release the translator knows before anything is built.

## What's new

Set in the game itself, under **Options > More Options** (also kept in
`~/.local/share/tdor3-native/options.txt`):

- Any resolution, window or full screen (Alt+Enter), widescreen at the screen's own shape (Hor+:
  a wider view, not a stretched one), the race HUD at the screen's edges
- Supersampling anti-aliasing and anisotropic filtering
- Smooth motion at any refresh rate (the physics keeps the game's 30 steps a second; the
  frames in between are drawn between them), v-sync and a frame limit
- Controllers: every connected controller drives the car (hot-plugging, triggers for the pedals,
  the stick for steering, with a dead zone and a softer centre if you want one), the menus by
  pad, Xbox button names in the prompts and on the controls page, rumble on knocks
- Movies (Bink) through FFmpeg, the CD music from your own OGG files, any key or button skips a
  movie, the intro can be turned off
- Options > Cheat Menu lists all ten of the game's cheats from the start (or as shipped: earned)

With the options off it is the game as shipped. The translation is checked against the original
code: a reference build runs the original machine code in an emulator on the same runtime, and a
scripted run through the menus and a race left memory identical on every frame.

## Building

Linux only for now. Packages (Debian/Ubuntu names): `build-essential pkg-config python3
python3-venv libvulkan-dev glslang-tools zlib1g-dev libavformat-dev libavcodec-dev
libswscale-dev libswresample-dev`, and SDL3 3.4 or later (its development package, or built from
source: see [third_party/README.md](third_party/README.md)). A graphics driver with Vulkan 1.3.

1. Python with Capstone for the translator:

       python3 -m venv .venv
       .venv/bin/pip install capstone

2. The game's files, in `~/.local/share/tdor3-native/game/` (or anywhere, with `GAME=<folder>`
   for make and `--game <folder>` for the program):
   - the installed game: the folder holding TDOR3.exe, sr.dll and the other DLLs, UIData,
     Vehicles, Static.zip, Sounds.zip and the rest, as the game's installer leaves it
   - `CD/`: a copy of the files on the CD (Movies, Tracks, ...); the game reads it as drive D:
   - `MUSIC/Track02.ogg` ... `Track11.ogg`: the CD's audio tracks as OGG files, if you want the
     music
3. `make` (or `make JOBS=4`): translates the game's modules (a minute) and compiles the C (a few
   minutes).
4. `build/tdor3 --window` (or `--fullscreen`). Saves, settings and a log of the last run go to
   `~/.local/share/tdor3-native/`; the game folder is only read.

## Tested on

- GPD Win Max 2 (2025) laptop: AMD Ryzen AI 9 HX 370 with Radeon 890M graphics; Ubuntu,
  KDE Plasma (Wayland), Mesa (RADV). Played with an Xbox controller.
- Steam Deck: starts and plays the intro; not yet played.

## Status

Arcade races, World Tour (buying, selling, cups, saving and loading), pausing and the options
work. Not tried yet: two-player split screen; whether high scores are
kept between runs. Not done: network play (off), a Windows build, a ready-made download. The
design notes, with what is known about the game, are in [DESIGN.md](DESIGN.md).

## Layout

- `tools/` the translator (Python + Capstone)
- `builds/` what is known about the game's modules (addresses and hashes only)
- `runtime/` the Windows stand-in the translated code runs on: files, threads, input, sound,
  the native SurRender display driver on Vulkan, Bink on FFmpeg, the added menus
- `ref/`, `tools/lockstep.py` the original code in Unicorn, to check the translation against
- `gen/`, `build/` the generated C and the build (from your own files; never committed)

## Licence

This project's own code is released into the public domain ([LICENSE](LICENSE)); the libraries
it uses keep their own licences. The translator and runtime are forked from
[1NSANE: Recompiled](https://github.com/AntAir267/1nsane-recompiled).

Test Drive is a trademark of its owners. This project is not made, endorsed or supported by
Accolade, Infogrames, Atari or the game's current owners.
