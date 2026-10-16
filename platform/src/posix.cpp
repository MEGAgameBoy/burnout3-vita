/*
 * The platform layer on Linux (and other POSIX systems): what the host's tests
 * and the ARM checks under qemu run on. There is no window or sound device
 * here; a run is headless, its pictures written as files (--frames).
 */

#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "fiber_switch.h"
#include "ps2plat/platform.h"

namespace ps2plat {

// ------------------------------------------------------------ threads

void SetThreadRole(ThreadRole role) { (void)role; }
void PrepareRealTime() {}

void SleepUntil(std::chrono::steady_clock::time_point when) { std::this_thread::sleep_until(when); }

namespace {
std::mutex g_wait_mutex;
std::condition_variable g_wait;
}  // namespace

void WaitWhile(const std::atomic<std::uint32_t>* address, std::uint32_t seen, unsigned timeout_ms) {
    std::unique_lock<std::mutex> hold(g_wait_mutex);
    g_wait.wait_for(hold, std::chrono::milliseconds(timeout_ms),
                    [&] { return address->load(std::memory_order_acquire) != seen; });
}

void WakeAll(const std::atomic<std::uint32_t>* address) {
    (void)address;
    { std::lock_guard<std::mutex> hold(g_wait_mutex); }
    g_wait.notify_all();
}

// ------------------------------------------------------------ sound

bool OpenAudioOut(unsigned frames) {
    (void)frames;
    return false;       // headless: --audio-wav records what would be heard
}
void AudioOutWrite(const short* grain) { (void)grain; }
void CloseAudioOut() {}

// ------------------------------------------------------------ the controls, a status screen
// Headless: none (scripted presses drive a run).

bool ReadController(ControllerState* state) {
    (void)state;
    return false;
}
bool ShowStatus(const std::vector<std::string>& lines) {
    (void)lines;
    return false;
}
bool ShowPicture(const std::uint32_t* pixels, unsigned width, unsigned height, float aspect,
                 const std::string& caption) {
    (void)pixels, (void)width, (void)height, (void)aspect, (void)caption;
    return false;
}
std::vector<std::uint32_t> TextPixels(const std::string& text, std::uint32_t colour, std::uint32_t background,
                                      unsigned* width, unsigned* height) {
    (void)text, (void)colour, (void)background;
    *width = *height = 0;
    return {};
}

// ------------------------------------------------------------ files

ReadOnlyFile::~ReadOnlyFile() { Close(); }

bool ReadOnlyFile::Open(const std::string& path) {
    Close();
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return false;
    }
    size_ = std::uint64_t(st.st_size);
    handle_ = fd;
    return true;
}

void ReadOnlyFile::Close() {
    if (handle_ != kClosed) ::close(int(handle_));
    handle_ = kClosed;
    size_ = 0;
}

bool ReadOnlyFile::ReadAt(std::uint64_t offset, void* out, std::size_t length) {
    if (handle_ == kClosed) return false;
    auto* to = static_cast<char*>(out);
    while (length) {
        const ssize_t got = ::pread(int(handle_), to, length, off_t(offset));
        if (got <= 0) return false;
        to += got;
        offset += std::uint64_t(got);
        length -= std::size_t(got);
    }
    return true;
}

std::FILE* OpenFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

bool FileExists(const std::string& path) {
    struct stat st {};
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
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
    char path[PATH_MAX] = {};
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return ".";
    std::string exe(path, std::size_t(n));
    const std::size_t slash = exe.rfind('/');
    return slash == std::string::npos ? "." : exe.substr(0, slash ? slash : 1);
}

std::string DataDirectory() { return ExecutableDirectory(); }

std::string JoinPath(const std::string& directory, const std::string& name) {
    if (directory.empty()) return name;
    return directory.back() == '/' ? directory + name : directory + "/" + name;
}

// ------------------------------------------------------------ the rest

bool LocalTime(std::time_t when, std::tm* out) { return ::localtime_r(&when, out) != nullptr; }
bool UtcTime(std::time_t when, std::tm* out) { return ::gmtime_r(&when, out) != nullptr; }
int CompareNoCase(const char* a, const char* b) { return ::strcasecmp(a, b); }

void AttachTerminal() {}

bool RedirectOutput(const std::string& path) {
    if (!std::freopen(path.c_str(), "w", stdout)) return false;
    ::dup2(::fileno(stdout), ::fileno(stderr));
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

void ShowMessage(const std::string& title, const std::string& text, bool error) {
    std::fprintf(stderr, "%s%s: %s\n", error ? "error: " : "", title.c_str(), text.c_str());
}

std::string PickDiscImage(const std::string& game_name, bool (*checked)(const std::string& path)) {
    (void)game_name, (void)checked;
    return "";          // no dialog: the disc is given with --disc
}

void QuickExit(int code) { std::_Exit(code); }

}  // namespace ps2plat
