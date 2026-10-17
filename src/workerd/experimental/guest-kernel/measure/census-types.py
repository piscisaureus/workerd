#!/usr/bin/env python3
# Emits a small wasm module with several distinct function signatures, so that
# instantiating it gives V8's type canonicalizer new recursion groups to add.
import struct
import sys

I32, I64, F32, F64 = 0x7F, 0x7E, 0x7D, 0x7C


def u(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def vec(items):
    return u(len(items)) + b"".join(items)


def section(sid, body):
    return bytes([sid]) + u(len(body)) + body


def functype(params, results):
    return (
        bytes([0x60])
        + vec([bytes([p]) for p in params])
        + vec([bytes([r]) for r in results])
    )


sigs = [
    ([I32, I32], [I32]),
    ([I64], [I64]),
    ([F64, F64], [F64]),
    ([], [I32]),
    ([I32, I64, F64], [F64]),
    ([F32, F32, F32, F32], [F32]),
]
bodies = [
    bytes([0x00, 0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B]),  # i32.add
    bytes([0x00, 0x20, 0x00, 0x42, 0x01, 0x7C, 0x0B]),  # i64 + 1
    bytes([0x00, 0x20, 0x00, 0x20, 0x01, 0xA0, 0x0B]),  # f64.add
    bytes([0x00, 0x41, 0x2A, 0x0B]),  # i32.const 42
    bytes([0x00, 0x20, 0x02, 0x0B]),  # return the f64 param
    bytes([0x00, 0x20, 0x00, 0x20, 0x01, 0x92, 0x0B]),  # f32.add
]
names = [b"f0", b"f1", b"f2", b"f3", b"f4", b"f5"]

mod = b"\x00asm" + struct.pack("<I", 1)
mod += section(1, vec([functype(p, r) for p, r in sigs]))
mod += section(3, vec([u(i) for i in range(len(sigs))]))
mod += section(
    7, vec([u(len(n)) + n + bytes([0x00]) + u(i) for i, n in enumerate(names)])
)
mod += section(10, vec([u(len(b)) + b for b in bodies]))
open(sys.argv[1], "wb").write(mod)
print("wrote", sys.argv[1], len(mod), "bytes,", len(sigs), "signatures")
