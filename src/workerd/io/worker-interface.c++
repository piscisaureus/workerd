// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-interface.h"

#include <workerd/jsg/heap-arena.h>
#include <workerd/util/http-util.h>
#include <workerd/util/stream-utils.h>

#include <kj/debug.h>

using kj::byte;
using kj::uint;

namespace workerd {

namespace {
// A WorkerInterface that delays requests until some promise resolves, then forwards them to the
// interface the promise resolved to.
class PromisedWorkerInterface final: public WorkerInterface {
 public:
  PromisedWorkerInterface(kj::Promise<kj::Own<WorkerInterface>> promise)
      : promise(promise.then([this](kj::Own<WorkerInterface> result) { worker = kj::mv(result); })
                    .fork()) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    KJ_IF_SOME(w, worker) {
      co_await w->request(method, url, headers, requestBody, response);
    } else {
      co_await promise;
      co_await KJ_ASSERT_NONNULL(worker)->request(method, url, headers, requestBody, response);
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_IF_SOME(w, worker) {
      co_await w->connect(host, headers, connection, response, kj::mv(settings));
    } else {
      co_await promise;
      co_await KJ_ASSERT_NONNULL(worker)->connect(
          host, headers, connection, response, kj::mv(settings));
    }
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w->prewarm(url);
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->prewarm(url);
    }
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_IF_SOME(wrk, worker) {
      co_return co_await wrk->runScheduled(scheduledTime, cron);
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->runScheduled(scheduledTime, cron);
    }
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w->runAlarm(scheduledTime, retryCount);
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->runAlarm(scheduledTime, retryCount);
    }
  }

  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w->abandonAlarm(scheduledTime);
    } else {
      co_await promise;
      co_return co_await KJ_ASSERT_NONNULL(worker)->abandonAlarm(scheduledTime);
    }
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    KJ_IF_SOME(w, worker) {
      co_return co_await w->customEvent(kj::mv(event));
    } else {
      try {
        co_await promise;
      } catch (...) {
        // Due to the exception, we're going to discard our CustomEvent. But we should tell it
        // about why it failed first. This is important for JsRpcSessionCustomEvent in
        // particular, as it needs to resolve the RPC client to the correct error.
        auto exception = kj::getCaughtExceptionAsKj();
        event->failed(exception);
        kj::throwFatalException(kj::mv(exception));
      }
      co_return co_await KJ_ASSERT_NONNULL(worker)->customEvent(kj::mv(event));
    }
  }

 private:
  kj::ForkedPromise<void> promise;
  kj::Maybe<kj::Own<WorkerInterface>> worker;
};
}  // namespace

kj::Own<WorkerInterface> newPromisedWorkerInterface(kj::Promise<kj::Own<WorkerInterface>> promise) {
  return kj::heap<PromisedWorkerInterface>(kj::mv(promise));
}

