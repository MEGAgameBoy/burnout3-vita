/*
 * Every game port's entry point. The game supplies its translated functions (the
 * table engine/tools/port.py generates) and its overrides; the rest is shared.
 * What differs from one system to another - the disc picker, the console, the
 * log's place, message boxes - is the platform layer's (ps2plat).
 */

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "ps2host/disc.h"
#include "ps2host/host.h"
#include "ps2host/run.h"
#include "ps2host/settings.h"
#include "ps2plat/platform.h"

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

extern const ps2host::FunctionEntry kGuestFunctions[];
extern const std::size_t kGuestFunctionCount;

/* The game's host implementations of guest functions (its port's overrides.cpp). */
void RegisterGameOverrides();

#ifdef __vita__
extern "C" {
/* The heap newlib takes as the program starts (one memory block), and the
   main thread's stack - the host's, under the EE's fibers (whose stacks are
   on the heap). PS2HOST_VITA_HEAP_MB: the port's CMake, from the memory the
   application is given (param.sfo's ATTRIBUTE2). */
#ifndef PS2HOST_VITA_HEAP_MB
#define PS2HOST_VITA_HEAP_MB 192
#endif
unsigned int _newlib_heap_size_user = PS2HOST_VITA_HEAP_MB * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 2u * 1024u * 1024u;     // not const: a const has internal linkage, and the loader would not see it
}
#endif

namespace {

#if defined(PS2HOST_APP_NAME) && defined(__GNUC__)
/* The application's name and the environment (the Vita's env.txt: it has
   no environment of its own) before any other static object is made: some
   read their PS2HOST_* variable as they are. But after the libraries' own
   start (priority 101): the Vita's pthreads start in a constructor of that
   priority, and a function's static (AppName's) is guarded by their locks. */
struct EarlyStart {
    EarlyStart() {
        ps2plat::SetAppName(PS2HOST_APP_NAME);
        ps2plat::LoadEnvironment();
    }
};
EarlyStart g_early_start __attribute__((init_priority(110)));
#endif

/* The port is built for AVX2 (engine/host/CMakeLists.txt; the VU's lanes and
   the rest take its three-operand forms): a processor without it is told so
   rather than stopped by an illegal instruction somewhere. */
bool HasAvx2() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)) && defined(__AVX2__)
    int info[4];
    __cpuid(info, 0);
    if (info[0] < 7) return false;
    __cpuid(info, 1);
    const bool osxsave = (info[2] >> 27) & 1, avx = (info[2] >> 28) & 1;
    if (!osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false;        // the OS saves the YMM registers
    __cpuidex(info, 7, 0);
    return (info[1] >> 5) & 1;
#else
    return true;
#endif
}

std::string g_log;     // the log a run started without arguments writes

#ifdef PS2HOST_GAME_EXECUTABLE
/* Whether `path` is an image of the game's disc: the executable it boots is
   the one the port was translated from (game.json's). */
bool IsGameDisc(const std::string& path) {
    ps2host::Disc disc;
    if (!disc.Open(path)) return false;
    std::string boot = disc.BootPath();
    const std::size_t semicolon = boot.find(';');
    if (semicolon != std::string::npos) boot.resize(semicolon);
    while (!boot.empty() && (boot[0] == '\\' || boot[0] == '/')) boot.erase(0, 1);
    return ps2plat::CompareNoCase(boot.c_str(), PS2HOST_GAME_EXECUTABLE) == 0;
}
#endif

/* The options in a file (run.args): split at spaces and line ends, "double
   quotes" keeping spaces in one; none when there is no such file. */
std::vector<std::string> RunArguments(const std::string& path) {
    std::vector<std::string> args;
    std::FILE* f = ps2plat::OpenFile(path, "r");
    if (!f) return args;
    std::string text;
    char chunk[512];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) text.append(chunk, n);
    std::fclose(f);
    std::string arg;
    bool quoted = false, any = false;
    for (char ch : text) {
        if (ch == '"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')) {
            if (any) args.push_back(arg);
            arg.clear();
            any = false;
        } else {
            arg += ch;
            any = true;
        }
    }
    if (any) args.push_back(arg);
    if (!args.empty()) std::fprintf(stderr, "[port] %llu options from %s\n", (unsigned long long)args.size(), path.c_str());
    return args;
}

/* Started without arguments - from Explorer, a shortcut, the Vita's home
   screen - the port plays its game from the player's own disc image: the one
   chosen before (kept in the settings, in the data directory), or asked for
   and checked to be the game's disc; the memory card a directory beside it
   too. In a window, with sound, on the GPU, with no time limit (the default
   is for test runs). An empty list: the player gave none. */
