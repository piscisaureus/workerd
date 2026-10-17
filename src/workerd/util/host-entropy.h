// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/array.h>
#include <kj/function.h>

namespace workerd {

// Fills `output` with cryptographically random bytes for a host-side secret: a key, a nonce, or
// anything else whose value the guest must be unable to learn or choose.
//
// This is the host-privileged counterpart of getEntropy() (entropy.h). getEntropy() keeps its
// unconsumed CSPRNG output in a thread-local buffer, which is shared runtime memory that a JS
// turn at guest ring 3 can read and write, so the bytes it will return next are the guest's to
// read, or to choose. That is fine for the bytes a turn asks for itself, which are handed to the
// isolate anyway, and wrong for a secret the host draws on the same thread between turns.
//
// getHostEntropy() keeps nothing the guest can reach. Its buffer, cursor and lock live in one
// host-memory block (hostMemory::allocateSlot, host-memory.h), named by a slot handle rather
// than a pointer; with the guest kernel active no guest access reaches them, and without it the
// block is ordinary heap memory and the function behaves the same. The bytes come from the
// operating system's generator (RAND_get_system_entropy_for_custom_prng, getrandom(2) on Linux)
// rather than from BoringSSL's in-process DRBG, whose per-thread state is itself in the shared
// heap and, on a CPU without a fast RDRAND, determines the DRBG's output outright. So the only
// generator state this path depends on is the kernel's.
//
// Host-side only: a draw takes a lock and touches host memory, so with the guest kernel active a
// call from inside a turn faults the turn rather than returning bytes. Draws are serialized
// process-wide; they are rare (a token key at startup, a nonce per minted token) and the
// simplicity of one buffer under one lock is worth more than throughput. The buffer is small and
// refilled with one call to the system generator; a draw larger than the buffer is served by
// repeated refills. Consumed bytes are scrubbed. A draw after a fork discards whatever the
// parent left unconsumed, so a child never repeats the parent's bytes.
void getHostEntropy(kj::ArrayPtr<kj::byte> output);

namespace hostEntropy {

// The buffer's size: one refill is one call to the system generator, which caps a call at 256
// bytes.
constexpr size_t BUFFER_SIZE = 256;

// Test-only hooks. setSourceForTest() replaces the system generator for refills with `source`
// and clearSourceForTest() restores it, so a test can predict the bytes a draw returns and check
// the sequence across a refill; discardForTest() drops the unconsumed bytes, so the next draw
// refills.
void setSourceForTest(kj::Function<void(kj::ArrayPtr<kj::byte>)> source);
void clearSourceForTest();
void discardForTest();

}  // namespace hostEntropy

}  // namespace workerd
