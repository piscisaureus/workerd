// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "host-entropy.h"

#include <kj/test.h>

namespace workerd {
namespace {

using hostEntropy::BUFFER_SIZE;

bool allZero(kj::ArrayPtr<const kj::byte> bytes) {
  for (auto b: bytes) {
    if (b != 0) return false;
  }
  return true;
}

KJ_TEST("host entropy: draws return distinct, non-zero bytes") {
  kj::byte a[32] = {};
  kj::byte b[32] = {};
  getHostEntropy(a);
  getHostEntropy(b);
  KJ_EXPECT(!allZero(a));
  KJ_EXPECT(!allZero(b));
  KJ_EXPECT(kj::arrayPtr(a) != kj::arrayPtr(b));

  // Larger than the buffer, so it spans several refills.
  auto big = kj::heapArray<kj::byte>(3 * BUFFER_SIZE + 7);
  getHostEntropy(big);
  KJ_EXPECT(!allZero(big));
  KJ_EXPECT(big.asPtr().first(32) != kj::arrayPtr(a));
  KJ_EXPECT(big.asPtr().slice(BUFFER_SIZE, BUFFER_SIZE + 32) != big.asPtr().first(32));
}

// The byte the deterministic test source writes at position `i` of refill number `refill`.
kj::byte expected(size_t refill, size_t i) {
  return static_cast<kj::byte>((refill * 131 + i * 7) & 0xff);
}

KJ_TEST("host entropy: a draw that spans the refill point continues into the next fill") {
  size_t refills = 0;
  hostEntropy::setSourceForTest([&refills](kj::ArrayPtr<kj::byte> buffer) {
    KJ_ASSERT(buffer.size() == BUFFER_SIZE);
    for (auto i: kj::indices(buffer)) {
      buffer[i] = expected(refills, i);
    }
    ++refills;
  });
  KJ_DEFER(hostEntropy::clearSourceForTest());
  hostEntropy::discardForTest();

  // Leave five bytes in the buffer, then draw twelve: the first five are the tail of the first
  // fill and the next seven the head of the second.
  auto head = kj::heapArray<kj::byte>(BUFFER_SIZE - 5);
  getHostEntropy(head);
  KJ_EXPECT(refills == 1);
  for (auto i: kj::indices(head)) {
    KJ_EXPECT(head[i] == expected(0, i), i);
  }

  kj::byte span[12] = {};
  getHostEntropy(span);
  KJ_EXPECT(refills == 2);
  for (size_t i = 0; i < 5; i++) {
    KJ_EXPECT(span[i] == expected(0, BUFFER_SIZE - 5 + i), i);
  }
  for (size_t i = 5; i < 12; i++) {
    KJ_EXPECT(span[i] == expected(1, i - 5), i);
  }

  // The next draw continues where the spanning one stopped.
  kj::byte next[3] = {};
  getHostEntropy(next);
  KJ_EXPECT(refills == 2);
  for (size_t i = 0; i < 3; i++) {
    KJ_EXPECT(next[i] == expected(1, 7 + i), i);
  }

  // A draw of exactly the buffer's size from a full buffer consumes it without a refill in the
  // middle, and the draw after it refills.
  hostEntropy::discardForTest();
  auto whole = kj::heapArray<kj::byte>(BUFFER_SIZE);
  getHostEntropy(whole);
  KJ_EXPECT(refills == 3);
  for (auto i: kj::indices(whole)) {
    KJ_EXPECT(whole[i] == expected(2, i), i);
  }
  kj::byte one[1] = {};
  getHostEntropy(one);
  KJ_EXPECT(refills == 4);
  KJ_EXPECT(one[0] == expected(3, 0));

  // A draw larger than the buffer is several refills end to end.
  hostEntropy::discardForTest();
  auto big = kj::heapArray<kj::byte>(2 * BUFFER_SIZE + 9);
  getHostEntropy(big);
  KJ_EXPECT(refills == 7);
  for (auto i: kj::indices(big)) {
    KJ_EXPECT(big[i] == expected(4 + i / BUFFER_SIZE, i % BUFFER_SIZE), i);
  }
}

}  // namespace
}  // namespace workerd
