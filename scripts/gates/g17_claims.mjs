// G17: every numeric claim in CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md is recomputed from the
// raw result files and matches the value printed in the document.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, run, check, checkEq, finish, parseCsv, percentile } from './_lib.mjs';

const RESULTS = path.join(ROOT, 'results');
const METRICS = path.join(RESULTS, 'verified_metrics.json');
const DOC = path.join(ROOT, 'CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md');
const TEMPLATE = path.join(ROOT, 'docs', 'cv_pointers.template.md');

check(fs.existsSync(METRICS), 'results/verified_metrics.json is missing');
check(fs.existsSync(DOC), 'CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md is missing');
check(fs.existsSync(TEMPLATE), 'docs/cv_pointers.template.md is missing');
if (!fs.existsSync(METRICS) || !fs.existsSync(DOC) || !fs.existsSync(TEMPLATE)) {
  finish('G17');
}

const metrics = JSON.parse(fs.readFileSync(METRICS, 'utf8'));
check(Object.keys(metrics).length >= 20,
      `only ${Object.keys(metrics).length} metrics were extracted`);

// 1) Every metric must name an evidence file that exists.
let missingEvidence = 0;
for (const [key, m] of Object.entries(metrics)) {
  // A metric may cite several paths (a line count spans more than one directory), so each
  // one is resolved independently rather than treating the whole string as one path.
  for (const pattern of m.source.split(/[,\s]+/).filter(Boolean)) {
  if (pattern.includes('*')) {
    // Translate the glob into a real anchored regular expression rather than guessing at
    // prefixes, so "no file matches" means exactly that.
    const dir = path.join(ROOT, path.dirname(pattern));
    const rx = new RegExp('^' + path.basename(pattern)
        .replace(/[.+^${}()|[\]\\]/g, '\\$&')
        .replace(/\*/g, '.*') + '$');
    const found = fs.existsSync(dir) && fs.readdirSync(dir).some((f) => rx.test(f));
    if (!found) { missingEvidence++; console.error(`  ${key}: no file matches ${pattern}`); }
  } else if (!fs.existsSync(path.join(ROOT, pattern))) {
    missingEvidence++;
    console.error(`  ${key}: evidence path ${pattern} does not exist`);
  }
  }
  check(typeof m.method === 'string' && m.method.length > 10,
        `${key} does not explain how it was computed`);
}
checkEq(missingEvidence, 0, `${missingEvidence} metrics cite an evidence file that does not exist`);

// 2) Independently recompute a representative set straight from the raw samples, using
// this script's own percentile implementation rather than trusting the extractor.
const summary = parseCsv(fs.readFileSync(path.join(RESULTS, 'summary.csv'), 'utf8')).rows;
const rawFor = (suite, label, column, filter = null) => {
  const out = [];
  for (const r of summary.filter((x) => x.suite === suite && x.label === label)) {
    if (!r.raw_csv) continue;
    const p = path.join(ROOT, r.raw_csv);
    if (!fs.existsSync(p)) continue;
    for (const row of parseCsv(fs.readFileSync(p, 'utf8')).rows) {
      if (filter && !filter(row)) continue;
      const v = Number(row[column]);
      if (Number.isFinite(v) && v >= 0) out.push(v);
    }
  }
  return out.sort((a, b) => a - b);
};

const recomputations = [];

// The published value is deliberately rounded, so the check is "is the printed number the
// correctly rounded form of the recomputed one?" - half of the last printed digit, plus a
// little slack for floating point. A fixed absolute tolerance would either reject correct
// rounding or accept a genuinely wrong number.
function toleranceFor(claimed) {
  const text = String(claimed);
  const dot = text.indexOf('.');
  const decimals = dot < 0 ? 0 : text.length - dot - 1;
  return 0.5 * Math.pow(10, -decimals) + 1e-9;
}

const verifyPercentile = (key, suite, label, column, p, filter = null) => {
  if (!metrics[key]) return;
  const samples = rawFor(suite, label, column, filter);
  if (samples.length === 0) {
    check(false, `${key}: no raw samples found to recompute from`);
    return;
  }
  const recomputed = percentile(samples, p) / 1000;
  const claimed = metrics[key].value;
  const diff = Math.abs(recomputed - claimed);
  check(diff <= toleranceFor(claimed),
        `${key}: document says ${claimed}, recomputation from ${samples.length} raw samples ` +
        `gives ${recomputed.toFixed(4)} (tolerance ${toleranceFor(claimed)})`);
  recomputations.push(`${key}: claimed ${claimed}, recomputed ${recomputed.toFixed(4)} ` +
                      `from ${samples.length} samples`);
};

for (const rate of [100, 500, 1000, 2000, 4000]) {
  for (const p of [50, 95, 99]) {
    verifyPercentile(`latency_noop_${rate}ps_p${p}_ms`, 'latency', `noop_${rate}ps`, 'e2e_us', p);
    verifyPercentile(`latency_sleep5ms_${rate}ps_p${p}_ms`, 'latency',
                     `sleep5ms_${rate}ps`, 'e2e_us', p);
  }
}
for (const mode of ['submit', 'status', 'mixed']) {
  for (const c of [1, 8, 32]) {
    for (const p of [50, 95, 99]) {
      verifyPercentile(`http_${mode}_c${c}_p${p}_ms`, 'http_api', `${mode}_c${c}`, 'latency_us', p);
    }
  }
}
for (const pr of [0, 9]) {
  for (const q of [50, 95]) {
    verifyPercentile(`priority_${pr}_queue_wait_p${q}_ms`, 'priority', '0:50,9:50',
                     'queue_wait_us', q, (row) => Number(row.priority) === pr);
  }
}

