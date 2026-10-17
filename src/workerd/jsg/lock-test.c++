// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

#include <csignal>

// What the isolate's lock slot (SET_DATA_LOCK, behind jsg::Lock::from) can and cannot
// represent. It names one Lock, and a Lock's destructor clears it rather than restoring an
// outer one, so a second Lock on an isolate this thread already has a Lock for would leave the
// outer one unnamed; the constructor refuses that. A v8::Locker an embedder holds without a
// Lock is a different thing: V8 supports nesting Lockers, and one leaves the slot alone.

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct LockContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(LockContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(LockIsolate, LockContext);

KJ_TEST("a v8::Locker held without a Lock does not make the next Lock recursive") {
  LockIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());

  // As src/rust/jsg-test's harness does: hold the isolate's V8 lock for longer than any one
  // Lock, and take Locks inside it.
  v8::Locker locker(isolate.getIsolate());

  isolate.runInLockScope(
      [&](LockIsolate::Lock& lock) { KJ_EXPECT(&Lock::from(lock.v8Isolate) == &lock); });
  // The slot is clear again, so the next one is no more recursive than the first.
  KJ_EXPECT(isolate.getIsolate()->GetData(SET_DATA_LOCK) == nullptr);
  isolate.runInLockScope(
      [&](LockIsolate::Lock& lock) { KJ_EXPECT(&Lock::from(lock.v8Isolate) == &lock); });
  KJ_EXPECT(isolate.getIsolate()->GetData(SET_DATA_LOCK) == nullptr);
}

KJ_TEST("a Lock on an isolate this thread already has a Lock for is refused") {
  LockIsolate isolate(v8System, newIsolateGroup(), nullptr, kj::heap<IsolateObserver>());

  isolate.runInLockScope([&](LockIsolate::Lock& outer) {
    KJ_EXPECT(&Lock::from(outer.v8Isolate) == &outer);
    auto takeAnother = [&]() {
      isolate.runInLockScope(
          [](LockIsolate::Lock&) { KJ_FAIL_EXPECT("the inner Lock was taken"); });
    };
#ifdef KJ_DEBUG
    // A debug build aborts rather than throwing, so the attempt is made in a child process.
    KJ_EXPECT_SIGNAL(SIGABRT, takeAnother());
#else
    KJ_EXPECT_THROW_MESSAGE("attempt to take recursive isolate lock", takeAnother());
    // The refused Lock left the outer one named.
    KJ_EXPECT(&Lock::from(outer.v8Isolate) == &outer);
#endif
  });
}

}  // namespace
}  // namespace workerd::jsg::test
