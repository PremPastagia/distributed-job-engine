#!/usr/bin/env node
// Reproducible benchmark suite.
//
//   node scripts/run_benchmarks.mjs            # full suite
//   node scripts/run_benchmarks.mjs --quick    # smaller sizes, fewer repetitions
//   node scripts/run_benchmarks.mjs --only worker_scaling,http_api
//   node scripts/run_benchmarks.mjs --quick --only priority --results-dir /tmp/scratch
//
// Every run appends one row to results/summary.csv with the parameters that produced it.
// Raw per-job and per-request samples are written to results/raw/. Nothing here computes a
// headline number: percentiles come from the raw samples (see scripts/analyze_benchmarks.py).
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, BUILD, run, Proc, waitForLine, startServer, stopServer, http, waitFor,
         sleep, ensureBuild, cpuCount } from './gates/_lib.mjs';

const args = process.argv.slice(2);
const quick = args.includes('--quick');
const onlyArg = args.find((a) => a.startsWith('--only'));
const only = onlyArg ? (onlyArg.includes('=') ? onlyArg.split('=')[1]
                                              : args[args.indexOf(onlyArg) + 1]).split(',')
                     : null;
const REPS = quick ? 1 : 3;
const scale = quick ? 0.25 : 1;
const n = (x) => Math.max(50, Math.round(x * scale));

// --results-dir keeps a throwaway run (the artifact gate re-runs a slice to prove the suite
// still works) out of the published results, so a verification step cannot overwrite the
// evidence it is verifying.
const resultsArg = args.find((a) => a.startsWith('--results-dir'));
const resultsDir = resultsArg
    ? (resultsArg.includes('=') ? resultsArg.split('=')[1] : args[args.indexOf(resultsArg) + 1])
    : 'results';
const RESULTS = path.isAbsolute(resultsDir) ? resultsDir : path.join(ROOT, resultsDir);
const RAW = path.join(RESULTS, 'raw');
fs.mkdirSync(RAW, { recursive: true });

const SUMMARY = path.join(RESULTS, 'summary.csv');
// Rows are appended as they are produced rather than held until the end: a long suite that
// is interrupted (this one was killed by memory pressure once) must not lose everything it
// already measured.
const APPEND_MODE = args.includes('--append') || only !== null;
const COLUMNS = [
  'suite', 'label', 'rep', 'workers', 'worker_processes', 'submitters', 'connections',
  'job_type', 'payload', 'priorities', 'max_attempts', 'jobs', 'group_commit',
  'max_outstanding', 'submitted', 'completed', 'rejected', 'succeeded', 'failed',
  'wall_s', 'throughput_per_s', 'submit_rate_per_s', 'store_batches', 'store_writes',
  'rows_per_transaction', 'e2e_mean_ms', 'e2e_p50_ms', 'e2e_p95_ms', 'e2e_p99_ms',
  'queue_p50_ms', 'queue_p95_ms', 'exec_p50_ms', 'exec_p95_ms', 'submit_call_p50_ms',
  'submit_call_p99_ms', 'error_rate', 'cpu_seconds', 'cores_used', 'peak_rss_mb',
  'ctx_switches', 'raw_csv',
];
const rows = [];

function shouldRun(suite) {
  return !only || only.includes(suite);
}

function bench(extraArgs) {
  return run(path.join(BUILD, 'jobengine-bench'), extraArgs, { timeout: 1800000 });
}

