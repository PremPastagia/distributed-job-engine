// G4: AddressSanitizer + UndefinedBehaviorSanitizer find no error, and the platform leak
// checker finds no leak.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, run, runGtest, assertGtest, check, checkEq, finish, cpuCount } from './_lib.mjs';

const dir = path.join(ROOT, 'build-asan');
if (!fs.existsSync(path.join(dir, 'CMakeCache.txt'))) {
  const cfg = run('cmake', ['-S', ROOT, '-B', dir, '-DCMAKE_BUILD_TYPE=Debug',
                            '-DJOBENGINE_SANITIZER=address', '-DJOBENGINE_BUILD_TESTS=ON']);
  checkEq(cfg.code, 0, `asan configure failed:\n${cfg.out.slice(-2000)}`);
}
const build = run('cmake', ['--build', dir, '-j', String(cpuCount())], { timeout: 1800000 });
checkEq(build.code, 0, `asan build failed:\n${build.out.slice(-3000)}`);

const linked = run('sh', ['-c', `nm -u ${path.join(dir, 'jobengine_tests')} | grep -c __asan || true`]);
check(Number(linked.out.trim()) > 0, 'the test binary is not actually linked against AddressSanitizer');

const r = runGtest('*', dir, { ASAN_OPTIONS: 'halt_on_error=0', UBSAN_OPTIONS: 'print_stacktrace=1' });
assertGtest(r, 'asan suite', 50);

const asanErrors = (r.out.match(/ERROR: AddressSanitizer/g) || []).length;
const ubsanErrors = (r.out.match(/runtime error:/g) || []).length;
checkEq(asanErrors, 0, `AddressSanitizer reported ${asanErrors} error(s)`);
checkEq(ubsanErrors, 0, `UndefinedBehaviorSanitizer reported ${ubsanErrors} error(s)`);

// LeakSanitizer is unavailable on macOS/arm64, so leaks are checked with the platform
// tool against the ordinary (unsanitised) Release binary instead.
const leakNotes = [];
const leaksAvailable = run('sh', ['-c', 'command -v leaks >/dev/null && echo yes || echo no']);
if (leaksAvailable.out.trim() === 'yes' && fs.existsSync(path.join(ROOT, 'build', 'jobengine_tests'))) {
  const lr = run('sh', ['-c',
    `MallocStackLogging=1 leaks --atExit -- ${path.join(ROOT, 'build', 'jobengine_tests')} ` +
    `--gtest_brief=1 2>&1 | tail -20`], { timeout: 1800000 });
  const m = /(\d+) leaks for (\d+) total leaked bytes/.exec(lr.out);
  check(m !== null, `the leak checker produced no verdict:\n${lr.out.slice(-1500)}`);
  if (m) {
    checkEq(Number(m[1]), 0, `leak checker found ${m[1]} leak(s), ${m[2]} bytes`);
    leakNotes.push(`macOS leaks: ${m[1]} leaks, ${m[2]} bytes leaked`);
  }
} else {
  leakNotes.push('platform leak checker unavailable; leak coverage NOT verified');
}

finish('G4', [
  `AddressSanitizer + UndefinedBehaviorSanitizer: ${r.total} tests, ${r.passed} passed`,
  `ASan errors: ${asanErrors}, UBSan errors: ${ubsanErrors}`,
  ...leakNotes,
]);
