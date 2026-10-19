/*
 * GS local memory layout (GS User's Manual, chapter 8: "Data format in local
 * memory").
 *
 * Memory is 4 MB of 8 KB pages, a page 32 blocks of 256 bytes, a block four
 * columns of 64 bytes. A buffer is a base block (bp) and a width in 64-pixel
 * units (bw); its pages run left to right, then down. Each format has a page
 * size in pixels and orders the blocks within a page, and the pixels within a
 * block, its own way - the tables below. The 8-bit formats PSMT8H, PSMT4HL and
 * PSMT4HH live in the top byte (or nibble) of a PSMCT32 pixel.
 *
 *   format          page       block    pixels per block
 *   PSMCT32/24, Z32 64 x 32    8 x 8    64 words
 *   PSMCT16/16S, Z  64 x 64   16 x 8    128 halfwords
 *   PSMT8           128 x 64  16 x 16   256 bytes
 *   PSMT4           128 x 128 32 x 16   512 nibbles
 */

#include "ps2host/gs.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ps2host {
namespace gs {

namespace {

constexpr u32 kMask = (4u << 20) - 1;

const u8 kBlock32[4][8] = {
    {0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
const u8 kBlockZ32[4][8] = {
    {24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15},
    {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
const u8 kBlock16[8][4] = {
    {0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
const u8 kBlock16S[8][4] = {
    {0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27},
    {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
const u8 kBlockZ16[8][4] = {
    {24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23},
    {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
const u8 kBlockZ16S[8][4] = {
    {24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3},
    {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};

// Within a block: the word (32-bit), halfword (16-bit), byte (8-bit) or
// nibble (4-bit) holding pixel (x, y).
const u8 kColumn32[8][8] = {
    {0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63}};
const u8 kColumn16[8][16] = {
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
    {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
    {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59},
    {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
    {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91},
    {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
    {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123},
    {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
// The first 8 rows; rows 8-15 are these plus 128.
const u8 kColumn8[8][16] = {
    {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
    {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
    {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
    {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
    {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
    {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
    {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127}};
// The first 8 rows; rows 8-15 are these plus 256.
const u16 kColumn4[8][32] = {
    {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106,
     4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122,
     20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43,
     69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59,
     85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
    {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170,
     196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
    {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186,
     212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
    {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235,
     133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
    {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251,
     149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255}};

u32 Page(u32 bp, u32 pages_per_row, u32 page_x, u32 page_y) {
    return bp + 32 * (page_y * (pages_per_row ? pages_per_row : 1) + page_x);
}

}  // namespace

u32 Memory::AddressReference(u32 bp, u32 bw, u32 x, u32 y, u32 psm, u32* nibble) {
    *nibble = 0;
    switch (psm) {
        case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: {
            const u32 block = Page(bp, bw, x / 64, y / 32) + kBlock32[(y % 32) / 8][(x % 64) / 8];
            return (block * 256 + kColumn32[y % 8][x % 8] * 4) & kMask;
        }
        case PSMZ32: case PSMZ24: {
            const u32 block = Page(bp, bw, x / 64, y / 32) + kBlockZ32[(y % 32) / 8][(x % 64) / 8];
            return (block * 256 + kColumn32[y % 8][x % 8] * 4) & kMask;
        }
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
            const u8 (*table)[4] = psm == PSMCT16 ? kBlock16 : psm == PSMCT16S ? kBlock16S
                                 : psm == PSMZ16 ? kBlockZ16 : kBlockZ16S;
            const u32 block = Page(bp, bw, x / 64, y / 64) + table[(y % 64) / 8][(x % 64) / 16];
            return (block * 256 + kColumn16[y % 8][x % 16] * 2) & kMask;
        }
        case PSMT8: {
            const u32 block = Page(bp, bw / 2, x / 128, y / 64) + kBlock32[(y % 64) / 16][(x % 128) / 16];
            const u32 row = y % 16;
            return (block * 256 + kColumn8[row % 8][x % 16] + (row >= 8 ? 128 : 0)) & kMask;
        }
        case PSMT4: {
            const u32 block = Page(bp, bw / 2, x / 128, y / 128) + kBlock16[(y % 128) / 16][(x % 128) / 32];
            const u32 row = y % 16;
            const u32 index = kColumn4[row % 8][x % 32] + (row >= 8 ? 256 : 0);
            *nibble = index & 1;
            return (block * 256 + index / 2) & kMask;
        }
        default:
            return (bp * 256 + (y * bw * 64 + x) * 4) & kMask;
    }
}

namespace {

// A layout's pages, precomputed: for each pixel of a page, its byte within
// the page (times two, plus the nibble, for PSMT4). Every access to GS memory
// - transfers, CLUT loads, texture decoding, the software rasteriser - goes
// through Address, and the tables took most of its time.
// The page's sides are powers of two, kept as shifts too: the Vita's
// Cortex-A9 has no divide instruction, and a division by a size read from
// the table was a library call - four of them a pixel.
struct PageTable {
    u32 width, height;                  // the page, in pixels
    bool half_width;                    // pages per row are BW / 2 (PSMT8, PSMT4)
    u32 wshift, hshift;                 // log2 of width and height
    std::vector<std::uint16_t> offset;
};

u32 Log2(u32 n) {
    u32 s = 0;
    while ((1u << s) < n) ++s;
    return s;
}

PageTable Build(u32 psm, u32 width, u32 height, bool half_width) {
    PageTable t{width, height, half_width, Log2(width), Log2(height),
                std::vector<std::uint16_t>(std::size_t(width) * height)};
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            u32 nibble = 0;
            const u32 a = Memory::AddressReference(0, half_width ? 2 : 1, x, y, psm, &nibble);
            t.offset[std::size_t(y) * width + x] = std::uint16_t(psm == PSMT4 ? a * 2 + nibble : a);
        }
    return t;
}

// Built once at start-up (the reference tables they come from are constant),
// and found by PSM without a guard or a switch: Address runs per pixel.
struct PageTables {
    PageTable ct32 = Build(PSMCT32, 64, 32, false), z32 = Build(PSMZ32, 64, 32, false);
    PageTable ct16 = Build(PSMCT16, 64, 64, false), ct16s = Build(PSMCT16S, 64, 64, false);
    PageTable z16 = Build(PSMZ16, 64, 64, false), z16s = Build(PSMZ16S, 64, 64, false);
    PageTable t8 = Build(PSMT8, 128, 64, true), t4 = Build(PSMT4, 128, 128, true);
    const PageTable* by_psm[64] = {};
    PageTables() {
        for (u32 psm : {PSMCT32, PSMCT24, PSMT8H, PSMT4HL, PSMT4HH}) by_psm[psm] = &ct32;
        by_psm[PSMZ32] = by_psm[PSMZ24] = &z32;
        by_psm[PSMCT16] = &ct16;
        by_psm[PSMCT16S] = &ct16s;
        by_psm[PSMZ16] = &z16;
        by_psm[PSMZ16S] = &z16s;
        by_psm[PSMT8] = &t8;
        by_psm[PSMT4] = &t4;
    }
};
const PageTables g_page_tables;

inline const PageTable* TableFor(u32 psm) { return g_page_tables.by_psm[psm & 63]; }

}  // namespace

u32 Memory::Address(u32 bp, u32 bw, u32 x, u32 y, u32 psm, u32* nibble) {
    const PageTable* t = TableFor(psm);
    if (!t) return AddressReference(bp, bw, x, y, psm, nibble);
    const u32 page = Page(bp, t->half_width ? bw / 2 : bw, x >> t->wshift, y >> t->hshift);
    const u32 within = t->offset[(std::size_t(y & (t->height - 1)) << t->wshift) + (x & (t->width - 1))];
    if (psm == PSMT4) {
        *nibble = within & 1;
        return (page * 256 + within / 2) & kMask;
    }
    *nibble = 0;
    return (page * 256 + within) & kMask;
}

u32 Memory::Read(u32 bp, u32 bw, u32 x, u32 y, u32 psm) const {
    u32 nibble = 0;
    const u32 a = Address(bp, bw, x, y, psm, &nibble);
    const u8* p = &bytes_[a];
    switch (psm) {
        case PSMCT32: case PSMZ32: return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
        case PSMCT24: case PSMZ24: return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return u32(p[0]) | u32(p[1]) << 8;
        case PSMT8: return p[0];
        case PSMT4: return nibble ? p[0] >> 4 : p[0] & 15;
        case PSMT8H: return p[3];
        case PSMT4HL: return p[3] & 15;
        case PSMT4HH: return p[3] >> 4;
        default: return 0;
    }
}

namespace {

// One pixel's store at byte `a` (and nibble), as Memory::Write.
template <u32 Psm>
inline void Store(u8* bytes, u32 within, u32 page, u32 value) {
    if constexpr (Psm == PSMT4) {
        u8* p = &bytes[(page * 256 + within / 2) & kMask];
        *p = (within & 1) ? u8((*p & 0x0F) | (value << 4)) : u8((*p & 0xF0) | (value & 15));
    } else {
        u8* p = &bytes[(page * 256 + within) & kMask];
        if constexpr (Psm == PSMCT32 || Psm == PSMZ32) {
            p[0] = u8(value); p[1] = u8(value >> 8); p[2] = u8(value >> 16); p[3] = u8(value >> 24);
        } else if constexpr (Psm == PSMCT24 || Psm == PSMZ24) {
            p[0] = u8(value); p[1] = u8(value >> 8); p[2] = u8(value >> 16);
        } else if constexpr (Psm == PSMT8) {
            p[0] = u8(value);
        } else if constexpr (Psm == PSMT8H) {
            p[3] = u8(value);
        } else if constexpr (Psm == PSMT4HL) {
            p[3] = u8((p[3] & 0xF0) | (value & 15));
        } else if constexpr (Psm == PSMT4HH) {
            p[3] = u8((p[3] & 0x0F) | (value << 4));
        } else {  // the 16-bit formats
            p[0] = u8(value); p[1] = u8(value >> 8);
        }
    }
}

// A row's run: the page and the row of offsets within it found once a page,
// not once a pixel.
template <u32 Psm>
void StoreRun(u8* bytes, const PageTable& t, u32 bp, u32 bw, u32 x, u32 y, const u32* values, u32 count) {
    const u32 page_bw = t.half_width ? bw / 2 : bw;
    const std::uint16_t* row = &t.offset[std::size_t(y & (t.height - 1)) << t.wshift];
    const u32 page_y = y >> t.hshift;
    for (u32 i = 0; i < count;) {
        const u32 px = x + i;
        const u32 page = Page(bp, page_bw, px >> t.wshift, page_y);
        const u32 in_page = std::min(count - i, t.width - (px & (t.width - 1)));
        for (u32 k = 0, wx = px & (t.width - 1); k < in_page; ++k, ++wx)
            Store<Psm>(bytes, row[wx], page, values[i + k]);
        i += in_page;
    }
}

}  // namespace

void Memory::WriteRun(u32 bp, u32 bw, u32 x, u32 y, u32 psm, const u32* values, u32 count) {
    const PageTable* t = TableFor(psm);
    u8* b = bytes_.data();
    switch (t ? psm : ~0u) {
        case PSMCT32: StoreRun<PSMCT32>(b, *t, bp, bw, x, y, values, count); return;
        case PSMZ32: StoreRun<PSMZ32>(b, *t, bp, bw, x, y, values, count); return;
        case PSMCT24: StoreRun<PSMCT24>(b, *t, bp, bw, x, y, values, count); return;
        case PSMZ24: StoreRun<PSMZ24>(b, *t, bp, bw, x, y, values, count); return;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
            StoreRun<PSMCT16>(b, *t, bp, bw, x, y, values, count); return;
        case PSMT8: StoreRun<PSMT8>(b, *t, bp, bw, x, y, values, count); return;
        case PSMT4: StoreRun<PSMT4>(b, *t, bp, bw, x, y, values, count); return;
        case PSMT8H: StoreRun<PSMT8H>(b, *t, bp, bw, x, y, values, count); return;
        case PSMT4HL: StoreRun<PSMT4HL>(b, *t, bp, bw, x, y, values, count); return;
        case PSMT4HH: StoreRun<PSMT4HH>(b, *t, bp, bw, x, y, values, count); return;
        default:
            for (u32 i = 0; i < count; ++i) Write(bp, bw, x + i, y, psm, values[i]);
    }
}

void Memory::Write(u32 bp, u32 bw, u32 x, u32 y, u32 psm, u32 value) {
    u32 nibble = 0;
    const u32 a = Address(bp, bw, x, y, psm, &nibble);
    u8* p = &bytes_[a];
    switch (psm) {
        case PSMCT32: case PSMZ32:
            p[0] = u8(value); p[1] = u8(value >> 8); p[2] = u8(value >> 16); p[3] = u8(value >> 24); break;
        case PSMCT24: case PSMZ24:
            p[0] = u8(value); p[1] = u8(value >> 8); p[2] = u8(value >> 16); break;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
            p[0] = u8(value); p[1] = u8(value >> 8); break;
        case PSMT8: p[0] = u8(value); break;
        case PSMT4: p[0] = nibble ? u8((p[0] & 0x0F) | (value << 4)) : u8((p[0] & 0xF0) | (value & 15)); break;
        case PSMT8H: p[3] = u8(value); break;
        case PSMT4HL: p[3] = u8((p[3] & 0xF0) | (value & 15)); break;
        case PSMT4HH: p[3] = u8((p[3] & 0x0F) | (value << 4)); break;
        default: break;
    }
}

}  // namespace gs
}  // namespace ps2host

// ------------------------------------------------------------------ textures, decoded whole

namespace ps2host {
namespace gs {

namespace {

// CLAMP's modes, as the software GS and the GPU renderers wrap a texel
// coordinate: REPEAT, CLAMP, REGION_CLAMP, REGION_REPEAT.
int WrapCoordinate(int i, u32 size, u32 mode, u32 lo, u32 hi) {
    if (mode == 0) return int(u32(i) & (size - 1));
    if (mode == 1) return i < 0 ? 0 : (int(size) - 1 < i ? int(size) - 1 : i);
    if (mode == 2) return i < int(lo) ? int(lo) : (int(hi) < i ? int(hi) : i);
    return int((u32(i) & lo) | hi);
}

// A level's column (row) for coordinate i: level 0 wrapped and kept within
// 2048, the smaller levels wrapped by their own region and clamped to them.
u32 LevelCoordinate(int i, u32 size0, u32 n, u32 mode, u32 lo, u32 hi) {
    if (n == 0) return u32(WrapCoordinate(i, size0, mode, lo, hi)) & 2047;
    const u32 size = std::max(1u, size0 >> n);
    const int c = WrapCoordinate(i, size, mode, lo >> n, hi >> n);
    return u32(c < 0 ? 0 : (int(size) - 1 < c ? int(size) - 1 : c));
}

u32 Expand(const TextureView& d, const u32* clut, u32 raw) {
    const u32 ta0 = d.texa_lo & 0xFF;
    const bool aem = (d.texa_lo & 0x8000u) != 0;
    switch (d.psm) {
        case PSMCT32: case PSMZ32: return raw;
        case PSMCT24: case PSMZ24: {
            const u32 rgb = raw & 0xFFFFFF;
            return rgb | ((aem && !rgb) ? 0u : ta0) << 24;
        }
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
            const u32 a = (raw & 0x8000u) ? d.ta1 : ((aem && !(raw & 0x7FFFu)) ? 0u : ta0);
            return ((raw & 31u) << 3) | (((raw >> 5) & 31u) << 11) | (((raw >> 10) & 31u) << 19) | (a << 24);
        }
        case PSMT8: case PSMT8H: return clut[raw & 0xFF];
        case PSMT4: case PSMT4HL: case PSMT4HH: return clut[(d.csa * 16 + (raw & 15)) & 0xFF];
        default: return raw;
    }
}

// A pixel's raw value at (page, its place there), as Memory::Read gives it.
template <u32 Psm>
inline u32 RawAt(const u8* bytes, u32 page, u32 within) {
    if constexpr (Psm == PSMT4) {
        const u8 b = bytes[(page * 256 + within / 2) & kMask];
        return (within & 1) ? u32(b >> 4) : u32(b & 15);
    } else {
        const u8* p = &bytes[(page * 256 + within) & kMask];
        if constexpr (Psm == PSMCT32) return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
        else if constexpr (Psm == PSMCT24) return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16;
        else if constexpr (Psm == PSMT8) return p[0];
        else if constexpr (Psm == PSMT8H) return p[3];
        else if constexpr (Psm == PSMT4HL) return u32(p[3] & 15);
        else if constexpr (Psm == PSMT4HH) return u32(p[3] >> 4);
        else return u32(p[0]) | u32(p[1]) << 8;          // the 16-bit formats
    }
}

// Row y's raw values at columns xs[0..count).
template <u32 Psm>
void ReadRow(const u8* bytes, const PageTable& t, u32 bp, u32 bw, u32 y, const u32* xs, u32 count, u32* raw) {
    const u32 page_bw = t.half_width ? bw / 2 : bw;
    const std::uint16_t* row = &t.offset[std::size_t(y & (t.height - 1)) << t.wshift];
    const u32 base = Page(bp, page_bw, 0, y >> t.hshift);
    const u32 wshift = t.wshift, wmask = t.width - 1;
    for (u32 i = 0; i < count; ++i) {
        const u32 x = xs[i];
        raw[i] = RawAt<Psm>(bytes, base + 32 * (x >> wshift), row[x & wmask]);
    }
}

void ReadRowAny(const u8* bytes, const PageTable& t, u32 psm, u32 bp, u32 bw, u32 y, const u32* xs, u32 count,
                u32* raw) {
    switch (psm) {
        case PSMCT32: case PSMZ32: ReadRow<PSMCT32>(bytes, t, bp, bw, y, xs, count, raw); return;
        case PSMCT24: case PSMZ24: ReadRow<PSMCT24>(bytes, t, bp, bw, y, xs, count, raw); return;
        case PSMT8: ReadRow<PSMT8>(bytes, t, bp, bw, y, xs, count, raw); return;
        case PSMT4: ReadRow<PSMT4>(bytes, t, bp, bw, y, xs, count, raw); return;
        case PSMT8H: ReadRow<PSMT8H>(bytes, t, bp, bw, y, xs, count, raw); return;
        case PSMT4HL: ReadRow<PSMT4HL>(bytes, t, bp, bw, y, xs, count, raw); return;
        case PSMT4HH: ReadRow<PSMT4HH>(bytes, t, bp, bw, y, xs, count, raw); return;
        default: ReadRow<PSMCT16>(bytes, t, bp, bw, y, xs, count, raw); return;     // the 16-bit formats
    }
}

// Raw values to RGBA, the format's switch outside the row.
void ExpandRow(const TextureView& d, const u32* clut, const u32* raw, u32 count, u32* out) {
    const u32 ta0 = d.texa_lo & 0xFF;
    const bool aem = (d.texa_lo & 0x8000u) != 0;
    switch (d.psm) {
        case PSMCT32: case PSMZ32:
            std::copy(raw, raw + count, out);
            return;
        case PSMCT24: case PSMZ24:
            for (u32 i = 0; i < count; ++i) {
                const u32 rgb = raw[i] & 0xFFFFFF;
                out[i] = rgb | ((aem && !rgb) ? 0u : ta0) << 24;
            }
            return;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
            for (u32 i = 0; i < count; ++i) {
                const u32 r = raw[i];
                const u32 a = (r & 0x8000u) ? d.ta1 : ((aem && !(r & 0x7FFFu)) ? 0u : ta0);
                out[i] = ((r & 31u) << 3) | (((r >> 5) & 31u) << 11) | (((r >> 10) & 31u) << 19) | (a << 24);
            }
            return;
        case PSMT8: case PSMT8H:
            for (u32 i = 0; i < count; ++i) out[i] = clut[raw[i] & 0xFF];
            return;
        case PSMT4: case PSMT4HL: case PSMT4HH: {
            const u32* row = clut + (d.csa * 16) % 256;     // CSA's 16 entries (CSA < 32: within the 256)
            if (d.csa < 16)
                for (u32 i = 0; i < count; ++i) out[i] = row[raw[i] & 15];
            else
                for (u32 i = 0; i < count; ++i) out[i] = clut[(d.csa * 16 + (raw[i] & 15)) & 0xFF];
            return;
        }
        default:
            std::copy(raw, raw + count, out);
            return;
    }
}

}  // namespace

u32 DecodeTexel(const Memory& memory, const TextureView& d, const u32* clut, int iu, int iv, int n) {
    u32 raw;
    if (n == 0) {
        const u32 u = LevelCoordinate(iu, d.tw, 0, d.wms, d.minu, d.maxu);
        const u32 v = LevelCoordinate(iv, d.th, 0, d.wmt, d.minv, d.maxv);
        raw = memory.Read(d.tbp, d.tbw, u, v, d.psm);
    } else {
        const u32 u = LevelCoordinate(iu, d.tw, u32(n), d.wms, d.minu, d.maxu);
        const u32 v = LevelCoordinate(iv, d.th, u32(n), d.wmt, d.minv, d.maxv);
        raw = memory.Read(d.level_tbp[n], std::max(1u, d.level_tbw[n]), u, v, d.psm);
    }
    return Expand(d, clut, raw);
}

u32 DecodedHeight(const TextureView& d) {
    const u32 levels = std::max(1u, std::min(d.levels, 7u));
    u32 height = 0;
    for (u32 n = 0; n < levels; ++n) height += std::max(1u, d.th >> n);
    return height;
}

void DecodeTexture(const Memory& memory, const TextureView& d, const u32* clut, u32* pixels) {
    const u32 levels = std::max(1u, std::min(d.levels, 7u));
    const PageTable* t = TableFor(d.psm);
    const u8* bytes = memory.data();
    std::vector<u32> xs(d.tw), raw(d.tw);
    u32 row = 0;
    for (u32 n = 0; n < levels; ++n) {
        const u32 w = std::max(1u, d.tw >> n), h = std::max(1u, d.th >> n);
        const u32 bp = n ? d.level_tbp[n] : d.tbp, bw = n ? std::max(1u, d.level_tbw[n]) : d.tbw;
        for (u32 u = 0; u < w; ++u) xs[u] = LevelCoordinate(int(u), d.tw, n, d.wms, d.minu, d.maxu);
        for (u32 v = 0; v < h; ++v) {
            u32* out = pixels + std::size_t(row + v) * d.tw;
            if (!t) {                                       // a format with no page table: as the GS reads it
                for (u32 u = 0; u < w; ++u) out[u] = DecodeTexel(memory, d, clut, int(u), int(v), int(n));
                continue;
            }
            const u32 y = LevelCoordinate(int(v), d.th, n, d.wmt, d.minv, d.maxv);
            ReadRowAny(bytes, *t, d.psm, bp, bw, y, xs.data(), w, raw.data());
            ExpandRow(d, clut, raw.data(), w, out);
        }
        row += h;
    }
}

}  // namespace gs
}  // namespace ps2host
