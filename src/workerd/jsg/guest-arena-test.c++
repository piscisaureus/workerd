// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Per-isolate guest-kernel arenas (see GuestArena in setup.h): each isolate created through
// jsg::newIsolateGroup() gets its own IsolateGroup, whose V8 sandbox lives in a private gk
// arena and whose out-of-sandbox memory (trusted range, code range, cppgc heap, pointer
// tables) lives in that arena's tail, and the isolate lock makes that arena the thread's active
// one. This is the real-V8 counterpart of gk_test.c's arena-isolation test: guest execution
// under one isolate's lock can touch that isolate's ArrayBuffer memory, JIT code, bytecode,
// cppgc objects and pointer tables, and takes a hardware fault on another isolate's.
//
// Where heap arenas are built in (HeapArena in heap-arena.h, with jemalloc as the allocator),
// the isolate lock also puts the thread's malloc() and operator new on the isolate's heap
// arena, in the upper part of the tail, so the same holds for the isolate's C++ objects.

#include "jsg-test.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#include <workerd/jsg/guest-run.h>
#include <workerd/jsg/heap-arena.h>

#ifdef WORKERD_HAS_HEAP_ARENA
#include <jemalloc/jemalloc.h>
#endif

#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>
#include <cppgc/persistent.h>
#include <unistd.h>
#include <v8-cppgc.h>
#include <v8-internal.h>

#include <kj/map.h>

#include <cstdlib>
#include <cstring>
#endif

namespace workerd::jsg::test {
namespace {

#ifdef WORKERD_HAS_GUEST_KERNEL

struct ArenaContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(ArenaContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(ArenaIsolate, ArenaContext);

constexpr size_t BUFFER_SIZE = 64 * 1024;
constexpr unsigned char BUFFER_FILL = 0x5a;

// Guest-side probe: reads the byte at `arg`, writes it back and returns it. Runs inside the
// guest via gk_run_here() (ring 0) or gk_run_here_user() (ring 3); an access to memory the
// active arena cannot address ends the run with GK_EFAULT instead of returning.
long touchByte(void* arg) {
  auto* p = reinterpret_cast<volatile unsigned char*>(arg);
  unsigned char v = *p;
  *p = v;
  return v;
}

// Allocates an ArrayBuffer in `isolate`'s sandbox and fills it. The backing store is returned
// so that the buffer stays alive (and its memory committed) after the lock is released.
std::shared_ptr<v8::BackingStore> allocateBuffer(ArenaIsolate& isolate) {
  std::shared_ptr<v8::BackingStore> store;
  isolate.runInLockScope([&](ArenaIsolate::Lock& lock) {
    jsg::Lock& js = lock;
    js.withinHandleScope([&] {
      auto context = lock.newContext<ArenaContext>().getHandle(js);
      v8::Context::Scope contextScope(context);
      auto buffer = v8::ArrayBuffer::New(js.v8Isolate, BUFFER_SIZE);
      store = buffer->GetBackingStore();
      memset(store->Data(), BUFFER_FILL, BUFFER_SIZE);
    });
  });
  return store;
}

// Guest-side probe: reads the byte at `arg` and returns it. Unlike touchByte() it never
// writes, so it can probe JIT code, which may be write-protected.
long readByte(void* arg) {
  return *reinterpret_cast<volatile unsigned char*>(arg);
}

bool contains(GuestArena& arena, void* p) {
  auto addr = reinterpret_cast<uintptr_t>(p);
  auto base = reinterpret_cast<uintptr_t>(arena.base());
  return addr >= base && addr < base + arena.size();
}

bool inTail(GuestArena& arena, void* p) {
  return arena.getPageAllocator().contains(p);
}

// gk must be initialized before V8 starts (it maps the address space as it is then), and
// newIsolateGroup() places isolates in arenas only when guest-kernel isolation is enabled,
// which is decided from the environment on first use. Returns false if gk is unavailable here.
bool enableGuestKernel() {
  setenv("WORKERD_EXPERIMENTAL_GUEST_KERNEL", "1", 1);
  if (gk_init() >= 0) {
    KJ_ASSERT(isGuestKernelEnabled());
    return true;
  }
  kj::StringPtr reason = gk_last_error() != nullptr ? gk_last_error() : "unknown";
  // A machine without a usable /dev/kvm is the one reason to skip: gk cannot run there at all,
  // and the hosts this is built on need not offer one. Every other failure belongs to this
  // build, and treating it as "not supported here" is what left everything below unrun.
  KJ_ASSERT(access("/dev/kvm", R_OK | W_OK) != 0,
      "the guest kernel did not initialize although /dev/kvm is usable", reason);
  KJ_LOG(WARNING, "skipping: no usable /dev/kvm", reason);
  return false;
}

// A cppgc-managed object, allocated in an isolate's CppHeap.
struct CppgcProbe final: public cppgc::GarbageCollected<CppgcProbe> {
  void Trace(cppgc::Visitor*) const {}
  unsigned char fill = BUFFER_FILL;
};

// Addresses of the code V8 generates while a script runs, collected from the isolate's JIT
// code events (v8::Isolate::SetJitCodeEventHandler): machine code lives in the code range,
// bytecode in the trusted range.
struct CodeAddresses {
  kj::Vector<void*> jitCode;
  kj::Vector<void*> bytecode;
};
CodeAddresses* jitEventSink = nullptr;
void onJitCodeEvent(const v8::JitCodeEvent* event) {
  if (jitEventSink == nullptr || event->type != v8::JitCodeEvent::CODE_ADDED) return;
  switch (event->code_type) {
    case v8::JitCodeEvent::JIT_CODE:
      jitEventSink->jitCode.add(event->code_start);
      break;
    case v8::JitCodeEvent::BYTE_CODE:
      jitEventSink->bytecode.add(event->code_start);
      break;
    case v8::JitCodeEvent::WASM_CODE:
      break;
  }
}

// One isolate's out-of-sandbox memory, populated so that each region has something to probe.
struct OutOfSandboxMemory {
  void* codeRangeStart = nullptr;
  size_t codeRangeSize = 0;
  void* jitCode = nullptr;   // optimized machine code of a function, in the code range
  void* bytecode = nullptr;  // that function's bytecode, in the trusted range
  cppgc::Persistent<CppgcProbe> cppgcObject;
};

// Compiles and optimizes a function in `isolate` (so its code range holds JIT code and its
// trusted range the function's bytecode) and allocates a cppgc object in its CppHeap. Runs on
// the host; only the probes below run in the guest.
void populateOutOfSandboxMemory(ArenaIsolate& isolate, OutOfSandboxMemory& memory) {
  isolate.runInLockScope([&](ArenaIsolate::Lock& lock) {
    jsg::Lock& js = lock;
    js.withinHandleScope([&] {
      auto context = lock.newContext<ArenaContext>().getHandle(js);
      v8::Context::Scope contextScope(context);

      js.v8Isolate->GetCodeRange(&memory.codeRangeStart, &memory.codeRangeSize);

      CodeAddresses addresses;
      jitEventSink = &addresses;
      js.v8Isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, &onJitCodeEvent);
      static constexpr const char* CODE =
          "function f(n, k) { let s = 0; for (let i = 0; i < n; i++) s += i * k; return s; }"
          "%PrepareFunctionForOptimization(f);"
          "f(10, 2); f(20, 3);"
          "%OptimizeFunctionOnNextCall(f);"
          "f(30, 4);";
      auto source = v8Str(js.v8Isolate, kj::StringPtr(CODE));
      auto script = check(v8::Script::Compile(context, source));
      auto value = check(script->Run(context));
      KJ_EXPECT(check(value->NumberValue(context)) == 435 * 4);
      js.v8Isolate->SetJitCodeEventHandler(v8::kJitCodeEventDefault, nullptr);
      jitEventSink = nullptr;

      auto codeRange = reinterpret_cast<uintptr_t>(memory.codeRangeStart);
      for (void* p: addresses.jitCode) {
        auto addr = reinterpret_cast<uintptr_t>(p);
        if (addr >= codeRange && addr < codeRange + memory.codeRangeSize) {
          memory.jitCode = p;
          break;
        }
      }
      KJ_ASSERT(memory.jitCode != nullptr, "no JIT code was generated in the code range");
      KJ_ASSERT(!addresses.bytecode.empty(), "no bytecode was generated");
      memory.bytecode = addresses.bytecode[0];

      memory.cppgcObject = cppgc::MakeGarbageCollected<CppgcProbe>(
          js.v8Isolate->GetCppHeap()->GetAllocationHandle());
    });
  });
}

// From the guest, every region of `memory` is readable (and the cppgc object writable) when
// its isolate's arena is active, and unaddressable when `reachable` is false.
void probeOutOfSandboxMemory(OutOfSandboxMemory& memory, bool reachable) {
  auto expect = [&](long (*probe)(void*), void* p, long expected) {
    long result = gk_run_here(probe, p);
    if (reachable) {
      KJ_EXPECT(result == expected, p, result, expected);
    } else {
      KJ_EXPECT(result == GK_EFAULT, p, result);
      KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(p));
    }
  };
  expect(&readByte, memory.jitCode, *static_cast<unsigned char*>(memory.jitCode));
  expect(&readByte, memory.bytecode, *static_cast<unsigned char*>(memory.bytecode));
  expect(&touchByte, &memory.cppgcObject->fill, BUFFER_FILL);
}

// The base addresses of an isolate's pointer tables, which V8 keeps outside of the sandbox and
// which an isolate group's page allocator places in the arena's tail. They are read from the
// isolate's data area at the offsets that V8's public API layout guarantees (the same way its
// inline API locates the external pointer table). Every table starts with a read-only segment
// that is mapped from the moment the table is initialized, so each base is a readable page.
struct PointerTables {
  void* externalPointerTable = nullptr;
  void* sharedExternalPointerTable = nullptr;
  void* cppHeapPointerTable = nullptr;
  void* trustedPointerTable = nullptr;
  void* sharedTrustedPointerTable = nullptr;
  void* jsDispatchTable = nullptr;