namespace {
// The relays below stage what crosses an in-process binding through buffers of at most this
// size, allocated by the event loop.
constexpr size_t RELAY_STAGING_BYTES = 64 * 1024;

// Reads from `inner` into `dst` from the event loop, outside any turn, where every arena is
// addressable: yields first, so that the read itself, the node it leaves on the pipe and the
// staging buffer are the loop's, then copies into `dst`, which is in the reader's arena. Called
// inside a jsg::SharedHeapScope, so that the coroutine's frame -- the event the writer's turn
// arms when it fills the staging buffer -- is in the shared heap.
kj::Promise<size_t> relayRead(
    kj::AsyncInputStream& inner, byte* dst, size_t minBytes, size_t maxBytes) {
  co_await kj::yield();
  auto staging = kj::heapArray<byte>(kj::min(maxBytes, RELAY_STAGING_BYTES));
  size_t total = 0;
  while (total < minBytes) {
    size_t want = kj::min(maxBytes - total, staging.size());
    size_t need = kj::min(minBytes - total, want);
    size_t n = co_await inner.tryRead(staging.begin(), need, want);
    memcpy(dst + total, staging.begin(), n);
    total += n;
    if (n < need) break;  // EOF.
  }
  co_return total;
}

// Relays the reads of a body that crosses an in-process service binding, so that no isolate's
// turn reaches the other isolate's side of the pipe the body travels through.
//
// A request or response body crosses the binding through a kj one-way pipe whose two ends are
// held by different isolates: the caller's turn writes the request body the callee's turn reads,
// and the other way round for the response. The pipe copies between the writer's buffer and the
// reader's in whichever of the two operations arrives second, so under per-isolate heap arenas
// the copy runs in one isolate's turn over a buffer in the other isolate's arena, which that turn
// cannot address. The bytes are not all that crosses: the state object the first operation
// leaves on the pipe (its BlockedRead or BlockedWrite, a promise node allocated in the first
// party's arena) is read and written by the second party, and completing it arms the first
// party's continuation, which is a write into the first party's arena too. Placing the buffers
// in the shared heap would therefore not be enough.
//
// The relay sits at the reading end, in front of the pipe, and makes every operation on the pipe
// from the event loop (relayRead above). The pipe only ever sees host-side reads, whose
// BlockedRead node and staging buffer are in the shared heap; the writer's turn fills the staging
// buffer and arms the relay's coroutine, whose frame is in the shared heap for exactly that; and
// the reader's buffer is touched by the reader's own turn and by the host, never by the writer.
// The writer's end needs nothing: what it leaves on the pipe is read only by the host, and its
// buffers keep their isolate provenance, as do the reader's. The cost is one more copy and one
// event-loop turn per chunk, and no synchronous fast path, on bodies that cross an in-process
// binding; it is paid only where per-isolate heap arenas are active (see asHttpClient() below).
//
// A response body the relay owns is dropped on the event loop whichever turn drops the relay.
// kj's HttpClient adapter ties the body to the callee's request: the body holds the promise of
// the callee's whole request, so dropping it mid-body cancels that request, and cancelling it
// destroys the callee's request coroutine, whose suspended frame holds the promise node its own
// turn built for the `co_await` it is parked on, in the callee's arena. The caller's turn drops
// the body whenever its JS cancels the stream -- reader.cancel(), an AbortController, an
// AbortSignal.timeout() -- so the relay hands the body, with the client it came from and may
// not outlive, to the event loop instead, where every arena is addressable, as a daemon of the
// loop. A body read to its end holds nothing of the callee's by then (the callee's coroutine
// has completed and its locals are gone), so that drop was never the problem; the mid-body one
// was.
class ReadRelay final: public kj::AsyncInputStream {
 public:
  // Over a stream this relay owns: a response body, with the client it was obtained from.
  ReadRelay(kj::Own<kj::AsyncInputStream> owned, kj::Own<kj::HttpClient> client)
      : inner(*owned),
        owned(kj::mv(owned)),
        client(kj::mv(client)) {}
  // Over a stream someone else owns for the relay's lifetime: a request body.
  explicit ReadRelay(kj::AsyncInputStream& inner): inner(inner) {}

  ~ReadRelay() noexcept(false) {
    KJ_IF_SOME(o, owned) {
#ifndef WORKERD_GK_NEG_BODY_DROP_DEFER
      // The promise and the loop's record of it are made here, in whichever turn drops the body,
      // and run by the loop. A negative build, never shipped, WORKERD_GK_NEG_BODY_DROP_DEFER,
      // drops the body in the dropping turn; the guest kernel's binding-crossing-test.sh must
      // then fail on a body cancelled mid-stream.
      jsg::SharedHeapScope sharedHeap;
      kj::evalLater([body = o.attach(kj::mv(client))](){}).detach([](kj::Exception&&){});
#endif
    }
  }

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    if (minBytes == 0) return size_t(0);
    jsg::SharedHeapScope sharedHeap;
    return relayRead(inner, static_cast<byte*>(buffer), minBytes, maxBytes);
  }

  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<byte> buffer, size_t minBytes) override {
    // A synchronous read would run in the reader's turn against what the writer left on the
    // pipe. Nothing is available synchronously through the relay.
    return kj::none;
  }

  kj::Maybe<uint64_t> tryGetLength() override {
    return inner.tryGetLength();
  }

  // pumpTo() is the default: the output pumps from this stream through tryRead(), or the
  // generic loop does, so the pipe still sees only the relay's host-side reads.

 private:
  kj::AsyncInputStream& inner;
  kj::Maybe<kj::Own<kj::AsyncInputStream>> owned;
  kj::Own<kj::HttpClient> client;
};

