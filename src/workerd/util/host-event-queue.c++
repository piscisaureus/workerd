// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/util/host-event-queue.h>
#include <workerd/util/host-memory.h>
#include <workerd/util/hostcall.h>

#include <kj/async.h>
#include <kj/debug.h>

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <new>

namespace workerd {
namespace {

using kj::EventLoop;
using Event = kj::_::Event;
using ArmKind = kj::_::ArmKind;

// Whether the guest kernel is active, read once host-side and kept (gk_init() runs before any
// isolate exists and never again). Mirrors host-memory.c++'s own cached check; without the guest
// kernel it is always false and every gk-gated branch below is dropped.
bool guestKernelActive() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  static const bool active = gk_initialized() != 0;
  return active;
#else
  return false;
#endif
}

// The journal capacity before an in-turn arm or disarm must flush it through one VM exit to make
// room. A single named constant so it is tunable; 256 is the chosen size, matching the reserved
// pool a turn's resolve registrations use (util/host-memory.h). It is the capacity the host
// iterates when it applies the journal, never a count the guest can write (see applyJournal).
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
    for (HostLink* s:
        {&headLink, &depthPoint, &breadthPoint, &tailLink, &wouldSleepHead, &wouldSleepTail}) {
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

// A turn's journal record: an arm or disarm recorded in order, applied host-side when the turn
// ends. ARM names the event to arm; DISARM names the host-queue link index of an already-applied
// arm; NOP is an arm annulled by a disarm in the same turn before it was applied, or a slot the
// host has consumed. These are payloads only -- nothing the host trusts rides in them.
struct Record {
  enum Op : uint8_t { NOP, ARM, DISARM } op;
  ArmKind kind;
  Event* event;
  uint64_t index;
};

// The guest-writable per-thread record region: the payloads a turn appends at ring 3, read by the
// host when the turn ends but never trusted. `count` is the turn's own append cursor; the host
// does not read it to decide what to apply -- applyJournal walks the full capacity and validates
// each slot on its own, so a forged cursor can neither drop a record nor send the host into a slot
// the turn did not write (every unwritten slot is NOP). Static thread-local storage is ordinary
// process memory every arena maps writable (memory-classes.md R7), so each isolate's turn on this
// thread appends to the same region, which is why the records live here and not in host memory the
// guest cannot reach or on the C++ heap the first isolate's arena would claim.
struct JournalRecords {
  Record records[EVENT_JOURNAL_CAP] = {};
  size_t count = 0;  // untrusted append cursor; see above
};
thread_local JournalRecords tlRecords;

// The per-thread metadata the host trusts when it applies a journal or flushes it mid-turn: which
// loop and relocated queue the records belong to, and whether a turn is in progress. None of it
// lives in guest-writable memory. With the guest kernel it sits behind a per-thread supervisor
// word (EVENT_THREAD_WORD below) a JS turn at ring 3 cannot write, exactly as the resolve pool
// sits behind POOL_THREAD_WORD (util/host-memory.c++); without the guest kernel it sits behind a
// thread-local, where there is no turn to distrust and today's behavior is preserved. `queue` is
// the resolved host-memory queue, cached host-side while the loop's handle is provably the host's
// own (onLoopConstructed, or the turn's start), so a handle a turn forges mid-turn cannot steer
// which queue a flush or an apply mutates.
struct EventThreadState {
  EventLoop* loop = nullptr;
  HostQueue* queue = nullptr;
  bool active = false;
};

#ifdef WORKERD_HAS_GUEST_KERNEL
// The free per-thread word above the ones already owned: the progress counter's 0..2
// (util/thread-scopes.c++) and the resolve pool's 3 (util/host-memory.c++, POOL_THREAD_WORD).
constexpr unsigned EVENT_THREAD_WORD = 4;
static_assert(EVENT_THREAD_WORD < GK_THREAD_WORDS);
#endif
// Used only without the guest kernel (or before gk_init()); see stateOfThread().
thread_local EventThreadState* fallbackState = nullptr;

EventThreadState* stateOfThread() {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    uint64_t word = 0;
    if (gk_thread_words_get(EVENT_THREAD_WORD, 1, &word) != 0) return nullptr;
    return reinterpret_cast<EventThreadState*>(static_cast<uintptr_t>(word));
  }
#endif
  return fallbackState;
}

void setStateOfThread(EventThreadState* state) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (guestKernelActive()) {
    uint64_t word = reinterpret_cast<uintptr_t>(state);
    KJ_ASSERT(gk_thread_words_set(EVENT_THREAD_WORD, 1, &word) == 0);
    return;
  }
#endif
  fallbackState = state;
}

