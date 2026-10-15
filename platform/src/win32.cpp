/*
 * The platform layer on Windows: what the host called directly before the
 * layer existed (threads.cpp's fibers, timers and priorities, main.cpp's
 * dialogs and console, the disc's 64-bit seeks), unchanged in what it does.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <timeapi.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "ps2plat/platform.h"

#ifdef _MSC_VER
#pragma comment(lib, "synchronization.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#endif

namespace ps2plat {

// ------------------------------------------------------------ fibers

Fiber* ThreadToFiber() {
    if (IsThreadAFiber()) return static_cast<Fiber*>(GetCurrentFiber());
    return static_cast<Fiber*>(ConvertThreadToFiber(nullptr));
}

namespace {
struct Start {
    void (*entry)(void*);
    void* argument;
};
void CALLBACK FiberStart(void* parameter) {
    const Start start = *static_cast<Start*>(parameter);
    delete static_cast<Start*>(parameter);
    start.entry(start.argument);
    std::abort();                               // a fiber's entry never returns
}
}  // namespace

Fiber* NewFiber(std::size_t stack_size, void (*entry)(void*), void* argument) {
    return static_cast<Fiber*>(CreateFiber(stack_size, FiberStart, new Start{entry, argument}));
}

void SwitchTo(Fiber* to) { SwitchToFiber(to); }
void FreeFiber(Fiber* fiber) {
    if (fiber) DeleteFiber(fiber);
}

// ------------------------------------------------------------ threads

namespace {
/* Never slowed for power: a thread that must keep up (the EE, VIF1, the GS,
   the blank's timer). */
void NotThrottled() {
    THREAD_POWER_THROTTLING_STATE throttling{};
    throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;               // never throttled
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof throttling);
}
}  // namespace

void SetThreadRole(ThreadRole role) {
    switch (role) {
        case ThreadRole::kEe:
        case ThreadRole::kVif1:
        case ThreadRole::kGs:
            NotThrottled();
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            break;
        case ThreadRole::kBlankTimer:
            // Asleep almost always, it must wake on time with every core busy
            // (a blank 13 ms late was a frame shown late): the highest priority.
            NotThrottled();
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            break;
        case ThreadRole::kPresent:
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            break;
        default:
            break;
    }
}

void PrepareRealTime() {
    timeBeginPeriod(1);             // sleeps to the millisecond, not Windows' 15.6 ms
    // ...even with the window hidden behind others: Windows 11 drops a
    // process's timer resolution then (a 200 us sleep took 15 ms, a blank
    // seen 14 ms late), unless told not to; and the process not throttled.
    PROCESS_POWER_THROTTLING_STATE process{};
    process.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    process.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    process.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &process, sizeof process);
}

/* Asleep until `when`, to a tenth of a millisecond or so: a high-resolution
   waitable timer (Windows 10 1803 on), not Sleep's millisecond steps that can
   overshoot by more; sleep_until where there is none. */
void SleepUntil(std::chrono::steady_clock::time_point when) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    if (!timer) {
        std::this_thread::sleep_until(when);
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
    else std::this_thread::sleep_until(when);
}

void WaitWhile(const std::atomic<std::uint32_t>* address, std::uint32_t seen, unsigned timeout_ms) {
    WaitOnAddress(const_cast<std::atomic<std::uint32_t>*>(address), &seen, sizeof seen, timeout_ms);
}

void WakeAll(const std::atomic<std::uint32_t>* address) {
    WakeByAddressAll(const_cast<std::atomic<std::uint32_t>*>(address));
}

// ------------------------------------------------------------ sound
// The host plays through waveOut itself (audio.cpp).

bool OpenAudioOut(unsigned frames) {
    (void)frames;
    return false;
}
void AudioOutWrite(const short* grain) { (void)grain; }
void CloseAudioOut() {}

// ------------------------------------------------------------ the controls, a status screen
// The window's controller is the display's (engine/gpu); the picture is the window's.