// Relays the reads of a connect() tunnel that crosses an in-process service binding, at both
// of its ends, so that neither isolate's turn reaches the other's side of the pipe.
//
// The tunnel is a kj two-way pipe: two one-way pipes, each written by one isolate and read by
// the other, so each is the body's case over again (see ReadRelay) and gets the same treatment
// at its reading end. The caller's end is wrapped where the tunnel is handed back
// (SharedHeapHttpClient::connect()), the callee's where it is handed over
// (BodyRelayService::connect()); every read on either end is the loop's, so what a writer's
// turn finds on its pipe is the host's BlockedRead and staging buffer or nothing, and what it
// leaves there -- its BlockedWrite, the pieces of a tryPumpFrom(), the ShutdownedWrite its
// shutdownWrite() makes -- is read only by the loop. The write side therefore passes straight
// through. abortRead() passes through too, although it is a read-side operation that would
// fault on a parked BlockedWrite: nothing calls it from a turn. workerd's streams cancel a
// read through a kj::Canceler and never abort the kj stream itself, kj's adapter and the
// revocable interface above call it on the loop, and the one in ~TwoWayPipeEnd runs where
// the end is dropped, which for the end this relay owns is the loop (below).
//
// The caller's end is dropped on the event loop whichever turn drops the relay. kj's adapter
// attaches the callee's whole connect() call to that end, so dropping it mid-tunnel cancels the
// callee's suspended coroutine as dropping a body does (ReadRelay above), and its ~TwoWayPipeEnd
// aborts the read side of the pipe the callee writes, which reaches whatever write the callee's
// turn left parked there. The caller's turn drops the end whenever its JS closes the socket
// (Socket::close() releases the IoOwn inside the owning context) or lets it go.
class IoStreamRelay final: public kj::AsyncIoStream {
 public:
  // Over the end the relay owns: the caller's, with the client it came from and may not outlive.
  IoStreamRelay(kj::Own<kj::AsyncIoStream> owned, kj::Own<kj::HttpClient> client)
      : inner(*owned),
        owned(kj::mv(owned)),
        client(kj::mv(client)) {}
  // Over the end someone else owns for the relay's lifetime: the callee's.
  explicit IoStreamRelay(kj::AsyncIoStream& inner): inner(inner) {}

  ~IoStreamRelay() noexcept(false) {
    KJ_IF_SOME(o, owned) {
#ifndef WORKERD_GK_NEG_TUNNEL_DROP_DEFER
      // As ~ReadRelay. A negative build, never shipped, WORKERD_GK_NEG_TUNNEL_DROP_DEFER, drops
      // the end in the dropping turn; the guest kernel's binding-crossing-test.sh must then fail
      // on a tunnel closed with the callee's write parked.
      jsg::SharedHeapScope sharedHeap;
      kj::evalLater([end = o.attach(kj::mv(client))](){}).detach([](kj::Exception&&){});
#endif
    }
  }

  // The read side, which is the loop's.

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
    if (minBytes == 0) return size_t(0);
    jsg::SharedHeapScope sharedHeap;
    return relayRead(inner, static_cast<byte*>(buffer), minBytes, maxBytes);
  }
  kj::Maybe<size_t> tryReadSync(kj::ArrayPtr<byte> buffer, size_t minBytes) override {
    // See ReadRelay::tryReadSync(): the pipe's state is the writer's, and even EOF is known only
    // by reading it.
    return kj::none;
  }
  kj::Maybe<uint64_t> tryGetLength() override {
    return inner.tryGetLength();
  }
  // pumpTo() is the default, as for ReadRelay.
  void abortRead() override {
    inner.abortRead();
  }

  // The write side, which is the writer's own.

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    return inner.write(buffer);
  }
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner.write(pieces);
  }
  bool tryWriteSync(kj::ArrayPtr<const byte> buffer) override {
    return inner.tryWriteSync(buffer);
  }
  bool tryWriteSync(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    return inner.tryWriteSync(pieces);
  }
  kj::Maybe<kj::Promise<uint64_t>> tryPumpFrom(
      kj::AsyncInputStream& input, uint64_t amount) override {
    return inner.tryPumpFrom(input, amount);
  }
  kj::Promise<void> whenWriteDisconnected() override {
    return inner.whenWriteDisconnected();
  }
  void shutdownWrite() override {
    inner.shutdownWrite();
  }
  void abortWrite(kj::Exception&& exception) override {
    inner.abortWrite(kj::mv(exception));
  }

 private:
  kj::AsyncIoStream& inner;
  kj::Maybe<kj::Own<kj::AsyncIoStream>> owned;
  kj::Own<kj::HttpClient> client;
};

