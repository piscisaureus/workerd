// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/util/host-event-queue.h>

#include <workerd/util/host-memory.h>
#include <workerd/util/hostcall.h>

#include <kj/async.h>
#include <kj/debug.h>

#include <new>

namespace workerd {
namespace {

using kj::EventLoop;
using Event = kj::_::Event;
using ArmKind = kj::_::ArmKind;

// The journal capacity before an in-turn arm or disarm must flush it through one VM exit to make
// room. A single named constant so it is tunable; 256 is the chosen size, matching the reserved
// pool a turn's resolve registrations use (util/host-memory.h).
constexpr size_t EVENT_JOURNAL_CAP = 256;

// What a loop keeps for an event's place, encoded into the eight bytes kj holds for it (the
// EventLink* field, opaque under these hooks). A zero value is an unarmed event, which is the
// `link == nullptr` kj still tests. PENDING names the journal slot the in-turn arm wrote;
// APPLIED names the host-queue link index the apply gave it.
enum class TokenState : uint64_t { UNARMED = 0, PENDING = 1, APPLIED = 2 };

uint64_t makeToken(TokenState state, uint64_t payload) {
  return (payload << 2) | static_cast<uint64_t>(state);
}
TokenState tokenState(uint64_t token) {
  return static_cast<TokenState>(token & 3);
}
uint64_t tokenPayload(uint64_t token) {
  return token >> 2;
}

// A link in a loop's host-memory run queue. The loop owns these -- it allocates them from its own
// pool and recycles them -- so linking and unlinking write host memory a turn cannot reach, never
// another event's object. These are workerd's own, distinct from kj's EventLink, because the
// queue they form lives where the loop's events do not.
struct HostLink {
  HostLink* next;
  HostLink** prev;
  Event* event;  // null for a sentinel or a link on the free list

  void unlink() {
    *prev = next;
    next->prev = prev;
  }
  void insertBefore(HostLink& other) {
    prev = other.prev;
    *prev = this;
    next = &other;
    other.prev = &next;
  }
  void insertAfter(HostLink& other) {
    insertBefore(*other.next);
  }
};

// A chunk of links, mapped from host memory, never moved or freed until the loop is destroyed, so
// a link keeps its index for as long as the queue holds it. The links follow the header in memory.
struct LinkChunk {
  LinkChunk* next;
  size_t count;
  size_t baseIndex;  // the index of this chunk's first link in the loop's flat index space
};

// A loop's relocated run queue: its sentinels, free list and link pool, all in host memory, named
// from the loop by a handle (EventLoop::queueHandle) and resolved through the guest kernel's slot
// table on every host-side use. A handle a turn forges in the shared-heap loop resolves to nothing,
// not to memory the turn chose.
struct HostQueue {
  HostLink headLink;
  HostLink depthPoint;
  HostLink breadthPoint;
  HostLink tailLink;
  HostLink wouldSleepHead;
  HostLink wouldSleepTail;
  HostLink* freeLinks = nullptr;
  LinkChunk* chunks = nullptr;
  size_t totalLinks = 0;
  size_t nextChunkLinks = 64;

  HostQueue() {
    auto chain = [](HostLink& a, HostLink& b) {
      a.next = &b;
      b.prev = &a.next;
    };
    for (HostLink* s: {&headLink, &depthPoint, &breadthPoint, &tailLink, &wouldSleepHead,
             &wouldSleepTail}) {
      *s = HostLink{nullptr, nullptr, nullptr};
    }
    chain(headLink, depthPoint);
    chain(depthPoint, breadthPoint);
    chain(breadthPoint, tailLink);
    chain(wouldSleepHead, wouldSleepTail);
  }

  static HostLink* chunkLinks(LinkChunk* chunk) {
    return reinterpret_cast<HostLink*>(chunk + 1);
  }

  HostLink* growLinks() {
    size_t count = nextChunkLinks;
    nextChunkLinks = kj::min(count * 2, size_t(256 * 1024) / sizeof(HostLink));
    size_t bytes = sizeof(LinkChunk) + count * sizeof(HostLink);
    auto* chunk = static_cast<LinkChunk*>(hostMemory::allocate(bytes, alignof(LinkChunk)));
    chunk->next = chunks;
    chunk->count = count;
    chunk->baseIndex = totalLinks;
    chunks = chunk;
    totalLinks += count;
    HostLink* links = chunkLinks(chunk);
    for (size_t i = count; i > 0; i--) {
      HostLink& link = links[i - 1];
      link.event = nullptr;
      link.next = freeLinks;
      freeLinks = &link;
    }
    return freeLinks;
  }