bool ReadController(ControllerState* state) {
    (void)state;
    return false;
}
bool ShowStatus(const std::vector<std::string>& lines) {
    (void)lines;
    return false;
}

// ------------------------------------------------------------ files

ReadOnlyFile::~ReadOnlyFile() { Close(); }

bool ReadOnlyFile::Open(const std::string& path) {
    Close();
    std::FILE* f = OpenFile(path, "rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    size_ = std::uint64_t(_ftelli64(f));
    handle_ = reinterpret_cast<std::intptr_t>(f);
    return true;
}

void ReadOnlyFile::Close() {
    if (handle_ != kClosed) std::fclose(reinterpret_cast<std::FILE*>(handle_));
    handle_ = kClosed;
    size_ = 0;
}

bool ReadOnlyFile::ReadAt(std::uint64_t offset, void* out, std::size_t length) {
    std::FILE* f = reinterpret_cast<std::FILE*>(handle_);
    if (handle_ == kClosed || _fseeki64(f, static_cast<long long>(offset), SEEK_SET) != 0) return false;
    return std::fread(out, 1, length, f) == length;
}

/* The narrow path as the host has always opened it: the system's code page,
   which is what the file dialog (GetOpenFileNameA) and the settings give. */
std::FILE* OpenFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

bool FileExists(const std::string& path) {
    return !path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ------------------------------------------------------------ places

namespace {
std::string& AppName() {
    static std::string name;
    return name;
}
}  // namespace

void SetAppName(const std::string& name) { AppName() = name; }
void StartUp() {}
void LoadEnvironment() {}

std::string ExecutableDirectory() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path().string();
}

std::string DataDirectory() { return ExecutableDirectory(); }

std::string JoinPath(const std::string& directory, const std::string& name) {
    return (std::filesystem::path(directory) / name).string();
}

// ------------------------------------------------------------ the rest

bool LocalTime(std::time_t when, std::tm* out) { return localtime_s(out, &when) == 0; }
bool UtcTime(std::time_t when, std::tm* out) { return gmtime_s(out, &when) == 0; }
int CompareNoCase(const char* a, const char* b) { return _stricmp(a, b); }

/* A port built as a windowed program has no console of its own. Started from
   a terminal its output goes to that one; redirected (a file, a pipe: the
   tools' runs), where it was sent. */
void AttachTerminal() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    std::FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
}

bool RedirectOutput(const std::string& path) {
    // Opened once, shared (fopen's would lock the file to one stream), and
    // given to both: their lines in the order they were written.
    std::FILE* stream = nullptr;
    freopen_s(&stream, "NUL", "w", stdout);    // streams with descriptors, in a windowed program
    freopen_s(&stream, "NUL", "w", stderr);
    const int file = _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_TEXT, _S_IREAD | _S_IWRITE);
    if (file >= 0) {
        _dup2(file, _fileno(stdout));
        _dup2(file, _fileno(stderr));
        _close(file);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a run ended by force loses nothing
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return file >= 0;
}

void ShowMessage(const std::string& title, const std::string& text, bool error) {
    MessageBoxA(nullptr, text.c_str(), title.c_str(), MB_OK | (error ? MB_ICONERROR : MB_ICONWARNING));
}

std::string PickDiscImage(const std::string& game_name, bool (*checked)(const std::string& path)) {
    (void)checked;      // the caller checks what is chosen, and asks again
    char file[MAX_PATH] = {};
    const std::string title = "Choose your " + game_name + " disc image";
    OPENFILENAMEA dialog = {};
    dialog.lStructSize = sizeof dialog;
    dialog.lpstrFilter = "Disc images (*.iso; *.cue; *.bin)\0*.iso;*.cue;*.bin\0All files (*.*)\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = title.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&dialog) ? std::string(file) : std::string();
}

void QuickExit(int code) {
    TerminateProcess(GetCurrentProcess(), UINT(code));
    std::_Exit(code);
}

}  // namespace ps2plat
