// G5: under multi-producer/multi-consumer load every submitted job is executed exactly
// once - nothing lost, nothing completed twice.
import { ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer, stopServer,
         http, waitFor, run, parseCsv, readText } from './_lib.mjs';
import path from 'node:path';
import fs from 'node:fs';
import { ROOT } from './_lib.mjs';

const buildDir = ensureBuild('build');

// 1) The in-process property tests.
const unit = runGtest(
    'QueueTest.MultiProducerMultiConsumerLosesNothingAndDuplicatesNothing:' +
    'EngineTest.NoJobIsLostOrExecutedTwiceUnderConcurrency:' +
    'EngineTest.CompletionCallbackFiresExactlyOncePerJob:' +
    'QueueTest.ConcurrentRemovesAndPopsStayConsistent', buildDir);
assertGtest(unit, 'no-loss unit tests', 4);

// 2) An independent end-to-end check through the real server, counted from the database
// rather than from the same counters the engine uses internally.
const N = 600;
const server = await startServer(['--workers', '6', '--db', ':memory:', '--http-threads', '8']);
const ids = [];
let submitFailures = 0;
for (let i = 0; i < N; i++) {
  const res = await http(server.port, 'POST', '/jobs',
                         { type: 'echo', payload: { i }, priority: i % 5 });
  if (res.ok && res.status === 201 && res.json?.job_id) ids.push(res.json.job_id);
  else submitFailures++;
}
checkEq(submitFailures, 0, `${submitFailures} submissions failed`);
checkEq(ids.length, N, 'not every submission returned a job id');
checkEq(new Set(ids).size, N, 'the server issued a duplicate job id');

const drained = await waitFor(async () => {
  const h = await http(server.port, 'GET', '/health');
  return h.json?.outstanding === 0;
}, 60000);
check(drained, 'the engine never drained');

const metrics = await http(server.port, 'GET', '/metrics');
const byState = metrics.json?.jobs_by_state || {};
checkEq(byState.SUCCEEDED || 0, N, 'not every job reached SUCCEEDED');
checkEq(Object.values(byState).reduce((a, b) => a + b, 0), N,
        'the total number of stored jobs does not match the number submitted');

// Each job must have exactly one recorded attempt: more would mean double execution.
let wrongAttemptCount = 0;
let missing = 0;
for (const id of ids) {
  const j = await http(server.port, 'GET', `/jobs/${id}`);
  if (!j.ok || j.status !== 200) { missing++; continue; }
  if (j.json.attempt !== 1) wrongAttemptCount++;
  const a = await http(server.port, 'GET', `/jobs/${id}/attempts`);
  if ((a.json?.attempts || []).length !== 1) wrongAttemptCount++;
}
checkEq(missing, 0, `${missing} submitted jobs could not be read back`);
checkEq(wrongAttemptCount, 0, `${wrongAttemptCount} jobs were executed more than once`);

const counters = metrics.json?.counters || {};
checkEq(counters.jobs_submitted, N, 'jobs_submitted counter disagrees with the submissions made');
checkEq(counters.jobs_succeeded, N, 'jobs_succeeded counter disagrees with the stored states');
checkEq(counters.attempts_started, N, 'more attempts were started than jobs submitted');

await stopServer(server);

// 3) The benchmark driver independently reports submitted vs completed vs rejected.
const csv = path.join(ROOT, 'results', 'gate_no_loss.csv');
fs.mkdirSync(path.dirname(csv), { recursive: true });
const bench = run(path.join(buildDir, 'jobengine-bench'),
                  ['--workers', '8', '--jobs', '5000', '--submitters', '4', '--type', 'noop',
                   '--out', csv, '--label', 'gate-no-loss']);
checkEq(bench.code, 0, 'the benchmark driver failed');
const m = /submitted=(\d+) completed=(\d+) rejected=(\d+) succeeded=(\d+)/.exec(bench.out);
check(m !== null, 'the benchmark driver did not report its counts');
if (m) {
  checkEq(Number(m[1]), 5000, 'not every benchmark job was submitted');
  checkEq(Number(m[2]), Number(m[1]), 'completed count does not equal submitted count');
  checkEq(Number(m[3]), 0, 'jobs were rejected during the benchmark');
  checkEq(Number(m[4]), Number(m[1]), 'not every benchmark job succeeded');
}
const rows = parseCsv(readText(path.relative(ROOT, csv))).rows;
checkEq(rows.length, 5000, 'the per-job CSV does not contain one row per job');
checkEq(new Set(rows.map((r) => r.job_id)).size, 5000, 'the CSV contains a duplicate job id');

finish('G5', [
  `unit: ${unit.passed}/${unit.total} concurrency property tests passed`,
  `end to end: ${N} jobs submitted over HTTP, ${byState.SUCCEEDED} succeeded, each with exactly 1 attempt`,
  `benchmark: 5000 jobs submitted, ${m ? m[2] : '?'} completed, ${m ? m[3] : '?'} rejected, 5000 unique CSV rows`,
]);
