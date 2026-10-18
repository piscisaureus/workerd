// The workers of memory-cache-v2-arena-test.sh. Each reads keys from its own memory cache
// binding with a fallback that names the worker which produced the value, so a response says
// both which worker served the request and whose fallback filled the entry. "owner" and
// "sharer" are configured with one cache id and "solo-a" and "solo-b" with none, which is the
// difference the script is about.
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === '/read') {
      const key = url.searchParams.get('key') ?? 'k';
      const value = await env.CACHE.read(key, async (k) => ({
        value: `${k} from ${env.NAME}`,
      }));
      return new Response(`${env.NAME} read: ${value}\n`);
    }
    return new Response(`${env.NAME} ok\n`);
  },
};
