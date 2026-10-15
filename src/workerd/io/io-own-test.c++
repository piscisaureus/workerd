#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

class TrackedObject {
 public:
  TrackedObject(uint& destructionCount, uint value)
      : value(value),
        destructionCount(destructionCount) {}
  ~TrackedObject() noexcept(false) {
    ++destructionCount;
  }

  uint value;

 private:
  uint& destructionCount;
};

KJ_TEST("ReverseIoOwn supports dereferencing, moves, and early destruction") {
  TestFixture fixture;
  uint destructionCount = 0;
  auto context = fixture.newIoContext();
  auto object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 123));

  KJ_EXPECT(object->value == 123);
  KJ_EXPECT((*object).value == 123);

  auto moved = kj::mv(object);
  KJ_EXPECT(object.tryGet() == kj::none);
  KJ_EXPECT(moved.tryGet() != kj::none);

  object = kj::mv(moved);
  KJ_EXPECT(moved.tryGet() == kj::none);
  KJ_EXPECT(object.tryGet() != kj::none);

  object = nullptr;
  KJ_EXPECT(destructionCount == 1);
  KJ_EXPECT(object.tryGet() == kj::none);
}

KJ_TEST("ReverseIoOwn can transfer a live object out of its IoContext") {
  TestFixture fixture;
  uint destructionCount = 0;
  kj::Own<TrackedObject> owned;

  {
    auto context = fixture.newIoContext();
    auto object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 123));

    owned = kj::mv(object);
    KJ_EXPECT(object.tryGet() == kj::none);
    KJ_EXPECT(owned->value == 123);
  }

  KJ_EXPECT(destructionCount == 0);
  owned = nullptr;
  KJ_EXPECT(destructionCount == 1);
}

KJ_TEST("ReverseIoOwn remains safe after its IoContext is destroyed") {
  TestFixture fixture;
  uint destructionCount = 0;
  ReverseIoOwn<TrackedObject> object = nullptr;

  {
    auto context = fixture.newIoContext();
    object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 123));
  }

  KJ_EXPECT(destructionCount == 1);
  KJ_EXPECT(object.tryGet() == kj::none);
  KJ_EXPECT_THROW_MESSAGE("execution context has ended", object->value);

  {
    auto context = fixture.newIoContext();
    object = context->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 456));
    KJ_EXPECT(object.tryGet() != kj::none);
    KJ_EXPECT(object->value == 456);
  }

  KJ_EXPECT(destructionCount == 2);
  KJ_EXPECT(object.tryGet() == kj::none);

  {
    auto destroyedAfterInvalidation = kj::mv(object);
    KJ_EXPECT(destroyedAfterInvalidation.tryGet() == kj::none);
  }
}

// The owned-node counts since the process started, before and after a scope, for the check that
// every node a scope allocates is freed by its end.
struct NodeCounts {
  OwnedObjectList::Stats before = OwnedObjectList::statsForTest();
  void expectBalanced() {
    auto after = OwnedObjectList::statsForTest();
    KJ_EXPECT(after.nodesAllocated - before.nodesAllocated == after.nodesFreed - before.nodesFreed,
        after.nodesAllocated - before.nodesAllocated, after.nodesFreed - before.nodesFreed);
  }
};

KJ_TEST("IoOwn created inside a turn is usable at once and dropped inside a later turn") {
  NodeCounts counts;
  {
    TestFixture fixture;
    uint destructionCount = 0;
    auto request = fixture.newIncomingRequest();
    kj::Maybe<IoOwn<TrackedObject>> own;

    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      // Registered through the turn's journal: the entry is committed after the turn, and the
      // dereference meanwhile resolves through the journal.
      auto o = env.context.addObject(kj::heap<TrackedObject>(destructionCount, 7));
      KJ_EXPECT(o->value == 7);
      own = kj::mv(o);
    });
    KJ_EXPECT(destructionCount == 0);

    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      // Committed now, and resolved against the table.
      KJ_EXPECT(KJ_ASSERT_NONNULL(own)->value == 7);
      // Dropped inside the turn: the turn disposes of the object itself, immediately, and the
      // node is freed after the turn.
      own = kj::none;
      KJ_EXPECT(destructionCount == 1);
    });
    KJ_EXPECT(destructionCount == 1);
  }
  counts.expectBalanced();
}

KJ_TEST("IoOwn dropped outside its context is released when the context next runs") {
  NodeCounts counts;
  {
    TestFixture fixture;
    uint destructionCount = 0;
    auto request = fixture.newIncomingRequest();
    kj::Maybe<IoOwn<TrackedObject>> own;

    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      own = env.context.addObject(kj::heap<TrackedObject>(destructionCount, 7));
    });
    // On the event loop, with no current context: the handle goes to the delete queue.
    own = kj::none;
    KJ_EXPECT(destructionCount == 0);
    // The context's next turn drains the queue host-side, before entering the turn.
    fixture.enterContext(
        *request, [&](const TestFixture::Environment& env) { KJ_EXPECT(destructionCount == 1); });
  }
  counts.expectBalanced();
}

