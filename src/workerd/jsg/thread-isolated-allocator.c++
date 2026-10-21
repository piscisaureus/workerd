// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "thread-isolated-allocator.h"

#include "heap-arena.h"

#include <kj/debug.h>
#include <kj/mutex.h>
#include <kj/vector.h>

#if __linux__
#include <sys/mman.h>

#include <cerrno>
#endif

namespace workerd::jsg {
namespace {

#if __linux__

// One slab backs many blocks of one size, so the OS is asked for memory about as often as V8
// grows its index by 64 KiB, which over a process's life is a handful of times.
constexpr size_t SLAB_BYTES = 64 * 1024;

// The sizes V8's index asks for are a few dozen bytes each -- std::map nodes, JitPage objects,
// v8::base::Mutex -- so these classes cover all of it with one slab per class in practice. A
// request above the largest is served by the allocator below this one, out of the shared heap,
// which places it correctly although it leaves it unkeyed.
constexpr size_t CLASS_SIZES[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
constexpr size_t CLASS_COUNT = kj::size(CLASS_SIZES);

struct Slab {
  uintptr_t begin;
  uintptr_t end;
  size_t blockSize;
  // Blocks handed out by the bump pointer so far; the rest of the slab is untouched. A block
  // that comes back is put on its class's free list rather than here.
  size_t handedOut;

  size_t capacity() const {
    return (end - begin) / blockSize;
  }
};

// A v8::ThreadIsolatedAllocator that serves V8's thread-isolation metadata from its own slabs.
// See the header for what that is for.
//
// Nothing here ever writes a byte of a block or of a slab: the free lists hold the blocks'
// addresses out of band, in the ordinary heap, rather than threading a list through the blocks
// themselves. That is what lets the slabs be keyed with a key whose default is to deny writes --
// the allocator would otherwise have to open the key to hand out memory, in places where V8 has
// deliberately closed it.
class SlabThreadIsolatedAllocator final: public v8::ThreadIsolatedAllocator {
 public:
  explicit SlabThreadIsolatedAllocator(v8::ThreadIsolatedAllocator& inner): inner(inner) {}

  // V8's interface declares its destructor noexcept and kj::MutexGuarded's is not, so this
  // would terminate on a throwing one. It is never reached: the one allocator the process has
  // outlives everything (see wrapThreadIsolatedAllocator).
  ~SlabThreadIsolatedAllocator() noexcept override {}
  KJ_DISALLOW_COPY_AND_MOVE(SlabThreadIsolatedAllocator);

  void* Allocate(size_t size) override {
    // Every allocation this makes for its own bookkeeping, and every one it passes down, is for
    // process-wide state although a turn may be what asks for it.
    SharedHeapScope sharedHeap;

    size_t cls = 0;
    while (cls < CLASS_COUNT && CLASS_SIZES[cls] < kj::max(size, size_t(1))) ++cls;
    if (cls == CLASS_COUNT) return inner.Allocate(size);

    auto lock = state.lockExclusive();
    auto& freeList = lock->freeLists[cls];
    if (!freeList.empty()) {
      void* block = freeList.back();
      freeList.removeLast();
      return block;
    }
    for (auto& slab: lock->slabs) {
      if (slab.blockSize == CLASS_SIZES[cls] && slab.handedOut < slab.capacity()) {
        return reinterpret_cast<void*>(slab.begin + slab.handedOut++ * slab.blockSize);
      }
    }
    KJ_IF_SOME(slab, addSlab(*lock, CLASS_SIZES[cls])) {
      return reinterpret_cast<void*>(slab.begin + slab.handedOut++ * slab.blockSize);
    }
    // The OS would not give us a slab. The metadata is then misplaced rather than missing, which
    // costs a turn that reads another isolate's entry rather than the process.
    return inner.Allocate(size);
  }

  void Free(void* object) override {
    if (object == nullptr) return;
    SharedHeapScope sharedHeap;
    auto addr = reinterpret_cast<uintptr_t>(object);
    {
      auto lock = state.lockExclusive();
      for (auto& slab: lock->slabs) {
        if (addr >= slab.begin && addr < slab.end) {
          for (size_t cls = 0; cls < CLASS_COUNT; ++cls) {
            if (CLASS_SIZES[cls] == slab.blockSize) {
              lock->freeLists[cls].add(object);
              return;
            }
          }
          KJ_FAIL_ASSERT("a thread-isolation slab has no size class", slab.blockSize);
        }
      }
    }
    // Not from a slab, so the allocator below served it (see Allocate).
    inner.Free(object);
  }

  enum Type Type() const override {
    return inner.Type();
  }

  int Pkey() const override {
    return inner.Pkey();
  }

 private:
  struct State {
    // Unordered and scanned linearly: there are a handful of slabs and the calls are rare.
    kj::Vector<Slab> slabs;
    kj::Vector<void*> freeLists[CLASS_COUNT];
    bool keyingRefused = false;
  };

  v8::ThreadIsolatedAllocator& inner;
  kj::MutexGuarded<State> state;

  // Takes a slab from the OS for blocks of `blockSize` and records it, or returns none. Caller
  // holds the lock, and holds a SharedHeapScope, since this allocates.
  kj::Maybe<Slab&> addSlab(State& s, size_t blockSize) {
    void* base =
        mmap(nullptr, SLAB_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return kj::none;

    int pkey = Pkey();
    if (pkey >= 0 && !s.keyingRefused &&
        pkey_mprotect(base, SLAB_BYTES, PROT_READ | PROT_WRITE, pkey) != 0) {
      // The slab stays unkeyed, which costs the write protection and nothing else: the metadata
      // is still out of every heap arena, which is what a turn's reads depend on. Reported once
      // per process. A kernel without protection keys is one reason; another is the guest
      // kernel declining to reflect the key (see experimental/guest-kernel/README.md).
      s.keyingRefused = true;
      KJ_LOG(WARNING,
          "V8's thread-isolation metadata could not be keyed; it is writable outside V8's "
          "write scopes",
          pkey, errno);
    }

    auto addr = reinterpret_cast<uintptr_t>(base);
    s.slabs.add(Slab{
      .begin = addr,
      .end = addr + SLAB_BYTES,
      .blockSize = blockSize,
      .handedOut = 0,
    });
    return s.slabs.back();
  }
};

// The key the metadata is served under, once there is one, for ThreadIsolatedWriteScope. Set
// before the allocator is published and never changed after.
int keyInUse = -1;

#endif  // __linux__

}  // namespace

v8::ThreadIsolatedAllocator* wrapThreadIsolatedAllocator(v8::ThreadIsolatedAllocator* inner) {
  if (inner == nullptr) return nullptr;
#ifdef WORKERD_GK_NEG_JIT_MAP_SLABS
  // WORKERD_GK_NEG_JIT_MAP_SLABS is a negative-test build, never shipped: V8's thread-isolation
  // metadata comes from the C++ heap, so the first isolate to grow it inside a turn puts the
  // entry in its own heap arena and the next isolate's turn faults reading it. guest-arena-test
  // must then fail in its part 7.
  return inner;
#elif __linux__
  struct Wrapped {
    v8::ThreadIsolatedAllocator* inner;
    SlabThreadIsolatedAllocator* allocator;
  };
  // The allocator is deliberately never destroyed: V8 keeps the pointer in ThreadIsolation's
  // trusted data and uses it from TearDown, which runs among the exit handlers. A raw pointer
  // also registers nothing with __cxa_atexit, which a static of an owning type would (see
  // __wrap___cxa_atexit in heap-arena.c++). V8 asks the platform for this once, while it
  // initializes thread isolation, so this static's guard is never reached from inside a turn.
  static Wrapped wrapped = [inner]() {
    keyInUse = inner->Pkey();
    return Wrapped{inner, new SlabThreadIsolatedAllocator(*inner)};
  }();
  KJ_REQUIRE(wrapped.inner == inner, "a second thread-isolated allocator reached V8");
  return wrapped.allocator;
#else
  // Only Linux has the mmap and protection-key calls this needs, and only x86-64 Linux has a
  // platform that supplies an allocator at all, so this is unreachable rather than a choice.
  return inner;
#endif
}

ThreadIsolatedWriteScope::ThreadIsolatedWriteScope() {
#if __linux__
  if (keyInUse < 0) return;
  int rights = pkey_get(keyInUse);
  if (rights < 0) return;
  previousRights = static_cast<unsigned>(rights);
  if (pkey_set(keyInUse, 0) != 0) return;
  pkey = keyInUse;
#endif
}

ThreadIsolatedWriteScope::~ThreadIsolatedWriteScope() noexcept(false) {
#if __linux__
  if (pkey < 0) return;
  if (pkey_set(pkey, previousRights) != 0) {
    KJ_LOG(ERROR, "closing the thread-isolation key failed", pkey, errno);
  }
#endif
}

}  // namespace workerd::jsg
