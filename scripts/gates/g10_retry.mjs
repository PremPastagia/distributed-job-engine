// G10: a failing job is retried exactly max_attempts times, and the observed backoff
// delays match the configured exponential schedule, measured from the recorded attempt
// timestamps rather than from any assertion inside the engine.
import { ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer, stopServer,
         http, waitFor } from './_lib.mjs';

const buildDir = ensureBuild('build');
const unit = runGtest('RetryTest.*:RetryPolicyTest.*', buildDir);
assertGtest(unit, 'retry tests', 8);

const server = await startServer(['--workers', '4', '--db', ':memory:']);

// 1) Retry count: a job that always fails must run exactly max_attempts times.
const MAX = 4;
const dead = await http(server.port, 'POST', '/jobs', {
  type: 'fail', max_attempts: MAX, backoff_base_ms: 10, backoff_multiplier: 1.0,
});
const deadId = dead.json.job_id;
await waitFor(async () => {
  const s = (await http(server.port, 'GET', `/jobs/${deadId}`)).json?.state;
  return s === 'FAILED';
}, 30000);
const deadJob = (await http(server.port, 'GET', `/jobs/${deadId}`)).json;
checkEq(deadJob.state, 'FAILED', 'a job with exhausted retries must end in FAILED (dead letter)');
checkEq(deadJob.attempt, MAX, 'the attempt counter does not match max_attempts');
const deadAttempts = (await http(server.port, 'GET', `/jobs/${deadId}/attempts`)).json.attempts;
checkEq(deadAttempts.length, MAX, 'the number of recorded attempts does not match max_attempts');
check(deadAttempts.every((a) => a.status === 'FAILED'), 'an attempt was not recorded as FAILED');

// 2) A job that recovers stops retrying immediately.
const recovering = await http(server.port, 'POST', '/jobs', {
  type: 'fail', payload: { fail_until_attempt: 3 }, max_attempts: 6,
  backoff_base_ms: 10, backoff_multiplier: 1.0,
});
const recId = recovering.json.job_id;
await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${recId}`)).json?.state === 'SUCCEEDED', 30000);
const recJob = (await http(server.port, 'GET', `/jobs/${recId}`)).json;
checkEq(recJob.state, 'SUCCEEDED', 'a job that recovers must succeed');
checkEq(recJob.attempt, 3, 'a recovered job kept retrying after it succeeded');

// 3) Backoff timing, measured from the stored attempt timestamps.
const BASE = 150;
const MULT = 2.0;
const timed = await http(server.port, 'POST', '/jobs', {
  type: 'fail', max_attempts: 4, backoff_base_ms: BASE, backoff_multiplier: MULT,
  backoff_max_ms: 60000, backoff_jitter: false,
});
const timedId = timed.json.job_id;
await waitFor(async () =>
    (await http(server.port, 'GET', `/jobs/${timedId}`)).json?.state === 'FAILED', 60000);
const attempts = (await http(server.port, 'GET', `/jobs/${timedId}/attempts`)).json.attempts;
checkEq(attempts.length, 4, 'expected four recorded attempts for the backoff measurement');

const gaps = [];
for (let i = 1; i < attempts.length; i++) {
  gaps.push(attempts[i].started_at_ms - attempts[i - 1].finished_at_ms);
}
// Expected schedule computed here from the formula, not read from the engine.
const expected = [0, 1, 2].map((k) => Math.round(BASE * Math.pow(MULT, k)));
const details = [];
for (let i = 0; i < expected.length; i++) {
  const observed = gaps[i];
  details.push(`retry ${i + 1}: expected ~${expected[i]}ms, observed ${observed}ms`);
  check(observed >= expected[i] - 25,
        `retry ${i + 1} fired early: ${observed}ms < ${expected[i]}ms`);
  check(observed <= expected[i] + 500,
        `retry ${i + 1} was far later than scheduled: ${observed}ms vs ${expected[i]}ms`);
}
// The schedule must actually be growing, not a constant delay that happens to fit.
check(gaps[1] > gaps[0] * 1.4 && gaps[2] > gaps[1] * 1.4,
      `the delays are not growing exponentially: ${gaps.join(', ')}`);

const metrics = (await http(server.port, 'GET', '/metrics')).json.counters;
check(metrics.attempts_retried >= 8, `attempts_retried is only ${metrics.attempts_retried}`);

await stopServer(server);

finish('G10', [
  `unit: ${unit.passed}/${unit.total} retry tests passed`,
  `retry count: max_attempts=${MAX} produced exactly ${deadAttempts.length} attempts, ending FAILED`,
  `early success: stopped after ${recJob.attempt} attempts instead of the 6 allowed`,
  `backoff (base=${BASE}ms, x${MULT}): ${details.join('; ')}`,
]);
