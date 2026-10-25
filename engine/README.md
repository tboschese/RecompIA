# RemasterIA

Software that redraws a classic game for a modern screen. A game is a folder in `games/<game>`: the expected ROM, the core, the options, and whether the picture remaster is on. The command builds the app, and the engine redraws the picture while the game runs.

```bash
brew install sdl2   # once
PYTHONPATH=tools/remasteria python3 -m remasteria mac games/<game> path/to/the/rom
```

What the engine adds, for every game that turns the remaster on:
- **hard pixels on a fine grid**, on top of the original art, with no blending and no blur;
- **90/120 Hz motion** from the real scroll and sprites;
- **depth**: farther layers a step darker, light and shadow on sprites, without moving what the game drew.

Games ship on **macOS** first, then **Android**. For another game, the new folder names the core and sets `"remaster": true`. The same engine and the same command apply.

The repository holds **only code and notes**. ROMs and anything extracted from them stay out of Git: each project reads its own copy of the game at build time.

## Layout

```
games/<game>/      one project per game: expected ROM version, app options, entry points, routine names
engine/            the engine (C + SDL2 + OpenGL) that runs the games: macOS today, Android later
engine/shaders/    looks: pixel perfect (default) and CRT
tools/remasteria/  the command that builds the remaster (macOS: .app) from games/<game> and the ROM
tools/snesre/      SNES reverse engineering (65816 disassembler, execution tracing)
tools/rom2apk/     the fast path: pack a ROM with an emulator into an APK
build/, dist/      analysis output and generated apps (gitignored)
```

## Play on Mac

```bash
PYTHONPATH=tools/remasteria python3 -m remasteria mac games/chrono-trigger "Chrono Trigger (USA).sfc"
open "dist/Chrono Trigger.app"
```

The app is self-contained: it carries the engine, the core, SDL, the controller database, and your ROM. It opens filling the screen, at the console's aspect, with bars on the sides of a wide monitor. F11 returns to a window. It resumes where you left off and keeps saves and controller mappings in `~/Library/Application Support/RemasterIA/<game>/`. The core and the engine are compiled from source the first time and cached in `~/.snesre`.

**Quality — Chrono Trigger 0.10, the first game with `remaster` on:**
- **Hard-pixel remaster:** the console picture is redrawn on the largest integer grid that fits the screen, so the picture fills the display and every console pixel is the same size. Flat areas keep the original color. Light is one flat step of that same color, not a new one and not a mix with the neighbor: the inside of a body's top third is a little lighter, the inside of the bottom third a little darker. Black stays black, so a dark area does not turn beige. The middle stays the color the game drew. The outline stays that color too, so the light does not trace the stair. A one-pixel stroke, such as a letter, and stipple, such as a cloud, stay as drawn. The ground shadow is cool and sits on the feet. A farther layer steps darker and cooler. Nothing is shifted, so gameplay stays where the game put it. Neighbors are never mixed, and nothing is blurred.
- **Pixel perfect:** each pixel of that grid becomes an exact block of screen pixels, at the largest integer scale where every block is the same size, with no smoothing.
- **HD Mode 7:** native 4x rendering with perspective correction (bsnes-hd), used only in Mode 7 scenes. Those scenes skip the remaster above.
- **No sprite limit:** nothing flickers.
- **True 120 frames per second (120 Hz displays, such as ProMotion):** between two game frames, the engine draws one frame where every layer and every sprite is halfway along its real motion, already in the remastered art. The motion comes from the console's own state: each layer's scroll on each line, and each sprite's position. It is not guessed the way a TV's "motion smoothing" is, so there is no blur and no warp. The camera and the characters slide at twice the original fluidity. An emulator cannot do this, because it only sees the finished picture. It does not apply to Mode 7 scenes yet. The menu can switch back to the original motion.
- **No stutter:** the game is locked to the display rate with vsync, with no tearing. Audio is resampled by up to 0.5% to keep up, without clicks.
- **Cubic audio interpolation:** more highs than the console's muffled sound.

**Every game already has:**
- **Native USB and Bluetooth controllers:** plug one in, including while the game is open. Known controllers (SDL's list plus [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB), thousands of models) already map to the SNES layout. Any other works after it is mapped in the menu. The second controller is player 2.
- **Keyboard** with every key remappable.
- **Pause menu** (Esc, the controller Home button, or Select+Start) with: resume, save state, load state, controls, look (pixel perfect or CRT), motion (smooth or original), fullscreen, and quit.
- **8 save-state slots**, each with a thumbnail, date, and time. The game also saves itself on quit and continues from there on the next launch.

Default keyboard: arrows, Z/X/A/S = B/A/Y/X, Q/W = L/R, Enter = Start, Right Shift = Select. Shortcuts: Esc = menu, hold Tab = fast forward, F2 = look, F5/F7 = save/load the current slot, F11 or Cmd+F = fullscreen.

## Current project: Chrono Trigger (USA)

1. **Code discovery** — disassembly from the CPU vectors, following calls and branches while tracking the M/X flags. *(done)*
2. **Full coverage** — jump tables, code in RAM, and execution logs (from our engine, or a Mesen 2 CDL), plus hand notes in `game.json`. *(in progress: still need to play through and decompress the code the game unpacks into RAM)*
3. **Recompiler** — each 65816 function becomes a C function. The game runs natively and is compared frame by frame with an emulator, so it behaves the same.
4. **Our own renderer** — the engine draws the scene from the game's graphics state (tiles, sprites, scroll, and palette), not from the console's finished picture. That is what makes it possible to:
   - interpolate positions to 90/120 Hz *(first version done: 120 Hz in-between frames from the layers and sprites the core exports; Mode 7 still missing)*;
   - draw the scene at a higher resolution, in hard pixels, on top of the original art *(Chrono Trigger 0.10: the largest integer grid that fits the screen, a lighter and a darker step of the same color inside the form, outline left as drawn, farther layers darker; recompiling the game itself is still item 3)*;
   - separate layers for depth. Parallax stays the game's: moving each layer's camera would change where the character stands.
5. **Platforms** — macOS *(done: pixel-perfect app with HD Mode 7 and vsync)*; Android with SDL2 + NDK, on-screen controls, and Bluetooth (the ones from rom2apk).

To map the game, play through the engine with tracing, then run the analysis:

```bash
export PYTHONPATH=tools/snesre

# Play while recording the code that runs (snes9x with a tracer; log in build/chrono-trigger/trace.bin)
python3 -m snesre play games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"

# Analyze: join the static disassembly with the execution logs (play, and Mesen 2 .cdl files)
python3 -m snesre analyze games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"
# → build/chrono-trigger/disassembly.asm and analysis.json
```

The ROM must be exactly the expected version (the SHA-1 is in `games/chrono-trigger/game.json`). Use only a dump of your own cartridge.
