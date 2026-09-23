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

#include "jsg-test.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>

#include <cppgc/allocation.h>
#include <cppgc/garbage-collected.h>
#include <cppgc/persistent.h>
#include <v8-cppgc.h>
#include <v8-internal.h>

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
// guest via gk_run_here(); an access to memory the active arena cannot address ends the run
// with GK_EFAULT instead of returning.
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
  if (gk_init() < 0) {
    KJ_LOG(WARNING, "skipping: guest kernel unavailable",
        kj::StringPtr(gk_last_error() != nullptr ? gk_last_error() : "unknown"));
    return false;
  }
  KJ_ASSERT(isGuestKernelEnabled());
  return true;
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

// A single case with a single pair of isolates: gk supports one round of arenas per process
// (a guest entry after arenas were destroyed and new ones created spins in KVM), so every check
// shares these two. Natives syntax lets a script force optimization, so that an isolate's code
// range holds JIT code.
KJ_TEST("each isolate's memory lives in its own guest-kernel arena") {
  if (!enableGuestKernel()) return;

  V8System v8System({"--allow-natives-syntax"_kj});
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

    JsTurn turn{lock};
    KJ_EXPECT(gk_run_here(&JsTurn::run, &turn) == 0);
    KJ_EXPECT(!turn.threw);
    // 1024 pages of bytes[i] == 7, plus 10000 objects contributing 1 each, plus floats[1023].
    KJ_EXPECT(turn.result == 1024 * 7 + 10000 + 511.5, turn.result);
  });

  // Mirror under B's lock.
  b.runInLockScope([&](ArenaIsolate::Lock& lock) {
    KJ_EXPECT(gk_run_here(&touchByte, bufB) == BUFFER_FILL);
    KJ_EXPECT(gk_run_here(&touchByte, bufA) == GK_EFAULT);
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
}

#else

KJ_TEST("guest-kernel arenas are not available on this platform") {
  KJ_LOG(WARNING, "skipping: workerd is built without the guest kernel");
}

#endif  // WORKERD_HAS_GUEST_KERNEL

}  // namespace
}  // namespace workerd::jsg::test
