// The victim worker of fault-timeout-test.sh. On /__gk_fault_timeout the host's test hook has
// already set a zero-delay timeout on this request whose callback faults inside the guest (see
// maybeForceGuestFaultForTest in src/workerd/api/global-scope.c++); this handler only keeps the
// request alive until that timer has fired, so the fault lands in a timer turn of a live
// request, whose IoContext is then torn down with the timeout state the fault abandoned.
export default {
  async fetch(req) {
    if (new URL(req.url).pathname === '/__gk_fault_timeout') {
      await new Promise((resolve) => setTimeout(resolve, 2000));
      return new Response('timer survived\n');
    }
    return new Response('victim ok\n');
  },
};
