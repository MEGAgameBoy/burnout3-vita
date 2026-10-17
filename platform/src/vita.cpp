/*
 * The platform layer on the PlayStation Vita (VitaSDK): the kernel's threads
 * pinned to cores by role, sceIo for files (64-bit offsets: a disc image is
 * over 4 GB), sceAudioOut for sound, the data directory ux0:data/<app>, and a
 * text screen of its own for messages (vita_console.h). Fibers are
 * fiber_arm.S's (fibers.cpp).
 */

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>

#include <strings.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "console_font.h"
#include "ps2plat/platform.h"
#include "ps2plat/vita_console.h"

extern "C" {
/* std::thread's stack. VitaSDK's pthreads give a thread 32 KB unless the
   program defines this (pthread-embedded's create.c): far too little for the
   host's threads - the GS model, VIF1 and VU1's compiled programs, the IOP's
   sound - which get what the EE's fibers get. */
unsigned int _pthread_stack_default_user = 1024u * 1024u;
}

namespace ps2plat {

// ------------------------------------------------------------ threads

namespace {
struct RoleSetting {
    int priority;       // 64 (highest) to 191
    int cores;          // SCE_KERNEL_CPU_MASK_USER_*
};
// The three cores an application has: the EE (the game, the IOP, the
// interrupts) on the first, VIF1 and VU1 on the second, the GS on the third.
// The blank's timer shares the EE's core above it, to take the EE's place
// the moment it wakes; sound shares the GS's. So does the SPU2's mixing (a
// third of the EE's core where it ran there), above the GS: it mixes a
// millisecond at a time and sleeps. (On VIF1's core it took a third of what
// VU1 had - the car select's 3D, VU1 at 99% there.) The sampler wakes a
// thousand times a second for a moment, off the EE's core.
RoleSetting Setting(ThreadRole role) {
    switch (role) {
        case ThreadRole::kEe: return {96, SCE_KERNEL_CPU_MASK_USER_0};
        case ThreadRole::kVif1: return {96, SCE_KERNEL_CPU_MASK_USER_1};
        case ThreadRole::kGs: return {96, SCE_KERNEL_CPU_MASK_USER_2};
        case ThreadRole::kBlankTimer: return {64, SCE_KERNEL_CPU_MASK_USER_0};
        case ThreadRole::kAudio: return {72, SCE_KERNEL_CPU_MASK_USER_2};
        case ThreadRole::kPresent: return {80, SCE_KERNEL_CPU_MASK_USER_2};
        case ThreadRole::kSound: return {90, SCE_KERNEL_CPU_MASK_USER_2};
        case ThreadRole::kSampler: return {70, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2};
        case ThreadRole::kDisc: return {120, SCE_KERNEL_CPU_MASK_USER_ALL};
        case ThreadRole::kHelper: return {128, SCE_KERNEL_CPU_MASK_USER_ALL};
    }
    return {128, SCE_KERNEL_CPU_MASK_USER_ALL};
}
}  // namespace

void SetThreadRole(ThreadRole role) {
    // The thread's floating point as the PC's: rounding to nearest,
    // denormals kept, NaNs as they come (FPSCR's RMode, FZ and DN clear) -
    // what the EE's FPU emulation and everything else assume as they start
    // (the VUs' and macro mode's rounding set it for themselves and put it
    // back). A thread need not start so: the Vita's host tests found the
    // FPU's results apart from the PC's (the thirteenth run).
    unsigned fpscr;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    fpscr &= ~((3u << 22) | (1u << 24) | (1u << 25));
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(fpscr));
    const RoleSetting s = Setting(role);
    sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, s.priority);
    sceKernelChangeThreadCpuAffinityMask(SCE_KERNEL_THREAD_ID_SELF, s.cores);
}

void PrepareRealTime() {}

void SleepUntil(std::chrono::steady_clock::time_point when) {
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(when - std::chrono::steady_clock::now());
    if (left.count() > 0) sceKernelDelayThread(SceUInt(left.count()));
}

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

namespace {
int g_audio_port = -1;
}

