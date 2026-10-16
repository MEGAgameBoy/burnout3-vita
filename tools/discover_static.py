#!/usr/bin/env python3
"""Code a run would discover, found from the executable alone.

    python engine/tools/discover_static.py --game games/<name> [--write]

A port meets code no function covers at run time: a switch's cases laid out
past the function the analysis gave it (Burnout 3's memory-card worker,
0x00242A60, jumps through 0x004BB320 to cases from 0x00242AD8), a branch into
the middle of another function. The host stops there, names the address, and
port.py translates it for the next build (build/port/discovered.txt) - one
stop a run. On a system that cannot run the PC build (the Vita, built
elsewhere) every stop is a round trip to the player.

This finds them ahead: the words of the image that hold addresses of code
in runs, as a switch's case table does (some of them in a short gap between
functions, in the code proper - not the data and VU microcode after it), and
every branch or jump out of a function's code (its range and chunks) to an
address that is no function's entry; then the same from each piece found.
Each piece is translated as port.py translates what a run found
(port.lift_piece): from its address to its first exit no branch inside it
passes, or to the next function or piece, where it falls through (a tail
call) - so a piece cut short has exits of its own, past the cut, and those
are pieces too, until nothing new turns up. They are as unverified as what
a run finds. A word that is data and only looks like an address is kept only
if the code there lifts; it is never called unless the game jumps there.

--write adds them to build/port/discovered.txt (as "run source" pairs, the
run's own address both times); without it, they are listed.
"""

from __future__ import annotations

import argparse
import bisect
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]

