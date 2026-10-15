// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "hostcall.h"

#ifdef WORKERD_HAS_GUEST_KERNEL
#include <workerd/experimental/guest-kernel/gk.h>
#endif

#include <kj/debug.h>

#include <cstring>

namespace workerd::hostcall {

namespace {

#ifdef WORKERD_HAS_GUEST_KERNEL
static_assert(BYTES == GK_HOSTCALL_BYTES && STATE == GK_HOSTCALL_STATE);
static_assert(sizeof(Frame) == sizeof(gk_hostcall_frame));
static_assert(offsetof(Frame, args) == offsetof(gk_hostcall_frame, args));
static_assert(offsetof(Frame, inLen) == offsetof(gk_hostcall_frame, in_len));
static_assert(offsetof(Frame, outLen) == offsetof(gk_hostcall_frame, out_len));
static_assert(offsetof(Frame, buf) == offsetof(gk_hostcall_frame, buf));
static_assert(offsetof(Frame, state) == offsetof(gk_hostcall_frame, state));

// Whether calls go through the guest kernel: set by install() once a handler is registered
// with an initialized guest kernel. Read inside turns, so it is a plain static rather than a
// query of the guest kernel's control block, which ring 3 cannot read. A turn that flips it off
// takes the direct path at ring 3 and faults on the host memory the handler reaches.
bool viaGuestKernel = false;

// The guest kernel calls this with the handler from its own table as the context.
long shim(void* ctx, gk_hostcall_frame* frame) {
  return reinterpret_cast<Handler>(ctx)(*reinterpret_cast<Frame*>(frame));
}
#endif

}  // namespace

long call(const Entry& entry, Frame& frame) {
  KJ_REQUIRE(frame.inLen <= BYTES, "hostcall arguments exceed the frame", frame.inLen);
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (viaGuestKernel) {
    gk_hostcall_frame* region = gk_hostcall_region();
    KJ_REQUIRE(region != nullptr, "guest-kernel: this thread has no hostcall region");
    memcpy(region->args, frame.args, sizeof(frame.args));
    region->in_len = frame.inLen;
    memcpy(region->buf, frame.buf, frame.inLen);
    long result = gk_hostcall(entry.index);
    // The region is shared memory: out_len is read once and clamped, as the guest kernel
    // bounds it but another guest thread could have rewritten it since.
    uint64_t outLen = region->out_len;
    if (outLen > BYTES) outLen = 0;
    frame.outLen = outLen;
    memcpy(frame.buf, region->buf, outLen);
    memset(region->buf, 0, kj::max(frame.inLen, outLen));
    memset(region->args, 0, sizeof(region->args));
    region->in_len = region->out_len = 0;
    return result;
  }
#endif
  return entry.handler(frame);
}

void install(const Entry& entry) {
#ifdef WORKERD_HAS_GUEST_KERNEL
  if (gk_initialized()) {
    int result = gk_hostcall_register(entry.index, &shim, reinterpret_cast<void*>(entry.handler));
    KJ_REQUIRE(result == 0, "guest-kernel: gk_hostcall_register failed", entry.index, result);
    viaGuestKernel = true;
  }
#endif
}

}  // namespace workerd::hostcall