bool OpenAudioOut(unsigned frames) {
    if (g_audio_port >= 0) return true;
    // MAIN: 48 kHz only, the SPU2's rate. A grain is 64 to 65,472 frames, a multiple of 64.
    g_audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, int(frames), 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (g_audio_port < 0) return false;
    int volume[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(g_audio_port, SceAudioOutChannelFlag(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                         volume);
    return true;
}

void AudioOutWrite(const short* grain) {
    if (g_audio_port >= 0) sceAudioOutOutput(g_audio_port, grain);
}

void CloseAudioOut() {
    if (g_audio_port < 0) return;
    sceAudioOutOutput(g_audio_port, nullptr);       // what is queued, played out
    sceAudioOutReleasePort(g_audio_port);
    g_audio_port = -1;
}

// ------------------------------------------------------------ the controls

namespace {
/* A stick from the Vita's 0-255 (128 centred): a radial dead zone taken out
   (the Vita's sticks rest a few steps off centre) and the rest stretched over
   the whole travel, so the smallest push past it is a small push. */
void Stick(std::uint8_t x, std::uint8_t y, std::uint8_t* out_x, std::uint8_t* out_y) {
    constexpr float kDead = 0.15f;
    float fx = float(int(x) - 128) / 127.0f, fy = float(int(y) - 128) / 127.0f;
    const float length = std::sqrt(fx * fx + fy * fy);
    if (length <= kDead) {
        *out_x = *out_y = 0x80;
        return;
    }
    const float scale = std::min(1.0f, (length - kDead) / (1.0f - kDead)) / length;
    auto axis = [](float v) { return std::uint8_t(std::clamp(int(std::lround(128.0f + v * 127.0f)), 0, 255)); };
    *out_x = axis(fx * scale);
    *out_y = axis(fy * scale);
}

SceTouchPanelInfo g_rear{};
bool g_rear_ready = false;
}  // namespace

bool ReadController(ControllerState* state) {
    SceCtrlData pad{};
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return false;
    // The Vita's buttons, as the DualShock 2's (libpad's order). Its shoulder
    // buttons are L1 and R1 (the system names them as triggers).
    static const struct {
        unsigned vita;
        int input;
    } kButtons[] = {
        {SCE_CTRL_SELECT, 0},   {SCE_CTRL_START, 3},     {SCE_CTRL_UP, 4},     {SCE_CTRL_RIGHT, 5},
        {SCE_CTRL_DOWN, 6},     {SCE_CTRL_LEFT, 7},      {SCE_CTRL_LTRIGGER, 10}, {SCE_CTRL_RTRIGGER, 11},
        {SCE_CTRL_TRIANGLE, 12}, {SCE_CTRL_CIRCLE, 13},  {SCE_CTRL_CROSS, 14}, {SCE_CTRL_SQUARE, 15},
    };
    ControllerState s;
    for (const auto& b : kButtons)
        if (pad.buttons & b.vita) s.buttons |= std::uint16_t(1u << b.input);
    Stick(pad.rx, pad.ry, &s.sticks[0], &s.sticks[1]);
    Stick(pad.lx, pad.ly, &s.sticks[2], &s.sticks[3]);
    // L2 and R2, which the Vita lacks: the rear panel's upper left and upper
    // right quarters, as seen from the front (where the fingers reach without
    // resting); its lower half is left alone, where they rest.
    if (!g_rear_ready) {
        g_rear_ready = true;
        if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &g_rear) < 0) g_rear = SceTouchPanelInfo{};
    }
    SceTouchData touch{};
    if (g_rear.maxAaX > g_rear.minAaX && sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) > 0) {
        const int middle_x = (g_rear.minAaX + g_rear.maxAaX) / 2, middle_y = (g_rear.minAaY + g_rear.maxAaY) / 2;
        for (SceUInt32 i = 0; i < touch.reportNum && i < 8; ++i) {
            const SceTouchReport& r = touch.report[i];
            if (r.y >= middle_y) continue;
            if (r.x < middle_x) s.buttons |= 1u << 8, s.l2 = 0xFF;
            else s.buttons |= 1u << 9, s.r2 = 0xFF;
        }
    }
    *state = s;
    return true;
}

