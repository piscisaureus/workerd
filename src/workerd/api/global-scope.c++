// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "global-scope.h"

#include "simdutf.h"

#include <workerd/api/cache.h>
#include <workerd/api/crypto/crypto.h>
#include <workerd/api/events.h>
#include <workerd/api/eventsource.h>
#ifdef WORKERD_FUZZILLI
#include <workerd/api/fuzzilli.h>
#endif
#include <workerd/api/hibernatable-web-socket.h>
#include <workerd/api/http.h>
#include <workerd/api/restore.h>
#include <workerd/api/scheduled.h>
#include <workerd/api/sockets.h>
#include <workerd/api/system-streams.h>
#include <workerd/api/trace.h>
#include <workerd/api/tracing.h>
#include <workerd/api/util.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/access-info.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/async-context.h>
#include <workerd/jsg/guest-run.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/util.h>
#include <workerd/util/entropy.h>
#include <workerd/util/host-memory.h>
#include <workerd/util/sentry.h>
#include <workerd/util/stream-utils.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/uncaught-exception-source.h>
#include <workerd/util/use-perfetto-categories.h>

#include <v8-microtask-queue.h>

#ifdef __linux__
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#include <kj/encoding.h>

#include <cstring>

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

namespace workerd::api {

namespace {

enum class NeuterReason { SENT_RESPONSE, THREW_EXCEPTION, CLIENT_DISCONNECTED };

kj::Exception makeNeuterException(NeuterReason reason) {
  switch (reason) {
    case NeuterReason::SENT_RESPONSE:
      return JSG_KJ_EXCEPTION(
          FAILED, TypeError, "Can't read from request stream after response has been sent.");
    case NeuterReason::THREW_EXCEPTION:
      return JSG_KJ_EXCEPTION(
          FAILED, TypeError, "Can't read from request stream after responding with an exception.");
    case NeuterReason::CLIENT_DISCONNECTED:
      return JSG_KJ_EXCEPTION(
          DISCONNECTED, TypeError, "Can't read from request stream because client disconnected.");
  }
  KJ_UNREACHABLE;
}

}  // namespace

void ExecutionContext::waitUntil(kj::Promise<void> promise) {
  IoContext::current().addWaitUntil(kj::mv(promise));
}

void ExecutionContext::passThroughOnException() {
  IoContext::current().setFailOpen();
}

jsg::Promise<jsg::Value> ExecutionContext::restore(jsg::Lock& js,
    jsg::JsObject params,
    const jsg::TypeHandler<jsg::Ref<Fetcher>>& fetcherHandler,
    const jsg::TypeHandler<jsg::Ref<JsRpcStub>>& rpcStubHandler) {
  return restoreCurrentEntrypoint(js, params, fetcherHandler, rpcStubHandler);
}

jsg::Optional<jsg::Ref<CacheContext>> ExecutionContext::getCache(jsg::Lock& js) {
  // Hook for the embedding application to provide a CacheContext.
  // The default Worker::Api implementation returns kj::none.
  if (IoContext::hasCurrent()) {
    return Worker::Isolate::from(js).getApi().getCtxCacheProperty(js);
  }
  return kj::none;
}

jsg::Promise<CachePurgeResult> CacheContext::purge(jsg::Lock& js,
    CachePurgeOptions options,
    const jsg::TypeHandler<CachePurgeOptions>& optionsHandler,
    const jsg::TypeHandler<CachePurgeResult>& resultHandler,
    const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler) {
  JSG_FAIL_REQUIRE(Error, "Cache purge is not available in this context.");
}

jsg::Ref<Tracing> ExecutionContext::getTracing(jsg::Lock& js) {
  // A new Tracing handle is allocated on first access only - `JSG_LAZY_INSTANCE_PROPERTY`
  // uses V8's SetLazyDataProperty, which caches the getter result on the instance after the
  // first call. So `ctx.tracing === ctx.tracing` and only one allocation per
  // ExecutionContext. Tracing itself is stateless (no fields); the per-request state lives
  // on IoContext::getCurrentUserTraceSpan(), which enterSpan consults at call time.
  return js.alloc<Tracing>();
}

kj::StringPtr AccessContext::getAud() {
  return info->getAudience();
}

jsg::Promise<jsg::Value> AccessContext::getIdentity(jsg::Lock& js,
    const jsg::TypeHandler<jsg::Ref<JsRpcProperty>>& rpcPropHandler,
    const jsg::TypeHandler<jsg::Function<jsg::Value()>>& getIdentityFnHandler) {
  // Dispatch `getIdentity` JS-RPC on the Access binding worker's subrequest channel.
  // Per-request props are injected by the IoChannelFactory at dispatch time.
  // Resolves to `undefined` when no identity service channel is configured.
  auto& context = IoContext::current();
  auto span = context.makeTraceSpan("access_get_identity"_kjc);

  KJ_IF_SOME(channel, info->getIdentityServiceChannel()) {
    span.setTag("access.has_identity_service"_kjc, true);
    auto fetcher =
        js.alloc<Fetcher>(channel, Fetcher::RequiresHostAndProtocol::NO, true /* isInHouse */);

    auto rpcProp = JSG_REQUIRE_NONNULL(fetcher->getRpcMethodInternal(js, kj::str("getIdentity")),
        Error, "Access binding worker is missing the getIdentity method");

    auto getIdentityFn = JSG_REQUIRE_NONNULL(
        getIdentityFnHandler.tryUnwrap(js, rpcPropHandler.wrap(js, kj::mv(rpcProp))), Error,
        "Access binding worker getIdentity is not callable");

    // The RPC method returns a `JsRpcPromise` (custom thenable). Normalize it into a real promise
    // via a resolver so we're independent of the `unwrapCustomThenables` compat flag.
    auto paf = js.newPromiseAndResolver<jsg::Value>();
    paf.resolver.resolve(js, getIdentityFn(js));
    return context.attachSpans(js, kj::mv(paf.promise), kj::mv(span));
  }

  span.setTag("access.has_identity_service"_kjc, false);
  return js.resolvedPromise(jsg::Value(js.v8Isolate, v8::Undefined(js.v8Isolate)));
}

jsg::Optional<jsg::Ref<AccessContext>> ExecutionContext::getAccess(jsg::Lock& js) {
  // Pull the per-request AccessInfo (if any) off the current IncomingRequest.
  if (!IoContext::hasCurrent()) return kj::none;
  auto& ioctx = IoContext::current();
  KJ_IF_SOME(info, ioctx.getAccessInfo()) {
    return js.alloc<AccessContext>(ioctx.addObject(kj::addRef(info)));
  }
  return kj::none;
}

kj::String ExecutionContext::mapVirtualHost(
    jsg::Lock& js, jsg::Ref<Fetcher> fetcher, uint16_t port) {
  // Set up override, or return existing magic hostname if override already exists. Overrides
  // created using mapVirtualHost() should never use the legacy hyperdrive hostname, so the only way
  // for that hostname to be used is if an override was already registered earlier using
  // ExtendedFetcher.
  fetcher->registerOverride(js, IsHyperdrive::NO, port);
  return kj::str(KJ_ASSERT_NONNULL(fetcher->getHostInternal()), ":",
      KJ_ASSERT_NONNULL(fetcher->getPortInternal()));
}

void ExecutionContext::abort(jsg::Lock& js, jsg::Optional<jsg::Value> reason) {
  KJ_IF_SOME(r, reason) {
    IoContext::current().abort(js.exceptionToKj(kj::mv(r)));
  } else {
    auto e =
        JSG_KJ_EXCEPTION(FAILED, Error, "Worker execution was aborted due to call to ctx.abort().");
    IoContext::current().abort(kj::mv(e));
  }

  js.terminateExecutionNow();
}

namespace {
template <typename T>
jsg::LenientOptional<T> mapAddRef(jsg::Lock& js, jsg::LenientOptional<T>& function) {
  return function.map([&](T& a) { return a.addRef(js); });
}
}  // namespace

ExportedHandler ExportedHandler::clone(jsg::Lock& js) {
  return ExportedHandler{
    .fetch{mapAddRef(js, fetch)},
    .connect{mapAddRef(js, connect)},
    .tail{mapAddRef(js, tail)},
    .trace{mapAddRef(js, trace)},
    .tailStream{mapAddRef(js, tailStream)},
    .scheduled{mapAddRef(js, scheduled)},
    .alarm{mapAddRef(js, alarm)},
    .test{mapAddRef(js, test)},
    .webSocketMessage{mapAddRef(js, webSocketMessage)},
    .webSocketClose{mapAddRef(js, webSocketClose)},
    .webSocketError{mapAddRef(js, webSocketError)},
    .self{js.v8Isolate, self.getHandle(js.v8Isolate)},
    .env{env.addRef(js)},
    .ctx{getCtx()},
    .missingSuperclass = missingSuperclass,
  };
}

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope()
    : unhandledRejections([this](jsg::Lock& js,
                              v8::PromiseRejectEvent event,
                              jsg::V8Ref<v8::Promise> promise,
                              jsg::Value value) {
        // If async context tracking is enabled, then we need to ensure that we enter the frame
        // associated with the promise before we invoke the unhandled rejection callback handling.
        auto ev = js.alloc<PromiseRejectionEvent>(event, kj::mv(promise), kj::mv(value));
        dispatchEventImpl(js, kj::mv(ev));
      }) {}

void ServiceWorkerGlobalScope::clear() {
  removeAllHandlers();
  unhandledRejections.clear();
}

kj::Promise<void> ServiceWorkerGlobalScope::connect(kj::String host,
    kj::Maybe<kj::String> clientAddress,
    const kj::HttpHeaders& headers,
    kj::AsyncIoStream& connection,
    kj::HttpService::ConnectResponse& response,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  ExportedHandler& eh = JSG_REQUIRE_NONNULL(exportedHandler, Error,
      "Connect ingress is not currently supported with Service Workers syntax.");

  KJ_IF_SOME(handler, eh.connect) {
    // Has a connect handler!
    response.accept(200, "OK", headers);

    // Using neuterable stream to manage lifetime of stream promises. We MUST neuter this when the
    // promise returned to the caller resolves because `connection` is owned by the caller and will
    // be destroyed when that happens, while the JS Socket can outlive us via ctx.waitUntil().
    auto ownConnection = newNeuterableIoStream(connection);
    auto deferredNeuter = kj::defer([ref = ownConnection.addRef()]() mutable {
      ref->neuter(makeNeuterException(NeuterReason::CLIENT_DISCONNECTED));
    });
    KJ_ON_SCOPE_FAILURE(ownConnection->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION)));

    auto& ioContext = IoContext::current();
    jsg::Lock& js = lock;

    // TLS support is not implemented so far. Note that setupSocket() expects the domain parameter
    // to be set to the expected host name using startTLS, so that it can be provided to the TLS
    // callback, so we'd need to change that or figure out a way to get the host domain.
    auto nullTlsStarter = kj::heap<kj::TlsStarterCallback>();
    // We set isDefaultFetchPort to false here – sockets.c++ sets it for ports 443 and 8080 to
    // provide a more descriptive error message for HTTP, but this is not relevant on the TCP server
    // side.
    // The handler is the server side of this connection: the peer half-closing means it has
    // finished sending, not that the reply is over, so the write side stays open until the handler
    // closes it or returns.
    jsg::Ref<Socket> jsSocket =
        setupSocket(js, ownConnection.addRef().toOwn(), kj::mv(clientAddress), kj::mv(host),
            SocketOptions{.allowHalfOpen = true}, kj::mv(nullTlsStarter), SecureTransportKind::OFF,
            SocketProtocol::TCP, kj::none, false, kj::none);
    // handleProxyStatus() is required to indicate that the socket was opened properly. Since the
    // connection is already open at this point, exception handling is not required.
    jsSocket->handleProxyStatus(js, kj::Promise<kj::Maybe<kj::Exception>>(kj::none));

    kj::Maybe<SpanBuilder> span = ioContext.makeTraceSpan("connect_handler"_kjc);
    auto promise = handler(js, kj::mv(jsSocket), eh.env.addRef(js), eh.getCtx());
    return ioContext.awaitJs(js, kj::mv(promise)).attach(kj::mv(span), kj::mv(deferredNeuter));
  }
  lock.logWarningOnce("Received a connect event but we lack a handler. "
                      "Did you remember to export a connect() function?");
  JSG_FAIL_REQUIRE(Error, "Handler does not export a connect() function.");
}

kj::Promise<void> ServiceWorkerGlobalScope::connectUdp(kj::String host,
    DatagramChannel& channel,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  ExportedHandler& eh = JSG_REQUIRE_NONNULL(exportedHandler, Error,
      "Connect ingress is not currently supported with Service Workers syntax.");
  KJ_REQUIRE(FeatureFlags::get(lock).getWorkerdExperimental(),
      "UDP ingress requires the experimental flag.");

  KJ_IF_SOME(handler, eh.connect) {
    // Using a neuterable wrapper to manage lifetime, exactly like connect()'s NeuterableIoStream:
    // we MUST neuter this when the promise returned to the caller resolves, since `channel` is
    // only guaranteed valid for that long, while the JS Socket can outlive us via ctx.waitUntil().
    auto ownChannel = newNeuterableDatagramChannel(channel);
    auto deferredNeuter = kj::defer([ref = ownChannel.addRef()]() mutable {
      ref->neuter(makeNeuterException(NeuterReason::CLIENT_DISCONNECTED));
    });
    KJ_ON_SCOPE_FAILURE(ownChannel->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION)));

    auto& ioContext = IoContext::current();
    jsg::Lock& js = lock;

    jsg::Ref<Socket> jsSocket = setupDatagramSocket(
        js, ownChannel.addRef().toOwn(), kj::none /* remoteAddress */, kj::mv(host));

    kj::Maybe<SpanBuilder> span = ioContext.makeTraceSpan("connect_handler"_kjc);
    auto promise = handler(js, kj::mv(jsSocket), eh.env.addRef(js), eh.getCtx());
    return ioContext.awaitJs(js, kj::mv(promise)).attach(kj::mv(span), kj::mv(deferredNeuter));
  }
  lock.logWarningOnce("Received a connect event but we lack a handler. "
                      "Did you remember to export a connect() function?");
  JSG_FAIL_REQUIRE(Error, "Handler does not export a connect() function.");
}