// Relays a WebSocket that crosses an in-process service binding, at both of its ends, so that
// neither isolate's turn reaches the other's side of the pipe its messages travel through.
//
// The WebSocket is a kj WebSocket pipe (made by kj's adapter when the callee accepts the
// upgrade, in the shared heap: WorkerEntrypoint's ResponseSentTracker scopes acceptWebSocket()),
// whose two directions each copy a message in whichever of send() and receive() arrives second,
// from the sender's buffer into an array the second operation allocates, completing the first
// operation's node. Unlike a byte pipe it has no reader's buffer to copy into: the array is
// made where the copy is made and handed to the receiver's turn. The relay therefore does two
// things. Every receive() is the loop's, so a sender's turn finds the host's BlockedReceive on
// the pipe or nothing (ReadRelay's trick); and every send() and close() runs under a
// jsg::SharedHeapScope, so the array a send that arrives second makes in the sender's turn is
// in the shared heap, where the receiver's turn can read it, as is the BlockedSend a send that
// arrives first leaves for the loop. disconnect() runs under the scope for the state it leaves
// on the pipe, which the other side's turn reaches through its own sends.
//
// kj's pumps do not go through receive(): a WebSocketPipeEnd's pumpTo() hands the destination
// to the pipe itself, which then runs the other side's sends straight into the destination, in
// the other side's turn, and that is where a callee's turn coupling its end to a WebSocketPair
// left the state the caller's first send() faulted on. The relay does not forward pumpTo() or
// tryPumpFrom(): it keeps kj::WebSocket's defaults, which pump through the destination's
// tryPumpFrom() or through a receive()/send() loop, so every message still crosses the pipe
// through a relayed receive.
//
// abort() is the loop's, and so is the drop of the end. Aborting a direction completes
// whatever the other side left parked there and fulfils the other side's whenAborted(), both
// writes into the other side's arena; and dropping an end aborts both directions. The caller's
// end also holds, through kj's DelayedCloseWebSocket, the callee's whole request, which dropping
// it mid-stream cancels as dropping a body does (ReadRelay above): the caller's turn drops it
// when an AbortSignal fires on the upgrade fetch, or lets it go. Both go to the loop as tasks
// that share the end, so an abort scheduled before the drop runs on a live end.
class WebSocketRelay final: public kj::WebSocket {
 public:
  // `client` is what the caller's end came from and may not outlive; the callee's end has none.
  WebSocketRelay(kj::Own<kj::WebSocket> inner, kj::Maybe<kj::Own<kj::HttpClient>> client)
      : end(kj::rc<End>(kj::mv(inner), kj::mv(client))) {}

  ~WebSocketRelay() noexcept(false) {
#ifndef WORKERD_GK_NEG_WS_DROP_DEFER
    // As ~ReadRelay. A negative build, never shipped, WORKERD_GK_NEG_WS_DROP_DEFER, drops the end
    // in the dropping turn and aborts it there; the guest kernel's binding-crossing-test.sh must
    // then fail on an upgrade fetch aborted mid-stream.
    jsg::SharedHeapScope sharedHeap;
    kj::evalLater([end = kj::mv(end)](){}).detach([](kj::Exception&&){});
#endif
  }

  kj::Promise<void> send(kj::ArrayPtr<const byte> message) override {
    SendScope scope;
    return end->ws->send(message);
  }
  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    SendScope scope;
    return end->ws->send(message);
  }
  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    SendScope scope;
    return end->ws->close(code, reason);
  }
  void disconnect() override {
    SendScope scope;
    end->ws->disconnect();
  }

  void abort() override {
#ifndef WORKERD_GK_NEG_WS_DROP_DEFER
    jsg::SharedHeapScope sharedHeap;
    kj::evalLater([end = end.addRef()]() mutable {
      end->ws->abort();
    }).detach([](kj::Exception&&){});
#else
    end->ws->abort();
#endif
  }
  kj::Promise<void> whenAborted() override {
    // What this leaves on the pipe is fulfilled by the other side's abort(), which is the loop's.
    return end->ws->whenAborted();
  }

  kj::Promise<Message> receive(size_t maxSize) override {
    jsg::SharedHeapScope sharedHeap;
    return relayReceive(*end->ws, maxSize);
  }
  // pumpTo() and tryPumpFrom() are kj::WebSocket's defaults: see above.

  uint64_t sentByteCount() override {
    return end->ws->sentByteCount();
  }
  uint64_t receivedByteCount() override {
    return end->ws->receivedByteCount();
  }
  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    // Answered by the pipe from the WebSockets its ends are pumping to, which with the relay in
    // front of both ends are none.
    return end->ws->getPreferredExtensions(ctx);
  }

 private:
  struct End final: public kj::Refcounted {
    End(kj::Own<kj::WebSocket> ws, kj::Maybe<kj::Own<kj::HttpClient>> client): ws(kj::mv(ws)) {
      KJ_IF_SOME(c, client) {
        // Dropped after the WebSocket, which may not outlive it.
        this->ws = this->ws.attach(kj::mv(c));
      }
    }
    kj::Own<kj::WebSocket> ws;
  };

#ifdef WORKERD_GK_NEG_WS_SEND_HEAP
  // A negative build, never shipped: a send's copy is made in the sender's arena; the guest
  // kernel's binding-crossing-test.sh must then fail on the receiver's first message.
  struct SendScope {};
#else
  using SendScope = jsg::SharedHeapScope;
#endif

  // The loop's receive: as relayRead(), for a message.
  static kj::Promise<Message> relayReceive(kj::WebSocket& inner, size_t maxSize) {
    co_await kj::yield();
    co_return co_await inner.receive(maxSize);
  }

  kj::Rc<End> end;
};

