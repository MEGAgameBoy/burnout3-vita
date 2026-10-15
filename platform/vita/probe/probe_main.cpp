/*
 * The Vita's code-size probe (make_probe.py): an executable with tens of MB
 * of code, run - whether the system loads it, and whether the calls the
 * linker carried across it with veneers arrive. On the screen and in
 * ux0:data/<name>/result.txt.
 */

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "probe.h"
#include "ps2plat/platform.h"
#include "ps2plat/vita_console.h"

extern "C" {
unsigned int _newlib_heap_size_user = 16u * 1024u * 1024u;
}

int main() {
    namespace vita = ps2plat::vita;
    ps2plat::SetAppName(PROBE_NAME);
    ps2plat::StartUp();
    vita::ConsoleShow();
    char line[256];
    std::snprintf(line, sizeof line, "Code-size probe: %u MB of code in %u functions\n", kProbeMegabytes,
                  kProbeFunctions);
    vita::ConsolePrint(line, vita::kYellow);
    std::snprintf(line, sizeof line, "first function at %p, last at %p\n", reinterpret_cast<const void*>(probe_table[0]),
                  reinterpret_cast<const void*>(probe_table[kProbeFunctions - 1]));
    vita::ConsolePrint(line, vita::kGrey);
    const auto began = std::chrono::steady_clock::now();
    std::uint32_t x = 0;
    for (unsigned i = 0; i < kProbeFunctions; ++i) x = probe_table[i](x);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    const bool ok = x == kProbeExpected;
    std::snprintf(line, sizeof line, "%s: %08X (expected %08X), %.0f ms\n", ok ? "LOADED AND RAN" : "WRONG RESULT",
                  unsigned(x), unsigned(kProbeExpected), ms);
    vita::ConsolePrint(line, ok ? vita::kGreen : vita::kRed);
    if (std::FILE* f = std::fopen(ps2plat::JoinPath(ps2plat::DataDirectory(), "result.txt").c_str(), "w")) {
        std::fprintf(f, "%u MB: %s", kProbeMegabytes, line);
        std::fclose(f);
    }
    vita::ConsolePrint("\nPress X to exit.\n", vita::kGrey);
    vita::ConsoleWaitButtons(SCE_CTRL_CROSS);
    ps2plat::QuickExit(ok ? 0 : 1);
}
