/*
 * The vertical blank timer above the EE on one core, as on the Vita - on Linux.
 *
 * On the Vita the timer's thread shares the EE's core at a higher priority, so
 * a pass of its loop that does not sleep is time the EE does not get; a loop
 * that never sleeps is an EE that never runs again. The ninth run on the
 * hardware was that: an alarm due (SetAlarm, Burnout 3's race loading asleep
 * three horizontal blanks at a time) was the timer's next event, at once, on
 * every pass, until the EE took it - which it never could.
 *
 * Here the process is held to one core and its threads given SCHED_FIFO
 * priorities, the EE's below the others' (the Vita's arrangement), the timer
 * built as the Vita's (PS2HOST_TIMER_NO_SPIN). A guest sleeps on short alarms
 * a few thousand times; a watchdog above everything fails the test if they
 * have not ended in ten seconds. SCHED_FIFO needs root (or CAP_SYS_NICE):
 * without it the test says so and passes.
 *
 * Build: the ps2host_timer_priority_test target (Linux). Run it alone.
 */

#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "ps2host/host.h"
#include "ps2host/run.h"

using namespace ps2host;

namespace {

constexpr u32 kEntry = 0x00100000u, kCallback = 0x00100100u;
constexpr int kSleeps = 3000;
std::atomic<pid_t> g_watchdog{0};

// Every thread of the process at FIFO priority 20, but this one (the EE's) at
// 10 and the watchdog (30).
bool Prioritize() {
    const pid_t ee = pid_t(syscall(SYS_gettid));
    bool all = true;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            const int tid = std::atoi(e->d_name);
            if (tid <= 0 || tid == g_watchdog.load()) continue;
            sched_param p{};
            p.sched_priority = tid == ee ? 10 : 20;
            all &= sched_setscheduler(tid, SCHED_FIFO, &p) == 0;
        }
        closedir(d);
    }
    return all;
}

void Callback(lifted::Context& c) {
    c.r[4] = c.r[6];
    c.r[3] = u64(s64(-0x34));          // iWakeupThread(common)
    lifted::Syscall(c);
}

void Entry(lifted::Context& c) {
    if (!Prioritize()) {
        std::printf("timer priority test: skipped (SCHED_FIFO is not allowed here)\n");
        std::_Exit(0);
    }
    c.r[3] = 0x2F;                     // GetThreadId
    lifted::Syscall(c);
    const u64 thread = c.r[2];
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kSleeps; ++i) {
        c.r[4] = 3;                    // SetAlarm(3 horizontal blanks, Callback, thread)
        c.r[5] = kCallback;
        c.r[6] = thread;
        c.r[3] = 0xFC;
        lifted::Syscall(c);
        c.r[3] = 0x32;                 // SleepThread
        lifted::Syscall(c);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("timer priority test: passed - %d alarm sleeps in %.2f s (%.0f us each)\n", kSleeps, seconds,
                seconds * 1e6 / kSleeps);
    std::fflush(stdout);
    std::_Exit(0);
}

}  // namespace

int main() {
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(0, &one);
    sched_setaffinity(0, sizeof one, &one);
    setenv("PS2HOST_TIMER_NO_SPIN", "1", 1);
    // The watchdog: above everything, asleep until the guest is late.
    std::thread([] {
        sched_param p{};
        p.sched_priority = 30;
        sched_setscheduler(pid_t(syscall(SYS_gettid)), SCHED_FIFO, &p);
        g_watchdog = pid_t(syscall(SYS_gettid));
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::printf("timer priority test: FAILED - the EE did not get through its alarm sleeps in 10 s "
                    "(the timer's thread spun above it)\n");
        std::fflush(stdout);
        std::_Exit(1);
    }).detach();
    while (!g_watchdog.load()) std::this_thread::yield();
    Override(kEntry, Entry, "the test's guest");
    Override(kCallback, Callback, "its alarm's callback");
    lifted::Context c{};
    c.r[29] = 0x01F00000;
    try {
        RunThreads(c, kEntry, INT64_MAX);
    } catch (const GuestExit&) {
    }
    std::printf("timer priority test: FAILED - the guest ended without its sleeps\n");
    return 1;
}
