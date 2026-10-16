"""The port's VPK run in Vita3K (the emulator's Linux build): installed, run
for a while on a virtual display, its log and a screenshot a second
collected. A check between builds that the program starts, threads, reads
its disc and draws - not a measure of speed, which only the hardware gives.

    python games/burnout3/port/vita/tools/vita3k_run.py --vita3k ~/Vita3K/AppRun
        [--vpk games/burnout3/build/port/vita/burnout3.vpk] [--seconds 60]
        [--disc "path/to/Burnout 3 - Takedown.iso" | --test-disc]
        [--run-args port/runs/menus.args] [--out games/burnout3/build/vita3k]

--test-disc: no disc image at hand, an image made here of the executable
alone (extracted/SLUS_210.50, from your own disc) with its SYSTEM.CNF - the
port's disc check takes it, and the game boots as far as it can without the
rest of the disc (the PC build stops at the same place: RWA.IRX). Made in
Vita3K's ux0:, never in the repository. Needs pycdlib (pip install pycdlib).

Vita3K's sceAudioOut crashes the emulator when SDL has no audio device: the
run gets SDL's dummy one. With no DISPLAY, Xvfb gives a screen (screenshots
need ImageMagick's `import`).
"""

import argparse
import io
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
TITLE_ID = "BRNT00003"          # port/CMakeLists.txt's VITA_TITLE_ID
APP = "burnout3"                # ux0:data/burnout3


def vita3k_ux0():
    """Vita3K's ux0: its config's pref-path, or the default."""
    config = os.path.expanduser("~/.config/Vita3K/config.yml")
    pref = os.path.expanduser("~/.local/share/Vita3K/Vita3K")
    if os.path.exists(config):
        with open(config, encoding="utf-8") as f:
            for line in f:
                if line.startswith("pref-path:"):
                    value = line.split(":", 1)[1].strip().strip('"')
                    if value:
                        pref = value
    return os.path.join(pref, "ux0")


def make_test_disc(executable, out):
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=1)
    name = os.path.basename(executable).upper()
    cnf = ("BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n" % name).encode()
    iso.add_fp(io.BytesIO(cnf), len(cnf), "/SYSTEM.CNF;1")
    with open(executable, "rb") as f:
        data = f.read()
    iso.add_fp(io.BytesIO(data), len(data), "/%s;1" % name)
    iso.write(out)
    iso.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vita3k", default=os.environ.get("VITA3K", "Vita3K"),
                    help="Vita3K's executable, or AppRun of its AppImage unpacked (--appimage-extract)")
    ap.add_argument("--vpk", default=os.path.join(GAME, "build", "port", "vita", "burnout3.vpk"))
    ap.add_argument("--seconds", type=int, default=60)
    disc = ap.add_mutually_exclusive_group()
    disc.add_argument("--disc", help="an image of your disc, linked into Vita3K's ux0:data/burnout3/")
    disc.add_argument("--test-disc", action="store_true", help="an image of the executable alone (above)")
    ap.add_argument("--run-args", help="the run's options (ux0:data/burnout3/run.args); none: played as started")
    ap.add_argument("--out", default=os.path.join(GAME, "build", "vita3k"), help="where the log and screenshots go")
    ap.add_argument("--no-shots", action="store_true")
    args = ap.parse_args()

    ux0 = vita3k_ux0()
    data = os.path.join(ux0, "data", APP)
    os.makedirs(data, exist_ok=True)
    if args.test_disc:
        exe = os.path.join(GAME, "extracted", "SLUS_210.50")
        if not os.path.exists(exe):
            sys.exit("no %s: --test-disc makes its image from your disc's executable" % exe)
        make_test_disc(exe, os.path.join(data, "Burnout 3 - Test.iso"))
    elif args.disc:
        target = os.path.join(data, os.path.basename(args.disc))
        if not os.path.exists(target):
            os.symlink(os.path.abspath(args.disc), target)
    run_args = os.path.join(data, "run.args")
    if args.run_args:
        shutil.copyfile(args.run_args, run_args)
    elif os.path.exists(run_args):
        os.remove(run_args)
    log = os.path.join(data, APP + ".log")
    if os.path.exists(log):
        os.remove(log)

    os.makedirs(args.out, exist_ok=True)
    shots = os.path.join(args.out, "shots")
    shutil.rmtree(shots, ignore_errors=True)
    env = dict(os.environ, SDL_AUDIODRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    xvfb = None
    if not env.get("DISPLAY"):
        display = ":77"
        xvfb = subprocess.Popen(["Xvfb", display, "-ac", "-screen", "0", "1280x720x24"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        env["DISPLAY"] = display
        time.sleep(1)
    if not env.get("XDG_RUNTIME_DIR"):
        runtime = os.path.join(args.out, "xdg")
        os.makedirs(runtime, mode=0o700, exist_ok=True)
        env["XDG_RUNTIME_DIR"] = runtime
    vita3k = os.path.abspath(os.path.expanduser(args.vita3k)) if os.sep in args.vita3k else args.vita3k
    if vita3k.endswith("AppRun"):
        env.setdefault("APPDIR", os.path.dirname(vita3k))
    emulator_log = os.path.join(args.out, "vita3k.log")
    with open(emulator_log, "w") as out:
        # Installed (a VPK given as the content to run), then run with the SDL
        # frontend: a plain window, gone when the program ends.
        vita = subprocess.Popen([vita3k, "--frontend", "sdl", "-B", "OpenGL", os.path.abspath(args.vpk)],
                                stdout=out, stderr=subprocess.STDOUT, env=env,
                                cwd=os.path.dirname(vita3k) if os.sep in vita3k else None)
        started = time.time()
        shot = 0
        while vita.poll() is None and time.time() - started < args.seconds:
            time.sleep(1)
            if not args.no_shots and shutil.which("import"):
                os.makedirs(shots, exist_ok=True)
                shot += 1
                subprocess.run(["import", "-window", "root", "-display", env["DISPLAY"],
                                os.path.join(shots, "shot_%03d.png" % shot)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if vita.poll() is None:
            vita.terminate()
            try:
                vita.wait(10)
            except subprocess.TimeoutExpired:
                vita.kill()
    if xvfb:
        xvfb.terminate()
    if os.path.exists(log):
        shutil.copyfile(log, os.path.join(args.out, APP + ".log"))
        with open(log, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        print("\n".join([line for line in lines if not line.startswith("[call]")][-60:]))
    else:
        print("no %s: the program did not get as far as its log (Vita3K's: %s)" % (log, emulator_log))
    print("\nlog, screenshots, Vita3K's log: %s" % args.out)


if __name__ == "__main__":
    main()
