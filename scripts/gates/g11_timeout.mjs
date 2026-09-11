// G11: a job exceeding its deadline is cancelled cooperatively and lands in TIMED_OUT,
// and the documented limit of cooperative cancellation is demonstrated, not assumed.
import { ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer, stopServer,
         http, waitFor } from './_lib.mjs';

const buildDir = ensureBuild('build');
const unit = runGtest('TimeoutTest.*', buildDir);
assertGtest(unit, 'timeout tests', 4);

const server = await startServer(['--workers', '4', '--db', ':memory:']);

// 1) A cooperative handler is stopped near its deadline, not after its natural duration.
const TIMEOUT = 200;
const t0 = Date.now();
const job = await http(server.port, 'POST', '/jobs',
                       { type: 'sleep', payload: { ms: 10000 }, timeout_ms: TIMEOUT });
const id = job.json.job_id;
await waitFor(async () =>
    ['TIMED_OUT', 'FAILED', 'SUCCEEDED'].includes(
        (await http(server.port, 'GET', `/jobs/${id}`)).json?.state), 20000);
const elapsed = Date.now() - t0;
const record = (await http(server.port, 'GET', `/jobs/${id}`)).json;
checkEq(record.state, 'TIMED_OUT', 'an expired deadline must be distinguishable from a failure');
check(/deadline exceeded/.test(record.error || ''), `unexpected error text: ${record.error}`);
check(elapsed < 3000, `the job ran for ${elapsed}ms despite a ${TIMEOUT}ms deadline`);
check(elapsed >= TIMEOUT - 30, `the job was stopped before its deadline (${elapsed}ms)`);
const attempts = (await http(server.port, 'GET', `/jobs/${id}/attempts`)).json.attempts;
checkEq(attempts.length, 1, 'exactly one attempt should have been recorded');
checkEq(attempts[0].status, 'TIMED_OUT', 'the attempt was not recorded as timed out');

// 2) A timeout consumes an attempt like any other failure.
const retried = await http(server.port, 'POST', '/jobs',
                           { type: 'sleep', payload: { ms: 10000 }, timeout_ms: 120,
                             max_attempts: 3, backoff_base_ms: 10, backoff_multiplier: 1.0 });
const rid = retried.json.job_id;
await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${rid}`)).json?.state === 'TIMED_OUT', 30000);
const rrec = (await http(server.port, 'GET', `/jobs/${rid}`)).json;
checkEq(rrec.attempt, 3, 'a timed-out job did not use its whole retry budget');
checkEq((await http(server.port, 'GET', `/jobs/${rid}/attempts`)).json.attempts.length, 3,
        'not every timed-out attempt was recorded');

// 3) The documented limitation: a handler that ignores its cancellation flag runs to
// completion and holds its worker. If this ever stops being true, D-07 is wrong.
const UNCOOP_MS = 1200;
const t1 = Date.now();
const stubborn = await http(server.port, 'POST', '/jobs',
                            { type: 'sleep_uncooperative', payload: { ms: UNCOOP_MS },
                              timeout_ms: 100 });
const sid = stubborn.json.job_id;
await waitFor(async () =>
    ['TIMED_OUT', 'FAILED', 'SUCCEEDED'].includes(
        (await http(server.port, 'GET', `/jobs/${sid}`)).json?.state), 20000);
const stubbornElapsed = Date.now() - t1;
const srec = (await http(server.port, 'GET', `/jobs/${sid}`)).json;
checkEq(srec.state, 'TIMED_OUT', 'the deadline breach was not recorded');
check(stubbornElapsed >= UNCOOP_MS * 0.8,
      `the uncooperative handler was stopped after ${stubbornElapsed}ms: cancellation appears ` +
      `preemptive, so DESIGN_DECISIONS D-07 no longer describes the system`);

// 4) No deadline means no interference.
const free = await http(server.port, 'POST', '/jobs', { type: 'sleep', payload: { ms: 150 } });
const fid = free.json.job_id;
await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${fid}`)).json?.state === 'SUCCEEDED', 20000);
checkEq((await http(server.port, 'GET', `/jobs/${fid}`)).json.state, 'SUCCEEDED',
        'a job without a deadline was interfered with');

const counters = (await http(server.port, 'GET', '/metrics')).json.counters;
check(counters.jobs_timed_out >= 3, `jobs_timed_out is only ${counters.jobs_timed_out}`);

await stopServer(server);

finish('G11', [
  `unit: ${unit.passed}/${unit.total} timeout tests passed`,
  `cooperative handler: ${TIMEOUT}ms deadline on a 10s job stopped it after ${elapsed}ms -> TIMED_OUT`,
  `timeout consumes attempts: 3 recorded attempts under max_attempts=3`,
  `uncooperative handler: ran ${stubbornElapsed}ms against a 100ms deadline, confirming that ` +
  `cancellation is cooperative (D-07) and is recorded as TIMED_OUT regardless`,
]);
