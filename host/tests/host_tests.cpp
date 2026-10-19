/*
 * The host's own tests, independent of any game: built with every port and run
 * by engine/tools/port.py before the port itself.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "ps2host/disc.h"
#include "ps2host/dma.h"
#include "ps2host/gs.h"
#include "ps2host/ipu.h"
#include "iop_vm_internal.h"
#include "ps2host/iop_vm.h"
#include "spu2.h"
#include "ps2host/mc2.h"
#include "ps2host/memory.h"
#include "ps2host/vif.h"
#include "ps2host/path3.h"
#include "ps2host/texture_pack.h"
#include "ps2host/vu.h"
#include "ps2host/vu_core.h"
#include "ps2rt/generated.h"
#include "ps2plat/platform.h"

#if PS2HOST_VULKAN
#include "ps2gpu/gs_renderer.h"
#endif

/* vu_corpus.cpp (vurecomp.py --corpus, as the build goes): the random programs, compiled. */
const ps2host::VuCompiled* VuGeneratedPrograms(std::size_t* count);

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

/* A test's folder and what is in it, gone - or as much as can be: errors are
   not the test's. Entries are listed before any is removed (the Vita's
   directory reads lost their place when the folder changed under them, and
   std::filesystem::remove_all threw "Directory not empty" - the eleventh
   run's ps2host_tests, ended by the uncaught exception). */
void RemoveTree(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code error;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
        entries.push_back(it->path());
    for (const fs::path& entry : entries) {
        if (fs::is_directory(entry, error)) RemoveTree(entry);
        else fs::remove(entry, error);
    }
    fs::remove(dir, error);
}

void MirrorsReachTheSameRam() {
    ps2host::Memory& m = ps2host::GuestMemory();
    m.Write(0x00100000u, 0x11223344u, 4);
    Check(m.Read(0x20100000u, 4) == 0x11223344u, "uncached mirror");
    Check(m.Read(0x30100000u, 4) == 0x11223344u, "uncached-accelerated mirror");
    Check(m.Read(0x80100000u, 4) == 0x11223344u, "kseg0");
    Check(m.Read(0xA0100000u, 4) == 0x11223344u, "kseg1");
}

void LoadsExtendAsTheEeDoes() {
    generated::Write32(0x00100010u, 0x80000001u);
    Check(generated::ReadS32(0x00100010u) == 0xFFFFFFFF80000001ull, "lw sign-extends");
    Check(generated::ReadU32(0x00100010u) == 0x80000001ull, "lwu zero-extends");
    generated::Write8(0x00100014u, 0xF0);
    Check(generated::ReadS8(0x00100014u) == 0xFFFFFFFFFFFFFFF0ull, "lb sign-extends");
}

void OutsideMemoryFaults() {
    bool faulted = false;
    try {
        ps2host::GuestMemory().Read(0x40000000u, 4);
    } catch (const ps2host::MemoryFault& fault) {
        faulted = fault.address == 0x40000000u && !fault.write;
    }
    Check(faulted, "an unmapped read faults");
}

void ScratchpadAndHooks() {
    ps2host::Memory& m = ps2host::GuestMemory();
    m.Write(ps2host::kScratchpadBase + 8, 0x55u, 1);
    Check(m.Read(ps2host::kScratchpadBase + 8, 1) == 0x55u, "scratchpad");
    int writes = 0;
    m.OnWrite(0x10003000u, [&](u32, u64 value) { writes += value == 7; });
    m.OnRead(0x10003000u, [](u32, u64 stored) { return stored + 1; });
    m.Write(0x10003000u, 7, 4);
    Check(writes == 1, "a write hook sees the value");
    Check(m.Read(0x10003000u, 4) == 8, "a read hook shapes the value");
}

void Kseg1ReachesDeviceRegisters() {
    ps2host::Memory& m = ps2host::GuestMemory();
    m.Write(0xB0001010u, 0x1234u, 4);                 // T1_MODE through kseg1
    Check(m.Read(0x10001010u, 4) == 0x1234u, "kseg1 reaches the EE registers");
    Check(m.Read(0x80100000u, 4) == m.Read(0x00100000u, 4), "kseg0 reaches RDRAM");
}

// A directory record: its length, extent, size, flags and name.
void Record(std::vector<u8>& image, u32 at, u32 lsn, u32 size, u8 flags, const std::string& name) {
    const u8 length = u8(33 + name.size() + (name.size() % 2 == 0 ? 1 : 0));
    image[at] = length;
    for (int i = 0; i < 4; ++i) {
        image[at + 2 + i] = u8(lsn >> (8 * i));
        image[at + 10 + i] = u8(size >> (8 * i));
    }
    image[at + 25] = flags;
    image[at + 32] = u8(name.size());
    std::memcpy(&image[at + 33], name.data(), name.size());
}

void DiscFindsFilesByPath() {
    using ps2host::kSectorSize;
    std::vector<u8> image(24 * kSectorSize, 0);
    u8* pvd = &image[16 * kSectorSize];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    Record(image, 16 * kSectorSize + 156, 20, kSectorSize, 2, std::string(1, '\0'));
    u32 at = 20 * kSectorSize;
    Record(image, at, 20, kSectorSize, 2, std::string(1, '\0')); at += 34;
    Record(image, at, 20, kSectorSize, 2, std::string(1, '\1')); at += 34;
    Record(image, at, 21, 5, 0, "FILE.BIN;1"); at += image[at];
    Record(image, at, 22, kSectorSize, 2, "DIR");
    Record(image, 22 * kSectorSize, 23, 3, 0, "INNER.DAT;1");
    std::memcpy(&image[21 * kSectorSize], "hello", 5);
    ps2host::Disc disc;
    disc.Use(image);
    ps2host::Disc::File file;
    Check(disc.Find("\\FILE.BIN;1", &file) && file.lsn == 21 && file.size == 5, "a file in the root");
    Check(disc.Find("/dir/inner.dat", &file) && file.lsn == 23 && file.size == 3,
          "a file in a directory, any case, no version");
    Check(!disc.Find("\\MISSING.BIN", &file), "a missing file");
    u8 sector[ps2host::kSectorSize];
    Check(disc.Read(21, 1, sector) && std::memcmp(sector, "hello", 5) == 0, "reading a sector");
    Check(!disc.Read(30, 1, sector), "reading past the end");
    Check(disc.IsCd(), "a small .iso counts as a CD");

    // The same disc as a raw CD image: Mode 2 sectors, data after the subheader.
    std::vector<u8> raw(image.size() / kSectorSize * ps2host::kRawSectorSize, 0);
    for (std::size_t i = 0; i < image.size() / kSectorSize; ++i) {
        u8* s = &raw[i * ps2host::kRawSectorSize];
        std::memset(s + 1, 0xFF, 10);
        s[15] = 2;
        s[16] = 0xAB;                                   // a subheader byte to see in 2340/2328 reads
        std::memcpy(s + 24, &image[i * kSectorSize], kSectorSize);
    }
    ps2host::Disc cd;
    cd.Use(raw);
    Check(cd.IsCd(), "a raw image is a CD");
    Check(cd.Find("/dir/inner.dat", &file) && file.lsn == 23 && file.size == 3, "a raw image's files");
    Check(cd.Read(21, 1, sector) && std::memcmp(sector, "hello", 5) == 0, "a raw sector's data");
    std::vector<u8> big(2340);
    Check(cd.ReadPattern(21, 1, 2, big.data()) && big[3] == 2 && big[4] == 0xAB &&
              std::memcmp(&big[12], "hello", 5) == 0,
          "a 2340-byte read: header, subheader, data");
    Check(cd.ReadPattern(21, 1, 1, big.data()) && std::memcmp(&big[0], "hello", 5) == 0,
          "a 2328-byte read starts at the data");
    Check(disc.ReadPattern(21, 1, 2, big.data()) && big[3] == 2 && std::memcmp(&big[12], "hello", 5) == 0,
          "an .iso's 2340-byte read has a made header");
    Check(!cd.Read(24, 1, sector), "reading past a raw image's end");
}

void BiosIsReadOnly() {
    ps2host::Memory& m = ps2host::GuestMemory();
    m.Write(ps2host::kBiosBase, 0xFFu, 1);
    Check(m.Read(ps2host::kBiosBase, 1) == 0, "the BIOS window ignores writes");
}

void WideWritesReachEveryWordsHook() {
    ps2host::Memory& m = ps2host::GuestMemory();
    u32 seen[2] = {0, 0};
    m.OnWrite(0x10006000u, [&seen](u32, u64 value) { seen[0] = u32(value); });
    m.OnWrite(0x10006004u, [&seen](u32, u64 value) { seen[1] = u32(value); });
    m.Write(0x10006000u, 0x2222222211111111ull, 8);
    Check(seen[0] == 0x11111111u && seen[1] == 0x22222222u, "a 64-bit write reaches both words");
    m.OnWrite(0x10006000u, nullptr);
    m.OnWrite(0x10006004u, nullptr);
}

void VifSkipsDataAndAppliesMaskPath3() {
    ps2host::VifStream vif;
    int masks = 0;
    bool masked = false;
    vif.on_mask_path3 = [&](bool on) { ++masks; masked = on; };
    // DIRECT of one quadword whose data looks like MSKPATH3: it must be skipped.
    vif.Word(0x50000001u);
    for (int i = 0; i < 4; ++i) vif.Word(0x06008000u);
    Check(masks == 0, "DIRECT data is not decoded");
    vif.Word(0x06008000u);
    Check(masks == 1 && masked, "MSKPATH3 masks");
    vif.Word(0x06000000u);
    Check(masks == 2 && !masked, "MSKPATH3 unmasks");
    // UNPACK V4-32 of 2 elements: 8 words. V2-16 of 3: 3 words.
    vif.Word(0x6C020000u);
    for (int i = 0; i < 8; ++i) vif.Word(0x06008000u);
    vif.Word(0x65030000u);
    for (int i = 0; i < 3; ++i) vif.Word(0x06008000u);
    Check(masks == 2 && vif.data_words() == 4 + 8 + 3, "UNPACK lengths");
    // STCYCL CL=1 WL=2: an UNPACK V4-32 of 4 reads 2 elements (filling writes).
    vif.Word(0x01000201u);
    vif.Word(0x6C040000u);
    for (int i = 0; i < 8; ++i) vif.Word(0);
    vif.Word(0x06008000u);
    Check(masks == 3, "UNPACK with WL > CL reads CL of every WL");
    // MPG of 1 instruction pair: 2 words.
    vif.Word(0x4A010000u);
    vif.Word(0x06000000u);
    vif.Word(0x06000000u);
    Check(masks == 3, "MPG data is not decoded");
}

void PutTag(u32 at, u32 qwc, u32 id, u32 address, u32 upper0, u32 upper1) {
    ps2host::Memory& m = ps2host::GuestMemory();
    m.Write(at, qwc | (id << 28), 4);
    m.Write(at + 4, address, 4);
    m.Write(at + 8, upper0, 4);
    m.Write(at + 12, upper1, 4);
}

void DmaChainsAreWalked() {
    ps2host::Memory& m = ps2host::GuestMemory();
    const u32 chain = 0x00200000u, table = 0x00210000u, sub = 0x00220000u;
    // cnt (1 qword after it) -> ref (1 qword at table) -> call sub -> end
    PutTag(chain, 1, 1, 0, 0xA0000001u, 0xA0000002u);
    for (u32 i = 0; i < 4; ++i) m.Write(chain + 16 + 4 * i, 0xD0 + i, 4);
    PutTag(chain + 32, 1, 3, table, 0, 0);
    for (u32 i = 0; i < 4; ++i) m.Write(table + 4 * i, 0xE0 + i, 4);
    PutTag(chain + 48, 0, 5, sub, 0, 0);
    PutTag(sub, 0, 6, 0, 0, 0);                     // ret: back to chain + 64
    PutTag(chain + 64, 0, 7, 0, 0, 0);              // end
    ps2host::DmaChannelState ch{0x145u, 0, 0, chain, 0, 0};    // chain, TTE, STR
    std::vector<u32> words;
    const ps2host::DmaOutcome out = ps2host::SendDma(ch, [&words](u32 w) { words.push_back(w); });
    Check(out.ended && out.tags == 5 && out.quadwords == 2, "a chain is walked to its end tag");
    Check(words.size() == 2 * 5 + 8 && words[0] == 0xA0000001u && words[2] == 0xD0u &&
          words[8] == 0xE0u, "tags' upper halves, then each tag's data, in order");
    Check(((ch.chcr >> 28) & 7) == 7 && ((ch.chcr >> 4) & 3) == 0,
          "the TAG field and the call depth are left as at the end");
    // A chain that never ends is stopped, not followed forever.
    PutTag(chain, 0, 2, chain, 0, 0);               // next -> itself
    ps2host::DmaChannelState loop{0x105u, 0, 0, chain, 0, 0};
    Check(!ps2host::SendDma(loop, [](u32) {}, 100).ended, "a looping chain is cut off");
}

// Every format's layout maps a page's pixels onto distinct bytes (nibbles).
/* A texture decoded whole, a row at a time (the GXM renderer's textures),
   is what the GS reads a texel at a time - every format, CLAMP's four modes
   with their regions, mipmap levels, TEXA and the CLUT - and what the
   renderer's texel-by-texel decode gave before it. */
void GsDecodesTexturesWhole() {
    using namespace ps2host::gs;
    auto memory = std::make_unique<Memory>();
    u32 seed = 2024;
    auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    for (u32 i = 0; i < (4u << 20); ++i) memory->data()[i] = u8(next() >> 24);
    std::vector<u32> clut(256);
    for (u32& c : clut) c = next();
    // The renderer's decode before (gxm_gs_renderer.cpp's TexelAt), as it was.
    auto wrap = [](int i, u32 size, u32 mode, u32 lo, u32 hi) {
        if (mode == 0) return int(u32(i) & (size - 1));
        if (mode == 1) return std::clamp(i, 0, int(size) - 1);
        if (mode == 2) return std::clamp(i, int(lo), int(hi));
        return int((u32(i) & lo) | hi);
    };
    auto old_texel = [&](const TextureView& d, int iu, int iv, int n) {
        u32 raw;
        if (n == 0) {
            const int u = wrap(iu, d.tw, d.wms, d.minu, d.maxu), v = wrap(iv, d.th, d.wmt, d.minv, d.maxv);
            raw = memory->Read(d.tbp, d.tbw, u32(u) & 2047, u32(v) & 2047, d.psm);
        } else {
            const u32 w = std::max(1u, d.tw >> n), h = std::max(1u, d.th >> n);
            const int u = std::clamp(wrap(iu, w, d.wms, d.minu >> n, d.maxu >> n), 0, int(w) - 1);
            const int v = std::clamp(wrap(iv, h, d.wmt, d.minv >> n, d.maxv >> n), 0, int(h) - 1);
            raw = memory->Read(d.level_tbp[n], std::max(1u, d.level_tbw[n]), u32(u), u32(v), d.psm);
        }
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
    };
    const u32 psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
                        PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
    u32 wrong_whole = 0, wrong_texel = 0, texels = 0;
    std::vector<u32> pixels;
    for (u32 psm : psms)
        for (int i = 0; i < 24; ++i) {
            TextureView d;
            d.psm = psm;
            d.tbp = next() >> 18;
            d.tbw = (next() >> 8) % 12;                     // 0 included: one page a row, as the GS takes it
            d.tw = 1u << ((next() >> 8) % 9);
            d.th = 1u << ((next() >> 8) % 9);
            d.wms = (next() >> 8) & 3;
            d.wmt = (next() >> 8) & 3;
            d.minu = (next() >> 8) & 1023, d.maxu = (next() >> 8) & 1023;
            d.minv = (next() >> 8) & 1023, d.maxv = (next() >> 8) & 1023;
            if (d.wms == 2 && d.minu > d.maxu) std::swap(d.minu, d.maxu);
            if (d.wmt == 2 && d.minv > d.maxv) std::swap(d.minv, d.maxv);
            d.texa_lo = next() & 0x80FF;
            d.ta1 = next() & 0xFF;
            d.csa = (next() >> 8) & 31;
            d.levels = 1 + (next() >> 8) % 7;
            for (u32 n = 1; n < 7; ++n) d.level_tbp[n] = next() >> 18, d.level_tbw[n] = (next() >> 8) % 12;
            const u32 height = DecodedHeight(d);
            pixels.assign(std::size_t(d.tw) * height, 0xDEADBEEFu);
            DecodeTexture(*memory, d, clut.data(), pixels.data());
            u32 row = 0;
            for (u32 n = 0; n < std::min(d.levels, 7u); ++n) {
                const u32 w = std::max(1u, d.tw >> n), h = std::max(1u, d.th >> n);
                for (u32 v = 0; v < h; ++v)
                    for (u32 u = 0; u < d.tw; ++u) {
                        const u32 got = pixels[std::size_t(row + v) * d.tw + u];
                        if (u >= w) {                       // right of a smaller level: untouched
                            wrong_whole += got != 0xDEADBEEFu;
                            continue;
                        }
                        const u32 want = old_texel(d, int(u), int(v), int(n));
                        wrong_whole += got != want;
                        wrong_texel += DecodeTexel(*memory, d, clut.data(), int(u), int(v), int(n)) != want;
                        ++texels;
                    }
                row += h;
            }
        }
    char what[160];
    std::snprintf(what, sizeof what, "GS textures decoded a row at a time are the GS's texels (%u of %u wrong)",
                  unsigned(wrong_whole), unsigned(texels));
    Check(wrong_whole == 0, what);
    std::snprintf(what, sizeof what, "...and so are they one at a time (%u wrong)", unsigned(wrong_texel));
    Check(wrong_texel == 0, what);
}