namespace {

#ifdef __linux__
// Test-only stand-in for the CPU limiter's signal (see maybeForceGuestFaultForTest below).
constexpr long WASM_SIGNAL_PROBE_MS = 20;
const TrackedWasmInstanceList* wasmSignalProbeList = nullptr;
timer_t wasmSignalProbeTimer;
bool wasmSignalProbeTimerArmed = false;

void wasmSignalProbeHandler(int) {
  if (wasmSignalProbeList != nullptr) wasmSignalProbeList->writeShutdownSignal();
}

// Host-side, on the event loop: installs the SIGXCPU handler once and arms the thread's timer.
void armWasmSignalTimerForTest(const TrackedWasmInstanceList& list) {
  static const bool installed = []() {
    struct sigaction sa {};
    sa.sa_handler = &wasmSignalProbeHandler;
    sa.sa_flags = SA_RESTART;
    return sigaction(SIGXCPU, &sa, nullptr) == 0;
  }();
  KJ_REQUIRE(installed);
  wasmSignalProbeList = &list;
  if (wasmSignalProbeTimerArmed) timer_delete(wasmSignalProbeTimer);
  struct sigevent sev {};
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGXCPU;
  sev._sigev_un._tid = static_cast<int>(syscall(SYS_gettid));
  // The clock is the one the turn's CPU is charged to: this thread's, unless the guest kernel
  // runs the turn's instructions on a vCPU runner thread (GK_RUNNER), whose clock gk names
  // then. The signal still comes to this thread, where the handler's write belongs.
  clockid_t clock = CLOCK_THREAD_CPUTIME_ID;
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (gk_initialized()) gk_turn_cpu_clockid(&clock);
#endif
  KJ_SYSCALL(timer_create(clock, &sev, &wasmSignalProbeTimer));
  wasmSignalProbeTimerArmed = true;
  struct itimerspec its {};
  its.it_value.tv_nsec = WASM_SIGNAL_PROBE_MS * 1000000;
  KJ_SYSCALL(timer_settime(wasmSignalProbeTimer, 0, &its, nullptr));
  KJ_LOG(WARNING,
      "WORKERD_GK_TEST_PROBE: armed a CPU-time timer on the clock the turn is charged to, whose "
      "signal handler on this thread writes the Wasm shutdown signal",
      WASM_SIGNAL_PROBE_MS, clock);
}

// The buffer a write primitive would aim the old descriptor at, and the descriptors as they
// were, for the restore.
byte chosenWasmBuffer[64] = {};
struct SavedWasmDescriptor {
  TrackedWasmInstance* entry;
  byte memory[sizeof(kj::Array<byte>)];
  kj::Maybe<uint32_t> signalByteOffset;
};
kj::Vector<SavedWasmDescriptor> savedWasmDescriptors;

// In-turn: rewrites each entry's `memory` (a kj::Array: pointer, size, disposer) to name the
// chosen buffer, and its signal offset to 0, as a write primitive with the old layout in mind
// would.
void corruptWasmDescriptorsForTest(const TrackedWasmInstanceList& list, kj::StringPtr url) {
  auto& entries = const_cast<SignalSafeList<TrackedWasmInstance>&>(list.signals());
  entries.iterate([&](TrackedWasmInstance& entry) {
    SavedWasmDescriptor saved{.entry = &entry, .signalByteOffset = entry.signalByteOffset};
    memcpy(saved.memory, &entry.memory, sizeof(saved.memory));
    savedWasmDescriptors.add(kj::mv(saved));
    uintptr_t words[2] = {reinterpret_cast<uintptr_t>(chosenWasmBuffer), sizeof(chosenWasmBuffer)};
    uintptr_t oldWords[2];
    memcpy(oldWords, &entry.memory, sizeof(oldWords));
    memcpy(&entry.memory, words, sizeof(words));
    entry.signalByteOffset = static_cast<uint32_t>(0);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: overwrote a Wasm signal descriptor in the heap from inside the "
        "JS turn",
        url, kj::str("0x", kj::hex(oldWords[0])), oldWords[1], kj::str("0x", kj::hex(words[0])),
        words[1]);
  });
}

// In-turn: reports what the chosen buffer holds and puts the descriptors back.
void checkWasmDescriptorsForTest(kj::StringPtr url) {
  uint32_t word = 0;
  memcpy(&word, chosenWasmBuffer, sizeof(word));
  KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the chosen buffer's first word after the signal", url,
      word, kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(chosenWasmBuffer))));
  for (auto& saved: savedWasmDescriptors) {
    memcpy(&saved.entry->memory, saved.memory, sizeof(saved.memory));
    saved.entry->signalByteOffset = saved.signalByteOffset;
  }
  KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: restored the Wasm signal descriptors",
      savedWasmDescriptors.size());
  savedWasmDescriptors.clear();
}
#endif  // __linux__

// Test-only hooks for the experimental guest kernel's fault path. With the guest kernel enabled
// and WORKERD_GK_TEST_FAULT set, a request whose path is /__gk_fault dereferences a null pointer
// from inside the JS turn. At guest ring 3 that is a page fault the guest cannot service, so
// gk_run_here_user() returns GK_EFAULT to IoContext::runImpl (see runJsTurn there), which must
// fail the request, condemn the isolate and keep the process serving. Both gates are required: a
// production process (no WORKERD_GK_TEST_FAULT) never takes this branch, and neither does one
// running without the guest kernel, where the dereference would be a host-side SIGSEGV.
//
// With WORKERD_GK_TEST_PROBE set, a request whose path is /__gk_probe_host instead reads one
// byte of the channel token key, and one whose path is /__gk_probe_host_entropy one byte of the
// host entropy buffer, from inside the JS turn; both are host-memory tenants registered through
// hostMemory::setProbeTargetForTest(). Host memory is mapped into no guest root, so at ring 3
// that is the same kind of fault, at the byte's own address; this is how the guest kernel's
// host-memory-test.sh shows both are out of a turn's reach. The same two gates apply.
//
// With the same variable set, a request whose path is /__gk_poison_entropy overwrites, from
// inside the JS turn, the whole of this thread's getEntropy() buffer with one byte value and
// marks all of it unconsumed, which is what a turn that wants to choose the bytes a later draw
// on the thread returns would do. On the event loop afterwards, outside the guest, the host
// mints an RPC channel token (see hostMemory::setHostUseForTest) and logs its nonce, then draws
// from getEntropy() and logs that too: the latter must be the pattern, showing the turn's write
// reached the host's next draw on that path, and the nonce must not be, showing the token's
// nonce comes from somewhere else. The buffer is then discarded so later draws are fresh.
//
// With the same variable set, a request whose path is /__gk_corrupt_handle overwrites, from
// inside the JS turn, the handle through which the tenant's owner reaches it (registered through
// hostMemory::setHandleProbeForTest()) with the address of a buffer this code chose, which is
// what a write primitive would leave in place of a pointer; the owner is in the ordinary heap,
// so the write lands and the turn completes. The host's use of the tenant then runs on the
// event loop, outside the guest, as the real uses do: it must be refused rather than reach the
// buffer. The handle is restored afterwards and the use run again.
//
// With the same variable set, three request paths exercise the Wasm shutdown signal, whose
// descriptors the guest kernel keeps in its own table (see TrackedWasmInstanceList). The signal
// has no source in workerd itself, so /__gk_wasm_arm stands in for the CPU limiter: on the event
// loop after the turn, it arms a one-shot CPU-time timer of WASM_SIGNAL_PROBE_MS on this thread,
// whose SIGXCPU handler calls writeShutdownSignal() on the isolate's list, as a limiter would
// from its own signal handler; a following request that burns CPU reading a registered module's
// field sees the value land. /__gk_wasm_corrupt first overwrites, from inside the turn, the heap
// entry of every registered module -- the pointer and size of its linear memory, and the signal
// offset -- with a buffer this code chose, which is what a write primitive aiming the old
// descriptor would do, then arms the timer the same way; the signal must still reach the
// module's real memory and the chosen buffer must stay untouched, which /__gk_wasm_check reads
// out and logs before restoring the entries.
// With the same variable set, a request whose path is /__gk_tls_restore overwrites, from inside
// the turn, the heap slot the isolate lock restores Worker::Api's thread-local from, with a
// sentinel; the host puts the real value back from a stack frame when the lock is released (see
// ~Worker::Lock and GuestTurnThreadLocals in io/), so an event-loop callback observes Worker::Api
// not holding the sentinel. This is how tls-test.sh shows the guest-writable thread-locals the
// host reads after a turn do not carry a turn's value past it.
//
// With the same variable set, four request paths exercise the isolate's lock slot, the V8 data
// slot jsg::Lock::from reads, which is in the C++ isolate object in the shared heap and so
// writable from any isolate's turn (lock-slot-test.sh). /__gk_lock_slot overwrites this isolate's
// own slot from inside the turn with a sentinel (0x1), as the turn's last act in the guest
// (jsg::setGuestTurnTailHookForTest), and arranges for the host to report, once
// the turn is over but while it still holds the lock (Worker::Lock::runAtNextReleaseForTest),
// what jsg::Lock::from returns against the lock it holds, and for an event-loop callback to
// report the slot once the lock is gone. /__gk_lock_announce records this isolate for another
// worker's turn; /__gk_lock_poison_other, on that other worker, overwrites the announced
// isolate's slot from inside its own turn and has an event-loop callback report the slot, to
// show the write landed; /__gk_lock_check on the announced worker then reports the same two
// things as /__gk_lock_slot without writing anything. The host must find its own lock in the
// slot in every case and must not act on the sentinel, including by refusing the lock as
// recursive.
void maybeForceGuestFaultForTest(kj::StringPtr url, Worker::Lock& lock) {
  static const bool faultEnabled =
      jsg::isGuestKernelEnabled() && getenv("WORKERD_GK_TEST_FAULT") != nullptr;
  if (faultEnabled && url.endsWith("/__gk_fault")) {
    KJ_LOG(WARNING, "WORKERD_GK_TEST_FAULT: forcing a guest fault for this request", url);
    *static_cast<volatile int*>(nullptr) = 0;
  }
  static const bool probeEnabled =
      jsg::isGuestKernelEnabled() && getenv("WORKERD_GK_TEST_PROBE") != nullptr;
  auto probeHost = [&](kj::StringPtr name) {
    KJ_IF_SOME(target, hostMemory::probeTargetForTest(name)) {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: reading host memory from inside the JS turn", url,
          name, kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(target))));
      volatile byte probed = *static_cast<const volatile byte*>(target);
      (void)probed;
    }
  };
  if (probeEnabled && url.endsWith("/__gk_probe_host")) {
    probeHost("token-key");
  }
  if (probeEnabled && url.endsWith("/__gk_probe_host_entropy")) {
    probeHost("host-entropy");
  }
  if (probeEnabled && url.endsWith("/__gk_probe_actor_key")) {
    probeHost("actor-id-key");
  }
  if (probeEnabled && url.endsWith("/__gk_poison_entropy")) {
    constexpr byte PATTERN = 0x5a;
    poisonEntropyBufferForTest(PATTERN);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: overwrote this thread's entropy buffer from inside the JS turn",
        url, kj::hex(PATTERN));
    IoContext::current().addTask(kj::evalLater([]() {
      KJ_IF_SOME(mint, hostMemory::hostUseForTest("mint-token")) {
        mint();
      }
      byte drawn[12] = {};
      getEntropy(drawn);
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: getEntropy() on the host after the turn returned",
          kj::encodeHex(kj::arrayPtr(drawn)));
      discardEntropyBufferForTest();
    }));
  }
#ifdef __linux__
  if (probeEnabled && url.endsWith("/__gk_wasm_arm")) {
    auto& list = lock.getWorker().getIsolate().getLimitEnforcer().getTrackedWasmInstances();
    IoContext::current().addTask(kj::evalLater([&list]() { armWasmSignalTimerForTest(list); }));
  }
  if (probeEnabled && url.endsWith("/__gk_wasm_corrupt")) {
    auto& list = lock.getWorker().getIsolate().getLimitEnforcer().getTrackedWasmInstances();
    corruptWasmDescriptorsForTest(list, url);
    IoContext::current().addTask(kj::evalLater([&list]() { armWasmSignalTimerForTest(list); }));
  }
  if (probeEnabled && url.endsWith("/__gk_wasm_check")) {
    checkWasmDescriptorsForTest(url);
  }
