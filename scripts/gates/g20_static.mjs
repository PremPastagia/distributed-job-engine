// G20: the clang static analyzer reports no defect in the engine, queue, store, HTTP and
// API sources.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, run, check, checkEq, finish } from './_lib.mjs';

const sdk = run('xcrun', ['--show-sdk-path']).out.trim();
const sources = [
  'src/core/json.cpp', 'src/core/util.cpp', 'src/core/job.cpp', 'src/core/metrics.cpp',
  'src/queue/queue.cpp', 'src/store/store.cpp', 'src/engine/handlers.cpp',
  'src/engine/engine.cpp', 'src/http/http_server.cpp', 'src/http/http_client.cpp',
  'src/api/api.cpp', 'src/worker/remote_worker.cpp',
];

// The unix.BlockInCriticalSection checker misreports any blocking call on a path that went
// through std::condition_variable::wait, even when the lock is released first. Rather than
// assume that, the probe fixture demonstrates it, and only then are such findings excluded.
const PROBE = path.join(ROOT, 'scripts', 'gates', 'fixtures',
                        'block_in_critical_section_probe.cpp');
const probe = run('c++', ['--analyze', '-Xanalyzer', '-analyzer-output=text', '-std=c++20',
                          PROBE, '-o', '/dev/null']);
const probeWarnings = (probe.out.match(/warning:/g) || []).length;
const probeLines = probe.out.split('\n').filter((l) => /warning:/.test(l));
// The probe must show the checker working (case_d) AND misfiring (case_e, case_f).
const probeFunctions = new Set();
for (const line of probeLines) {
  const m = /probe\.cpp:(\d+):/.exec(line);
  if (!m) continue;
  const lineNo = Number(m[1]);
  const text = fs.readFileSync(PROBE, 'utf8').split('\n');
  for (let i = lineNo - 1; i >= 0; i--) {
    const fn = /^void (case_[a-z])\(/.exec(text[i]);
    if (fn) { probeFunctions.add(fn[1]); break; }
  }
}
check(probeFunctions.has('case_d'),
      'the probe did not flag case_d, so the BlockInCriticalSection checker is not running');
check(probeFunctions.has('case_e') && probeFunctions.has('case_f'),
      'the probe no longer reproduces the condition_variable false positive; the ' +
      'exclusion in this gate is no longer justified and must be removed');
for (const clean of ['case_a', 'case_b', 'case_c', 'case_g']) {
  check(!probeFunctions.has(clean),
        `the probe flagged ${clean}, which releases its lock without any condition variable: ` +
        'the exclusion rule would hide real findings');
}

// Confirm the analyzer is real before trusting a clean result: a file with a known defect
// must be reported. Without this control, a broken invocation would look like success.
const controlDir = path.join(ROOT, 'results', 'static_control');
fs.mkdirSync(controlDir, { recursive: true });
const controlFile = path.join(controlDir, 'known_defect.cpp');
fs.writeFileSync(controlFile, `
int deref() {
  int* p = nullptr;
  return *p;              // null dereference, the analyzer must find this
}
int leak() {
  int* q = new int(5);
  return *q;              // allocated and never freed
}
`);
const control = run('c++', ['--analyze', '-Xanalyzer', '-analyzer-output=text', '-std=c++20',
                            controlFile, '-o', '/dev/null']);
const controlFindings = (control.out.match(/warning:/g) || []).length;
check(controlFindings > 0,
      'the positive control produced no finding, so the analyzer is not actually running');

let realFindings = 0;
let excluded = 0;
const details = [];
const excludedDetails = [];
for (const src of sources) {
  const r = run('c++', ['--analyze', '-Xanalyzer', '-analyzer-output=text', '-std=c++20',
                        '-Iinclude', ...(sdk ? ['-I', `${sdk}/usr/include`] : []),
                        src, '-o', '/dev/null'], { timeout: 300000 });
  const warningLines = r.out.split('\n').filter((l) => /warning:/.test(l));
  check(r.code === 0 || warningLines.length > 0,
        `${src}: the analyzer failed to run: ${r.out.slice(-500)}`);
  const usesConditionVariable = /condition_variable|cv_\.wait|wb_work_cv_|wb_done_cv_|pool_cv_/
      .test(fs.readFileSync(path.join(ROOT, src), 'utf8'));
  for (const line of warningLines) {
    const isBlockChecker = /unix\.BlockInCriticalSection/.test(line);
    if (isBlockChecker && usesConditionVariable) {
      // Known false positive, demonstrated by the probe above. Reported, never hidden.
      excluded++;
      excludedDetails.push(`  excluded (condition_variable false positive): ${line.trim()}`);
      continue;
    }
    realFindings++;
    details.push(`  ${line.trim()}`);
  }
}
checkEq(realFindings, 0,
        `static analysis found ${realFindings} issue(s):\n${details.join('\n')}`);

finish('G20', [
  `positive control reported ${controlFindings} finding(s), so the analyzer is live`,
  `probe fixture confirms the checker flags a genuinely held lock (case_d) and misfires on ` +
  `condition_variable paths (case_e, case_f) while staying clean on case_a/b/c/g`,
  `analysed ${sources.length} translation units: ${realFindings} real findings, ` +
  `${excluded} excluded as the demonstrated false positive`,
  ...excludedDetails,
]);