void GsLayoutsArePermutations() {
    using namespace ps2host::gs;
    struct Case { u32 psm, w, h, bw; };
    const Case cases[] = {{PSMCT32, 64, 32, 1}, {PSMZ32, 64, 32, 1}, {PSMCT16, 64, 64, 1},
                          {PSMCT16S, 64, 64, 1}, {PSMZ16, 64, 64, 1}, {PSMZ16S, 64, 64, 1},
                          {PSMT8, 128, 64, 2}, {PSMT4, 128, 128, 2}};
    for (const Case& c : cases) {
        std::vector<int> seen(8192 * 2, 0);
        bool ok = true;
        const u32 bytes = c.psm == PSMT4 ? 0 : c.psm == PSMT8 ? 1 : (c.psm & 2) ? 2 : 4;
        for (u32 y = 0; y < c.h; ++y)
            for (u32 x = 0; x < c.w; ++x) {
                u32 nibble = 0;
                const u32 a = Memory::Address(0, c.bw, x, y, c.psm, &nibble);
                if (a >= 8192) { ok = false; continue; }
                const u32 slot = bytes ? a / bytes : a * 2 + nibble;
                if (seen[slot]++) ok = false;
            }
        char what[64];
        std::snprintf(what, sizeof what, "GS layout 0x%02X covers one page exactly", unsigned(c.psm));
        Check(ok, what);
    }
    // The precomputed pages give what the manual's tables give, anywhere.
    const u32 psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
                        PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
    u32 seed = 99, wrong = 0;
    for (u32 psm : psms)
        for (int i = 0; i < 20000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const u32 bp = seed >> 18, bw = 1 + ((seed >> 8) & 15);
            seed = seed * 1664525u + 1013904223u;
            const u32 x = (seed >> 5) & 2047, y = (seed >> 17) & 2047;
            u32 n1 = 0, n2 = 0;
            if (Memory::Address(bp, bw, x, y, psm, &n1) != Memory::AddressReference(bp, bw, x, y, psm, &n2) ||
                n1 != n2)
                ++wrong;
        }
    Check(wrong == 0, "GS addresses from precomputed pages match the step-by-step layout");
    // A row's run (image transfers) stores what one pixel at a time stores.
    auto one = std::make_unique<Memory>(), run = std::make_unique<Memory>();
    for (u32 psm : psms)
        for (int i = 0; i < 300; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const u32 bp = seed >> 18, bw = 1 + ((seed >> 8) & 15);
            seed = seed * 1664525u + 1013904223u;
            const u32 x = (seed >> 5) & 2047, y = (seed >> 17) & 2047, count = std::min(1 + (seed & 255), 2048 - x);
            std::vector<u32> values(count);
            for (u32& v : values) v = seed = seed * 1664525u + 1013904223u;
            for (u32 k = 0; k < count; ++k) one->Write(bp, bw, x + k, y, psm, values[k]);
            run->WriteRun(bp, bw, x, y, psm, values.data(), count);
        }
    Check(std::memcmp(one->data(), run->data(), 4u << 20) == 0, "GS row runs store as single pixels do");
}

void GsTakesRegisterPacketsInBlocks() {
    using namespace ps2host::gs;
    std::vector<u32> words;
    auto quad = [&](u64 low, u64 high) {
        words.insert(words.end(), {u32(low), u32(low >> 32), u32(high), u32(high >> 32)});
    };
    quad(3ull | 1ull << 60, 0xE); // PACKED A+D: configure a 64-pixel target.
    quad(1ull << 16, 0x4C);
    quad(63ull << 16 | 63ull << 48, 0x40);
    quad(6, 0x00);
    quad(1ull | 1ull << 58 | 3ull << 60, 0x551); // odd REGLIST, with padding.
    quad(0x80402010, 4ull << 4 | (4ull << 4) << 16);
    quad(8ull << 4 | (6ull << 4) << 16, ~0ull);
    quad(0, 0); // empty tag, then another PACKED sprite.
    quad(2ull | 2ull << 60, 0x51);
    quad(0x10ull | 0x20ull << 32, 0x40ull | 0x80ull << 32);
    quad(10ull << 4 | (10ull << 4) << 32, 0);
    quad(0x10ull | 0x20ull << 32, 0x40ull | 0x80ull << 32);
    quad(14ull << 4 | (12ull << 4) << 32, 0);
    auto reference = std::make_unique<Gs>();
    for (u32 word : words) reference->Word(3, word);
    reference->Finish();
    Check(reference->primitives() == 2, "mixed GIF fixture draws two sprites");
    for (u32 chunk = 1; chunk <= 19; ++chunk) {
        auto blocks = std::make_unique<Gs>();
        for (u32 i = 0; i < words.size();) {
            const u32 n = std::min(chunk, u32(words.size()) - i);
            blocks->Words(3, words.data() + i, n);
            i += n;
        }
        blocks->Finish();
        Check(blocks->Summary() == reference->Summary() &&
              blocks->primitives() == reference->primitives() &&
              std::memcmp(blocks->memory.data(), reference->memory.data(), 4u << 20) == 0,
              "batched and fragmented register packets match single words");
    }
}

void GsDrawsASprite() {
    using namespace ps2host::gs;
    Gs gs;
    gs.WriteRegister(0x4C, (u64(0) << 0) | (u64(1) << 16) | (u64(PSMCT32) << 24));   // FRAME: fbp 0, 64 wide
    gs.WriteRegister(0x40, u64(63) << 16 | u64(63) << 48);                          // SCISSOR 0..63
    gs.WriteRegister(0x00, 6);                                                      // PRIM: sprite
    gs.WriteRegister(0x01, 0x80402010ull);                                          // RGBAQ
    gs.WriteRegister(0x05, (u64(4) << 4) | (u64(4) << 4) << 16);                    // XYZ2 (4,4)
    gs.WriteRegister(0x05, (u64(8) << 4) | (u64(6) << 4) << 16);                    // XYZ2 (8,6)
    gs.Finish();                                                                    // the queued draws
    Check(gs.memory.Read(0, 1, 5, 5, PSMCT32) == 0x80402010u, "a sprite's pixels take its colour");
    Check(gs.memory.Read(0, 1, 8, 5, PSMCT32) == 0 && gs.memory.Read(0, 1, 5, 6, PSMCT32) == 0,
          "a sprite stops short of its far edges");
    Check(gs.primitives() == 1 && gs.pixels_drawn() == 8, "one sprite of 4x2 pixels");
}

// PATH3 held by MSKPATH3: an unmask lets one EOP-ended group of packets go,
// as Burnout 3's texture uploads need (one group per window, five a picture).
void GsLoadsACsm2Clut() {
    using namespace ps2host::gs;
    Gs gs;
    // CSM2: 16 CT16 entries in a row, at (32..47, 3) of a 64-wide buffer at
    // block 0x40 - TEXCLUT CBW 1, COU 2, COV 3 - loaded in order, CSA aside.
    for (u32 i = 0; i < 16; ++i) gs.memory.Write(0x40, 1, 32 + i, 3, PSMCT16, 0x8000u | (i * 0x0421u));
    gs.WriteRegister(0x1C, 1 | (2u << 6) | (3u << 12));
    gs.WriteRegister(0x06, (u64(PSMT4) << 20) | (u64(0x40) << 37) | (u64(PSMCT16) << 51) | (u64(1) << 55) |
                               (u64(5) << 56) | (u64(1) << 61));
    bool ok = true;
    for (u32 i = 0; i < 16; ++i) ok = ok && (gs.Clut()[i] & 0xFFFFFF) == (i << 3) * 0x010101u;
    Check(ok, "a CSM2 CLUT loads from TEXCLUT's row, in order");
}

void Path3GoesOneGroupPerWindow() {
    ps2host::Path3Queue queue;
    auto tag = [&](u32 nloop, bool eop, u32 flg, u32 nreg) {
        const u64 low = u64(nloop) | u64(eop) << 15 | u64(flg) << 58 | u64(nreg) << 60;
        queue.Push(u32(low));
        queue.Push(u32(low >> 32));
        queue.Push(0);
        queue.Push(0);
    };
    auto data = [&](u32 qwords) { for (u32 i = 0; i < 4 * qwords; ++i) queue.Push(0xD0D0D0D0u); };
    tag(1, false, 0, 4); data(4);                   // PACKED: A+D setup, no EOP
    tag(3, true, 2, 0); data(3);                    // IMAGE, EOP: the first group ends here
    tag(2, true, 1, 3); data(3);                    // REGLIST 2 x 3 registers: 3 quadwords
    std::vector<u32> sent;
    queue.Release(false, [&](u32 word) { sent.push_back(word); });
    Check(sent.size() == 4 * (1 + 4 + 1 + 3), "an unmask lets the packets up to the first EOP go");
    sent.clear();
    queue.Release(false, [&](u32 word) { sent.push_back(word); });
    Check(sent.size() == 4 * (1 + 3) && queue.empty(), "the next unmask the next group (REGLIST sized by halves)");
    tag(1, false, 2, 0); data(1);
    queue.Release(false, [&](u32 word) { sent.push_back(word); });
    Check(queue.empty(), "a group without its EOP yet goes whole");
}

// Fog (PRIM.FGE): C = (F * C + (255 - F) * FOGCOL) >> 8, F from XYZF2 or the
// FOG register for XYZ2.
void GsFogsTowardsFogcol() {
    using namespace ps2host::gs;
    Gs gs;
    gs.WriteRegister(0x4C, (u64(0) << 0) | (u64(1) << 16) | (u64(PSMCT32) << 24));   // FRAME: fbp 0, 64 wide
    gs.WriteRegister(0x40, u64(63) << 16 | u64(63) << 48);                          // SCISSOR 0..63
    gs.WriteRegister(0x3D, 0x0000FFull);                                            // FOGCOL: red
    gs.WriteRegister(0x00, 6 | 1 << 5);                                             // PRIM: sprite, FGE
    gs.WriteRegister(0x01, 0x80FFFFFFull);                                          // RGBAQ: white
    gs.WriteRegister(0x04, (u64(0) << 4) | (u64(0) << 4) << 16 | u64(0x80) << 56);  // XYZF2 (0,0) F 0x80
    gs.WriteRegister(0x04, (u64(4) << 4) | (u64(4) << 4) << 16 | u64(0x80) << 56);  // XYZF2 (4,4)
    gs.Finish();
    Check(gs.memory.Read(0, 1, 1, 1, PSMCT32) == 0x807F7FFEu, "fog halfway to red: (128 * 255 + 127 * FOGCOL) >> 8");
    gs.WriteRegister(0x0A, u64(0) << 56);                                           // FOG: F 0 for XYZ2
    gs.WriteRegister(0x05, (u64(8) << 4) | (u64(0) << 4) << 16);                    // XYZ2 (8,0)
    gs.WriteRegister(0x05, (u64(12) << 4) | (u64(4) << 4) << 16);                   // XYZ2 (12,4)
    gs.Finish();
    Check(gs.memory.Read(0, 1, 9, 1, PSMCT32) == 0x800000FEu, "F 0 from the FOG register: FOGCOL (* 255 / 256)");
}

// A flat depth stays exact across a triangle: Burnout 3's loading bar is drawn at
// z 0xFFFFFF with GEQUAL over a buffer cleared to 0xFFFFFF, and float
// interpolation once lost the last bit and carved it into stripes.
void GsKeepsAFlatDepthExact() {
    using namespace ps2host::gs;
    Gs gs;
    gs.WriteRegister(0x4C, (u64(0) << 0) | (u64(1) << 16) | (u64(PSMCT32) << 24));   // FRAME: fbp 0, 64 wide
    gs.WriteRegister(0x4E, u64(8) | (u64(PSMZ24 & 0xF) << 24));                      // ZBUF: zbp 8 (blocks 256)
    gs.WriteRegister(0x40, u64(63) << 16 | u64(63) << 48);                          // SCISSOR 0..63
    for (u32 y = 0; y < 64; ++y)
        for (u32 x = 0; x < 64; ++x) gs.memory.Write(256, 1, x, y, PSMZ24, 0xFFFFFF);
    gs.WriteRegister(0x47, 1ull << 16 | 2ull << 17);                                // TEST: Z GEQUAL
    gs.WriteRegister(0x00, 3);                                                      // PRIM: triangle
    gs.WriteRegister(0x01, 0x80FFFFFFull);
    const u64 z = u64(0xFFFFFF) << 32;
    gs.WriteRegister(0x05, (u64(3) << 4) | (u64(1) << 4) << 16 | z);
    gs.WriteRegister(0x05, (u64(61) << 4) | (u64(1) << 4) << 16 | z);
    gs.WriteRegister(0x05, (u64(3) << 4) | (u64(37) << 4) << 16 | z);
    gs.Finish();
    u32 missing = 0;
    for (u32 y = 2; y < 18; ++y)
        for (u32 x = 4; x < 20; ++x)
            if (gs.memory.Read(0, 1, x, y, PSMCT32) != 0x80FFFFFFu) ++missing;
    Check(missing == 0, "a flat depth passes GEQUAL against itself everywhere");
}

// Triangles fill top-left: a pixel on an edge is drawn only when the edge is a
// left or top one, so two triangles sharing an edge draw it once, and a right
// or bottom edge on a pixel leaves it alone. Burnout 3's text-entry keys are
// An IMAGE packet taken a run of words at a time (Gs::Words: its quadwords
// to the transfer together) stores what it stores a word at a time, for
// every format, 24-bit pixels across quadwords and a transfer that ends
// inside the packet included.
/* PCSX2's names for a texture pack: a 64x64 PSMT4 texture's blocks hashed as
   PCSX2 hashes them (the reference from xxhash's own XXH3 over the same
   blocks, in PCSX2's order), its CLUT's 16 entries from CSA's; and file
   names of every form read back. */
void TexturePackNamesAsPcsx2() {
    namespace texture_pack = ps2host::texture_pack;
    ps2host::gs::Memory memory;
    for (std::size_t i = 0; i < (4u << 20); ++i) memory.data()[i] = std::uint8_t((i * 7) % 251);
    std::uint32_t clut[256];
    for (std::uint32_t i = 0; i < 256; ++i) clut[i] = i * 0x01020304u + 5;
    const std::uint64_t tex0 = 0x100 | 2ull << 14 | 0x14ull << 20 | 6ull << 26 | 6ull << 30 | 3ull << 56;
    const texture_pack::Name name = texture_pack::NameOf(memory, tex0, 0, 0, clut);
    Check(name.Text() == "c7671cbbc91ae787-c31b5682050eba5e-00001994", "a 4-bit texture's PCSX2 name");
    // A pack's files by name, in a folder of their own: the forms PCSX2 reads.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "ps2host_texture_pack_test";
    RemoveTree(dir);
    fs::create_directories(dir / "sub");
    for (const char* file : {"c7671cbbc91ae787-c31b5682050eba5e-00005994.dds", "sub/1a2b-00001980.png",
                             "3c4d-r64x32-00001980.png", "5e6f-7a8b-r0x16-00001993.dds", "not-a-texture.png",
                             "1234-5678.txt"}) {
        std::FILE* f = std::fopen((dir / file).string().c_str(), "wb");
        if (f) std::fclose(f);
    }
    Check(texture_pack::Open(dir.string()) == 4, "a pack's files of every form, in folders too");
    bool pending = false;
    texture_pack::Find(name, &pending);
    Check(pending, "a texture the pack has (TCC's bit ignored): its file read in the background");
    texture_pack::Open("");                 // the pack closed (its reader done with the files) first
    RemoveTree(dir);
}

void GsTakesImagesInBlocks() {
    using namespace ps2host::gs;
    const u32 psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
                        PSMZ32, PSMZ24, PSMZ16, PSMZ16S};
    u32 seed = 7, wrong = 0;
    for (u32 psm : psms) {
        std::vector<u32> stream;
        auto qword = [&](u64 low, u64 high) {
            stream.insert(stream.end(), {u32(low), u32(low >> 32), u32(high), u32(high >> 32)});
        };
        const u32 w = 37, h = 9, qwords = 60;   // 60 quadwords: more than 37x9 pixels of most formats
        qword(4 | u64(1) << 60, 0xE);           // A+D, four registers
        qword(u64(0x40) << 32 | u64(2) << 48 | u64(psm) << 56, 0x50);  // BITBLTBUF: DBP 0x40, DBW 2
        qword(u64(5) << 32 | u64(3) << 48, 0x51);                      // TRXPOS: 5, 3
        qword(w | u64(h) << 32, 0x52);                                 // TRXREG
        qword(0, 0x53);                                                // TRXDIR: host to local
        qword(qwords | u64(2) << 58, 0);        // IMAGE
        for (u32 i = 0; i < 4 * qwords; ++i) stream.push_back(seed = seed * 1664525u + 1013904223u);
        auto one = std::make_unique<Gs>(), blocks = std::make_unique<Gs>();
        for (u32 word : stream) one->Word(3, word);
        for (std::size_t at = 0, step = 1; at < stream.size(); at += step, step = step * 3 % 23 + 1)
            blocks->Words(3, stream.data() + at, u32(std::min(step, stream.size() - at)));
        if (std::memcmp(one->memory.data(), blocks->memory.data(), 4u << 20) != 0 || !one->uploads() ||
            !blocks->uploads())
            ++wrong;
    }
    Check(wrong == 0, "GS image packets taken in blocks store as a word at a time");
}

