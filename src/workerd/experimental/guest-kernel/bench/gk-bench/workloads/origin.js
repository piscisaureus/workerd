// Stands in for a CDN origin: returns a body of BODY_BYTES bytes.
const BODY_BYTES = 65536;
const body = "x".repeat(BODY_BYTES);
export default {
  async fetch(request) {
    return new Response(body, {headers: {"content-type": "application/octet-stream"}});
  },
};
