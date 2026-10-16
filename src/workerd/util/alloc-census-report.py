#!/usr/bin/env python3
"""Symbolizes an allocation census dump (see util/alloc-census.h).

  alloc-census-report.py sites <dump> <binary> [pattern]
      Every site whose stack matches `pattern` (default: the ones with a
      cross-isolate free), with its counters.

  alloc-census-report.py shared <earlier-dump> <later-dump> <binary>
      Sites allocated under exactly one isolate's lock whose counts do not grow
      when a second isolate does the same work and whose blocks are still live:
      process-wide state established inside one isolate's turn, which the free
      counters cannot see.

The binary must be the one that wrote the dump, built with --copt=-g1 and
--strip=never. Needs llvm-symbolizer on PATH or in LLVM_SYMBOLIZER.
"""

import os
import re
import shutil
import subprocess
import sys

FIELDS = (
    "aNone",
    "aLock",
    "aTurn",
    "bytes",
    "fSame",
    "fOther",
    "fHost",
    "fOfHost",
    "fInTurn",
    "nAllocIsolates",
    "nFreeIsolates",
    "live",
    "liveIsolateOwned",
    "liveBytes",
)
NOISE = re.compile(
    r"^(operator new|operator delete|__wrap_|malloc|calloc|realloc|free$)"
)
ROW = "{:>9} {:>9} {:>8} {:>8} {:>8} {:>8} {:>3} {:>3}  {}"


def load(path):
    rows = []
    for line in open(path):
        if not line.startswith("S "):
            continue
        f = line.split()
        row = {"frames": [int(x, 16) for x in f[1].split(",")]}
        row.update({name: int(f[2 + i]) for i, name in enumerate(FIELDS)})
        row["allocs"] = row["aNone"] + row["aLock"] + row["aTurn"]
        row["underLock"] = row["aLock"] + row["aTurn"]
        rows.append(row)
    return rows


def symbolize(rows, binary):
    tool = os.environ.get("LLVM_SYMBOLIZER") or shutil.which("llvm-symbolizer")
    if tool is None:
        sys.exit("no llvm-symbolizer; set LLVM_SYMBOLIZER")
    addrs, owner = [], []
    for i, r in enumerate(rows):
        for fr in r["frames"]:
            if fr:
                addrs.append(fr)
                owner.append(i)
    out = subprocess.run(
        [
            tool,
            "--obj",
            binary,
            "--inlining=true",
            "--demangle",
            "--output-style=LLVM",
            "--functions=linkage",
        ],
        input="".join(f"0x{a - 1:x}\n" for a in addrs),
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    names = {}
    for i, block in zip(owner, out.split("\n\n"), strict=False):
        lines = [x for x in block.strip().split("\n") if x]
        names.setdefault(i, []).extend(
            lines[j].split("(")[0] for j in range(0, len(lines), 2)
        )
    for i, r in enumerate(rows):
        seen = []
        for n in names.get(i, ["?"]):
            if NOISE.match(n) or n in seen:
                continue
            seen.append(n)
        r["site"] = " <- ".join(s[:62] for s in seen[:5])


def report(rows, title):
    allocs = sum(r["allocs"] for r in rows)
    print(f"\n== {title}  [{len(rows)} sites, {allocs} allocations]")
    print(
        ROW.format(
            "allocs", "inTurn", "fSame", "fOther", "fHost", "live", "nA", "nF", "site"
        )
    )
    for r in rows:
        print(
            ROW.format(
                r["allocs"],
                r["aTurn"],
                r["fSame"],
                r["fOther"],
                r["fHost"],
                r["live"],
                r["nAllocIsolates"],
                r["nFreeIsolates"],
                r["site"],
            )
        )


def sites(dump, binary, pattern=None):
    rows = load(dump)
    symbolize(rows, binary)
    total = sum(r["allocs"] for r in rows)
    under = sum(r["underLock"] for r in rows)
    in_turn = sum(r["aTurn"] for r in rows)
    print(f"sites={len(rows)} allocs={total} underLock={under} inTurn={in_turn}")
    same, other, host, ownerless = (
        sum(r[k] for r in rows) for k in ("fSame", "fOther", "fHost", "fOfHost")
    )
    print(
        f"frees: same={same} cross-isolate={other} no-lock={host} ownerless={ownerless}"
    )
    if pattern is None:
        sel = [r for r in rows if r["fOther"] > 0]
        title = "sites with a cross-isolate free"
    else:
        rx = re.compile(pattern)
        sel = [r for r in rows if rx.search(r["site"])]
        title = f"sites matching {pattern!r}"
    sel.sort(key=lambda r: (-r["fOther"], -r["allocs"]))
    report(sel, title)


def shared(earlier, later, binary):
    before = {tuple(r["frames"]): r for r in load(earlier)}
    sel = [
        r
        for r in load(later)
        if r["underLock"] >= 10
        and r["nAllocIsolates"] == 1
        and r["live"] > 0
        and tuple(r["frames"]) in before
        and before[tuple(r["frames"])]["underLock"] == r["underLock"]
    ]
    symbolize(sel, binary)
    sel.sort(key=lambda r: -r["allocs"])
    report(sel, "process-wide state established in one isolate's turn")


def main(argv):
    if len(argv) >= 4 and argv[1] == "sites":
        sites(argv[2], argv[3], argv[4] if len(argv) > 4 else None)
    elif len(argv) == 5 and argv[1] == "shared":
        shared(argv[2], argv[3], argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