// Whether bodies cross an in-process call through a ReadRelay, tunnels through IoStreamRelays
// and WebSockets through WebSocketRelays: only where per-isolate heap arenas are active, which
// is the one configuration in which an isolate's turn cannot address what another isolate's
// turn wrote. Everywhere else the pipes are used directly.
bool bodiesRelayed() {
#ifdef WORKERD_GK_NEG_BODY_RELAY
  // A negative build, never shipped: bodies cross the binding through the bare pipe even with
  // heap arenas on, so a body read faults the reading isolate's turn. The guest kernel's
  // body-arena-test.sh must then fail; this is the check that the test tests the relay.
  return false;
#else
  return jsg::heapArenasAvailable();
#endif
}
bool tunnelsRelayed() {
#ifdef WORKERD_GK_NEG_TUNNEL_RELAY
  // A negative build, never shipped: binding-crossing-test.sh must fail on its first tunnel.
  return false;
#else
  return jsg::heapArenasAvailable();
#endif
}
bool webSocketsRelayed() {
#ifdef WORKERD_GK_NEG_WS_RELAY
  // A negative build, never shipped: binding-crossing-test.sh must fail on its first WebSocket.
  return false;
#else
  return jsg::heapArenasAvailable();
#endif
}

// The Response an in-process callee answers into: puts a WebSocketRelay on the callee's end of
// a WebSocket it accepts. A plain response passes through; its body is relayed at the reading
// end (SharedHeapHttpClient::request()), and what accepting makes -- the pipe, the headers'
// copy -- WorkerEntrypoint's ResponseSentTracker already places in the shared heap.
class RelayResponse final: public kj::HttpService::Response {
 public:
  explicit RelayResponse(kj::HttpService::Response& inner): inner(inner) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    return inner.send(statusCode, statusText, headers, expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    auto ws = inner.acceptWebSocket(headers);
    if (webSocketsRelayed()) {
      return kj::heap<WebSocketRelay>(kj::mv(ws), kj::none);
    }
    return ws;
  }

 private:
  kj::HttpService::Response& inner;
};

// The HttpService side of an in-process call: puts a ReadRelay in front of the request body
// before the target reads it, an IoStreamRelay in front of the callee's end of a tunnel, and a
// RelayResponse behind the response. request() and connect() are called from the caller's turn,
// inside SharedHeapHttpClient's scope, so the relays and these frames are in the shared heap.
class BodyRelayService final: public kj::HttpService {
 public:
  explicit BodyRelayService(kj::HttpService& inner): inner(inner) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    ReadRelay relay(requestBody);
    RelayResponse relayResponse(response);
    co_await inner.request(method, url, headers, relay, relayResponse);
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    if (!tunnelsRelayed()) {
      co_return co_await inner.connect(host, headers, connection, response, kj::mv(settings));
    }
    IoStreamRelay relay(connection);
    co_await inner.connect(host, headers, relay, response, kj::mv(settings));
  }

 private:
  kj::HttpService& inner;
};

// The HttpClient over a WorkerInterface, with every call made inside a jsg::SharedHeapScope. A
// call starts the target's handling of the request synchronously, on the calling thread, which
// holds the calling isolate's lock. When the target is a Worker in this process, everything
// created for it up to its first suspension -- the copies of the URL and headers, the body pipe
// and the response object the adapter makes, the entrypoint and its coroutine frame, the new
// request's IoContext -- is created here, and would otherwise lie in the calling isolate's heap
// arena, where the target's turns fault on it.
class SharedHeapHttpClient final: public kj::HttpClient, public kj::Refcounted {
 public:
  explicit SharedHeapHttpClient(kj::Own<kj::HttpClient> inner): inner(kj::mv(inner)) {}

