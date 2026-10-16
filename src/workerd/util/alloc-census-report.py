#!/usr/bin/env python3
"""Symbolizes an allocation census dump (see util/alloc-census.h).

  alloc-census-report.py sites <dump> <binary> [pattern]
      Every site whose stack matches `pattern` (default: the ones with a
      cross-isolate free), with its counters.

  alloc-census-report.py shared <earlier-dump> <later-dump> <binary> [min]
      Sites allocated under exactly one isolate's lock whose counts do not grow
      when a second isolate does the same work and whose blocks are still live:
      process-wide state established inside one isolate's turn, which the free
      counters cannot see. `min` is the smallest number of allocations under a
      lock to report, 10 by default.

  alloc-census-report.py phases <p0> <p1> <p2> <binary> [min]
      The same question over the three dumps census-run.sh writes, and with the
      sites aggregated by their symbol names first. A site is keyed on ten
      return addresses, and an allocation made inside a turn has JIT code
      addresses among them, which differ between two isolates running the same
      script: keying on the raw stack therefore splits one logical site into one
      per isolate, and each piece then looks like state only one isolate ever
      allocated. Aggregating first is what makes the diff mean what it says.
      Reports where each aggregate grew -- `startup` for what was there before
      any request, `phase1` for what the first worker's own requests added --
      and how much of it was allocated with no lock held at all, which is in
      the shared heap already and is not a hazard.

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
        r["names"] = seen
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


def shared(earlier, later, binary, low=10):
    before = {tuple(r["frames"]): r for r in load(earlier)}
    sel = [
        r
        for r in load(later)
        if r["underLock"] >= low
        and r["nAllocIsolates"] == 1
        and r["live"] > 0
        and tuple(r["frames"]) in before
        and before[tuple(r["frames"])]["underLock"] == r["underLock"]
    ]
    symbolize(sel, binary)
    sel.sort(key=lambda r: -r["allocs"])
    report(sel, "process-wide state established in one isolate's turn")


# The name of a frame, for aggregating sites that are the same code reached
# through different JIT addresses. The allocator's own frames carry no
# information and the STL's container plumbing repeats across every container,
# so both are dropped before the first five distinct names are taken.
PLUMBING = re.compile(
    r"^(Malloc|AllocateAtLeast|__libcpp_allocate|allocate$|__construct|"
    r"std::__1::__hash_table|std::__1::__tree)"
)


def labels(rows, binary):
    symbolize(rows, binary)
    out = []
    for r in rows:
        names = [n for n in r["names"] if not PLUMBING.match(n)]
        out.append(tuple(names[:5]) or ("?",))
    return out


def phases(p0, p1, p2, binary, low=1):
    d0, d1, d2 = ({tuple(r["frames"]): r for r in load(p)} for p in (p0, p1, p2))
    keys = set(d0) | set(d1) | set(d2)
    rows = [d2.get(k) or d1.get(k) or d0.get(k) for k in keys]
    for r, k in zip(rows, keys, strict=True):
        r["frames"] = list(k)
    lab = labels(rows, binary)
    agg = {}
    for k, r, name in zip(keys, rows, lab, strict=True):
        a = agg.setdefault(
            name,
            dict(startup=0, phase1=0, phase2=0, turn=0, nolock=0, live=0, iso=0, n=0),
        )
        u0 = d0.get(k, {}).get("underLock", 0)
        u1 = d1.get(k, {}).get("underLock", u0)
        u2 = d2.get(k, {}).get("underLock", u1)
        a["startup"] += u0
        a["phase1"] += u1 - u0
        a["phase2"] += u2 - u1
        a["turn"] += r["aTurn"]
        a["nolock"] += r["aNone"]
        a["live"] += r["live"]
        a["iso"] += r["nAllocIsolates"]
        a["n"] += 1
    sel = [
        (name, a)
        for name, a in agg.items()
        if a["phase2"] == 0 and a["startup"] + a["phase1"] >= low and a["iso"] <= 1
    ]
    sel.sort(key=lambda p: -(p[1]["startup"] + p[1]["phase1"]))
    total = sum(a["startup"] + a["phase1"] for _, a in sel)
    print(
        f"established by one isolate and not by the second: {len(sel)} aggregated "
        f"sites, {total} allocations under a lock"
    )
    head = "{:>7} {:>7} {:>7} {:>7} {:>6} {:>5}  {}"
    print(head.format("startup", "phase1", "inTurn", "nolock", "live", "sites", "site"))
    for name, a in sel:
        print(
            head.format(
                a["startup"],
                a["phase1"],
                a["turn"],
                a["nolock"],
                a["live"],
                a["n"],
                " <- ".join(x[:62] for x in name),
            )
        )


def main(argv):
    if len(argv) >= 4 and argv[1] == "sites":
        sites(argv[2], argv[3], argv[4] if len(argv) > 4 else None)
    elif len(argv) in (5, 6) and argv[1] == "shared":
        shared(argv[2], argv[3], argv[4], int(argv[5]) if len(argv) > 5 else 10)
    elif len(argv) in (6, 7) and argv[1] == "phases":
        phases(argv[2], argv[3], argv[4], argv[5], int(argv[6]) if len(argv) > 6 else 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
