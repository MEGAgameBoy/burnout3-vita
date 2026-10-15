/*
 * The Vita's text screen: lines of text on a framebuffer of its own, for what
 * must be read before (or without) the game's picture - a message, the host
 * tests' results. The X11 "fixed" 10x20 font (public domain): 96 columns, 27
 * lines on the 960 x 544 screen. Only on the Vita (vita.cpp).
 */

#ifndef PS2PLAT_VITA_CONSOLE_H
#define PS2PLAT_VITA_CONSOLE_H

#include <cstdint>
#include <string>

namespace ps2plat {
namespace vita {

/* Colours as the display takes them: 0xAABBGGRR. */
constexpr std::uint32_t kWhite = 0xFFEBEBEBu, kGrey = 0xFF9A9A9Au, kYellow = 0xFF3CD2FFu, kRed = 0xFF5050FFu,
                        kGreen = 0xFF64DC64u, kBlue = 0xFFFFBE78u;

/* The screen shown (made the first time); false if no framebuffer could be had. */
bool ConsoleShow();
void ConsoleClear();
/* Text from the current line on, wrapped; '\n' starts a new line. Scrolls. */
void ConsolePrint(const std::string& text, std::uint32_t colour = kWhite);
/* Waits until one of `buttons` (SCE_CTRL_*) is pressed (and let go of
   first, so a press that is already down does not count); returns which. */
std::uint32_t ConsoleWaitButtons(std::uint32_t buttons);

}  // namespace vita
}  // namespace ps2plat

#endif  /* PS2PLAT_VITA_CONSOLE_H */