  Request request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    jsg::SharedHeapScope sharedHeap;
    auto result = inner->request(method, url, headers, expectedBodySize);
    if (bodiesRelayed()) {
      // The response body is read by the caller's turns and written by the callee's: see
      // ReadRelay. The relay is made where the response is delivered, on the event loop.
      result.response = result.response.then([self = kj::addRef(*this)](Response response) mutable {
        response.body = kj::heap<ReadRelay>(kj::mv(response.body), kj::mv(self));
        return kj::mv(response);
      });
    }
    return result;
  }

  kj::Promise<WebSocketResponse> openWebSocket(
      kj::StringPtr url, const kj::HttpHeaders& headers) override {
    jsg::SharedHeapScope sharedHeap;
    auto promise = inner->openWebSocket(url, headers);
    if (bodiesRelayed()) {
      // A callee that answers an upgrade request with a plain response hands back a body that
      // crosses like any other: it gets the same relay. A WebSocket gets the caller's end of a
      // WebSocketRelay. Both are made where the response is delivered, on the event loop.
      promise = promise.then([self = kj::addRef(*this)](WebSocketResponse response) mutable {
        KJ_SWITCH_ONEOF(response.webSocketOrBody) {
          KJ_CASE_ONEOF(body, kj::Own<kj::AsyncInputStream>) {
            body = kj::heap<ReadRelay>(kj::mv(body), kj::mv(self));
          }
          KJ_CASE_ONEOF(ws, kj::Own<kj::WebSocket>) {
            if (webSocketsRelayed()) {
              ws = kj::heap<WebSocketRelay>(kj::mv(ws), kj::mv(self));
            }
          }
        }
        return kj::mv(response);
      });
    }
    return promise;
  }

  ConnectRequest connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::HttpConnectSettings settings) override {
    jsg::SharedHeapScope sharedHeap;
    auto result = inner->connect(host, headers, kj::mv(settings));
    if (tunnelsRelayed()) {
      // The caller's end of the tunnel: see IoStreamRelay.
      result.connection = kj::heap<IoStreamRelay>(kj::mv(result.connection), kj::addRef(*this));
    }
    return result;
  }

 private:
  kj::Own<kj::HttpClient> inner;
};
}  // namespace

kj::Own<kj::HttpClient> asHttpClient(kj::Own<WorkerInterface> workerInterface) {
  if (bodiesRelayed()) {
    auto relay = kj::heap<BodyRelayService>(*workerInterface);
    auto client = kj::newHttpClient(*relay).attach(kj::mv(relay)).attach(kj::mv(workerInterface));
    return kj::refcounted<SharedHeapHttpClient>(kj::mv(client));
  }
  return kj::refcounted<SharedHeapHttpClient>(
      kj::newHttpClient(*workerInterface).attach(kj::mv(workerInterface)));
}

// =======================================================================================
namespace {
// A Revocable WebSocket wrapper, revoked when revokeProm rejects
class RevocableWebSocket final: public kj::WebSocket {
 public:
  RevocableWebSocket(kj::Own<WebSocket> ws, kj::Promise<void> revokeProm)
      : ws(kj::mv(ws)),
        revokeProm(revokeProm
                       .catch_([this](kj::Exception&& e) -> kj::Promise<void> {
                         canceler.cancel(e.clone());
                         KJ_IF_SOME(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
                           (ws)->abort();
                         }
                         this->ws = kj::mv(e);
                         return kj::READY_NOW;
                       })
                       .eagerlyEvaluate(nullptr)) {}

  kj::Promise<void> send(kj::ArrayPtr<const byte> message) override {
    return wrap<void>(getInner().send(message));
  }
  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return wrap<void>(getInner().send(message));
  }

  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return wrap<void>(getInner().close(code, reason));
  }

  void disconnect() override {
    KJ_IF_SOME(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
      return (ws)->disconnect();
    }
  }

  void abort() override {
    KJ_IF_SOME(ws, this->ws.tryGet<kj::Own<kj::WebSocket>>()) {
      return (ws)->abort();
    }
  }

  kj::Promise<void> whenAborted() override {
    return wrap<void>(getInner().whenAborted());
  }

  kj::Promise<Message> receive(size_t maxSize) override {
    return wrap<Message>(getInner().receive(maxSize));
  }

  kj::Promise<void> pumpTo(WebSocket& other) override {
    return wrap<void>(getInner().pumpTo(other));
  }

  kj::Maybe<kj::Promise<void>> tryPumpFrom(WebSocket& other) override {
    return wrap<void>(other.pumpTo(getInner()));
  }

  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    return getInner().getPreferredExtensions(ctx);
  };

  uint64_t sentByteCount() override {
    return 0;
  }
  uint64_t receivedByteCount() override {
    return 0;
  }

 private:
  template <typename T>
  kj::Promise<T> wrap(kj::Promise<T> prom) {
    // just to fix the revocation promise return type, serves no purpose otherwise
    return canceler.wrap(kj::mv(prom));
  }

  kj::WebSocket& getInner() {
    KJ_SWITCH_ONEOF(ws) {
      KJ_CASE_ONEOF(e, kj::Exception) {
        kj::throwFatalException(e.clone());
      }
      KJ_CASE_ONEOF(ws, kj::Own<kj::WebSocket>) {
        return *ws.get();
      }
    }
    KJ_UNREACHABLE;
  }

  kj::OneOf<kj::Exception, kj::Own<kj::WebSocket>> ws;
  kj::Promise<void> revokeProm;
  kj::Canceler canceler;
};

// A HttpResponse that can revoke long-running websocket connections started as part of the
// response. Ordinary HTTP requests are not revoked.
class RevocableWebSocketHttpResponse final: public kj::HttpService::Response {
 public:
  RevocableWebSocketHttpResponse(kj::HttpService::Response& inner, kj::Promise<void> revokeProm)
      : inner(inner),
        revokeProm(revokeProm.fork()) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    return inner.send(statusCode, statusText, headers, expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    return kj::heap<RevocableWebSocket>(inner.acceptWebSocket(headers), revokeProm.addBranch());
  }

