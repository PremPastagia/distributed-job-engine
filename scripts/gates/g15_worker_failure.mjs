// G15: killing a worker mid-job causes lease expiry, reassignment and eventual completion,
// and a superseded worker's late result is rejected by its fencing token.
import path from 'node:path';
import { ensureBuild, Proc, waitForLine, check, checkEq, finish, startServer, stopServer,
         http, waitFor, sleep } from './_lib.mjs';

const buildDir = ensureBuild('build');

const LEASE_MS = 1200;
const HEARTBEAT_TIMEOUT_MS = 800;
const JOB_MS = 2500;
const JOBS = 4;

const server = await startServer([
  '--workers', '0', '--db', ':memory:',
  '--lease-ms', String(LEASE_MS),
  '--heartbeat-timeout-ms', String(HEARTBEAT_TIMEOUT_MS),
  '--watchdog-ms', '25',
]);

// --- part 1: a worker process is killed mid-job -------------------------------------------
const victim = new Proc(path.join(buildDir, 'jobengine-worker'),
                        ['--host', '127.0.0.1', '--port', String(server.port),
                         '--name', 'victim', '--threads', String(JOBS),
                         '--poll-wait-ms', '200', '--log-level', 'warn']);
const victimReady = await waitForLine(victim, /READY worker_id=(\S+)/, 15000);
check(victimReady !== null, 'the victim worker never registered');
const victimId = victimReady ? victimReady[1] : null;

const ids = [];
for (let i = 0; i < JOBS; i++) {
  const res = await http(server.port, 'POST', '/jobs',
                         { type: 'sleep', payload: { ms: JOB_MS }, max_attempts: 4 });
  checkEq(res.status, 201, `submission ${i} failed`);
  ids.push(res.json.job_id);
}

const claimed = await waitFor(async () => {
  let running = 0;
  for (const id of ids) {
    if ((await http(server.port, 'GET', `/jobs/${id}`)).json?.state === 'RUNNING') running++;
  }
  return running === JOBS;
}, 20000);
check(claimed, 'the worker never took all the jobs, so there was nothing to lose');

// SIGKILL: no clean shutdown, no final report - exactly what a crashed machine looks like.
const tKill = Date.now();
victim.kill('SIGKILL');
await victim.waitExit(10000);
check(victim.exited, 'the victim worker did not die');

// The coordinator must notice, mark it dead, and put its work back.
const noticed = await waitFor(async () =>
    (await http(server.port, 'GET', '/health')).json?.remote_workers_alive === 0, 20000);
check(noticed, 'the coordinator never noticed the dead worker');
const detectionMs = Date.now() - tKill;

const requeued = await waitFor(async () => {
  let back = 0;
  for (const id of ids) {
    const s = (await http(server.port, 'GET', `/jobs/${id}`)).json?.state;
    if (s === 'QUEUED' || s === 'SUCCEEDED') back++;
  }
  return back === JOBS;
}, 20000);
check(requeued, 'the dead worker\'s jobs were never returned to the queue');
const requeueMs = Date.now() - tKill;

// A replacement worker finishes the work.
const rescuer = new Proc(path.join(buildDir, 'jobengine-worker'),
                         ['--host', '127.0.0.1', '--port', String(server.port),
                          '--name', 'rescuer', '--threads', String(JOBS),
                          '--poll-wait-ms', '200', '--log-level', 'warn']);
const rescuerReady = await waitForLine(rescuer, /READY worker_id=(\S+)/, 15000);
check(rescuerReady !== null, 'the replacement worker never registered');

const finished = await waitFor(async () =>
    (await http(server.port, 'GET', '/health')).json?.outstanding === 0, 60000);
check(finished, 'the reassigned jobs never completed');
const recoveryMs = Date.now() - tKill;

let succeeded = 0;
const owners = new Set();
for (const id of ids) {
  const j = (await http(server.port, 'GET', `/jobs/${id}`)).json;
  if (j?.state === 'SUCCEEDED') succeeded++;
  if (j?.lease_owner) owners.add(j.lease_owner);
  check((j?.attempt || 0) >= 2,
        `job ${id} shows ${j?.attempt} attempts: the interrupted attempt was not counted`);
}
checkEq(succeeded, JOBS, `${JOBS - succeeded} jobs did not survive the worker's death`);
check(!owners.has(victimId), 'a job is still attributed to the killed worker');

const counters = (await http(server.port, 'GET', '/metrics')).json.counters;
check(counters.workers_marked_dead >= 1, 'no worker was marked dead');
check(counters.leases_expired >= JOBS, `only ${counters.leases_expired} leases expired`);
check(counters.jobs_reassigned >= JOBS, `only ${counters.jobs_reassigned} jobs were reassigned`);

rescuer.kill('SIGTERM');
await rescuer.waitExit(15000);

// --- part 2: a superseded worker's late result is fenced out --------------------------------
// A worker claims a job, goes quiet long enough to lose the lease, then reports anyway.
const late = await http(server.port, 'POST', '/internal/workers/register',
                        { name: 'slowpoke', capacity: 1 });
const lateId = late.json.worker_id;
const fenceJob = await http(server.port, 'POST', '/jobs', { type: 'noop', max_attempts: 5 });
const fenceId = fenceJob.json.job_id;
const lease = await http(server.port, 'POST', '/internal/claim',
                         { worker_id: lateId, max_jobs: 1, wait_ms: 2000 });
checkEq(lease.json?.count, 1, 'the slow worker did not receive the job');
const staleToken = lease.json.jobs[0].lease_token;
const staleAttempt = lease.json.jobs[0].attempt;

const expired = await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${fenceId}`)).json?.state === 'QUEUED', 20000);
check(expired, 'the slow worker\'s lease never expired');

const lateResult = await http(server.port, 'POST', '/internal/complete', {
  worker_id: lateId, job_id: fenceId, attempt: staleAttempt,
  lease_token: staleToken, status: 'SUCCEEDED', result: { late: true }, error: '',
});
checkEq(lateResult.status, 200, 'the late report was not answered');
checkEq(lateResult.json?.accepted, false, 'a superseded worker\'s result was accepted');
checkEq(lateResult.json?.reason, 'stale_lease', `unexpected rejection reason: ${lateResult.body}`);
const fenceState = (await http(server.port, 'GET', `/jobs/${fenceId}`)).json;
check(fenceState.state !== 'SUCCEEDED',
      'the stale result was written to the job despite being rejected');

const finalCounters = (await http(server.port, 'GET', '/metrics')).json.counters;
check(finalCounters.stale_results_rejected >= 1, 'the stale result was not counted');

await stopServer(server);

finish('G15', [
  `topology: 1 coordinator, lease=${LEASE_MS}ms, heartbeat timeout=${HEARTBEAT_TIMEOUT_MS}ms`,
  `SIGKILLed a worker holding ${JOBS} jobs of ${JOB_MS}ms each`,
  `  worker marked dead ${detectionMs}ms after the kill`,
  `  all ${JOBS} jobs back on the queue ${requeueMs}ms after the kill`,
  `  all ${JOBS} jobs completed by a replacement worker ${recoveryMs}ms after the kill`,
  `fencing: a superseded worker's late result was rejected with reason "stale_lease" and ` +
  `did not change the job`,
]);
