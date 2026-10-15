// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>

#include <cstddef>
#include <cstdint>

namespace workerd::hostcall {

// A hostcall is a synchronous call from a JS turn to a handler that runs host-side, with the
// result back before the turn's next statement. It is the crossing for a runtime operation that
// is synchronous to JS and must run outside the guest because it touches host memory
// (util/host-memory.h): an actor id minted with a key the turn cannot read, say. The guest
// kernel's primitive is gk_hostcall (experimental/guest-kernel/gk.h, "Hostcalls"); this is its
// C++ face, and the same code calls the handler directly when the guest kernel is off, so a
// handler and its callers read identically in both builds and are exercised by the ordinary
// test suite.
//
// With the guest kernel active, call() copies the frame's arguments into the thread's exchange
// region, issues the hostcall -- one VM exit from inside a turn, a direct dispatch through the
// same table from host code -- and copies the result back. The guest kernel selects the handler
// by a small integer index checked against a table in its supervisor memory, never by anything
// resembling an address; snapshots the region into a frame in host memory before the handler
// runs, so the handler sees what the caller wrote and nothing a concurrent guest thread writes
// after; bounds the bytes by BYTES; and answers -ENOSYS for an index with no handler, -E2BIG for
// a length past the frame. The handler receives no pointer the turn chose.
//
// A handler is host code that the turn invokes at will, with arguments of its choosing, so it is
// an attack surface: it validates every field of the frame it is given -- the operation against
// a closed set, a handle through hostMemory::resolveSlot(), each length against inLen -- and
// computes only what the turn is entitled to. It must return rather than throw, must not make a
// hostcall itself, and should not allocate from the heap: the turn issued the call at a point of
// its choosing, possibly in the middle of the allocator, so a handler that takes the heap's lock
// can be deadlocked by the turn that called it.
constexpr size_t BYTES = 4096;
constexpr size_t STATE = 1024;

// The exchange frame. Arguments go in `args` (by value) and the first `inLen` bytes of `buf`;
// the result comes back as the first `outLen` bytes of `buf`. `state` is host-side per-thread
// memory the guest kernel keeps between calls on one thread and never shows the guest, for a
// handler whose operation spans several calls; it is shared by every handler on the thread, so
// such a handler tags it and checks the tag on each continuation. Without the guest kernel the
// state is the caller's own frame, which the caller then keeps across the sequence.
struct Frame {
  uint64_t args[4];
  uint64_t inLen;
  uint64_t outLen;
  kj::byte buf[BYTES];
  kj::byte state[STATE];
};

using Handler = long (*)(Frame& frame);

// A handler and the index it is registered under: defined once by the handler's owner, used to
// install it and at every call site, so the two cannot disagree.
struct Entry {
  unsigned index;
  Handler handler;
};

// Index assignments. The guest kernel's table has GK_HOSTCALLS entries; every index in use is
// listed here.
enum Index : unsigned {
  ACTOR_ID = 1,     // keyed operations on an actor-id namespace key (server/actor-id-impl.h)
  WASM_SIGNAL = 2,  // register or unregister a Wasm signal field (io/tracked-wasm-instance.h)
  VFS_ROOT = 3,     // set or clear the turn directory for a SQLite VFS's root (util/sqlite.h)
  RESOLVE = 4,      // refill a turn's reserved resolve entries or flush its retire list
                    // (util/host-memory.h, ResolveTurn)
};

// Calls `entry`'s handler on `frame` and returns its result: a negative errno-style value from
// the handler or from the guest kernel's refusal, anything else from the handler. frame.inLen
// must not exceed BYTES. On return, frame.outLen and the first outLen bytes of frame.buf hold
// the result.
long call(const Entry& entry, Frame& frame);

// Registers `entry` with the guest kernel, when the guest kernel is active; no-op otherwise.
// Called once per entry, host-side, after gk_init() and before any turn runs.
void install(const Entry& entry);

}  // namespace workerd::hostcall