  template <typename Func>
  void forEach(Func&& func) {
    func("external pointer table", externalPointerTable);
    func("shared external pointer table", sharedExternalPointerTable);
    func("cpp heap pointer table", cppHeapPointerTable);
    func("trusted pointer table", trustedPointerTable);
    func("shared trusted pointer table", sharedTrustedPointerTable);
    func("JS dispatch table", jsDispatchTable);
  }
};

PointerTables getPointerTables(v8::Isolate* isolate) {
  using Internals = v8::internal::Internals;
  auto root = reinterpret_cast<uintptr_t>(isolate);
  // A table embedded in the isolate data: its base pointer is its first field.
  auto tableBase = [&](int tableOffset) {
    return *reinterpret_cast<void**>(
        root + tableOffset + Internals::kExternalEntityTableBasePointerOffset);
  };
  // A shared table, which the isolate data points to.
  auto sharedTableBase = [&](int tableAddressOffset) {
    auto table = *reinterpret_cast<uintptr_t*>(root + tableAddressOffset);
    return *reinterpret_cast<void**>(table + Internals::kExternalEntityTableBasePointerOffset);
  };
  PointerTables tables;
  tables.externalPointerTable = tableBase(Internals::kIsolateExternalPointerTableOffset);
  tables.sharedExternalPointerTable =
      sharedTableBase(Internals::kIsolateSharedExternalPointerTableAddressOffset);
  tables.cppHeapPointerTable = tableBase(Internals::kIsolateCppHeapPointerTableOffset);
  tables.trustedPointerTable = tableBase(Internals::kIsolateTrustedPointerTableOffset);
  tables.sharedTrustedPointerTable =
      sharedTableBase(Internals::kIsolateSharedTrustedPointerTableAddressOffset);
  tables.jsDispatchTable = tableBase(Internals::kIsolateJSDispatchTableOffset);
  // V8's own accessor for the external pointer table must agree with the layout used above.
  KJ_ASSERT(tables.externalPointerTable == Internals::GetExternalPointerTableBase(isolate));
  KJ_ASSERT(
      tables.sharedExternalPointerTable == Internals::GetSharedExternalPointerTableBase(isolate));
  return tables;
}

// From the guest, the first page of each of `tables` is readable when its isolate's arena is
// active, and unaddressable when `reachable` is false. The host is never confined, so the
// expected byte is read directly.
void probePointerTables(PointerTables& tables, bool reachable) {
  tables.forEach([&](kj::StringPtr name, void* base) {
    long result = gk_run_here(&readByte, base);
    if (reachable) {
      KJ_EXPECT(result == *static_cast<unsigned char*>(base), name, base, result);
    } else {
      KJ_EXPECT(result == GK_EFAULT, name, base, result);
      KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(base), name, base);
    }
  });
}