// cells whose right edge lies on a whole pixel, translucent: drawn on both
// sides they doubled a column.
void GsFillsTopLeft() {
    using namespace ps2host::gs;
    Gs gs;
    gs.WriteRegister(0x4C, (u64(0) << 0) | (u64(1) << 16) | (u64(PSMCT32) << 24));   // FRAME: fbp 0, 64 wide
    gs.WriteRegister(0x40, u64(63) << 16 | u64(63) << 48);                          // SCISSOR 0..63
    gs.WriteRegister(0x42, 0ull | 2ull << 2 | 2ull << 4 | 1ull << 6 | 0x80ull << 32); // ALPHA: Cs * FIX 1.0 + Cd
    gs.WriteRegister(0x00, 3 | 1 << 6);                                             // PRIM: triangle, ABE
    gs.WriteRegister(0x01, 0x80101010ull);
    const auto xy = [](u32 x, u32 y) { return (u64(x) << 4) | (u64(y) << 4) << 16; };
    gs.WriteRegister(0x05, xy(2, 2));                                               // the square (2,2)-(10,10)
    gs.WriteRegister(0x05, xy(10, 2));                                              // in two halves on its diagonal
    gs.WriteRegister(0x05, xy(10, 10));
    gs.WriteRegister(0x05, xy(2, 2));
    gs.WriteRegister(0x05, xy(10, 10));
    gs.WriteRegister(0x05, xy(2, 10));
    gs.Finish();
    u32 once = 0, twice = 0, outside = 0;
    for (u32 y = 0; y < 12; ++y)
        for (u32 x = 0; x < 12; ++x) {
            const u32 rgb = gs.memory.Read(0, 1, x, y, PSMCT32) & 0xFFFFFF;
            const bool inside = x >= 2 && x < 10 && y >= 2 && y < 10;
            if (inside && rgb == 0x101010) ++once;
            if (rgb == 0x202020) ++twice;
            if (!inside && rgb != 0) ++outside;
        }
    Check(once == 64, "every pixel of the square drawn once");
    Check(twice == 0, "the shared diagonal drawn once, not by both halves");
    Check(outside == 0, "the right and bottom edges, on whole pixels, not drawn");
}

#if PS2HOST_VULKAN
// GS local memory on the GPU (--gpu): targets and depth buffers swizzled into it
// and out of it by a compute shader must land where the GS model puts them,
// byte for byte, in every format a target or depth buffer takes.
void GpuMemorySwizzlesAsTheGs() {
    using namespace ps2host::gs;
    std::string error;
    auto gpu = ps2gpu::CreateGpu(&error);
    if (!gpu) {
        std::printf("  (no Vulkan device, GPU local memory unchecked: %s)\n", error.c_str());
        return;
    }
    auto renderer = ps2gpu::CreateGsRenderer(*gpu, &error);
    Check(renderer != nullptr, "the GPU renderer starts");
    if (!renderer) return;
    const u32 w = 128, h = 64, fbw = 2;
    u32 seed = 12345;
    auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    auto to16 = [](u32 c) {
        return ((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10) | ((c >> 31) << 15);
    };
    auto from16 = [](u32 c) {
        return ((c & 31) << 3) | (((c >> 5) & 31) << 11) | (((c >> 10) & 31) << 19) | ((c & 0x8000) ? 0x80000000u : 0);
    };
    Memory reference;                               // what the GPU's copy must hold, all along
    std::vector<u8> gpu_bytes(4u << 20);
    auto same_memory = [&] {
        renderer->MemoryRead(0, 16384, gpu_bytes.data());
        return std::memcmp(gpu_bytes.data(), reference.data(), gpu_bytes.size()) == 0;
    };
    struct Format { u32 psm; bool depth; const char* name; };
    const Format formats[] = {{PSMCT32, false, "CT32"}, {PSMCT24, false, "CT24"}, {PSMCT16, false, "CT16"},
                              {PSMCT16S, false, "CT16S"}, {PSMZ32, true, "Z32"}, {PSMZ24, true, "Z24"},
                              {PSMZ16, true, "Z16"}, {PSMZ16S, true, "Z16S"}};
    u32 base = 0x40;                                // FBP, in 2048-word units; a new place each time
    for (const Format& f : formats) {
        const bool sixteen = f.psm == PSMCT16 || f.psm == PSMCT16S || f.psm == PSMZ16 || f.psm == PSMZ16S;
        // Image to memory.
        std::vector<u32> pixels(w * h);
        for (u32& p : pixels) p = f.depth ? next() & (sixteen ? 0xFFFFu : 0xFFFFFFu) : next();
        if (f.depth) {
            renderer->UploadDepth(base, w, h, pixels.data());
            Check(renderer->DepthToMemory(base, w, h, fbw, f.psm, w, h), "a depth buffer goes to GPU memory");
        } else {
            renderer->UploadTarget(base, fbw, f.psm, pixels.data(), w, h);
            Check(renderer->TargetToMemory(base, fbw, f.psm, w, h), "a target goes to GPU memory");
        }
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) {
                const u32 c = pixels[y * w + x];
                reference.Write(base * 32, fbw, x, y, f.psm, !f.depth && sixteen ? to16(c) : c);
            }
        std::string what = std::string(f.name) + ": an image swizzled on the GPU as the GS lays it out";
        Check(same_memory(), what.c_str());
        // Memory to image: other bytes there, from the CPU.
        const u32 blocks = fbw * (sixteen ? 1 : 2) * 32;   // the 64 rows' pages
        for (u32 i = base * 32 * 256; i < (base * 32 + blocks) * 256; ++i) reference.data()[i] = u8(next() >> 24);
        renderer->MemoryWrite(base * 32, base * 32 + blocks, reference.data() + base * 32 * 256);
        std::vector<u32> back;
        u32 bw_ = 0, bh = 0;
        if (f.depth) {
            renderer->DepthFromMemory(base, w, h, fbw, f.psm);
            Check(renderer->ReadDepth(base, w, h, &back), "a depth buffer reads back");
        } else {
            renderer->TargetFromMemory(base, fbw, f.psm, w, h);
            Check(renderer->ReadTarget(base, fbw, f.psm, &back, &bw_, &bh) && bw_ == w, "a target reads back");
        }
        u32 wrong = 0;
        for (u32 y = 0; y < h && back.size() == w * h; ++y)
            for (u32 x = 0; x < w; ++x) {
                const u32 raw = reference.Read(base * 32, fbw, x, y, f.psm);
                // Depth goes through a float (Z / 2^32): exact up to 24 bits.
                const u32 want = f.depth ? (raw >= 0xFFFFFF80u ? 0xFFFFFFFFu : u32(float(raw)))
                                         : sixteen ? from16(raw) : raw;
                if (back[y * w + x] != want) ++wrong;
            }
        what = std::string(f.name) + ": memory unswizzled on the GPU as the GS reads it";
        Check(back.size() == w * h && wrong == 0, what.c_str());
        base += 8;
    }
}

// A target drawn at 2x-4x taken to memory and back (swizzle.comp, its
// specialised pipelines): GS memory and every pixel as the scalar GS has them.
void GpuScaledColourTransfersAsTheGs() {
    using namespace ps2host::gs;
    std::string error;
    auto gpu = ps2gpu::CreateGpu(&error);
    if (!gpu) return;
    auto renderer = ps2gpu::CreateGsRenderer(*gpu, &error);
    Check(renderer != nullptr, "scaled transfer renderer starts");
    if (!renderer) return;
    Memory reference;
    renderer->MemoryWrite(0, 16384, reference.data());
    const u32 w = 384, h = 64, fbw = 6;
    u32 base = 0x20, seed = 52413;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    for (u32 scale : {2u, 3u, 4u}) {
        renderer->SetScale(scale);
        for (u32 psm : {PSMCT32, PSMCT24, PSMCT16, PSMCT16S}) {
            const bool sixteen = psm == PSMCT16 || psm == PSMCT16S;
            renderer->TargetFromMemory(base, fbw, psm, w, h);
            ps2gpu::GsDrawState state;
            state.fbp = base; state.fbw = fbw; state.psm = psm;
            state.width = w; state.height = h;
            state.scissor_x1 = w - 1; state.scissor_y1 = h - 1;
            const ps2gpu::GsVertex vertices[] = {
                {0, 0, 0, 16, 32, 64, 128, 0, 0, 1},
                {float(w), 0, 0, 16, 32, 64, 128, 0, 0, 1},
                {0, float(h), 0, 16, 32, 64, 128, 0, 0, 1},
                {float(w), 0, 0, 16, 32, 64, 128, 0, 0, 1},
                {float(w), float(h), 0, 16, 32, 64, 128, 0, 0, 1},
                {0, float(h), 0, 16, 32, 64, 128, 0, 0, 1}};
            renderer->DrawTriangles(state, vertices, 6);
            Check(renderer->TargetToMemory(base, fbw, psm, w, h), "scaled target swizzles to memory");
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x) {
                    u32 c = 0x80402010u;
                    if (sixteen) c = ((c >> 3) & 31) | (((c >> 11) & 31) << 5) |
                        (((c >> 19) & 31) << 10) | ((c >> 31) << 15);
                    reference.Write(base * 32, fbw, x, y, psm, c);
                }
            std::vector<u8> bytes(4u << 20);
            renderer->MemoryRead(0, 16384, bytes.data());
            Check(std::memcmp(bytes.data(), reference.data(), bytes.size()) == 0,
                  "2x/3x/4x color transfer matches scalar GS memory");
            const u32 begin = base * 32, end = begin + fbw * (sixteen ? 1 : 2) * 32;
            for (u32 i = begin * 256; i < end * 256; ++i) reference.data()[i] = u8(next() >> 24);
            renderer->MemoryWrite(begin, end, reference.data() + begin * 256);
            renderer->TargetFromMemory(base, fbw, psm, w, h);
            std::vector<u32> back;
            u32 width = 0, height = 0;
            Check(renderer->ReadTarget(base, fbw, psm, &back, &width, &height) && width == w && height == h,
                  "scaled reloaded image reads at native dimensions");
            u32 wrong = 0;
            if (back.size() == w * h)
                for (u32 y = 0; y < h; ++y)
                    for (u32 x = 0; x < w; ++x) {
                        const u32 raw = reference.Read(begin, fbw, x, y, psm);
                        const u32 want = sixteen ? ((raw & 31) << 3) | (((raw >> 5) & 31) << 11) |
                            (((raw >> 10) & 31) << 19) | ((raw & 0x8000) ? 0x80000000u : 0) : raw;
                        if (back[y * w + x] != want) ++wrong;
                    }
            Check(back.size() == w * h && wrong == 0, "scaled reload preserves every native color pixel");
            base += 16;
        }
    }
}

// Textures decoded on the GPU from its copy of local memory (--gpu, when the
// GPU holds their blocks) must come out texel for texel as the GS model
// decodes them: every format, CLUTs (CSA for 4-bit ones), TEXA, region modes.
void GpuDecodesTexturesAsTheGs() {
    using namespace ps2host::gs;
    std::string error;
    auto gpu = ps2gpu::CreateGpu(&error);
    if (!gpu) return;                               // reported by GpuMemorySwizzlesAsTheGs
    auto renderer = ps2gpu::CreateGsRenderer(*gpu, &error);
    if (!renderer) return;
    Gs gs;
    gs.rasterize = false;
    u32 seed = 777;
    for (u32 i = 0; i < (4u << 20); ++i) {
        seed = seed * 1664525u + 1013904223u;
        gs.memory.data()[i] = u8(seed >> 24);
    }
    renderer->MemoryWrite(0, 16384, gs.memory.data());
    std::vector<u32> decoded;
    gs.on_draw = [&](const Gs::DrawCall&) {
        u32 w = 0, h = 0;
        gs.DecodeTexture(&decoded, &w, &h);
    };
    struct Case { u32 psm, cpsm, csa; u64 clamp; const char* name; };
    const u64 region_clamp = 2 | 2 << 2 | u64(5) << 4 | u64(40) << 14 | u64(3) << 24 | u64(20) << 34;
    const u64 region_repeat = 3 | 3 << 2 | u64(15) << 4 | u64(32) << 14 | u64(7) << 24 | u64(8) << 34;
    const Case cases[] = {
        {PSMCT32, 0, 0, 0, "CT32"}, {PSMCT24, 0, 0, 0, "CT24"}, {PSMCT16, 0, 0, 0, "CT16"},
        {PSMCT16S, 0, 0, region_clamp, "CT16S, region clamp"}, {PSMT8, 0, 0, 0, "T8, CT32 CLUT"},
        {PSMT8, PSMCT16, 0, region_repeat, "T8, CT16 CLUT, region repeat"}, {PSMT4, 0, 3, 0, "T4, CSA 3"},
        {PSMT8H, 0, 0, 0, "T8H"}, {PSMT4HL, 0, 5, 0, "T4HL"}, {PSMT4HH, 0, 1, 0, "T4HH"},
        {PSMZ32, 0, 0, 0, "Z32"}, {PSMZ24, 0, 0, 0, "Z24"}, {PSMZ16, 0, 0, 0, "Z16"}, {PSMZ16S, 0, 0, 0, "Z16S"}};
    u64 key = 1;
    for (const Case& k : cases) {
        const u32 tbp = 0x480, tbw = 2, lw = 6, lh = 6;             // 64 x 64
        const u64 texa = 0x40 | 1ull << 15 | 0x90ull << 32;           // TA0 0x40, AEM, TA1 0x90
        const u64 tex0 = u64(tbp) | u64(tbw) << 14 | u64(k.psm) << 20 | u64(lw) << 26 | u64(lh) << 30 |
                         u64(0x2400) << 37 | u64(k.cpsm) << 51 | u64(k.csa) << 56 | 1ull << 61;   // CLD 1
        gs.WriteRegister(0x3B, texa);
        gs.WriteRegister(0x08, k.clamp);
        gs.WriteRegister(0x06, tex0);
        gs.WriteRegister(0x00, 6 | 1 << 4);                           // a textured sprite
        gs.WriteRegister(0x05, 0);
        gs.WriteRegister(0x05, (u64(16) << 4) | (u64(16) << 4) << 16);
        ps2gpu::GsTextureDecode d;
        d.tbp = tbp;
        d.tbw = tbw;
        d.psm = k.psm;
        d.tw = 1u << lw;
        d.th = 1u << lh;
        d.wms = u32(k.clamp & 3);
        d.wmt = u32(k.clamp >> 2 & 3);
        d.minu = u32(k.clamp >> 4 & 1023);
        d.maxu = u32(k.clamp >> 14 & 1023);
        d.minv = u32(k.clamp >> 24 & 1023);
        d.maxv = u32(k.clamp >> 34 & 1023);
        d.texa_lo = u32(texa & 0x80FF);
        d.ta1 = u32(texa >> 32 & 0xFF);
        d.csa = k.csa;
        d.clut = gs.Clut().data();
        renderer->TextureFromMemory(key, d);
        std::vector<u32> back;
        u32 w = 0, h = 0;
        const bool read = renderer->ReadTexture(key, &back, &w, &h);
        u32 wrong = 0;
        for (std::size_t i = 0; i < back.size() && i < decoded.size(); ++i) wrong += back[i] != decoded[i];
        const std::string what = std::string(k.name) + ": decoded on the GPU as the GS model decodes it";
        Check(read && back.size() == decoded.size() && decoded.size() == 64 * 64 && wrong == 0, what.c_str());
        ++key;
        if (k.psm != PSMT8 && k.psm != PSMT4 && k.psm != PSMT8H && k.psm != PSMT4HL && k.psm != PSMT4HH) continue;
        // The same through a CLUT the GPU loads from its own copy of memory
        // (--gpu, a CLUT drawn on the GPU): its other rows garbage first, so
        // a texture reading any row the load did not set fails.
        std::vector<u32> garbage(256);
        for (u32 i = 0; i < 256; ++i) garbage[i] = i * 0x9E3779B9u;
        const bool eight = k.psm == PSMT8 || k.psm == PSMT8H;
        renderer->ClutRows(garbage.data(), 0xFFFF);
        if (!eight) renderer->ClutRows(gs.Clut().data(), ~(1u << (k.csa % 16)));
        ps2gpu::GsClutLoad load;
        load.cbp = 0x2400;
        load.cpsm = k.cpsm;
        load.csa = k.csa;
        load.eight = eight;
        load.texa_lo = d.texa_lo;
        load.ta1 = d.ta1;
        renderer->ClutFromMemory(load);
        d.clut = nullptr;
        renderer->TextureFromMemory(key, d);
        back.clear();
        const bool again = renderer->ReadTexture(key, &back, &w, &h);
        wrong = 0;
        for (std::size_t i = 0; i < back.size() && i < decoded.size(); ++i) wrong += back[i] != decoded[i];
        const std::string loaded = std::string(k.name) + ": through a CLUT loaded on the GPU";
        Check(again && back.size() == decoded.size() && wrong == 0, loaded.c_str());
        ++key;
    }
}
#endif

