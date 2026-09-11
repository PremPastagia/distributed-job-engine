// G18: the documented HTTP routes are exactly the routes the server implements. Neither a
// documented-but-missing endpoint nor an undocumented one is allowed.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, ensureBuild, startServer, stopServer, http, check, checkEq, finish } from './_lib.mjs';

ensureBuild('build');
const server = await startServer(['--workers', '1', '--db', ':memory:']);
const res = await http(server.port, 'GET', '/routes');
checkEq(res.status, 200, `GET /routes returned ${res.status}`);
const implemented = new Set(res.json?.routes || []);
check(implemented.size > 8, `the server reported only ${implemented.size} routes`);

// Every implemented route must actually respond (not 404 "no such endpoint"), so the table
// cannot advertise a route that is not wired up.
let unwired = 0;
for (const route of implemented) {
  const [method, pattern] = route.split(' ');
  const concrete = pattern.replace('{id}', 'probe_id_that_does_not_exist');
  const r = await http(server.port, method, concrete, method === 'POST' ? '{}' : null);
  if (r.status === 404 && /no such endpoint/.test(r.body || '')) unwired++;
}
checkEq(unwired, 0, `${unwired} advertised routes are not actually routed`);
await stopServer(server);

// Parse the documented table out of docs/ARCHITECTURE.md.
const doc = fs.readFileSync(path.join(ROOT, 'docs', 'ARCHITECTURE.md'), 'utf8');
const documented = new Set();
for (const line of doc.split('\n')) {
  const m = /^\|\s*(GET|POST|PUT|DELETE|PATCH)\s*\|\s*`([^`]+)`/.exec(line.trim());
  if (m) documented.add(`${m[1]} ${m[2].replace(/\\/g, '')}`);
}
check(documented.size > 8, `only ${documented.size} routes were parsed from ARCHITECTURE.md`);

const missing = [...documented].filter((r) => !implemented.has(r));
const undocumented = [...implemented].filter((r) => !documented.has(r));
checkEq(missing.length, 0, `documented but not implemented: ${missing.join(', ')}`);
checkEq(undocumented.length, 0, `implemented but not documented: ${undocumented.join(', ')}`);

finish('G18', [
  `server advertises ${implemented.size} routes, all of them reachable`,
  `docs/ARCHITECTURE.md documents ${documented.size} routes`,
  `no documented-but-missing and no undocumented endpoints`,
]);
