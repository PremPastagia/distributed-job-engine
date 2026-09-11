// G3: ThreadSanitizer finds no data race across the concurrency-bearing tests.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, run, runGtest, assertGtest, check, checkEq, finish, cpuCount } from './_lib.mjs';

const dir = path.join(ROOT, 'build-tsan');
if (!fs.existsSync(path.join(dir, 'CMakeCache.txt'))) {
  const cfg = run('cmake', ['-S', ROOT, '-B', dir, '-DCMAKE_BUILD_TYPE=Debug',
                            '-DJOBENGINE_SANITIZER=thread', '-DJOBENGINE_BUILD_TESTS=ON']);
  checkEq(cfg.code, 0, `tsan configure failed:\n${cfg.out.slice(-2000)}`);
}
const build = run('cmake', ['--build', dir, '-j', String(cpuCount())], { timeout: 1800000 });
checkEq(build.code, 0, `tsan build failed:\n${build.out.slice(-3000)}`);

// Verify the sanitizer is genuinely linked in, so a silent "0 warnings" cannot come from
// an accidentally unsanitised binary.
const linked = run('sh', ['-c', `nm -u ${path.join(dir, 'jobengine_tests')} | grep -c __tsan || true`]);
check(Number(linked.out.trim()) > 0, 'the test binary is not actually linked against ThreadSanitizer');

const r = runGtest('*', dir, { TSAN_OPTIONS: 'halt_on_error=0 exitcode=66' });
assertGtest(r, 'tsan suite', 50);

const races = (r.out.match(/WARNING: ThreadSanitizer/g) || []).length;
checkEq(races, 0, `ThreadSanitizer reported ${races} warning(s)`);
if (races > 0) {
  const first = r.out.slice(r.out.indexOf('WARNING: ThreadSanitizer'));
  console.error(first.slice(0, 3000));
}

finish('G3', [
  `ThreadSanitizer build verified linked; ran ${r.total} tests, ${r.passed} passed`,
  `ThreadSanitizer warnings: ${races}`,
]);