// Pulls the numbers out of the driver's own output, so a summary row can never disagree
// with what the driver printed.
function parseBench(out) {
  const g = (re, idx = 1) => { const m = re.exec(out); return m ? m[idx] : ''; };
  const stats = (name) => {
    const m = new RegExp(`BENCH ${name}: n=(\\d+) mean=([\\d.]+)ms p50=([\\d.]+)ms ` +
                         `p90=([\\d.]+)ms p95=([\\d.]+)ms p99=([\\d.]+)ms`).exec(out);
    return m ? { n: +m[1], mean: +m[2], p50: +m[3], p90: +m[4], p95: +m[5], p99: +m[6] }
             : { n: 0, mean: '', p50: '', p90: '', p95: '', p99: '' };
  };
  return {
    submitted: g(/submitted=(\d+)/), completed: g(/completed=(\d+)/),
    rejected: g(/rejected=(\d+)/), succeeded: g(/succeeded=(\d+)/), failed: g(/failed=(\d+)/),
    wall_s: g(/wall_s=([\d.]+)/),
    throughput: g(/throughput_jobs_per_s=([\d.]+)/),
    submit_rate: g(/submit_rate_jobs_per_s=([\d.]+)/),
    store_batches: g(/store_batches=(\d+)/), store_writes: g(/store_writes=(\d+)/),
    rows_per_tx: g(/avg_rows_per_transaction=([\d.]+)/),
    e2e: stats('e2e'), queue: stats('queue_wait'), exec: stats('exec'),
    submit_call: stats('submit_call'),
    cpu_seconds: g(/cpu_seconds=([\d.]+)/), cores_used: g(/cores_used=([\d.]+)/),
    peak_rss_mb: g(/peak_rss_mb=([\d.]+)/), ctx_switches: g(/ctx_switches=(\d+)/),
  };
}

