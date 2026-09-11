// G7: graceful shutdown with jobs in flight joins every thread, leaves no job stranded in
// a non-terminal running state, and completes within its bound.
import path from 'node:path';
import { ROOT, ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer,
         http, waitFor, sleep, run } from './_lib.mjs';
import fs from 'node:fs';

const buildDir = ensureBuild('build');

const unit = runGtest('EngineTest.GracefulShutdownWithJobsInFlightJoinsEveryThread:' +
                      'EngineTest.DrainingShutdownFinishesQueuedWork:' +
                      'QueueTest.CloseWakesBlockedConsumersAndRejectsPushes:' +
                      'QueueTest.CloseStillDrainsAlreadyReadyEntries:' +
                      'HttpServerTest.StopIsCleanAndRepeatable', buildDir);
assertGtest(unit, 'shutdown tests', 5);

// A real coordinator process, with work genuinely in flight, must exit cleanly on SIGTERM.
const dbDir = path.join(ROOT, 'results', 'gate_shutdown');
fs.rmSync(dbDir, { recursive: true, force: true });
fs.mkdirSync(dbDir, { recursive: true });
const dbPath = path.join(dbDir, 'shutdown.db');

const server = await startServer(['--workers', '4', '--db', dbPath]);
for (let i = 0; i < 200; i++) {
  await http(server.port, 'POST', '/jobs', { type: 'sleep', payload: { ms: 60 } });
}
const busy = await waitFor(async () => {
  const h = await http(server.port, 'GET', '/health');
  return (h.json?.running || 0) > 0;
}, 10000);
check(busy, 'no job was running when shutdown was requested');

const t0 = Date.now();
server.proc.kill('SIGTERM');
const exited = await server.proc.waitExit(20000);
const elapsed = Date.now() - t0;
check(exited, 'the server did not exit after SIGTERM');
checkEq(server.proc.exitCode, 0, `the server exited with code ${server.proc.exitCode}`);
check(elapsed < 15000, `shutdown took ${elapsed}ms, beyond its bound`);
check(server.proc.output.includes('SHUTDOWN complete'),
      'the server did not report a complete shutdown');
if (!exited) server.proc.kill('SIGKILL');

// Nothing may be left RUNNING: an interrupted job must be recorded as recoverable.
const q = run('sqlite3', [dbPath, "SELECT state, COUNT(*) FROM jobs GROUP BY state;"]);
checkEq(q.code, 0, `could not read the database back: ${q.out}`);
const counts = {};
for (const line of q.out.trim().split('\n').filter(Boolean)) {
  const [state, n] = line.split('|');
  counts[state] = Number(n);
}
checkEq(counts.RUNNING || 0, 0, `${counts.RUNNING} jobs were left RUNNING after shutdown`);
const total = Object.values(counts).reduce((a, b) => a + b, 0);
checkEq(total, 200, `the database holds ${total} jobs, not the 200 that were submitted`);
const recoverable = (counts.QUEUED || 0) + (counts.SCHEDULED || 0) + (counts.RETRYING || 0);
check((counts.SUCCEEDED || 0) + recoverable === 200,
      'jobs ended in states other than succeeded or recoverable');

finish('G7', [
  `unit: ${unit.passed}/${unit.total} shutdown tests passed`,
  `SIGTERM with work in flight: exited 0 in ${elapsed}ms`,
  `after shutdown: ${JSON.stringify(counts)} (0 RUNNING, ${total} accounted for)`,
]);
