#!/usr/bin/env python3
"""Disassemble vector unit microprograms, for reading them.

The programs a run recorded (`--vu-record FILE`; engine/host/src/vu.cpp,
RecordProgram) as text, a pair to a line: its address, the upper
instruction and the lower, in the usual assembler's spelling. For working
out what a game's microprograms do - their inputs in data memory, what they
compute, what they kick to the GS - before doing that natively.

    python engine/tools/vudis.py build/vu_programs.txt              # every program
    python engine/tools/vudis.py build/vu_programs.txt --program 3  # the fourth line's
    python engine/tools/vudis.py build/vu_programs.txt --list       # one line each

The file holds the game's microcode: what this prints is the game's code
too, for reading at hand, never for the repository.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vurecomp import field, imm11, read_manifest, special_index  # noqa: E402

LANES = "xyzw"


def dest(word: int) -> str:
    d = field(word, 21, 4)
    return "".join(LANES[i] for i in range(4) if d & (8 >> i))


def vf(r: int) -> str:
    return "vf%02d" % r


def vi(r: int) -> str:
    return "vi%02d" % r


def lane(word: int, low: int) -> str:
    return LANES[field(word, low, 2)]


UPPER_BC = ["add", "sub", "madd", "msub", "max", "mini", "mul"]
UPPER_TABLE = {0x1C: "mulq", 0x1D: "maxi", 0x1E: "muli", 0x1F: "minii", 0x20: "addq", 0x21: "maddq",
               0x22: "addi", 0x23: "maddi", 0x24: "subq", 0x25: "msubq", 0x26: "subi", 0x27: "msubi",
               0x28: "add", 0x29: "madd", 0x2A: "mul", 0x2B: "max", 0x2C: "sub", 0x2D: "msub",
               0x2E: "opmsub", 0x2F: "mini"}
UPPER_SPECIAL = {0x10: "itof0", 0x11: "itof4", 0x12: "itof12", 0x13: "itof15", 0x14: "ftoi0", 0x15: "ftoi4",
                 0x16: "ftoi12", 0x17: "ftoi15", 0x1C: "mulaq", 0x1D: "abs", 0x1E: "mulai", 0x1F: "clipw",
                 0x20: "addaq", 0x21: "maddaq", 0x22: "addai", 0x23: "maddai", 0x24: "subaq", 0x25: "msubaq",
                 0x26: "subai", 0x27: "msubai", 0x28: "adda", 0x29: "madda", 0x2A: "mula", 0x2C: "suba",
                 0x2D: "msuba", 0x2E: "opmula", 0x2F: "nop"}


def upper(word: int) -> str:
    flags = "".join(name for bit, name in ((31, "[I]"), (30, "[E]"), (29, "[M]"), (28, "[D]"), (27, "[T]"))
                    if word >> bit & 1)
    ft, fs, fd, d = field(word, 16, 5), field(word, 11, 5), field(word, 6, 5), dest(word)
    funct = word & 0x3F
    if funct < 0x3C:
        if funct <= 0x1B:
            text = "%s%s.%s %s, %s, %s%s" % (UPPER_BC[funct >> 2], LANES[funct & 3], d, vf(fd), vf(fs), vf(ft),
                                             LANES[funct & 3])
        elif funct in UPPER_TABLE:
            name = UPPER_TABLE[funct]
            last = vf(ft) if name in ("add", "madd", "mul", "max", "sub", "msub", "opmsub", "mini") else \
                name[-1].upper()
            text = "%s.%s %s, %s, %s" % (name, d, vf(fd), vf(fs), last)
        else:
            text = "upper? %08X" % word
        return text + flags
    index = special_index(word)
    if index <= 0x0F or 0x18 <= index <= 0x1B:
        name = ["adda", "suba", "madda", "msuba"][index >> 2] if index <= 0x0F else "mula"
        return "%s%s.%s ACC, %s, %s%s%s" % (name, LANES[index & 3], d, vf(fs), vf(ft), LANES[index & 3], flags)
    name = UPPER_SPECIAL.get(index)
    if name is None:
        return "upper? %08X%s" % (word, flags)
    if name == "nop":
        return "nop" + flags
    if name.startswith(("itof", "ftoi")) or name == "abs":
        return "%s.%s %s, %s%s" % (name, d, vf(ft), vf(fs), flags)
    if name == "clipw":
        return "clipw.xyz %s, %sw%s" % (vf(fs), vf(ft), flags)
    if name in ("adda", "madda", "mula", "suba", "msuba", "opmula"):
        return "%s.%s ACC, %s, %s%s" % (name, d, vf(fs), vf(ft), flags)
    return "%s.%s ACC, %s, %s%s" % (name, d, vf(fs), name[-1].upper(), flags)


LOWER_SPECIAL = {0x30: "move", 0x31: "mr32", 0x34: "lqi", 0x35: "sqi", 0x36: "lqd", 0x37: "sqd", 0x38: "div",
                 0x39: "sqrt", 0x3A: "rsqrt", 0x3B: "waitq", 0x3C: "mtir", 0x3D: "mfir", 0x3E: "ilwr",
                 0x3F: "iswr", 0x40: "rnext", 0x41: "rget", 0x42: "rinit", 0x43: "rxor", 0x64: "mfp",
                 0x68: "xtop", 0x69: "xitop", 0x6C: "xgkick", 0x70: "esadd", 0x71: "ersadd", 0x72: "eleng",
                 0x73: "erleng", 0x74: "eatanxy", 0x75: "eatanxz", 0x76: "esum", 0x78: "esqrt", 0x79: "ersqrt",
                 0x7A: "ercpr", 0x7B: "waitp", 0x7C: "esin", 0x7D: "eatan", 0x7E: "eexp"}
BRANCHES = {0x20: "b", 0x21: "bal", 0x24: "jr", 0x25: "jalr", 0x28: "ibeq", 0x29: "ibne", 0x2C: "ibltz",
            0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}
FLAG_OPS = {0x10: "fceq", 0x11: "fcset", 0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset",
            0x16: "fsand", 0x17: "fsor", 0x18: "fmeq", 0x1A: "fmand", 0x1B: "fmor", 0x1C: "fcget"}


def lower(word: int, pc: int, mask: int) -> str:
    op = word >> 25
    it, is_, d = field(word, 16, 5), field(word, 11, 5), dest(word)
    target = (pc + 8 + imm11(word) * 8) & mask
    if op == 0x40:
        funct = word & 0x3F
        if funct < 0x3C:
            fd = field(word, 6, 5)
            if funct == 0x32:
                imm = fd - 32 if fd & 0x10 else fd
                return "iaddi %s, %s, %d" % (vi(it), vi(is_), imm)
            names = {0x30: "iadd", 0x31: "isub", 0x34: "iand", 0x35: "ior"}
            if funct in names:
                return "%s %s, %s, %s" % (names[funct], vi(fd), vi(is_), vi(it))
            return "lower? %08X" % word
        index = special_index(word)
        name = LOWER_SPECIAL.get(index)
        if name is None:
            return "lower? %08X" % word
        if name in ("move", "mr32"):
            return "%s.%s %s, %s" % (name, d, vf(it), vf(is_))
        if name in ("lqi", "lqd"):
            return "%s.%s %s, (%s%s)" % (name, d, vf(it), vi(is_), "++" if name == "lqi" else "--")
        if name in ("sqi", "sqd"):
            return "%s.%s %s, (%s%s)" % (name, d, vf(is_), vi(it), "++" if name == "sqi" else "--")
        if name in ("div", "rsqrt"):
            return "%s Q, %s%s, %s%s" % (name, vf(is_), lane(word, 21), vf(it), lane(word, 23))
        if name == "sqrt":
            return "sqrt Q, %s%s" % (vf(it), lane(word, 23))
        if name in ("waitq", "waitp", "rnext", "rget"):
            return name if name.startswith("wait") else "%s.%s %s, R" % (name, d, vf(it))
        if name == "mtir":
            return "mtir %s, %s%s" % (vi(it), vf(is_), lane(word, 21))
        if name == "mfir":
            return "mfir.%s %s, %s" % (d, vf(it), vi(is_))
        if name in ("ilwr", "iswr"):
            return "%s.%s %s, (%s)" % (name, d, vi(it), vi(is_))
        if name == "mfp":
            return "mfp.%s %s, P" % (d, vf(it))
        if name in ("xtop", "xitop"):
            return "%s %s" % (name, vi(it))
        if name == "xgkick":
            return "xgkick %s" % vi(is_)
        if name in ("rinit", "rxor"):
            return "%s R, %s%s" % (name, vf(is_), lane(word, 21))
        if name.startswith("e"):
            if name in ("esadd", "ersadd", "eleng", "erleng", "esum", "eatanxy", "eatanxz"):
                return "%s P, %s" % (name, vf(is_))
            return "%s P, %s%s" % (name, vf(is_), lane(word, 21))
        return name
    if op in (0x00, 0x01):
        reg = vf(it) if op == 0 else vf(is_)
        base = vi(is_) if op == 0 else vi(it)
        return "%s.%s %s, %d(%s)" % ("lq" if op == 0 else "sq", d, reg, imm11(word), base)
    if op in (0x04, 0x05):
        return "%s.%s %s, %d(%s)" % ("ilw" if op == 4 else "isw", d, vi(it), imm11(word), vi(is_))
    if op in (0x08, 0x09):
        unsigned = (field(word, 21, 4) << 11) | field(word, 0, 11)
        return "%s %s, %s, 0x%X" % ("iaddiu" if op == 8 else "isubiu", vi(it), vi(is_), unsigned)
    if op in FLAG_OPS:
        name = FLAG_OPS[op]
        if name.startswith("fc"):
            return "%s vi01, 0x%06X" % (name, word & 0xFFFFFF) if name != "fcget" else "fcget %s" % vi(it)
        if name.startswith("fs"):
            return "%s %s, 0x%03X" % (name, vi(it), (field(word, 21, 1) << 11) | field(word, 0, 11))
        return "%s %s, %s" % (name, vi(it), vi(is_))
    if op in BRANCHES:
        name = BRANCHES[op]
        if name in ("jr", "jalr"):
            return "%s %s%s" % (name, vi(is_), ", " + vi(it) if name == "jalr" else "")
        if name in ("b", "bal"):
            return "%s %s0x%04X" % (name, vi(it) + ", " if name == "bal" else "", target)
        if name in ("ibeq", "ibne"):
            return "%s %s, %s, 0x%04X" % (name, vi(it), vi(is_), target)
        return "%s %s, 0x%04X" % (name, vi(is_), target)
    return "lower? %08X" % word


def code_hash(pairs: dict, entry: int) -> int:
    """FNV-1a over the first four pairs' bytes from `entry`, as vu.cpp's Run keys a
    program for the frame line ("VU1's programs: 0020/1A2B3C4D 41%")."""
    h = 2166136261
    for pc in range(entry, entry + 32, 8):
        lo, up = pairs.get(pc, (0, 0))
        for byte in struct.pack("<II", lo, up):
            h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h


def disassemble(program: tuple) -> list:
    """Lines for one manifest program (read_manifest's: unit, entries, pairs)."""
    unit, entries, pairs = program
    mask = (0x4000 if unit == 1 else 0x1000) - 1
    out = ["; vu%d program entered at %s: %d pairs" % (unit, ", ".join("0x%04X" % e for e in entries), len(pairs))]
    previous = None
    for pc in sorted(pairs):
        lo, up = pairs[pc]
        if previous is not None and pc != previous + 8:
            out.append("        ...")
        previous = pc
        if up >> 31 & 1:
            low_text = "loi 0x%08X (%g)" % (lo, struct.unpack("<f", struct.pack("<I", lo))[0])
        else:
            low_text = lower(lo, pc, mask)
        mark = ">" if pc in entries else " "
        out.append("%s%04X  %-40s %s" % (mark, pc, upper(up), low_text))
    return out


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("manifest")
    parser.add_argument("--program", type=int, help="the manifest's line (from 0) to disassemble")
    parser.add_argument("--list", action="store_true", help="one line a program: unit, entry, pairs")
    args = parser.parse_args(argv)
    programs = read_manifest(args.manifest)
    if args.list:
        for i, program in enumerate(programs):
            unit, entries, pairs = program
            hashes = ",".join("%04X/%08X" % (e, code_hash(pairs, e)) for e in entries)
            print("%3d  vu%d %-32s %5d pairs from 0x%04X" % (i, unit, hashes, len(pairs), min(pairs)))
        return 0
    chosen = [programs[args.program]] if args.program is not None else programs
    for program in chosen:
        print("\n".join(disassemble(program)))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
