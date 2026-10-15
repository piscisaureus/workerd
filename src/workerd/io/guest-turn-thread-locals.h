// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/worker-fs.h>
#include <workerd/io/worker.h>
#include <workerd/util/sqlite-metering.h>
#include <workerd/util/thread-scopes.h>

namespace workerd {

// The guest-writable thread-locals the host trusts after a JS turn, saved on the host stack
// frame that runs the turn and put back once the guest has returned.
//
// Each of these thread-locals names something the host dereferences, or counts something the
// host asserts on, after the turn: the current IoContext (IoContext::current(), read from
// host-side destructors), the current Worker::Api (Worker::Api::current()), the active SQLite
// memory scope (the SQLite allocator hook, which the host also drives during database teardown),
// the current temp-directory and symbolic-link-guard scopes (the virtual filesystem), and the
// AllowV8BackgroundThreads / WarnAboutIsolateLock scope depths (assertions). Thread-local storage
// is ordinary user memory that a turn at guest ring 3 can write, so a turn could leave any of
// them naming an object of its choosing; by the provenance rule in
// experimental/guest-kernel/memory-classes.md, a value the host later dereferences must not come
// from memory the guest can write.
//
// save() reads them before the turn into this object, which lives in the host frame that calls
// jsg::runInGuest -- a frame the guest cannot write, because the host-frame wall (see
// gk_run_here_user in gk.h) maps the caller's stack above the turn's entry read-only. restore()
// puts them back afterward, whatever the turn left in the thread-locals, so nothing the turn
// wrote to them outlives it. This is "technique A" of the thread-local work: it is enough for a
// value the host reads only after the turn. A value a signal handler or a forwarded syscall
// consumes mid-turn needs the storage itself out of the guest's reach and does not go through
// here (see thread-scopes.c++ for the watchdog progress counter, which uses the guest kernel's
// per-thread supervisor words, and the turn directory in gk.h for SQLite's VFS root).
//
// IoContext::current() itself is unchanged and adds nothing: this save/restore runs once per
// turn, not on that path.
class GuestTurnThreadLocals {
 public:
  static GuestTurnThreadLocals save() {
    return {
      .ioContext = IoContext::saveThreadLocalForGuestTurn(),
      .api = Worker::Api::saveThreadLocalForGuestTurn(),
      .sqliteMemoryScope = SqliteMemoryScope::saveThreadLocalForGuestTurn(),
      .tmpDirStoreScope = TmpDirStoreScope::saveThreadLocalForGuestTurn(),
      .symbolicLinkGuard = SymbolicLinkRecursionGuardScope::saveThreadLocalForGuestTurn(),
      .scopeCounts = saveThreadScopeCountsForGuestTurn(),
    };
  }

  void restore() {
    IoContext::restoreThreadLocalAfterGuestTurn(ioContext);
    Worker::Api::restoreThreadLocalAfterGuestTurn(api);
    SqliteMemoryScope::restoreThreadLocalAfterGuestTurn(sqliteMemoryScope);
    TmpDirStoreScope::restoreThreadLocalAfterGuestTurn(tmpDirStoreScope);
    SymbolicLinkRecursionGuardScope::restoreThreadLocalAfterGuestTurn(symbolicLinkGuard);
    restoreThreadScopeCountsAfterGuestTurn(scopeCounts);
  }

  // Public so this stays an aggregate for the designated initializer in save(); the members are
  // opaque saved values, only ever read back by restore().
  IoContext* ioContext;
  const Worker::Api* api;
  SqliteMemoryScope* sqliteMemoryScope;
  TmpDirStoreScope* tmpDirStoreScope;
  SymbolicLinkRecursionGuardScope* symbolicLinkGuard;
  ThreadScopeCounts scopeCounts;
};

}  // namespace workerd
