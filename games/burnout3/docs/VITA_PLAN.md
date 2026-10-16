# The PlayStation Vita port: the plan

Goal (2026-10-15): `burnout3.vpk`, the same game the PC port runs - the PS2
program translated to C++ by the engine, compiled ahead of time with VitaSDK -
booting from the player's own disc image on the Vita
(`ux0:data/burnout3/Burnout 3 - Takedown.iso`), reaching the menus and a race
that can be played. Playable first, fast second.

This is M0's report: what the engine and the port depend on that a Vita does
not have, what the Vita gives instead, what is likely and unlikely to work, and
the order of work. Nothing has been built yet; every number below that is not
quoted from the repository is an estimate, said so, and is to be replaced by a
measurement on the hardware.

## Decisions (2026-10-15, with the user)

* **Renderer: raw GXM**, not vitaGL - a fragment program can read the pixel
  it replaces, so the GS's blending, FBMSK, DATE and AFAIL can be exact.
* **Shaders: Cg compiled on the Vita at first start with `libshacccg`**, kept
  as GXP in `ux0:data/burnout3/shaders/`; players install libshacccg once.
* **The game's code is generated here** from the player's own executable
  (SLUS_210.50), never committed; the VPK built from it is the player's.
* **Repositories**: `MEGAgameBoy/burnout3-vita` (branch `vita`), the engine
  in a fork of ps2-port-engine (branch `vita`).
* **Input and sound native** (`sceCtrl`, `sceTouch`, `sceAudioOut`), not SDL2.

## Status

**M0 - done (2026-10-15).** The engine's platform layer (`engine/platform`:
Windows as before, Linux, the Vita), its SSE paths on ARMv7 through
`ps2rt/sse_on_neon.h` (NEON for integers, VFP for the floats, FPSCR for the
rounding), VitaSDK 15.2 built from source in the workspace, the host's tests
as a VPK and code-size probes. On x86-64 and on ARMv7 under qemu every host
test passes and the arithmetic hashes are identical (EE FPU, macro mode, MMI,
the VU corpus run interpreted and compiled). On the Vita: test VPKs handed
over, results pending (`ps2host_tests.vpk`; `ps2plat_probe32/64/96.vpk`, 34,
67 and 101 MB of code with the linker's veneers).

Found on the way: VitaSDK's build scripts build GCC unoptimised (empty
`CFLAGS`); its compile of the VU corpus took 11.5 min at `-O1` against 6.7
at `-O3` for Ubuntu's GCC 13, and 32 min at the toolchain's default `-O3`.
The compiler is being rebuilt with `-O2` (VITA.md, "VitaSDK's compiler").

**M1 - first build on the hardware (2026-10-16).** The game's code lifted
from the player's executable (9,277 of 9,279 functions, as on the PC); the
code it copies into kernel memory at start-up found by a Linux build of the
port run here without the disc (0x80076000 from 0x00485060, 0x80075000 from
0x00484228 - it then stops where it needs the disc: RWA.IRX), and
translated in. The Vita build: 59 MB of C++ to 25.7 MB of Thumb-2 code
(text), in about ten minutes on two cores with the rebuilt compiler; with
the 224 MB heap about 255 MB of the 365. `burnout3.vpk` (14.5 MB) handed
over: headless (`--no-draw`), the status screen, sound, the log. This first
build is from the lifted code as verification sees it - the plain form of
every function; the structured form (smaller, faster) comes when
verification has finished.

Found on the way: VitaSDK's pthreads give std::thread 32 KB of stack
(1 MB now); `const` hid the main thread's stack size from the loader;
std::filesystem's `absolute()` mangles `ux0:` paths; port.py ran the port
with the checks' round-to-nearest (PS2RT_EE_ROUND_NEAREST leaked from
cppcheck.py) - each fixed.

