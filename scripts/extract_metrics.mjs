#!/usr/bin/env node
// Derives the headline metrics from the benchmark artifacts and writes:
//   results/verified_metrics.json  - machine-readable, one entry per metric
//   results/metrics_table.md       - the table injected into CV_POINTERS_*.md
//
// Nothing here invents a number: every value is computed from results/summary.csv or from
// the raw per-sample CSVs it points at. A metric with no supporting data is omitted rather
// than defaulted, so a missing benchmark shows up as a missing claim.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, parseCsv, percentile, run } from './gates/_lib.mjs';

const RESULTS = path.join(ROOT, 'results');
const SUMMARY = path.join(RESULTS, 'summary.csv');
if (!fs.existsSync(SUMMARY)) {
  console.error(`missing ${SUMMARY}; run: node scripts/run_benchmarks.mjs`);
  process.exit(1);
}

const rows = parseCsv(fs.readFileSync(SUMMARY, 'utf8')).rows;
const metrics = {};

const median = (xs) => {
  const v = xs.filter((x) => Number.isFinite(x)).sort((a, b) => a - b);
  if (v.length === 0) return null;
  const mid = Math.floor(v.length / 2);
  return v.length % 2 ? v[mid] : (v[mid - 1] + v[mid]) / 2;
};

const select = (suite, label) => rows.filter((r) => r.suite === suite && r.label === label);

const rawSamples = (suite, label, column) => {
  const out = [];
  for (const r of select(suite, label)) {
    if (!r.raw_csv) continue;
    const p = path.join(ROOT, r.raw_csv);
    if (!fs.existsSync(p)) continue;
    for (const row of parseCsv(fs.readFileSync(p, 'utf8')).rows) {
      const v = Number(row[column]);
      if (Number.isFinite(v) && v >= 0) out.push(v);
    }
  }
  return out.sort((a, b) => a - b);
};

// Round to a precision the measurement actually supports. Reporting 101.143% efficiency
// or 8563.323 ms implies a resolution three runs of a noisy benchmark cannot justify.
function round(value, unit) {
  switch (unit) {
    case 'jobs/s': case 'req/s': case 'lines': case 'tests': case 'states': case 'routes':
      return Number(value.toFixed(0));
    case '%':
      return Number(value.toFixed(1));
    case 'x': case 'rows': case 'workflows/s':
      return Number(value.toFixed(2));
    case 'ms':
      // Sub-millisecond figures need three decimals; multi-second ones do not.
      return Number(value.toFixed(value >= 100 ? 1 : value >= 1 ? 2 : 3));
    default:
      return Number(value.toFixed(3));
  }
}

function add(key, value, unit, source, method) {
  if (value === null || value === undefined || !Number.isFinite(value)) return;
  metrics[key] = { value: round(value, unit), unit, source, method };
}

// --- throughput and scaling -----------------------------------------------------------
// Three workloads, because they answer different questions:
//   noop         - zero work per job, so it measures the engine's own overhead ceiling.
//                  It is FASTEST single-threaded: with nothing to parallelise, extra
//                  threads only add contention. Never quote it as a scaling result.
//   sleep5ms     - an I/O-bound stand-in that yields its core, so it shows how well the
//                  scheduler overlaps waiting work.
//   cpu_hash200k - CPU-bound, so it is bounded by real cores.
for (const workload of ['noop', 'sleep5ms', 'cpu_hash200k']) {
  for (const w of [1, 2, 4, 8]) {
    const rs = select('worker_scaling', `${workload}_w${w}`);
    add(`throughput_${workload}_${w}_worker_threads`,
        median(rs.map((r) => Number(r.throughput_per_s))), 'jobs/s', 'results/summary.csv',
        `median of ${rs.length} runs, suite=worker_scaling label=${workload}_w${w}`);
  }
  const base = metrics[`throughput_${workload}_1_worker_threads`]?.value;
  for (const w of [2, 4, 8]) {
    const v = metrics[`throughput_${workload}_${w}_worker_threads`]?.value;
    if (!base || !v) continue;
    add(`scaling_speedup_${workload}_${w}_threads`, v / base, 'x', 'results/summary.csv',
        `throughput at ${w} threads divided by throughput at 1 thread, same workload`);
    add(`scaling_efficiency_${workload}_${w}_threads`, (v / base / w) * 100, '%',
        'results/summary.csv', 'speedup divided by thread count');
  }
}