// VIF1 into a VU: a microprogram loaded with MPG and run with MSCAL, UNPACK's
// formats, mask and skipping writes, and XGKICK.
void VifRunsMicroprogramsOnAVu() {
    ps2host::Vu vu(1);
    ps2host::VifStream vif;
    vif.vu = &vu;
    vif.double_buffered = true;
    std::vector<std::uint64_t> kicked;
    vu.on_kick = [&](std::uint64_t low, std::uint64_t) { kicked.push_back(low); };
    auto f = [](float x) { std::uint32_t u; std::memcpy(&u, &x, 4); return u; };
    const std::uint32_t kNopU = 0x000002FF, kNopL = 0x8000033C, kEnd = 0x40000000;
    const std::uint32_t program[] = {
        // 0: lq.xyzw vf1, 0(vi0)              1: add.xyzw vf2, vf1, vf1
        0x01E10000, kNopU, kNopL, 0x01E108A8,
        // 2: sq.xyzw vf2, 1(vi0) [E]          3: (the pair after the end)
        0x03E01001, kNopU | kEnd, kNopL, kNopU,
        // 4: iaddiu vi1, vi0, 2               5: xgkick vi1 [E]          6: -
        0x10010002, kNopU, 0x80000EFC, kNopU | kEnd, kNopL, kNopU};
    vif.Word(0x4A000000u | (7u << 16));                     // MPG 7 pairs at 0
    for (std::uint32_t w : program) vif.Word(w);
    vif.Word(0x01000101u);                                  // STCYCL CL 1 WL 1
    vif.Word(0x6C010000u);                                  // UNPACK V4-32, 1 at 0
    for (float x : {1.0f, 2.0f, 3.0f, 4.0f}) vif.Word(f(x));
    vif.Word(0x14000000u);                                  // MSCAL 0
    const std::uint32_t* q = vu.Quad(1);
    Check(q[0] == f(2) && q[1] == f(4) && q[2] == f(6) && q[3] == f(8), "VU: lq, add, sq");
    Check(vu.resume == 4 * 8, "VU: a program ends after the pair following its E bit");

    // V3-16 masked: w from the row register.
    vif.Word(0x20000000u);                                  // STMASK
    vif.Word(0x40u);                                        // cycle 0, w: row
    vif.Word(0x30000000u);                                  // STROW
    for (std::uint32_t w : {0u, 0u, 0u, f(1)}) vif.Word(w);
    vif.Word(0x79010003u);                                  // UNPACK V3-16 masked, 1 at 3
    vif.Word(0x0002FFFFu);
    vif.Word(0x00000003u);
    q = vu.Quad(3);
    Check(q[0] == 0xFFFFFFFFu && q[1] == 2 && q[2] == 3 && q[3] == f(1), "UNPACK V3-16, sign-extended, masked");

    // CL 2, WL 1: every other quadword.
    vif.Word(0x01000102u);                                  // STCYCL CL 2 WL 1
    vif.Word(0x6002000Au);                                  // UNPACK S-32, 2 at 10
    vif.Word(5);
    vif.Word(6);
    Check(vu.Quad(10)[3] == 5 && vu.Quad(11)[0] == 0 && vu.Quad(12)[1] == 6, "UNPACK skipping writes");

    // A GIF packet at quadword 2: one A+D register write, then EOP.
    std::uint32_t* tag = vu.Quad(2);
    tag[0] = 1u | 1u << 15;                                 // NLOOP 1, EOP
    tag[1] = 1u << 28;                                      // FLG PACKED, NREG 1
    tag[2] = 0xE;                                           // A+D
    tag[3] = 0;
    vif.Word(0x01000101u);
    vif.Word(0x14000004u);                                  // MSCAL 4 (byte 32)
    Check(kicked.size() == 2 && (kicked[0] & 0x7FFF) == 1, "XGKICK sends the tag and its data");
}

// The VU's visible timing: the flags reach FMAND four cycles after the FMAC
// operation, Q seven after DIV (WAITQ waits for it), and an upper beside a LOI
// reads the I from before - as Burnout 3's microcode depends on.
// The FMAC's four lanes at once (single precision, SSE) give what the
// arithmetic model gives a lane at a time in double: every operation, operand
// form, dest mask and destination, on bit patterns rich in zeros, denormals,
// infinities, NaNs and values near overflow and underflow.
void VuLanesAreTheModel() {
    auto context = std::make_unique<lifted::Context>(), scalar = std::make_unique<lifted::Context>();
    u32 seed = 2024, wrong = 0, runs = 0;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    auto value = [&]() -> u32 {
        const u32 r = next(), sign = r & 0x80000000u;
        switch ((r >> 8) % 8) {
            case 0: return sign;                                            // zero
            case 1: return sign | (r & 0x007FFFFFu);                        // denormal
            case 2: return sign | 0x7F800000u | ((r >> 4) & 1);             // infinity, NaN
            case 3: return sign | 0x7F000000u | (next() & 0x007FFFFFu);     // near the top
            case 4: return sign | 0x00800000u | (next() & 0x00FFFFFFu);     // near the bottom
            case 5: return sign | 0x3F800000u | (next() & 0x007FFFFFu);     // about one
            default: return next();
        }
    };
    for (int i = 0; i < 200000; ++i) {
        for (auto& v : context->vf) for (u32& lane : v) lane = value();
        for (u32& lane : context->vacc) lane = value();
        context->vc[20 + 1] = value();                                       // I
        context->vc[20 + 2] = value();                                       // Q
        lifted::FmacOp f;
        f.op = int(next() % 7);
        f.operand = int(next() % 7);
        f.to_acc = (next() & 3) == 0;
        f.fd = next() % 32;
        f.fs = next() % 32;
        f.ft = next() % 32;
        f.dest = next() % 16;
        *scalar = *context;
        lifted::RunFmac(*context, f);
        lifted::RunFmacScalar(*scalar, f);
        ++runs;
        if (std::memcmp(context->vf, scalar->vf, sizeof context->vf) != 0 ||
            std::memcmp(context->vacc, scalar->vacc, sizeof context->vacc) != 0)
            ++wrong;
    }
    char what[96];
    std::snprintf(what, sizeof what, "VU FMAC lanes at once match the model (%u of %u differ)", wrong, runs);
    Check(wrong == 0, what);
}

// libmc2's handlers over a directory (mc2.h): a save made and read back, the
// listing the game's "open" reads sizes from, the answers it maps to errors.
void MemoryCardKeepsSaves() {
    namespace mc2 = ps2host::mc2;
    namespace fs = std::filesystem;
    const fs::path card = fs::temp_directory_path() / "ps2host_tests_memcard";
    RemoveTree(card);
    mc2::SetCardDirectory(card.u8string());
    auto context = std::make_unique<lifted::Context>();
    lifted::Context& c = *context;
    constexpr u32 kName = 0x00100100, kBuffer = 0x00100200, kEntries = 0x00100400, kCount = 0x00100300;
    auto name = [&](const char* text) {
        for (u32 i = 0;; ++i) {
            generated::Write8(kName + i, u8(text[i]));
            if (!text[i]) break;
        }
        c.r[5] = kName;
    };
    auto call = [&](void (*handler)(lifted::Context&)) { handler(c); return u32(c.r[2]); };
    c.r[4] = 0;
    name("SAVE");
    c.r[6] = 0x8427;
    Check(call(mc2::Create) == 0, "memory card: a directory made");
    Check(call(mc2::Create) == 0x81010011, "memory card: making it again is EEXIST");
    name("SAVE/data");
    c.r[6] = 0x8417;
    Check(call(mc2::Create) == 0, "memory card: a file made");
    for (u32 i = 0; i < 1500; ++i) generated::Write8(kBuffer + i, u8(i * 7));
    c.r[6] = kBuffer; c.r[7] = 0; c.r[8] = 1500;
    Check(call(mc2::WriteFile) == 1500, "memory card: written");
    for (u32 i = 0; i < 1500; ++i) generated::Write8(kBuffer + i, 0);
    c.r[6] = kBuffer; c.r[7] = 100; c.r[8] = 0;
    bool same = call(mc2::ReadFile) == 1400;
    for (u32 i = 0; i < 1400 && same; ++i) same = generated::ReadU8(kBuffer + i) == u8((i + 100) * 7);
    Check(same, "memory card: read back from an offset to the end");
    name("SAVE/data");
    c.r[6] = 0; c.r[7] = 1; c.r[8] = kEntries; c.r[9] = kCount;
    Check(call(mc2::GetDir) == 0 && generated::ReadU32(kCount) == 1 && generated::ReadU32(kEntries + 0x10) == 1500 &&
              (generated::ReadU32(kEntries + 0x14) & 0xFFFF) == 0x8497,
          "memory card: the listing gives the size and a written file's mode");
    name("SAVE/missing");
    Check(call(mc2::GetDir) == 0 && generated::ReadU32(kCount) == 0, "memory card: listing nothing is no error");
    name("NONE/data");
    Check(call(mc2::GetDir) == 0x81010002, "memory card: listing in no directory is ENOENT");
    name("SAVE/missing");
    c.r[6] = kBuffer; c.r[7] = 0; c.r[8] = 0;
    Check(call(mc2::ReadFile) == 0x81010002, "memory card: reading what is not there is ENOENT");
    name("../outside");
    c.r[6] = 0x8417;
    Check(call(mc2::Create) == 0x81010016 && !fs::exists(card.parent_path() / "outside"),
          "memory card: a path out of the card is refused");
    name("SAVE");
    c.r[6] = kEntries;
    Check(call(mc2::Lookup) == 0x81010011 && (generated::ReadU32(kEntries) & 0x20) != 0,
          "memory card: the lookup finds a directory");
    c.r[5] = kBuffer;
    Check(call(mc2::GetInfo) == 0 && generated::ReadU32(kBuffer) == 2 && generated::ReadU32(kBuffer + 4) == 1 &&
              generated::ReadU32(kBuffer + 8) < 8000,
          "memory card: a formatted PS2 card, its free space less the save");
    mc2::SetCardDirectory("");
    RemoveTree(card);
}

// Random microprograms (vu_corpus.cpp, from engine/tools/vurecomp.py
// --corpus), compiled ahead of time, run as the interpreter runs them: from
// random registers and memory, every register, flag, pipeline, cycle and
// byte of data memory the same at the end - and the pairs counted.
/* A program of the test corpus in a VU's instruction memory (NOPs around
   it), and random registers, flags and data memory from `seed`, which goes
   on: two VUs given the same seed start the same. */
void PrepareVuRun(ps2host::Vu& vu, const ps2host::VuCompiled& program, u32& seed) {
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    auto number = [&]() -> u32 {
        const u32 r = next();
        switch (r % 6) {
            case 0: return 0;
            case 1: return 0x3F800000u ^ (r & 0x80000000u);                    // +-1
            case 2: return (r & 0x807FFFFFu) | 0x3F000000u;                     // about one
            case 3: return (r & 0x807FFFFFu) | 0x43000000u;                     // about 128-256
            default: return next();
        }
    };
    for (std::size_t at = 0; at < vu.micro.size(); at += 8) {
        const u32 nop[2] = {0x8000033Cu, 0x000002FFu};
        std::memcpy(vu.micro.data() + at, nop, 8);
    }
    const u32* code = program.code;
    const u32 runs_in = *code++;
    for (u32 r = 0; r < runs_in; ++r) {
        std::memcpy(vu.micro.data() + code[0], code + 2, 4 * code[1]);
        code += 2 + code[1];
    }
    for (int r = 1; r < 32; ++r)
        for (u32& lane : vu.regs.vf[r]) lane = number();
    for (u32& lane : vu.regs.vacc) lane = number();
    for (int r = 1; r < 16; ++r) vu.regs.vc[r] = next() % ((next() & 1) ? 16 : 0x10000);
    vu.regs.vc[20] = 0x3F800000u | (next() & 0x7FFFFF);
    vu.regs.vc[21] = number();
    vu.regs.vc[22] = number();
    for (std::size_t b = 0; b < vu.data.size(); b += 4) {
        const u32 value = number();
        std::memcpy(vu.data.data() + b, &value, 4);
    }
    vu.mac = next() & 0xFFFF;
    vu.status = next() & 0xFFF;
    vu.clip = next() & 0xFFFFFF;
    vu.p = number();
}

void VuCompiledIsTheInterpreter() {
    std::size_t count = 0;
    const ps2host::VuCompiled* programs = VuGeneratedPrograms(&count);
    u32 seed = 77;
    u32 wrong = 0, runs = 0, entered = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const ps2host::VuCompiled& program = programs[i];
        if (!program.block_count) continue;
        for (int trial = 0; trial < 6; ++trial) {
            auto interpreted = std::make_unique<ps2host::Vu>(program.unit);
            auto ahead = std::make_unique<ps2host::Vu>(program.unit);
            const u32 start = seed;
            for (ps2host::Vu* vu : {interpreted.get(), ahead.get()}) {
                seed = start;
                PrepareVuRun(*vu, program, seed);
            }
            interpreted->use_compiled = false;
            const u64 limit = 3000;
            const u64 a = interpreted->Run(program.entry, limit), b = ahead->Run(program.entry, limit);
            ++runs;
            if (ahead->compiled_pairs()) ++entered;
            if (a != b || !interpreted->SameState(*ahead)) {
                ++wrong;
                if (std::getenv("PS2HOST_VU_DIFF"))
                    std::printf("vu diff: program %zu (entry %04X) trial %d: %llu vs %llu pairs; %s\n", i,
                                unsigned(program.entry), trial, (unsigned long long)a, (unsigned long long)b,
                                interpreted->StateDifference(*ahead).c_str());
            }
        }
    }
    char what[128];
    std::snprintf(what, sizeof what, "VU programs compiled ahead run as interpreted (%u of %u runs differ)", wrong, runs);
    Check(wrong == 0 && runs > 0, what);
    // (A jump to a register's address mostly leaves for the NOPs around: the
    // interpreter goes on there.)
    std::snprintf(what, sizeof what, "VU compiled programs taken where they are (%u of %u runs)", entered, runs);
    Check(entered == runs, what);
}

/*
 * Fibers (ps2plat: Windows' own; on ARM a switch of our own; ucontext
 * elsewhere), as the EE's threads use them: two fibers taking turns with the
 * host's, doubles kept in callee-saved registers across every switch, an
 * exception thrown and caught inside one, a deep stack in the other. And
 * what a switch costs, for the record.
 */
namespace fiber_test {
ps2plat::Fiber *host = nullptr, *a = nullptr, *b = nullptr;
int turns = 0;
bool caught = false;
double kept = 0.0;
int deepest = 0;
constexpr int kTurns = 1000;

double Step(double x) { return x * 1.0000001 + 0.5; }

int Deep(int depth) {
    volatile char frame[256];
    frame[depth & 255] = char(depth);
    return depth == 0 ? frame[0] : Deep(depth - 1) + 1;
}

void A(void*) {
    double x = 1.5, y = -2.25;
    for (int i = 0; i < kTurns; ++i) {
        x = Step(x);
        y = Step(y);
        ps2plat::SwitchTo(b);
    }
    try {
        throw std::runtime_error("thrown in a fiber");
    } catch (const std::exception&) {
        caught = true;
    }
    kept = x + y;
    ps2plat::SwitchTo(host);            // never resumed
}

void B(void*) {
    deepest = Deep(1000);               // some 300 KB of the 512 KB stack
    for (;;) {
        ++turns;
        ps2plat::SwitchTo(a);
    }
}

ps2plat::Fiber* pong = nullptr;
void Pong(void*) {
    for (;;) ps2plat::SwitchTo(host);
}
}  // namespace fiber_test

void FibersSwitchAndUnwind() {
    using namespace fiber_test;
    host = ps2plat::ThreadToFiber();
    a = ps2plat::NewFiber(512 << 10, A, nullptr);
    b = ps2plat::NewFiber(512 << 10, B, nullptr);
    ps2plat::SwitchTo(a);
    double x = 1.5, y = -2.25;
    for (int i = 0; i < kTurns; ++i) {
        x = Step(x);
        y = Step(y);
    }
    Check(turns == kTurns && kept == x + y, "fibers take turns, doubles kept across switches");
    Check(caught, "fibers: an exception thrown and caught inside one");
    Check(deepest == 1000, "fibers: a deep stack");
    ps2plat::FreeFiber(a);
    ps2plat::FreeFiber(b);
    pong = ps2plat::NewFiber(64 << 10, Pong, nullptr);
    const auto began = std::chrono::steady_clock::now();
    constexpr int kSwitches = 200000;
    for (int i = 0; i < kSwitches / 2; ++i) ps2plat::SwitchTo(pong);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - began).count();
    std::printf("fibers: %.0f ns a switch\n", ns / kSwitches);
    ps2plat::FreeFiber(pong);
}

/*
 * The same arithmetic on every processor. The port runs one translation of
 * the game everywhere, so the EE's FPU, VU0's macro mode, the VU programs and
 * the MMI must leave the same bits on ARM (the Vita: NEON and VFP lanes,
 * FPSCR's rounding) as on the PC (x86-64, SSE) - not just agree with
 * themselves. Each part is hashed over random inputs and compared with the
 * hash a PC build gave (kPc*; ARM builds print theirs). The VU corpus is
 * vurecomp's, made by the build's Python: its programs are hashed too, and
 * compared only when they are the ones the PC hash was made from.
 */