#endif
  if (probeEnabled && url.endsWith("/__gk_tls_restore")) {
    // A JS turn's guest-writable thread-locals are put back by the host after the turn (see
    // GuestTurnThreadLocals and ~Worker::Lock), so a value the turn leaves in them does not reach
    // the host. This exercises the Api case, whose restored value must come from a host stack
    // frame and not from the heap slot the lock otherwise restores from. From inside the turn,
    // overwrite that heap slot (Isolate::Impl::Lock::oldCurrentApi, which ~Worker::Lock reads) with
    // a sentinel, as a write primitive would; ~Worker::Lock must still leave Worker::Api's
    // thread-local holding the value it had before the lock, which an event-loop callback checks
    // once the lock is gone -- it must not be the sentinel.
    auto* apiSlot = static_cast<const Worker::Api**>(lock.savedApiStorageForTest());
    const auto* sentinel = reinterpret_cast<const Worker::Api*>(0x1);
    const auto* heapWas = *apiSlot;
    *apiSlot = sentinel;
    KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: overwrote the Api restore slot from inside the JS turn",
        url, kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(apiSlot))),
        kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(heapWas))));
    IoContext::current().addTask(kj::evalLater([]() {
      // On the event loop, no isolate lock held: Worker::Api's thread-local holds whatever the
      // request's lock left. If the stack restore held, it is not the sentinel.
      const Worker::Api* cur = nullptr;
      KJ_IF_SOME(api, Worker::Api::tryCurrent()) {
        cur = &api;
      }
      kj::StringPtr verdict =
          cur == reinterpret_cast<const Worker::Api*>(0x1) ? "leaked"_kj : "restored"_kj;
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: Worker::Api after the turn",
          kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(cur))), verdict);
    }));
  }
  // The isolate's lock slot (see the comment above and jsg::Lock::from).
  static v8::Isolate* announcedIsolate = nullptr;
  auto addr = [](const void* p) { return kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(p))); };
  static void* const LOCK_SENTINEL = reinterpret_cast<void*>(0x1);
  if (probeEnabled && url.endsWith("/__gk_lock_announce")) {
    jsg::Lock& js = lock;
    announcedIsolate = js.v8Isolate;
    KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: announced this isolate for another worker's turn", url,
        addr(js.v8Isolate), addr(js.v8Isolate->GetData(jsg::SET_DATA_LOCK)));
  }
  if (probeEnabled && url.endsWith("/__gk_lock_poison_other")) {
    jsg::Lock& js = lock;
    v8::Isolate* other = announcedIsolate;
    KJ_REQUIRE(other != nullptr, "no isolate announced");
    KJ_REQUIRE(other != js.v8Isolate, "the announced isolate is this one");
    void* was = other->GetData(jsg::SET_DATA_LOCK);
    other->SetData(jsg::SET_DATA_LOCK, LOCK_SENTINEL);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: overwrote another isolate's lock slot from inside the JS turn", url,
        addr(other), addr(was), addr(LOCK_SENTINEL));
    IoContext::current().addTask(kj::evalLater([other, addr]() {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the other isolate's lock slot on the event loop",
          addr(other), addr(other->GetData(jsg::SET_DATA_LOCK)));
    }));
  }
  if (probeEnabled && (url.endsWith("/__gk_lock_slot") || url.endsWith("/__gk_lock_check"))) {
    jsg::Lock& js = lock;
    if (url.endsWith("/__gk_lock_slot")) {
      // As the turn's last act, in the guest: the rest of this turn calls jsg::Lock::from itself,
      // at ring 3, and a sentinel there would fault the turn before the host got to read it.
      v8::Isolate* isolate = js.v8Isolate;
      jsg::Lock* held = &js;
      jsg::setGuestTurnTailHookForTest([isolate, held, addr, url = kj::str(url)]() {
        isolate->SetData(jsg::SET_DATA_LOCK, LOCK_SENTINEL);
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: overwrote this isolate's lock slot from inside the JS turn",
            url, addr(isolate), addr(held), addr(LOCK_SENTINEL));
      });
    }
    Worker::Lock::runAtNextReleaseForTest([addr](Worker::Lock& released) {
      // Host-side, after the turn, with the isolate still locked: what jsg::Lock::from hands host
      // code now, against the lock this frame holds.
      jsg::Lock& held = released;
      jsg::Lock* from = &jsg::Lock::from(held.v8Isolate);
      kj::StringPtr verdict = from == &held ? "restored"_kj
          : from == LOCK_SENTINEL           ? "leaked"_kj
                                            : "other"_kj;
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: jsg::Lock::from on the host after the turn",
          addr(from), addr(&held), verdict);
    });
    v8::Isolate* isolate = js.v8Isolate;
    IoContext::current().addTask(kj::evalLater([isolate, addr]() {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the lock slot on the event loop after the lock",
          addr(isolate), addr(isolate->GetData(jsg::SET_DATA_LOCK)));
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_vfs_rewrite")) {
    // SQLite's native VFS keeps the directory descriptor for its path syscalls in a thread-local
    // a turn can write (currentVfsRoot in util/sqlite.c++); the guest kernel's turn directory is
    // what keeps a rewritten descriptor from redirecting the files. The rewrite is armed on the
    // event loop (the directory it names must be opened on the host, which a confined turn
    // cannot do) and applies to every native VFS call after that; /__gk_vfs_restore lifts it.
    IoContext::current().addTask(kj::evalLater([]() {
      KJ_IF_SOME(rewrite, hostMemory::hostUseForTest("vfs-root-rewrite")) {
        rewrite();
      }
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_vfs_restore")) {
    IoContext::current().addTask(kj::evalLater([]() {
      KJ_IF_SOME(restore, hostMemory::hostUseForTest("vfs-root-restore")) {
        restore();
      }
    }));
  }
  // The IoOwn handle (see the comment above and io/io-own.h): the probe object is a T an IoOwn
  // owns, held in a heap holder that stands in for the JS wrapper's C++ object a turn can write.
  struct IoOwnProbe {
    IoOwnProbe(uint64_t magic): magic(magic) {}
    ~IoOwnProbe() noexcept(false) {
      KJ_LOG(WARNING,
          "WORKERD_GK_TEST_PROBE: an IoOwn probe object was disposed of through its "
          "own disposer",
          kj::str("0x", kj::hex(magic)));
    }
    uint64_t magic;
  };
  struct IoOwnProbeHolder {
    IoOwn<IoOwnProbe> own;
  };
  constexpr uint64_t IOOWN_GOOD_MAGIC = 0x600d600d600d600d;
  constexpr uint64_t IOOWN_CHOSEN_MAGIC = 0xbadbadbadbadbad0;
  constexpr uint64_t IOOWN_OTHER_MAGIC = 0x0e7e70e7e70e7e70;
  constexpr uint64_t IOOWN_NODE_MAGIC = 0x0de0de0de0de0de0;
  // The IoOwn's first word is the context's handle (its DeleteQueue) and its second the object's
  // handle, before the disposal record; a write primitive aiming at either writes exactly there.
  static_assert(
      sizeof(IoOwn<IoOwnProbe>) == sizeof(DeleteQueue) + sizeof(uint64_t) + sizeof(OwnedDisposal));
  constexpr size_t IOOWN_CONTEXT_OFFSET = 0;
  constexpr size_t IOOWN_HANDLE_OFFSET = sizeof(DeleteQueue);
  auto ioOwnWord = [](IoOwnProbeHolder& holder, size_t offset) {
    uint64_t word;
    memcpy(&word, reinterpret_cast<byte*>(&holder.own) + offset, sizeof(word));
    return word;
  };
  auto setIoOwnWord = [](IoOwnProbeHolder& holder, size_t offset, uint64_t word) {
    memcpy(reinterpret_cast<byte*>(&holder.own) + offset, &word, sizeof(word));
  };
  auto ioOwnHandle = [ioOwnWord](IoOwnProbeHolder& holder) {
    return ioOwnWord(holder, IOOWN_HANDLE_OFFSET);
  };
  auto setIoOwnHandle = [setIoOwnWord](IoOwnProbeHolder& holder, uint64_t handle) {
    setIoOwnWord(holder, IOOWN_HANDLE_OFFSET, handle);
  };
  auto ioOwnContext = [ioOwnWord](IoOwnProbeHolder& holder) {
    return ioOwnWord(holder, IOOWN_CONTEXT_OFFSET);
  };
  auto setIoOwnContext = [setIoOwnWord](IoOwnProbeHolder& holder, uint64_t context) {
    setIoOwnWord(holder, IOOWN_CONTEXT_OFFSET, context);
  };
  // A later turn of the same request uses the IoOwn, as an API call on the wrapper would, and
  // reports what it reached: the magic it read, or the refusal.
  auto useIoOwnLater = [addr](IoContext& context, kj::StringPtr what, IoOwnProbeHolder& holder) {
    return context.run([what, &holder, addr](Worker::Lock&) {
      kj::String verdict;
      KJ_IF_SOME(e, kj::runCatchingExceptions([&]() {
        uint64_t magic = holder.own->magic;
        verdict = kj::str("read magic 0x", kj::hex(magic), " at ", addr(holder.own.operator->()));
      })) {
        verdict = kj::str("refused: ", e.getDescription());
      }
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: IoOwn use after the overwrite", what, verdict);
    });
  };
  static uint64_t announcedIoOwnHandle = 0;
  static uint64_t announcedIoOwnContext = 0;
  if (probeEnabled && url.endsWith("/__gk_ioown_corrupt")) {
    auto& context = IoContext::current();
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_GOOD_MAGIC))});
    IoOwnProbeHolder& h = *holder;
    uint64_t handle = ioOwnHandle(h);
    // The buffer the turn chose: an object as the one the handle names, carrying a magic of its
    // own, so a host that dereferences the address reads it.
    static auto* chosen = new IoOwnProbe(IOOWN_CHOSEN_MAGIC);
    KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: created an IoOwn inside the JS turn", url,
        kj::str("0x", kj::hex(handle)), addr(h.own.operator->()));
    // A later turn first uses the IoOwn as it is (the control: the handle committed at the end
    // of this turn resolves to the object) and then overwrites the handle, as the turn's own
    // act; the use after that runs in a further turn. Then the holder is destroyed on the event
    // loop, outside any turn, which hands the handle to the delete queue, and one more turn
    // makes the host drain that queue -- host-side, before the turn enters the guest -- which
    // must refuse the handle rather than unlink through the chosen address.
    context.addWaitUntil(
        useIoOwnLater(context, "intact", h)
            .then([&context, &h, url = kj::str(url), addr, handle, setIoOwnHandle]() mutable {
      return context.run([&h, url = kj::mv(url), addr, handle, setIoOwnHandle](Worker::Lock&) {
        setIoOwnHandle(h, reinterpret_cast<uintptr_t>(chosen));
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: overwrote the IoOwn's handle from inside the JS turn", url,
            kj::str("0x", kj::hex(handle)), addr(chosen), addr(chosen));
      });
    })
            .then(
                [&context, &h, useIoOwnLater]() { return useIoOwnLater(context, "rewritten", h); })
            .then([holder = kj::mv(holder)]() mutable {
      return kj::evalLater([holder = kj::mv(holder)]() mutable {
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: destroying the IoOwn's holder on the event loop");
        holder = nullptr;
      });
    }).then([&context]() {
      // On the event loop, with no current context: the context is alive for as long as this
      // waitUntil runs, and run() makes it current again for the turn.
      return context.run([](Worker::Lock&) {
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the turn after the holder's destruction ran");
      });
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_ioown_announce")) {
    // This isolate's IoOwn, for another isolate's turn to name. The object lives in this
    // request's owned list and is retired with the request's context, which the worker's JS
    // keeps open for a while (see ioown-test.capnp); the holder outlives it.
    auto& context = IoContext::current();
    static kj::Maybe<kj::Own<IoOwnProbeHolder>> announced;
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_OTHER_MAGIC))});
    announcedIoOwnHandle = ioOwnHandle(*holder);
    announcedIoOwnContext = ioOwnContext(*holder);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: announced this isolate's IoOwn for another worker's turn", url,
        kj::str("0x", kj::hex(announcedIoOwnHandle)), addr(holder->own.operator->()),
        kj::str("0x", kj::hex(announcedIoOwnContext)));
    IoOwnProbeHolder& announcedHolder = *holder;
    announced = kj::mv(holder);
    context.addWaitUntil(useIoOwnLater(context, "announced-own", announcedHolder));
  }
  if (probeEnabled && url.endsWith("/__gk_ioown_steal")) {
    // From inside this isolate's turn, rewrite an IoOwn of its own to the other isolate's live
    // handle. The entry carries the page-table root it was registered under, which the ring-0
    // lookup compares against the root this turn runs under, so the use is refused: the handle
    // is the other isolate's. The host-side resolve logged after it, which makes no such
    // comparison, reaches the object through the same handle, so the refusal is about who
    // asked and not about the entry having gone.
    KJ_REQUIRE(announcedIoOwnHandle != 0, "no IoOwn announced");
    auto& context = IoContext::current();
    static kj::Maybe<kj::Own<IoOwnProbeHolder>> thief;
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_GOOD_MAGIC))});
    IoOwnProbeHolder& h = *holder;
    uint64_t handle = ioOwnHandle(h);
    setIoOwnHandle(h, announcedIoOwnHandle);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: overwrote the IoOwn's handle from inside the JS turn with another isolate's",
        url, kj::str("0x", kj::hex(handle)), kj::str("0x", kj::hex(announcedIoOwnHandle)));
    // The thief is never destroyed, so the stolen handle is never handed to a delete queue; its
    // own entry is retired with its context. The use runs now, and again when asked for later.
    thief = kj::mv(holder);
    context.addWaitUntil(useIoOwnLater(context, "cross-isolate", h).then([addr]() {
      return kj::evalLater([addr]() {
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the stolen handle, resolved host-side",
            kj::str("0x", kj::hex(announcedIoOwnHandle)),
            addr(hostMemory::resolve(announcedIoOwnHandle)));
      });
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_ioown_steal_again")) {
    // The same stolen handle, used after the other isolate's request, and so its context, has
    // ended: the entry is retired with the context's owned list.
    static kj::Maybe<kj::Own<IoOwnProbeHolder>> thief2;
    auto& context = IoContext::current();
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_GOOD_MAGIC))});
    IoOwnProbeHolder& h = *holder;
    setIoOwnHandle(h, announcedIoOwnHandle);
    thief2 = kj::mv(holder);
    context.addWaitUntil(useIoOwnLater(context, "cross-isolate-after-release", h));
  }
  // The owned node (see OwnedObjectList in io/io-own.h): the record the host keeps about the
  // object behind an IoOwn's handle, in host memory. The turn creates an IoOwn; after the turn
  // the host, on the event loop, finds the node behind its handle and announces its address; a
  // later turn rewrites the node's link and disposer to a slot and a disposer the turn chose,
  // as a write primitive would. Under the guest kernel the write faults at the node, the isolate
  // is condemned and the request's context is torn down host-side, where the walk disposes of
  // the object through the disposer the node recorded. Where the write lands (no guest kernel,
  // or the WORKERD_GK_NEG_IOOWN_NODE build), the holder is destroyed on the event loop and the
  // next turn's drain follows the rewritten node: it writes the chosen slot and calls the chosen
  // disposer, which logs that it was reached.
  struct ChosenDisposer final: public kj::Disposer {
    void disposeImpl(void* pointer) const override {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the chosen disposer was called by the host",
          kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(pointer))));
    }
  };
  static const ChosenDisposer chosenDisposer;
  static void* chosenSlot = nullptr;
  if (probeEnabled && url.endsWith("/__gk_ioown_node_corrupt")) {
    auto& context = IoContext::current();
    static kj::Maybe<kj::Own<IoOwnProbeHolder>> nodeHolder;
    // The node's fields, found host-side and kept for the turn: the lookup itself reads host
    // state a turn cannot.
    static OwnedObjectList::NodeForTest probedNode = {nullptr, nullptr, nullptr};
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_NODE_MAGIC))});
    uint64_t handle = ioOwnHandle(*holder);
    KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: created an IoOwn for the node probe inside the JS turn",
        url, kj::str("0x", kj::hex(handle)), addr(holder->own.operator->()));
    nodeHolder = kj::mv(holder);
    context.addWaitUntil(kj::evalLater([&context, handle, addr, url = kj::str(url)]() mutable {
      // On the event loop, after the turn: the node exists in host memory now.
      probedNode = OwnedObjectList::nodeForTest(handle);
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the owned node behind the IoOwn, found host-side",
          kj::str("0x", kj::hex(handle)), addr(probedNode.node), addr(probedNode.prev),
          addr(probedNode.disposer));
      return context
          .run([addr, url = kj::mv(url)](Worker::Lock&) {
        auto node = probedNode;
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: rewriting the owned node from inside the JS turn",
            url, addr(node.node), addr(&chosenSlot), addr(&chosenDisposer));
        // The write: under the guest kernel this faults at the node and nothing below runs.
        *node.prev = &chosenSlot;
        *node.disposer = &chosenDisposer;
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: the owned node was rewritten from inside the JS turn; the "
            "write landed",
            addr(node.node));
      })
          .then([]() {
        return kj::evalLater([]() {
          KJ_LOG(WARNING,
              "WORKERD_GK_TEST_PROBE: destroying the node probe's holder on the event loop");
          nodeHolder = kj::none;
        });
      }).then([&context, addr]() {
        return context.run([addr](Worker::Lock&) {
          KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the turn after the node holder's destruction ran",
              addr(chosenSlot));
        });
      });
    }));
  }
  // The delete queue (see DeleteQueue in io/io-own.h): a holder names its context by the
  // context's handle, and the queue is in the context's record in host memory.
  //
  // /__gk_queue_corrupt aims at the record. The turn creates an IoOwn; after the turn the host
  // finds the record behind the IoOwn's context handle and announces its address and the word
  // holding the cross-thread signal's fulfiller, which the host calls through when it queues an
  // action; a later turn writes a fulfiller of its choosing into that word, as a write primitive
  // would. Under the guest kernel the write faults at the record, the isolate is condemned and
  // the context is torn down host-side, which locks the record and disposes of the object. Where
  // the write lands (no guest kernel, or the WORKERD_GK_NEG_DELETE_QUEUE build), the host then
  // queues an action for the context from the event loop and calls the chosen fulfiller, which
  // logs that it was reached.
  //
  // /__gk_queue_rewrite aims at the handle: a later turn overwrites the IoOwn's context handle
  // with the address of a buffer the turn chose, laid out as a record would be, which is what the
  // delete queue pointer was; the holder is then destroyed on the event loop. The host must
  // refuse the handle and queue nothing, leaving the object to the context's list, and must not
  // read or write the buffer. /__gk_queue_steal overwrites it with another isolate's live context
  // handle instead (announced by /__gk_ioown_announce): the host finds that context, but the
  // handle the holder drops names an object of this context's list, so nothing is queued there
  // either. /__gk_queue_after queues an action for this context from the event loop, the
  // host-side path through the record's lock and the fulfiller, and reports when the action has
  // run in a turn.
  struct ChosenFulfiller final: public kj::CrossThreadPromiseFulfiller<void> {
    void fulfill(kj::_::Void&& value) const override {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the chosen fulfiller was called by the host");
    }
    void reject(kj::Exception&& exception) const override {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the chosen fulfiller was called by the host");
    }
    bool isWaiting() const override {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the chosen fulfiller was called by the host");
      return true;
    }
  };
  static const ChosenFulfiller chosenFulfiller;
  constexpr uint64_t IOOWN_QUEUE_MAGIC = 0x0bab0bab0bab0bab;
  constexpr uint64_t IOOWN_REWRITE_MAGIC = 0x0cab0cab0cab0cab;
  constexpr uint64_t IOOWN_STEAL_MAGIC = 0x0dab0dab0dab0dab;
  if (probeEnabled && url.endsWith("/__gk_queue_corrupt")) {
    auto& context = IoContext::current();
    static kj::Maybe<kj::Own<IoOwnProbeHolder>> queueHolder;
    static DeleteQueue::RecordForTest probedRecord = {nullptr, 0, nullptr};
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_QUEUE_MAGIC))});
    uint64_t ctx = ioOwnContext(*holder);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: created an IoOwn for the queue probe inside the JS turn", url,
        kj::str("0x", kj::hex(ctx)), addr(holder->own.operator->()));
    queueHolder = kj::mv(holder);
    context.addWaitUntil(kj::evalLater([&context, ctx, addr, url = kj::str(url)]() mutable {
      // On the event loop, after the turn: the record exists in host memory, with the signal
      // task's fulfiller installed.
      probedRecord = DeleteQueue::recordForTest(ctx);
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the context record behind the IoOwn, found host-side",
          kj::str("0x", kj::hex(ctx)), addr(probedRecord.record), probedRecord.size,
          addr(probedRecord.fulfiller),
          addr(probedRecord.fulfiller == nullptr ? nullptr : *probedRecord.fulfiller));
      return context
          .run([addr, url = kj::mv(url)](Worker::Lock&) {
        auto record = probedRecord;
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: rewriting the context record from inside the JS turn", url,
            addr(record.record), addr(&chosenFulfiller));
        // The write: under the guest kernel this faults at the record and nothing below runs.
        *record.fulfiller = const_cast<ChosenFulfiller*>(&chosenFulfiller);
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: the context record was rewritten from inside the JS turn; "
            "the write landed",
            addr(record.record));
      })
          .then([&context]() {
        return kj::evalLater([&context]() {
          // Host-side: queuing an action calls the fulfiller the record holds.
          bool queued = context.getCrossContextExecutor().tryExecute([](jsg::Lock&) {});
          KJ_LOG(
              WARNING, "WORKERD_GK_TEST_PROBE: queued an action for the context host-side", queued);
        });
      }).then([&context]() {
        return context.run([](Worker::Lock&) {
          KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the turn after the record rewrite ran");
        });
      });
    }));
  }
  // The buffer /__gk_queue_rewrite aims the context handle at: a chosen fulfiller where a record
  // keeps its own, and a mutex-sized word of zeroes before it, checked afterwards for a host
  // write.
  static uint64_t chosenRecord[8] = {
    0, 0, 0, 0, 0, 0, 0, reinterpret_cast<uintptr_t>(&chosenFulfiller)};
  if (probeEnabled && url.endsWith("/__gk_queue_rewrite")) {
    auto& context = IoContext::current();
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_REWRITE_MAGIC))});
    IoOwnProbeHolder& h = *holder;
    uint64_t ctx = ioOwnContext(h);
    uint64_t handle = ioOwnHandle(h);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: created an IoOwn for the rewrite probe inside the JS turn", url,
        kj::str("0x", kj::hex(ctx)), kj::str("0x", kj::hex(handle)), addr(h.own.operator->()));
    context.addWaitUntil(
        context
            .run([&h, url = kj::str(url), addr, ctx, setIoOwnContext](Worker::Lock&) {
      setIoOwnContext(h, reinterpret_cast<uintptr_t>(chosenRecord));
      KJ_LOG(WARNING,
          "WORKERD_GK_TEST_PROBE: overwrote the IoOwn's context handle from inside the JS turn",
          url, kj::str("0x", kj::hex(ctx)), addr(chosenRecord));
    })
            .then([holder = kj::mv(holder)]() mutable {
      return kj::evalLater([holder = kj::mv(holder)]() mutable {
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: destroying the rewrite probe's holder on the event loop");
        holder = nullptr;
      });
    }).then([&context]() {
      return context.run([](Worker::Lock&) {
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: the turn after the rewrite probe holder's destruction ran",
            kj::str("0x", kj::hex(chosenRecord[0])), kj::str("0x", kj::hex(chosenRecord[7])));
      });
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_queue_steal")) {
    KJ_REQUIRE(announcedIoOwnContext != 0, "no IoOwn announced");
    auto& context = IoContext::current();
    auto holder = kj::heap<IoOwnProbeHolder>(
        IoOwnProbeHolder{.own = context.addObject(kj::heap<IoOwnProbe>(IOOWN_STEAL_MAGIC))});
    IoOwnProbeHolder& h = *holder;
    uint64_t ctx = ioOwnContext(h);
    uint64_t handle = ioOwnHandle(h);
    KJ_LOG(WARNING,
        "WORKERD_GK_TEST_PROBE: created an IoOwn for the queue steal probe inside the JS turn", url,
        kj::str("0x", kj::hex(ctx)), kj::str("0x", kj::hex(handle)), addr(h.own.operator->()));
    context.addWaitUntil(context
                             .run([&h, url = kj::str(url), ctx, setIoOwnContext](Worker::Lock&) {
      setIoOwnContext(h, announcedIoOwnContext);
      KJ_LOG(WARNING,
          "WORKERD_GK_TEST_PROBE: overwrote the IoOwn's context handle from inside the JS turn "
          "with another isolate's",
          url, kj::str("0x", kj::hex(ctx)), kj::str("0x", kj::hex(announcedIoOwnContext)));
    })
                             .then([holder = kj::mv(holder)]() mutable {
      return kj::evalLater([holder = kj::mv(holder)]() mutable {
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: destroying the queue steal probe's holder on the event loop");
        holder = nullptr;
      });
    }).then([&context]() {
      return context.run([](Worker::Lock&) {
        KJ_LOG(WARNING,
            "WORKERD_GK_TEST_PROBE: the turn after the queue steal probe holder's destruction "
            "ran");
      });
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_queue_after")) {
    auto& context = IoContext::current();
    auto paf = kj::newPromiseAndFulfiller<void>();
    context.addWaitUntil(kj::evalLater([&context, fulfiller = kj::mv(paf.fulfiller)]() mutable {
      bool queued = context.getCrossContextExecutor().tryExecute(
          [fulfiller = kj::mv(fulfiller)](jsg::Lock&) mutable {
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: a host-side queued action ran in its context");
        fulfiller->fulfill();
      });
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: queued an action for the context host-side", queued);
    }).then([promise = kj::mv(paf.promise)]() mutable { return kj::mv(promise); }));
  }
  if (probeEnabled && url.endsWith("/__gk_ioown_counts")) {
    // The host-memory block and resolve-entry counts, for a leak check across a load: read on
    // the event loop, since the entry count is the guest kernel's, which a turn cannot read.
    IoContext::current().addTask(kj::evalLater([]() {
      auto stats = OwnedObjectList::statsForTest();
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: owned-node counts", stats.nodesAllocated,
          stats.nodesFreed, stats.recordsAllocated, stats.recordsFreed, stats.chunksAllocated,
          stats.chunksFreed, hostMemory::liveResolveEntries());
    }));
  }
  // The AbortSignal native registration (api/basics.h): the record holding one abort action,
  // which the signal owns and only code under the isolate lock reads, and the table entry that
  // says the consumer still wants it, which the consumer's RAII handle owns and retires when
  // it is dropped -- on the event loop, outside the guest, where a `fetch` with a signal drops
  // it when its promise settles. /__gk_abort_cell_corrupt creates a signal and an action in
  // the turn, keeps both alive, and then, from a later turn, overwrites the disposer word of
  // the action's kj::Function with a disposer the turn chose, as a write primitive would. The
  // handle is then dropped on the event loop: it must retire the entry and nothing else, so
  // the chosen disposer is never called. (Where the handle clears the action instead, as in
  // the WORKERD_GK_NEG_ABORT_CELL build, the host destroys the function and calls the disposer
  // the turn chose, which logs that it was reached, and this script fails.)
  //
  // /__gk_abort_cell_live and /__gk_abort_cell_dropped are the behaviour either side of that:
  // a signal aborted while its consumer's handle is alive must run the action, and one aborted
  // after the handle was dropped on the event loop must not.
  struct ChosenActionDisposer final: public kj::Disposer {
    void disposeImpl(void* pointer) const override {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the chosen action disposer was called",
          kj::str("0x", kj::hex(reinterpret_cast<uintptr_t>(pointer))),
          kj::str("inTurn = ", hostMemory::inTurn()));
    }
  };
  static const ChosenActionDisposer chosenActionDisposer;
  struct AbortProbe {
    jsg::Ref<AbortSignal> signal;
    kj::Own<void> registration;
  };
  if (probeEnabled && url.endsWith("/__gk_abort_cell_corrupt")) {
    jsg::Lock& js = lock;
    auto& context = IoContext::current();
    static kj::Maybe<AbortProbe> corruptProbe;
    auto signal = js.alloc<AbortSignal>();
    auto registration = signal->addAbortAction(js, [](jsg::Lock&, const kj::Exception&) {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the corrupt probe's abort action ran");
    });
    auto found = signal->newestNativeRegistrationForTest();
    KJ_REQUIRE(found.record != nullptr && found.actionDisposerSlot != nullptr,
        "no native registration was made");
    KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: created an AbortSignal registration inside the JS turn",
        url, addr(found.record), addr(found.actionDisposerSlot),
        kj::str("0x", kj::hex(found.armed)));
    corruptProbe = AbortProbe{.signal = kj::mv(signal), .registration = kj::mv(registration)};
    context.addWaitUntil(context
                             .run([url = kj::str(url), addr](Worker::Lock& lock) {
      auto& probe = KJ_ASSERT_NONNULL(corruptProbe);
      auto found = probe.signal->newestNativeRegistrationForTest();
      void* saved = found.actionDisposerSlot[0];
      found.actionDisposerSlot[0] = const_cast<ChosenActionDisposer*>(&chosenActionDisposer);
      KJ_LOG(WARNING,
          "WORKERD_GK_TEST_PROBE: overwrote the abort action's disposer from inside the JS turn",
          url, addr(found.record), addr(saved), addr(&chosenActionDisposer));
    })
                             .then([]() {
      return kj::evalLater([]() {
        // On the event loop, outside the guest, as the holder of a signal-wrapped promise is.
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: dropping the abort registration on the event loop",
            kj::str("inTurn = ", hostMemory::inTurn()));
        KJ_ASSERT_NONNULL(corruptProbe).registration = nullptr;
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the abort registration was dropped");
      });
    }).then([&context]() {
      return context.run([](Worker::Lock&) {
        auto& probe = KJ_ASSERT_NONNULL(corruptProbe);
        auto found = probe.signal->newestNativeRegistrationForTest();
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the turn after the registration was dropped ran",
            kj::str("count = ", probe.signal->getNativeRegistrationCountForTest()),
            kj::str("armed = 0x", kj::hex(found.armed)),
            kj::str("resolves = ", hostMemory::resolve(found.armed) == found.record),
            kj::str("action = ", found.actionDisposerSlot != nullptr ? "present" : "destroyed"));
      });
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_abort_cell_live")) {
    // The control: the handle is still held, so the abort runs the action.
    jsg::Lock& js = lock;
    auto& context = IoContext::current();
    static kj::Maybe<AbortProbe> liveProbe;
    auto signal = js.alloc<AbortSignal>();
    auto registration = signal->addAbortAction(js, [](jsg::Lock&, const kj::Exception&) {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the live probe's abort action ran");
    });
    liveProbe = AbortProbe{.signal = kj::mv(signal), .registration = kj::mv(registration)};
    context.addWaitUntil(context.run([](Worker::Lock& lock) {
      jsg::Lock& js = lock;
      auto& probe = KJ_ASSERT_NONNULL(liveProbe);
      probe.signal->triggerAbort(js, kj::none);
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the live probe's signal was aborted");
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_abort_cell_dropped")) {
    // The other side: the handle is dropped on the event loop first, so the abort must not run
    // the action even though the record is still the signal's.
    jsg::Lock& js = lock;
    auto& context = IoContext::current();
    static kj::Maybe<AbortProbe> droppedProbe;
    auto signal = js.alloc<AbortSignal>();
    auto registration = signal->addAbortAction(js, [](jsg::Lock&, const kj::Exception&) {
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the dropped probe's abort action ran");
    });
    droppedProbe = AbortProbe{.signal = kj::mv(signal), .registration = kj::mv(registration)};
    context.addWaitUntil(kj::evalLater([]() {
      KJ_ASSERT_NONNULL(droppedProbe).registration = nullptr;
      KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the dropped probe's registration was dropped",
          kj::str("inTurn = ", hostMemory::inTurn()));
    }).then([&context]() {
      return context.run([](Worker::Lock& lock) {
        jsg::Lock& js = lock;
        auto& probe = KJ_ASSERT_NONNULL(droppedProbe);
        probe.signal->triggerAbort(js, kj::none);
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: the dropped probe's signal was aborted");
      });
    }));
  }
  if (probeEnabled && url.endsWith("/__gk_corrupt_handle")) {
    KJ_IF_SOME(probe, hostMemory::handleProbeForTest()) {
      static byte chosenKey[32] = {0x41};
      hostMemory::SlotHandle saved;
      memcpy(&saved, probe.handleStorage, sizeof(saved));
      hostMemory::SlotHandle chosen = reinterpret_cast<uintptr_t>(chosenKey);
      memcpy(probe.handleStorage, &chosen, sizeof(chosen));
      KJ_LOG(WARNING,
          "WORKERD_GK_TEST_PROBE: overwrote the tenant's handle from inside the JS turn", url,
          kj::str("0x", kj::hex(saved)), kj::str("0x", kj::hex(chosen)));
      IoContext::current().addTask(kj::evalLater([&probe, saved]() {
        probe.useOnHost();
        memcpy(probe.handleStorage, &saved, sizeof(saved));
        KJ_LOG(WARNING, "WORKERD_GK_TEST_PROBE: restored the tenant's handle",
            kj::str("0x", kj::hex(saved)));
        probe.useOnHost();
      }));
    }
  }
}

}  // namespace

kj::Promise<DeferredProxy<void>> ServiceWorkerGlobalScope::request(kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::AsyncInputStream& requestBody,
    kj::HttpService::Response& response,
    kj::Maybe<kj::StringPtr> cfBlobJson,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler,
    kj::Maybe<jsg::Ref<AbortSignal>> abortSignal) {
  TRACE_EVENT("workerd", "ServiceWorkerGlobalScope::request()");
  maybeForceGuestFaultForTest(url, lock);
  // To construct a ReadableStream object, we're supposed to pass in an Own<AsyncInputStream>, so
  // that it can drop the reference whenever it gets GC'ed. But in this case the stream's lifetime
  // is not under our control -- it's attached to the request. So, we wrap it in a
  // NeuterableInputStream which allows us to disconnect the stream before it becomes invalid.
  auto ownRequestBody = newNeuterableInputStream(requestBody);
  auto deferredNeuter = kj::defer([ownRequestBody = kj::addRef(*ownRequestBody)]() mutable {
    // Make sure to cancel the request body stream since the native stream is no longer valid once
    // the returned promise completes. Note that the KJ HTTP library deals with the fact that we
    // haven't consumed the entire request body.
    ownRequestBody->neuter(makeNeuterException(NeuterReason::CLIENT_DISCONNECTED));
  });
  KJ_ON_SCOPE_FAILURE(ownRequestBody->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION)));

  auto& ioContext = IoContext::current();
  jsg::Lock& js = lock;

  CfProperty cf(cfBlobJson);

  // We only create the body stream if there is a body to read.
  kj::Maybe<JsReadableStream> maybeJsStream = kj::none;

  // If the request has "no body", we want `request.body` to be null. But, this is not the same
  // thing as the request having a body that happens to be empty. Unfortunately, KJ HTTP gives us
  // a zero-length AsyncInputStream either way, so we can't just check the stream length.
  //
  // The HTTP spec says: "The presence of a message body in a request is signaled by a
  // Content-Length or Transfer-Encoding header field." RFC 7230, section 3.3.
  // https://tools.ietf.org/html/rfc7230#section-3.3
  //
  // But, the request was not necessarily received over HTTP! It could be from another worker in
  // a pipeline, or it could have been received over RPC. In either case, the headers don't
  // necessarily mean anything; the calling worker can fill them in however it wants.
  //
  // So, we decide if the body is null if both headers are missing AND the stream is known to have
  // zero length. And on the sending end (fetchImpl() in http.c++), if we're sending a request with
  // a non-null body that is known to be empty, we explicitly set Content-Length: 0. This should
  // mean that in all worker-to-worker interactions, if the sender provided a non-null body, the
  // receiver will receive a non-null body, independent of anything else.
  //
  // TODO(cleanup): Should KJ HTTP interfaces explicitly communicate the difference between a
  //   missing body and an empty one?
  kj::Maybe<Body::ExtractedBody> body;
  if (headers.get(kj::HttpHeaderId::CONTENT_LENGTH) != kj::none ||
      headers.get(kj::HttpHeaderId::TRANSFER_ENCODING) != kj::none ||
      requestBody.tryGetLength().orDefault(1) > 0) {
    // We do not automatically decode gzipped request bodies because the fetch() standard doesn't
    // specify any automatic encoding of requests. https://github.com/whatwg/fetch/issues/589
    auto b = newSystemStream(kj::addRef(*ownRequestBody), StreamEncoding::IDENTITY);
    auto jsStream = JsReadableStream::create(js, ioContext, kj::mv(b));
    body = Body::ExtractedBody(js, jsStream.addRef(js));
    maybeJsStream = kj::mv(jsStream);
  }

  auto jsHeaders = js.alloc<Headers>(js, headers, Headers::Guard::REQUEST);

  // If the request doesn't specify "Content-Length" or "Transfer-Encoding", set "Content-Length"
  // to the body length if it's known. This ensures handlers for worker-to-worker requests can
  // access known body lengths if they're set, without buffering bodies.
  if (body != kj::none && !jsHeaders->hasCommon(capnp::CommonHeaderName::CONTENT_LENGTH) &&
      !jsHeaders->hasCommon(capnp::CommonHeaderName::TRANSFER_ENCODING)) {
    KJ_IF_SOME(l, requestBody.tryGetLength()) {
      jsHeaders->setCommon(capnp::CommonHeaderName::CONTENT_LENGTH, kj::str(l));
    } else {
      jsHeaders->setCommon(capnp::CommonHeaderName::TRANSFER_ENCODING, kj::str("chunked"));
    }
  }

  if (defaultFetcher == kj::none) {
    // The default fetcher can be shared across requests since it does not persist any
    // request-specific state. We can create one lazily here for the first request
    // handled by this global scope and avoid the extra allocations on subsequence requests.
    // We could also consider creating it lazily when first accessed, but Fetcher is very
    // lightweight so this seems sufficient for now.
    defaultFetcher =
        js.alloc<Fetcher>(IoContext::NEXT_CLIENT_CHANNEL, Fetcher::RequiresHostAndProtocol::YES);
  }

  auto jsRequest = js.alloc<Request>(js, method, url, Request::Redirect::MANUAL, kj::mv(jsHeaders),
      KJ_ASSERT_NONNULL(defaultFetcher).addRef(),
      /* signal */ kj::mv(abortSignal), kj::mv(cf), kj::mv(body),
      /* thisSignal */ kj::none, Request::CacheMode::NONE);

  // signal vs thisSignal
  // --------------------
  // The fetch spec definition of Request has a distinction between the
  // "signal" (which is an optional AbortSignal passed in with the options), and "this' signal",
  // which is an AbortSignal that is always available via the request.signal accessor.
  //
  // redirect
  // --------
  // I set the redirect mode to manual here, so that by default scripts that just pass requests
  // through to a fetch() call will behave the same as scripts which don't call .respondWith(): if
  // the request results in a redirect, the visitor will see that redirect.

  auto event = js.alloc<FetchEvent>(kj::mv(jsRequest));

  uint tasksBefore = ioContext.taskCount();

  // We'll drop our span once the promise (fetch handler result) resolves.
  kj::Maybe<SpanBuilder> span = ioContext.makeTraceSpan("fetch_handler"_kjc);
  bool useDefaultHandling;
  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(f, h.fetch) {
      auto promise = f(lock, event->getRequest(), h.env.addRef(js), h.getCtx());
      event->respondWith(lock, kj::mv(promise));
      useDefaultHandling = false;
    } else {
      // In modules mode we don't have a concept of "default handling".
      lock.logWarningOnce("Received a FetchEvent but we lack a handler for FetchEvents. "
                          "Did you remember to export a fetch() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export a fetch() function.");
    }
  } else {
    // Fire off the handlers.
    useDefaultHandling = dispatchEventImpl(lock, event.addRef()).result;
  }

  if (useDefaultHandling) {
    // No one called respondWith() or preventDefault(). Go directly to subrequest.

    if (ioContext.taskCount() > tasksBefore) {
      lock.logWarning(
          "FetchEvent handler did not call respondWith() before returning, but initiated some "
          "asynchronous task. That task will be canceled and default handling will occur -- the "
          "request will be sent unmodified to your origin. Remember that you must call "
          "respondWith() *before* the event handler returns, if you don't want default handling. "
          "You cannot call it asynchronously later on. If you need to wait for I/O (e.g. a "
          "subrequest) before generating a Response, then call respondWith() with a Promise (for "
          "the eventual Response) as the argument.");
    }

    KJ_IF_SOME(jsStream, maybeJsStream) {
      if (jsStream.isDisturbed(js)) {
        lock.logUncaughtException(
            "Script consumed request body but didn't call respondWith(). Can't forward request.");
        return addNoopDeferredProxy(
            response.sendError(500, "Internal Server Error", ioContext.getHeaderTable()));
      }
      maybeJsStream = kj::none;  // Release our reference; we don't need it anymore.
    }

    auto client = ioContext.getHttpClient(
        IoContext::NEXT_CLIENT_CHANNEL, false, mapCopyString(cfBlobJson), "fetch_default"_kjc);
    auto adapter = kj::newHttpService(*client);
    auto promise = adapter->request(method, url, headers, requestBody, response);
    // Default handling doesn't rely on the IoContext at all so we can return it as a
    // deferred proxy task.
    return DeferredProxy<void>{promise.attach(kj::mv(adapter), kj::mv(client))};
  } else KJ_IF_SOME(promise, event->getResponsePromise(lock)) {
    maybeJsStream = kj::none;  // Release reference to request body stream. We don't need it.
    auto body2 = kj::addRef(*ownRequestBody);

    // HACK: If the client disconnects, the `response` reference is no longer valid. But our
    //   promise resolves in JavaScript space, so won't be canceled. So we need to track
    //   cancellation separately. We use a refcounted boolean.
    // TODO(cleanup): Is there something less ugly we can do here?
    auto canceled = kj::rc<bool>(false);

    return ioContext
        .awaitJs(lock,
            promise.then(kj::implicitCast<jsg::Lock&>(lock),
                ioContext.addFunctor(
                    [&response, allowWebSocket = headers.isWebSocket(),
                        canceled = canceled.addRef(), &headers, span = kj::mv(span)](
                        jsg::Lock& js, IoContext& context, jsg::Ref<Response> innerResponse) mutable
                    -> IoOwn<kj::Promise<DeferredProxy<void>>> {
      JSG_REQUIRE(innerResponse->getType() != "error"_kj, TypeError,
          "Return value from serve handler must not be an error response (like Response.error())");

      // Drop our fetch_handler span now that the promise has resolved.
      span = kj::none;
      if (*canceled) {
        // Oops, the client disconnected before the response was ready to send. `response` is
        // a dangling reference, let's not use it.
        return context.addObject(kj::heap(addNoopDeferredProxy(kj::READY_NOW)));
      } else {
        return context.addObject(kj::heap(
            innerResponse->send(js, response, {.allowWebSocket = allowWebSocket}, headers)));
      }
    })))
        .attach(kj::defer([canceled = kj::mv(canceled)]() mutable { *canceled = true; }))
        .then(
            [ownRequestBody = kj::mv(ownRequestBody), deferredNeuter = kj::mv(deferredNeuter)](
                DeferredProxy<void> deferredProxy) mutable {
      // In the case of bidirectional streaming, the request body stream needs to remain valid
      // while proxying the response. So, arrange for neutering to happen only after the proxy
      // task finishes.
      deferredProxy.proxyTask = deferredProxy.proxyTask
                                    .then([body = kj::addRef(*ownRequestBody)]() mutable {
        body->neuter(makeNeuterException(NeuterReason::SENT_RESPONSE));
      }, [body = kj::addRef(*ownRequestBody)](kj::Exception&& e) mutable {
        body->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION));
        kj::throwFatalException(kj::mv(e));
      }).attach(kj::mv(deferredNeuter));

      return deferredProxy;
    },
            [body = kj::mv(body2)](kj::Exception&& e) mutable -> DeferredProxy<void> {
      // HACK: We depend on the fact that the success-case lambda above hasn't been destroyed yet
      //   so `deferredNeuter` hasn't been destroyed yet.
      body->neuter(makeNeuterException(NeuterReason::THREW_EXCEPTION));
      kj::throwFatalException(kj::mv(e));
    });
  } else {
    // The service worker API says that if default handling is prevented and respondWith() wasn't
    // called, the request should result in "a network error".
    return KJ_EXCEPTION(DISCONNECTED, "preventDefault() called but respondWith() not called");
  }
}

