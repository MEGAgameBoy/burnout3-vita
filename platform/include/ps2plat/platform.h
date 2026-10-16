/*
 * What the host needs from the system it runs on, in one place: fibers for
 * the EE's threads, the host's own threads' priorities (and on the Vita their
 * cores), precise sleeps, files read at an offset, paths, the terminal and
 * messages. One implementation a system - win32.cpp (the PC port as it was
 * built before this layer: the same calls), posix.cpp (Linux, for the tests
 * and the ARM checks under qemu), vita.cpp - and nothing else in the host
 * includes a system's headers for these.
 */

#ifndef PS2PLAT_PLATFORM_H
#define PS2PLAT_PLATFORM_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace ps2plat {

// ------------------------------------------------------------ fibers
/* A fiber: a stack and the registers to resume it with. The thread that
   calls ThreadToFiber becomes one (its own stack); NewFiber makes others,
   which start in `entry(argument)` on their first SwitchTo and must never
   return from it (they switch away for good instead). Switching is
   cooperative and within one thread. */
struct Fiber;
Fiber* ThreadToFiber();
Fiber* NewFiber(std::size_t stack_size, void (*entry)(void*), void* argument);
void SwitchTo(Fiber* to);
void FreeFiber(Fiber* fiber);

// ------------------------------------------------------------ threads
/* What a host thread is for: its priority, and on the Vita the core it is
   kept on (the EE's on one, VIF1/VU1's on another, the GS's on the third). */
enum class ThreadRole {
    kEe,            // the EE's thread: the game, the IOP, the interrupts
    kVif1,          // VIF1 and VU1's worker (vif1_worker.cpp)
    kGs,            // the GS thread (gs_thread.cpp)
    kBlankTimer,    // the vertical blank's timer: asleep, and on time when it wakes
    kAudio,         // the sound device's feeder
    kDisc,          // disc reads ahead
    kPresent,       // a display's present thread
    kHelper,        // anything else (a texture pack's reader, the software GS's bands)
};
/* The calling thread's role: called first thing in the thread. */
void SetThreadRole(ThreadRole role);
/* Real time begins: the system's timers to the millisecond, the process
   not slowed for power (Windows); nothing where that is the default. */
void PrepareRealTime();
/* Asleep until `when`, as precisely as the system allows. */
void SleepUntil(std::chrono::steady_clock::time_point when);
/* A spin's pause. */
inline void CpuRelax() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__arm__) || defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}
/* Asleep while `*address` holds `seen`, up to `timeout_ms`; woken by
   WakeAll(address). (Windows' WaitOnAddress, elsewhere a condition.) */
void WaitWhile(const std::atomic<std::uint32_t>* address, std::uint32_t seen, unsigned timeout_ms);
void WakeAll(const std::atomic<std::uint32_t>* address);

// ------------------------------------------------------------ sound
/* The system's sound output, 48 kHz stereo, 16-bit, a grain of `frames` at a
   time (Windows has its own device in the host, audio.cpp's waveOut): false
   where there is none. AudioOutWrite blocks until the device takes the grain
   - the feeder's pace. */
bool OpenAudioOut(unsigned frames);
void AudioOutWrite(const short* grain);
void CloseAudioOut();

// ------------------------------------------------------------ the controls
/* The system's own controls as a DualShock 2 - the Vita's buttons, sticks and
   rear touch panel: false where the system has none of its own (on a PC the
   window's controller is the display's, engine/gpu). `buttons` bit n is input
   n in libpad's order (Select, L3, R3, Start, Up, Right, Down, Left, L2, R2,
   L1, R1, Triangle, Circle, Cross, Square); the sticks right X, Y, left X, Y
   (0x80 centred, a dead zone taken out); l2, r2 the triggers' travel. */
struct ControllerState {
    std::uint16_t buttons = 0;
    std::uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};
    std::uint8_t l2 = 0, r2 = 0;
};
bool ReadController(ControllerState* state);