// ------------------------------------------------------------ a status screen

bool ShowStatus(const std::vector<std::string>& lines) {
    if (!vita::ConsoleShow()) return false;
    vita::ConsoleClear();
    for (std::size_t i = 0; i < lines.size(); ++i)
        vita::ConsolePrint(lines[i] + "\n", i == 0 ? vita::kYellow : vita::kWhite);
    return true;
}

// ------------------------------------------------------------ files

ReadOnlyFile::~ReadOnlyFile() { Close(); }

bool ReadOnlyFile::Open(const std::string& path) {
    Close();
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size < 0) {
        sceIoClose(fd);
        return false;
    }
    size_ = std::uint64_t(size);
    handle_ = fd;
    return true;
}

void ReadOnlyFile::Close() {
    if (handle_ != kClosed) sceIoClose(SceUID(handle_));
    handle_ = kClosed;
    size_ = 0;
}

bool ReadOnlyFile::ReadAt(std::uint64_t offset, void* out, std::size_t length) {
    if (handle_ == kClosed) return false;
    auto* to = static_cast<char*>(out);
    while (length) {
        const SceSize chunk = SceSize(std::min<std::size_t>(length, 1u << 20));
        const int got = sceIoPread(SceUID(handle_), to, chunk, SceOff(offset));
        if (got <= 0) return false;
        to += got;
        offset += std::uint64_t(got);
        length -= std::size_t(got);
    }
    return true;
}

std::FILE* OpenFile(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }

bool FileExists(const std::string& path) {
    SceIoStat stat;
    return !path.empty() && sceIoGetstat(path.c_str(), &stat) >= 0;
}

// ------------------------------------------------------------ places

namespace {
std::string& AppName() {
    static std::string name;
    return name;
}
}  // namespace

void SetAppName(const std::string& name) { AppName() = name; }

std::string ExecutableDirectory() { return "app0:"; }

std::string DataDirectory() { return "ux0:data/" + (AppName().empty() ? std::string("ps2port") : AppName()); }

std::string JoinPath(const std::string& directory, const std::string& name) {
    if (directory.empty()) return name;
    const char last = directory.back();
    return last == '/' || last == ':' ? directory + name : directory + "/" + name;
}

void LoadEnvironment() {
    std::FILE* f = std::fopen(JoinPath(DataDirectory(), "env.txt").c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        if (text.empty() || text[0] == '#') continue;
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos || equals == 0) continue;
        setenv(text.substr(0, equals).c_str(), text.substr(equals + 1).c_str(), 1);
    }
    std::fclose(f);
}

void StartUp() {
    // The fastest the system gives an application without a plugin: the ARM
    // cores at 444 MHz, the bus and GPU at 222, the GPU's crossbar at 166.
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(DataDirectory().c_str(), 0777);
    sceSysmoduleLoadModule(SCE_SYSMODULE_FIBER);           // the EE's fibers (fibers.cpp), loaded before they are needed
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);   // L2, R2 (ReadController)
}

// ------------------------------------------------------------ the rest

bool LocalTime(std::time_t when, std::tm* out) { return localtime_r(&when, out) != nullptr; }
bool UtcTime(std::time_t when, std::tm* out) { return gmtime_r(&when, out) != nullptr; }
int CompareNoCase(const char* a, const char* b) { return strcasecmp(a, b); }

void AttachTerminal() {}

bool RedirectOutput(const std::string& path) {
    // Both streams appending to one file, unbuffered: their lines in the
    // order they were written, and nothing lost if the program is closed.
    if (std::FILE* f = std::fopen(path.c_str(), "w")) std::fclose(f);
    const bool out = std::freopen(path.c_str(), "a", stdout) != nullptr;
    const bool err = std::freopen(path.c_str(), "a", stderr) != nullptr;
    if (out) std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (err) std::setvbuf(stderr, nullptr, _IONBF, 0);
    return out && err;
}