// Frees the thread's state block when the thread exits. The loop and queue it names are owned
// elsewhere (onLoopDestroyed tears the queue down); this only reclaims the block itself.
struct StateReleaser {
  ~StateReleaser() noexcept(false) {
    EventThreadState* state = stateOfThread();
    if (state == nullptr) return;
    setStateOfThread(nullptr);
    state->~EventThreadState();
    hostMemory::deallocate(state, sizeof(EventThreadState), alignof(EventThreadState));
  }
};

EventThreadState& ensureStateOfThread() {
  EventThreadState* state = stateOfThread();
  if (state == nullptr) {
    state = new (hostMemory::allocate(sizeof(EventThreadState), alignof(EventThreadState)))
        EventThreadState();
    setStateOfThread(state);
    static thread_local StateReleaser releaser;
    (void)releaser;
  }
  return *state;
}

HostQueue* resolveQueue(EventLoop& loop) {
  uint64_t handle = loop.queueHandle();
  if (handle == 0) return nullptr;
  return static_cast<HostQueue*>(hostMemory::resolveSlot(handle));
}

// Apply the records to the loop's host-memory queue. Walks the host-trusted capacity, never
// rec.count: a forged cursor cannot make the host skip a record -- skipping a disarm would strand a
// host-queue link pointing at a freed event for turn() to fire. Each slot is validated on its own
// and consumed (set NOP) as it is read, so that no stale payload survives to a later apply where it
// would be re-read against an event that has since been freed. An arm is honored only while the
// event still holds the PENDING token this very slot wrote; a disarm's link index is bounds-checked
// against the loop's own pool inside disarmByIndex. A payload a turn scribbles can therefore do no
// more than disturb this isolate's own queue (fired at ring 3, within the isolate).
void applyJournal(HostQueue& queue, JournalRecords& rec) {
  for (size_t i = 0; i < EVENT_JOURNAL_CAP; i++) {
    Record& r = rec.records[i];
    Record::Op op = r.op;
    r.op = Record::NOP;  // consume the slot before acting on it
    switch (op) {
      case Record::NOP:
        break;
      case Record::ARM:
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
  rec.count = 0;
}

// Apply the calling thread's records to its loop's host-memory queue. Runs host-side, either at the
// end of the turn (HostEventQueueTurn) or, when the records fill mid-turn, through this hostcall --
// one VM exit, which the guest kernel turns into a direct call when it is off. The queue comes from
// the host-trusted per-thread state, never from anything the turn can write, so a forged handle or
// loop pointer cannot steer which queue this mutates.
long eventQueueHostcall(hostcall::Frame& frame) {
  (void)frame;
  EventThreadState* state = stateOfThread();
  if (state != nullptr && state->queue != nullptr) {
    applyJournal(*state->queue, tlRecords);
    return 0;
  }
  // No queue to apply to (none cached for this thread); drop the records so the slab makes room.
  // The applied arms this would strand are the turn's own.
  tlRecords.count = 0;
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
  // count is the turn's own cursor; a value it has drifted to at or past the capacity flushes the
  // slab (which resets the cursor) rather than writing past the records, so a forged cursor harms
  // only this turn's own bookkeeping, never host memory.
  if (tlRecords.count >= EVENT_JOURNAL_CAP) flushJournalMidTurn();
  size_t slot = tlRecords.count;
  tlRecords.records[slot] = Record{Record::ARM, kind, &event, 0};
  tlRecords.count = slot + 1;
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
      // Annul the pending arm in place: no new record, nothing to apply. The slot is bounded by the
      // capacity (not the untrusted cursor) and confirmed to still name this event; if it does not,
      // the arm is left to the apply, which skips it anyway because the token below goes to zero.
      // (A flush since the arm would have left the event APPLIED instead of PENDING.)
      if (payload < EVENT_JOURNAL_CAP && tlRecords.records[payload].event == &event) {
        tlRecords.records[payload].op = Record::NOP;
      }
      event.setQueueToken(0);
    } else if (state == TokenState::APPLIED) {
      if (tlRecords.count >= EVENT_JOURNAL_CAP) flushJournalMidTurn();
      size_t slot = tlRecords.count;
      tlRecords.records[slot] = Record{Record::DISARM, ArmKind::DEPTH_FIRST, &event, payload};
      tlRecords.count = slot + 1;
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
  auto* queue = new (mem) HostQueue();
  loop.setQueueHandle(handle);
  // Cache the loop and its just-resolved queue in the host-trusted per-thread state, while the
  // handle is provably the host's own (a turn has not run on this loop yet). The turn-end apply and
  // the mid-turn flush read the queue from here, never by re-resolving a handle a turn can forge.
  EventThreadState& st = ensureStateOfThread();
  st.loop = &loop;
  st.queue = queue;
}

void onLoopDestroyedHook(EventLoop& loop) {
  uint64_t handle = loop.queueHandle();
  if (handle == 0) return;
  auto* queue = static_cast<HostQueue*>(hostMemory::resolveSlot(handle));
  EventThreadState* st = stateOfThread();
  if (st != nullptr && st->loop == &loop) {
    st->loop = nullptr;
    st->queue = nullptr;
  }
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

// A standalone event for the count-forge check: it carries only a queue token and is never fired or
// armed through kj, so it needs no running loop. Its token is cleared before it is destroyed, so
// ~Event's disarm() sees it unarmed and touches no queue.
struct TestEvent final: public Event {
  TestEvent(): Event(kj::SourceLocation()) {}
  void fire() override {}
  void traceEvent(kj::_::TraceBuilder&) override {}
};

bool countForgeInert() {
  // The standalone events are destroyed on this host frame, which may sit under a scope that
  // forbids destroying async objects; this check builds and tears down its own, so permit it.
  kj::AllowAsyncDestructorsScope allowDestructors;
  HostQueue queue;
  JournalRecords rec;
  TestEvent e0;
  TestEvent e1;

  // Arm two events through the journal and apply, as a turn's end would: both take a host link and
  // their tokens become APPLIED, with e0 at the head.
  rec.records[0] = Record{Record::ARM, ArmKind::DEPTH_FIRST, &e0, 0};
  e0.setQueueToken(makeToken(TokenState::PENDING, 0));
  rec.records[1] = Record{Record::ARM, ArmKind::DEPTH_FIRST, &e1, 0};
  e1.setQueueToken(makeToken(TokenState::PENDING, 1));
  rec.count = 2;
  applyJournal(queue, rec);

  bool armedBoth = queue.head() == &e0 && tokenState(e0.queueToken()) == TokenState::APPLIED &&
      tokenState(e1.queueToken()) == TokenState::APPLIED;

  // The attack: record a disarm of e0 at a high slot, then forge the cursor down to zero. An apply
  // that trusted the cursor would walk no records and leave e0's link live past its disarm.
  uint64_t e0LinkIndex = tokenPayload(e0.queueToken());
  size_t highSlot = EVENT_JOURNAL_CAP - 1;
  rec.records[highSlot] = Record{Record::DISARM, ArmKind::DEPTH_FIRST, &e0, e0LinkIndex};
  rec.count = 0;  // forged
  applyJournal(queue, rec);

  // The disarm ran despite the forged cursor: e0's link is freed and the head is now e1, so no link
  // is left pointing at the event the disarm named. That is the forge shown inert.
  bool disarmRan = queue.head() == &e1;

  // Leave the events unarmed before they fall out of scope (~Event would otherwise read the token
  // as a link), and tear the standalone queue down.
  e0.setQueueToken(0);
  e1.setQueueToken(0);
  queue.teardown();

  return armedBoth && disarmRan;
}

}  // namespace

const void* resolveQueueHandleForTest(uint64_t handle) {
  if (handle == 0) return nullptr;
  return hostMemory::resolveSlot(handle);
}

bool countForgeInertForTest() {
  return countForgeInert();
}

void installHostEventQueue() {
  hostcall::install(EVENT_QUEUE_HOSTCALL);
  kj::_::setEventQueueHooks(&HOOKS);
}

HostEventQueueTurn::HostEventQueueTurn(): outermost(false) {
  if (kj::_::getEventQueueHooks() == nullptr) return;  // not a guest-kernel build: nothing to do
  EventThreadState& st = ensureStateOfThread();
  if (st.active) return;  // nested in an outer turn
  outermost = true;
  st.active = true;
  // The loop running on this thread, read host-side before the turn can forge anything. Its queue
  // was cached host-side at onLoopConstructed; resolve it here for a loop whose cache this thread
  // does not hold (handle still the host's own, the turn not yet entered).
  EventLoop* loop = kj::EventLoop::saveCurrentLoopPointer();
  if (loop != nullptr && loop != st.loop) {
    st.loop = loop;
    st.queue = resolveQueue(*loop);
  }
}

HostEventQueueTurn::~HostEventQueueTurn() noexcept(false) {
  if (!outermost) return;
  EventThreadState& st = ensureStateOfThread();
  if (st.queue != nullptr) {
    applyJournal(*st.queue, tlRecords);
    if (st.loop != nullptr) st.loop->setRunnableFromQueueHooks(st.queue->head() != nullptr);
  }
  tlRecords.count = 0;
  st.active = false;
}

}  // namespace workerd
