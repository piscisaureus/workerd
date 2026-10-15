// Worker for disk-sqlite-test.sh and vfs-root-test.sh: a SQLite-backed Durable Object whose
// storage is on local disk, driven through request paths. `o` selects the object by name, `k`
// and `v` a key and value.
//
// /put stores k=v; /get returns k's value or "missing". /throw runs a statement that fails
// inside SQLite and reports the error it caught; /throw-uncaught runs it without catching, so the
// error leaves the turn as an uncaught exception.
export class Store {
  constructor(state, env) {
    this.sql = state.storage.sql;
  }
  async fetch(req) {
    const url = new URL(req.url);
    const k = url.searchParams.get('k') || 'k';
    const v = url.searchParams.get('v') || 'v';
    switch (url.pathname) {
      case '/put':
        this.sql.exec(
          'CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT)'
        );
        this.sql.exec('INSERT OR REPLACE INTO kv (k, v) VALUES (?, ?)', k, v);
        return new Response('ok');
      case '/get': {
        const rows = this.sql.exec('SELECT v FROM kv WHERE k = ?', k).toArray();
        return new Response(rows.length ? rows[0].v : 'missing');
      }
      case '/throw':
        try {
          this.sql.exec('SELECT * FROM no_such_table');
          return new Response('no error');
        } catch (e) {
          return new Response('caught: ' + e.message);
        }
      case '/throw-uncaught':
        this.sql.exec('SELECT * FROM no_such_table');
        return new Response('no error');
      default:
        return new Response('store ok');
    }
  }
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (url.pathname === '/' || url.pathname.startsWith('/__gk_')) {
      return new Response('victim ok\n');
    }
    const id = env.NS.idFromName(url.searchParams.get('o') || 'one');
    return env.NS.get(id).fetch(req);
  },
};
