// G14: a coordinator plus multiple separate worker PROCESSES completes every submitted
// job, with the work actually spread across processes rather than done by one of them.
import path from 'node:path';
import { ROOT, BUILD, ensureBuild, runGtest, assertGtest, Proc, waitForLine, check, checkEq,
         finish, startServer, stopServer, http, waitFor, sleep } from './_lib.mjs';

const buildDir = ensureBuild('build');
const unit = runGtest('DistributedTest.*', buildDir);
assertGtest(unit, 'distributed tests', 8);

// The coordinator runs with ZERO embedded workers, so any job that completes was executed
// by a separate OS process.
const server = await startServer(['--workers', '0', '--db', ':memory:', '--http-threads', '12']);

const WORKERS = 3;
const THREADS_PER_WORKER = 2;
const workers = [];
for (let i = 0; i < WORKERS; i++) {
  const p = new Proc(path.join(buildDir, 'jobengine-worker'),
                     ['--host', '127.0.0.1', '--port', String(server.port),
                      '--name', `gate-worker-${i}`, '--threads', String(THREADS_PER_WORKER),
                      '--poll-wait-ms', '250', '--log-level', 'warn']);
  const ready = await waitForLine(p, /READY worker_id=(\S+)/, 15000);
  check(ready !== null, `worker ${i} did not register: ${p.output.slice(0, 1000)}`);
  workers.push({ proc: p, id: ready ? ready[1] : null });
}

const alive = await waitFor(async () =>
    (await http(server.port, 'GET', '/health')).json?.remote_workers_alive === WORKERS, 15000);
check(alive, 'the coordinator did not see all worker processes as alive');

const JOBS = 400;
const ids = [];
for (let i = 0; i < JOBS; i++) {
  const res = await http(server.port, 'POST', '/jobs',
                         { type: 'sleep', payload: { ms: 5 }, priority: i % 3 });
  checkEq(res.status, 201, `submission ${i} failed: ${res.body}`);
  ids.push(res.json.job_id);
}

const drained = await waitFor(async () =>
    (await http(server.port, 'GET', '/health')).json?.outstanding === 0, 120000);
check(drained, 'the coordinator never drained with only remote workers available');

// Every job must have succeeded, and each must name the worker that ran it.
const owners = new Map();
let notSucceeded = 0;
let noOwner = 0;
for (const id of ids) {
  const j = (await http(server.port, 'GET', `/jobs/${id}`)).json;
  if (j?.state !== 'SUCCEEDED') notSucceeded++;
  if (!j?.lease_owner) noOwner++;
  else owners.set(j.lease_owner, (owners.get(j.lease_owner) || 0) + 1);
}
checkEq(notSucceeded, 0, `${notSucceeded} jobs did not succeed`);
checkEq(noOwner, 0, `${noOwner} jobs do not record which worker executed them`);
checkEq(owners.size, WORKERS,
        `work reached ${owners.size} of ${WORKERS} worker processes, so it was not distributed`);
for (const [id, n] of owners) {
  check(n > 0, `worker ${id} executed no jobs`);
}

const counters = (await http(server.port, 'GET', '/metrics')).json.counters;
checkEq(counters.jobs_succeeded, JOBS, 'the success counter disagrees with the job states');
checkEq(counters.workers_registered, WORKERS, 'the registration counter is wrong');
checkEq(counters.stale_results_rejected, 0,
        'results were rejected as stale in a run with no failures');

// Shut the workers down cleanly and confirm each reports the work it did.
let reportedCompleted = 0;
for (const w of workers) {
  w.proc.kill('SIGTERM');
  const exited = await w.proc.waitExit(20000);
  check(exited, `worker ${w.id} did not exit on SIGTERM`);
  const m = /STOPPED completed=(\d+) failed=(\d+) rejected=(\d+)/.exec(w.proc.output);
  check(m !== null, `worker ${w.id} did not report its totals`);
  if (m) {
    reportedCompleted += Number(m[1]);
    checkEq(Number(m[3]), 0, `worker ${w.id} had ${m[3]} results rejected`);
  }
  if (!exited) w.proc.kill('SIGKILL');
}
checkEq(reportedCompleted, JOBS,
        'the totals reported by the worker processes do not add up to the jobs submitted');

await stopServer(server);

const distribution = [...owners.entries()].map(([k, v]) => `${k}:${v}`).join(' ');
finish('G14', [
  `unit: ${unit.passed}/${unit.total} distributed protocol tests passed`,
  `topology: 1 coordinator process with 0 embedded workers, ${WORKERS} worker processes ` +
  `x ${THREADS_PER_WORKER} threads = ${WORKERS * THREADS_PER_WORKER} concurrent job slots`,
  `${JOBS} jobs submitted over HTTP, all SUCCEEDED, 0 stale results`,
  `per-process execution counts: ${distribution}`,
]);