The first run on the hardware stopped at the first std::thread ("Enable
multithreading to use std::thread: Not owner"): libstdc++ reaches pthreads
through weak references, which the static link left null - so no threads,
and with pthread_cancel alone linked, a function's static (pthread_once)
jumped to 0. All of pthreads is linked now (`--whole-archive`), and the
early start runs after pthread-embedded's. VitaSDK's newlib has no C99
printf formats either (`%zu` printed "zu").

**Vita3K** (the emulator's Linux AppImage, on a virtual display) runs the VPK
here: `port/vita/tools/vita3k_run.py`. With an image of the executable alone
(`--test-disc`) the game boots there as far as the PC build does without the
disc - its LOADING screen, then RWA.IRX, which only the disc has. It caught
the pthread_once crash before the hardware did; it says nothing of speed.

**A picture before the renderer.** The Vita's "window" is the screen: the
software GS's picture, at the end of each frame on the GS thread, scaled to
960 x 544 by the CPU (nearest pixel), with a line over it - pictures a
second, blanks a second, blanks lost. Slow (the GS's rasteriser on the CPU,
the scaling on the GS thread), but it shows the menus while the GXM renderer
is written, and makes M1's measurements with the drawing in or out
(`--no-draw`).

**M2 begun (2026-10-16), ahead of M1's measurements.** The software GS
cannot draw a race on this processor whatever the gate decides, and the
renderer's round trips go through the player's Vita (Vita3K has no
libshacccg), so it is started while M1's numbers come in:

* `engine/gpu/gxm`: the device (GXM, the shader patcher, Cg compiled by
  libshacccg on the Vita and kept as GXP in `ux0:data/burnout3/shaders/`),
  a `ps2gpu::Display` (the picture or a target drawn onto the screen,
  filtered, at the aspect asked for, three buffers through the display
  queue) and `gxm_gs_renderer.cpp`, a `GsRenderer` the bridge drives as it
  drives the Vulkan one (`--gpu`, the Vita's default).
* First version: targets linear RGBA8 in CDRAM, depth linear DF32 + S8
  force-loaded and stored, a scene per target switch; fragment programs by
  what a batch needs; the fixed-function blender with the GS's factor as
  the fragment's alpha (alpha written in a second pass where it is); the
  alpha test by discard, AFAIL's writes in a second pass; GS local memory
  "on the GPU" a copy the CPU keeps, swizzled and decoded by the CPU after
  the GPU has finished, and the GPU finished at each frame's end.
* `ps2gxm_tests.vpk`: the Vulkan renderer's tests (gpu/tests) run on it.
* Next, by what the hardware says: DATE (stencil, as on the PC), PABE,
  PSMT8H through the CLUT, the frame pipelined (notifications rather than
  a finish), fragment passes for the swizzles that cost.

**The fourth build on the hardware (2026-10-16): the LOADING screen.** With
the blank's timer asleep rather than spinning (it starved the EE on their
shared core), the game boots: the IOP rebooted with the disc's modules, the
GTFS file loads, the LOADING screen drawn by the GPU after ~30 s, ~12,000
draws. Then it stayed there for the 30 s the player waited, the EE's thread
busy throughout (`EE idle` never printed), a picture every 2-3 s while the
blanks came eight times as fast (FastLoading). On the PC the same load is
8 s at the console's pace, the EE 85% idle. Two candidates, measured by the
next build: the game's code (the plain lifted form on a 32-bit core), or an
IOP module that never waits - the PC saw one at boot hold the EE past blanks
with its 20-million-instruction slice, which on the Vita's interpreter is
on the order of a second a frame. So:

* `[frames]` says the IOP's and the SPU2's share of the EE's thread, the
  IOP's instructions a second and its busiest thread; `[load]` and
  `[frontend]` say how far the load is (port/overrides.cpp).
* `--iop-share 25` (the Vita's default): the IOP's runs at frames take at
  most a quarter of the EE's thread in real time, an RPC's 2 ms (iop_vm.cpp's
  Run, threads.cpp's IopFrameBudgetNs). Scripted runs (virtual time) are
  unchanged.
* L, R, SELECT and START held three seconds stop the run with its report
  (RequestReportStop): where a stuck game is, without a debugger.

The GPU tests on the hardware: every depth surface the probe tried (DF32,
DF32 + S8, S8D24; linear or tiled; CDRAM or LPDDR) keeps depth across scenes
with force load and store - the renderer's DF32 + S8 linear in CDRAM is
right. The 20 failures left were mostly the renderer's own bug: ReadTarget
(and DisplayTarget, TargetToMemory, DepthToMemory) looked the target up
before recording the draws still pending, so a target made by the last draw
was "not there" (the tests' DEADBEEF). Expected still: REGION_CLAMP and
REGION_REPEAT, FBMSK within a channel. The status screen was also put up
over the GPU's pictures once a second (the flicker the player saw): with the
GPU drawing, only the caption now.

## How it will be measured

* **Exactness, without a renderer** - a per-frame checksum of everything the
  GS is given (GIF packets on its three paths, its privileged registers),
  recorded by the PC build and compared by the ARM builds on the same scripted
  run (`--virtual-time`). Equal checksums say the EE, the IOP, DMA, VIF and the
  VUs on ARM do to the byte what they do on x64. New in the engine
  (`--gs-stream-crc`, game-independent); its `<run>.gsstream.crc` files are
  checksums like the picture CRCs already committed.
* **The host's own tests** (`ps2host_tests`: VuCompiledIsTheInterpreter,
  VuLanesAreTheModel, the IOP's CPU vectors, GS memory) built for ARMv7 and run
  under qemu-arm, and as a test VPK on the Vita.
* **Pictures** - the GXM renderer's frames checked against the software GS's
  (`framediff.py`, with a tolerance, as the Vulkan renderer was), and
  `<run>.gxm.crc` recorded on the Vita for regressions (the GPU draws the same
  pictures each time on the same hardware, as `.gpu.crc` relies on).
* **Speed** - on the Vita only: `--frame-times`, the report's timed sections,
  the frame-pacing timeline (`PS2HOST_PROFILE`, written to `ux0:`), and an
  on-screen counter. Wall-clock per thread, per frame, in the menus and in the
  race script's racing stretch (`--real-time-from`).

## What the port stands on today

The pieces that matter for a new platform (engine v0.7.0, burnout3-pc 0.4.0):

* `engine/runtime` (ps2rt) - the R5900's semantics the generated code
  compiles against: the EE FPU (`eefpu.cpp`), VU0 macro mode (`vu0.cpp`),
  the 128-bit MMI (`lifted_support.cpp`), the VU arithmetic model
  (`ps2rt/vu_math.h`).
* `engine/host` (ps2host) - guest memory, dispatch, the EE kernel and its
  threads (fibers), the IOP (an R3000 interpreter running the game's own IRX
  modules), DMA/VIF, VU1 (interpreter and compiled microprograms,
  `vu_core.h`), the GS model (`gs*.cpp`) on a thread of its own, the GPU
  bridge, the SPU2, the IPU, pads, memory cards, the disc.
* `engine/gpu` (ps2gpu) - the window and the Vulkan GS renderer, behind two
  interfaces with no Vulkan in them: `ps2gpu::Display` and
  `ps2gpu::GsRenderer`.
* `games/burnout3/port` - the game's overrides and PC features. They use the
  host's API only (`ps2host/run.h`, `settings.h`): no platform code at all.

The game's translated code (`src/lifted`, `discovered.cpp`, the compiled VU
programs, `dispatch.cpp`) is generated from the player's disc and is never in
the repository. Its emitters (`lift.py`, `structure.py`, `vurecomp.py`) write
no intrinsics and no compiler-specific keywords; that is to be confirmed on
generated sources in M1, as none are here.

## Dependency inventory

### x86 SIMD and other processor-specific code

| where | what | on the Vita |
| --- | --- | --- |
| `runtime/include/ps2rt/vu_math.h` 23-38, 109-175 | SSE2/SSE4.1: `CleanLanes`, `OrderLanes`, `LanesSse` - the VU's four lanes in single precision | the scalar model (`BinaryOf`) is already beside it; a NEON + VFP version to add (below) |
| `host/include/ps2host/vu_core.h` (77 uses) | SSE in every VuCore step: moves, FMAC, OPMULA/OPMSUB, FTOI, MAC/STATUS flags, landing; an AVX2 store of ready cycles (696-700) | each SSE block has an `#else` scalar path today; NEON paths to add; the AVX2 block is x86-only already |
| `host/src/vu.cpp` 21, 806-813 | `_mm_getcsr`/`_mm_setcsr`: a VU run rounds toward zero, denormals flushed | FPSCR (RMode = RZ, FZ = 1) - which VFP obeys and **NEON does not** (ARMv7 Advanced SIMD always rounds to nearest) |
| `runtime/src/vu0.cpp` 13, 128-134, 207-216 | macro mode's lanes in SSE; MXCSR set and restored around every COP2 instruction | FPSCR likewise; on ARM the EE thread is better kept in RZ + FZ for good (`vmsr` waits for the VFP pipeline) - safe for `eefpu.cpp`, which rounds toward zero in software either way; to be measured |
| `runtime/src/lifted_support.cpp` 10, 151-169, 236-240 | 128-bit MMI (pcgt\*, pceq\*, pmaxh, paddub, ...) in SSE2, with the x64-only `_mm_cvtsi128_si64`, `_mm_set_epi64x` | `LanesScalar`, `InterleaveScalar` exist; NEON versions to add |
| `host/src/main.cpp` 24-52, 157 | `__cpuid`, `__cpuidex`, `_xgetbv`: the AVX2 check | none |
| `host/src/profile.cpp` 20-44 | `__rdtsc` for timed sections | the kernel's microsecond clock |
| `host/src/gs_thread.cpp` 33, 103 | `_mm_pause` in a spin | `yield` |
| `host/src/ipu.cpp` 346 | `_byteswap_uint64` | `__builtin_bswap64` |
| `host/CMakeLists.txt` 98-107 | `/arch:AVX2 /EHsc /bigobj /Z7`, dbghelp, winmm | `-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -ffp-contract=off`, exceptions on |

The rounding is the one that matters for the pictures: `vu.cpp`'s VuRounding
comment has the race's smoke puffs lit with wrapped colours and stretched
when VU arithmetic rounded to nearest. On ARMv7 the exact path is
therefore VFP scalar arithmetic under FPSCR RZ + FZ for the float lanes, with
NEON for the integer work around it (cleaning operands, masks, flags) - VFP and
NEON share registers, so the lanes need no moves. A NEON-float path (round to
nearest) is possible only as an opt-in inexact mode, judged by its pictures,
like `--vu-fast`.

### MSVC-only code and flags

* `__forceinline` (`vu_math.h` 18, `vu_core.h` 30) - already has a GCC branch.
* `long long(ready)` (`vu_core.h` 698) - not C++ GCC accepts; inside the AVX2
  block, so never seen on ARM.
* `#pragma comment(lib, ...)` (`audio.cpp`, `threads.cpp`).
* `localtime_s`/`gmtime_s` (`cdvd.cpp`, `devices.cpp`, `mc2.cpp`, `mcserv.cpp`)
  - `localtime_r`/`gmtime_r` (the arguments the other way round).
* `freopen_s`, `_dup2`, `_fileno`, `_stricmp` (`main.cpp`; `_stricmp` is the
  disc check, which stays, as `strcasecmp`).
* `_fseeki64`/`_ftelli64` (`disc.cpp` 80-114): the image is read through one
  `FILE*` with 64-bit seeks - on the Vita `sceIoOpen`/`sceIoPread` (64-bit
  offsets; newlib's `off_t` there is not to be trusted past 2 GB).
* The port's and engine's CMake: `/MT`, `/d1trimfile`, `/bigobj`, `/Z7`,
  `/SUBSYSTEM:WINDOWS`, an RC icon and version block.

### Windows APIs

| file | what | on the Vita |
| --- | --- | --- |
| `host/src/threads.cpp` | **fibers** for the EE's threads and the coroutines (`ConvertThreadToFiber`, `CreateFiber` with 1 MB stacks, `SwitchToFiber`, `DeleteFiber`); high-resolution waitable timers; `timeBeginPeriod`; `SetThreadPriority` (time-critical blank timer); power-throttling opt-outs | a small ARM context switch (r4-r11, sp, lr, d8-d15, FPSCR) in the platform layer; `sceKernelDelayThread`; `sceKernelCreateThread` with priority and core |
| `host/src/audio.cpp` | waveOut, 1,024-frame buffers | `sceAudioOut`, 48 kHz stereo, its own thread |
| `host/src/window.cpp` | the GDI fallback (`GetDC`, `StretchDIBits`); Win32 virtual keys mapped to SDL3 scancodes | the GXM display; the Vita's buttons |
| `host/src/dbc.cpp` | the keyboard by virtual key; `HostRumble` | `sceCtrl` (+ `sceTouch` for the missing buttons); rumble does nothing |
| `host/src/main.cpp` | `AttachConsole`, `GetStdHandle`, `GetOpenFileNameA` (the disc picker), `MessageBoxA`, `GetModuleFileNameW`, `CONOUT$`/`NUL` | a settings file and a search for the image; the log in `ux0:data/burnout3/`; errors on screen |
| `host/src/host.cpp` | `GetModuleFileNameW` (the memory card beside the executable); `OpenWindow(L"PS2 port")` | `ux0:data/burnout3/` |
| `host/src/profile.cpp` | the sampling profiler (`SuspendThread`, `GetThreadContext`, `StackWalk64`, dbghelp's `Sym*`), `SetUnhandledExceptionFilter`, `timeBeginPeriod` | no user-mode way to sample another thread: `--sample` off; timed sections and the timeline kept |
| `host/src/pc_menu.cpp` | keys by virtual key | the Vita's buttons |
| `gpu/src/vulkan_display.cpp` | waitable timer and priority for the present thread | GXM's display queue |
| CMake | comdlg32, dbghelp, winmm, the RC file | - |

The host's public API is Windows-shaped in two places: `OpenWindow(const
wchar_t*)` and `HostKeyDown(int key)` ("a Win32 virtual key").

### SDL3, Vulkan, volk, glslang

All of it is in `engine/gpu`, fetched at configure time:

* `vulkan_display.cpp` (1,601 lines): the SDL3 window, events, keyboard,
  gamepad (rumble, DualSense trigger effects), mouse and cursor, fullscreen,
  the swapchain and present thread, FXAA/SSAA, the overlay, F1/F12/Tab.
* `gpu.cpp` (208): the Vulkan device.
* `gs_renderer.cpp` (2,828): the GS renderer. What it uses that GXM has no
  equivalent for: compute (`vkCmdDispatch`: `swizzle.comp` - targets and depth
  buffers into and out of the GPU's copy of GS memory -, `decode.comp` -
  textures and CLUTs from it -, `patch.comp`, `downsample.comp`,
  `fxaa.comp`), storage images and buffers, dual-source blending (the blend's
  C as a second output), logic ops AND/OR (FBMSK in part of a channel), a
  stencil pass for DATE, Vulkan 1.3 dynamic rendering and dynamic state.
* GLSL compiled to SPIR-V by glslang: `gs.vert`, `gs.frag` (and its
  single-output and shader-blend variants), `date.*`, `depth_load.frag`,
  `swizzle.comp` (three variants), `decode.comp`, `patch.comp`,
  `downsample.comp`, `fxaa.comp`, `gs_address.glsl` - about 970 lines.

The host reaches it through factories only - `ps2gpu::CreateGpu` and
`CreateGsRenderer` (`gpu_bridge.cpp` 1611-1615), `CreateVulkanDisplay`
(`window.cpp` 168) - so a GXM backend implements `Display` and `GsRenderer`
and the bridge (1,864 lines, which keeps GS memory and the GPU's images in
step) stays as it is.

### C++ library features to check on VitaSDK

* `std::filesystem` in eight files (`host.cpp`, `main.cpp`, `mc2.cpp`,
  `mcserv.cpp`, `devices.cpp`, `texture_pack.cpp`, `gpu_bridge.cpp`, the
  tests). Vita paths (`ux0:data/...`) are not POSIX paths, and there is no
  working directory to make one absolute against: a small wrapper of our own.
* `std::thread`, `std::mutex`, `std::condition_variable` (`gs_thread.cpp`,
  `vif1_worker.cpp`, `gs_draw.cpp`, `texture_pack.cpp`, `host.cpp`, `timers`):
  VitaSDK has them over its pthreads, but with no priority or core - the hot
  threads move to the platform's thread.
* `thread_local` (`vu.cpp` 163 - looked up per VU run -, `gs_draw.cpp`,
  `vif1_worker.cpp`, `timeline.cpp`, `window.cpp`): to check it is not
  emulated TLS on a hot path.
* `std::getenv`: some forty `PS2HOST_*` switches. The Vita has no
  environment: they are read from `ux0:data/burnout3/env.txt` instead.
* `wchar_t` is 4 bytes on newlib (2 on Windows).
* Stacks: the EE's fibers have 1 MB each; the Vita's main thread has a small
  stack unless `sceUserMainThreadStackSize` says otherwise.

### The Python pipeline

* `isotool.py`, `analyze.py`, `derive.py`, `lift.py`, `vurecomp.py` are plain
  Python. The compiled-code check (`ps2decomp/cppcheck.py`, `liftcheck.py`)
  builds with MSVC (`vswhere`, `vcvars64.bat`, `cl`, `__declspec(dllexport)`)
  and says it is unavailable without it.
* `port.py build` runs Visual Studio's CMake and Ninja through `cmd /c` and a
  batch file, then the tests and the game; `--discover` (code copied at run
  time) needs a native build that runs.

Since the C++ the pipeline writes is the same whatever it is compiled for, the
plan keeps generation where it is (the player's Windows machine, as today) and
adds `port.py build --target vita`: the VitaSDK build of the same sources
(Linux, macOS or WSL), taking the PC build's `discovered.cpp` and
`vu_programs.txt` as they are.

### Already portable

* Guest memory is ordinary allocations reached by 32-bit guest addresses
  (`memory.h`, `generated.h`'s `RamPointer`) - no address-space reservation,
  no pointer-size assumption found (one `uintptr_t`, in `dma.cpp`).
* `long` is 32 bits on Windows and on ARM32 alike; both are little-endian.
* Every SIMD block of the VU and MMI code has its scalar twin, and the host's
  tests compare the two.
* The EE FPU rounds toward zero in software (`eefpu.cpp`): it does not depend
  on the host's rounding mode.

### What is costly on a 32-bit processor

Every EE register value in the generated code is a `u64` (`generated.h`
12-15; `lifted::Context` keeps `u64 r[32]` and `rh[32]`), because that is the
width the code was verified at. On a 32-bit core each add, compare and shift
of a 64-bit value is two or more instructions. Most of it is word arithmetic
whose upper half is only the sign, so a later engine change could give the
emitters a register-width analysis (emit `s32` where the upper half is
provably the sign; verified as everything else is). Not in the first
milestones; it is the main lever on the EE's speed and code size on ARM.

## The Vita, as far as checked

* **Processor**: four Cortex-A9 cores with NEON (VFPv3, no fused multiply-add),
  three for an application (`SCE_KERNEL_CPU_MASK_USER_0..2` in vita-headers'
  `psp2/kernel/cpu.h`). 444 MHz is reachable from user mode
  (`scePowerSetArmClockFrequency`); the 500 MHz step needs a kernel plugin
  (PSVshell's settings list it).
* **Memory** (vita-rust book's memory page): 256 MiB main for an
  application, 365 MiB with `ATTRIBUTE2=12` in param.sfo (a flag, nothing the
  player installs); 112 MiB CDRAM; 26 MiB physically contiguous; newlib's heap
  128 MiB unless `_newlib_heap_size_user` says otherwise.
* **GPU (GXM)**: no compute; fragment programs can read the pixel they are
  about to write (the program flag Vita3K models as FRAGCOLOR_USED) - blending
  is shader code on this GPU; paletted textures in hardware (P4, P8);
  BC1-BC3 and PVRTC; depth DF32, DF32 + S8, S8D24, D16. A tile-based renderer:
  every change of render target is a scene, with its tiles stored and loaded.
* **Shaders**: there is no open-source GXP compiler. Homebrew compiles Cg at
  run time with `libshacccg.suprx`, which the player installs once with known
  tools (it comes from the PSM runtime; consolemods' guide). Sony's offline
  compiler is part of the official SDK, which this project should not use.
* **Not found**: a documented limit on an executable's size, and how
  `vita-elf-create` handles tens of megabytes of code with long-branch veneers.
  To be tested in M0 with a synthetic executable.

## Feasibility: the honest part

### Processor time

From the repository, on the laptop the PC port was measured on: the race's
busiest stretches run at ~52 pictures a second with the VIF1/VU1 worker and
the GS thread **each near 80% busy** (FINDINGS, "Frame rate"): some 15 ms of
work each per picture. VU1 runs ~1.17 million instruction pairs per race frame
(FINDINGS, "A race frame, timed"); the compiled VU code costs 4.93 ns a pair
in fast mode (PORTING.md).

A Cortex-A9 core at 444 MHz is, for this kind of code, an estimated **20 to 40
times** slower than one of that laptop's cores: the clock alone is ~10x, a
2-wide core against a 6- to 8-wide one, VU lanes done one at a time in VFP
(exact rounding) instead of four in SSE, and 64-bit register arithmetic on a
32-bit machine. With the code as it is, that gives, per race frame:

| thread | PC | Vita (estimate) |
| --- | --- | --- |
| VIF1 + VU1 worker | ~15 ms | 300-600 ms |
| of which VU1 alone | ~5.8 ms | 115-230 ms |
| GS thread (GS model + bridge + renderer's recording) | ~15 ms | 300-600 ms |
| EE (the game, ~15% of the work) | ~3 ms | 60-120 ms |

So the race would run at roughly **2 to 3 pictures a second** as ported, and VU1
alone caps it below ~9 even if everything else cost nothing. The menus cost a
fraction of a race frame; whether they reach the game's own 30 depends on how
small that fraction is. These are estimates; M1 measures them. If they hold,
a race at 20 to 30 pictures a second needs the worker and the GS thread each
**roughly 6 to 18 times cheaper**, which is not a tuning pass:

* VU1 - an ARM-shaped `vurecomp` output (32-bit cycle arithmetic, stalls
  resolved when the block is compiled, NEON for the integer side): perhaps
  2-3x. Beyond that, native replacements for the few 3D microprograms that do
  most of the work (#52, #18, #21, #26 in FINDINGS), producing the same GS
  packets, checked against the interpreter with the existing VU snapshot
  tools - large, and they lean on pipeline timing, so exactness is work.
* The GS thread - the BLUR setting's override (0x002FA9A0) already drops the
  glow and blur passes, the costliest; a leaner GIF-to-GXM path for the plain
  cases; frame skip (which saves GS and GPU work, not VU1's).
* The EE - FPSCR set once per thread; `-O2` only where the profile says;
  later the register-width analysis.

Frame limiting at 30 is natural here (the game itself drops to every other
blank when it cannot keep up), but it is a ceiling, not a target that is in
reach without the work above.

### Memory (estimate)

| | main (256 / 365 MiB) | CDRAM (112 MiB) |
| --- | --- | --- |
| the executable (code + data) | 30-80 MiB: unknown until built | - |
| guest: RDRAM 32, IOP 2, SPU2 2, BIOS 4 (need not be allocated), the rest | ~38 MiB | - |
| GS model (4 MiB local memory and its tables), GS ring (16 MiB today, smaller here) | ~10-22 MiB | - |
| fiber stacks (1 MiB per EE thread and coroutine today) | 20-40 MiB, less once measured | - |
| disc read-ahead, audio, host vectors | ~30 MiB | - |
| GS targets at 1x (640 x 448 RGBA8 = 1.1 MiB each) and depth | - | 30-45 MiB |
| GS memory's copy and views on the GPU, textures | - | ~40 MiB |
| display buffers (3 x 960 x 544), vertex and uniform rings | - | ~16 MiB |

It should fit in the default 256 MiB, with `ATTRIBUTE2=12` as headroom; the
unknown is the code's size. Textures are not where memory goes: the PS2 had 4
MB for everything, so its textures decoded are tens of megabytes at most. No
DXT/PVRTC conversion is planned - it would cost processor time and quality to
save memory that is not short; PSMT8 and PSMT4 can stay paletted (GXM's P8/P4,
the CLUT as the palette), which saves decoding too.

### Resolution

GS targets are drawn at 1x (640 x 448) or above, **not below**. The game draws
data as pixels - a CLUT written by a one-pixel draw that the fog then reads,
the glow's threshold laid down as colour and as Z (FINDINGS, "The race on the
GPU") - and a scale under 1x would corrupt it. 1x is 287 thousand pixels, fewer
than the Vita's screen (522 thousand), so the display scales up to 960 x 544:
4:3 pillarboxed (725 x 544), or 16:9 with `widescreen.cpp`'s camera and HUD
anchors - the Vita's screen is 16:9, so that is the natural default. 1.5x is an
option if the GPU has room.

### Code size, compile time, link

Unknown until the generated sources are compiled with GCC. Concerns: GCC
needs far more time and memory than MSVC on huge generated functions; LTO over
tens of megabytes of code is out of the question; Thumb-2 calls reach ±16 MB,
so the linker adds veneers, which `vita-elf-create` must carry; and whether
the loader accepts such an executable at all. Plan: `-O1` (or `-Os`) for the
lifted code by default, `-O2` for the files the profile names and the VU
programs, LTO for the engine only if it helps; measured in M1.

## The design

### Where things go

```
engine/
  platform/include/ps2plat/   threads (priority, core, stack), fibers, clock and sleep,
                              files (64-bit reads, paths), audio out, log, messages
  platform/win32/             what must have two implementations, moved from host/src
                              (fibers, timers, waveOut, the dialogs) - nothing else moves
  platform/vita/              sceKernel threads and semaphores, the ARM fiber switch (.S),
                              sceIo, sceAudioOut, sceCtrl/sceTouch, clocks, crash log
  gpu/gxm/                    GxmDisplay (ps2gpu::Display), GxmGsRenderer (ps2gpu::GsRenderer),
                              the Cg shaders
  cmake/vita.cmake            ps2_add_port for the Vita: SELF, VPK, param.sfo, LiveArea
games/burnout3/port/vita/     the Vita's settings entries, touch mapping, LiveArea art
                              (drawn by a script, as the icon is), runs for the Vita
```

The Windows build stays as it is: x86 code paths unchanged, the same flags.

### Threads and cores

Three cores for three heavy threads: core 0 the EE (the game's fibers, the
IOP interpreter, the blank timer), core 1 the VIF1/VU1 worker, core 2 the GS
thread. Audio mixing (short, frequent, high priority) and disc read-ahead (low
priority, mostly waiting) share them; where they go is decided by the
timeline, not now.

### The renderer: raw GXM (decided)

Raw GXM over vitaGL: the PC renderer is explicit about pipelines, passes and
memory, and so is GXM (scenes, surfaces, memory blocks), where vitaGL would
put a GL state machine and its own memory manager between the bridge and the
hardware; GXM has palettes (P8/P4), memory the CPU can map, and control over
scenes, which a tile-based GPU needs when GS effects switch targets many
times a frame. The cost: memory, scenes and the shader patcher are ours
(vita2d and vitaGL as references).

**What the hardware probes found (2026-10-16, GXM probes 1-3 on the player's
Vita):**

* libshacccg (SDK 3.0.0, shacc r13225) loads from `ur0:data/` and compiles a
  shader in 82-92 ms: a few dozen shaders compiled once, cached as GXP.
* A scene drawn into memory the CPU reads back is exact (a constant colour
  from literals and from a uniform), given a depth/stencil surface - without
  one every draw fails (0x805B001F).
* **Integers in fragment programs work**: `floor`, `(int)`, XOR, AND, shifts,
  multiplies, all exact (`(int)` rounds to nearest there: floor first). GS
  memory's swizzle can run in fragment passes.
* **The destination cannot be read in the shader with this compiler.**
  FRAGCOLOR is "not a valid semantic for a varying"; on a `uniform` any
  semantic is ignored (a plain uniform: proved by filling it); `__regformat`
  and `__nativecolor` are not keywords anywhere; no program compiled had
  GXP's FRAGCOLOR_USED flag. (The hardware has it - Vita3K reads such
  programs' output register - so a GXP compiled elsewhere could; there is no
  open compiler that writes one.)

So the blend is the fixed-function blender's, as on the PC before shader
blending - and as PCSX2's GLES-era renderers did it:

| GS | GXM |
| --- | --- |
| Cv = (A - B) * C + D, the common cases (Cs-Cd)*As+Cd, Cs*As+Cd, (Cs-Cd)*FIX+Cd, Cd*As..., additive | the blender's factors, colour and alpha separately (SceGxmBlendInfo): the source's alpha written as is; FIX given to the blend as the source alpha where alpha is not written |
| C = Ad (the destination's alpha), factors over 1 (As > 0x80), what the blender cannot reach | the target's region copied to a texture (the scene ended, a transfer), the draw blending in the shader from the copy - rare, costly; approximations behind a setting first |
| DATE | the target's alpha bit into stencil by a pass, the draw stencil-tested (S8D24 surfaces) - as on the PC |
| AFAIL | two passes, the shader told which pixels each keeps (discard) - as on the PC |
| FBMSK by channel | the colour mask; within a channel (the race's 0x3FFF through a 16-bit view): the shader-blend path |
| compute swizzle/decode/patch over GS memory | fragment passes over a 1024 x 1024 surface, the address computed per pixel with integers (proved), or NEON on the CPU where cheaper |
| D32 + S8 depth | S8D24 (Z24 exact; Z32 rounded) / D16; depth sampled as a texture (the race's depth of field) needs depth stored to memory (force-store) - open |
| FXAA, SSAA | dropped |
| three frames in flight | GXM notifications and the display queue |

Shaders: Cg, compiled on the Vita at first run with `libshacccg` and kept as
GXP files in `ux0:data/burnout3/shaders/` - the player installs libshacccg
once, as most vitaGL homebrew asks. If "shacc" in the brief is an open
offline compiler I do not know of, it would replace that.

### Platform layer

* **Input**: native `sceCtrl` and `sceTouch` behind the host's seams
  (`HostPad`, `dbc.cpp`), not SDL2: SDL2's Vita video owns GXM itself, so it
  could only ever give input and audio, which are a few hundred lines natively.
  The Vita's buttons as the DualShock 2's; L2 and R2 on the rear touchpad's
  halves and L3, R3 on its top corners (or the front's corners: a setting);
  radial dead zones; rumble does nothing. The front touchscreen as the mouse
  (`TakeHostMouse`), so `mouse.cpp` gives touch menus for nothing. Cross's
  pressure matters to the race (FINDINGS, "The race's controls"): the right
  stick as an analog throttle and brake, through `controls.cpp`'s modern
  controls, is an option.
* **Audio**: the SPU2's 48 kHz stereo into `sceAudioOut` on a thread of its
  own, a few 256-sample grains queued.
* **Disc**: `sceIoOpen`/`sceIoPread` with 64-bit offsets; a cache of 64 KB
  blocks with read-ahead on a low-priority thread; the image searched for in
  `ux0:data/burnout3/`, then `uma0:`, `imc0:`, `grw0:`; the check that the
  image is the disc `game.json`'s executable comes from stays.
* **Saves**: `ux0:data/burnout3/memcard/` (`mc2.h`'s directory card).
* **Settings**: `ux0:data/burnout3/settings.ini`. PC SETTINGS in the main menu
  keeps its place with the Vita's entries: internal resolution (1x/1.5x), frame
  limit (30/60), frame skip, BLUR, aspect (4:3/16:9), FPS counter, where L2/R2
  are. The F10 HUD editor and Tab are not on the Vita; F12's capture becomes a
  button combination writing to `ux0:data/burnout3/captures/`, replayable on
  the PC.
* **Movies**: the IPU decodes MPEG in software with a reference IDCT
  (`ipu.cpp`): too slow here, very likely. Start already skips them
  (`SkipMovie`); the race training's tutorial, which no button skips, needs an
  override.

### Test runs on the hardware

`ux0:data/burnout3/run.args`, when present, is the command line: the VPK runs
that scripted run and writes its log, checksums and chosen frames to
`ux0:data/burnout3/runs/<run>/`. A script on the PC puts the files there and
collects the results over FTP (VitaShell's server); with vitacompanion
installed (a developer plugin) it can also start the run. The runs are the
existing `port/runs/*.args`, unchanged, so the same frame numbers mean the same
moments as on the PC.

## Milestones

**M0 - build and portability, no game code.** Platform interfaces with Vita
implementations and null renderer/audio; NEON and VFP paths; FPSCR for the VU
and macro mode; the GCC-specific fixes above; CMake for the VitaSDK toolchain;
a test VPK of `ps2host_tests`; a synthetic executable with tens of megabytes of
code to find the loader's limits. Done when the host's tests pass under
qemu-arm and on the Vita, and the synthetic executable loads.
Risks: VitaSDK's C++ library (filesystem, threads); the fiber switch with C++
exceptions inside fibers; vita-elf-create on large code.

**M1 - boot headless.** The player's generated sources built with VitaSDK
(compile time, peak memory, code size measured); the disc through `sceIo`;
the run reaches the game's first frames with the null renderer, and the GS
stream checksums of `car_select` match the PC's frame for frame. Then the
numbers: `--frame-times` and the timed sections in the menus and - headless,
via the race script - in the race.

**Gate - is the race reachable?** With M1's measurements, choose: carry on as
planned; first make VU1 and the GS thread cheaper (above); or settle what
"playable" will mean. The renderer is weeks of work and should not start before
this is known.

**M2 - GXM.** A clear colour, then textured triangles, then the first menu
frame; developed against PC captures replayed on the Vita (`--gs-replay`), so
the renderer can be tested before the whole game runs on it.

**M3 - input and audio**: menus navigable, sound.

**M4 - a race played**, at whatever rate M1 found, with BLUR off as the fallback.

**M5 - speed and polish**: memory, the texture cache, frame pacing (the 30
limit, frame skip), the settings, LiveArea art, `docs/VITA.md`, the release
checks (skill `ps2-release`).

## Risks, most serious first

1. **Processor time** (above): likely, and it decides whether the race is
   playable. Measured in M1, before the renderer.
2. **64-bit registers on a 32-bit core**: code size and EE time.
3. **Exact VU rounding**: NEON cannot round toward zero, so exact lanes are VFP
   scalar code, slower; an inexact NEON mode must be judged by its pictures.
4. **The GS on GXM**: the PC design rests on compute shaders; integer work in
   Cg fragment programs, and reading the destination pixel through libshacccg,
   are unproven (the first things M2 tries); depth sampled as a texture on a
   tile-based GPU; the cost of a scene per target switch.
5. **Shaders**: no open GXP compiler; players install libshacccg.
6. **Executable and build**: the loader's limits, veneers, GCC's time and
   memory on the generated code.
7. **The IOP interpreter** running the sound driver (RWA.IRX) and other
   modules: its cost on this processor is unknown.
8. **Disc latency**: reads are synchronous on the EE's side today.
9. **Upstream**: the engine is another repository with its own rule that every
   game's runs pass before a tag. The Windows build cannot be built or run here
   (no MSVC, no disc), so x86 paths stay as they are and the PC regressions are
   run on the player's machine.

## What this workspace can and cannot do

* It can: build VitaSDK from source and the port with it; build the engine
  for ARMv7 with GCC (Ubuntu's arm-linux-gnueabihf) and run its tests under
  qemu-arm; generate the game's code from the player's executable; run the
  VPK in Vita3K as far as the game goes without its disc; write and review
  every line of the port.
* It cannot: run anything on a Vita, measure speed, or run the game past
  what its executable alone gives (that needs the disc). Those happen on the
  player's Vita, with the logs sent back.

## Sources

* Vita memory budget: https://vita-rust.github.io/book/dev/mem.html
* Application cores: vita-headers, `include/psp2/kernel/cpu.h`
  (https://github.com/vitasdk/vita-headers)
* GXM formats and program flags: vita-headers `include/psp2/gxm.h`; Vita3K,
  `vita3k/gxm/include/gxm/types.h` (https://github.com/Vita3K/Vita3K)
* libshacccg: https://consolemods.org/wiki/Vita:Installing_Libshacccg.suprx
* vitaGL: https://github.com/Rinnegatamante/vitaGL
* CPU steps: https://consolemods.org/wiki/Vita:PSVshell