void ServiceWorkerGlobalScope::sendTraces(kj::ArrayPtr<kj::Own<Trace>> traces,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  auto isolate = lock.getIsolate();
  jsg::Lock& js = lock;

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(f, h.tail) {
      auto tailEvent = js.alloc<TailEvent>(lock, "tail"_kjc, traces);
      auto promise = f(lock, tailEvent->getEvents(), h.env.addRef(isolate), h.getCtx());
      tailEvent->waitUntil(kj::mv(promise));
    } else KJ_IF_SOME(f, h.trace) {
      auto traceEvent = js.alloc<TailEvent>(lock, "trace"_kjc, traces);
      auto promise = f(lock, traceEvent->getEvents(), h.env.addRef(isolate), h.getCtx());
      traceEvent->waitUntil(kj::mv(promise));
    } else {
      lock.logWarningOnce("Attempted to send events but we lack a handler, "
                          "did you remember to export a tail() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export a tail() function.");
    }
  } else {
    // Fire off the handlers.
    // We only create both events here.
    auto tailEvent = js.alloc<TailEvent>(lock, "tail"_kjc, traces);
    auto traceEvent = js.alloc<TailEvent>(lock, "trace"_kjc, traces);
    dispatchEventImpl(lock, tailEvent.addRef());
    dispatchEventImpl(lock, traceEvent.addRef());

    // We assume no action is necessary for "default" trace handling.
  }
}