// Positive control: one JS turn run inside the guest under an isolate's lock, allocating heap
// objects and typed arrays in that isolate's sandbox and touching them. Exceptions must not
// unwind across the guest boundary, so the result is reported through the struct.
struct JsTurn {
  ArenaIsolate::Lock& lock;
  double result = -1;
  bool threw = false;

  static long run(void* arg) {
    auto& self = *reinterpret_cast<JsTurn*>(arg);
    try {
      jsg::Lock& js = self.lock;
      js.withinHandleScope([&] {
        auto context = self.lock.newContext<ArenaContext>().getHandle(js);
        v8::Context::Scope contextScope(context);
        static constexpr const char* CODE =
            "const objs = [];"
            "for (let i = 0; i < 10000; i++) objs.push({i, s: 'item' + i, a: [i, i + 1]});"
            "const bytes = new Uint8Array(4 * 1024 * 1024);"
            "bytes.fill(7);"
            "const floats = new Float64Array(1024);"
            "for (let i = 0; i < floats.length; i++) floats[i] = i * 0.5;"
            "let sum = 0;"
            "for (let i = 0; i < bytes.length; i += 4096) sum += bytes[i];"
            "for (const o of objs) sum += o.a[1] - o.a[0];"
            "sum + floats[1023];";
        auto source = v8Str(js.v8Isolate, kj::StringPtr(CODE));
        auto script = check(v8::Script::Compile(context, source));
        auto value = check(script->Run(context));
        self.result = check(value->NumberValue(context));
      });
    } catch (...) {
      self.threw = true;
    }
    return 0;
  }
};

#ifdef WORKERD_HAS_HEAP_ARENA
// Allocation sizes spanning jemalloc's small, large and huge size classes.
constexpr size_t HEAP_SIZES[] = {16, 96, 1024, 4096, 20000, 300 * 1024, 3 * 1024 * 1024};
constexpr size_t HEAP_COUNT = kj::size(HEAP_SIZES);

// One round of C++ heap allocations, made by code running at guest ring 3 or on the host and
// read back by the host afterwards. A record is allocated with no lock held, so it lies in the
// shared heap: a turn under any arena can read it and allocate() can write it (it is off the
// stack, so the host-frame wall does not apply), and release() writes nothing to it.
struct HeapTurn {
  void* blocks[HEAP_COUNT] = {};
  unsigned char* object = nullptr;
  unsigned arenaIndex = 0;

  // Allocates a block of each size with malloc() and an array with new, filling each.
  static long allocate(void* arg) {
    auto& self = *reinterpret_cast<HeapTurn*>(arg);
    self.arenaIndex = currentHeapArenaIndex();
    for (size_t i = 0; i < HEAP_COUNT; i++) {
      void* p = malloc(HEAP_SIZES[i]);
      if (p == nullptr) return -1;
      memset(p, BUFFER_FILL, HEAP_SIZES[i]);
      self.blocks[i] = p;
    }
    self.object = new unsigned char[64];
    memset(self.object, BUFFER_FILL, 64);
    return 0;
  }

  // As allocate(), inside a SharedHeapScope.
  static long allocateShared(void* arg) {
    SharedHeapScope sharedHeap;
    return allocate(arg);
  }

  // Frees what allocate() recorded, under whatever arena is active at the time.
  static long release(void* arg) {
    auto& self = *reinterpret_cast<HeapTurn*>(arg);
    for (void* p: self.blocks) free(p);
    delete[] self.object;
    return 0;
  }

  template <typename Func>
  void forEach(Func&& func) {
    for (size_t i = 0; i < HEAP_COUNT; i++) func(blocks[i], HEAP_SIZES[i]);
    func(static_cast<void*>(object), size_t(64));
  }
};

bool inHeap(GuestArena& arena, const void* p) {
  return KJ_ASSERT_NONNULL(arena.getHeapArena()).contains(p);
}

// One of the shared arena's statistics, read fresh: jemalloc serves them from a snapshot that a
// write to `epoch` refreshes. T must be the control's own type, which mallctl checks.
template <typename T>
T sharedArenaStat(kj::StringPtr name) {
  uint64_t epoch = 1;
  size_t epochSize = sizeof(epoch);
  KJ_REQUIRE(mallctl("epoch", &epoch, &epochSize, &epoch, sizeof(epoch)) == 0);
  auto key = kj::str("stats.arenas.0.", name);
  T value = 0;
  size_t size = sizeof(value);
  KJ_REQUIRE(mallctl(key.cStr(), &value, &size, nullptr, 0) == 0, key);
  return value;
}
#endif  // WORKERD_HAS_HEAP_ARENA

