// G12: cancellation works for queued and running jobs, is refused for terminal jobs, and
// the cancel-versus-start race resolves to exactly one outcome.
import { ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer, stopServer,
         http, waitFor } from './_lib.mjs';

const buildDir = ensureBuild('build');
const unit = runGtest('CancelTest.*', buildDir);
assertGtest(unit, 'cancellation tests', 6);

// Queued cancellation: with no workers nothing can start, so the outcome is deterministic.
const idle = await startServer(['--workers', '0', '--db', ':memory:']);
const queued = await http(idle.port, 'POST', '/jobs', { type: 'noop' });
const qid = queued.json.job_id;
checkEq((await http(idle.port, 'GET', `/jobs/${qid}`)).json.state, 'QUEUED', 'job did not queue');
const cancelRes = await http(idle.port, 'POST', `/jobs/${qid}/cancel`, '');
checkEq(cancelRes.status, 200, `cancel returned ${cancelRes.status}`);
checkEq((await http(idle.port, 'GET', `/jobs/${qid}`)).json.state, 'CANCELLED',
        'a queued job was not cancelled immediately');
const healthAfter = (await http(idle.port, 'GET', '/health')).json;
checkEq(healthAfter.queue_depth, 0, 'a cancelled job is still counted in the queue depth');
checkEq(healthAfter.outstanding, 0, 'a cancelled job still holds an admission slot');
checkEq((await http(idle.port, 'POST', `/jobs/${qid}/cancel`, '')).status, 409,
        'cancelling an already cancelled job should conflict');

// A delayed job must not resurrect when its delay elapses.
const delayed = await http(idle.port, 'POST', '/jobs', { type: 'noop', delay_ms: 400 });
const did = delayed.json.job_id;
checkEq((await http(idle.port, 'GET', `/jobs/${did}`)).json.state, 'SCHEDULED', 'not scheduled');
await http(idle.port, 'POST', `/jobs/${did}/cancel`, '');
await new Promise((r) => setTimeout(r, 700));
checkEq((await http(idle.port, 'GET', `/jobs/${did}`)).json.state, 'CANCELLED',
        'a cancelled delayed job came back when it matured');
await stopServer(idle);

// Running cancellation.
const server = await startServer(['--workers', '4', '--db', ':memory:']);
const running = await http(server.port, 'POST', '/jobs', { type: 'sleep', payload: { ms: 20000 } });
const rid = running.json.job_id;
const started = await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${rid}`)).json?.state === 'RUNNING', 10000);
check(started, 'the job never started, so running-cancellation was not exercised');
const t0 = Date.now();
checkEq((await http(server.port, 'POST', `/jobs/${rid}/cancel`, '')).status, 200, 'cancel failed');
const stopped = await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${rid}`)).json?.state === 'CANCELLED', 10000);
const cancelLatency = Date.now() - t0;
check(stopped, 'a running job did not reach CANCELLED');
check(cancelLatency < 3000, `cancellation took ${cancelLatency}ms to take effect`);

// Cancelling a completed job is refused.
const done = await http(server.port, 'POST', '/jobs', { type: 'noop' });
const doneId = done.json.job_id;
await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${doneId}`)).json?.state === 'SUCCEEDED', 10000);
checkEq((await http(server.port, 'POST', `/jobs/${doneId}/cancel`, '')).status, 409,
        'cancelling a completed job was accepted');
checkEq((await http(server.port, 'GET', `/jobs/${doneId}`)).json.state, 'SUCCEEDED',
        'a completed job was overwritten by a cancel');

// The race: cancel issued immediately after submission, repeated many times. Every job
// must settle in exactly one terminal state and nothing may be left outstanding.
const ROUNDS = 150;
const outcomes = {};
for (let i = 0; i < ROUNDS; i++) {
  const j = await http(server.port, 'POST', '/jobs', { type: 'sleep', payload: { ms: 15 } });
  const id = j.json.job_id;
  await http(server.port, 'POST', `/jobs/${id}/cancel`, '');
  const settled = await waitFor(async () => {
    const s = (await http(server.port, 'GET', `/jobs/${id}`)).json?.state;
    return ['CANCELLED', 'SUCCEEDED', 'FAILED', 'TIMED_OUT'].includes(s);
  }, 10000);
  check(settled, `race round ${i} never settled`);
  const state = (await http(server.port, 'GET', `/jobs/${id}`)).json.state;
  outcomes[state] = (outcomes[state] || 0) + 1;
  check(['CANCELLED', 'SUCCEEDED'].includes(state),
        `race round ${i} ended in an unexpected state: ${state}`);
}
const settledTotal = Object.values(outcomes).reduce((a, b) => a + b, 0);
checkEq(settledTotal, ROUNDS, 'not every race round produced a terminal state');
const finalHealth = (await http(server.port, 'GET', '/health')).json;
checkEq(finalHealth.outstanding, 0, 'jobs were left outstanding after the race');

await stopServer(server);

finish('G12', [
  `unit: ${unit.passed}/${unit.total} cancellation tests passed`,
  `queued cancel is immediate and releases both the queue slot and the admission slot`,
  `running cancel took effect in ${cancelLatency}ms; cancelling a terminal job returns 409`,
  `cancel/start race over ${ROUNDS} rounds: ${JSON.stringify(outcomes)}, 0 left outstanding`,
]);