struct Fnv {
    u64 h = 0xCBF29CE484222325ull;
    void Add(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    }
    template <typename T>
    void Of(const T& value) { Add(&value, sizeof value); }
};

void ArithmeticIsThePcs() {
    // Recorded from the x86-64 build (GCC 13, SSE; MSVC's is the same SSE).
    constexpr u64 kPcCorpus = 0xC55468E84D825446ull, kPcVu = 0xC0F321C949A5192Dull,
                  kPcMacro = 0xDCE9557991444CB5ull, kPcFpu = 0xBAA155C57922E6B5ull, kPcMmi = 0x503D33F3D142B379ull;
    u32 seed = 4321;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    auto value = [&]() -> u32 {
        const u32 r = next(), sign = r & 0x80000000u;
        switch ((r >> 8) % 8) {
            case 0: return sign;
            case 1: return sign | (r & 0x007FFFFFu);                        // denormal
            case 2: return sign | 0x7F800000u | ((r >> 4) & 1);             // infinity, NaN
            case 3: return sign | 0x7F000000u | (next() & 0x007FFFFFu);     // near the top
            case 4: return sign | 0x00800000u | (next() & 0x00FFFFFFu);     // near the bottom
            case 5: return sign | 0x3F800000u | (next() & 0x007FFFFFu);     // about one
            default: return next();
        }
    };

    // The VU corpus, interpreted and compiled, from the same states as
    // VuCompiledIsTheInterpreter's.
    Fnv corpus, vu;
    std::size_t count = 0;
    const ps2host::VuCompiled* programs = VuGeneratedPrograms(&count);
    u32 vu_seed = 77;
    for (std::size_t i = 0; i < count; ++i) {
        const ps2host::VuCompiled& program = programs[i];
        const u32* code = program.code;
        const u32 runs_in = *code;
        std::size_t words = 1;
        for (u32 r = 0, at = 1; r < runs_in; ++r) {
            words += 2 + code[at + 1];
            at += 2 + code[at + 1];
        }
        corpus.Add(code, words * 4);
        if (!program.block_count) continue;
        for (int trial = 0; trial < 6; ++trial) {
            for (bool compiled : {false, true}) {
                auto unit = std::make_unique<ps2host::Vu>(program.unit);
                u32 s = vu_seed;
                PrepareVuRun(*unit, program, s);
                unit->use_compiled = compiled;
                const u64 pairs = unit->Run(program.entry, 3000);
                vu.Of(pairs);
                vu.Add(unit->regs.vf, sizeof unit->regs.vf);
                vu.Add(unit->regs.vc, sizeof unit->regs.vc);
                vu.Add(unit->regs.vacc, sizeof unit->regs.vacc);
                vu.Of(unit->mac);
                vu.Of(unit->status);
                vu.Of(unit->clip);
                vu.Of(unit->p);
                vu.Add(unit->data.data(), unit->data.size());
                if (compiled) vu_seed = s;
            }
        }
    }

    // Macro mode's FMAC (Cop2: VU0 from EE code, rounded as the game runs
    // it): the broadcast forms of ADD, SUB, MADD, MSUB, MAX, MINI, MUL.
    Fnv macro;
    {
        auto c = std::make_unique<lifted::Context>();
        for (int i = 0; i < 200000; ++i) {
            if ((i & 255) == 0) {
                for (auto& v : c->vf) for (u32& lane : v) lane = value();
                for (u32& lane : c->vacc) lane = value();
            }
            const u32 funct = next() % 0x1C, dest = 1 + next() % 15;
            const u32 ft = next() % 32, fs = next() % 32, fd = 1 + next() % 31;
            lifted::Cop2(*c, 0x4A000000u | dest << 21 | ft << 16 | fs << 11 | fd << 6 | funct);
            if ((i & 255) == 255) {
                macro.Add(c->vf, sizeof c->vf);
                macro.Add(c->vacc, sizeof c->vacc);
            }
        }
    }

    // The EE's FPU (eefpu.cpp: computed in double, saturated, toward zero).
    Fnv fpu;
    Fnv each[10];           // by operation too: which one, when the whole differs (the Vita's did)
    for (int i = 0; i < 300000; ++i) {
        const u32 a = value(), b = value(), acc = value();
        const u32 results[10] = {generated::FloatAdd(a, b),          generated::FloatSub(a, b),
                                 generated::FloatMul(a, b),          generated::FloatDiv(a, b),
                                 generated::FloatSqrt(a),            generated::FloatMulAdd(acc, a, b),
                                 generated::FloatMulSub(acc, a, b),  generated::IntFromFloat(a),
                                 generated::FloatFromInt(b),         lifted::FloatRsqrt(a, b)};
        for (int op = 0; op < 10; ++op) {
            fpu.Of(results[op]);
            each[op].Of(results[op]);
        }
    }
    std::printf("arithmetic: the FPU's operations: add %04X sub %04X mul %04X div %04X sqrt %04X madd %04X msub %04X "
                "cvt.w %04X cvt.s %04X rsqrt %04X\n",
                unsigned(each[0].h & 0xFFFF), unsigned(each[1].h & 0xFFFF), unsigned(each[2].h & 0xFFFF),
                unsigned(each[3].h & 0xFFFF), unsigned(each[4].h & 0xFFFF), unsigned(each[5].h & 0xFFFF),
                unsigned(each[6].h & 0xFFFF), unsigned(each[7].h & 0xFFFF), unsigned(each[8].h & 0xFFFF),
                unsigned(each[9].h & 0xFFFF));
#if defined(__arm__)
    {
        unsigned fpscr = 0;
        __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
        std::printf("arithmetic: FPSCR %08X (rounding %u, flush to zero %u, default NaN %u)\n", fpscr,
                    (fpscr >> 22) & 3, (fpscr >> 24) & 1, (fpscr >> 25) & 1);
    }
#endif

    // The MMI's lanes.
    Fnv mmi;
    for (int i = 0; i < 100000; ++i) {
        const u64 alo = u64(next()) << 32 | next(), ahi = u64(next()) << 32 | next();
        const u64 blo = u64(next()) << 32 | next(), bhi = u64(next()) << 32 | next();
        for (int op = lifted::kPmaxw; op <= lifted::kPminh; ++op) {
            u64 lo = 0, hi = 0;
            lifted::Lanes(op, lo, hi, alo, ahi, blo, bhi);
            mmi.Of(lo);
            mmi.Of(hi);
        }
        u64 lo = 0, hi = 0;
        lifted::Interleave(lo, hi, alo, blo, 8u << (i % 3));
        mmi.Of(lo);
        mmi.Of(hi);
    }

    std::printf("arithmetic: corpus %016llX vu %016llX macro %016llX fpu %016llX mmi %016llX\n",
                (unsigned long long)corpus.h, (unsigned long long)vu.h, (unsigned long long)macro.h,
                (unsigned long long)fpu.h, (unsigned long long)mmi.h);
    if (kPcCorpus == 0) return;     // not recorded yet
    if (corpus.h == kPcCorpus) Check(vu.h == kPcVu, "the VU corpus runs as on the PC, to the bit");
    else std::printf("arithmetic: the VU corpus is not the one the PC's hash was made from: not compared\n");
    Check(macro.h == kPcMacro, "macro mode's FMAC as on the PC, to the bit");
    Check(fpu.h == kPcFpu, "the EE's FPU as on the PC, to the bit");
    Check(mmi.h == kPcMmi, "the MMI's lanes as on the PC, to the bit");
}

// --vu-fast: the corpus's VU1 programs that never read MAC or STATUS
// (vurecomp's fast_eligible) run their flag-free blocks, and end as the
// exact blocks do in everything else - registers, memory, CLIP, Q and P,
// every pipeline and cycle - and in the pairs counted.
void VuFastIsTheExactButFlags() {
    std::size_t count = 0;
    const ps2host::VuCompiled* programs = VuGeneratedPrograms(&count);
    u32 seed = 991;
    u32 wrong = 0, runs = 0, flags_differ = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const ps2host::VuCompiled& program = programs[i];
        if (!program.block_count || !program.fast_blocks) continue;
        for (int trial = 0; trial < 20; ++trial) {
            auto exact = std::make_unique<ps2host::Vu>(program.unit);
            auto fast = std::make_unique<ps2host::Vu>(program.unit);
            const u32 start = seed;
            for (ps2host::Vu* vu : {exact.get(), fast.get()}) {
                seed = start;
                PrepareVuRun(*vu, program, seed);
            }
            fast->fast = true;
            const u64 limit = 3000;
            const u64 a = exact->Run(program.entry, limit), b = fast->Run(program.entry, limit);
            ++runs;
            if (a != b || !exact->SameState(*fast, false) || !fast->compiled_pairs()) {
                ++wrong;
                std::fprintf(stderr, "[fast] program %llu entry %X trial %d: pairs %llu %llu, compiled %llu\n",
                             (unsigned long long)i,
                             program.entry, trial, (unsigned long long)a, (unsigned long long)b,
                             (unsigned long long)fast->compiled_pairs());
                for (int r = 0; r < 32; ++r)
                    for (int l = 0; l < 4; ++l)
                        if (exact->regs.vf[r][l] != fast->regs.vf[r][l]) std::fprintf(stderr, "  vf%d.%d\n", r, l);
                for (int r = 0; r < 32; ++r)
                    if (exact->regs.vc[r] != fast->regs.vc[r])
                        std::fprintf(stderr, "  vc%d %X %X\n", r, exact->regs.vc[r], fast->regs.vc[r]);
                if (exact->clip != fast->clip) std::fprintf(stderr, "  clip\n");
                if (std::memcmp(exact->data.data(), fast->data.data(), exact->data.size())) std::fprintf(stderr, "  data\n");
            }
            if (!exact->SameState(*fast)) ++flags_differ;
        }
    }
    char what[160];
    std::snprintf(what, sizeof what, "VU fast blocks run as the exact ones but for MAC and STATUS (%u of %u runs differ)",
                  wrong, runs);
    Check(wrong == 0 && runs >= 100, what);
    // The fast blocks do leave the flags alone (else they were not taken).
    // (A fifth of the runs: since mac_live counts from the first later FMAC,
    // programs that drop only a little have fast blocks too, and many of
    // those end with the flags their last FMAC would have left anyway.)
    std::snprintf(what, sizeof what, "VU fast blocks taken: the flags differ in %u of %u runs", flags_differ, runs);
    Check(flags_differ > runs / 8, what);
}

void VuTimingIsVisible() {
    ps2host::Vu vu(0);
    auto f = [](float x) { std::uint32_t u; std::memcpy(&u, &x, 4); return u; };
    const std::uint32_t kNopU = 0x000002FF, kNopL = 0x8000033C, kI = 0x80000000, kEnd = 0x40000000;
    const std::uint32_t program[] = {
        0x100200FF, 0x0100006C,         // iaddiu vi2, vi0, 0xFF    sub.x vf1, vf0, vf0 (zero: MAC Zx)
        0x34011000, kNopU,              // fmand vi1, vi2: the flags from before
        0x81E003BC, kNopU,              // div Q, vf0w, vf0w
        kNopL, 0x002000DC,              // mulq.w vf3, vf0, Q: the Q from before
        0x34031000, kNopU,              // fmand vi3, vi2: four cycles on, the sub's
        0x800003BF, kNopU,              // waitq
        kNopL, 0x0020011C,              // mulq.w vf4, vf0, Q
        f(3), kI | kNopU,               // loi 3
        f(5), kI | 0x0020015E,          // loi 5                    muli.w vf5, vf0, I
        kNopL, kEnd | 0x0020019E,       //                          muli.w vf6, vf0, I [E]
        kNopL, kNopU};
    std::memcpy(vu.micro.data(), program, sizeof program);
    vu.Run(0);
    Check(vu.regs.vc[1] == 0 && vu.regs.vc[3] == 8, "VU: FMAND sees the MAC flags four cycles late");
    Check(vu.regs.vf[3][3] == 0 && vu.regs.vf[4][3] == f(1), "VU: Q arrives after DIV's latency; WAITQ waits");
    Check(vu.regs.vf[5][3] == f(3) && vu.regs.vf[6][3] == f(5), "VU: an upper beside LOI reads the old I");
}

// An upper operation into VF0 writes nowhere, but its flags are its result's:
// Burnout 3 tests a box against planes with `msubz.xyzw vf00, ...` and FMAND,
// and with VF0's 0, 0, 0, 1 read back instead culled and clipped nothing.
void VuFlagsOfAResultIntoVf0() {
    ps2host::Vu vu(0);
    auto f = [](float x) { std::uint32_t u; std::memcpy(&u, &x, 4); return u; };
    const std::uint32_t kNopU = 0x000002FF, kNopL = 0x8000033C, kEnd = 0x40000000;
    const std::uint32_t program[] = {
        0x100200FF, 0x01E1002C,         // iaddiu vi2, vi0, 0xFF    sub.xyzw vf0, vf0, vf1
        kNopL, kNopU,
        kNopL, kNopU,
        kNopL, kNopU,
        0x34011000, kNopU,              // fmand vi1, vi2: the sub's flags
        kNopL, kEnd | kNopU,
        kNopL, kNopU};
    std::memcpy(vu.micro.data(), program, sizeof program);
    const std::uint32_t vf1[4] = {f(1), f(-2), f(0), f(3)};
    std::memcpy(vu.regs.vf[1], vf1, sizeof vf1);
    vu.Run(0);
    // 0 - (1, -2, 0, 3): x and w negative (MAC 0x80, 0x10), z zero (0x02).
    Check(vu.regs.vc[1] == 0x92, "VU: an upper into VF0 sets the flags of its result");
    Check(vu.regs.vf[0][0] == 0 && vu.regs.vf[0][2] == 0 && vu.regs.vf[0][3] == f(1), "VU: VF0 stays 0, 0, 0, 1");
}

// The IPU: FDEC and VDEC on known bits, and BDEC on two intra macroblocks of
// flat blocks (DC only), the second split over the input FIFO's quadwords
// so that it runs out of data after its first block's DC - retried, the
// DC predictor must not have moved (a retry once added the luma DC again
// each time, and a movie's slice went white from there).
void IpuDecodesAndRetriesCleanly() {
    ps2host::InstallIpu();
    ps2host::Memory& m = ps2host::GuestMemory();
    auto command = [&](u32 word) { m.Write(0x10002000u, word, 4); };
    auto busy = [&] { return (m.Read(0x10002000u, 8) >> 63) & 1; };
    std::vector<u8> bytes;
    auto pack = [&](const std::string& bits) {      // MSB first, as the stream is read
        bytes.assign((bits.size() + 7) / 8, 0);
        for (std::size_t i = 0; i < bits.size(); ++i)
            if (bits[i] == '1') bytes[i / 8] |= u8(0x80 >> (i % 8));
        bytes.resize((bytes.size() + 15) / 16 * 16, 0);
    };
    auto push = [&](std::size_t quadword) {
        for (u32 w = 0; w < 4; ++w) {
            u32 word = 0;
            std::memcpy(&word, &bytes[quadword * 16 + w * 4], 4);
            m.Write(0x10007010u + 4 * w, word, 4);
        }
    };
    auto first_luma = [&] {                         // the macroblock's Y[0][0], the rest drained
        const u64 q = m.Read(0x10007000u, 8);
        m.Read(0x10007008u, 8);
        for (int i = 1; i < 48; ++i) { m.Read(0x10007000u, 8); m.Read(0x10007008u, 8); }
        return s16(q & 0xFFFF);
    };
    m.Write(0x10002010u, 1u << 30, 4);              // reset; MPEG-2, IDP 0, zig-zag, B.14
    // FDEC and VDEC: 0x000001B3, then a 1-bit address increment of 1.
    pack("00000000000000000000000110110011" "1");
    command(0x00000000u);                           // BCLR, BP 0
    push(0);
    command(0x40000000u);                           // FDEC
    Check(!busy() && u32(m.Read(0x10002000u, 4)) == 0x000001B3u, "IPU: FDEC reads 32 bits");
    command(0x40000000u | 32);                      // FDEC past them
    command(0x30000000u);                           // VDEC, address increment
    Check(u32(m.Read(0x10002000u, 4)) == 0x00010001u, "IPU: VDEC gives the increment and its length");
    // Two macroblocks: luma blocks "100" (DC size 0) "10" (end of block),
    // chroma "00" "10"; the second's first luma DC "00" "1" (+1).
    const std::string flat = std::string("10010") + "10010" + "10010" + "10010" + "0010" + "0010";
    const std::string plus = std::string("00110") + "10010" + "10010" + "10010" + "0010" + "0010";
    // The first at bit 210, the second at 238: its first DC in the second
    // quadword, the end-of-block code after it running into the third.
    // Then the slice's stuffing to the byte and a start code (SCD).
    pack(std::string(210, '0') + flat + plus + "000000" + "00000000" "00000000" "00000001" "10110011");
    command(0x0000007Fu);                           // BCLR, BP 127
    push(0);
    push(1);
    command(0x40000000u | 63);                      // FDEC past 63 more: bit 190
    command(0x2C000000u | 20);                      // BDEC past 20: intra, DC reset, QSC 0
    Check(!busy() && first_luma() == 128, "IPU: BDEC of a flat intra block");
    command(0x28000000u);                           // BDEC: intra
    for (int i = 0; i < 5; ++i) busy();             // tried, and tried again, short of data
    Check(busy(), "IPU: BDEC waits for data");
    push(2);
    Check(!busy() && first_luma() == 129, "IPU: a retried BDEC keeps its DC predictor");
    Check(((m.Read(0x10002010u, 4) >> 15) & 1) && u32(m.Read(0x10002030u, 4)) == 0x000001B3u,
          "IPU: BDEC goes over a slice's stuffing to the start code (SCD)");
}

