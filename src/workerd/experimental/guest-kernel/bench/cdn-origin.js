const body = 'x'.repeat(4096);

export default {
  fetch() {
    return new Response(body, { headers: { 'cache-control': 'max-age=60' } });
  },
};