// A single case with a single pair of isolates that every check up to part 6 shares; part 7
// creates and destroys more. Natives syntax lets a script force optimization, so that an
// isolate's code range holds JIT code.
KJ_TEST("each isolate's memory lives in its own guest-kernel arena") {
  if (!enableGuestKernel()) return;

  V8System v8System({"--allow-natives-syntax"_kj});
#ifdef WORKERD_HAS_HEAP_ARENA
  // Before the first HeapArena exists, this thread is the shared arena's only one; see part 0.
  unsigned sharedThreadsBefore = sharedArenaStat<unsigned>("nthreads");
#endif
  ArenaIsolate a(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  ArenaIsolate b(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());

  auto& arenaA = KJ_ASSERT_NONNULL(a.getGuestArena());
  auto& arenaB = KJ_ASSERT_NONNULL(b.getGuestArena());
  KJ_EXPECT(arenaA.get() != arenaB.get());
  KJ_EXPECT(arenaA.base() != arenaB.base());
  KJ_EXPECT(!contains(arenaA, arenaB.base()));
  KJ_EXPECT(!contains(arenaB, arenaA.base()));

  // The tail follows the sandbox reservation within the arena, and is disjoint between arenas.
  auto& tailA = arenaA.getPageAllocator();
  auto& tailB = arenaB.getPageAllocator();
  KJ_EXPECT(tailA.begin() == reinterpret_cast<uintptr_t>(arenaA.base()) + arenaA.size());
  KJ_EXPECT(tailB.begin() == reinterpret_cast<uintptr_t>(arenaB.base()) + arenaB.size());
  KJ_EXPECT(tailA.end() > tailA.begin());
  KJ_EXPECT(tailA.end() <= tailB.begin() || tailB.end() <= tailA.begin());

#ifdef WORKERD_HAS_HEAP_ARENA
  // Part 0: the shared arena has a thread of its own from the first HeapArena on
  // (keepSharedHeapArenaPopulated() in heap-arena.c++), so that leaving it for an isolate's
  // arena never purges it: jemalloc purges an arena when the thread leaving it was the last one
  // bound to it. Checked before any lock is taken, while this thread and the keeper are the
  // only ones the shared arena can have: jemalloc binds every other thread on its first
  // allocation to the automatic arena with the fewest threads, which the shared arena is not
  // while this thread is on it but is during a turn, so after the parts below the count says
  // nothing about the keeper.
  KJ_EXPECT(sharedThreadsBefore == 1, sharedThreadsBefore);
  KJ_EXPECT(sharedArenaStat<unsigned>("nthreads") == 2);
  {
    auto& heapA = KJ_ASSERT_NONNULL(arenaA.getHeapArena());
    KJ_EXPECT(currentHeapArenaIndex() == 0);
    {
      // Free some MiB on the shared arena, so that it has dirty pages a purge would return.
      kj::Vector<kj::Array<kj::byte>> blocks;
      for (int i = 0; i < 16; i++) {
        auto block = kj::heapArray<kj::byte>(1024 * 1024);
        memset(block.begin(), BUFFER_FILL, block.size());
        blocks.add(kj::mv(block));
      }
    }
    size_t dirtyBefore = sharedArenaStat<size_t>("pdirty");
    KJ_EXPECT(dirtyBefore > 0);
    uint64_t purgesBefore = sharedArenaStat<uint64_t>("dirty_npurge");
    for (int i = 0; i < 100; i++) {
      HeapArenaScope scope(heapA);
      KJ_EXPECT(currentHeapArenaIndex() == heapA.getIndex());
    }
    KJ_EXPECT(currentHeapArenaIndex() == 0);
    KJ_EXPECT(sharedArenaStat<uint64_t>("dirty_npurge") == purgesBefore);
    KJ_EXPECT(sharedArenaStat<size_t>("pdirty") == dirtyBefore);
  }
#endif  // WORKERD_HAS_HEAP_ARENA

  // Part 1: the sandbox. ArrayBuffer memory lives in the sandbox, which lies in the arena.

  auto storeA = allocateBuffer(a);
  auto storeB = allocateBuffer(b);
  void* bufA = storeA->Data();
  void* bufB = storeB->Data();
  KJ_LOG(INFO, "arenas", arenaA.base(), arenaA.size(), bufA, arenaB.base(), arenaB.size(), bufB);

  // Each buffer lies in its own isolate's arena, and each group's sandbox is disjoint from the
  // other's.
  KJ_EXPECT(contains(arenaA, bufA));
  KJ_EXPECT(!contains(arenaB, bufA));
  KJ_EXPECT(contains(arenaB, bufB));
  KJ_EXPECT(!contains(arenaA, bufB));
  {
    auto groupA = a.getIsolate()->GetGroup();
    auto groupB = b.getIsolate()->GetGroup();
    KJ_EXPECT(groupA != groupB);
    KJ_EXPECT(groupA.SandboxContains(bufA));
    KJ_EXPECT(!groupA.SandboxContains(bufB));
    KJ_EXPECT(groupB.SandboxContains(bufB));
    KJ_EXPECT(!groupB.SandboxContains(bufA));
  }

  // Under A's lock (arena A active): A's buffer is reachable from the guest, B's faults.
  a.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(gk_run_here(&touchByte, bufA) == BUFFER_FILL);
    KJ_EXPECT(gk_run_here(&touchByte, bufB) == GK_EFAULT);
    KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(bufB));

    // The same from ring 3, which is where production runs JS turns (IoContext::runImpl uses
    // gk_run_here_user): the arena wall must hold on the user-mode path too, where the page
    // tables' user bit is checked as well as the root. A's buffer is reachable, B's faults.
    KJ_EXPECT(gk_run_here_user(&touchByte, bufA) == BUFFER_FILL);
    KJ_EXPECT(gk_run_here_user(&touchByte, bufB) == GK_EFAULT);
    KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(bufB));

    JsTurn turn{lock};
    KJ_EXPECT(gk_run_here(&JsTurn::run, &turn) == 0);
    KJ_EXPECT(!turn.threw);
    // 1024 pages of bytes[i] == 7, plus 10000 objects contributing 1 each, plus floats[1023].
    KJ_EXPECT(turn.result == 1024 * 7 + 10000 + 511.5, turn.result);
  });

  // Mirror under B's lock, from ring 0 and ring 3.
  b.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(gk_run_here(&touchByte, bufB) == BUFFER_FILL);
    KJ_EXPECT(gk_run_here(&touchByte, bufA) == GK_EFAULT);
    KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(bufA));
    KJ_EXPECT(gk_run_here_user(&touchByte, bufB) == BUFFER_FILL);
    KJ_EXPECT(gk_run_here_user(&touchByte, bufA) == GK_EFAULT);
    KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(bufA));
  });

  // With no lock held the thread is back on the base root, from which neither is reachable.
  KJ_EXPECT(gk_run_here(&touchByte, bufA) == GK_EFAULT);
  KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(bufA));
  KJ_EXPECT(gk_run_here(&touchByte, bufB) == GK_EFAULT);
  KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(bufB));

  // The host is never confined: both buffers still hold their fill.
  KJ_EXPECT(static_cast<unsigned char*>(bufA)[0] == BUFFER_FILL);
  KJ_EXPECT(static_cast<unsigned char*>(bufB)[BUFFER_SIZE - 1] == BUFFER_FILL);

  // Release the buffers before their isolates (and groups, and arenas) go away.
  storeA.reset();
  storeB.reset();

  // Part 2: the out-of-sandbox memory, in the arena's tail.
  // Declared after the isolates so that the cppgc handles are released before their heaps.
  OutOfSandboxMemory memoryA, memoryB;
  populateOutOfSandboxMemory(a, memoryA);
  populateOutOfSandboxMemory(b, memoryB);
  KJ_LOG(INFO, "out-of-sandbox memory", tailA.begin(), tailA.end(), memoryA.codeRangeStart,
      memoryA.codeRangeSize, memoryA.jitCode, memoryA.bytecode,
      static_cast<void*>(memoryA.cppgcObject.Get()), tailB.begin(), tailB.end(),
      memoryB.codeRangeStart, memoryB.codeRangeSize, memoryB.jitCode, memoryB.bytecode,
      static_cast<void*>(memoryB.cppgcObject.Get()));

  // Each isolate's code range, trusted range (holding the bytecode) and cppgc heap lie in its
  // own arena's tail, outside its sandbox.
  auto expectInTail = [](GuestArena& arena, OutOfSandboxMemory& memory) {
    auto& tail = arena.getPageAllocator();
    KJ_EXPECT(inTail(arena, memory.codeRangeStart));
    KJ_EXPECT(
        reinterpret_cast<uintptr_t>(memory.codeRangeStart) + memory.codeRangeSize <= tail.end());
    KJ_EXPECT(inTail(arena, memory.jitCode));
    KJ_EXPECT(inTail(arena, memory.bytecode));
    auto codeRange = reinterpret_cast<uintptr_t>(memory.codeRangeStart);
    auto bytecode = reinterpret_cast<uintptr_t>(memory.bytecode);
    KJ_EXPECT(bytecode < codeRange || bytecode >= codeRange + memory.codeRangeSize);
    KJ_EXPECT(inTail(arena, memory.cppgcObject.Get()));
    KJ_EXPECT(!contains(arena, memory.codeRangeStart));
    KJ_EXPECT(!contains(arena, memory.bytecode));
    KJ_EXPECT(!contains(arena, memory.cppgcObject.Get()));
  };
  expectInTail(arenaA, memoryA);
  expectInTail(arenaB, memoryB);
  KJ_EXPECT(!inTail(arenaB, memoryA.jitCode));
  KJ_EXPECT(!inTail(arenaA, memoryB.jitCode));

  // The allocator refuses an address outside the tail it manages. V8 asks it to map, protect
  // and discard pages by address, and those addresses come from V8's own state, which lives in
  // the arena and is therefore writable by guest execution; letting one through would hand
  // guest-chosen addresses to mprotect() and MAP_FIXED. The end of the tail is the interesting
  // case: it is page-aligned and above every address the tail holds.
  constexpr size_t pageSize = 4096;
  auto refusesOutside = [](ArenaPageAllocator& tail, uintptr_t address) {
    auto* p = reinterpret_cast<void*>(address);
    KJ_EXPECT_THROW_MESSAGE(
        "not in the arena tail", tail.SetPermissions(p, pageSize, v8::PageAllocator::kNoAccess));
    KJ_EXPECT_THROW_MESSAGE("not in the arena tail", tail.DiscardSystemPages(p, pageSize));
    KJ_EXPECT_THROW_MESSAGE("not in the arena tail", tail.DecommitPages(p, pageSize));
  };
  refusesOutside(tailA, tailA.end());
  refusesOutside(tailA, tailA.begin() - pageSize);
  refusesOutside(tailA, tailB.begin());
  refusesOutside(tailB, tailA.begin());

  // Under A's lock (arena A active): A's code, bytecode and cppgc object are reachable from
  // the guest, B's fault. Mirror under B's lock.
  a.runInLockScope([&](ArenaIsolate::Lock& lock) {
    probeOutOfSandboxMemory(memoryA, true);
    probeOutOfSandboxMemory(memoryB, false);
  });
  b.runInLockScope([&](ArenaIsolate::Lock& lock) {
    probeOutOfSandboxMemory(memoryB, true);
    probeOutOfSandboxMemory(memoryA, false);
  });

  // With no lock held the thread is back on the base root, from which neither is reachable.
  probeOutOfSandboxMemory(memoryA, false);
  probeOutOfSandboxMemory(memoryB, false);

  // Part 3: the pointer tables, also in the arena's tail. Each isolate owns its shared tables
  // too, as it is alone in its group.
  auto tablesA = getPointerTables(a.getIsolate());
  auto tablesB = getPointerTables(b.getIsolate());
  KJ_LOG(INFO, "pointer tables", tablesA.externalPointerTable, tablesA.sharedExternalPointerTable,
      tablesA.cppHeapPointerTable, tablesA.trustedPointerTable, tablesA.sharedTrustedPointerTable,
      tablesA.jsDispatchTable, tablesB.externalPointerTable, tablesB.sharedExternalPointerTable,
      tablesB.cppHeapPointerTable, tablesB.trustedPointerTable, tablesB.sharedTrustedPointerTable,
      tablesB.jsDispatchTable);

  // Every table of an isolate lies in its own arena's tail, outside its sandbox and outside the
  // other isolate's arena, and no two tables share a base.
  auto expectTablesInTail = [](GuestArena& own, GuestArena& other, PointerTables& tables) {
    kj::Vector<void*> bases;
    tables.forEach([&](kj::StringPtr name, void* base) {
      KJ_EXPECT(base != nullptr, name);
      KJ_EXPECT(inTail(own, base), name, base);
      KJ_EXPECT(!contains(own, base), name, base);
      KJ_EXPECT(!inTail(other, base), name, base);
      KJ_EXPECT(!contains(other, base), name, base);
      for (void* seen: bases) KJ_EXPECT(seen != base, name, base);
      bases.add(base);
    });
  };
  expectTablesInTail(arenaA, arenaB, tablesA);
  expectTablesInTail(arenaB, arenaA, tablesB);

  // Under A's lock (arena A active): A's tables are readable from the guest, B's fault. Mirror
  // under B's lock.
  a.runInLockScope([&](ArenaIsolate::Lock& lock) {
    probePointerTables(tablesA, true);
    probePointerTables(tablesB, false);
  });
  b.runInLockScope([&](ArenaIsolate::Lock& lock) {
    probePointerTables(tablesB, true);
    probePointerTables(tablesA, false);
  });

  // With no lock held the thread is back on the base root, from which neither is reachable.
  probePointerTables(tablesA, false);
  probePointerTables(tablesB, false);

  // Part 4: turns. runInGuest() runs its body at ring 3 and reports what it did through the
  // heap turn record, which the host reads back as bytes (see GuestTurnOutcome): a return value
  // comes back, a kj::Exception is rethrown host-side with its type and message intact, an
  // over-long description is cut at the record's capacity, and JsExceptionThrown is re-raised.
  a.runInLockScope([&](ArenaIsolate::Lock& lock) {
    jsg::Lock& js = lock;
    auto onFault = [](const kj::Exception& e) { KJ_FAIL_ASSERT("turn faulted", e); };

    KJ_EXPECT(runInGuest(js, []() { return 42; }, onFault) == 42);

    auto thrown = kj::runCatchingExceptions([&]() {
      runInGuest(js,
          []() { kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "from the guest")); }, onFault);
    });
    auto& e = KJ_ASSERT_NONNULL(thrown);
    KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED);
    KJ_EXPECT(e.getDescription() == "from the guest", e.getDescription());

    auto longThrown = kj::runCatchingExceptions([&]() {
      runInGuest(js, []() {
        auto message = kj::heapArray<char>(2 * _::GuestTurnOutcome::MAX_DESCRIPTION);
        message.asPtr().fill('y');
        kj::throwFatalException(KJ_EXCEPTION(FAILED, kj::str(message)));
      }, onFault);
    });
    auto& longE = KJ_ASSERT_NONNULL(longThrown);
    KJ_EXPECT(longE.getDescription().size() == _::GuestTurnOutcome::MAX_DESCRIPTION);
    KJ_EXPECT(longE.getDescription().endsWith("yyyy"));

    bool jsThrown = false;
    try {
      runInGuest(js, []() { throw JsExceptionThrown(); }, onFault);
    } catch (const JsExceptionThrown&) {
      jsThrown = true;
    }
    KJ_EXPECT(jsThrown);
  });