KJ_TEST("IoOwn can be converted to a kj::Own inside and outside a turn") {
  NodeCounts counts;
  {
    TestFixture fixture;
    uint destructionCount = 0;
    auto request = fixture.newIncomingRequest();
    kj::Maybe<IoOwn<TrackedObject>> a;
    kj::Maybe<IoOwn<TrackedObject>> b;
    kj::Own<TrackedObject> ownedA;
    kj::Own<TrackedObject> ownedB;

    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      a = env.context.addObject(kj::heap<TrackedObject>(destructionCount, 1));
      b = env.context.addObject(kj::heap<TrackedObject>(destructionCount, 2));
      // Inside the turn that created it: the disposer comes from the IoOwn's own record.
      ownedA = kj::mv(KJ_ASSERT_NONNULL(a));
      a = kj::none;
    });
    fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
      // Inside a later turn: from the IoOwn's record again, with the node freed after the turn.
      ownedB = kj::mv(KJ_ASSERT_NONNULL(b));
      b = kj::none;
    });
    KJ_EXPECT(ownedA->value == 1);
    KJ_EXPECT(ownedB->value == 2);
    KJ_EXPECT(destructionCount == 0);
    ownedA = nullptr;
    ownedB = nullptr;
    KJ_EXPECT(destructionCount == 2);
  }
  counts.expectBalanced();
}

KJ_TEST("objects an IoOwn still names are released with their context") {
  NodeCounts counts;
  uint destructionCount = 0;
  {
    TestFixture fixture;
    {
      auto request = fixture.newIncomingRequest();
      fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
        auto o = env.context.addObject(kj::heap<TrackedObject>(destructionCount, 1));
        kj::mv(o).deferGcToContext();
        // Two more, registered in this turn and never dropped by a holder.
        env.context.addObject(kj::heap<TrackedObject>(destructionCount, 2)).deferGcToContext();
        env.context.addObject(kj::heap<TrackedObject>(destructionCount, 3)).deferGcToContext();
      });
      KJ_EXPECT(destructionCount == 0);
    }
    // The context is torn down host-side with the request: its list is walked and every object
    // disposed of.
    KJ_EXPECT(destructionCount == 3);
  }
  counts.expectBalanced();
}

KJ_TEST("a context torn down inside another context's turn releases its objects there") {
  NodeCounts counts;
  uint destructionCount = 0;
  {
    TestFixture fixture;
    auto other = fixture.newIoContext();
    // Registered host-side, outside any turn: the node is built at once.
    auto committed = other->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 1));
    {
      kj::Own<TrackedObject> taken = kj::mv(committed);  // taken out host-side, then dropped
    }
    KJ_EXPECT(destructionCount == 1);
    auto kept = other->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 2));
    auto kept2 = other->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 3));

    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      // Inside a turn of another context: one more object registered into `other` through the
      // journal, then `other` torn down here, where its nodes cannot be reached. The journaled
      // registration is released from the journal, the committed ones drained through the
      // hostcall, and each object disposed of in this turn.
      auto journaled = other->addObjectReverse(kj::heap<TrackedObject>(destructionCount, 4));
      KJ_EXPECT(journaled->value == 4);
      KJ_EXPECT(kept->value == 2);
      KJ_EXPECT(destructionCount == 1);
      // A committed object dropped in this turn: disposed of now, its release journaled, and its
      // node still linked until the host sees the release. The teardown must not dispose of it
      // again.
      kept2 = nullptr;
      KJ_EXPECT(destructionCount == 2);
      other = nullptr;
      KJ_EXPECT(destructionCount == 4);
      KJ_EXPECT(journaled.tryGet() == kj::none);
      KJ_EXPECT(kept.tryGet() == kj::none);
    });
    KJ_EXPECT(destructionCount == 4);
  }
  counts.expectBalanced();
}

KJ_TEST("a context with many objects torn down inside a turn drains them in batches") {
  NodeCounts counts;
  uint destructionCount = 0;
  {
    TestFixture fixture;
    auto other = fixture.newIoContext();
    constexpr uint N = 700;  // more than one hostcall frame hands over
    kj::Vector<ReverseIoOwn<TrackedObject>> owns;
    for (uint i = 0; i < N; i++) {
      owns.add(other->addObjectReverse(kj::heap<TrackedObject>(destructionCount, i)));
    }
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      other = nullptr;
      KJ_EXPECT(destructionCount == N);
    });
  }
  counts.expectBalanced();
}

KJ_TEST("IoPtr names its object by an entry of its own and can be copied") {
  TestFixture fixture;
  uint destructionCount = 0;
  TrackedObject target(destructionCount, 42);
  auto request = fixture.newIncomingRequest();
  kj::Maybe<IoPtr<TrackedObject>> ptr;
  kj::Maybe<IoPtr<TrackedObject>> copy;
  // A first turn, so that the thread's reserved entries are in the count before it is taken.
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {});
  size_t liveBefore = hostMemory::liveResolveEntries();

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto p = env.context.addObject(target);
    KJ_EXPECT(p->value == 42);
    IoPtr<TrackedObject> c = p;
    KJ_EXPECT(c->value == 42);
    ptr = kj::mv(p);
    copy = kj::mv(c);
  });
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(KJ_ASSERT_NONNULL(ptr)->value == 42);
    KJ_EXPECT(KJ_ASSERT_NONNULL(copy)->value == 42);
    ptr = kj::none;
  });
  copy = kj::none;
  KJ_EXPECT(destructionCount == 0);
  // Both entries are retired: the live count (which includes the thread's reserved entries, two
  // of which the IoPtrs took) is no higher than before they existed.
  KJ_EXPECT(hostMemory::liveResolveEntries() <= liveBefore);
}

}  // namespace
}  // namespace workerd