void ServiceWorkerGlobalScope::startScheduled(kj::Date scheduledTime,
    kj::StringPtr cron,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  auto& context = IoContext::current();
  jsg::Lock& js = lock;

  double eventTime = (scheduledTime - kj::UNIX_EPOCH) / kj::MILLISECONDS;

  auto event = js.alloc<ScheduledEvent>(eventTime, cron);

  auto isolate = lock.getIsolate();

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(f, h.scheduled) {
      auto promise =
          f(lock, js.alloc<ScheduledController>(event.addRef()), h.env.addRef(isolate), h.getCtx())
              .then([&context]() {
        KJ_IF_SOME(t, context.getWorkerTracer()) {
          t.setReturn(context.now());
        }
      });
      event->waitUntil(kj::mv(promise));
    } else {
      lock.logWarningOnce(
          "Received a ScheduledEvent but we lack a handler for ScheduledEvents "
          "(a.k.a. Cron Triggers). Did you remember to export a scheduled() function?");
      context.setNoRetryScheduled();
      JSG_FAIL_REQUIRE(Error, "Handler does not export a scheduled() function");
    }
  } else {
    // Fire off the handlers after confirming there is at least one.
    if (getHandlerCount("scheduled") == 0) {
      lock.logWarningOnce(
          "Received a ScheduledEvent but we lack an event listener for scheduled events "
          "(a.k.a. Cron Triggers). Did you remember to call addEventListener(\"scheduled\", ...)?");
      context.setNoRetryScheduled();
      JSG_FAIL_REQUIRE(Error, "No event listener registered for scheduled events.");
    }
    dispatchEventImpl(lock, event.addRef());
  }
}