void ShowMessage(const std::string& title, const std::string& text, bool error) {
    std::fprintf(stderr, "%s%s: %s\n", error ? "error: " : "", title.c_str(), text.c_str());
    if (!vita::ConsoleShow()) return;
    vita::ConsoleClear();
    vita::ConsolePrint(title + "\n\n", error ? vita::kRed : vita::kYellow);
    vita::ConsolePrint(text + "\n\n");
    vita::ConsolePrint("Press X to continue.\n", vita::kGrey);
    vita::ConsoleWaitButtons(SCE_CTRL_CROSS);
}

namespace {
bool HasImageExtension(const char* name) {
    const std::size_t n = std::strlen(name);
    if (n < 5) return false;
    const char* ext = name + n - 4;
    return strcasecmp(ext, ".iso") == 0 || strcasecmp(ext, ".cue") == 0 || strcasecmp(ext, ".bin") == 0;
}
}  // namespace

std::string PickDiscImage(const std::string& game_name, bool (*checked)(const std::string& path)) {
    // No file dialog: the images in the data directory, and in the same
    // place on the other storage devices (a memory card, a USB or SD card
    // adapter), the first that is the game's disc.
    const std::string sub = "data/" + (AppName().empty() ? std::string("ps2port") : AppName());
    std::vector<std::string> directories = {DataDirectory()};
    for (const char* device : {"uma0:", "imc0:", "grw0:", "xmc0:", "ur0:"}) directories.push_back(device + sub);
    std::vector<std::string> seen;
    for (const std::string& directory : directories) {
        const SceUID d = sceIoDopen(directory.c_str());
        if (d < 0) continue;
        SceIoDirent entry;
        std::vector<std::string> images;
        while (sceIoDread(d, &entry) > 0)
            if (HasImageExtension(entry.d_name)) images.push_back(JoinPath(directory, entry.d_name));
        sceIoDclose(d);
        std::sort(images.begin(), images.end());
        for (const std::string& image : images) {
            seen.push_back(image);
            if (checked && checked(image)) return image;
        }
    }
    std::string text = "No image of your " + game_name + " disc was found.\n\n"
                       "Put the image you made of your own disc (an .iso, or a .cue with its .bin) in\n\n    " +
                       DataDirectory() + "/\n\n(or the same folder on uma0:, imc0:, grw0: or xmc0:) and start again.";
    if (!seen.empty()) {
        text += "\n\nThese are not images of the game's disc:";
        for (const std::string& image : seen) text += "\n    " + image;
    }
    ShowMessage(game_name, text, true);
    return "";
}

void QuickExit(int code) {
    sceKernelExitProcess(code);
    std::_Exit(code);
}

// ------------------------------------------------------------ the text screen

