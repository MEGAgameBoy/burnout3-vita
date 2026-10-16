# Burnout 3 on the PlayStation Vita

The same port as the PC's - the PS2 program translated to C++ by the engine,
not emulated - compiled for the Vita with VitaSDK. Unofficial and fan-made,
like the PC port: you need your own disc (NTSC-U, SLUS-21050), and nothing of
the game is in this repository or in what it builds from it.

**Where it stands (M1, 2026-10-16): no picture yet.** The Vita build runs the
game headless - a status screen says how fast it goes, its log what it does -
while the GXM renderer is written (M2). The plan, the measurements and the
risks: [VITA_PLAN.md](VITA_PLAN.md).

## Installing and running

1. A Vita with HENkaku/Ensō (or h-encore) and VitaShell.
2. Install `burnout3.vpk` with VitaShell. Its data directory is
   `ux0:data/burnout3/` (made at the first start).
3. Put the image of your own disc in that directory, as an `.iso` (or a
   `.cue` with its `.bin`):
   `ux0:data/burnout3/Burnout 3 - Takedown.iso`. The port also looks in
   `data/burnout3/` on `uma0:`, `imc0:`, `grw0:` and `xmc0:` (memory cards and
   SD adapters) and takes the first image that is the game's disc; any other
   image is refused.
4. Start it from its bubble.

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

* No picture (M2: the GXM renderer), so no menus to see. Sound goes to the
  Vita's speaker (sceAudioOut) but is not yet heard on hardware.
* No settings menu for the Vita's options yet; rumble does nothing (the Vita
  has none).
* Speed: to be measured on the hardware (VITA_PLAN.md). Expect the race to
  be far from full speed until the VU1 and GS work is made cheaper.

## Memory map

Main memory: 365 MB for the application (param.sfo's `ATTRIBUTE2=12`), of
which newlib's heap takes 224 MB (`VITA_HEAP_MB`) - the guest's 32 MB of RAM,
2 MB of IOP RAM, the GS's 4 MB and its thread's ring, the SPU2, the EE's
fiber stacks (1 MB each) - and the executable the rest. CDRAM (112 MB) will
hold the GXM renderer's targets and textures; until then only the status
screen's framebuffer.