  HostLink* allocLink(Event& event) {
    HostLink* link = freeLinks;
    if (KJ_UNLIKELY(link == nullptr)) link = growLinks();
    freeLinks = link->next;
    link->event = &event;
    return link;
  }

  size_t indexOf(HostLink* link) const {
    for (LinkChunk* chunk = chunks; chunk != nullptr; chunk = chunk->next) {
      HostLink* base = chunkLinks(chunk);
      if (link >= base && link < base + chunk->count) {
        return chunk->baseIndex + static_cast<size_t>(link - base);
      }
    }
    KJ_FAIL_ASSERT("event queue link is not from this loop's pool");
  }

  // Translate an index back to its link, or null for one out of range. The caller must treat null
  // as a refusal rather than a pointer.
  HostLink* linkAt(size_t index) const {
    if (index >= totalLinks) return nullptr;
    for (LinkChunk* chunk = chunks; chunk != nullptr; chunk = chunk->next) {
      if (index >= chunk->baseIndex && index < chunk->baseIndex + chunk->count) {
        return chunkLinks(chunk) + (index - chunk->baseIndex);
      }
    }
    return nullptr;
  }

  size_t armLink(Event& event, ArmKind kind) {
    HostLink* link = allocLink(event);
    switch (kind) {
      case ArmKind::DEPTH_FIRST:
        link->insertBefore(depthPoint);
        break;
      case ArmKind::BREADTH_FIRST:
        link->insertBefore(breadthPoint);
        break;
      case ArmKind::LAST:
        link->insertAfter(breadthPoint);
        break;
      case ArmKind::WHEN_WOULD_SLEEP:
        link->insertAfter(wouldSleepHead);
        break;
    }
    return indexOf(link);
  }

  // Disarm the link named by `index`. The index is bounds-checked against the loop's own pool
  // first: an index a turn forged out of range is refused (nothing is dereferenced), while an
  // in-range one names a link of this loop, so the worst a forged in-range index does is disturb
  // this loop's own queue. A legitimate in-range disarm is never refused -- doing so would leave a
  // host-queue link pointing at a destroyed event for turn() to fire.
  void disarmByIndex(size_t index) {
    HostLink* link = linkAt(index);
    if (link == nullptr) {
      WD_LOG_REFUSAL(hostMemory::LogSite::USE_UNKNOWN,
          "event queue disarm names no link of this loop", index, totalLinks);
      return;
    }
    if (link->event == nullptr) return;  // already unlinked (e.g. a sentinel or free link)
    link->unlink();
    link->event = nullptr;
    link->next = freeLinks;
    freeLinks = link;
  }

  Event* head() const {
    for (const HostLink* link = headLink.next; link != nullptr; link = link->next) {
      if (link->event != nullptr) return link->event;
    }
    return nullptr;
  }