namespace vita {

namespace {
constexpr int kWidth = 960, kHeight = 544, kPitch = 960;
constexpr int kColumns = kWidth / kConsoleGlyphWidth, kLines = kHeight / kConsoleGlyphHeight;
constexpr int kMarginX = (kWidth - kColumns * kConsoleGlyphWidth) / 2;
constexpr int kMarginY = (kHeight - kLines * kConsoleGlyphHeight) / 2;
constexpr std::uint32_t kBackground = 0xFF201410u;

struct Screen {
    SceUID block = -1;
    std::uint32_t* pixels = nullptr;
    int line = 0, column = 0;
};
Screen& S() {
    static Screen screen;
    return screen;
}

// A character at x0, y0 of a 960-pixel-wide framebuffer, on `background`.
void DrawGlyph(std::uint32_t* pixels, int x0, int y0, char ch, std::uint32_t colour, std::uint32_t background) {
    const int index = (ch < 32 || ch > 126) ? '?' - 32 : ch - 32;
    for (int y = 0; y < kConsoleGlyphHeight; ++y) {
        const std::uint16_t row = kConsoleFont[index][y];
        std::uint32_t* p = pixels + std::size_t(y0 + y) * kPitch + x0;
        for (int x = 0; x < kConsoleGlyphWidth; ++x) p[x] = (row & (0x8000u >> x)) ? colour : background;
    }
}

void Glyph(int column, int line, char ch, std::uint32_t colour) {
    DrawGlyph(S().pixels, kMarginX + column * kConsoleGlyphWidth, kMarginY + line * kConsoleGlyphHeight, ch, colour,
              kBackground);
}

void NewLine() {
    Screen& s = S();
    s.column = 0;
    if (++s.line < kLines) return;
    // Scroll up a line.
    const std::size_t row = std::size_t(kConsoleGlyphHeight) * kPitch;
    std::uint32_t* top = s.pixels + std::size_t(kMarginY) * kPitch;
    std::memmove(top, top + row, (std::size_t(kLines - 1) * row) * 4);
    std::uint32_t* last = top + std::size_t(kLines - 1) * row;
    std::fill(last, last + row, kBackground);
    s.line = kLines - 1;
}
}  // namespace

bool ConsoleShow() {
    Screen& s = S();
    if (!s.pixels) {
        const SceSize size = (kPitch * kHeight * 4 + 0x3FFFF) & ~0x3FFFFu;     // CDRAM: in 256 KB
        s.block = sceKernelAllocMemBlock("ps2plat console", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, nullptr);
        if (s.block < 0) return false;
        void* base = nullptr;
        if (sceKernelGetMemBlockBase(s.block, &base) < 0) return false;
        s.pixels = static_cast<std::uint32_t*>(base);
        std::fill(s.pixels, s.pixels + std::size_t(kPitch) * kHeight, kBackground);
    }
    SceDisplayFrameBuf frame{};
    frame.size = sizeof frame;
    frame.base = s.pixels;
    frame.pitch = kPitch;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = kWidth;
    frame.height = kHeight;
    return sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) >= 0;
}

void ConsoleClear() {
    Screen& s = S();
    if (!s.pixels) return;
    std::fill(s.pixels, s.pixels + std::size_t(kPitch) * kHeight, kBackground);
    s.line = s.column = 0;
}

void ConsolePrint(const std::string& text, std::uint32_t colour) {
    Screen& s = S();
    if (!s.pixels) return;
    for (char ch : text) {
        if (ch == '\n') {
            NewLine();
            continue;
        }
        if (s.column >= kColumns) NewLine();
        Glyph(s.column++, s.line, ch, colour);
    }
}

std::uint32_t ConsoleWaitButtons(std::uint32_t buttons) {
    SceCtrlData pad{};
    // Let go of whatever is held, then wait for a press.
    do {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceKernelDelayThread(16000);
    } while (pad.buttons & buttons);
    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & buttons) return pad.buttons & buttons;
        sceKernelDelayThread(16000);
    }
}

}  // namespace vita

// ------------------------------------------------------------ the picture

namespace {

/* Two framebuffers in CDRAM, drawn in turn: the display takes the one just
   drawn at the next refresh while the next picture goes into the other. */
struct PictureScreen {
    SceUID block = -1;
    std::uint32_t* buffers[2] = {};
    int next = 0;
    // The placement each buffer's borders were last made black for (4:3 is
    // pillarboxed): once, not every picture.
    int left[2] = {-1, -1}, top[2] = {-1, -1}, width[2] = {}, height[2] = {};
    std::vector<std::uint16_t> columns;     // the source column of each column shown
    unsigned source_width = 0;
};

PictureScreen& Picture() {
    static PictureScreen picture;
    return picture;
}

constexpr int kScreenWidth = 960, kScreenHeight = 544, kScreenPitch = 960;
constexpr int kCaptionColumns = 64;         // the caption's band, whatever its text: the last one covered
constexpr std::uint32_t kCaptionBackground = 0xFF000000u;

}  // namespace