// ------------------------------------------------------------ a status screen
/* Lines of text on the screen where there is no picture yet (the Vita until
   it has a renderer): what the run is doing. False where there is no such
   screen. Cheap enough for once a second. */
bool ShowStatus(const std::vector<std::string>& lines);

// ------------------------------------------------------------ the picture
/* The game's picture where the screen is the system's own and no display of
   ps2gpu's draws it (the Vita, until its GXM renderer): 32-bit pixels, BGRA
   in memory (B first), rows top-down, scaled to the screen at `aspect`
   (width over height; 0 fills the screen), shown at the next refresh.
   `caption`, when not empty, is a line of text over its top left (how fast
   the run goes). False where there is no such screen: a window shows the
   picture there. */
bool ShowPicture(const std::uint32_t* pixels, unsigned width, unsigned height, float aspect,
                 const std::string& caption);

// ------------------------------------------------------------ files
/* A file read at offsets, past 4 GB too (a DVD image is up to 8.5). */
class ReadOnlyFile {
public:
    ReadOnlyFile() = default;
    ~ReadOnlyFile();
    ReadOnlyFile(const ReadOnlyFile&) = delete;
    ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;
    bool Open(const std::string& path);
    void Close();
    bool IsOpen() const { return handle_ != kClosed; }
    std::uint64_t Size() const { return size_; }
    bool ReadAt(std::uint64_t offset, void* out, std::size_t length);

private:
    static constexpr std::intptr_t kClosed = -1;
    std::intptr_t handle_ = kClosed;
    std::uint64_t size_ = 0;
};
/* fopen, for the host's paths (on Windows narrow, in the system's code page,
   as the file dialog gives them). */
std::FILE* OpenFile(const std::string& path, const char* mode);
bool FileExists(const std::string& path);

// ------------------------------------------------------------ places
/* The application's name, for its data directory (the Vita's
   ux0:data/<name>): the port's main sets it before anything else. */
void SetAppName(const std::string& name);
/* What the system needs before anything runs, once the name is set: on the
   Vita the processor's clocks, the data directory made, and the environment
   (below). Nothing elsewhere. */
void StartUp();
/* The Vita has no environment variables, and the host reads many
   (PS2HOST_*): they are lines KEY=VALUE in <data directory>/env.txt there,
   set as the program starts - before the static objects that read them, as
   main.cpp calls this from an early initializer. Nothing elsewhere. */
void LoadEnvironment();
/* Where the executable is (the directory, no trailing separator). */
std::string ExecutableDirectory();
/* Where the player's things go - settings, the memory card, the log:
   beside the executable on a PC, ux0:data/<app name> on the Vita. */
std::string DataDirectory();
/* `directory` and `name` joined with the system's separator. */
std::string JoinPath(const std::string& directory, const std::string& name);

// ------------------------------------------------------------ the rest
bool LocalTime(std::time_t when, std::tm* out);
bool UtcTime(std::time_t when, std::tm* out);
int CompareNoCase(const char* a, const char* b);
/* A program started from a terminal writes to it (a Windows program built
   windowed has none otherwise). */
void AttachTerminal();
/* stdout and stderr into `path`, unbuffered: a run with no terminal. */
bool RedirectOutput(const std::string& path);
/* A message the player sees: a box on a PC, the screen on the Vita. */
void ShowMessage(const std::string& title, const std::string& text, bool error);
/* The player's disc image, chosen (a file dialog on a PC; on the Vita the
   first image found in the data directory and the other places it looks);
   "" when there is none. `checked` says whether a path is the game's disc. */
std::string PickDiscImage(const std::string& game_name, bool (*checked)(const std::string& path));
/* The process ends now, with `code`: what must be finished is (the caller
   flushed it), the rest goes with the process. */
[[noreturn]] void QuickExit(int code);

}  // namespace ps2plat

#endif  /* PS2PLAT_PLATFORM_H */
