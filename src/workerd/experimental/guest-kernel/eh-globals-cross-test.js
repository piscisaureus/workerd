// Fixture for eh-globals-cross-test.sh. The probe paths are dispatched host-side in
// maybeForceGuestFaultForTest (src/workerd/api/global-scope.c++); this handler only has to return
// so the request completes. /__gk_fallback_poke drives a real module resolution that the module
// fallback service cannot satisfy, so the real FallbackServiceClient thread throws and catches a
// C++ exception on a failed fetch -- the thread whose exception runtime the steer aims at.
export default {
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === '/__gk_fallback_poke') {
      try {
        await import('gk-probe-missing-module');
      } catch (e) {
        // Expected: the fallback service fails to fetch the module.
      }
    }
    return new Response('victim ok\n');
  },
};