// --- group commit A/B -------------------------------------------------------------------
for (const w of [1, 4, 8]) {
  const off = median(select('group_commit', `false_w${w}`).map((r) => Number(r.throughput_per_s)));
  const on = median(select('group_commit', `true_w${w}`).map((r) => Number(r.throughput_per_s)));
  add(`group_commit_off_throughput_${w}_threads`, off, 'jobs/s', 'results/summary.csv',
      'median throughput with --group-commit false');
  add(`group_commit_on_throughput_${w}_threads`, on, 'jobs/s', 'results/summary.csv',
      'median throughput with --group-commit true');
  if (off && on) {
    add(`group_commit_speedup_${w}_threads`, on / off, 'x', 'results/summary.csv',
        'on divided by off, same binary and workload, only the flag changed');
  }
}
const rowsPerTx = median(select('group_commit', 'true_w4').map((r) => Number(r.rows_per_transaction)));
add('group_commit_rows_per_transaction_4_threads', rowsPerTx, 'rows',
    'results/summary.csv', 'row writes divided by committed transactions, reported by the store');

// --- latency under controlled load --------------------------------------------------------
for (const workload of ['noop', 'sleep5ms']) {
  for (const rate of [100, 500, 1000, 2000, 4000]) {
    const samples = rawSamples('latency', `${workload}_${rate}ps`, 'e2e_us');
    if (samples.length === 0) continue;
    const achieved = median(select('latency', `${workload}_${rate}ps`)
                                .map((r) => Number(r.throughput_per_s)));
    add(`latency_${workload}_${rate}ps_achieved`, achieved, 'jobs/s', 'results/summary.csv',
        'completed jobs divided by wall time');
    for (const p of [50, 95, 99]) {
      add(`latency_${workload}_${rate}ps_p${p}_ms`, percentile(samples, p) / 1000, 'ms',
          `results/raw/latency_${workload}_r${rate}_rep*.csv`,
          `nearest-rank p${p} over ${samples.length} raw e2e samples from intended submit time`);
    }
  }
}

// --- HTTP API --------------------------------------------------------------------------------
for (const mode of ['submit', 'status', 'mixed', 'health']) {
  for (const c of [1, 8, 32]) {
    const samples = rawSamples('http_api', `${mode}_c${c}`, 'latency_us');
    if (samples.length === 0) continue;
    const thr = median(select('http_api', `${mode}_c${c}`).map((r) => Number(r.throughput_per_s)));
    const err = median(select('http_api', `${mode}_c${c}`).map((r) => Number(r.error_rate)));
    add(`http_${mode}_c${c}_throughput`, thr, 'req/s', 'results/summary.csv',
        `median of the load generator's reported throughput`);
    for (const p of [50, 95, 99]) {
      add(`http_${mode}_c${c}_p${p}_ms`, percentile(samples, p) / 1000, 'ms',
          `results/raw/http_${mode}_c${c}_r*.csv`,
          `nearest-rank p${p} over ${samples.length} raw per-request latencies`);
    }
    if (err !== null) {
      add(`http_${mode}_c${c}_error_rate`, err * 100, '%', 'results/summary.csv',
          'non-2xx responses divided by requests issued');
    }
  }
}

