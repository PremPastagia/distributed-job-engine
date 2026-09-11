// G6: dequeue order is strict priority with FIFO tie-break, checked against an expected
// order computed independently of the implementation.
import path from 'node:path';
import fs from 'node:fs';
import { ROOT, ensureBuild, runGtest, assertGtest, check, checkEq, finish, run,
         parseCsv, readText, percentile } from './_lib.mjs';

const buildDir = ensureBuild('build');

const unit = runGtest('QueueTest.PopsStrictlyByPriorityThenFifo:' +
                      'QueueTest.NegativePrioritiesOrderBelowZero:' +
                      'QueueTest.DelayedEntriesAreNotReadyBeforeTheirTime:' +
                      'EngineTest.HigherPriorityJobsRunFirst:' +
                      'EngineTest.FifoWithinAPriorityLevel:' +
                      'WorkflowTest.WorkflowNodesRespectPriorityWithinTheReadySet', buildDir);
assertGtest(unit, 'priority ordering tests', 6);

// Independent measurement: with a backlog and a mixed priority workload, high-priority
// jobs must wait less than low-priority jobs. This is measured from the per-job CSV, not
// from any assertion inside the engine.
const csv = path.join(ROOT, 'results', 'gate_priority.csv');
fs.mkdirSync(path.dirname(csv), { recursive: true });
const bench = run(path.join(buildDir, 'jobengine-bench'),
                  ['--workers', '2', '--jobs', '4000', '--submitters', '4',
                   '--type', 'sleep', '--payload', '{"ms":1}',
                   '--priorities', '0:50,9:50', '--out', csv, '--label', 'gate-priority']);
checkEq(bench.code, 0, `benchmark failed:\n${bench.out.slice(-1500)}`);

const rows = parseCsv(readText(path.relative(ROOT, csv))).rows;
check(rows.length > 3000, `expected roughly 4000 rows, got ${rows.length}`);
const waitsByPriority = new Map();
for (const r of rows) {
  const p = Number(r.priority);
  if (!waitsByPriority.has(p)) waitsByPriority.set(p, []);
  waitsByPriority.get(p).push(Number(r.queue_wait_us));
}
check(waitsByPriority.has(0) && waitsByPriority.has(9),
      'the benchmark did not produce both priority classes');

const high = (waitsByPriority.get(9) || []).sort((a, b) => a - b);
const low = (waitsByPriority.get(0) || []).sort((a, b) => a - b);
const highP50 = percentile(high, 50);
const lowP50 = percentile(low, 50);
const highP95 = percentile(high, 95);
const lowP95 = percentile(low, 95);

check(high.length > 1000 && low.length > 1000,
      `unbalanced priority split: ${high.length} high, ${low.length} low`);
check(highP50 < lowP50,
      `priority 9 did not wait less than priority 0 at p50 (${highP50}us vs ${lowP50}us)`);
check(highP95 < lowP95,
      `priority 9 did not wait less than priority 0 at p95 (${highP95}us vs ${lowP95}us)`);

finish('G6', [
  `unit: ${unit.passed}/${unit.total} ordering tests passed`,
  `measured over ${rows.length} jobs, 2 workers, 1ms jobs, 50/50 priority mix:`,
  `  priority 9 queue wait p50=${(highP50 / 1000).toFixed(1)}ms p95=${(highP95 / 1000).toFixed(1)}ms (n=${high.length})`,
  `  priority 0 queue wait p50=${(lowP50 / 1000).toFixed(1)}ms p95=${(lowP95 / 1000).toFixed(1)}ms (n=${low.length})`,
]);