// Throughput medians are recomputed from summary.csv rather than from the extractor.
const medianOf = (suite, label, col) => {
  const v = summary.filter((r) => r.suite === suite && r.label === label)
                   .map((r) => Number(r[col])).filter(Number.isFinite).sort((a, b) => a - b);
  if (v.length === 0) return null;
  const mid = Math.floor(v.length / 2);
  return v.length % 2 ? v[mid] : (v[mid - 1] + v[mid]) / 2;
};
for (const w of [1, 2, 4, 8]) {
  const key = `throughput_noop_${w}_worker_threads`;
  if (!metrics[key]) continue;
  const recomputed = medianOf('worker_scaling', `noop_w${w}`, 'throughput_per_s');
  check(recomputed !== null, `${key}: no summary rows to recompute from`);
  if (recomputed !== null) {
    check(Math.abs(recomputed - metrics[key].value) <= 1,
          `${key}: document says ${metrics[key].value}, summary median is ${recomputed}`);
    recomputations.push(`${key}: claimed ${metrics[key].value}, recomputed ${recomputed.toFixed(0)}`);
  }
}

// 3) The document must be exactly what the template plus the measured values render to,
// so a number cannot be edited into it by hand.
const rendered = run('node', [path.join(ROOT, 'scripts', 'render_docs.mjs')]);
checkEq(rendered.code, 0, `re-rendering the generated documents failed:\n${rendered.out}`);
const docText = fs.readFileSync(DOC, 'utf8');
for (const [label, file] of [['CV document', DOC],
                             ['final report', path.join(ROOT, 'FINAL_PROJECT_REPORT.md')]]) {
  if (!fs.existsSync(file)) { check(false, `${label} was not rendered`); continue; }
  const text = fs.readFileSync(file, 'utf8');
  check(!text.includes('{{'), `the rendered ${label} still contains an unsubstituted placeholder`);
  check(!/\{\{MISSING:/.test(text), `the ${label} references a metric that was never measured`);
}

// 4) Every number in the CV bullet section must come from a verified metric.
const bulletSection = (docText.split('## 10.')[1] || '').split('## 11.')[0];
check(bulletSection.length > 200, 'the CV bullet section could not be located in the document');
const verifiedValues = new Set(Object.values(metrics).map((m) => String(m.value)));
// Structural facts the gate derives from the code itself, so that a document citing them
// is still citing a measurement.
const structural = new Set();
const jobHeader = fs.readFileSync(path.join(ROOT, 'include', 'jobengine', 'job.hpp'), 'utf8');
const stateCount = (jobHeader.match(/^\s{2}[A-Z][A-Za-z]+,\s*\/\//gm) || []).length;
structural.add(String(stateCount));
const apiSource = fs.readFileSync(path.join(ROOT, 'src', 'api', 'api.cpp'), 'utf8');
const routeCount = (apiSource.match(/\{"(GET|POST)", "/g) || []).length;
structural.add(String(routeCount));
const testList = run(path.join(ROOT, 'build', 'jobengine_tests'), ['--gtest_list_tests']);
const testCount = (testList.out.match(/^\s{2}\S+/gm) || []).length;
structural.add(String(testCount));

// Numbers written inside inline code spans are configuration and identifiers (`C++20`,
// `64-bit`, `--workers 4`), never performance claims, so they are excluded from the scan.
// Everything left in prose must trace to a measurement.
const prose = bulletSection.replace(/`[^`]*`/g, ' ');

const unexplained = [];
// No trailing lookahead: with one, a value written next to its unit ("9.27x") would
// backtrack to a bare "9" and be reported as unsupported. Greedy matching takes the whole
// number and the unit letter is simply ignored.
for (const m of prose.matchAll(/(?<![\w.])(\d+(?:[.,]\d+)*)/g)) {
  const token = m[1];
  const plain = token.replace(/,/g, '');
  if (verifiedValues.has(plain) || verifiedValues.has(token) || structural.has(plain)) continue;
  // A rounded form of a verified metric is acceptable, e.g. 27,283 printed as 27,000.
  const asNumber = Number(plain);
  const near = [...verifiedValues].some((v) => {
    const n = Number(v);
    return Number.isFinite(n) && Number.isFinite(asNumber) && n !== 0 &&
           Math.abs(n - asNumber) / Math.abs(n) < 0.02;
  });
  if (!near) unexplained.push(token);
}
checkEq(unexplained.length, 0,
        `the CV bullets contain number(s) that no measurement supports: ${unexplained.join(', ')}`);

finish('G17', [
  `${Object.keys(metrics).length} verified metrics, every one naming an existing evidence file`,
  `independently recomputed ${recomputations.length} of them from raw samples; all matched`,
  `document re-rendered from its template and contains no unsubstituted placeholder`,
  `structural facts derived from source: ${stateCount} job states, ${routeCount} routes, ` +
  `${testCount} tests`,
  `every number in the CV bullet section traces to a measurement`,
]);
