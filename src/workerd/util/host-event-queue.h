// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/common.h>

#include <cstdint>

namespace workerd {

// Runs every kj event loop's run queue out of host memory, so a JS turn at guest ring 3 -- and an
// escape from one with a read or write primitive -- cannot reach the queue the host walks in
// EventLoop::turn(). It installs kj's EventQueueHooks (kj/async.h): the loop keeps its sentinels,
// free list and link pool in host memory named by a handle rather than a pointer, in-turn arms and
// disarms are journaled into guest-writable records and applied host-side when the turn ends, and
// head()/turn() walk the host-memory queue. The metadata the host trusts to apply those records --
// which loop and queue they belong to -- lives in a per-thread supervisor word a turn cannot write,
// not beside the records. A no-op unless the guest kernel is active; called once, host-side, after
// gk_init() and before any turn runs.
void installHostEventQueue();

// RAII scope on the host frame around a JS turn (io/io-context.c++, runJsTurn). While it is in
// scope the turn's arms and disarms are journaled rather than applied to the host-memory queue;
// when it ends it applies the journal and reconciles the loop's runnable state. Nested instances
// defer to the outermost, as ResolveTurn does, and it is a no-op when no queue hooks are installed
// (every build but a guest-kernel one), so the ordinary path is unaffected.
class HostEventQueueTurn {
 public:
  HostEventQueueTurn();
  ~HostEventQueueTurn() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(HostEventQueueTurn);

 private:
  bool outermost;
};

// Test-only (experimental/guest-kernel/event-loop-queue-test.sh): resolve `handle` through the
// guest kernel's slot table exactly as the installed hooks resolve a loop's queue, returning the
// address it names or null when it names no live slot -- which is what a handle a JS turn forged
// into EventLoop::queueHandle resolves to.
const void* resolveQueueHandleForTest(uint64_t handle);

// Test-only (event-loop-queue-test.sh): drives the count-forge attack on the journal apply and
// reports whether it is inert. It arms two events and applies them, then records a disarm of the
// first at a high slot while forging the append cursor to zero and applies again. Returns true when
// the disarm still ran -- the host walks the full capacity rather than the forged cursor, so no
// link is left pointing at the disarmed event. A regression that made the apply trust the cursor
// would return false.
bool countForgeInertForTest();

}  // namespace workerd