// RFC 4180 quoting. Several columns legitimately contain commas (a priority mix such as
// "0:50,9:50", a JSON payload), and writing them raw silently shifts every later column.
function csvField(value) {
  const text = String(value ?? '');
  return /[",\n\r]/.test(text) ? '"' + text.replace(/"/g, '""') + '"' : text;
}

function ensureSummaryHeader() {
  if (!fs.existsSync(SUMMARY) || fs.readFileSync(SUMMARY, 'utf8').trim() === '') {
    fs.writeFileSync(SUMMARY, COLUMNS.join(',') + '\n');
  }
}

function record(suite, label, fields) {
  const row = {};
  for (const c of COLUMNS) row[c] = fields[c] === undefined ? '' : fields[c];
  row.suite = suite;
  row.label = label;
  rows.push(row);
  ensureSummaryHeader();
  fs.appendFileSync(SUMMARY, COLUMNS.map((c) => csvField(row[c])).join(',') + '\n');
  const summaryBits = [`${suite}/${label}`];
  if (fields.throughput_per_s) summaryBits.push(`${Number(fields.throughput_per_s).toFixed(1)}/s`);
  if (fields.e2e_p95_ms !== '' && fields.e2e_p95_ms !== undefined) {
    summaryBits.push(`p95=${fields.e2e_p95_ms}ms`);
  }
  console.log(`  ${summaryBits.join('  ')}`);
}

function recordBench(suite, label, rep, params, result, rawCsv, extra = {}) {
  const p = parseBench(result.out);
  record(suite, label, {
    ...extra,
    rep, ...params,
    submitted: p.submitted, completed: p.completed, rejected: p.rejected,
    succeeded: p.succeeded, failed: p.failed,
    wall_s: p.wall_s, throughput_per_s: p.throughput, submit_rate_per_s: p.submit_rate,
    store_batches: p.store_batches, store_writes: p.store_writes,
    rows_per_transaction: p.rows_per_tx,
    e2e_mean_ms: p.e2e.mean, e2e_p50_ms: p.e2e.p50, e2e_p95_ms: p.e2e.p95, e2e_p99_ms: p.e2e.p99,
    queue_p50_ms: p.queue.p50, queue_p95_ms: p.queue.p95,
    exec_p50_ms: p.exec.p50, exec_p95_ms: p.exec.p95,
    submit_call_p50_ms: p.submit_call.p50, submit_call_p99_ms: p.submit_call.p99,
    cpu_seconds: p.cpu_seconds, cores_used: p.cores_used, peak_rss_mb: p.peak_rss_mb,
    ctx_switches: p.ctx_switches,
    raw_csv: rawCsv ? path.relative(ROOT, rawCsv) : '',
  });
  return p;
}

// ---- environment record --------------------------------------------------------------
function writeEnvironment() {
  const lines = [
    `timestamp_utc=${new Date().toISOString()}`,
    `os=${run('uname', ['-srm']).out.trim()}`,
    `cpu=${run('sh', ['-c', 'sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown']).out.trim()}`,
    `logical_cores=${run('sh', ['-c', 'sysctl -n hw.ncpu 2>/dev/null || nproc']).out.trim()}`,
    `physical_cores=${run('sh', ['-c', 'sysctl -n hw.physicalcpu 2>/dev/null || echo unknown']).out.trim()}`,
    `memory_bytes=${run('sh', ['-c', 'sysctl -n hw.memsize 2>/dev/null || echo unknown']).out.trim()}`,
    `compiler=${run('sh', ['-c', 'c++ --version | head -1']).out.trim()}`,
    `cmake=${run('sh', ['-c', 'cmake --version | head -1']).out.trim()}`,
    `sqlite=${run('sh', ['-c', 'sqlite3 --version 2>/dev/null | cut -d" " -f1 || echo unknown']).out.trim()}`,
    `build_type=Release`,
    `repetitions_per_configuration=${REPS}`,
    `quick_mode=${quick}`,
    'note=benchmarks should be run on an otherwise idle machine; a concurrent build or',
    'note=heavy background process changes these numbers by more than a factor of two.',
  ];
  fs.writeFileSync(path.join(RESULTS, 'env.txt'), lines.join('\n') + '\n');
  console.log(lines.slice(0, 6).map((l) => `  ${l}`).join('\n'));
}

// ---- suites -----------------------------------------------------------------------------

async function suiteWorkerScaling() {
  for (const [label, type, payload, jobs] of [
    ['noop', 'noop', '{}', n(20000)],
    ['sleep5ms', 'sleep', '{"ms":5}', n(4000)],
    ['cpu_hash200k', 'cpu_hash', '{"iterations":200000}', n(4000)],
  ]) {
    for (const workers of [1, 2, 4, 8]) {
      for (let rep = 1; rep <= REPS; rep++) {
        const raw = path.join(RAW, `worker_scaling_${label}_w${workers}_r${rep}.csv`);
        const r = bench(['--workers', String(workers), '--jobs', String(jobs), '--submitters', '4',
                         '--type', type, '--payload', payload, '--out', raw,
                         '--label', `${label}-w${workers}`]);
        recordBench('worker_scaling', `${label}_w${workers}`, rep,
                    { workers, submitters: 4, job_type: type, payload, jobs }, r, raw);
      }
    }
  }
}

async function suiteJobDuration() {
  for (const [label, type, payload, jobs] of [
    ['0ms_noop', 'noop', '{}', n(20000)],
    ['1ms', 'sleep', '{"ms":1}', n(8000)],
    ['5ms', 'sleep', '{"ms":5}', n(4000)],
    ['20ms', 'sleep', '{"ms":20}', n(2000)],
    ['cpu_50k', 'cpu_hash', '{"iterations":50000}', n(8000)],
    ['cpu_1m', 'cpu_hash', '{"iterations":1000000}', n(2000)],
  ]) {
    for (let rep = 1; rep <= REPS; rep++) {
      const raw = path.join(RAW, `duration_${label}_r${rep}.csv`);
      const r = bench(['--workers', '4', '--jobs', String(jobs), '--submitters', '4',
                       '--type', type, '--payload', payload, '--out', raw,
                       '--label', `duration-${label}`]);
      recordBench('job_duration', label, rep,
                  { workers: 4, submitters: 4, job_type: type, payload, jobs }, r, raw);
    }
  }
}

async function suitePriority() {
  for (const mix of ['0:100', '0:50,9:50', '0:80,5:15,9:5']) {
    for (let rep = 1; rep <= REPS; rep++) {
      const safe = mix.replace(/[:,]/g, '_');
      const raw = path.join(RAW, `priority_${safe}_r${rep}.csv`);
      const r = bench(['--workers', '2', '--jobs', String(n(4000)), '--submitters', '4',
                       '--type', 'sleep', '--payload', '{"ms":1}', '--priorities', mix,
                       '--out', raw, '--label', `priority-${mix}`]);
      recordBench('priority', mix, rep,
                  { workers: 2, submitters: 4, job_type: 'sleep', payload: '{"ms":1}',
                    priorities: mix, jobs: n(4000) }, r, raw);
    }
  }
}

async function suiteQueueCapacity() {
  for (const cap of [100, 1000, 10000, 200000]) {
    for (let rep = 1; rep <= REPS; rep++) {
      const raw = path.join(RAW, `capacity_${cap}_r${rep}.csv`);
      const r = bench(['--workers', '4', '--jobs', String(n(20000)), '--submitters', '8',
                       '--type', 'noop', '--queue-capacity', String(cap), '--out', raw,
                       '--label', `capacity-${cap}`]);
      // The rejection rate must be computed before the row is appended: rows are written
      // to summary.csv as they are produced, so patching one afterwards would leave the
      // column empty in the file.
      const parsed = parseBench(r.out);
      const submitted = Number(parsed.submitted) || 0;
      const rejected = Number(parsed.rejected) || 0;
      const errorRate =
          submitted + rejected > 0 ? (rejected / (submitted + rejected)).toFixed(6) : '';
      recordBench('queue_capacity', String(cap), rep,
                  { workers: 4, submitters: 8, job_type: 'noop', jobs: n(20000),
                    max_outstanding: cap }, r, raw, { error_rate: errorRate });
    }
  }
}

async function suiteFailureHeavy() {
  for (const [label, payload, attempts] of [
    ['fail0pct', '{"fail_probability_pct":0}', 3],
    ['fail30pct', '{"fail_probability_pct":30}', 3],
    ['fail70pct', '{"fail_probability_pct":70}', 3],
  ]) {
    for (let rep = 1; rep <= REPS; rep++) {
      const raw = path.join(RAW, `failure_${label}_r${rep}.csv`);
      const r = bench(['--workers', '4', '--jobs', String(n(4000)), '--submitters', '4',
                       '--type', 'flaky', '--payload', payload,
                       '--max-attempts', String(attempts), '--out', raw,
                       '--label', `failure-${label}`]);
      recordBench('failure_heavy', label, rep,
                  { workers: 4, submitters: 4, job_type: 'flaky', payload,
                    max_attempts: attempts, jobs: n(4000) }, r, raw);
    }
  }
}

async function suiteGroupCommit() {
  for (const enabled of ['false', 'true']) {
    for (const workers of [1, 4, 8]) {
      for (let rep = 1; rep <= REPS; rep++) {
        const raw = path.join(RAW, `groupcommit_${enabled}_w${workers}_r${rep}.csv`);
        const r = bench(['--workers', String(workers), '--jobs', String(n(20000)),
                         '--submitters', '4', '--type', 'noop', '--group-commit', enabled,
                         '--out', raw, '--label', `groupcommit-${enabled}-w${workers}`]);
        recordBench('group_commit', `${enabled}_w${workers}`, rep,
                    { workers, submitters: 4, job_type: 'noop', jobs: n(20000),
                      group_commit: enabled }, r, raw);
      }
    }
  }
}

async function suiteWorkflow() {
  for (const [shape, nodes] of [['chain', 4], ['fanout', 6], ['diamond', 6]]) {
    for (const workers of [1, 4, 8]) {
      for (let rep = 1; rep <= REPS; rep++) {
        const raw = path.join(RAW, `workflow_${shape}${nodes}_w${workers}_r${rep}.csv`);
        const count = n(400);
        const r = bench(['--workers', String(workers), '--workflow', shape,
                         '--workflow-nodes', String(nodes), '--workflows', String(count),
                         '--type', 'sleep', '--payload', '{"ms":5}', '--out', raw,
                         '--label', `workflow-${shape}${nodes}-w${workers}`]);
        const g = (re, i = 1) => { const m = re.exec(r.out); return m ? m[i] : ''; };
        const lat = /BENCH workflow_latency: n=(\d+) mean=([\d.]+)ms p50=([\d.]+)ms p90=([\d.]+)ms p95=([\d.]+)ms p99=([\d.]+)ms/.exec(r.out);
        record('workflow', `${shape}${nodes}_w${workers}`, {
          rep, workers, job_type: 'sleep', payload: '{"ms":5}',
          jobs: count * nodes,
          submitted: g(/workflows_submitted=(\d+)/), completed: g(/workflows_completed=(\d+)/),
          failed: g(/nodes_failed=(\d+)/),
          wall_s: g(/wall_s=([\d.]+)/),
          throughput_per_s: g(/throughput_workflows_per_s=([\d.]+)/),
          e2e_mean_ms: lat ? lat[2] : '', e2e_p50_ms: lat ? lat[3] : '',
          e2e_p95_ms: lat ? lat[5] : '', e2e_p99_ms: lat ? lat[6] : '',
          cpu_seconds: g(/cpu_seconds=([\d.]+)/), cores_used: g(/cores_used=([\d.]+)/),
          peak_rss_mb: g(/peak_rss_mb=([\d.]+)/), ctx_switches: g(/ctx_switches=(\d+)/),
          raw_csv: path.relative(ROOT, raw),
        });
      }
    }
  }
}

// Latency under a CONTROLLED offered load. The saturating suites above deliberately
// overload the engine, so their "latency" is really a measure of backlog depth. Here each
// job has an intended submission time and latency is measured from that time, which is the
// standard guard against coordinated omission.
async function suiteLatency() {
  for (const [label, type, payload] of [
    ['noop', 'noop', '{}'],
    ['sleep5ms', 'sleep', '{"ms":5}'],
  ]) {
    for (const rate of [100, 500, 1000, 2000, 4000]) {
      for (let rep = 1; rep <= REPS; rep++) {
        const jobs = Math.min(n(20000), Math.max(600, rate * 5));
        const raw = path.join(RAW, `latency_${label}_r${rate}_rep${rep}.csv`);
        const r = bench(['--workers', '4', '--jobs', String(jobs), '--submitters', '4',
                         '--type', type, '--payload', payload, '--rate', String(rate),
                         '--out', raw, '--label', `latency-${label}-${rate}ps`]);
        recordBench('latency', `${label}_${rate}ps`, rep,
                    { workers: 4, submitters: 4, job_type: type, payload, jobs }, r, raw);
      }
    }
  }
}

async function suiteHttpApi() {
  const server = await startServer(['--workers', '8', '--db', ':memory:', '--http-threads', '16']);
  try {
    for (const mode of ['submit', 'status', 'mixed', 'health']) {
      for (const connections of [1, 8, 32]) {
        for (let rep = 1; rep <= REPS; rep++) {
          const raw = path.join(RAW, `http_${mode}_c${connections}_r${rep}.csv`);
          const r = run(path.join(BUILD, 'jobengine-load'),
                        ['--host', '127.0.0.1', '--port', String(server.port),
                         '--connections', String(connections), '--requests', String(n(8000)),
                         '--mode', mode, '--type', 'noop', '--out', raw,
                         '--label', `http-${mode}-c${connections}`], { timeout: 900000 });
          const g = (re, i = 1) => { const m = re.exec(r.out); return m ? m[i] : ''; };
          const lat = /LOAD latency_all: n=(\d+) mean=([\d.]+)ms p50=([\d.]+)ms p90=([\d.]+)ms p95=([\d.]+)ms p99=([\d.]+)ms/.exec(r.out);
          record('http_api', `${mode}_c${connections}`, {
            rep, connections, job_type: mode,
            submitted: g(/requests=(\d+)/), completed: g(/ ok=(\d+)/),
            wall_s: g(/wall_s=([\d.]+)/),
            throughput_per_s: g(/throughput_rps=([\d.]+)/),
            error_rate: g(/error_rate=([\d.]+)/),
            e2e_mean_ms: lat ? lat[2] : '', e2e_p50_ms: lat ? lat[3] : '',
            e2e_p95_ms: lat ? lat[5] : '', e2e_p99_ms: lat ? lat[6] : '',
            cpu_seconds: g(/cpu_seconds=([\d.]+)/), cores_used: g(/cores_used=([\d.]+)/),
            peak_rss_mb: g(/peak_rss_mb=([\d.]+)/),
            raw_csv: path.relative(ROOT, raw),
          });
          // Let the engine drain between runs so the next one starts from an empty queue.
          await waitFor(async () =>
              (await http(server.port, 'GET', '/health')).json?.outstanding === 0, 120000);
        }
      }
    }
  } finally {
    await stopServer(server);
  }
}

async function suiteDistributed() {
  const JOBS = n(3000);
  for (const processes of [1, 2, 4, 8]) {
    for (let rep = 1; rep <= REPS; rep++) {
      // The coordinator needs a connection thread per worker thread (each long-polls) plus
      // headroom for the submitting client. The pool also grows on demand, but starting it
      // at the right size keeps the measurement about scheduling rather than about pool
      // growth.
      const server = await startServer(['--workers', '0', '--db', ':memory:',
                                        '--http-threads', String(processes * 2 + 16)]);
      const workers = [];
      for (let i = 0; i < processes; i++) {
        const p = new Proc(path.join(BUILD, 'jobengine-worker'),
                           ['--host', '127.0.0.1', '--port', String(server.port),
                            '--name', `bench-w${i}`, '--threads', '2',
                            '--poll-wait-ms', '200', '--log-level', 'error']);
        await waitForLine(p, /READY worker_id=/, 15000);
        workers.push(p);
      }
      await waitFor(async () =>
          (await http(server.port, 'GET', '/health')).json?.remote_workers_alive === processes,
          20000);

      const t0 = Date.now();
      const submissions = [];
      for (let i = 0; i < JOBS; i++) {
        submissions.push(http(server.port, 'POST', '/jobs',
                              { type: 'sleep', payload: { ms: 2 } }));
        if (submissions.length >= 64) { await Promise.all(submissions.splice(0)); }
      }
      await Promise.all(submissions);
      const drained = await waitFor(async () =>
          (await http(server.port, 'GET', '/health')).json?.outstanding === 0, 300000);
      const wall = (Date.now() - t0) / 1000;
      // A failed or timed-out metrics fetch must degrade the row, not crash the suite and
      // lose every result after it.
      const metricsRes = await http(server.port, 'GET', '/metrics');
      const counters = metricsRes.json?.counters || {};
      if (!metricsRes.json) {
        console.log(`  WARNING: ${processes}proc rep ${rep}: /metrics unavailable ` +
                    `(${metricsRes.error || `status ${metricsRes.status}`})`);
      }

      record('distributed', `${processes}proc`, {
        rep, worker_processes: processes, workers: processes * 2, job_type: 'sleep',
        payload: '{"ms":2}', jobs: JOBS,
        submitted: counters.jobs_submitted ?? '', completed: counters.jobs_succeeded ?? '',
        succeeded: counters.jobs_succeeded ?? '', failed: counters.jobs_failed ?? '',
        wall_s: wall.toFixed(4),
        throughput_per_s: drained ? (JOBS / wall).toFixed(1) : '',
      });

      for (const w of workers) { w.kill('SIGTERM'); await w.waitExit(15000); }
      await stopServer(server);
    }
  }
}

// ---- driver ------------------------------------------------------------------------------

const SUITES = {
  worker_scaling: suiteWorkerScaling,
  job_duration: suiteJobDuration,
  priority: suitePriority,
  queue_capacity: suiteQueueCapacity,
  failure_heavy: suiteFailureHeavy,
  group_commit: suiteGroupCommit,
  latency: suiteLatency,
  workflow: suiteWorkflow,
  http_api: suiteHttpApi,
  distributed: suiteDistributed,
};

ensureBuild('build');
if (!APPEND_MODE && fs.existsSync(SUMMARY)) fs.rmSync(SUMMARY);
ensureSummaryHeader();
console.log(`\nbenchmark environment (${REPS} repetition(s) per configuration):`);
if (!fs.existsSync(path.join(RESULTS, 'env.txt')) || !APPEND_MODE) writeEnvironment();
else console.log('  (environment already recorded by an earlier suite in this run)');

const started = Date.now();
for (const [name, fn] of Object.entries(SUITES)) {
  if (!shouldRun(name)) continue;
  console.log(`\n=== ${name} ===`);
  await fn();
}

console.log(`\nwrote ${rows.length} rows to ${path.relative(ROOT, SUMMARY)} ` +
            `in ${((Date.now() - started) / 1000).toFixed(1)}s`);
console.log(`raw per-job and per-request samples are in ${path.relative(ROOT, RAW)}/`);