std::vector<std::string> PlayArguments(const char* self) {
#ifdef PS2HOST_GAME_EXECUTABLE
    const std::string here = ps2plat::DataDirectory();
    ps2host::LoadSettings(ps2plat::JoinPath(here, "pc_settings.ini"));     // where --memcard puts them
    std::string disc = ps2host::Setting("disc");
    while (!IsGameDisc(disc)) {
        if (!disc.empty() && ps2plat::FileExists(disc))
            ps2plat::ShowMessage(PS2HOST_GAME_NAME,
                                 "This is not an image of the " PS2HOST_GAME_NAME " disc "
                                 "(" PS2HOST_GAME_EXECUTABLE "):\n\n" + disc,
                                 false);
        disc = ps2plat::PickDiscImage(PS2HOST_GAME_NAME, &IsGameDisc);
        if (disc.empty()) return {};
    }
    if (disc != ps2host::Setting("disc")) ps2host::SetSetting("disc", disc);
    // No terminal to read: what the run says goes to a log beside it.
    g_log = std::filesystem::path(self ? self : "port").stem().string() + ".log";
#ifdef PS2HOST_APP_NAME
    g_log = std::string(PS2HOST_APP_NAME) + ".log";
#endif
    ps2plat::RedirectOutput(ps2plat::JoinPath(here, g_log));
    std::fprintf(stderr, "[port] disc %s\n", disc.c_str());
    std::vector<std::string> args = {self ? self : "port", "--disc", disc, "--memcard", ps2plat::JoinPath(here, "memcard"),
                                     "--audio", "--timeout", "315360000"};     // ten years
#ifdef __vita__
    // The screen (window.cpp's OpenScreen), the GS drawn by the GPU (GXM;
    // the software GS where it cannot be had - no libshacccg - or the
    // "renderer" setting is "software"), the log (--frame-times every ten
    // seconds) saying how fast. run.args's --no-draw: headless. The IOP's
    // interpreter is slower here than the console's IOP: half the EE's
    // thread at most for its threads at each frame (--iop-share). The SPU2
    // mixes on a core of its own (--sound-thread): a third of the EE's core
    // where it shared it. The frame line says where the EE's time goes
    // (--sample: the published sampler, a thousand looks a second).
    for (const char* arg : {"--window", "--frame-times", "600", "--iop-share", "50", "--sound-thread", "--sample"})
        args.push_back(arg);
    // The VU microprograms the game starts, recorded beside it: the
    // player's own, for building into the next build compiled ahead of time
    // (the Vita has no recompiler at run time). Each recorded once, ever.
    args.push_back("--vu-record");
    args.push_back(ps2plat::JoinPath(here, "vu_programs.txt"));
    if (ps2host::Setting("renderer", "gpu") != "software") args.push_back("--gpu");
#else
    for (const char* arg : {"--window", "--gpu"}) args.push_back(arg);
#endif
    // run.args in the data directory, when there: more options for this run
    // (a scripted run on a system with no command line - the Vita's - from
    // port/runs; later options win). {data} is the data directory.
    for (std::string arg : RunArguments(ps2plat::JoinPath(here, "run.args"))) {
        for (std::size_t at; (at = arg.find("{data}")) != std::string::npos;) arg.replace(at, 6, here);
        args.push_back(arg);
    }
    return args;
#else
    return {self ? self : "port"};
#endif
}

}  // namespace

int main(int argc, char** argv) {
#ifdef PS2HOST_APP_NAME
    ps2plat::SetAppName(PS2HOST_APP_NAME);
#endif
    ps2plat::StartUp();
    ps2plat::AttachTerminal();
    std::vector<std::string> play;
    std::vector<char*> play_argv;
    if (argc <= 1) {
        play = PlayArguments(argc > 0 ? argv[0] : nullptr);
        if (play.empty()) return 0;
        for (std::string& arg : play) play_argv.push_back(arg.data());
        play_argv.push_back(nullptr);
        argc = int(play.size());
        argv = play_argv.data();
    }
    if (!HasAvx2()) {
        std::fprintf(stderr, "This build needs a processor with AVX2 (Intel Haswell, AMD Excavator or later).\n");
        return 70;
    }
#ifdef PS2HOST_GAME_TITLE
    ps2host::SetGameTitle(PS2HOST_GAME_TITLE);      // the window's (game.json's "title")
#endif
#ifdef PS2HOST_GAME_VERSION
    std::fprintf(stderr, "[port] %s, version %s\n", PS2HOST_GAME_TITLE, PS2HOST_GAME_VERSION);
#endif
    ps2host::RegisterFunctions(kGuestFunctions, kGuestFunctionCount);
    RegisterGameOverrides();
    const int code = ps2host::Main(argc, argv);
    if (!play.empty() && code != 0) {         // closing the window ends a run with 0
        std::fflush(nullptr);
        const std::string text = "The game stopped because of an error. What happened is in " +
                                 g_log + ", beside the game.";
        ps2plat::ShowMessage(ps2host::GameTitle().empty() ? "PS2 port" : ps2host::GameTitle(), text, true);
    }
    // What must be finished is (the report written, the settings and the
    // memory card saved as they changed, the WAV's header here); the rest -
    // the window, the GPU device, the threads - goes with the process.
    // Taken down one by one at exit, the Vulkan device's could hang
    // (vkDestroyDevice with the driver's threads and the renderer's images
    // still about), leaving a window that never closed.
    ps2host::CloseAudio();
    ps2host::ReleaseController();
    std::fflush(nullptr);
    ps2plat::QuickExit(code);
}