#ifdef WORKERD_HAS_HEAP_ARENA
  // Part 5: the C++ heap. Each isolate has a jemalloc arena over the heap part of its arena's
  // tail, and its lock puts the thread's malloc() and operator new on it (HeapArenaScope, via
  // GuestArenaScope), whether the allocation is made on the host side of the lock or at guest
  // ring 3 inside a turn. With no lock held the thread allocates from one of jemalloc's
  // automatic arenas, in no isolate's memory.
  KJ_EXPECT(heapArenasAvailable());
  auto& heapA = KJ_ASSERT_NONNULL(arenaA.getHeapArena());
  auto& heapB = KJ_ASSERT_NONNULL(arenaB.getHeapArena());
  KJ_EXPECT(heapA.getIndex() != heapB.getIndex());
  KJ_EXPECT(!isAutomaticHeapArena(heapA.getIndex()));
  KJ_EXPECT(!isAutomaticHeapArena(heapB.getIndex()));

  // The heap part of the tail follows V8's part and ends at the end of the reservation, which
  // gk rounds up to a whole 512 GiB slot.
  constexpr uintptr_t SLOT = uintptr_t(1) << 39;
  auto reservationEnd = [](GuestArena& arena) {
    auto base = reinterpret_cast<uintptr_t>(arena.base());
    return (base + arena.size() + SLOT - 1) / SLOT * SLOT;
  };
  KJ_EXPECT(heapA.getBegin() == tailA.end());
  KJ_EXPECT(heapA.getEnd() == reservationEnd(arenaA));
  KJ_EXPECT(heapB.getBegin() == tailB.end());
  KJ_EXPECT(heapB.getEnd() == reservationEnd(arenaB));

  // The isolate objects themselves are in the shared heap: isolate creation stays off the heap
  // arena (see newIsolate() in setup.c++).
  KJ_EXPECT(!inHeap(arenaA, a.getIsolate()) && !inHeap(arenaB, a.getIsolate()));
  KJ_EXPECT(!inHeap(arenaA, b.getIsolate()) && !inHeap(arenaB, b.getIsolate()));

  // No lock held: the thread is on an automatic arena, and what it allocates lies in neither
  // isolate's arena. The turn records are allocated here, in the shared heap.
  unsigned homeIndex = currentHeapArenaIndex();
  KJ_EXPECT(isAutomaticHeapArena(homeIndex), homeIndex);
  auto hostTurn = kj::heap<HeapTurn>();
  auto turnA = kj::heap<HeapTurn>();
  auto turnB = kj::heap<HeapTurn>();
  KJ_EXPECT(!inHeap(arenaA, hostTurn.get()) && !inHeap(arenaB, hostTurn.get()));
  KJ_EXPECT(HeapTurn::allocate(hostTurn.get()) == 0);
  KJ_EXPECT(hostTurn->arenaIndex == homeIndex);
  hostTurn->forEach([&](void* p, size_t size) {
    KJ_EXPECT(!inHeap(arenaA, p) && !inHeap(arenaB, p), p, size);
    KJ_EXPECT(!contains(arenaA, p) && !contains(arenaB, p), p, size);
    KJ_EXPECT(!inTail(arenaA, p) && !inTail(arenaB, p), p, size);
  });
  KJ_EXPECT(HeapTurn::release(hostTurn.get()) == 0);

  // Under A's lock the thread is on A's heap arena. Allocations made on the host side of the
  // lock and at guest ring 3 inside a turn both land in A's heap region (not in its sandbox or
  // V8's part of the tail), where the guest can read and write them.
  a.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(currentHeapArenaIndex() == heapA.getIndex());
    KJ_EXPECT(HeapTurn::allocate(hostTurn.get()) == 0);
    KJ_EXPECT(hostTurn->arenaIndex == heapA.getIndex());
    hostTurn->forEach([&](void* p, size_t size) {
      KJ_EXPECT(inHeap(arenaA, p), p, size);
      KJ_EXPECT(!inHeap(arenaB, p), p, size);
    });

    KJ_EXPECT(gk_run_here_user(&HeapTurn::allocate, turnA.get()) == 0);
    KJ_EXPECT(turnA->arenaIndex == heapA.getIndex());
    turnA->forEach([&](void* p, size_t size) {
      KJ_EXPECT(inHeap(arenaA, p), p, size);
      KJ_EXPECT(!inHeap(arenaB, p), p, size);
      KJ_EXPECT(!contains(arenaA, p) && !inTail(arenaA, p), p, size);
      KJ_EXPECT(gk_run_here_user(&touchByte, p) == BUFFER_FILL, p, size);
      KJ_EXPECT(gk_run_here(&touchByte, static_cast<kj::byte*>(p) + size - 1) == BUFFER_FILL);
    });
  });
  KJ_EXPECT(currentHeapArenaIndex() == homeIndex);

  // With no lock held the thread is back on the base root: A's heap objects are unaddressable
  // from the guest, while the host still reads them.
  turnA->forEach([&](void* p, size_t size) {
    KJ_EXPECT(gk_run_here(&touchByte, p) == GK_EFAULT, p, size);
    KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(p));
    KJ_EXPECT(static_cast<unsigned char*>(p)[0] == BUFFER_FILL);
    KJ_EXPECT(static_cast<unsigned char*>(p)[size - 1] == BUFFER_FILL);
  });

  // Under B's lock: B's allocations land in B's heap region; A's heap objects fault; and A's
  // objects can be freed from B's turn, at ring 3 with A's arena unmapped, since a free touches
  // only the allocator's shared bookkeeping and never the object.
  b.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(currentHeapArenaIndex() == heapB.getIndex());
    KJ_EXPECT(gk_run_here_user(&HeapTurn::allocate, turnB.get()) == 0);
    KJ_EXPECT(turnB->arenaIndex == heapB.getIndex());
    turnB->forEach([&](void* p, size_t size) {
      KJ_EXPECT(inHeap(arenaB, p), p, size);
      KJ_EXPECT(!inHeap(arenaA, p), p, size);
      KJ_EXPECT(gk_run_here_user(&touchByte, p) == BUFFER_FILL, p, size);
    });
    turnA->forEach([&](void* p, size_t size) {
      KJ_EXPECT(gk_run_here_user(&touchByte, p) == GK_EFAULT, p, size);
      KJ_EXPECT(gk_fault_addr() == reinterpret_cast<uintptr_t>(p));
    });
    KJ_EXPECT(gk_run_here_user(&HeapTurn::release, turnA.get()) == 0);
  });
  // Frees from the host with no lock held, of B's turn's objects and of A's host-side ones.
  KJ_EXPECT(HeapTurn::release(turnB.get()) == 0);
  KJ_EXPECT(HeapTurn::release(hostTurn.get()) == 0);

  // Repeatedly: allocate in A's turn and free from B's turn, from the host with no lock held,
  // or from A's own turn. The process survives every round, and the memory is reused rather
  // than consumed: the rounds allocate far more in total than the arena takes from its region,
  // and the addresses handed out repeat.
  constexpr int ROUNDS = 500;
  size_t perRound = 64;
  for (size_t size: HEAP_SIZES) perRound += size;
  size_t mappedBefore = heapA.getRegionBytes();
  kj::HashSet<uintptr_t> addresses;
  for (int round = 0; round < ROUNDS; round++) {
    a.runInLockScope([&](ArenaIsolate::Lock& lock) {
      KJ_ASSERT(gk_run_here_user(&HeapTurn::allocate, turnA.get()) == 0, round);
    });
    turnA->forEach([&](void* p, size_t size) {
      KJ_ASSERT(inHeap(arenaA, p), round, p, size);
      auto addr = reinterpret_cast<uintptr_t>(p);
      if (addresses.find(addr) == kj::none) addresses.insert(addr);
    });
    switch (round % 3) {
      case 0:
        b.runInLockScope([&](ArenaIsolate::Lock& lock) {
          KJ_ASSERT(gk_run_here_user(&HeapTurn::release, turnA.get()) == 0, round);
        });
        break;
      case 1:
        KJ_ASSERT(HeapTurn::release(turnA.get()) == 0, round);
        break;
      case 2:
        a.runInLockScope([&](ArenaIsolate::Lock& lock) {
          KJ_ASSERT(gk_run_here_user(&HeapTurn::release, turnA.get()) == 0, round);
        });
        break;
    }
  }
  size_t mappedAfter = heapA.getRegionBytes();
  size_t allocated = ROUNDS * perRound;
  KJ_LOG(INFO, "heap reuse over all rounds", allocated, mappedBefore, mappedAfter, addresses.size(),
      ROUNDS * (HEAP_COUNT + 1));
  KJ_EXPECT(mappedAfter - mappedBefore < allocated / 8, mappedBefore, mappedAfter, allocated);
  KJ_EXPECT(addresses.size() < ROUNDS * (HEAP_COUNT + 1) / 4, addresses.size());

  // After all those frees, an allocation for B still comes from B's region and one on the host
  // from neither: memory freed from one arena is never recycled into another.
  b.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(gk_run_here_user(&HeapTurn::allocate, turnB.get()) == 0);
    turnB->forEach([&](void* p, size_t size) { KJ_EXPECT(inHeap(arenaB, p), p, size); });
    KJ_EXPECT(gk_run_here_user(&HeapTurn::release, turnB.get()) == 0);
  });
  KJ_EXPECT(HeapTurn::allocate(hostTurn.get()) == 0);
  hostTurn->forEach(
      [&](void* p, size_t size) { KJ_EXPECT(!inHeap(arenaA, p) && !inHeap(arenaB, p), p, size); });
  KJ_EXPECT(HeapTurn::release(hostTurn.get()) == 0);

  // Part 6: the shared heap from under a lock. A SharedHeapScope inside A's lock puts the
  // thread's allocations in the shared heap for its lifetime, whether entered on the host side
  // of the lock or at guest ring 3 inside a turn: what it allocates lies in neither isolate's
  // arena, and B's ring-3 turn reads and writes it. The scope nests: A's heap arena is the
  // thread's again once it ends.
  auto sharedHostTurn = kj::heap<HeapTurn>();
  auto sharedGuestTurn = kj::heap<HeapTurn>();
  a.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(currentHeapArenaIndex() == heapA.getIndex());
    {
      SharedHeapScope sharedHeap;
      KJ_EXPECT(isAutomaticHeapArena(currentHeapArenaIndex()));
      KJ_EXPECT(HeapTurn::allocate(sharedHostTurn.get()) == 0);
    }
    KJ_EXPECT(currentHeapArenaIndex() == heapA.getIndex());
    KJ_EXPECT(gk_run_here_user(&HeapTurn::allocateShared, sharedGuestTurn.get()) == 0);
    KJ_EXPECT(currentHeapArenaIndex() == heapA.getIndex());
    for (HeapTurn* turn: {sharedHostTurn.get(), sharedGuestTurn.get()}) {
      KJ_EXPECT(isAutomaticHeapArena(turn->arenaIndex), turn->arenaIndex);
      turn->forEach([&](void* p, size_t size) {
        KJ_EXPECT(!inHeap(arenaA, p) && !inHeap(arenaB, p), p, size);
        KJ_EXPECT(!contains(arenaA, p) && !inTail(arenaA, p), p, size);
        KJ_EXPECT(!contains(arenaB, p) && !inTail(arenaB, p), p, size);
      });
    }
    // Still A's: an allocation after the scopes is in A's heap region again.
    KJ_EXPECT(HeapTurn::allocate(hostTurn.get()) == 0);
    hostTurn->forEach([&](void* p, size_t size) { KJ_EXPECT(inHeap(arenaA, p), p, size); });
    KJ_EXPECT(HeapTurn::release(hostTurn.get()) == 0);
  });
  // Under B's lock, from ring 3: readable and writable at both ends of every block, and freed.
  b.runInLockScope([&](ArenaIsolate::Lock& lock) {
    for (HeapTurn* turn: {sharedHostTurn.get(), sharedGuestTurn.get()}) {
      turn->forEach([&](void* p, size_t size) {
        KJ_EXPECT(gk_run_here_user(&touchByte, p) == BUFFER_FILL, p, size);
        KJ_EXPECT(gk_run_here_user(&touchByte, static_cast<kj::byte*>(p) + size - 1) == BUFFER_FILL,
            p, size);
      });
      KJ_EXPECT(gk_run_here_user(&HeapTurn::release, turn) == 0);
    }
  });