from ps2decomp import gaps, lift  # noqa: E402
from ps2decomp import game as game_config  # noqa: E402
from ps2decomp.r5900 import DISASM  # noqa: E402
import port  # noqa: E402


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    game_config.add_argument(parser)
    parser.add_argument("--write", action="store_true", help="add them to build/port/discovered.txt")
    args = parser.parse_args(argv)
    game = game_config.from_args(args)

    elf = game.load_elf()
    section = elf.exec_sections[0]
    image = elf.section_data(section)
    base, end = section.addr, section.addr + len(image)
    words = struct.unpack_from("<%dI" % (len(image) // 4), image, 0)

    def word_at(address):
        return words[(address - base) // 4]

    database = game.load_database()
    # A function's code: its range and its chunks (out-of-line pieces the
    # analysis joined to it, which its translation holds).
    pieces = {f["addr"]: [(f["addr"], f["addr"] + f["size"])] + [(a, b) for a, b in f.get("chunks") or []]
              for f in database["functions"] if f["size"]}
    ranges = sorted(r for own in pieces.values() for r in own)
    starts = [s for s, _e in ranges]

    def covered(address):
        i = bisect.bisect_right(starts, address) - 1
        return i >= 0 and ranges[i][0] <= address < ranges[i][1]

    lifted_dir = game.path(os.path.join(os.path.dirname(game.generated), "lifted"))
    entries = set(port.translated_functions(lifted_dir)) | {f["addr"] for f in database["functions"]}
    discovered_path = port.discoveries_path(game)
    known = set()
    if os.path.exists(discovered_path):
        with open(discovered_path, encoding="utf-8") as handle:
            known = {int(line.split()[0], 16) for line in handle if line.strip()}
    entries |= known

    def code(address):
        return base <= address < end and address % 4 == 0

    def exits(own):
        """Targets of the branches and jumps in the pieces `own` that leave
        them, and of its calls."""
        for start, stop in own:
            for at in range(start, stop, 4):
                ins = DISASM.decode(word_at(at), at)
                t = ins.target
                if t is None or t < 0 or not code(t):
                    continue
                if ins.is_call:
                    yield t
                elif (ins.is_branch or ins.is_jump) and not any(a <= t < b for a, b in own):
                    yield t

    # The code proper: from the first function to the first gap of 64 KB or
    # more (an executable's one section may hold its data, and VU microcode,
    # after it; what the analysis took for functions there is no evidence).
    code_end = ranges[-1][1]
    for (_s, e), (s2, _e2) in zip(ranges, ranges[1:]):
        if s2 - e >= 0x10000:
            code_end = e
            break

    def small_gap(address):
        """Whether `address` lies in a gap between functions, a short one (the
        cases a switch left outside its function), in the code proper."""
        if address >= code_end:
            return False
        i = bisect.bisect_right(starts, address) - 1
        if i < 0 or i + 1 >= len(ranges) or ranges[i][0] <= address < ranges[i][1]:
            return False
        low = max(e for _s, e in ranges[:i + 1][-64:])
        return ranges[i + 1][0] - low <= 0x1000

    # Seeds: the cases of switch tables - runs of words, each an address of
    # code close to the others, some of them in a short gap - and the
    # functions' own exits.
    seeds = set()
    for s in elf.sections:
        if not s.is_alloc or not s.size or s.type == 8:
            continue
        data = elf.section_data(s)
        values = struct.unpack_from("<%dI" % (len(data) // 4), data, 0)
        i = 0
        while i < len(values):
            j = i
            while j < len(values) and code(values[j]) and values[j] < code_end and \
                    (values[j] in entries or covered(values[j]) or small_gap(values[j])):
                j += 1
            run = values[i:j]
            if len(run) >= 2 and max(run) - min(run) <= 0x4000:
                seeds.update(v for v in run if v not in entries and not covered(v))
            i = j + 1 if j == i else j
    pointer_seeds = set(seeds)
    for own in pieces.values():
        for t in exits(own):
            if t not in entries:
                seeds.add(t)

    # Each piece runs to its first exit no branch inside it passes - or to
    # the next function or piece, where it falls through (a tail call): so
    # port.py lifts it (its discovered_functions). A piece cut short so has
    # exits of its own - a branch past the cut, into another piece's middle
    # (Burnout 3's 0x00242AA0, the worker's loop, leaves for 0x00242D50
    # inside the piece at 0x00242D4C) - each a piece too, which cuts others
    # again: until nothing new turns up.
    lift.use_sections([(image, base)] + [(elf.section_data(s), s.addr) for s in elf.sections
                                         if s.is_alloc and s.size and s is not section and s.type != 8])
    found, refused, sizes = set(), {}, {}
    pending, again = set(seeds), True
    while pending or again:
        found |= {a for a in pending if a not in entries and a not in refused}
        pending, again = set(), False
        boundaries = sorted(entries | found)    # (a piece refused leaves them: its neighbours run on)
        for address in sorted(found):
            offset = address - base
            following = bisect.bisect_right(boundaries, address)
            limit = boundaries[following] if following < len(boundaries) else None
            try:
                length, _text = port.lift_piece(image[offset:offset + gaps.MAX_FUNCTION], address, limit)
            except (ValueError, lift.Refused) as exc:
                refused[address] = str(exc)
                again = True
                continue
            if sizes.get(address) == length:
                continue                        # its exits were looked at with this length
            sizes[address] = length
            for t in exits([(address, address + length)]):
                if t not in entries and t not in found and t not in refused:
                    pending.add(t)
        found -= set(refused)
    kept = sorted(found)

    from_pointers = sum(1 for a in kept if a in pointer_seeds)
    print("%d pieces of code a run would discover%s, %d bytes (%d from case tables and other pointers, %d "
          "reached from them or by branches out of functions); %d candidates refused" %
          (len(kept), " beside the %d discovered.txt has" % len(known) if known else "",
           sum(sizes[a] for a in kept), from_pointers, len(kept) - from_pointers, len(refused)))
    if args.write:
        os.makedirs(os.path.dirname(discovered_path), exist_ok=True)
        with open(discovered_path, "a", encoding="utf-8") as handle:
            for address in kept:
                if address not in known:
                    handle.write("0x%08X 0x%08X\n" % (address, address))
        print("added to %s" % discovered_path)
    else:
        for address in kept:
            print("0x%08X  %d bytes%s" % (address, sizes[address], "  (pointer)" if address in pointer_seeds else ""))
        for address in sorted(refused):
            print("refused 0x%08X: %s" % (address, refused[address]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
