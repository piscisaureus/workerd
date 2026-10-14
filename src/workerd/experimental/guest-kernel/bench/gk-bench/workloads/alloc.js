// Allocates a fresh 4 MiB ArrayBuffer per request and touches it. In stock
// workerd each buffer is a fresh mmap that the kernel demand-zeroes page by
// page and then unmaps, which is why this workload is faster under the guest
// kernel: the arena satisfies the allocation from memory that is already
// mapped.
export default {
  async fetch(request) {
    const buf = new Uint8Array(4 * 1024 * 1024);
    for (let i = 0; i < buf.length; i += 4096) buf[i] = i & 0xff;
    let sum = 0;
    for (let i = 0; i < buf.length; i += 65536) sum += buf[i];
    return new Response(String(sum) + "\n");
  },
};