 private:
  kj::HttpService::Response& inner;
  kj::ForkedPromise<void> revokeProm;
};

// A WorkerInterface that cancels WebSockets when revokeProm is rejected.
// Currently only supports cancelling for upgrades.
class RevocableWebSocketWorkerInterface final: public WorkerInterface {
 public:
  RevocableWebSocketWorkerInterface(WorkerInterface& worker, kj::Promise<void> revokeProm);
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override;
  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override;
  kj::Promise<void> prewarm(kj::StringPtr url) override;
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override;
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override;
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override;

 private:
  WorkerInterface& worker;
  kj::ForkedPromise<void> revokeProm;
};

kj::Promise<void> RevocableWebSocketWorkerInterface::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    kj::HttpService::Response& response) {
  auto wrappedResponse = kj::heap<RevocableWebSocketHttpResponse>(response, revokeProm.addBranch());
  return worker.request(method, url, headers, requestBody, *wrappedResponse)
      .attach(kj::mv(wrappedResponse));
}

kj::Promise<void> RevocableWebSocketWorkerInterface::connect(kj::StringPtr host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    ConnectResponse& response,
    kj::HttpConnectSettings settings) {
  // We give TCP sockets the same treatment as WebSockets because the purpose here is to
  // disconnect long-running connections, e.g. on a code update for a Durable Object, and that
  // applies equally to TCP sockets.
  auto wrappedConnection = newNeuterableIoStream(connection);
  auto* wrappedConnectionPtr = wrappedConnection.get();
  auto revokeTask =
      revokeProm.addBranch()
          .catch_([&connection, wrappedConnectionPtr](kj::Exception&& e) -> kj::Promise<void> {
    wrappedConnectionPtr->neuter(e.clone());
    connection.abortWrite(kj::mv(e));
    connection.abortRead();
    return kj::READY_NOW;
  }).eagerlyEvaluate(nullptr);

  return worker.connect(host, headers, *wrappedConnection, response, kj::mv(settings))
      .attach(kj::mv(wrappedConnection), kj::mv(revokeTask));
}

RevocableWebSocketWorkerInterface::RevocableWebSocketWorkerInterface(
    WorkerInterface& worker, kj::Promise<void> revokeProm)
    : worker(worker),
      revokeProm(revokeProm.fork()) {}

kj::Promise<void> RevocableWebSocketWorkerInterface::prewarm(kj::StringPtr url) {
  return worker.prewarm(url);
}

kj::Promise<WorkerInterface::ScheduledResult> RevocableWebSocketWorkerInterface::runScheduled(
    kj::Date scheduledTime, kj::StringPtr cron) {
  return worker.runScheduled(scheduledTime, cron);
}

kj::Promise<WorkerInterface::AlarmResult> RevocableWebSocketWorkerInterface::runAlarm(
    kj::Date scheduledTime, uint32_t retryCount) {
  return worker.runAlarm(scheduledTime, retryCount);
}

kj::Promise<WorkerInterface::CustomEvent::Result> RevocableWebSocketWorkerInterface::customEvent(
    kj::Own<CustomEvent> event) {
  return worker.customEvent(kj::mv(event));
}

}  // namespace

kj::Own<WorkerInterface> newRevocableWebSocketWorkerInterface(
    kj::Own<WorkerInterface> worker, kj::Promise<void> revokeProm) {
  return kj::heap<RevocableWebSocketWorkerInterface>(*worker, kj::mv(revokeProm))
      .attach(kj::mv(worker));
}

// =======================================================================================

namespace {

class ErrorWorkerInterface final: public WorkerInterface {
 public:
  ErrorWorkerInterface(kj::Exception&& exception): exception(kj::mv(exception)) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    // ignore
    return kj::READY_NOW;
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    kj::throwFatalException(kj::mv(exception));
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    event->failed(exception);
    kj::throwFatalException(kj::mv(exception));
  }

 private:
  kj::Exception exception;
};

}  // namespace

kj::Own<WorkerInterface> WorkerInterface::fromException(kj::Exception&& e) {
  return kj::heap<ErrorWorkerInterface>(kj::mv(e));
}

// =======================================================================================

RpcWorkerInterface::RpcWorkerInterface(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    FrankenvalueHandler& frankenvalueHandler,
    rpc::EventDispatcher::Client dispatcher)
    : httpOverCapnpFactory(httpOverCapnpFactory),
      byteStreamFactory(byteStreamFactory),
      frankenvalueHandler(frankenvalueHandler),
      dispatcher(kj::mv(dispatcher)) {}