// --- distributed ---------------------------------------------------------------------------
// The distributed suite runs `sleep 2ms` jobs on 2 threads per process, so each worker slot
// has a hard ceiling of 500 jobs/s. Utilisation of that ceiling is the meaningful figure:
// "efficiency relative to one process" exceeds 100% here, not because the system found extra
// capacity, but because the single-process baseline is itself the least efficient point -
// per-job coordination cost amortises better as load rises.
const DIST_JOB_MS = 2;
const DIST_THREADS_PER_PROCESS = 2;
let distBase = null;
for (const p of [1, 2, 4, 8]) {
  const thr = median(select('distributed', `${p}proc`).map((r) => Number(r.throughput_per_s)));
  if (thr === null) continue;
  add(`distributed_throughput_${p}_processes`, thr, 'jobs/s', 'results/summary.csv',
      `median over runs with ${p} worker processes of ${DIST_THREADS_PER_PROCESS} threads ` +
      `each; the coordinator runs 0 embedded workers, so every job was executed by a ` +
      `separate process`);
  const ceiling = p * DIST_THREADS_PER_PROCESS * (1000 / DIST_JOB_MS);
  add(`distributed_ceiling_utilisation_${p}_processes`, (thr / ceiling) * 100, '%',
      'results/summary.csv',
      `throughput divided by the theoretical ceiling of ${ceiling} jobs/s ` +
      `(${p * DIST_THREADS_PER_PROCESS} slots x one ${DIST_JOB_MS}ms job each)`);
  if (p === 1) distBase = thr;
  else if (distBase) {
    add(`distributed_speedup_${p}_processes`, thr / distBase, 'x', 'results/summary.csv',
        'throughput divided by the single-process throughput');
  }
}

// --- priority -------------------------------------------------------------------------------
{
  const rs = select('priority', '0:50,9:50');
  const byPriority = { 0: [], 9: [] };
  for (const r of rs) {
    if (!r.raw_csv) continue;
    const p = path.join(ROOT, r.raw_csv);
    if (!fs.existsSync(p)) continue;
    for (const row of parseCsv(fs.readFileSync(p, 'utf8')).rows) {
      const pr = Number(row.priority);
      if (byPriority[pr]) byPriority[pr].push(Number(row.queue_wait_us));
    }
  }
  for (const pr of [0, 9]) {
    const s = byPriority[pr].sort((a, b) => a - b);
    if (s.length === 0) continue;
    add(`priority_${pr}_queue_wait_p50_ms`, percentile(s, 50) / 1000, 'ms',
        'results/raw/priority_0_50_9_50_r*.csv',
        `nearest-rank p50 over ${s.length} raw queue-wait samples at priority ${pr}`);
    add(`priority_${pr}_queue_wait_p95_ms`, percentile(s, 95) / 1000, 'ms',
        'results/raw/priority_0_50_9_50_r*.csv',
        `nearest-rank p95 over ${s.length} raw queue-wait samples at priority ${pr}`);
  }
  const hi = metrics.priority_9_queue_wait_p50_ms?.value;
  const lo = metrics.priority_0_queue_wait_p50_ms?.value;
  if (hi && lo) {
    add('priority_queue_wait_reduction_p50', ((lo - hi) / lo) * 100, '%',
        'results/raw/priority_0_50_9_50_r*.csv',
        'reduction in median queue wait for priority 9 relative to priority 0, same run');
  }
}

// --- workflows ---------------------------------------------------------------------------------
for (const shape of ['chain4', 'fanout6', 'diamond6']) {
  for (const w of [1, 4, 8]) {
    const rs = select('workflow', `${shape}_w${w}`);
    const p50 = median(rs.map((r) => Number(r.e2e_p50_ms)));
    const thr = median(rs.map((r) => Number(r.throughput_per_s)));
    add(`workflow_${shape}_w${w}_p50_ms`, p50, 'ms', 'results/summary.csv',
        `median of the driver's p50 over ${rs.length} runs`);
    add(`workflow_${shape}_w${w}_throughput`, thr, 'workflows/s', 'results/summary.csv',
        `median workflows completed per second`);
  }
}

// --- failure-heavy -----------------------------------------------------------------------------
for (const label of ['fail0pct', 'fail30pct', 'fail70pct']) {
  const rs = select('failure_heavy', label);
  const thr = median(rs.map((r) => Number(r.throughput_per_s)));
  const succeeded = median(rs.map((r) => Number(r.succeeded)));
  const failed = median(rs.map((r) => Number(r.failed)));
  add(`failure_${label}_throughput`, thr, 'jobs/s', 'results/summary.csv',
      'median throughput with retries enabled (max_attempts=3)');
  if (succeeded !== null && failed !== null && succeeded + failed > 0) {
    add(`failure_${label}_dead_letter_rate`, (failed / (succeeded + failed)) * 100, '%',
        'results/summary.csv', 'jobs that exhausted their retries, divided by jobs completed');
  }
}