  void teardown() {
    // Cut any still-armed event loose from its place before the pool goes away, so that destroying
    // such an event afterwards writes nothing (its token becomes unarmed, which it would have
    // become anyway).
    for (LinkChunk* chunk = chunks; chunk != nullptr; chunk = chunk->next) {
      HostLink* links = chunkLinks(chunk);
      for (size_t i = 0; i < chunk->count; i++) {
        if (links[i].event != nullptr) {
          links[i].event->setQueueToken(0);
          links[i].event = nullptr;
        }
      }
    }
    for (LinkChunk* chunk = chunks; chunk != nullptr;) {
      LinkChunk* next = chunk->next;
      size_t bytes = sizeof(LinkChunk) + chunk->count * sizeof(HostLink);
      hostMemory::deallocate(chunk, bytes, alignof(LinkChunk));
      chunk = next;
    }
    chunks = nullptr;
    freeLinks = nullptr;
    totalLinks = 0;
  }
};

// A turn's journal: arms and disarms recorded in order, applied host-side when the turn ends. ARM
// names the event to arm; DISARM names the host-queue link index of an already-applied arm; NOP is
// an arm annulled by a disarm in the same turn before it was applied.
struct Record {
  enum Op : uint8_t { NOP, ARM, DISARM } op;
  ArmKind kind;
  Event* event;
  uint64_t index;
};

// Per thread. The records are ordinary (guest-writable) memory the turn appends to; the host reads
// them when the turn ends, validating each (a disarm's index against the pool, an arm against the
// event's own token). `loop` is captured host-side at the turn's start, not from anything the turn
// writes.
struct Journal {
  Record records[EVENT_JOURNAL_CAP];
  size_t count = 0;
  EventLoop* loop = nullptr;
  bool active = false;
};
thread_local Journal tlJournal;

HostQueue* resolveQueue(EventLoop& loop) {
  uint64_t handle = loop.queueHandle();
  if (handle == 0) return nullptr;
  return static_cast<HostQueue*>(hostMemory::resolveSlot(handle));
}

void applyJournal(HostQueue& queue, Journal& journal) {
  for (size_t i = 0; i < journal.count; i++) {
    Record& r = journal.records[i];
    switch (r.op) {
      case Record::NOP:
        break;
      case Record::ARM:
        // Apply only if the event is still the pending arm this record stands for; a disarm earlier
        // in the turn annuls it (NOP) or supersedes it.
        if (tokenState(r.event->queueToken()) == TokenState::PENDING &&
            tokenPayload(r.event->queueToken()) == i) {
          size_t index = queue.armLink(*r.event, r.kind);
          r.event->setQueueToken(makeToken(TokenState::APPLIED, index));
        }
        break;
      case Record::DISARM:
        queue.disarmByIndex(r.index);
        break;
    }
  }
  journal.count = 0;
}

// Apply the calling thread's journal to its loop's host-memory queue. Runs host-side, either at the
// end of the turn (HostEventQueueTurn) or, when the journal fills mid-turn, through this hostcall --
// one VM exit, which the guest kernel turns into a direct call when it is off.
long eventQueueHostcall(hostcall::Frame& frame) {
  (void)frame;
  Journal& journal = tlJournal;
  if (journal.loop != nullptr) {
    HostQueue* queue = resolveQueue(*journal.loop);
    if (queue != nullptr) {
      applyJournal(*queue, journal);
      return 0;
    }
  }
  // No queue to apply to (a forged or cleared handle); drop the records so the slab makes room. The
  // applied arms this would strand are the turn's own; see the handle discussion above.
  journal.count = 0;
  return 0;
}

const hostcall::Entry EVENT_QUEUE_HOSTCALL = {hostcall::EVENT_QUEUE, &eventQueueHostcall};

void flushJournalMidTurn() {
  // Make room by applying the slab. The flush is legal from a noexcept destructor (~Event -> disarm)
  // because the exit is not a C++ throw, and it always makes room: applying a disarm never allocates
  // and never refuses, so the slab empties.
  hostcall::Frame frame{};
  frame.inLen = 0;
  frame.outLen = 0;
  hostcall::call(EVENT_QUEUE_HOSTCALL, frame);
}

void journalArm(Event& event, ArmKind kind) {
  if (tlJournal.count == EVENT_JOURNAL_CAP) flushJournalMidTurn();
  size_t slot = tlJournal.count;
  tlJournal.records[slot] = Record{Record::ARM, kind, &event, 0};
  tlJournal.count = slot + 1;
  event.setQueueToken(makeToken(TokenState::PENDING, slot));
}

// ---- The hooks kj calls (kj/async.h, EventQueueHooks) --------------------------------------------

void armHook(EventLoop& loop, Event& event, ArmKind kind) {
  // kj calls this only for an unarmed event (token zero), so there is no already-armed case here.
  if (hostMemory::inTurn()) {
    journalArm(event, kind);
  } else {
    // Host-side: apply at once and reflect the port's runnable state, as kj's own arm would.
    HostQueue* queue = resolveQueue(loop);
    if (queue == nullptr) return;
    size_t index = queue->armLink(event, kind);
    event.setQueueToken(makeToken(TokenState::APPLIED, index));
    loop.setRunnableFromQueueHooks(true);
  }
}

void disarmHook(EventLoop& loop, Event& event) {
  uint64_t token = event.queueToken();
  TokenState state = tokenState(token);
  uint64_t payload = tokenPayload(token);
  if (hostMemory::inTurn()) {
    if (state == TokenState::PENDING) {
      // Annul the pending arm in place: no new record, nothing to apply. (Still in this journal
      // generation at `payload`; a flush since the arm would have left the event APPLIED instead.)
      if (payload < tlJournal.count && tlJournal.records[payload].event == &event) {
        tlJournal.records[payload].op = Record::NOP;
      }
      event.setQueueToken(0);
    } else if (state == TokenState::APPLIED) {
      if (tlJournal.count == EVENT_JOURNAL_CAP) flushJournalMidTurn();
      size_t slot = tlJournal.count;
      tlJournal.records[slot] = Record{Record::DISARM, ArmKind::DEPTH_FIRST, &event, payload};
      tlJournal.count = slot + 1;
      event.setQueueToken(0);
    }
  } else {
    if (state == TokenState::APPLIED) {
      HostQueue* queue = resolveQueue(loop);
      if (queue != nullptr) queue->disarmByIndex(payload);
    }
    event.setQueueToken(0);
  }
}

Event* headHook(EventLoop& loop) {
  // Inside a turn (guest ring 3) the host-memory queue cannot be read; the only caller there is
  // Event::isNext(), an optimization that is safe to decline, so report an empty queue.
  if (hostMemory::inTurn()) return nullptr;
  HostQueue* queue = resolveQueue(loop);
  return queue == nullptr ? nullptr : queue->head();
}

bool wouldSleepEmptyHook(EventLoop& loop) {
  HostQueue* queue = resolveQueue(loop);
  if (queue == nullptr) return true;
  return queue->wouldSleepHead.next == &queue->wouldSleepTail;
}

Event* firstWouldSleepHook(EventLoop& loop) {
  HostQueue* queue = resolveQueue(loop);
  if (queue == nullptr) return nullptr;
  if (queue->wouldSleepHead.next == &queue->wouldSleepTail) return nullptr;
  return queue->wouldSleepHead.next->event;
}

void resetDepthFirstHook(EventLoop& loop) {
  HostQueue* queue = resolveQueue(loop);
  if (queue == nullptr) return;
  if (queue->headLink.next == &queue->depthPoint) return;
  queue->depthPoint.unlink();
  queue->depthPoint.insertAfter(queue->headLink);
}

void onLoopConstructedHook(EventLoop& loop) {
  // A loop a JS turn builds at guest ring 3 -- a hostile turn, or the event-loop-pointer probe --
  // cannot allocate host memory from there, and does not need to: it is the guest's own loop and
  // never drives the host's turns, so it keeps no host-memory queue (its handle stays zero) and
  // uses its inline queue. The host's own loops are built host-side and get a relocated queue.
  if (hostMemory::inTurn()) return;
  hostMemory::SlotHandle handle = hostMemory::allocateSlot(sizeof(HostQueue), alignof(HostQueue));
  void* mem = hostMemory::resolveSlot(handle);
  KJ_ASSERT(mem != nullptr, "a fresh host slot handle must resolve");
  new (mem) HostQueue();
  loop.setQueueHandle(handle);
}

void onLoopDestroyedHook(EventLoop& loop) {
  uint64_t handle = loop.queueHandle();
  if (handle == 0) return;
  auto* queue = static_cast<HostQueue*>(hostMemory::resolveSlot(handle));
  if (queue != nullptr) {
    queue->teardown();
    queue->~HostQueue();
  }
  hostMemory::freeSlot(handle);
  loop.setQueueHandle(0);
}

const kj::_::EventQueueHooks HOOKS = {
  &armHook,
  &disarmHook,
  &headHook,
  &wouldSleepEmptyHook,
  &firstWouldSleepHook,
  &resetDepthFirstHook,
  &onLoopConstructedHook,
  &onLoopDestroyedHook,
};

}  // namespace

const void* resolveQueueHandleForTest(uint64_t handle) {
  if (handle == 0) return nullptr;
  return hostMemory::resolveSlot(handle);
}

void installHostEventQueue() {
  hostcall::install(EVENT_QUEUE_HOSTCALL);
  kj::_::setEventQueueHooks(&HOOKS);
}

HostEventQueueTurn::HostEventQueueTurn(): outermost(false) {
  if (kj::_::getEventQueueHooks() == nullptr) return;  // not a guest-kernel build: nothing to do
  if (tlJournal.active) return;                        // nested in an outer turn
  outermost = true;
  tlJournal.active = true;
  tlJournal.count = 0;
  tlJournal.loop = kj::EventLoop::saveCurrentLoopPointer();
}

HostEventQueueTurn::~HostEventQueueTurn() noexcept(false) {
  if (!outermost) return;
  if (tlJournal.loop != nullptr) {
    HostQueue* queue = resolveQueue(*tlJournal.loop);
    if (queue != nullptr) {
      applyJournal(*queue, tlJournal);
      tlJournal.loop->setRunnableFromQueueHooks(queue->head() != nullptr);
    }
  }
  tlJournal.count = 0;
  tlJournal.active = false;
  tlJournal.loop = nullptr;
}

}  // namespace workerd
