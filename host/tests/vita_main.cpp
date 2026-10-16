/*
 * The host's tests on the Vita (ps2host_tests.vpk): the same tests as on a
 * PC, then the benchmarks, the results on the screen as they come and in
 * ux0:data/ps2host_tests/results.txt - the file to send back.
 */

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "ps2plat/platform.h"
#include "ps2plat/vita_console.h"

int RunHostTests(void (*observe)(const char* name, double ms, int failures));

extern "C" {
unsigned int _newlib_heap_size_user = 160u * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 2u * 1024u * 1024u;     // not const: a const has internal linkage, and the loader would not see it
}

namespace {

namespace vita = ps2plat::vita;
int g_failures_seen = 0;

void Observe(const char* name, double ms, int failures) {
    char line[128];
    std::snprintf(line, sizeof line, "  %-38s %9.1f ms", name, ms);
    vita::ConsolePrint(line, failures > g_failures_seen ? vita::kRed : vita::kWhite);
    vita::ConsolePrint(failures > g_failures_seen ? "  FAILED\n" : "\n", vita::kRed);
    g_failures_seen = failures;
}

}  // namespace

int main() {
    ps2plat::SetAppName("ps2host_tests");
    ps2plat::StartUp();
    const std::string here = ps2plat::DataDirectory();
    // std::filesystem's temporary directory (the memory card's and texture
    // pack's tests write there): the data directory's own.
    const std::string temp = ps2plat::JoinPath(here, "tmp");
    sceIoMkdir(temp.c_str(), 0777);
    setenv("TMPDIR", temp.c_str(), 1);
    setenv("PS2HOST_TEST_TIMES", "1", 1);
    setenv("PS2HOST_TEST_BENCH", "1", 1);
    vita::ConsoleShow();
    vita::ConsolePrint("ps2-port-engine: the host's tests on the Vita\n", vita::kYellow);
    char clocks[128];
    std::snprintf(clocks, sizeof clocks, "ARM %d MHz, bus %d MHz, GPU %d MHz\n\n", scePowerGetArmClockFrequency(),
                  scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
    vita::ConsolePrint(clocks, vita::kGrey);
    const std::string results = ps2plat::JoinPath(here, "results.txt");
    ps2plat::RedirectOutput(results);
    std::printf("%s", clocks);
    const int code = RunHostTests(Observe);
    std::fflush(nullptr);
    // The benchmarks and the verdict, from the file, on the screen.
    if (std::FILE* f = std::fopen(results.c_str(), "r")) {
        char line[256];
        bool benchmarks = false;
        auto starts = [](const std::string& text, const char* with) { return text.rfind(with, 0) == 0; };
        while (std::fgets(line, sizeof line, f)) {
            const std::string text(line);
            const bool verdict = starts(text, "host tests:"), failed = starts(text, "FAIL");
            if (starts(text, "benchmarks:")) benchmarks = true;
            if (verdict) benchmarks = false;
            if (benchmarks || verdict || failed || starts(text, "arithmetic:") || starts(text, "fibers:"))
                vita::ConsolePrint(text, failed || (verdict && code) ? vita::kRed
                                         : verdict                  ? vita::kGreen
                                                                    : vita::kWhite);
        }
        std::fclose(f);
    }
    vita::ConsolePrint("\nAll of it is in " + results + ". Press X to exit.\n", vita::kGrey);
    vita::ConsoleWaitButtons(SCE_CTRL_CROSS);
    ps2plat::QuickExit(code);
}
