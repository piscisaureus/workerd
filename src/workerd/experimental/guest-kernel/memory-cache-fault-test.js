// The workers of memory-cache-fault-test.sh: both read keys from the shared memory cache with a
// fallback that names the worker which produced the value. On /__gk_fault_cache_fallback the
// host's test hook has armed the next fallback to fault inside the guest (see
// maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++), so the read below never
// settles for that request: its turn is abandoned and its IoContext torn down with the cache's
// in-progress entry for the key still outstanding.
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (
      url.pathname === '/read' ||
      url.pathname === '/slow' ||
      url.pathname === '/__gk_fault_cache_fallback'
    ) {
      // The fault path is matched on the URL's end, so it carries no query: its key is fixed.
      const key = url.searchParams.get('key') ?? 'stranded';
      // /slow holds its fallback open for a while, so that a co-tenant's read of the same key
      // finds it in flight and waits on it.
      const delay = url.pathname === '/slow' ? 500 : 0;
      const value = await env.CACHE.read(key, async (k) => {
        if (delay > 0) await scheduler.wait(delay);
        return { value: `${k} from ${env.NAME}` };
      });
      return new Response(`${env.NAME} read: ${value}\n`);
    }
    return new Response(`${env.NAME} ok\n`);
  },
};