namespace {
// Returns true if an alarm failure should count against the user's retry limit.
// A failure is user-generated if any of:
//   - The exception was explicitly tagged with EXCEPTION_IS_USER_ERROR at construction time
//     (e.g. state.abort(), exceededCpu, exceededMemory, exceededWallTime, overload queue).
//   - The exception originated from user code throwing inside blockConcurrencyWhile, which
//     breaks the input gate as a secondary side-effect.
//   - The exception is a plain jsg.* error without broken.* or jsg-internal.* prefixes,
//     meaning the user's handler threw directly.
bool isAlarmFailureUserError(kj::StringPtr description, bool hasUserErrorDetail) {
  if (hasUserErrorDetail) return true;
  if (jsg::isExceptionFromInputGateBroken(description)) return true;
  auto tunneled = jsg::tunneledErrorType(description);
  return tunneled.isJsgError && !tunneled.isInternal && !tunneled.isDurableObjectReset;
}

struct AlarmExceptionInfo {
  bool isAbort;
  bool retry;
};

AlarmExceptionInfo inspectAlarmException(const kj::Exception& exception, IoContext& context) {
  // `exception` is the immediate promise rejection, which may differ from the original context
  // abort reason after V8 termination or an output-gate failure. Details may survive on only one
  // of these exceptions, so inspect both.
  AlarmExceptionInfo result{
    .isAbort = exception.getDetail(jsg::EXCEPTION_DURABLE_OBJECT_ABORT) != kj::none,
    .retry = exception.getDetail(jsg::EXCEPTION_DURABLE_OBJECT_ABORT_NO_RETRY) == kj::none,
  };
  KJ_IF_SOME(abortReason, context.getAbortReason()) {
    result.isAbort =
        result.isAbort || abortReason.getDetail(jsg::EXCEPTION_DURABLE_OBJECT_ABORT) != kj::none;
    result.retry = result.retry &&
        abortReason.getDetail(jsg::EXCEPTION_DURABLE_OBJECT_ABORT_NO_RETRY) == kj::none;
  }
  return result;
}
}  // namespace

kj::Promise<WorkerInterface::AlarmResult> ServiceWorkerGlobalScope::runAlarm(kj::Date scheduledTime,
    kj::Duration timeout,
    uint32_t retryCount,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {

  auto& context = IoContext::current();
  auto& actor = KJ_ASSERT_NONNULL(context.getActor());
  auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());

  kj::String actorId;
  KJ_SWITCH_ONEOF(actor.getId()) {
    KJ_CASE_ONEOF(f, kj::Own<ActorIdFactory::ActorId>) {
      actorId = f->toString();
    }
    KJ_CASE_ONEOF(s, kj::String) {
      actorId = kj::str(s);
    }
  }

  auto currentTime = context.now();
  KJ_SWITCH_ONEOF(persistent.armAlarmHandler(
                      scheduledTime, context.getCurrentTraceSpan(), currentTime, false, actorId)) {
    KJ_CASE_ONEOF(armResult, ActorCacheInterface::RunAlarmHandler) {
      auto& handler = KJ_REQUIRE_NONNULL(exportedHandler);
      if (handler.alarm == kj::none) {

        lock.logWarningOnce("Attempted to run a scheduled alarm without a handler, "
                            "did you remember to export an alarm() function?");
        return WorkerInterface::AlarmResult{
          .retry = false, .outcome = EventOutcome::SCRIPT_NOT_FOUND};
      }

      auto& alarm = KJ_ASSERT_NONNULL(handler.alarm);

      return context
          .run([exportedHandler, timeout, retryCount, scheduledTime, &alarm,
                   maybeAsyncContext = jsg::AsyncContextFrame::currentRef(lock)](Worker::Lock& lock,
                   IoContext& context) mutable -> kj::Promise<WorkerInterface::AlarmResult> {
        jsg::AsyncContextFrame::Scope asyncScope(lock, maybeAsyncContext);
        // We want to limit alarm handler walltime to 15 minutes at most. If the timeout promise
        // completes we want to cancel the alarm handler. If the alarm handler promise completes
        // first timeout will be canceled.
        jsg::Lock& js = lock;
        auto timeoutPromise = context.afterLimitTimeout(timeout).then(
            [&context]() -> kj::Promise<WorkerInterface::AlarmResult> {
          // We don't want to delete the alarm since we have not successfully completed the alarm
          // execution.
          auto& actor = KJ_ASSERT_NONNULL(context.getActor());
          auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());
          persistent.cancelDeferredAlarmDeletion();

          LOG_NOSENTRY(WARNING, "Alarm exceeded its allowed execution time");
          // Report alarm handler failure and log it.
          auto e = KJ_EXCEPTION(OVERLOADED,
              "broken.dropped; worker_do_not_log; jsg.Error: Alarm exceeded its allowed execution time");
          e.setDetail(jsg::EXCEPTION_IS_USER_ERROR, kj::heapArray<kj::byte>(0));
          e.setDetail(WALL_TIME_LIMIT_DETAIL_ID, kj::heapArray<kj::byte>(0));
          context.getMetrics().reportFailure(e);

          // We don't want the handler to keep running after timeout.
          context.abort(kj::mv(e));
          // We want timed out alarms to be treated as user errors. As such, we'll mark them as
          // retriable, and we'll count the retries against the alarm retries limit. This will ensure
          // that the handler will attempt to run for a number of times before giving up and deleting
          // the alarm.
          return WorkerInterface::AlarmResult{.retry = true,
            .retryCountsAgainstLimit = true,
            .outcome = EventOutcome::EXCEEDED_WALL_TIME};
        });

        return alarm(lock, js.alloc<AlarmInvocationInfo>(scheduledTime, retryCount))
            .then([]() -> kj::Promise<WorkerInterface::AlarmResult> {
          return WorkerInterface::AlarmResult{.retry = false, .outcome = EventOutcome::OK};
        }).exclusiveJoin(kj::mv(timeoutPromise));
      })
          .catch_([&context, deferredDelete = kj::mv(armResult.deferredDelete)](
                      kj::Exception&& e) mutable {
        auto& actor = KJ_ASSERT_NONNULL(context.getActor());
        auto& persistent = KJ_ASSERT_NONNULL(actor.getPersistent());
        persistent.cancelDeferredAlarmDeletion();

        context.getMetrics().reportFailure(e);

        auto description = kj::str(e.getDescription());  // because e is moved before this is used
        auto log = !jsg::isTunneledException(description) && !jsg::isDoNotLogException(description);
        auto isUserError = e.getDetail(jsg::EXCEPTION_IS_USER_ERROR) != kj::none;
        auto alarmException = inspectAlarmException(e, context);

        // This will include the error in inspector/tracers and log to syslog if internal.
        context.logUncaughtExceptionAsync(UncaughtExceptionSource::ALARM_HANDLER, kj::mv(e));

        auto limitsExceeded = context.getLimitEnforcer().getLimitsExceeded();
        EventOutcome outcome = EventOutcome::ABORTED;
        if (!alarmException.isAbort) {
          outcome = EventOutcome::EXCEPTION;
          KJ_IF_SOME(status, limitsExceeded) {
            outcome = status;
          }
        }

        kj::String actorId;
        KJ_SWITCH_ONEOF(actor.getId()) {
          KJ_CASE_ONEOF(f, kj::Own<ActorIdFactory::ActorId>) {
            actorId = f->toString();
          }
          KJ_CASE_ONEOF(s, kj::String) {
            actorId = kj::str(s);
          }
        }

        auto isUserGeneratedError = isAlarmFailureUserError(description, isUserError);
        auto shouldRetryCountsAgainstLimits = alarmRetryCountsAgainstLimit({
          .outputGateBroken = context.isOutputGateBroken(),
          .isUserError = isUserGeneratedError,
          .resourceLimitExceeded = limitsExceeded != kj::none,
        });

        // We want to alert if we aren't going to count this alarm retry against limits, since an
        // uncounted broken-output-gate failure can retry forever. Skip logging when the retry IS
        // counted: a user-generated error, or a resource-limit breach that broke the gate (e.g.
        // SQLITE_INTERRUPT from the CPU limiter), is expected behaviour.
        if (!shouldRetryCountsAgainstLimits && log && context.isOutputGateBroken()) {
          LOG_NOSENTRY(ERROR, "output lock broke during alarm execution", actorId, description);
        } else if (!shouldRetryCountsAgainstLimits && context.isOutputGateBroken()) {
          // Tunneled or do-not-log non-user error with a broken output gate. Log for diagnostics
          // so we can investigate stuck alarms.
          LOG_NOSENTRY(ERROR,
              "output lock broke during alarm execution without an interesting error description",
              actorId, description, shouldRetryCountsAgainstLimits);
        }
        return WorkerInterface::AlarmResult{.retry = alarmException.retry,
          .retryCountsAgainstLimit = shouldRetryCountsAgainstLimits,
          .outcome = outcome,
          .errorDescription = kj::str(description)};
      })
          .then([&context](WorkerInterface::AlarmResult result)
                    -> kj::Promise<WorkerInterface::AlarmResult> {
        return context.waitForOutputLocks().then([result = kj::mv(result)]() mutable {
          return kj::mv(result);
        }, [&context](kj::Exception&& e) {
          auto& actor = KJ_ASSERT_NONNULL(context.getActor());
          kj::String actorId;
          KJ_SWITCH_ONEOF(actor.getId()) {
            KJ_CASE_ONEOF(f, kj::Own<ActorIdFactory::ActorId>) {
              actorId = f->toString();
            }
            KJ_CASE_ONEOF(s, kj::String) {
              actorId = kj::str(s);
            }
          }
          context.getMetrics().reportFailure(e);
          auto isUserGeneratedError = isAlarmFailureUserError(
              e.getDescription(), e.getDetail(jsg::EXCEPTION_IS_USER_ERROR) != kj::none);
          auto shouldRetryCountsAgainstLimits = alarmRetryCountsAgainstLimit({
            // This continuation only runs when waitForOutputLocks() failed, i.e. the output gate
            // is broken.
            .outputGateBroken = true,
            .isUserError = isUserGeneratedError,
            .resourceLimitExceeded = context.getLimitEnforcer().getLimitsExceeded() != kj::none,
          });
          if (auto desc = e.getDescription();
              !jsg::isTunneledException(desc) && !jsg::isDoNotLogException(desc)) {
            if (!shouldRetryCountsAgainstLimits) {
              if (isInterestingException(e)) {
                LOG_EXCEPTION("alarmOutputLock"_kj, e);
              } else {
                LOG_NOSENTRY(ERROR, "output lock broke after executing alarm", actorId, e);
              }
            }
          } else if (!shouldRetryCountsAgainstLimits) {
            // Tunneled or do-not-log exception that is neither a user error nor a resource-limit
            // breach, so it is not counted against the retry limit.
            LOG_NOSENTRY(ERROR,
                "output lock broke after executing alarm with tunneled non-user error", actorId,
                e.getDescription());
          }
          auto alarmException = inspectAlarmException(e, context);
          return WorkerInterface::AlarmResult{.retry = alarmException.retry,
            .retryCountsAgainstLimit = shouldRetryCountsAgainstLimits,
            .outcome = alarmException.isAbort ? EventOutcome::ABORTED : EventOutcome::EXCEPTION,
            .errorDescription = kj::str(e.getDescription())};
        });
      });
    }
    KJ_CASE_ONEOF(armResult, ActorCacheInterface::CancelAlarmHandler) {
      return armResult.waitBeforeCancel.then([]() {
        return WorkerInterface::AlarmResult{.retry = false, .outcome = EventOutcome::CANCELED};
      });
    }
  }
  KJ_UNREACHABLE;
}