bool ShowPicture(const std::uint32_t* pixels, unsigned width, unsigned height, float aspect,
                 const std::string& caption) {
    if (!pixels || width == 0 || height == 0 || width > 0xFFFF) return false;
    PictureScreen& p = Picture();
    if (p.block < 0) {
        const SceSize one = SceSize(kScreenPitch) * kScreenHeight * 4;
        const SceSize size = (2 * one + 0x3FFFF) & ~0x3FFFFu;       // CDRAM: in 256 KB
        const SceUID block = sceKernelAllocMemBlock("ps2plat picture", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size,
                                                    nullptr);
        if (block < 0) return false;
        void* base = nullptr;
        if (sceKernelGetMemBlockBase(block, &base) < 0) return false;
        p.block = block;
        p.buffers[0] = static_cast<std::uint32_t*>(base);
        p.buffers[1] = p.buffers[0] + std::size_t(kScreenPitch) * kScreenHeight;
    }
    // The screen's height at `aspect`, or its width where that is wider.
    int w = kScreenWidth, h = kScreenHeight;
    if (aspect > 0.0f) {
        w = int(std::lround(kScreenHeight * aspect));
        if (w > kScreenWidth) {
            w = kScreenWidth;
            h = int(std::lround(kScreenWidth / aspect));
        }
    }
    const int left = (kScreenWidth - w) / 2, top = (kScreenHeight - h) / 2;
    const int b = p.next;
    std::uint32_t* out = p.buffers[b];
    if (p.left[b] != left || p.top[b] != top || p.width[b] != w || p.height[b] != h) {
        std::fill(out, out + std::size_t(kScreenPitch) * kScreenHeight, 0xFF000000u);
        p.left[b] = left, p.top[b] = top, p.width[b] = w, p.height[b] = h;
    }
    if (p.source_width != width || p.columns.size() != std::size_t(w)) {
        p.columns.resize(std::size_t(w));
        for (int x = 0; x < w; ++x) p.columns[std::size_t(x)] = std::uint16_t((2u * x + 1) * width / (2u * w));
        p.source_width = width;
    }
    // Nearest pixel (the column and row whose centre is nearest), BGRA to
    // the display's RGBA.
    const std::uint16_t* columns = p.columns.data();
    for (int y = 0; y < h; ++y) {
        const unsigned source_y = (2u * y + 1) * height / (2u * h);
        const std::uint32_t* in = pixels + std::size_t(source_y) * width;
        std::uint32_t* row = out + std::size_t(top + y) * kScreenPitch + left;
        for (int x = 0; x < w; ++x) {
            const std::uint32_t c = in[columns[x]];
            row[x] = 0xFF000000u | (c & 0x0000FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
        }
    }
    if (!caption.empty()) {
        for (int i = 0; i < kCaptionColumns; ++i)
            vita::DrawGlyph(out, 4 + i * kConsoleGlyphWidth, 2,
                            std::size_t(i) < caption.size() ? caption[std::size_t(i)] : ' ', vita::kYellow,
                            kCaptionBackground);
    }
    SceDisplayFrameBuf frame{};
    frame.size = sizeof frame;
    frame.base = out;
    frame.pitch = kScreenPitch;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = kScreenWidth;
    frame.height = kScreenHeight;
    if (sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) return false;
    p.next = b ^ 1;
    return true;
}

std::vector<std::uint32_t> TextPixels(const std::string& text, std::uint32_t colour, std::uint32_t background,
                                      unsigned* width, unsigned* height) {
    *width = unsigned(text.size()) * kConsoleGlyphWidth;
    *height = kConsoleGlyphHeight;
    std::vector<std::uint32_t> pixels(std::size_t(*width) * *height, background);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        const int index = (ch < 32 || ch > 126) ? '?' - 32 : ch - 32;
        for (int y = 0; y < kConsoleGlyphHeight; ++y) {
            const std::uint16_t row = kConsoleFont[index][y];
            std::uint32_t* out = pixels.data() + std::size_t(y) * *width + i * kConsoleGlyphWidth;
            for (int x = 0; x < kConsoleGlyphWidth; ++x)
                if (row & (0x8000u >> x)) out[x] = colour;
        }
    }
    return pixels;
}

}  // namespace ps2plat
