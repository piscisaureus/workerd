// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Per-isolate guest-kernel arenas (see GuestArena in setup.h): each isolate created through
// jsg::newIsolateGroup() gets its own IsolateGroup, whose V8 sandbox lives in a private gk
// arena, and the isolate lock makes that arena the thread's active one. This is the real-V8
// counterpart of gk_test.c's arena-isolation test: guest execution under one isolate's lock can
// touch that isolate's ArrayBuffer memory and takes a hardware fault on another isolate's.

#include "jsg-test.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>

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

bool contains(GuestArena& arena, void* p) {
  auto addr = reinterpret_cast<uintptr_t>(p);
  auto base = reinterpret_cast<uintptr_t>(arena.base());
  return addr >= base && addr < base + arena.size();
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

KJ_TEST("each isolate's sandbox lives in its own guest-kernel arena") {
  // newIsolateGroup() places isolates in arenas only when guest-kernel isolation is enabled,
  // which is decided from the environment on first use; gk must be initialized before V8
  // starts (it maps the address space as it is then).
  setenv("WORKERD_EXPERIMENTAL_GUEST_KERNEL", "1", 1);
  if (gk_init() < 0) {
    KJ_LOG(WARNING, "skipping: guest kernel unavailable",
        kj::StringPtr(gk_last_error() != nullptr ? gk_last_error() : "unknown"));
    return;
  }
  KJ_ASSERT(isGuestKernelEnabled());

  V8System v8System;
  ArenaIsolate a(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());
  ArenaIsolate b(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());

  auto& arenaA = KJ_ASSERT_NONNULL(a.getGuestArena());
  auto& arenaB = KJ_ASSERT_NONNULL(b.getGuestArena());
  KJ_EXPECT(arenaA.get() != arenaB.get());
  KJ_EXPECT(arenaA.base() != arenaB.base());
  KJ_EXPECT(!contains(arenaA, arenaB.base()));
  KJ_EXPECT(!contains(arenaB, arenaA.base()));

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
}

#else

KJ_TEST("guest-kernel arenas are not available on this platform") {
  KJ_LOG(WARNING, "skipping: workerd is built without the guest kernel");
}

#endif  // WORKERD_HAS_GUEST_KERNEL

}  // namespace
}  // namespace workerd::jsg::test