jsg::Promise<void> ServiceWorkerGlobalScope::test(
    Worker::Lock& lock, kj::Maybe<ExportedHandler&> exportedHandler) {
  // TODO(someday): For Service Workers syntax, do we want addEventListener("test")? Not supporting
  //   it for now.
  ExportedHandler& eh = JSG_REQUIRE_NONNULL(
      exportedHandler, Error, "Tests are not currently supported with Service Workers syntax.");

  auto& testHandler =
      JSG_REQUIRE_NONNULL(eh.test, Error, "Entrypoint does not export a test() function.");

  jsg::Lock& js = lock;
  return testHandler(lock, js.alloc<TestController>(), eh.env.addRef(lock), eh.getCtx());
}

// This promise is used to set the timeout for hibernatable websocket events. It's expected to be
// dropped in most cases, as long as the hibernatable websocket event promise completes before it.
kj::Promise<void> ServiceWorkerGlobalScope::eventTimeoutPromise(uint32_t timeoutMs) {
  auto& actor = KJ_ASSERT_NONNULL(IoContext::current().getActor());
  co_await IoContext::current().afterLimitTimeout(timeoutMs * kj::MILLISECONDS);
  // This is the ActorFlushReason for eviction in Cloudflare's internal implementation.
  auto evictionCode = 2;
  auto e = KJ_EXCEPTION(DISCONNECTED,
      "broken.dropped; jsg.Error: Actor exceeded event execution time and was disconnected.");
  e.setDetail(jsg::EXCEPTION_IS_USER_ERROR, kj::heapArray<kj::byte>(0));
  actor.shutdown(evictionCode, kj::mv(e));
}

kj::Promise<void> ServiceWorkerGlobalScope::setHibernatableEventTimeout(
    kj::Promise<void> event, kj::Maybe<uint32_t> eventTimeoutMs) {
  // If we have a maximum event duration timeout set, we should prevent the actor from running
  // for more than the user selected duration.
  auto timeoutMs = eventTimeoutMs.orDefault(static_cast<uint32_t>(0));
  if (timeoutMs > 0) {
    return event.exclusiveJoin(eventTimeoutPromise(timeoutMs));
  }
  return event;
}

// TODO(cleanup): the hibernatable websocket handler functions here are largely identical – consider
// folding them.
//
// Note: The hibernatable WebSocket message path passes kj::OneOf<kj::String, kj::Array<byte>>
// directly to the webSocketMessage() handler, so binary data is always delivered as ArrayBuffer.
// The WebSocket binaryType property (and the websocket_standard_binary_type compat flag) has no
// effect here — this is by design for the Durable Object handler API, which bypasses the
// normal WebSocket read loop and its Blob/ArrayBuffer dispatch logic.
void ServiceWorkerGlobalScope::sendHibernatableWebSocketMessage(IoContext& context,
    kj::OneOf<kj::String, kj::Array<byte>> message,
    kj::Maybe<uint32_t> eventTimeoutMs,
    kj::String websocketId,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  jsg::Lock& js = lock;
  auto event = js.alloc<HibernatableWebSocketEvent>();
  // Even if no handler is exported, we need to claim the websocket so it's removed from the map.
  auto websocket = event->claimWebSocket(lock, websocketId);

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(handler, h.webSocketMessage) {
      event->waitUntil(setHibernatableEventTimeout(
          handler(lock, kj::mv(websocket), kj::mv(message)), eventTimeoutMs)
                           .then([&context]() {
        KJ_IF_SOME(t, context.getWorkerTracer()) {
          t.setReturn(context.now());
        }
      }));
    }
    // We want to deliver a message, but if no webSocketMessage handler is exported, we shouldn't fail
  }
}

void ServiceWorkerGlobalScope::sendHibernatableWebSocketClose(IoContext& context,
    HibernatableSocketParams::Close close,
    kj::Maybe<uint32_t> eventTimeoutMs,
    kj::String websocketId,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  jsg::Lock& js = lock;
  auto event = js.alloc<HibernatableWebSocketEvent>();

  // Even if no handler is exported, we need to claim the websocket so it's removed from the map.
  //
  // We won't be dispatching any further events because we've received a close, so copy the
  // websocket's tags before the manager drops them.
  auto releasePackage = event->prepareForRelease(lock, websocketId);
  auto websocket = kj::mv(releasePackage.webSocketRef);
  websocket->initiateHibernatableRelease(
      lock, kj::mv(releasePackage.tags), api::WebSocket::HibernatableReleaseState::CLOSE);
  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(handler, h.webSocketClose) {
      event->waitUntil(setHibernatableEventTimeout(
          handler(lock, kj::mv(websocket), close.code, kj::mv(close.reason), close.wasClean),
          eventTimeoutMs)
                           .then([&context]() {
        KJ_IF_SOME(t, context.getWorkerTracer()) {
          t.setReturn(context.now());
        }
      }));
    }
    // We want to deliver close, but if no webSocketClose handler is exported, we shouldn't fail
  }
}

void ServiceWorkerGlobalScope::sendHibernatableWebSocketError(IoContext& context,
    kj::Exception e,
    kj::Maybe<uint32_t> eventTimeoutMs,
    kj::String websocketId,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler) {
  jsg::Lock& js = lock;
  auto event = js.alloc<HibernatableWebSocketEvent>();

  // Even if no handler is exported, we need to claim the websocket so it's removed from the map.
  //
  // We won't be dispatching any further events because we've encountered an error, so copy the
  // websocket's tags before the manager drops them.
  auto releasePackage = event->prepareForRelease(lock, websocketId);
  auto& websocket = releasePackage.webSocketRef;
  websocket->initiateHibernatableRelease(
      lock, kj::mv(releasePackage.tags), WebSocket::HibernatableReleaseState::ERROR);

  KJ_IF_SOME(h, exportedHandler) {
    KJ_IF_SOME(handler, h.webSocketError) {
      event->waitUntil(setHibernatableEventTimeout(
          handler(js, kj::mv(websocket), js.exceptionToJs(kj::mv(e))), eventTimeoutMs)
                           .then([&context]() {
        KJ_IF_SOME(t, context.getWorkerTracer()) {
          t.setReturn(context.now());
        }
      }));
    }
    // We want to deliver an error, but if no webSocketError handler is exported, we shouldn't fail
  }
}

void ServiceWorkerGlobalScope::emitPromiseRejection(jsg::Lock& js,
    v8::PromiseRejectEvent event,
    jsg::V8Ref<v8::Promise> promise,
    jsg::Value value) {

  const auto hasHandlers = [this] {
    return getHandlerCount("unhandledrejection"_kj) + getHandlerCount("rejectionhandled"_kj);
  };

  const auto hasInspector = [] {
    KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
      return ioContext.isInspectorEnabled();
    } else {
      return false;
    }
  };

  if (hasHandlers() || hasInspector()) {
    unhandledRejections.setUseMicrotasksCompletedCallback(
        FeatureFlags::get(js).getUnhandledRejectionAfterMicrotaskCheckpoint());
    unhandledRejections.report(js, event, kj::mv(promise), kj::mv(value));
  }
}

void ServiceWorkerGlobalScope::setConnectOverride(kj::String networkAddress, ConnectFn connectFn) {
  connectOverrides.upsert(kj::mv(networkAddress), kj::mv(connectFn));
}

kj::Maybe<ServiceWorkerGlobalScope::ConnectFn&> ServiceWorkerGlobalScope::getConnectOverride(
    kj::StringPtr networkAddress) {
  return connectOverrides.find(networkAddress);
}

void ServiceWorkerGlobalScope::setDnsOverride(kj::String hostname, kj::String ip) {
  dnsOverrides.upsert(kj::mv(hostname), kj::mv(ip));
}

kj::Maybe<kj::StringPtr> ServiceWorkerGlobalScope::getDnsOverride(kj::StringPtr hostname) {
  return dnsOverrides.find(hostname).map([](kj::String& ip) -> kj::StringPtr { return ip; });
}

jsg::JsString ServiceWorkerGlobalScope::btoa(jsg::Lock& js, jsg::JsString str) {
  // We could implement btoa() by accepting a kj::String, but then we'd have to check that it
  // doesn't have any multibyte code points. Easier to perform that test using v8::String's
  // ContainsOnlyOneByte() function.
  JSG_REQUIRE(str.containsOnlyOneByte(), DOMInvalidCharacterError,
      "btoa() can only operate on characters in the Latin1 (ISO/IEC 8859-1) range.");
  auto strArray = str.toArray<kj::byte>(js);
  auto expected_length = simdutf::base64_length_from_binary(strArray.size());
  KJ_STACK_ARRAY(kj::byte, result, expected_length, 1024, 1024);
  auto written = simdutf::binary_to_base64(
      strArray.asChars().begin(), strArray.size(), result.asChars().begin());
  return js.str(result.first(written));
}

#ifdef WORKERD_FUZZILLI
void ServiceWorkerGlobalScope::fuzzilli(jsg::Lock& js, jsg::Arguments<jsg::Value> args) {
  // Delegate to the fuzzilli handler in fuzzilli.c++
  fuzzilli_handler(js, args);
}
#endif

jsg::JsString ServiceWorkerGlobalScope::atob(jsg::Lock& js, kj::String data) {
  auto size = simdutf::maximal_binary_length_from_base64(data.begin(), data.size());
  auto decoded = kj::heapArray<kj::byte>(size);
  auto result = simdutf::base64_to_binary(
      data.begin(), data.size(), decoded.asChars().begin(), simdutf::base64_default);

  JSG_REQUIRE(result.error == simdutf::SUCCESS, DOMInvalidCharacterError,
      "atob() called with invalid base64-encoded data. (Only whitespace, '+', '/', alphanumeric "
      "ASCII, and up to two terminal '=' signs when the input data length is divisible by 4 are "
      "allowed.)");

  // Similar to btoa() taking a v8::Value, we return a v8::String directly, as this allows us to
  // construct a string from the non-nul-terminated array returned from base64_to_binary(). This avoids
  // making a copy purely to append a nul byte.
  KJ_ASSERT(result.count <= size);
  return js.str(decoded.first(result.count));
}

void ServiceWorkerGlobalScope::queueMicrotask(jsg::Lock& js, jsg::Function<void()> task) {
  auto fn = js.wrapSimpleFunction(js.v8Context(),
      JSG_VISITABLE_LAMBDA((this, fn = kj::mv(task)), (fn),
          (jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
            js.tryCatch([&] {
              // The function won't be called with any arguments, so we can
              // safely ignore anything passed in to args.
              fn(js);
            }, [&](jsg::Value exception) {
              // The reportError call itself can potentially throw errors. Let's catch
              // and report them as well.
              js.tryCatch([&] { reportError(js, jsg::JsValue(exception.getHandle(js))); },
                  [&](jsg::Value exception) {
                // An error was thrown by the 'error' event handler. That's unfortunate.
                // Let's log the error and just continue. It won't be possible to actually
                // catch or handle this error so logging is really the only way to notify
                // folks about it.
                auto val = jsg::JsValue(exception.getHandle(js));
                // If the value is an object that has a stack property, log that so we get
                // the stack trace if it is an exception.
                KJ_IF_SOME(obj, val.tryCast<jsg::JsObject>()) {
                auto stack = obj.get(js, "stack"_kj);
                if (!stack.isUndefined()) {
                js.reportError(stack);
                return;
                }
                } else {
                }  // Here to avoid a compile warning
                // Otherwise just log the stringified value generically.
                js.reportError(val);
              });
            });
          }));

  js.v8Context()->GetMicrotaskQueue()->EnqueueMicrotask(js.v8Isolate, fn);
}