#endif  // WORKERD_HAS_HEAP_ARENA

  // Part 7: isolates come and go. A destroyed isolate's arena is destroyed with it, heap arena
  // and page-table slot included, and the next isolate is placed in a fresh arena, in which
  // ring-3 JS turns run as before. Three isolates created and destroyed in turn do not take
  // three new slots: a released slot is reused.
  kj::Vector<void*> bases;
  for (int i = 0; i < 3; i++) {
    // On the heap, as in production: the turn writes the isolate's own state and its record,
    // and a local would be behind the host-frame wall (see runInGuest()). The isolate goes in
    // the shared heap, as no lock is held, and the record in the isolate's arena.
    auto c =
        kj::heap<ArenaIsolate>(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
    auto& arenaC = KJ_ASSERT_NONNULL(c->getGuestArena());
    bases.add(arenaC.base());
    c->runInLockScope([&](ArenaIsolate::Lock& lock) {
      auto turn = kj::heap<JsTurn>(lock);
      KJ_EXPECT(gk_run_here_user(&JsTurn::run, turn.get()) == 0, i);
      KJ_EXPECT(!turn->threw, i);
      KJ_EXPECT(turn->result == 1024 * 7 + 10000 + 511.5, i, turn->result);
    });
  }
  KJ_LOG(INFO, "arenas of isolates created and destroyed in turn", bases[0], bases[1], bases[2]);
  KJ_EXPECT(bases[1] == bases[0] || bases[2] == bases[0] || bases[2] == bases[1], bases[0],
      bases[1], bases[2]);
  // And the two isolates of the parts above still run turns after all that.
  a.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(gk_run_here_user(&touchByte, bufA) == BUFFER_FILL);
  });
}

