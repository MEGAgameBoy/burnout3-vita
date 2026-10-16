# Burnout 3 on the PlayStation Vita

The same port as the PC's - the PS2 program translated to C++ by the engine,
not emulated - compiled for the Vita with VitaSDK. Unofficial and fan-made,
like the PC port: you need your own disc (NTSC-U, SLUS-21050), and nothing of
the game is in this repository or in what it builds from it.

**Where it stands (M2 begun, 2026-10-16): the GPU draws, untested.** The
GS is drawn by the Vita's GPU (GXM) in a first version written without the
hardware at hand; where it cannot start (no libshacccg) or the `renderer`
setting says `software`, the engine's software GS draws instead - the CPU's
work, so slow. Either way the picture is scaled to the screen with a line
over it saying how fast the run goes, and the log says what the game does.
The plan, the measurements and the risks: [VITA_PLAN.md](VITA_PLAN.md).

## Installing and running

1. A Vita with HENkaku/Ensō (or h-encore) and VitaShell.
2. Install `burnout3.vpk` with VitaShell. Its data directory is
   `ux0:data/burnout3/` (made at the first start).
3. Install libshacccg (`ur0:data/libshacccg.suprx`, extracted from your
   Vita's own firmware as most vitaGL homebrew asks): the GPU's shaders are
   compiled with it at the first start and kept in `ux0:data/burnout3/shaders/`.
   Without it the game runs, drawn by the CPU.
4. Put the image of your own disc in that directory, as an `.iso` (or a
   `.cue` with its `.bin`):
   `ux0:data/burnout3/Burnout 3 - Takedown.iso`. The port also looks in
   `data/burnout3/` on `uma0:`, `imc0:`, `grw0:` and `xmc0:` (memory cards and
   SD adapters) and takes the first image that is the game's disc; any other
   image is refused.
5. Start it from its bubble.

What it writes, all in `ux0:data/burnout3/`: `burnout3.log` (send it with any
report), `pc_settings.ini` (the disc chosen, the settings), `memcard/` (the
game's saves, slot 1).

### Controls

The Vita's buttons are the DualShock 2's: the face buttons and the D-pad,
Start and Select, **L** and **R** as L1 and R1, the left and right sticks.
The DualShock 2's **L2** and **R2** are the rear touch panel's upper left and
upper right quarters (the lower half is ignored: it is where fingers rest).
The race: Cross accelerates, Square brakes, R1 boosts, L1 looks back,
Triangle changes the camera (FINDINGS.md, "The race's controls").

### Scripted runs on the Vita

With no command line, a run's options go in `ux0:data/burnout3/run.args` -
the contents of a `port/runs/*.args` file, say, with `{data}` for the data
directory (`--memcard {data}/runcard`). Delete it to play normally.
`--no-draw` in it runs the game with no picture at all (a status screen
instead): what the game costs without its drawing.

The line over the picture (pictures a second, the PS2's vertical blanks a
second and how many of them the game was too busy for, the time) is the
`fps_counter` setting, on unless `pc_settings.ini` turns it off.

`pc_settings.ini` also takes, a `key=value` a line:

| setting | |
| --- | --- |
| `renderer=software` | the software GS draws (the CPU), not the GPU |
| `gpu_display=off` | the picture scaled by the CPU rather than the GPU (software GS only) |
| `aspect=16:9` | the picture's shape: `4:3` (the default), `16:9`, `auto` (the screen's) |
| `fps_counter=off` | no line over the picture |

`ps2gxm_tests.vpk` (engine/host/tests/gxm_tests_main.cpp) runs the GPU
renderer's tests on the hardware: each check on the screen and in
`ux0:data/ps2gxm_tests/results.txt`.

### In Vita3K

The VPK runs in [Vita3K](https://vita3k.org) too (its Linux build, on a
virtual display): `port/vita/tools/vita3k_run.py` installs it, runs it and
collects the log and screenshots - the port's own checks between builds, not
a measure of speed. Without the disc it boots the executable alone
(`--test-disc`: an image of `extracted/SLUS_210.50`, made locally) as far as
the PC build does without the disc - the LOADING screen, then the sound
driver (RWA.IRX) the disc would give. Vita3K needs an audio device, or a
dummy one (`SDL_AUDIODRIVER=dummy`, which the script sets): without one its
sceAudioOut crashes the emulator.

## Building

The game's code is generated from your disc's executable, as for the PC, and
then compiled with VitaSDK. On Linux (or WSL):

```sh
git clone --recursive https://github.com/MEGAgameBoy/burnout3-vita && cd burnout3-vita
export VITASDK=/usr/local/vitasdk PATH=$VITASDK/bin:$PATH
# your disc's executable (or isotool.py extract from your image, as in README.md)
cp SLUS_210.50 games/burnout3/extracted/
python3 engine/tools/analyze.py --game games/burnout3
python3 engine/tools/lift.py --game games/burnout3 --structure --emit     # hours: verifies every function (g++)
python3 engine/tools/port.py --game games/burnout3 --target vita build    # games/burnout3/build/port/vita/burnout3.vpk
```

or, with the generated sources in place, CMake directly:

```sh
cmake -S games/burnout3/port -B games/burnout3/build/port/vita -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
      -DPS2_GENERATED_DIR=$PWD/games/burnout3/build/port/generated
ninja -C games/burnout3/build/port/vita
```

`derive.py` (the readable tier) is not needed to build: the port compiles
the lifted code. `discovered.txt` and `vu_programs.txt` from a PC build of
your own (`build/port/`, `build/`) go in the same places here: the code
the game copies at run time, and its VU1 microprograms compiled ahead of
time (without them VU1 runs interpreted, about 1.5x slower).

**VitaSDK's compiler.** VitaSDK's build scripts configure GCC with empty
`CFLAGS`/`CXXFLAGS`, which builds the compiler itself unoptimised; on the
port's generated code it was several times slower than it need be (the
engine's VU test corpus: 11.5 minutes at `-O1`, against 6.7 at `-O3` for
Ubuntu's ARM GCC 13). Rebuilding the compiler (not the libraries) with
`CFLAGS=-O2 CXXFLAGS=-O2` and `make all-gcc install-gcc` in a build of its
own fixes that.

The LiveArea's art is the port's own, drawn by
`port/vita/tools/make_livearea.py` from the PC icon's letters.

## Known issues

* The GPU renderer is a first version (VITA_PLAN.md, "M2"): native
  resolution, the GPU finished at every frame's end, local memory's swizzles
  and decodes on the CPU; the destination alpha test, PABE and FBMSK within a
  channel not yet drawn. Sound goes to the Vita's speaker (sceAudioOut) but
  is not yet heard on hardware.
* No settings menu for the Vita's options yet; rumble does nothing (the Vita
  has none).
* Speed: to be measured on the hardware (VITA_PLAN.md). Expect the race to
  be far from full speed until the VU1 and GS work is made cheaper.

## Memory map

Main memory: 365 MB for the application (param.sfo's `ATTRIBUTE2=12`), of
which newlib's heap takes 224 MB (`VITA_HEAP_MB`) - the guest's 32 MB of RAM,
2 MB of IOP RAM, the GS's 4 MB and its thread's ring, the SPU2, the EE's
fiber stacks (1 MB each) - and the executable the rest. CDRAM (112 MB):
96 MB for the GPU renderer's targets, depth buffers and textures, three
display buffers, the picture's texture, the text screen's framebuffer.
