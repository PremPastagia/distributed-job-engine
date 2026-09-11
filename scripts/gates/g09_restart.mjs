// G9: job metadata survives a coordinator restart, and jobs interrupted mid-flight are
// recovered and completed.
import path from 'node:path';
import fs from 'node:fs';
import { ROOT, ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer,
         http, waitFor, run, sleep } from './_lib.mjs';

const buildDir = ensureBuild('build');
const unit = runGtest('EngineTest.RecoveryRequeuesInterruptedJobsAfterRestart:' +
                      'StoreTest.DataSurvivesReopeningTheFile:' +
                      'StoreTest.RecoveryListOnlyReturnsNonTerminalJobs', buildDir);
assertGtest(unit, 'recovery tests', 3);

const dir = path.join(ROOT, 'results', 'gate_restart');
fs.rmSync(dir, { recursive: true, force: true });
fs.mkdirSync(dir, { recursive: true });
const db = path.join(dir, 'restart.db');

// Phase 1: a coordinator with no workers, so every job is durably queued but unexecuted.
const first = await startServer(['--workers', '0', '--db', db]);
const ids = [];
for (let i = 0; i < 50; i++) {
  const res = await http(first.port, 'POST', '/jobs', { type: 'echo', payload: { i }, priority: i % 3 });
  checkEq(res.status, 201, `submission ${i} failed: ${res.body}`);
  ids.push(res.json.job_id);
}
// Also leave one job in RUNNING, the state a crash mid-execution would produce. It is
// submitted at a priority above every other queued job so the claim below is deterministic:
// the scheduler hands out the highest-priority job, not an arbitrary one.
const running = await http(first.port, 'POST', '/jobs',
                           { type: 'sleep', payload: { ms: 60000 }, priority: 100 });
const runningId = running.json.job_id;

// Give it to a remote worker so the coordinator marks it RUNNING without executing it.
const reg = await http(first.port, 'POST', '/internal/workers/register',
                       { name: 'ghost', capacity: 1 });
const claimed = await http(first.port, 'POST', '/internal/claim',
                           { worker_id: reg.json.worker_id, max_jobs: 1, wait_ms: 2000 });
check(claimed.json?.count >= 1, 'the worker did not receive a job to leave RUNNING');
checkEq(claimed.json?.jobs?.[0]?.job_id, runningId,
        'the claim returned a different job, so strict priority ordering is not holding');
const runningState = (await http(first.port, 'GET', `/jobs/${runningId}`)).json.state;
checkEq(runningState, 'RUNNING', 'the job was not left in RUNNING before the restart');

// SIGKILL: a hard crash, not a graceful shutdown, is the interesting case.
first.proc.kill('SIGKILL');
await first.proc.waitExit(10000);

// Phase 2: restart with workers and let recovery finish the work.
const second = await startServer(['--workers', '4', '--db', db, '--lease-ms', '2000',
                                  '--heartbeat-timeout-ms', '1000']);
const recoveredLine = /recovered=(\d+)/.exec(second.proc.output);
check(recoveredLine !== null, 'the server did not report how many jobs it recovered');
if (recoveredLine) {
  checkEq(Number(recoveredLine[1]), 51,
          'the number of recovered jobs does not match what was left behind');
}

const drained = await waitFor(async () => {
  const h = await http(second.port, 'GET', '/health');
  return h.json?.outstanding === 0;
}, 60000);
check(drained, 'the recovered jobs never completed');

let notSucceeded = 0;
for (const id of ids) {
  const j = await http(second.port, 'GET', `/jobs/${id}`);
  if (j.json?.state !== 'SUCCEEDED') notSucceeded++;
}
checkEq(notSucceeded, 0, `${notSucceeded} of the 50 queued jobs did not survive the restart`);

const recoveredRunning = await http(second.port, 'GET', `/jobs/${runningId}`);
check(['SUCCEEDED', 'TIMED_OUT', 'FAILED', 'QUEUED', 'RUNNING'].includes(recoveredRunning.json?.state),
      `the interrupted job is in an unexpected state: ${recoveredRunning.json?.state}`);
check(recoveredRunning.json?.attempt >= 1,
      'the interrupted attempt was not counted, so retries would be unbounded');

second.proc.kill('SIGTERM');
await second.proc.waitExit(20000);

// Phase 3: the durable record outlives the process entirely.
const q = run('sqlite3', [db, "SELECT COUNT(*) FROM jobs;", "SELECT COUNT(*) FROM job_attempts;"]);
checkEq(q.code, 0, `could not read the database: ${q.out}`);
const [jobCount, attemptCount] = q.out.trim().split('\n').map(Number);
checkEq(jobCount, 51, `the database holds ${jobCount} jobs, expected 51`);
check(attemptCount >= 50, `only ${attemptCount} attempts were recorded`);

finish('G9', [
  `unit: ${unit.passed}/${unit.total} recovery tests passed`,
  `crash (SIGKILL) with 50 QUEUED + 1 RUNNING job, restart recovered ${recoveredLine ? recoveredLine[1] : '?'} jobs`,
  `all 50 queued jobs completed after restart; the interrupted job kept its attempt count`,
  `database after both processes exited: ${jobCount} jobs, ${attemptCount} attempt records`,
]);
