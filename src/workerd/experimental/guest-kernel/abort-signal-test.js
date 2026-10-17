// Driven by abort-signal-test.sh: /fetch-abort starts a subrequest through a service binding
// with an AbortController signal, aborts it shortly after, and reports what the subrequest did
// and how many native registrations the signal was holding. Under the guest kernel the
// registration is made inside the turn and disarmed from the event loop, so a working one shows
// up as a prompt rejection rather than the five seconds the backend takes to answer.
export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname !== '/fetch-abort') {
      return new Response('fetcher ok\n');
    }
    const ac = new AbortController();
    const started = Date.now();
    const pending = env.BACK.fetch('http://back/slow', { signal: ac.signal });
    setTimeout(() => ac.abort(new Error('abort-signal-test')), 200);
    let outcome;
    try {
      await pending;
      outcome = 'resolved';
    } catch (e) {
      outcome = 'rejected:' + e.message;
    }
    const held = ac.signal.getNativeRegistrationCountForTest();
    return new Response(
      'outcome=' +
        outcome +
        ' elapsed=' +
        (Date.now() - started) +
        ' held=' +
        held +
        '\n'
    );
  },
};
