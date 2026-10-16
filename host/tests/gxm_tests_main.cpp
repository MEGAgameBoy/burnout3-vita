/*
 * The GS renderer's tests on the Vita's GPU (ps2gxm_tests.vpk): the Vulkan
 * renderer's tests (engine/gpu/tests/renderer_tests.cpp) run on the GXM
 * renderer - each check on the screen as it comes, and in
 * ux0:data/ps2gxm_tests/results.txt, the file to send back. Needs
 * libshacccg, as the game's GPU renderer does.
 */

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include <cstdio>
#include <memory>
#include <string>

#include "ps2gpu/gs_renderer.h"
#include "ps2plat/platform.h"
#include "ps2plat/vita_console.h"

int RunRendererTests(ps2gpu::GsRenderer& renderer, bool say_passes);

extern "C" {
unsigned int _newlib_heap_size_user = 96u * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 1u * 1024u * 1024u;     // not const: the loader reads it
}

int main() {
    namespace vita = ps2plat::vita;
    ps2plat::SetAppName("ps2gxm_tests");
    ps2plat::StartUp();
    vita::ConsoleShow();
    vita::ConsolePrint("ps2-port-engine: the GS renderer's tests on the Vita's GPU (GXM)\n\n", vita::kYellow);
    const std::string results = ps2plat::JoinPath(ps2plat::DataDirectory(), "results.txt");
    ps2plat::RedirectOutput(results);
    std::string error;
    std::unique_ptr<ps2gpu::GsRenderer> renderer = ps2gpu::CreateGxmGsRenderer(&error);
    int failures = -1;
    if (!renderer) {
        std::printf("FAIL: no GXM renderer: %s\n", error.c_str());
    } else {
        failures = RunRendererTests(*renderer, true);
        std::printf("gxm renderer tests: %d failed\n", failures);
    }
    std::fflush(nullptr);
    // What was written, on the screen: the shaders' compiles, each check.
    if (std::FILE* f = std::fopen(results.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            const std::string text(line);
            const bool failed = text.rfind("FAIL", 0) == 0, verdict = text.rfind("gxm renderer tests:", 0) == 0;
            if (text.rfind("[gxm]", 0) == 0) continue;
            vita::ConsolePrint(text, failed ? vita::kRed : verdict ? (failures ? vita::kRed : vita::kGreen) : vita::kWhite);
        }
        std::fclose(f);
    }
    vita::ConsolePrint("\nAll of it is in " + results + ". Press X to exit.\n", vita::kGrey);
    vita::ConsoleWaitButtons(SCE_CTRL_CROSS);
    ps2plat::QuickExit(failures == 0 ? 0 : 1);
}