// The host reads the turn record as bytes. A description length past the buffer is clamped to
// it, a type outside kj::Exception::Type falls back to FAILED, the description is copied by
// length and not by terminator, and a status outside the closed set is an error of its own.
KJ_TEST("a guest turn's outcome is read back as bounded bytes") {
  using Outcome = _::GuestTurnOutcome;
  auto outcome = kj::heap<Outcome>();
  auto rethrown = [&]() {
    return KJ_ASSERT_NONNULL(
        kj::runCatchingExceptions([&]() { _::throwIfGuestTurnFailed(*outcome); }));
  };

  outcome->status = Outcome::EXCEPTION;
  outcome->exceptionType = 0xdeadbeef;
  outcome->descriptionLength = 0xffffffff;
  memset(outcome->description, 'x', Outcome::MAX_DESCRIPTION);
  auto clamped = rethrown();
  KJ_EXPECT(clamped.getType() == kj::Exception::Type::FAILED);
  KJ_EXPECT(clamped.getDescription().size() == Outcome::MAX_DESCRIPTION);

  outcome->exceptionType = static_cast<uint32_t>(kj::Exception::Type::OVERLOADED);
  outcome->descriptionLength = 5;
  memcpy(outcome->description, "ab\0cdefgh", 9);
  auto byLength = rethrown();
  KJ_EXPECT(byLength.getType() == kj::Exception::Type::OVERLOADED);
  KJ_EXPECT(byLength.getDescription().size() == 5);
  KJ_EXPECT(byLength.getDescription()[2] == '\0');
  KJ_EXPECT(byLength.getDescription()[4] == 'd');

  outcome->status = Outcome::RETURNED;
  KJ_EXPECT(kj::runCatchingExceptions([&]() { _::throwIfGuestTurnFailed(*outcome); }) == kj::none);

  outcome->status = Outcome::INCOMPLETE;
  auto incomplete = rethrown();
  KJ_EXPECT(incomplete.getDescription().contains("invalid status"), incomplete.getDescription());

  outcome->status = 1234;
  auto unknown = rethrown();
  KJ_EXPECT(unknown.getDescription().contains("invalid status"), unknown.getDescription());
}

#else

KJ_TEST("guest-kernel arenas are not available on this platform") {
  KJ_LOG(WARNING, "skipping: workerd is built without the guest kernel");
}

#endif  // WORKERD_HAS_GUEST_KERNEL

}  // namespace
}  // namespace workerd::jsg::test
