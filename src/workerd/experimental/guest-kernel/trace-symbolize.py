#!/usr/bin/env python3
"""Symbolize a gk foreign-memory trace dump (GK_TRACE_FOREIGN, see gk.c).

The dump names each site by the file its RIP lies in and the file offset
("mod=<path> off=0x..."). This turns those into symbols with llvm-symbolizer:
the file offset is mapped to the object's virtual address through its PT_LOAD
program headers, which llvm-symbolizer then resolves.

Usage: trace-symbolize.py DUMP [--top N] [--symbolizer PATH]
"""
import argparse
import collections
import json
import re
import struct
import subprocess
import sys

SITE_RE = re.compile(
    r"\[gk-trace\] site n=(\d+) r=(\d+) w=(\d+) heap=(\d+) anon=(\d+) data=(\d+) "
    r"first=(0x[0-9a-f]+) in=(\S+) rip=(0x[0-9a-f]+) mod=(\S+) off=(0x[0-9a-f]+)"
)


def load_segments(path):
    """The PT_LOAD segments of an ELF64 file as (offset, vaddr, filesz) tuples."""
    segs = []
    try:
        with open(path, "rb") as f:
            ehdr = f.read(64)
            if ehdr[:4] != b"\x7fELF" or ehdr[4] != 2:  # ELF64 only
                return segs
            e_phoff, = struct.unpack_from("<Q", ehdr, 32)
            e_phentsize, e_phnum = struct.unpack_from("<HH", ehdr, 54)
            f.seek(e_phoff)
            for i in range(e_phnum):
                ph = f.read(e_phentsize)
                p_type, = struct.unpack_from("<I", ph, 0)
                if p_type == 1:  # PT_LOAD
                    p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<QQQQ", ph, 8)
                    segs.append((p_offset, p_vaddr, p_filesz))
    except OSError:
        pass
    return segs


def file_offset_to_vaddr(segs, off):
    for seg_off, seg_vaddr, seg_filesz in segs:
        if seg_off <= off < seg_off + seg_filesz:
            return seg_vaddr + (off - seg_off)
    return None


def symbolize(symbolizer, path, vaddrs):
    """vaddr -> 'function file:line' for each address, via one llvm-symbolizer run."""
    if not vaddrs:
        return {}
    args = [symbolizer, "--obj=" + path, "--functions=linkage", "--demangle", "--inlining=true",
            "--output-style=JSON"]
    inp = "\n".join("0x%x" % v for v in vaddrs) + "\n"
    out = subprocess.run(args, input=inp, capture_output=True, text=True, check=False).stdout
    # JSON style: one object per address, innermost inlined frame first.
    result = {}
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        obj = json.loads(line)
        try:
            v = int(obj.get("Address", "0"), 16)
        except ValueError:
            continue
        frames = []
        for sym in obj.get("Symbol", []):
            fn = sym.get("FunctionName") or "??"
            loc = "%s:%s" % (sym.get("FileName") or "??", sym.get("Line", 0))
            frames.append((fn, loc))
        result[v] = frames
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--top", type=int, default=60, help="sites to print (default 60; 0 for all)")
    ap.add_argument("--symbolizer", default="llvm-symbolizer")
    ap.add_argument("--frames", type=int, default=3, help="inlined frames to show per site")
    a = ap.parse_args()

    sites = []
    header = []
    with open(a.dump) as f:
        for line in f:
            m = SITE_RE.match(line)
            if m:
                n, r, w, heap, anon, data = (int(x) for x in m.group(1, 2, 3, 4, 5, 6))
                sites.append(dict(n=n, r=r, w=w, heap=heap, anon=anon, data=data,
                                  first=m.group(7), where=m.group(8), rip=int(m.group(9), 16),
                                  mod=m.group(10), off=int(m.group(11), 16)))
            elif line.startswith("[gk-trace]") and not line.startswith("[gk-trace] sample"):
                header.append(line.rstrip("\n"))
    for h in header:
        print(h)
    print()

    by_mod = collections.defaultdict(list)
    for s in sites:
        if s["mod"] != "?":
            by_mod[s["mod"]].append(s)
    names = {}
    for mod, ss in by_mod.items():
        segs = load_segments(mod)
        vaddrs = []
        for s in ss:
            v = file_offset_to_vaddr(segs, s["off"])
            s["vaddr"] = v
            if v is not None:
                vaddrs.append(v)
        vaddrs = sorted(set(vaddrs))
        names[mod] = symbolize(a.symbolizer, mod, vaddrs)

    total = sum(s["n"] for s in sites)
    sites.sort(key=lambda s: (-s["n"], s["rip"]))
    shown = sites if a.top == 0 else sites[: a.top]
    print("%d sites, %d hits in all; top %d:" % (len(sites), total, len(shown)))
    print("%8s %6s %6s %5s %5s %5s  %-6s %s" % ("hits", "reads", "writes", "heap", "anon", "data", "share", "site"))
    for s in shown:
        frames = names.get(s["mod"], {}).get(s.get("vaddr"), [])
        if frames:
            desc = " <- ".join("%s (%s)" % (fn, loc.split("/")[-1]) for fn, loc in frames[: a.frames])
        else:
            desc = "%s+0x%x" % (s["mod"].split("/")[-1], s["off"])
        share = "%.1f%%" % (100.0 * s["n"] / total) if total else "-"
        print("%8d %6d %6d %5d %5d %5d  %-6s %s" % (s["n"], s["r"], s["w"], s["heap"], s["anon"],
                                                   s["data"], share, desc))
        print("%46s first page %s in %s; rip 0x%x = %s+0x%x" % ("", s["first"], s["where"], s["rip"],
                                                            s["mod"].split("/")[-1], s["off"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
