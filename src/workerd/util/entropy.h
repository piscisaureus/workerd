// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>

namespace workerd {

// Fills `output` with cryptographically-random bytes, from a per-thread buffer of CSPRNG output.
//
// The buffer is thread-local storage, which under guest-kernel isolation is shared runtime
// memory that a JS turn at guest ring 3 can read and write: the bytes the next draw on that
// thread returns are the guest's to read, or to choose. That is acceptable for the draws a turn
// makes for itself (crypto.getRandomValues, multipart boundaries, retry seeds), whose bytes go to
// the isolate anyway, and it is what makes this function unfit for a host-side secret such as a
// key or a nonce: those are drawn with getHostEntropy() (host-entropy.h), whose buffer is in host
// memory.
void getEntropy(kj::ArrayPtr<kj::byte> output);

// Test-only hooks for the guest kernel's end-to-end check (experimental/guest-kernel/
// host-memory-test.sh), which show what a turn's write to this thread's buffer does to a later
// draw on the same thread. poisonEntropyBufferForTest() overwrites the whole of the calling
// thread's buffer with `pattern` and marks all of it unconsumed, so the thread's next draws
// return the pattern; discardEntropyBufferForTest() drops the thread's unconsumed bytes, so the
// next draw refills from the CSPRNG.
void poisonEntropyBufferForTest(kj::byte pattern);
void discardEntropyBufferForTest();

}  // namespace workerd