/* The IPU fed and drained through its channels as libmpeg does it (4 from
   memory, 3 to memory): intra macroblocks with DC differentials and a few
   AC coefficients through BDEC, then CSC over raw macroblocks - the bytes
   out the same as they were before the IPU's channels and FIFOs were made
   cheaper (hashes recorded from that build), and each command's time
   (IpuMacroblockTimes, the benchmark). */
constexpr u64 kIpuBdecHash = 0x5CEA2D1CA138E325ull, kIpuCscHash = 0x512F46001937988Cull;
struct IpuRun {
    u64 bdec_hash = 0, csc_hash = 0;
    double bdec_ns = 0, csc_ns = 0;     // a macroblock
};
IpuRun RunIpuThroughChannels(u32 macroblocks) {
    ps2host::InstallIpu();
    ps2host::Memory& m = ps2host::GuestMemory();
    u32 seed = 77;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    std::string bits;
    auto put = [&](u32 value, u32 count) { for (u32 i = count; i-- > 0;) bits += ((value >> i) & 1) ? '1' : '0'; };
    // B.12 (luminance) and B.13 (chrominance) DC sizes 0-3; B.14's codes.
    const char* const kLumaSize[4] = {"100", "00", "01", "101"};
    const char* const kChromaSize[4] = {"00", "01", "10", "110"};
    for (u32 mb = 0; mb < macroblocks; ++mb)
        for (int b = 0; b < 6; ++b) {
            const u32 size = next() >> 30;
            bits += b < 4 ? kLumaSize[size] : kChromaSize[size];
            if (size) put(next() >> (32 - size), size);
            for (u32 k = next() >> 30; k-- > 0;) {        // up to three: (0,1) "11s", (1,1) "011s", (0,2) "0100s"
                static const char* const kCodes[3] = {"11", "011", "0100"};
                bits += kCodes[(next() >> 8) % 3];
                put(next() >> 31, 1);
            }
            bits += "10";                                   // end of block
        }
    bits += std::string(8 - bits.size() % 8, '0') + "00000000" "00000000" "00000001" "10110011";
    std::vector<u8> stream((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        if (bits[i] == '1') stream[i / 8] |= u8(0x80 >> (i % 8));
    stream.resize((stream.size() + 15) / 16 * 16 + 64, 0);
    const u32 kStream = 0x00400000u, kRaw16 = 0x00800000u, kRaw8 = 0x00A00000u, kRgb = 0x00C00000u;
    for (std::size_t i = 0; i < stream.size(); ++i) m.Write(kStream + u32(i), stream[i], 1);
    auto busy = [&] { return (m.Read(0x10002010u, 4) >> 31) & 1; };
    IpuRun out;
    using Clock = std::chrono::steady_clock;
    // BDEC: intra, DC reset on the first, QSC 8; MPEG-2, IDP 0, B.14, zig-zag, an I picture.
    m.Write(0x10002010u, 1u << 30, 4);
    m.Write(0x10002010u, 1u << 24, 4);
    m.Write(0x10002000u, 0x00000000u, 4);                   // BCLR
    m.Write(0x1000B410u, kStream, 4);
    m.Write(0x1000B420u, u32(stream.size() / 16), 4);
    m.Write(0x1000B400u, 0x101u, 4);                        // channel 4: normal, from memory
    m.Write(0x1000B010u, kRaw16, 4);
    m.Write(0x1000B020u, macroblocks * 48, 4);
    m.Write(0x1000B000u, 0x100u, 4);                        // channel 3
    auto began = Clock::now();
    for (u32 mb = 0; mb < macroblocks; ++mb) {
        m.Write(0x10002000u, 0x28000000u | (mb == 0 ? 1u << 26 : 0u) | 8u << 16, 4);
        for (int guard = 0; busy() && guard < 100; ++guard) {}
    }
    out.bdec_ns = std::chrono::duration<double, std::nano>(Clock::now() - began).count() / macroblocks;
    Fnv bdec;
    for (u32 i = 0; i < macroblocks * 768; i += 4) bdec.Of(u32(m.Read(kRaw16 + i, 4)));
    out.bdec_hash = bdec.h;
    // CSC: raw macroblocks (Y 256, Cb 64, Cr 64), RGB32, no dither.
    for (u32 i = 0; i < macroblocks * 384; i += 4) m.Write(kRaw8 + i, next(), 4);
    m.Write(0x10002000u, 0x00000000u, 4);                   // BCLR
    m.Write(0x1000B410u, kRaw8, 4);
    m.Write(0x1000B420u, macroblocks * 24, 4);
    m.Write(0x1000B400u, 0x101u, 4);
    m.Write(0x1000B010u, kRgb, 4);
    m.Write(0x1000B020u, macroblocks * 64, 4);
    m.Write(0x1000B000u, 0x100u, 4);
    began = Clock::now();
    m.Write(0x10002000u, 0x70000000u | macroblocks, 4);
    for (int guard = 0; busy() && guard < 100; ++guard) {}
    out.csc_ns = std::chrono::duration<double, std::nano>(Clock::now() - began).count() / macroblocks;
    Fnv csc;
    for (u32 i = 0; i < macroblocks * 1024; i += 4) csc.Of(u32(m.Read(kRgb + i, 4)));
    out.csc_hash = csc.h;
    return out;
}

void IpuStreamsThroughItsChannels() {
    const IpuRun run = RunIpuThroughChannels(40);
    char what[160];
    std::snprintf(what, sizeof what, "IPU: BDEC through channels 4 and 3 gives what it did (%016llX)",
                  (unsigned long long)run.bdec_hash);
    Check(run.bdec_hash == kIpuBdecHash, what);
    std::snprintf(what, sizeof what, "IPU: CSC through channels 4 and 3 gives what it did (%016llX)",
                  (unsigned long long)run.csc_hash);
    Check(run.csc_hash == kIpuCscHash, what);
}

// MAX and MINI hand back an operand as it is: Burnout 3's car microcode
// builds its GIF tag's REGS (0x412, a denormal as a float) with LOI and MAXI;
// cleaned to 0, every vertex went to PRIM and no car was drawn.
// The MMI lane operations and PEXTL* in SSE2 (lifted_support.cpp) give
// what the lane-by-lane model gives, over random values and lanes at their
// limits (0, 1, the signed extremes, all ones).
}  // namespace
namespace lifted {     // (lifted_support.cpp's, the model NEON's forms are checked against)
void AddBytesScalar(u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi, bool subtract);
void ShiftHalfwordsRightScalar(u64& lo, u64& hi, u64 blo, u64 bhi, u32 amount);
void ShiftLanesScalar(u64& lo, u64& hi, u64 blo, u64 bhi, u32 width, u32 kind, u32 amount);
void PackHalfwordsScalar(u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi);
void PackBytesScalar(u64& lo, u64& hi, u64 alo, u64 ahi, u64 blo, u64 bhi);
}  // namespace lifted
namespace {

void MmiLanesAreScalar() {
    u64 seed = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    static const u64 kEdges[] = {0, 1, 0x7F7F7F7F7F7F7F7Full, 0x8080808080808080ull, 0x7FFF7FFF7FFF7FFFull,
                                 0x8000800080008000ull, 0x7FFFFFFF7FFFFFFFull, 0x8000000080000000ull, ~0ull,
                                 0xFF00FF00FF00FF00ull};
    auto value = [&]() -> u64 {
        const u64 r = next();
        if (r % 3 == 0) return kEdges[(r >> 8) % (sizeof kEdges / sizeof kEdges[0])];
        if (r % 3 == 1) return next() & next();     // more zero lanes
        return next();
    };
    u32 wrong = 0, checked = 0;
    for (int op = lifted::kPmaxw; op <= lifted::kPminh; ++op)
        for (int trial = 0; trial < 4000; ++trial) {
            const u64 alo = value(), ahi = value(), blo = trial & 1 ? alo : value(), bhi = value();
            u64 lo1, hi1, lo2, hi2;
            lifted::Lanes(op, lo1, hi1, alo, ahi, blo, bhi);
            lifted::LanesScalar(op, lo2, hi2, alo, ahi, blo, bhi);
            ++checked;
            if (lo1 != lo2 || hi1 != hi2) ++wrong;
        }
    for (u32 width : {8u, 16u, 32u})
        for (int trial = 0; trial < 4000; ++trial) {
            const u64 a = value(), b = value();
            u64 lo1, hi1, lo2, hi2;
            lifted::Interleave(lo1, hi1, a, b, width);
            lifted::InterleaveScalar(lo2, hi2, a, b, width);
            ++checked;
            if (lo1 != lo2 || hi1 != hi2) ++wrong;
        }
    // The byte adds, packs and shifts (NEON's on ARM) against their lane-by-lane forms.
    for (int trial = 0; trial < 4000; ++trial) {
        const u64 alo = value(), ahi = value(), blo = value(), bhi = value();
        const u32 amount = u32(next() % 64);
        u64 lo1, hi1, lo2, hi2;
        auto same = [&] {
            ++checked;
            if (lo1 != lo2 || hi1 != hi2) ++wrong;
        };
        for (bool subtract : {false, true}) {
            lifted::AddBytes(lo1, hi1, alo, ahi, blo, bhi, subtract);
            lifted::AddBytesScalar(lo2, hi2, alo, ahi, blo, bhi, subtract);
            same();
        }
        lifted::ShiftHalfwordsRight(lo1, hi1, blo, bhi, amount);
        lifted::ShiftHalfwordsRightScalar(lo2, hi2, blo, bhi, amount);
        same();
        for (u32 width : {16u, 32u})
            for (u32 kind : {0u, 1u, 2u}) {
                lifted::ShiftLanes(lo1, hi1, blo, bhi, width, kind, amount);
                lifted::ShiftLanesScalar(lo2, hi2, blo, bhi, width, kind, amount);
                same();
            }
        lifted::PackHalfwords(lo1, hi1, alo, ahi, blo, bhi);
        lifted::PackHalfwordsScalar(lo2, hi2, alo, ahi, blo, bhi);
        same();
        lifted::PackBytes(lo1, hi1, alo, ahi, blo, bhi);
        lifted::PackBytesScalar(lo2, hi2, alo, ahi, blo, bhi);
        same();
    }
    char what[128];
    std::snprintf(what, sizeof what, "MMI lanes in SSE2 or NEON are the model's (%u of %u differ)", wrong, checked);
    Check(wrong == 0, what);
}

void VuMaxKeepsADenormal() {
    ps2host::Vu vu(0);
    const std::uint32_t kNopU = 0x000002FF, kNopL = 0x8000033C, kI = 0x80000000, kEnd = 0x40000000;
    const std::uint32_t program[] = {
        0x00000412, kI | kNopU,         // loi 0x412
        kNopL, 0x0040005D,              //                          maxi.z vf1, vf0, I
        0x00000412, kI | kNopU,         // loi 0x412
        kNopL, 0x0040009F,              //                          minii.z vf2, vf0, I
        kNopL, kEnd | kNopU,
        kNopL, kNopU};
    std::memcpy(vu.micro.data(), program, sizeof program);
    vu.Run(0);
    Check(vu.regs.vf[1][2] == 0x412 && vu.regs.vf[2][2] == 0, "VU: MAXI and MINII compare and keep the bits");
}

// The IOP interpreter against emu.py, the verified R5900 interpreter: one MIPS I
// instruction per vector (engine/tools/iop_vectors.py).
struct CpuVector {
    const char* name;
    u32 word;
    u32 regs[32];
    u8 data[64];
    u32 out[32];
    u32 hi, lo;
    u8 data_out[64];
};

const CpuVector kCpuVectors[] = {
#include "iop_cpu_vectors.inc"
};

void IopInstructionsMatchTheR5900() {
    using namespace ps2host::iop;
    Vm& vm = TheVm();
    int failures = 0;
    for (const CpuVector& v : kCpuVectors) {
        vm.Write(0x00100000u, v.word, 4);
        for (u32 i = 0; i < 64; ++i) vm.Write(0x00100100u + i, v.data[i], 1);
        Context c;
        for (u32 i = 1; i < 32; ++i) c.r[i] = v.regs[i];
        c.hi = v.regs[0];
        c.lo = v.regs[0] ^ 0x5A5A5A5Au;
        c.pc = 0x00100000u;
        c.npc = 0x00100004u;
        vm.Step(c, 1);
        bool ok = c.hi == v.hi && c.lo == v.lo;
        for (u32 i = 1; i < 32; ++i) ok = ok && c.r[i] == v.out[i];
        for (u32 i = 0; i < 64; ++i) ok = ok && vm.Read(0x00100100u + i, 1) == v.data_out[i];
        if (!ok && failures++ < 5) {
            char what[96];
            std::snprintf(what, sizeof what, "IOP %s (0x%08X) matches the R5900", v.name, unsigned(v.word));
            Check(false, what);
        }
    }
    Check(failures == 0, "every IOP instruction vector matches");
}

}  // namespace

/* The SPU2 on its own thread mixes each voice over a whole call at once
   (SetBatchVoices): the same sound, sample for sample, as a sample at a time
   across the voices - with every voice's ways through it (sweeps, the
   envelope's phases, key-offs and key-ons between calls, blocks' loop and
   end flags, pitches up to four times, both cores' reverb, MMIX, an
   interrupt in a voice's blocks and one in a capture ring, re-armed as the
   driver does), SPU RAM the same after, and the same interrupts each call. */
void Spu2MixesVoicesInBatchesOnce(bool parked) {
    using ps2host::iop::Spu2;
    u32 seed = parked ? 3141 : 2718;
    auto next = [&] { return seed = seed * 1664525u + 1013904223u; };
    auto one = std::make_unique<Spu2>(), batch = std::make_unique<Spu2>();
    batch->SetBatchVoices(true);
    std::vector<Spu2::s16> out_one, out_batch;
    one->SetOutput([&](const Spu2::s16* f, Spu2::u32 n) { out_one.insert(out_one.end(), f, f + 2 * n); });
    batch->SetOutput([&](const Spu2::s16* f, Spu2::u32 n) { out_batch.insert(out_batch.end(), f, f + 2 * n); });
    auto both = [&](auto&& act) { act(*one); act(*batch); };
    // Blocks: random filters, shifts and nibbles; flags 0, loop start,
    // end + repeat, end alone.
    std::vector<Spu2::u8> blocks(0x10000);
    for (std::size_t b = 0; b < blocks.size(); b += 16) {
        blocks[b] = Spu2::u8(((next() >> 8) % 5) << 4 | (next() >> 8) % 13);
        const u32 r = (next() >> 8) % 16;
        blocks[b + 1] = Spu2::u8(r < 10 ? 0 : r < 12 ? 4 : r < 14 ? 3 : 1);
        for (int i = 2; i < 16; ++i) blocks[b + i] = Spu2::u8(next() >> 24);
    }
    both([&](Spu2& spu) { spu.Write(0x10000, blocks.data(), Spu2::u32(blocks.size())); });
    for (u32 core = 0; core < 2; ++core) {
        const u32 mode = 1 + (next() >> 8) % 9;
        both([&](Spu2& spu) {
            spu.SetEffectAttr(core, mode, 0x3000, 0x2800, 0x40, 0x30);
            spu.SetCoreAttr(0x02 | core, 1);
            spu.SetParam(0x0B00 | core, 0x2000);
            spu.SetParam(0x0C00 | core, 0x1800);
            spu.SetParam(0x0800 | core, core ? 0xFFF : 0xFF0);
        });
    }
    // Main volume: core 1's a sweep down.
    both([&](Spu2& spu) { spu.SetParam(0x0901, 0x8000 | 0x2000 | 0x30); });
    auto set_voice = [&](u32 core, u32 voice) {
        const u32 entry = core | voice << 1;
        const u32 ssa = 0x10000 + ((next() >> 8) % 4096) * 16;
        const u32 voll = (next() >> 8) % 3 == 0 ? (0x8000 | ((next() >> 8) & 0x7F7F)) : ((next() >> 8) & 0x3FFF);
        const u32 volr = (next() >> 8) % 3 == 0 ? (0x8000 | ((next() >> 8) & 0x7F7F)) : ((next() >> 8) & 0x3FFF);
        const u32 pitch = 0x200 + (next() >> 8) % 0x3E00;
        const u32 adsr1 = next() & 0xFFFF, adsr2 = next() & 0xFFFF;
        // (LSAX where it starts, as a driver's waveform has it: a voice
        // never wanders into the capture rings, which a batch writes later
        // in its call than a sample at a time does.)
        both([&](Spu2& spu) {
            spu.SetAddr(0x2000 | entry, ssa);
            spu.SetAddr(0x2100 | entry, ssa);
            // Parked where it would start, or (not) left at address 0 to
            // walk through the capture rings silent, a sample at a time.
            if (parked) spu.SetAddr(0x2200 | entry, ssa);
            spu.SetParam(0x0000 | entry, voll);
            spu.SetParam(0x0100 | entry, volr);
            spu.SetParam(0x0200 | entry, pitch);
            spu.SetParam(0x0300 | entry, adsr1);
            spu.SetParam(0x0400 | entry, adsr2);
        });
    };
    for (u32 core = 0; core < 2; ++core) {
        for (u32 voice = 0; voice < 24; ++voice) set_voice(core, voice);
        const u32 l = next() & 0xFFFFFF, r = next() & 0xFFFFFF, el = next() & 0xFFFFFF, er = next() & 0xFFFFFF;
        both([&](Spu2& spu) {
            spu.SetSwitch(0x1800 | core, l);
            spu.SetSwitch(0x1A00 | core, r);
            spu.SetSwitch(0x1900 | core, el);
            spu.SetSwitch(0x1B00 | core, er);
        });
    }
    // Interrupts: core 0's in the voices' blocks, core 1's in a capture ring.
    both([&](Spu2& spu) {
        spu.SetAddr(0x1F00, 0x10000 + 0x2340);
        spu.SetAddr(0x1F01, 0x2000 + 0x180);
        spu.SetCoreAttr(0x04, 1);
        spu.SetCoreAttr(0x05, 1);
    });
    u64 now = 1000;
    u32 raised_one = 0, raised_batch = 0, calls_differing = 0, interrupts = 0;
    std::vector<Spu2::u8> ram_one(0x6000), ram_batch(0x6000);
    u32 ram_differing = 0;
    for (int call = 0; call < 600; ++call) {
        if (call % 25 == 0) {                               // keys on and off, as a driver's frame might
            const u32 core = (next() >> 8) & 1, on = next() & 0xFFFFFF, off = next() & 0xFFFFFF & ~on;
            if (call % 75 == 0)
                for (u32 voice = 0; voice < 24; ++voice)
                    if (on >> voice & 1) set_voice(core, voice);
            both([&](Spu2& spu) {
                spu.SetSwitch(0x1600 | core, off);
                spu.SetSwitch(0x1500 | core, on);
            });
        }
        now += 500 + (next() >> 8) % 1500;
        raised_one = raised_batch = 0;
        const std::size_t before_out = out_one.size();
        one->Advance(now, [&](Spu2::u32 core) { raised_one |= 1u << core; });
        batch->Advance(now, [&](Spu2::u32 core) { raised_batch |= 1u << core; });
        if (std::getenv("PS2HOST_SPU_DEBUG"))
            for (std::size_t i = before_out; i < out_one.size() && i < out_batch.size(); ++i)
                if (out_one[i] != out_batch[i]) {
                    std::printf("spu debug: call %d, sample %zu of the call: %d vs %d\n", call, (i - before_out) / 2,
                                out_one[i], out_batch[i]);
                    break;
                }
        calls_differing += raised_one != raised_batch;
        for (u32 core = 0; core < 2; ++core)
            if (raised_one >> core & 1) {
                ++interrupts;
                both([&](Spu2& spu) { spu.SetCoreAttr(0x04 | core, 1); });
            }
        one->Read(0, ram_one.data(), 0x6000);
        batch->Read(0, ram_batch.data(), 0x6000);
        ram_differing += ram_one != ram_batch;
        if (std::getenv("PS2HOST_SPU_DEBUG") && call >= 520 && call <= 531) {
            std::vector<Spu2::u8> a(2u << 20), b(2u << 20);
            one->Read(0, a.data(), Spu2::u32(a.size()));
            batch->Read(0, b.data(), Spu2::u32(b.size()));
            for (std::size_t i = 0; i < a.size(); ++i)
                if (a[i] != b[i]) { std::printf("spu debug: call %d, RAM first differs at %06zX\n", call, i); break; }
        }
    }
    std::vector<Spu2::u8> all_one(2u << 20), all_batch(2u << 20);
    one->Read(0, all_one.data(), Spu2::u32(all_one.size()));
    batch->Read(0, all_batch.data(), Spu2::u32(all_batch.size()));
    u64 differing = out_one.size() != out_batch.size() ? ~0ull : 0;
    for (std::size_t i = 0; i < out_one.size() && i < out_batch.size(); ++i) differing += out_one[i] != out_batch[i];
    char what[192];
    std::snprintf(what, sizeof what, "SPU2: voices mixed a call at a time sound the same%s (%llu of %zu samples differ)",
                  parked ? "" : ", voices walking through the rings", (unsigned long long)differing, out_one.size());
    Check(differing == 0 && !out_one.empty(), what);
    std::snprintf(what, sizeof what, "SPU2: ...with the same interrupts (%u calls differ, %u raised) and RAM (%u calls differ, %s at the end)",
                  calls_differing, interrupts, ram_differing, all_one == all_batch ? "the same" : "different");
    Check(calls_differing == 0 && interrupts > 0 && ram_differing == 0 && all_one == all_batch, what);
}

void Spu2MixesVoicesInBatches() {
    Spu2MixesVoicesInBatchesOnce(true);
    Spu2MixesVoicesInBatchesOnce(false);
}

void SpuDecodesAdpcmAndPlaysAVoice() {
    using ps2host::iop::Spu2;
    // Shift 0, filter 0: a nibble is the sample's top four bits.
    Spu2::u8 block[16] = {0x00, 0x00, 0x81};
    Spu2::s32 out[28], older = 0, old = 0;
    Spu2::DecodeBlock(block, out, &older, &old);
    Check(out[0] == 4096 && out[1] == -32768 && out[2] == 0, "ADPCM: shift 0, filter 0");
    // Filter 1 predicts 60/64 of the previous sample.
    Spu2::u8 predicted[16] = {0x1C};
    older = 0;
    old = 64;
    Spu2::DecodeBlock(predicted, out, &older, &old);
    Check(out[0] == 60, "ADPCM: filter 1 predicts from the previous sample");

    // A voice looping one block of 4096s, at full volume once its attack is done.
    Spu2 spu;
    Spu2::u8 loop[16] = {0x00, 0x07};
    for (int i = 2; i < 16; ++i) loop[i] = 0x11;
    spu.Write(0x5000, loop, 16);
    std::vector<Spu2::s16> heard;
    spu.SetOutput([&](const Spu2::s16* frames, Spu2::u32 count) { heard.assign(frames, frames + 2 * count); });
    spu.SetAddr(0x2000, 0x5000);                    // SSA, voice 0 of core 0
    spu.SetParam(0x0000, 0x3FFF);                   // VOLL
    spu.SetParam(0x0100, 0x3FFF);                   // VOLR
    spu.SetParam(0x0200, 0x1000);                   // pitch: 48 kHz
    spu.SetParam(0x0300, 0x000F);                   // ADSR1: fastest attack, full sustain
    spu.SetParam(0x0400, 0x1FC0);                   // ADSR2: sustain holds
    spu.SetSwitch(0x1800, 1);                       // VMIXL
    spu.SetSwitch(0x1A00, 1);                       // VMIXR
    spu.SetSwitch(0x1500, 1);                       // KON
    auto none = [](Spu2::u32) {};
    spu.Advance(1000, none);
    spu.Advance(2000, none);                        // 48 samples
    Check(heard.size() == 96, "SPU2: a millisecond is 48 stereo frames");
    Check(heard.size() == 96 && heard[90] > 3500 && heard[90] < 4200 && heard[91] == heard[90],
          "SPU2: a voice sounds at its volume");
    Check(spu.GetSwitch(0x1700) & 1, "SPU2: ENDX after the loop's end");
    Check(spu.GetParam(0x0500) == 0x7FFF, "SPU2: ENVX after the attack");
    // A sweep: VOLL 0xA020 (linear, decreasing, rate 0x20 - a step of 64 a
    // sample) fades the voice from where it was to nothing in about 512
    // samples; VOLR stays. Burnout 3 quiets the voices it leaves running so:
    // played at full volume instead, a race's twenty of them clipped.
    spu.SetParam(0x0000, 0xA020);
    spu.Advance(3000, none);                        // 48 samples on
    const int fading = heard[90];
    spu.Advance(15000, none);                       // 576 more
    Check(fading > 2500 && fading < heard[91], "SPU2: a decreasing sweep lowers the volume from where it was");
    Check(heard[2 * 575] == 0 && heard[2 * 575 + 1] > 3500, "SPU2: the sweep reaches silence, the other side plays on");
    Check(spu.GetParam(0x0600) == 0, "SPU2: VOLXL, the current volume, is the sweep's");

    // Wet-only voices need their capture buffers even with reverb disabled.
    // Burnout 3 DMA-reads this mix to process the player's engine sound.
    Spu2 captured;
    captured.Write(0x5000, loop, 16);
    std::vector<Spu2::s16> capture_output;
    captured.SetOutput([&](const Spu2::s16* frames, Spu2::u32 count) { capture_output.assign(frames, frames + 2 * count); });
    captured.SetAddr(0x2000, 0x5000);
    captured.SetParam(0x0000, 0x3FFF);
    captured.SetParam(0x0100, 0x3FFF);
    captured.SetParam(0x0200, 0x1000);
    captured.SetParam(0x0300, 0x000F);
    captured.SetParam(0x0400, 0x1FC0);
    captured.SetSwitch(0x1900, 1);
    captured.SetSwitch(0x1B00, 1);
    captured.SetSwitch(0x1500, 1);
    captured.SetAddr(0x1F00, 0x2820);
    captured.SetAddr(0x1F01, 0x2820);
    captured.SetCoreAttr(0x04, 1);
    captured.SetCoreAttr(0x05, 1);
    unsigned capture_irqs = 0;
    auto capture_irq = [&](Spu2::u32 core) { capture_irqs |= 1u << core; };
    captured.Advance(1000, capture_irq);
    captured.Advance(2000, capture_irq);
    auto capture_sample = [&](unsigned address) {
        Spu2::u8 bytes[2]; captured.Read(address, bytes, 2);
        return int(Spu2::s16(unsigned(bytes[0]) | unsigned(bytes[1]) << 8));
    };
    Check(capture_sample(0x2800 + 94) > 3500 && capture_sample(0x2C00 + 94) > 3500,
          "SPU2: wet capture is written at 48 kHz without audible reverb");
    Check(capture_sample(0x2000 + 94) == 0 && capture_sample(0x2400 + 94) == 0 &&
          capture_output.back() == 0, "SPU2: a wet-only voice does not leak into the dry output");
    Check(capture_irqs == 3, "SPU2: both cores see a capture-buffer write at their IRQA");
    captured.Advance(12000, none);                  // Fill the complete ring before fading.
    captured.SetParam(0x0000, 0);
    captured.SetParam(0x0100, 0);
    captured.Advance(14000, none);                  // 624 samples total: wrapped at 512
    Check(capture_sample(0x2800 + 94) == 0 && capture_sample(0x2C00 + 94) == 0 &&
          capture_sample(0x2800 + 400) > 3500,
          "SPU2: 512-sample captures wrap and retain the unoverwritten part");
    // Voice 1 and 3 taps are before voice volume and mix routing.
    captured.SetAddr(0x2002, 0x5000);
    captured.SetParam(0x0202, 0x1000);
    captured.SetParam(0x0302, 0x000F);
    captured.SetParam(0x0402, 0x1FC0);
    captured.SetSwitch(0x1500, 2);
    captured.Advance(15000, none);
    Check(capture_sample(0x0800 + 300) > 3500,
          "SPU2: the raw voice-1 tap captures post-envelope audio at zero volume");

    // B3ROUTE returns planar, signed PCM to core 1's input RAM. BVOL is
    // signed 16-bit (unlike a voice volume's signed 15-bit fixed mode).
    Spu2 pcm;
    std::vector<Spu2::s16> pcm_output;
    pcm.SetOutput([&](const Spu2::s16* frames, Spu2::u32 count) { pcm_output.assign(frames, frames + 2 * count); });
    std::vector<Spu2::u8> pcm_data(0x800);
    for (unsigned i = 0; i < 512; ++i) {
        pcm_data[i*2] = 0x10; pcm_data[i*2+1] = 0x27;     // left: +10000
        pcm_data[0x400+i*2] = 0xF0; pcm_data[0x401+i*2] = 0xD8; // right: -10000
    }
    pcm.Write(0x4800, pcm_data.data(), unsigned(pcm_data.size()));
    pcm.SetParam(0x0801, 0x0C0);                   // PCM directly to both speakers
    pcm.SetParam(0x0F81, 0x7FFF);
    pcm.SetParam(0x1081, 0x4000);
    pcm.Advance(1000, none); pcm.Advance(2000, none);
    Check(pcm_output[90] > 9900 && pcm_output[90] < 10000 &&
          pcm_output[91] >= -5000 && pcm_output[91] < -4900,
          "SPU2: planar PCM input plays at independent signed 16-bit volumes");
    Check(pcm.GetParam(0x0801) == 0x0C0 && pcm.GetParam(0x0F81) == 0x7FFF &&
          pcm.GetParam(0x1081) == 0x4000, "SPU2: input parameters read back");
    pcm.SetParam(0x0801, 0x040);                   // gate off only the left input
    pcm.Advance(3000, none);
    Check(pcm_output[90] == 0 && pcm_output[91] < -4900,
          "SPU2: MMIX gates PCM separately on each side");
    pcm.SetParam(0x0801, 0x0C0);
    pcm.SetParam(0x0F81, 0x8000);                  // negative unity, not a sweep
    pcm.SetParam(0x1081, 0);
    pcm.Advance(4000, none);
    Check(pcm_output[90] < -9900 && pcm_output[91] == 0,
          "SPU2: signed PCM volume can invert or silence the input");
    pcm.Advance(15000, none);                     // crosses the 512-sample ring
    Check(pcm_output[900] < -9900 && pcm_output[901] == 0,
          "SPU2: PCM input wraps at 512 samples");

    // Host engine gain changes only returned PCM, with a 5 ms ramp. The
    // guest's samples and volume registers remain untouched.
    pcm.SetParam(0x0F81, 0x7FFF);
    pcm.SetParam(0x1081, 0x7FFF);
    pcm.Advance(16000, none);
    const int unity_left = pcm_output[90], unity_right = pcm_output[91];
    pcm.SetInputVolume(1, 50);
    pcm.Advance(25000, none);
    Check(pcm_output[700] >= 4990 && pcm_output[700] <= 5000 &&
          pcm_output[701] >= -5000 && pcm_output[701] <= -4990,
          "SPU2: host PCM gain halves both channels after the ramp");
    pcm.SetInputVolume(1, 0);
    pcm.Advance(35000, none);
    Check(pcm_output[0] > 0 && pcm_output[700] == 0 && pcm_output[701] == 0,
          "SPU2: gain fades smoothly to silence");
    pcm.SetInputVolume(1, 200);
    pcm.Advance(45000, none);
    Check(pcm_output[700] >= 19980 && pcm_output[700] <= 20000,
          "SPU2: host PCM gain can boost to 200 percent");
    pcm.SetInputVolume(1, 100);
    pcm.Advance(55000, none);
    Check(pcm_output[700] == unity_left && pcm_output[701] == unity_right,
          "SPU2: 100 percent preserves the original PCM exactly");
    Spu2::u8 retained[2] = {};
    pcm.Read(0x4800, retained, 2);
    Check(retained[0] == 0x10 && retained[1] == 0x27 &&
          pcm.GetParam(0x0F81) == 0x7FFF,
          "SPU2: host gain preserves guest input RAM and BVOL");

    // Voices (traffic/effects/music) remain audible with player PCM muted.
    Spu2::u8 traffic[16] = {0x00, 0x07};
    for (int i = 2; i < 16; ++i) traffic[i] = 0x11;
    pcm.Write(0x5000, traffic, 16);
    pcm.SetAddr(0x2001, 0x5000);
    pcm.SetParam(0x0001, 0x3FFF);
    pcm.SetParam(0x0101, 0x3FFF);
    pcm.SetParam(0x0201, 0x1000);
    pcm.SetParam(0x0301, 0x000F);
    pcm.SetParam(0x0401, 0x1FC0);
    pcm.SetParam(0x0801, 0x0CC0);
    pcm.SetSwitch(0x1801, 1);
    pcm.SetSwitch(0x1A01, 1);
    pcm.SetSwitch(0x1501, 1);
    pcm.SetInputVolume(1, 0);
    pcm.Advance(70000, none);
    Check(pcm_output[1000] > 3500 && pcm_output[1001] > 3500,
          "SPU2: PCM mute does not mute the other ADPCM voices");

    // The reverb: one loud sample sent only to it (VMIXEL/VMIXER, not the dry
    // mix) comes out later, and keeps coming out after the voice has ended.
    Spu2 wet;
    Spu2::u8 burst[16] = {0x00, 0x01};              // loop end, no repeat: one block, then silence
    for (int i = 2; i < 16; ++i) burst[i] = 0x77;   // 28 loud samples (the envelope rises through them)
    wet.Write(0x5000, burst, 16);
    std::vector<Spu2::s16> tail;
    wet.SetOutput([&](const Spu2::s16* frames, Spu2::u32 count) { tail.assign(frames, frames + 2 * count); });
    wet.SetAddr(0x2000, 0x5000);
    wet.SetParam(0x0000, 0x3FFF);
    wet.SetParam(0x0100, 0x3FFF);
    wet.SetParam(0x0200, 0x1000);
    wet.SetParam(0x0300, 0x000F);
    wet.SetParam(0x0400, 0x1FC0);
    wet.SetSwitch(0x1900, 1);                       // VMIXEL
    wet.SetSwitch(0x1B00, 1);                       // VMIXER
    Check(wet.SetEffectAttr(0, 0x101, 0x3FFF, 0x3FFF, 0, 0) == 0, "SPU2: the Room preset, its work area cleared");
    wet.SetCoreAttr(0x2, 1);                        // SD_CORE_EFFECT_ENABLE
    wet.SetSwitch(0x1500, 1);
    wet.Advance(1000, none);
    wet.Advance(201000, none);                      // 200 ms
    int loud_late = 0;
    for (std::size_t i = 2 * 4800; i < tail.size(); ++i) loud_late += std::abs(int(tail[i])) > 8;
    Check(tail.size() == 2 * 9600 && tail[0] == 0, "SPU2: nothing dry");
    Check(loud_late > 100, "SPU2: the reverb's tail, long after the sound ended");
    Check(wet.SetEffectAttr(0, 10, 0, 0, 0, 0) == -1, "SPU2: no eleventh preset");
}

/* The SPU2 on a thread of its own (--sound-thread, the Vita's): IRQA in core
   0's voice-1 capture ring, written every sample, is reached there; the
   interrupt waits for the EE's thread, whose SoundInterrupts calls the
   handler (an IOP function counting its calls) - once until it is re-armed. */
void SoundThreadRaisesInterruptsForTheEe() {
    using namespace ps2host::iop;
    Vm& vm = TheVm();
    constexpr u32 kHandler = 0x00080000u, kCount = 0x00080100u;
    const u32 code[] = {0x3C080008u,        // lui   $t0, 0x0008
                        0x8D090100u,        // lw    $t1, 0x100($t0)
                        0x00000000u,
                        0x25290001u,        // addiu $t1, $t1, 1
                        0xAD090100u,        // sw    $t1, 0x100($t0)
                        0x03E00008u,        // jr    $ra
                        0x00000000u};
    for (u32 i = 0; i < 7; ++i) vm.Write(kHandler + 4 * i, code[i], 4);
    vm.Write(kCount, 0, 4);
    auto call = [&](u32 index, u32 a0, u32 a1) {
        Context c;
        c.r[4] = a0;
        c.r[5] = a1;
        FindLibrary("libsd", index)(vm, c);
    };
    call(27, kHandler, 0);                  // sceSdSetSpu2IntrHandler
    call(9, 0x1F00, 0x0800);                // IRQA: core 0's voice-1 capture ring
    call(11, 0x04, 1);                      // the interrupt on
    StartSoundThread();
    auto wait_for = [&](u32 count) {
        for (int i = 0; i < 200 && vm.Read(kCount, 4) < count; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            SoundInterrupts(vm);
        }
        return vm.Read(kCount, 4);
    };
    Check(wait_for(1) == 1, "SPU2 thread: the interrupt reaches the EE's handler");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    SoundInterrupts(vm);
    Check(vm.Read(kCount, 4) == 1, "SPU2 thread: once, until re-armed");
    call(11, 0x04, 1);
    Check(wait_for(2) == 2, "SPU2 thread: again once re-armed");
    StopSoundThread();
    call(11, 0x04, 0);
    call(27, 0, 0);
}

/*
 * What the processor does with the host's work, measured (PS2HOST_TEST_BENCH;
 * the Vita's runner asks for it): the numbers the port's speed is estimated
 * from on a new processor, taken the same way on each. Nothing is checked.
 */
void Benchmarks() {
    using Clock = std::chrono::steady_clock;
    auto ns_since = [](Clock::time_point from) {
        return std::chrono::duration<double, std::nano>(Clock::now() - from).count();
    };
    std::printf("benchmarks:\n");

    // VU programs: the corpus (random microprograms, vurecomp's) run compiled,
    // compiled without the flags no one reads (--vu-fast), and interpreted.
    std::size_t count = 0;
    const ps2host::VuCompiled* programs = VuGeneratedPrograms(&count);
    for (int mode = 0; mode < 3; ++mode) {
        double ns = 0;
        u64 pairs = 0;
        for (int pass = 0; pass < 4; ++pass) {
            u32 seed = 1234;
            for (std::size_t i = 0; i < count; ++i) {
                const ps2host::VuCompiled& program = programs[i];
                if (!program.block_count || (mode == 1 && !program.fast_blocks)) continue;
                auto unit = std::make_unique<ps2host::Vu>(program.unit);
                PrepareVuRun(*unit, program, seed);
                unit->use_compiled = mode != 2;
                unit->fast = mode == 1;
                const auto began = Clock::now();
                pairs += unit->Run(program.entry, 3000);
                ns += ns_since(began);
            }
        }
        static const char* const kModes[] = {"compiled", "compiled, fast", "interpreted"};
        std::printf("  VU %-16s %7.1f ns a pair (%llu pairs)\n", kModes[mode], pairs ? ns / double(pairs) : 0.0,
                    (unsigned long long)pairs);
    }

    // A texture decoded from GS memory (the GXM renderer's textures): a
    // 256 x 256 8-bit one through the CLUT, a texel at a time as before and
    // a row at a time.
    {
        using namespace ps2host::gs;
        auto memory = std::make_unique<Memory>();
        for (u32 i = 0; i < (4u << 20); ++i) memory->data()[i] = u8(i * 2654435761u >> 24);
        std::vector<u32> clut(256, 0x80808080u), pixels(256 * 256);
        TextureView d;
        d.tbp = 0x1000, d.tbw = 4, d.psm = PSMT8, d.tw = 256, d.th = 256;
        u32 sum = 0;
        auto began = Clock::now();
        for (int i = 0; i < 4; ++i)
            for (u32 v = 0; v < 256; ++v)
                for (u32 u = 0; u < 256; ++u) sum += DecodeTexel(*memory, d, clut.data(), int(u), int(v), 0);
        const double one = ns_since(began) / (4.0 * 65536.0);
        began = Clock::now();
        for (int i = 0; i < 4; ++i) DecodeTexture(*memory, d, clut.data(), pixels.data());
        const double whole = ns_since(began) / (4.0 * 65536.0);
        std::printf("  texture decode     %7.1f ns a texel one at a time, %.1f a row at a time (%08X)\n", one, whole,
                    unsigned(sum + pixels[1234]));
    }

    // The IPU through its channels (libmpeg's movies): a macroblock's BDEC
    // and its CSC.
    {
        const IpuRun run = RunIpuThroughChannels(400);
        std::printf("  IPU macroblock     %7.1f us BDEC, %.1f us CSC\n", run.bdec_ns / 1000.0, run.csc_ns / 1000.0);
    }

    // Main memory: a copy of the size of the PS2's RAM.
    {
        std::vector<u8> from(32u << 20, 1), to(32u << 20);
        const auto began = Clock::now();
        for (int i = 0; i < 4; ++i) std::memcpy(to.data(), from.data(), from.size());
        std::printf("  memcpy             %7.0f MB/s\n", 4.0 * 32.0 / (ns_since(began) / 1e9));
    }

    // The generated code's arithmetic: every EE register a u64, most of it
    // word arithmetic sign-extended - against the same in 32 bits.
    {
        volatile u64 seed64 = 0x12345678;
        u64 a = seed64, b = seed64 * 3;
        auto began = Clock::now();
        for (int i = 0; i < 20000000; ++i) {
            a = u64(s64(s32(u32(a + b))));
            b = (b << 1) ^ (a >> 3);
        }
        const double ns64 = ns_since(began);
        volatile u32 seed32 = 0x12345678;
        u32 c = seed32, d = seed32 * 3;
        began = Clock::now();
        for (int i = 0; i < 20000000; ++i) {
            c = c + d;
            d = (d << 1) ^ (c >> 3);
        }
        const double ns32 = ns_since(began);
        std::printf("  integer steps      %7.2f ns in 64 bits, %.2f in 32 (%llu %u)\n", ns64 / 20e6, ns32 / 20e6,
                    (unsigned long long)(a ^ b), unsigned(c ^ d));
    }

    // The EE's FPU as the generated code calls it.
    {
        u32 x = 0x3F800001u, acc = 0;
        const auto began = Clock::now();
        for (int i = 0; i < 2000000; ++i) {
            acc = generated::FloatAdd(acc, generated::FloatMul(x, 0x3F7FFFF0u));
            x = generated::FloatAdd(x, 0x33800000u);
        }
        std::printf("  EE float add+mul  %7.1f ns (%08X)\n", ns_since(began) / 2e6, acc);
    }

    // Macro mode's COP2 (its rounding set and put back each time).
    {
        auto c = std::make_unique<lifted::Context>();
        for (auto& v : c->vf) for (u32& lane : v) lane = 0x3F800000u;
        const auto began = Clock::now();
        for (int i = 0; i < 500000; ++i) lifted::Cop2(*c, 0x4A000000u | 15u << 21 | 2u << 16 | 1u << 11 | 3u << 6 | 0x28);
        std::printf("  COP2 ADD           %7.1f ns\n", ns_since(began) / 5e5);
    }

    // The SPU2 as Burnout 3 leaves it: its menus' 42 voices keyed off and
    // running silent, and a race's 25 heard of 42 - core 0's reverb on,
    // IRQA in a capture ring - a millisecond's mixing (48 samples) at a
    // time, as its thread does.
    for (u32 heard : {0u, 25u}) {
        using ps2host::iop::Spu2;
        auto spu = std::make_unique<Spu2>();
        spu->SetBatchVoices(true);          // as its own thread mixes
        u64 samples = 0;
        spu->SetOutput([&](const Spu2::s16*, Spu2::u32 count) { samples += count; });
        std::vector<Spu2::u8> block(64 * 16, 0x11);
        for (std::size_t b = 0; b < block.size(); b += 16) block[b] = 0, block[b + 1] = 0;
        block[1] = 0x04;
        block[block.size() - 16 + 1] = 0x03;
        spu->Write(0x10000, block.data(), Spu2::u32(block.size()));
        spu->SetEffectAttr(0, 4, 0x3000, 0x3000, 0, 0);
        spu->SetCoreAttr(0x02, 1);
        spu->SetAddr(0x1F00, 0x0800);
        u32 n = 0;
        for (u32 core = 0; core < 2; ++core)
            for (u32 voice = 0; voice < (core ? 18u : 24u); ++voice, ++n) {
                const u32 entry = core | voice << 1;
                spu->SetAddr(0x2000 | entry, 0x10000);
                spu->SetParam(0x0000 | entry, 0x3FFF);
                spu->SetParam(0x0100 | entry, 0x3FFF);
                spu->SetParam(0x0200 | entry, 0x1000);
                spu->SetParam(0x0300 | entry, 0x000F);
                spu->SetParam(0x0400 | entry, 0x1FC0);      // sustained
                spu->SetSwitch(0x1800 | core, spu->GetSwitch(0x1800 | core) | 1u << voice);
                spu->SetSwitch(0x1A00 | core, spu->GetSwitch(0x1A00 | core) | 1u << voice);
                spu->SetSwitch(0x1500 | core, 1u << voice);
                if (n >= heard) spu->SetSwitch(0x1600 | core, 1u << voice);
            }
        auto none = [](Spu2::u32) {};
        u64 now = 1000;
        spu->Advance(now, none);
        const auto began = Clock::now();
        for (int ms = 0; ms < 2000; ++ms) spu->Advance(now += 1000, none);
        std::printf("  SPU2 mixing        %7.1f us a millisecond of sound (%u voices heard, %u silent, reverb; %llu samples)\n",
                    ns_since(began) / 1e3 / 2000.0, unsigned(heard), unsigned(42 - heard), (unsigned long long)samples);
    }

    // The EE's MMI as the generated code calls it: a halfword add, an
    // interleave, a pack - the movies' motion compensation is made of them.
    {
        u64 lo = 0x0102030405060708ull, hi = 0x1112131415161718ull, a = lo, b = hi;
        const auto began = Clock::now();
        for (int i = 0; i < 1000000; ++i) {
            lifted::Lanes(lifted::kPaddh, lo, hi, lo, hi, a, b);
            lifted::Interleave(a, b, lo, hi, 8);
            lifted::PackBytes(lo, hi, a, b, lo, hi);
        }
        std::printf("  MMI add+zip+pack   %7.1f ns (%016llX)\n", ns_since(began) / 1e6, (unsigned long long)(lo ^ hi ^ a ^ b));
    }

    // Sleeping: a 200 us sleep (the EE's idle wait), how long it takes.
    {
        double total = 0, worst = 0;
        for (int i = 0; i < 50; ++i) {
            const auto began = Clock::now();
            ps2plat::SleepUntil(began + std::chrono::microseconds(200));
            const double us = ns_since(began) / 1000.0;
            total += us;
            worst = std::max(worst, us);
        }
        std::printf("  sleep 200 us       %7.0f us on average, %.0f at worst\n", total / 50, worst);
    }
}

/*
 * Every test, named, in order. Timed when PS2HOST_TEST_TIMES is set (the
 * Vita's runner sets it), with the benchmarks after them when
 * PS2HOST_TEST_BENCH is. `observe`, if given, hears of each as it ends.
 */
int RunHostTests(void (*observe)(const char* name, double ms, int failures)) {
    struct Named {
        const char* name;
        void (*test)();
    };
    static const Named kTests[] = {
        {"MirrorsReachTheSameRam", MirrorsReachTheSameRam},
        {"LoadsExtendAsTheEeDoes", LoadsExtendAsTheEeDoes},
        {"OutsideMemoryFaults", OutsideMemoryFaults},
        {"ScratchpadAndHooks", ScratchpadAndHooks},
        {"BiosIsReadOnly", BiosIsReadOnly},
        {"Kseg1ReachesDeviceRegisters", Kseg1ReachesDeviceRegisters},
        {"DiscFindsFilesByPath", DiscFindsFilesByPath},
        {"WideWritesReachEveryWordsHook", WideWritesReachEveryWordsHook},
        {"VifSkipsDataAndAppliesMaskPath3", VifSkipsDataAndAppliesMaskPath3},
        {"DmaChainsAreWalked", DmaChainsAreWalked},
        {"GsLayoutsArePermutations", GsLayoutsArePermutations},
        {"GsDecodesTexturesWhole", GsDecodesTexturesWhole},
        {"GsDrawsASprite", GsDrawsASprite},
        {"GsFogsTowardsFogcol", GsFogsTowardsFogcol},
        {"GsLoadsACsm2Clut", GsLoadsACsm2Clut},
        {"Path3GoesOneGroupPerWindow", Path3GoesOneGroupPerWindow},
        {"GsKeepsAFlatDepthExact", GsKeepsAFlatDepthExact},
        {"GsFillsTopLeft", GsFillsTopLeft},
        {"GsTakesImagesInBlocks", GsTakesImagesInBlocks},
        {"GsTakesRegisterPacketsInBlocks", GsTakesRegisterPacketsInBlocks},
        {"TexturePackNamesAsPcsx2", TexturePackNamesAsPcsx2},
#if PS2HOST_VULKAN
        {"GpuMemorySwizzlesAsTheGs", GpuMemorySwizzlesAsTheGs},
        {"GpuScaledColourTransfersAsTheGs", GpuScaledColourTransfersAsTheGs},
        {"GpuDecodesTexturesAsTheGs", GpuDecodesTexturesAsTheGs},
#endif
        {"VifRunsMicroprogramsOnAVu", VifRunsMicroprogramsOnAVu},
        {"MemoryCardKeepsSaves", MemoryCardKeepsSaves},
        {"VuLanesAreTheModel", VuLanesAreTheModel},
        {"VuCompiledIsTheInterpreter", VuCompiledIsTheInterpreter},
        {"ArithmeticIsThePcs", ArithmeticIsThePcs},
        {"FibersSwitchAndUnwind", FibersSwitchAndUnwind},
        {"VuFastIsTheExactButFlags", VuFastIsTheExactButFlags},
        {"VuTimingIsVisible", VuTimingIsVisible},
        {"VuFlagsOfAResultIntoVf0", VuFlagsOfAResultIntoVf0},
        {"VuMaxKeepsADenormal", VuMaxKeepsADenormal},
        {"MmiLanesAreScalar", MmiLanesAreScalar},
        {"IpuDecodesAndRetriesCleanly", IpuDecodesAndRetriesCleanly},
        {"IpuStreamsThroughItsChannels", IpuStreamsThroughItsChannels},
        {"IopInstructionsMatchTheR5900", IopInstructionsMatchTheR5900},
        {"SpuDecodesAdpcmAndPlaysAVoice", SpuDecodesAdpcmAndPlaysAVoice},
        {"Spu2MixesVoicesInBatches", Spu2MixesVoicesInBatches},
        {"SoundThreadRaisesInterruptsForTheEe", SoundThreadRaisesInterruptsForTheEe},
    };
    const bool times = std::getenv("PS2HOST_TEST_TIMES") != nullptr;
    for (const Named& t : kTests) {
        const auto began = std::chrono::steady_clock::now();
        t.test();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        if (times) std::printf("  %-36s %9.1f ms\n", t.name, ms);
        if (observe) observe(t.name, ms, g_failures);
    }
    if (std::getenv("PS2HOST_TEST_BENCH")) Benchmarks();
    std::printf(g_failures ? "host tests: %d failed\n" : "host tests: all passed\n", g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

#ifndef PS2HOST_TESTS_NO_MAIN
int main() { return RunHostTests(nullptr); }
#endif
