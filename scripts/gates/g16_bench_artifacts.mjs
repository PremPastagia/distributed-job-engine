// G16: the benchmark suite reruns end to end and emits the CSVs, plots and recorded
// environment that BENCHMARKS.md relies on.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, run, check, checkEq, finish, parseCsv, csvComments, ensureBuild } from './_lib.mjs';

ensureBuild('build');
const RESULTS = path.join(ROOT, 'results');

// 1) The suite must actually rerun. A small slice proves the harness works today rather
// than proving that some old files happen to exist. It writes into a scratch directory:
// a check that overwrote the published results would be corrupting the evidence it exists
// to verify.
const scratch = path.join(RESULTS, 'gate_bench_rerun');
fs.rmSync(scratch, { recursive: true, force: true });
const rerun = run('node', [path.join(ROOT, 'scripts', 'run_benchmarks.mjs'),
                           '--quick', '--only', 'priority',
                           '--results-dir', path.relative(ROOT, scratch)], { timeout: 900000 });
checkEq(rerun.code, 0, `the benchmark suite failed to rerun:\n${rerun.out.slice(-2000)}`);
check(/wrote \d+ rows to/.test(rerun.out), 'the rerun did not report writing a summary');
check(fs.existsSync(path.join(scratch, 'summary.csv')),
      'the rerun did not produce a summary in its own directory');
check(fs.existsSync(path.join(scratch, 'raw')) && fs.readdirSync(path.join(scratch, 'raw')).length > 0,
      'the rerun produced no raw samples');

// 2) The full artifact set from the recorded run.
const summaryPath = path.join(RESULTS, 'summary.csv');
check(fs.existsSync(summaryPath), 'results/summary.csv is missing');
const summaryText = fs.readFileSync(summaryPath, 'utf8');
const summary = parseCsv(summaryText);
check(summary.rows.length >= 3, `summary.csv has only ${summary.rows.length} rows`);
for (const col of ['suite', 'label', 'rep', 'wall_s', 'throughput_per_s', 'raw_csv']) {
  check(summary.header.includes(col), `summary.csv is missing the ${col} column`);
}

// 3) The environment must be recorded, or a number cannot be attributed to a machine.
const envPath = path.join(RESULTS, 'env.txt');
check(fs.existsSync(envPath), 'results/env.txt is missing');
const env = fs.existsSync(envPath) ? fs.readFileSync(envPath, 'utf8') : '';
for (const field of ['timestamp_utc', 'os', 'cpu', 'logical_cores', 'compiler', 'cmake',
                     'build_type', 'repetitions_per_configuration']) {
  check(new RegExp(`^${field}=.+`, 'm').test(env), `env.txt does not record ${field}`);
}

// 4) Every raw file a summary row points at must exist, carry its own environment header,
// and contain data.
let checkedRaw = 0;
let missingRaw = 0;
let headerless = 0;
let emptyRaw = 0;
for (const row of summary.rows) {
  if (!row.raw_csv) continue;
  const p = path.join(ROOT, row.raw_csv);
  if (!fs.existsSync(p)) { missingRaw++; continue; }
  const text = fs.readFileSync(p, 'utf8');
  const comments = csvComments(text).join('\n');
  if (!/# cpu=/.test(comments) || !/# build_type=/.test(comments) ||
      !/# measurement:/.test(comments)) headerless++;
  if (parseCsv(text).rows.length === 0) emptyRaw++;
  checkedRaw++;
}
checkEq(missingRaw, 0, `${missingRaw} raw CSVs referenced by summary.csv do not exist`);
checkEq(headerless, 0, `${headerless} raw CSVs lack the environment/measurement header`);
checkEq(emptyRaw, 0, `${emptyRaw} raw CSVs contain no samples`);
check(checkedRaw >= 3, `only ${checkedRaw} raw sample files were produced`);

// 5) The analysis step must run and produce plots plus the generated tables.
const analyze = run('python3', [path.join(ROOT, 'scripts', 'analyze_benchmarks.py')],
                    { timeout: 600000 });
checkEq(analyze.code, 0, `the analysis script failed:\n${analyze.out.slice(-2000)}`);
const tablesPath = path.join(RESULTS, 'tables.md');
check(fs.existsSync(tablesPath), 'results/tables.md was not produced');
const tables = fs.existsSync(tablesPath) ? fs.readFileSync(tablesPath, 'utf8') : '';
check(tables.length > 500, 'the generated tables are suspiciously short');

const plotsDir = path.join(RESULTS, 'plots');
const plots = fs.existsSync(plotsDir) ? fs.readdirSync(plotsDir).filter((f) => f.endsWith('.png')) : [];
check(plots.length >= 4, `only ${plots.length} plots were produced`);
for (const p of plots) {
  const size = fs.statSync(path.join(plotsDir, p)).size;
  check(size > 5000, `plot ${p} is only ${size} bytes, probably empty`);
}

// 6) BENCHMARKS.md must exist and carry the generated section, so its numbers cannot drift
// away from the CSVs by hand-editing.
const benchDoc = path.join(ROOT, 'BENCHMARKS.md');
check(fs.existsSync(benchDoc), 'BENCHMARKS.md is missing');
if (fs.existsSync(benchDoc)) {
  const doc = fs.readFileSync(benchDoc, 'utf8');
  check(doc.includes('BEGIN GENERATED'), 'BENCHMARKS.md has no generated section marker');
  check(doc.includes('END GENERATED'), 'BENCHMARKS.md has no generated section end marker');
  const generated = doc.split('BEGIN GENERATED')[1] || '';
  check(generated.includes('|'), 'the generated section of BENCHMARKS.md contains no table');
  check(/Environment/.test(generated), 'the generated section does not record the environment');
  for (const cmd of ['run_benchmarks.mjs', 'analyze_benchmarks.py']) {
    check(doc.includes(cmd), `BENCHMARKS.md does not state how to rerun (${cmd} missing)`);
  }
}

const suites = [...new Set(summary.rows.map((r) => r.suite))];
finish('G16', [
  `suite reruns: --quick --only priority completed successfully`,
  `summary.csv: ${summary.rows.length} rows across suites [${suites.join(', ')}]`,
  `raw samples: ${checkedRaw} files, all present, all with environment and measurement headers`,
  `plots: ${plots.length} PNGs; tables.md regenerated (${tables.length} bytes)`,
  `BENCHMARKS.md carries a generated section and documents how to rerun`,
]);