kj::Promise<void> RpcWorkerInterface::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    Response& response) {
  auto inner = httpOverCapnpFactory.capnpToKj(dispatcher.getHttpServiceRequest().send().getHttp());
  auto promise = inner->request(method, url, headers, requestBody, response);
  return promise.attach(kj::mv(inner));
}

kj::Promise<void> RpcWorkerInterface::connect(kj::StringPtr host,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    ConnectResponse& tunnel,
    kj::HttpConnectSettings settings) {
  auto inner = httpOverCapnpFactory.capnpToKj(dispatcher.getHttpServiceRequest().send().getHttp());
  auto promise = inner->connect(host, headers, connection, tunnel, kj::mv(settings));
  return promise.attach(kj::mv(inner));
}

kj::Promise<void> RpcWorkerInterface::prewarm(kj::StringPtr url) {
  auto req = dispatcher.prewarmRequest(capnp::MessageSize{url.size() / sizeof(capnp::word) + 4, 0});
  req.setUrl(url);
  return req.sendIgnoringResult();
}

kj::Promise<WorkerInterface::ScheduledResult> RpcWorkerInterface::runScheduled(
    kj::Date scheduledTime, kj::StringPtr cron) {
  auto req = dispatcher.runScheduledRequest();
  req.setScheduledTime((scheduledTime - kj::UNIX_EPOCH) / kj::SECONDS);
  req.setCron(cron);
  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    return WorkerInterface::ScheduledResult{
      .retry = respResult.getRetry(), .outcome = respResult.getOutcome()};
  });
}

kj::Promise<WorkerInterface::AlarmResult> RpcWorkerInterface::runAlarm(
    kj::Date scheduledTime, uint32_t retryCount) {
  auto req = dispatcher.runAlarmRequest();
  req.setScheduledTime((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  req.setRetryCount(retryCount);
  return req.send().then([](auto resp) {
    auto respResult = resp.getResult();
    kj::Maybe<kj::String> errorDescription;
    if (respResult.hasErrorDescription()) {
      errorDescription = kj::str(respResult.getErrorDescription());
    }
    return WorkerInterface::AlarmResult{.retry = respResult.getRetry(),
      .retryCountsAgainstLimit = respResult.getRetryCountsAgainstLimit(),
      .outcome = respResult.getOutcome(),
      .errorDescription = kj::mv(errorDescription)};
  });
}

kj::Promise<kj::Maybe<kj::Date>> RpcWorkerInterface::abandonAlarm(kj::Date scheduledTime) {
  auto req = dispatcher.abandonAlarmRequest();
  req.setScheduledTimeMs((scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS);
  auto response = co_await req.send();
  auto storedAlarmTimeMs = response.getStoredAlarmTimeMs();
  if (storedAlarmTimeMs != 0) {
    co_return kj::UNIX_EPOCH + storedAlarmTimeMs* kj::MILLISECONDS;
  }
  co_return kj::Maybe<kj::Date>(kj::none);
}

kj::Promise<WorkerInterface::CustomEvent::Result> RpcWorkerInterface::customEvent(
    kj::Own<CustomEvent> event) {
  return event->sendRpc(httpOverCapnpFactory, byteStreamFactory, frankenvalueHandler, dispatcher)
      .attach(kj::mv(event));
}

// ======================================================================================
WorkerInterface::AlarmFulfiller::AlarmFulfiller(
    kj::Own<kj::PromiseFulfiller<AlarmOutcome>> fulfiller)
    : maybeFulfiller(kj::mv(fulfiller)) {}

WorkerInterface::AlarmFulfiller::~AlarmFulfiller() noexcept(false) {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.reject(KJ_EXCEPTION(FAILED, "AlarmFulfiller destroyed without resolution"));
  }
}

void WorkerInterface::AlarmFulfiller::fulfill(const AlarmOutcome& result) {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.fulfill(kj::cp(result));
  }
}

void WorkerInterface::AlarmFulfiller::reject(const kj::Exception& e) {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.reject(e.clone());
  }
}

void WorkerInterface::AlarmFulfiller::cancel() {
  KJ_IF_SOME(fulfiller, getFulfiller()) {
    fulfiller.fulfill(AlarmOutcome{
      .retry = false,
      .outcome = EventOutcome::CANCELED,
    });
  }
}

kj::Maybe<kj::PromiseFulfiller<WorkerInterface::AlarmOutcome>&> WorkerInterface::AlarmFulfiller::
    getFulfiller() {
  KJ_IF_SOME(fulfiller, maybeFulfiller) {
    if (fulfiller.get()->isWaiting()) {
      return *fulfiller;
    }
  }

  return kj::none;
}

}  // namespace workerd
