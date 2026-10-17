// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <v8-platform.h>

#include <kj/common.h>

namespace workerd::jsg {

// V8's ThreadIsolation keeps the process-wide index that answers "which code allocation is this
// instruction pointer in?": the `Address -> JitPage*` map, each `JitPage`'s own map of
// allocations, and the two mutexes over them (`src/common/code-memory-access.h`). Every
// allocation of that index goes through the embedder's v8::ThreadIsolatedAllocator --
// ThreadIsolation::StlAllocator for the map nodes, ThreadIsolation::ConstructNew for the pages
// and mutexes -- and the platform's own allocator serves all of it with plain malloc(), above a
// TODO saying it ought to return thread-isolated memory instead. This one, which wraps the
// platform's, fixes the two things that follow from that.
//
// The index is read at guest ring 3 inside a turn: GC-safe code lookup
// (Heap::GcSafeFindCodeForInnerPointer) and stack iteration
// (StackFrameIterator::ComputeStackFrameType) both reach ThreadIsolation::StartOfJitAllocationAt,
// the latter from Runtime_CompileOptimizedOSR, while an optimizing compile in a turn adds entries
// to it. With malloc() behind it, the entries one isolate's turn adds land in that isolate's heap
// arena (see heap-arena.h), where every other isolate's turn faults reading them. This allocator
// serves them from slabs it takes from the OS instead, which are process memory outside every
// arena and so readable from every isolate's turn.
//
// And V8 write-protects JIT code with a protection key while leaving the index that protection
// is bookkept in writable by anything in the process. The slabs are keyed with the same key --
// the one the platform's allocator allocated, which V8 takes as ThreadIsolation::pkey() -- so
// the index is read-only outside the RwxMemoryWriteScope / CFIMetadataWriteScope windows V8
// already enters around every read and write of it.
//
// What that second property is worth is bounded, and not by this code. PKRU is ring-3 writable,
// WRPKRU is unprivileged, and V8 reaches it through glibc's exported pkey_set, so the key stops
// an adversary who can write memory and not one who can redirect control flow. Keeping the index
// out of a turn's reach at all is the page table's job, not the key's.
//
// Returns the allocator to hand V8 in place of `inner`, which is the same object on every call.
// Null for a null `inner`: a platform with no allocator to give leaves V8's thread isolation
// disabled, and the index in the ordinary heap, which is what every platform but x86-64 Linux
// does. `inner` itself where there is nothing to wrap it with, for the same reason.
v8::ThreadIsolatedAllocator* wrapThreadIsolatedAllocator(v8::ThreadIsolatedAllocator* inner);

// Opens the key the thread-isolation metadata is keyed with, on the calling thread, for the
// scope's lifetime.
//
// V8 enters a write scope of its own around every read and write of that metadata but one:
// ThreadIsolation::TearDown, which v8::V8::DisposePlatform calls, deletes the JIT-page map and
// its mutex with neither a CFIMetadataWriteScope nor an RwxMemoryWriteScope. With the stub
// allocator V8 ships that is harmless, because the metadata is ordinary heap; with it really
// keyed the two destructors fault. The fix belongs in V8, next to the write scope
// ThreadIsolation::Initialize already takes around the matching construction; until it is there,
// this is how workerd gets through DisposePlatform.
//
// A no-op where the metadata is not keyed, which is every build but x86-64 Linux's and any
// where the kernel had no key to give.
class ThreadIsolatedWriteScope final {
 public:
  ThreadIsolatedWriteScope();
  ~ThreadIsolatedWriteScope() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(ThreadIsolatedWriteScope);

 private:
  int pkey = -1;
  unsigned previousRights = 0;
};

}  // namespace workerd::jsg