jsg::JsValue ServiceWorkerGlobalScope::structuredClone(
    jsg::Lock& js, jsg::JsValue value, jsg::Optional<StructuredCloneOptions> maybeOptions) {
  KJ_IF_SOME(options, maybeOptions) {
    KJ_IF_SOME(transfer, options.transfer) {
      auto transfers = KJ_MAP(i, transfer) { return i.getHandle(js); };
      return value.structuredClone(js, kj::mv(transfers));
    }
  }
  return value.structuredClone(js);
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setTimeoutInternal(
    jsg::Function<void()> function, double msDelay) {
  auto timeoutId = IoContext::current().setTimeoutImpl(timeoutIdGenerator,
      /* repeat */ false, kj::mv(function), msDelay);
  return timeoutId.toNumber();
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setTimeout(jsg::Lock& js,
    jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
    jsg::Optional<double> msDelay,
    jsg::Arguments<jsg::Value> args) {
  function.setReceiver(js.v8Ref<v8::Value>(js.v8Context()->Global()));
  auto fn = [function = kj::mv(function), args = kj::mv(args),
                context = jsg::AsyncContextFrame::currentRef(js)](jsg::Lock& js) mutable {
    jsg::AsyncContextFrame::Scope scope(js, context);
    function(js, kj::mv(args));
  };
  auto timeoutId = IoContext::current().setTimeoutImpl(timeoutIdGenerator,
      /* repeat */ false, [function = kj::mv(fn)](jsg::Lock& js) mutable { function(js); },
      msDelay.orDefault(0));
  return timeoutId.toNumber();
}

void ServiceWorkerGlobalScope::clearTimeout(jsg::Lock& js, kj::Maybe<jsg::JsNumber> timeoutId) {
  KJ_IF_SOME(rawId, timeoutId) {
    // Browsers does not throw an error when "unsafe" integers are passed to the clearTimeout method.
    // Let's make sure we ignore those values, just like browsers and other runtimes.
    KJ_IF_SOME(id, rawId.toSafeInteger(js)) {
      IoContext::current().clearTimeoutImpl(TimeoutId::fromNumber(id));
    }
  }
}

TimeoutId::NumberType ServiceWorkerGlobalScope::setInterval(jsg::Lock& js,
    jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
    jsg::Optional<double> msDelay,
    jsg::Arguments<jsg::Value> args) {
  function.setReceiver(js.v8Ref<v8::Value>(js.v8Context()->Global()));
  auto fn = [function = kj::mv(function), args = kj::mv(args),
                context = jsg::AsyncContextFrame::currentRef(js)](jsg::Lock& js) mutable {
    jsg::AsyncContextFrame::Scope scope(js, context);
    // Because the fn is called multiple times, we will clone the args on each call.
    auto argv = KJ_MAP(i, args) { return i.addRef(js); };
    function(js, jsg::Arguments(kj::mv(argv)));
  };
  auto timeoutId = IoContext::current().setTimeoutImpl(timeoutIdGenerator,
      /* repeat */ true, [function = kj::mv(fn)](jsg::Lock& js) mutable { function(js); },
      msDelay.orDefault(0));
  return timeoutId.toNumber();
}

void ServiceWorkerGlobalScope::clearInterval(jsg::Lock& js, kj::Maybe<jsg::JsNumber> timeoutId) {
  clearTimeout(js, kj::mv(timeoutId));
}

jsg::Ref<Crypto> ServiceWorkerGlobalScope::getCrypto(jsg::Lock& js) {
  return js.alloc<Crypto>(js);
}

jsg::Ref<CacheStorage> ServiceWorkerGlobalScope::getCaches(jsg::Lock& js) {
  return js.alloc<CacheStorage>(js);
}

jsg::Promise<jsg::Ref<Response>> ServiceWorkerGlobalScope::fetch(jsg::Lock& js,
    kj::OneOf<jsg::Ref<Request>, kj::String> requestOrUrl,
    jsg::Optional<Request::Initializer> requestInit) {
  return fetchImpl(js, kj::none, kj::mv(requestOrUrl), kj::mv(requestInit));
}

void ServiceWorkerGlobalScope::reportError(jsg::Lock& js, jsg::JsValue error) {
  // Per the spec, we are going to first emit an error event on the global object.
  // If that event is not prevented, we will log the error to the console. Note
  // that we do not throw the error at all.
  const auto logError = [&](const jsg::JsValue& error) {
    // This helper must not throw: it is reached from dispatch paths with a no-throw contract
    // (a REPORT-policy dispatch, and the re-entrancy branch below). Reading `stack` can run
    // arbitrary user code — a getter or proxy trap — so a failure there falls back to the
    // generic logging, which is side-effect-free (ToDetailString; no user code).
    JSG_TRY(js) {
      // If the value is an object that has a stack property, log that so we get
      // the stack trace if it is an exception.
      KJ_IF_SOME(obj, error.tryCast<jsg::JsObject>()) {
        auto stack = obj.get(js, "stack"_kj);
        if (!stack.isUndefined()) {
          js.reportError(stack);
          return;
        }
      }
      // Otherwise just log the stringified value generically.
      js.reportError(error);
    }
    JSG_CATCH(exception KJ_UNUSED) {
      // Getting the stack property can throw an error if the accessor is
      // overridden by user code, etc. We don't want to propagate that error
      // because it violates the no-throw contract of this function, but we
      // don't want to just swallow it either. Let's log so we can at least
      // have a record of it happening at all. We dont want to log every case
      // or spam sentry, so let's log periodically with NOSENTRY.
      LOG_PERIODICALLY(
          WARNING, "NOSENTRY Error while reporting error to console", exception.getHandle(js));
      js.reportError(error);
    };
  };

  // Per HTML's "report an exception" re-entrancy guard (the global's "in error reporting
  // mode" flag): an exception reported while the 'error' event is being dispatched — e.g.
  // an 'error' listener that itself throws, which the REPORT dispatch policy routes right
  // back here — skips the event and goes straight to the console. Without this, a throwing
  // 'error' listener would either propagate out of whatever REPORT dispatch triggered the
  // report (violating its no-throw contract) or recurse indefinitely.
  if (inErrorReportingMode) {
    logError(error);
    return;
  }
  inErrorReportingMode = true;
  KJ_DEFER(inErrorReportingMode = false);

  // Technically speaking, the jsg::checks below can also trigger a throw, but
  // these aren't triggering user code so it's unlikely unless we're in a fatal
  // state. Just allow the error to propagate in these cases.
  auto message = v8::Exception::CreateMessage(js.v8Isolate, error);
  auto event = js.alloc<ErrorEvent>(ErrorEvent::ErrorEventInit{.message = kj::str(message->Get()),
    .filename = kj::str(message->GetScriptResourceName()),
    .lineno = jsg::check(message->GetLineNumber(js.v8Context())),
    .colno = jsg::check(message->GetStartColumn(js.v8Context())),
    .error = jsg::JsRef(js, error)});
  if (dispatchEventImpl(
          js, kj::mv(event), effectiveExceptionPolicy(js, DispatchExceptionPolicy::REPORT))
          .result) {
    logError(error);
  }
}

jsg::JsValue ServiceWorkerGlobalScope::getBuffer(jsg::Lock& js) {
  KJ_IF_SOME(p, bufferValue) {
    return p.getHandle(js);
  }
  constexpr auto kSpecifier = "node:buffer"_kj;
  KJ_IF_SOME(module, js.resolveModule(kSpecifier)) {
    KJ_IF_SOME(p, bufferValue) {
      // There's a chance that resolving the module caused side-effects
      // that set the bufferValue, we let's check again.
      return p.getHandle(js);
    }
    // When requireReturnsDefaultExport flag is enabled, resolveModule returns the
    // default export directly. Otherwise it returns the module namespace.
    auto obj = module.has(js, "default"_kj)
        ? KJ_ASSERT_NONNULL(module.get(js, "default"_kj).tryCast<jsg::JsObject>())
        : module;
    auto buffer = obj.get(js, "Buffer"_kj);
    JSG_REQUIRE(buffer.isFunction(), TypeError, "Invalid node:buffer implementation");
    bufferValue = jsg::JsRef(js, buffer);
    return buffer;
  } else {
    // If we are unable to resolve the node:buffer module here, it likely
    // means that we don't actually have a module registry installed. Just
    // return undefined in this case.
    bufferValue = jsg::JsRef(js, js.undefined());
    return js.undefined();
  }
}

void ServiceWorkerGlobalScope::setProcess(jsg::Lock& js, jsg::JsValue newProcess) {
  processValue = jsg::JsRef(js, newProcess);
}

void ServiceWorkerGlobalScope::setBuffer(jsg::Lock& js, jsg::JsValue newBuffer) {
  bufferValue = jsg::JsRef(js, newBuffer);
}

jsg::JsValue ServiceWorkerGlobalScope::getProcess(jsg::Lock& js) {
  KJ_IF_SOME(p, processValue) {
    return p.getHandle(js);
  }
  // Handle process module redirection based on enable_nodejs_process_v2 flag
  auto specifier = ([&]() -> kj::StringPtr {
    if (FeatureFlags::get(js).getEnableNodeJsProcessV2()) {
      return "node-internal:public_process"_kj;
    } else {
      return "node-internal:legacy_process"_kj;
    }
  })();

  KJ_IF_SOME(module, js.resolveInternalModule(specifier)) {
    KJ_IF_SOME(p, processValue) {
      // There's a chance that resolving the module caused side-effects
      // that set the processValue, we let's check again.
      return p.getHandle(js);
    }
    // When requireReturnsDefaultExport flag is enabled, resolveInternalModule returns the
    // default export directly. Otherwise it returns the module namespace.
    auto def = module.has(js, "default"_kj) ? module.get(js, "default"_kj) : jsg::JsValue(module);
    JSG_REQUIRE(def.isObject(), TypeError, "Invalid node:process implementation");
    processValue = jsg::JsRef(js, def);
    return def;
  } else {
    // If we are unable to resolve the internal module here, it likely
    // means that we don't actually have a module registry installed. Just
    // return undefined in this case.
    processValue = jsg::JsRef(js, js.undefined());
    return js.undefined();
  }
}

jsg::Ref<StorageManager> Navigator::getStorage(jsg::Lock& js) {
  return js.alloc<StorageManager>();
}

bool Navigator::sendBeacon(jsg::Lock& js, kj::String url, jsg::Optional<Body::Initializer> body) {
  KJ_IF_SOME(context, IoContext::tryCurrent()) {
    auto v8Context = js.v8Context();
    auto& global =
        jsg::extractInternalPointer<ServiceWorkerGlobalScope, true>(v8Context, v8Context->Global());
    auto promise = global.fetch(js, kj::mv(url),
        Request::InitializerDict{
          .method = kj::str("POST"),
          .body = kj::mv(body),
        });

    context.addWaitUntil(context.awaitJs(js, kj::mv(promise)).ignoreResult());
    return true;
  }

  // We cannot schedule a beacon to be sent outside of a request context.
  return false;
}

// ======================================================================================

Immediate::Immediate(IoContext& context, TimeoutId timeoutId)
    : ioContext(context.addObject(context)),
      timeoutId(timeoutId) {}

void Immediate::dispose() {
  // IoPtr will throw if the IoContext is no longer valid, which is fine - accessing the Immediate
  // from the wrong IoContext should throw an error, just as accessing the Immediate when it's
  // owning IoContext is gone.
  ioContext->clearTimeoutImpl(timeoutId);
}

jsg::Ref<Immediate> ServiceWorkerGlobalScope::setImmediate(jsg::Lock& js,
    jsg::Function<void(jsg::Arguments<jsg::Value>)> function,
    jsg::Arguments<jsg::Value> args) {

  // This is an approximation of the Node.js setImmediate global API.
  // We implement it in terms of setting a 0 ms timeout. This is not
  // how Node.js does it so there will be some edge cases where the
  // timing of the callback will differ relative to the equivalent
  // operations in Node.js. For the vast majority of cases, users
  // really shouldn't be able to tell a difference. It would likely
  // only be somewhat pathological edge cases that could be affected
  // by the differences. Unfortunately, changing this later to match
  // Node.js would likely be a breaking change for some users that
  // would require a compat flag... but that's OK for now?

  auto& context = IoContext::current();
  auto fn = [function = kj::mv(function), args = kj::mv(args),
                context = jsg::AsyncContextFrame::currentRef(js)](jsg::Lock& js) mutable {
    jsg::AsyncContextFrame::Scope scope(js, context);
    function(js, kj::mv(args));
  };
  auto timeoutId = context.setTimeoutImpl(timeoutIdGenerator,
      /* repeat */ false, [function = kj::mv(fn)](jsg::Lock& js) mutable { function(js); }, 0);
  return js.alloc<Immediate>(context, timeoutId);
}

void ServiceWorkerGlobalScope::clearImmediate(kj::Maybe<jsg::Ref<Immediate>> maybeImmediate) {
  KJ_IF_SOME(immediate, maybeImmediate) {
    immediate->dispose();
  }
}

jsg::JsObject Cloudflare::getCompatibilityFlags(jsg::Lock& js) {
  auto flags = FeatureFlags::get(js);
  auto obj = js.objNoProto();
  auto dynamic = capnp::toDynamic(flags);
  auto schema = dynamic.getSchema();

  for (auto field: schema.getFields()) {
    auto annotations = field.getProto().getAnnotations();

    // Note that disable flags are not exposed.
    for (auto annotation: annotations) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        obj.setReadOnly(
            js, annotation.getValue().getText(), js.boolean(dynamic.get(field).as<bool>()));
      }
    }
  }

  obj.seal(js);
  return obj;
}

}  // namespace workerd::api