// --- backpressure -------------------------------------------------------------------------------
for (const cap of [100, 1000, 10000, 200000]) {
  const rs = select('queue_capacity', String(cap));
  const rejected = median(rs.map((r) => Number(r.rejected)));
  const accepted = median(rs.map((r) => Number(r.submitted)));
  if (rejected !== null && accepted !== null && accepted + rejected > 0) {
    add(`backpressure_cap${cap}_rejection_rate`, (rejected / (accepted + rejected)) * 100, '%',
        'results/summary.csv',
        `rejected submissions divided by attempted, with max_outstanding=${cap} under a saturating submit rate`);
  }
}

// --- structural facts, read from the code rather than from a benchmark -------------------
// These are still measurements: each is counted from a source file or from the test
// runner, so a document can cite them without anybody typing a number by hand.
{
  const listed = run(path.join(ROOT, 'build', 'jobengine_tests'), ['--gtest_list_tests']);
  const testCount = (listed.out.match(/^\s{2}\S+/gm) || []).length;
  add('tests_total', testCount, 'tests', 'build/jobengine_tests',
      'counted from `jobengine_tests --gtest_list_tests`');

  const jobHeader = fs.readFileSync(path.join(ROOT, 'include', 'jobengine', 'job.hpp'), 'utf8');
  const stateCount = (jobHeader.match(/^\s{2}[A-Z][A-Za-z]+,\s*\/\//gm) || []).length;
  add('job_states', stateCount, 'states', 'include/jobengine/job.hpp',
      'counted from the JobState enumerators');

  const apiSource = fs.readFileSync(path.join(ROOT, 'src', 'api', 'api.cpp'), 'utf8');
  const routeCount = (apiSource.match(/\{"(GET|POST)", "/g) || []).length;
  add('http_routes', routeCount, 'routes', 'src/api/api.cpp',
      'counted from the kRoutes table, which is also what GET /routes returns');

  let sourceLines = 0;
  const countDir = (dir, exts) => {
    const full = path.join(ROOT, dir);
    if (!fs.existsSync(full)) return;
    for (const entry of fs.readdirSync(full, { withFileTypes: true })) {
      const p2 = path.join(dir, entry.name);
      if (entry.isDirectory()) countDir(p2, exts);
      else if (exts.some((e) => entry.name.endsWith(e))) {
        sourceLines += fs.readFileSync(path.join(ROOT, p2), 'utf8').split('\n').length;
      }
    }
  };
  countDir('src', ['.cpp']);
  countDir('include', ['.hpp']);
  countDir('apps', ['.cpp', '.hpp']);
  add('source_lines_cpp', sourceLines, 'lines', 'src/ include/ apps/',
      'newline count across the C++ sources and headers, excluding tests and third_party');

  let testLines = 0;
  countDirTests('tests', ['.cpp', '.hpp']);
  function countDirTests(dir, exts) {
    const full = path.join(ROOT, dir);
    if (!fs.existsSync(full)) return;
    for (const entry of fs.readdirSync(full, { withFileTypes: true })) {
      const p2 = path.join(dir, entry.name);
      if (entry.isDirectory()) countDirTests(p2, exts);
      else if (exts.some((e) => entry.name.endsWith(e))) {
        testLines += fs.readFileSync(path.join(ROOT, p2), 'utf8').split('\n').length;
      }
    }
  }
  add('test_lines_cpp', testLines, 'lines', 'tests/', 'newline count across the test sources');
}

fs.writeFileSync(path.join(RESULTS, 'verified_metrics.json'),
                 JSON.stringify(metrics, null, 2) + '\n');

// --- markdown table ----------------------------------------------------------------------------
const keys = Object.keys(metrics).sort();
const lines = [
  '| metric | value | unit | evidence file | how it was computed |',
  '|---|---|---|---|---|',
];
for (const k of keys) {
  const m = metrics[k];
  lines.push(`| \`${k}\` | ${m.value} | ${m.unit} | \`${m.source}\` | ${m.method} |`);
}
fs.writeFileSync(path.join(RESULTS, 'metrics_table.md'), lines.join('\n') + '\n');

console.log(`extracted ${keys.length} verified metrics`);
console.log('  results/verified_metrics.json');
console.log('  results/metrics_table.md');
