// G8: the REST API satisfies its documented contract for valid, invalid, missing,
// conflicting and concurrent requests.
import { ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer, stopServer,
         http, waitFor } from './_lib.mjs';

const buildDir = ensureBuild('build');
const unit = runGtest('ApiTest.*:HttpParserTest.*:HttpServerTest.*', buildDir);
assertGtest(unit, 'api tests', 20);

// Contract checks against a real server process over real sockets.
const server = await startServer(['--workers', '4', '--db', ':memory:']);
const cases = [];
const record = (name, ok, detail) => {
  cases.push(name);
  check(ok, `${name}: ${detail}`);
};

// health
const health = await http(server.port, 'GET', '/health');
record('GET /health returns 200', health.status === 200, `got ${health.status}`);
record('health reports worker threads', health.json?.worker_threads === 4,
       `got ${health.json?.worker_threads}`);

// submit + read back
const created = await http(server.port, 'POST', '/jobs',
                           { type: 'echo', payload: { a: 1 }, priority: 2 });
record('POST /jobs returns 201', created.status === 201, `got ${created.status}: ${created.body}`);
const jobId = created.json?.job_id;
record('POST /jobs returns a job id', typeof jobId === 'string' && jobId.length > 0, 'missing');

await waitFor(async () => (await http(server.port, 'GET', `/jobs/${jobId}`)).json?.state === 'SUCCEEDED',
              10000);
const fetched = await http(server.port, 'GET', `/jobs/${jobId}`);
record('GET /jobs/{id} returns 200', fetched.status === 200, `got ${fetched.status}`);
record('the job reached SUCCEEDED', fetched.json?.state === 'SUCCEEDED', fetched.body);
for (const field of ['job_id', 'type', 'payload', 'priority', 'state', 'attempt', 'max_attempts',
                     'timeout_ms', 'created_at_ms', 'queue_wait_ms', 'exec_ms', 'result',
                     'error', 'workflow_id']) {
  record(`job object exposes ${field}`, field in (fetched.json || {}), 'absent');
}

// invalid requests
const invalid = [
  ['empty body', 'POST', '/jobs', '', 400],
  ['not json', 'POST', '/jobs', 'garbage', 400],
  ['array body', 'POST', '/jobs', '[1]', 400],
  ['missing type', 'POST', '/jobs', '{}', 400],
  ['unknown type', 'POST', '/jobs', '{"type":"nope"}', 400],
  ['bad max_attempts', 'POST', '/jobs', '{"type":"noop","max_attempts":0}', 400],
  ['negative timeout', 'POST', '/jobs', '{"type":"noop","timeout_ms":-1}', 400],
  ['bad state filter', 'GET', '/jobs?state=BOGUS', null, 400],
  ['bad limit', 'GET', '/jobs?limit=0', null, 400],
  ['missing job', 'GET', '/jobs/j_NOPE', null, 404],
  ['missing attempts', 'GET', '/jobs/j_NOPE/attempts', null, 404],
  ['cancel missing job', 'POST', '/jobs/j_NOPE/cancel', '', 404],
  ['unknown endpoint', 'GET', '/nope', null, 404],
  ['wrong method', 'POST', '/health', '', 405],
  ['missing workflow', 'GET', '/workflows/w_NOPE', null, 404],
  ['workflow cycle', 'POST', '/workflows',
   '{"nodes":[{"name":"a","type":"noop"},{"name":"b","type":"noop"}],' +
   '"edges":[{"from":"a","to":"b"},{"from":"b","to":"a"}]}', 400],
];
for (const [name, method, p, body, want] of invalid) {
  const res = await http(server.port, method, p, body);
  record(`${name} -> ${want}`, res.status === want, `got ${res.status}: ${res.body}`);
  if (want >= 400) {
    record(`${name} returns an error message`, typeof res.json?.error === 'string',
           `body was ${res.body}`);
  }
}

// cancellation conflict semantics
const toCancel = await http(server.port, 'POST', '/jobs', { type: 'sleep', payload: { ms: 3000 } });
const cancelId = toCancel.json.job_id;
const cancelled = await http(server.port, 'POST', `/jobs/${cancelId}/cancel`, '');
record('cancel returns 200', cancelled.status === 200, `got ${cancelled.status}`);
await waitFor(async () => {
  const s = (await http(server.port, 'GET', `/jobs/${cancelId}`)).json?.state;
  return s === 'CANCELLED';
}, 10000);
const conflict = await http(server.port, 'POST', `/jobs/${cancelId}/cancel`, '');
record('cancelling a terminal job returns 409', conflict.status === 409, `got ${conflict.status}`);

// listing
const list = await http(server.port, 'GET', '/jobs?limit=5');
record('GET /jobs returns 200', list.status === 200, `got ${list.status}`);
record('listing is paginated', Array.isArray(list.json?.jobs) && list.json.jobs.length <= 5,
       `returned ${list.json?.jobs?.length}`);
record('listing reports a total', typeof list.json?.total === 'number', 'missing total');

// metrics
const metrics = await http(server.port, 'GET', '/metrics');
record('GET /metrics returns 200', metrics.status === 200, `got ${metrics.status}`);
record('metrics expose counters', typeof metrics.json?.counters?.jobs_submitted === 'number',
       'missing counters');

// concurrent submissions
const concurrent = await Promise.all(
    Array.from({ length: 120 }, (_, i) =>
        http(server.port, 'POST', '/jobs', { type: 'noop', payload: { i } })));
const okCount = concurrent.filter((r) => r.status === 201).length;
const uniqueIds = new Set(concurrent.map((r) => r.json?.job_id).filter(Boolean));
record('all concurrent submissions succeeded', okCount === 120, `${okCount}/120`);
record('all concurrent submissions got unique ids', uniqueIds.size === 120,
       `${uniqueIds.size} unique`);

await stopServer(server);

finish('G8', [
  `unit: ${unit.passed}/${unit.total} api, parser and server tests passed`,
  `live contract: ${cases.length} assertions against a real server process`,
  `concurrency: 120 simultaneous submissions, ${okCount} accepted with ${uniqueIds.size} unique ids`,
]);
