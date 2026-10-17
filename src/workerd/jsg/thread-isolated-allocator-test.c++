// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "thread-isolated-allocator.h"

#include <kj/debug.h>
#include <kj/test.h>
#include <kj/vector.h>

#if __linux__
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

namespace workerd::jsg::test {
namespace {

#if __linux__

// Stands in for the platform's allocator: it owns the protection key, as
// v8::platform::DefaultThreadIsolatedAllocator does, and serves with malloc() anything the
// allocator under test passes down to it, as that one does for everything.
class FakePlatformAllocator final: public v8::ThreadIsolatedAllocator {
 public:
  void* Allocate(size_t size) override {
    ++passedDown;
    return malloc(size);
  }
  void Free(void* object) override {
    ++freedBelow;
    free(object);
  }
  enum Type Type() const override {
    return Type::kPkey;
  }
  int Pkey() const override {
    return pkey;
  }

  // Allocated once, as V8's platform does it, and -1 where the kernel has no keys left or none
  // at all.
  int pkey = pkey_alloc(0, PKEY_DISABLE_WRITE);
  uint passedDown = 0;
  uint freedBelow = 0;
};

// One platform allocator for the whole file: wrapThreadIsolatedAllocator() returns the one
// allocator the process uses, bound to the first platform allocator it is given, exactly as V8
// gets it.
FakePlatformAllocator& platformAllocator() {
  static FakePlatformAllocator* allocator = new FakePlatformAllocator();
  return *allocator;
}
v8::ThreadIsolatedAllocator& allocatorUnderTest() {
  auto* allocator = wrapThreadIsolatedAllocator(&platformAllocator());
  KJ_ASSERT(allocator != nullptr);
  return *allocator;
}

// Opens the platform's protection key for writing on this thread, the way V8's
// RwxMemoryWriteScope does around every read and write of its thread-isolation metadata. Writing
// a block without this faults, which is the point of keying the slabs.
class OpenKeyScope {
 public:
  explicit OpenKeyScope(int pkey): pkey(pkey) {
    if (pkey >= 0) KJ_ASSERT(pkey_set(pkey, 0) == 0, errno);
  }
  ~OpenKeyScope() noexcept(false) {
    if (pkey >= 0) KJ_ASSERT(pkey_set(pkey, PKEY_DISABLE_WRITE) == 0, errno);
  }
  KJ_DISALLOW_COPY_AND_MOVE(OpenKeyScope);

 private:
  int pkey;
};

// The protection key /proc/self/smaps reports for the mapping that holds `addr`, or none when
// the kernel reports no keys at all (no CONFIG_X86_INTEL_MEMORY_PROTECTION_KEYS).
kj::Maybe<uint> protectionKeyOf(const void* addr) {
  auto target = reinterpret_cast<uintptr_t>(addr);
  FILE* f = fopen("/proc/self/smaps", "r");
  KJ_ASSERT(f != nullptr);
  KJ_DEFER(fclose(f));
  char line[512];
  bool inRange = false;
  bool anyKeyReported = false;
  kj::Maybe<uint> result;
  while (fgets(line, sizeof(line), f) != nullptr) {
    unsigned long begin = 0, end = 0;
    unsigned key = 0;
    if (sscanf(line, "%lx-%lx ", &begin, &end) == 2) {
      inRange = target >= begin && target < end;
    } else if (sscanf(line, "ProtectionKey: %u", &key) == 1) {
      anyKeyReported = true;
      if (inRange) {
        result = key;
        break;
      }
    }
  }
  if (!anyKeyReported) return kj::none;
  return result;
}

KJ_TEST("a platform with no thread-isolated allocator leaves V8's thread isolation alone") {
  KJ_EXPECT(wrapThreadIsolatedAllocator(nullptr) == nullptr);
}

KJ_TEST("V8's thread-isolation metadata comes from slabs of its own") {
  auto& platform = platformAllocator();
  auto& alloc = allocatorUnderTest();
  uint passedDownBefore = platform.passedDown;
  uint freedBelowBefore = platform.freedBelow;

  KJ_EXPECT(alloc.Pkey() == platform.Pkey());
  KJ_EXPECT(alloc.Type() == v8::ThreadIsolatedAllocator::Type::kPkey);

  // The sizes V8's index asks for -- std::map nodes, JitPage objects, v8::base::Mutex -- and
  // enough of each to need a second slab of the smallest class.
  kj::Vector<void*> blocks;
  for (size_t size:
      {size_t(8), size_t(16), size_t(40), size_t(56), size_t(64), size_t(112), size_t(200)}) {
    for (uint i = 0; i < 64; i++) {
      void* p = alloc.Allocate(size);
      KJ_ASSERT(p != nullptr, size);
      // Suitably aligned for anything V8 places in it, as malloc() is.
      KJ_EXPECT(reinterpret_cast<uintptr_t>(p) % 16 == 0, p);
      blocks.add(p);
    }
  }
  KJ_EXPECT(platform.passedDown == passedDownBefore, platform.passedDown);

  // Distinct and non-overlapping: the fill of each survives every other allocation.
  {
    OpenKeyScope writable(platform.Pkey());
    for (size_t i = 0; i < blocks.size(); i++) {
      memset(blocks[i], static_cast<int>(i & 0xff), 8);
    }
    for (size_t i = 0; i < blocks.size(); i++) {
      KJ_EXPECT(static_cast<kj::byte*>(blocks[i])[0] == static_cast<kj::byte>(i & 0xff), i);
    }
  }
  auto sorted = kj::heapArray<void*>(blocks.asPtr());
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 1; i < sorted.size(); i++) KJ_EXPECT(sorted[i] != sorted[i - 1]);

  // A freed block is handed out again rather than the slabs growing for ever.
  void* reused = blocks[0];
  alloc.Free(reused);
  KJ_EXPECT(alloc.Allocate(8) == reused);
  KJ_EXPECT(platform.freedBelow == freedBelowBefore, platform.freedBelow);

  // Above the largest class there is no slab, so the platform's allocator serves the request
  // and takes the free back.
  void* big = alloc.Allocate(64 * 1024);
  KJ_ASSERT(big != nullptr);
  KJ_EXPECT(platform.passedDown == passedDownBefore + 1, platform.passedDown);
  alloc.Free(big);
  KJ_EXPECT(platform.freedBelow == freedBelowBefore + 1, platform.freedBelow);

  for (size_t i = 1; i < blocks.size(); i++) alloc.Free(blocks[i]);
  KJ_EXPECT(platform.freedBelow == freedBelowBefore + 1, platform.freedBelow);
}

KJ_TEST("V8's thread-isolation metadata carries the platform's protection key") {
  auto& platform = platformAllocator();
  if (platform.Pkey() < 0) {
    KJ_LOG(WARNING, "no protection key could be allocated; skipping", errno);
    return;
  }
  void* block = allocatorUnderTest().Allocate(64);
  KJ_ASSERT(block != nullptr);
  KJ_IF_SOME(key, protectionKeyOf(block)) {
    KJ_EXPECT(key == static_cast<uint>(platform.Pkey()), key, platform.Pkey());
  } else {
    KJ_LOG(WARNING, "this kernel reports no protection keys in smaps; skipping");
  }
}

#endif  // __linux__

}  // namespace
}  // namespace workerd::jsg::test
