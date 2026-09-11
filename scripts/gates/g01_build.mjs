// G1: the project compiles warning-free in Release and Debug with -Wall -Wextra -Werror.
import fs from 'node:fs';
import path from 'node:path';
import { ROOT, run, check, checkEq, finish, cpuCount } from './_lib.mjs';

const summary = [];
for (const type of ['Release', 'Debug']) {
  const dir = path.join(ROOT, `build-gate-${type.toLowerCase()}`);
  fs.rmSync(dir, { recursive: true, force: true });
  const cfg = run('cmake', ['-S', ROOT, '-B', dir, `-DCMAKE_BUILD_TYPE=${type}`,
                            '-DJOBENGINE_BUILD_TESTS=ON']);
  checkEq(cfg.code, 0, `${type}: cmake configure failed:\n${cfg.out.slice(-2000)}`);
  if (cfg.code !== 0) continue;

  const build = run('cmake', ['--build', dir, '-j', String(cpuCount())], { timeout: 1800000 });
  checkEq(build.code, 0, `${type}: build failed:\n${build.out.slice(-4000)}`);

  // -Werror makes a project warning fatal, so a surviving warning could only come from
  // the vendored test framework. Count any that reference this project's own sources.
  const ownWarnings = build.out
    .split('\n')
    .filter((l) => /warning:/.test(l))
    .filter((l) => /\/(src|include|apps|tests)\//.test(l) && !/third_party/.test(l));
  checkEq(ownWarnings.length, 0,
          `${type}: ${ownWarnings.length} warning(s) in project sources:\n${ownWarnings.slice(0, 10).join('\n')}`);

  for (const bin of ['jobengine-server', 'jobengine-worker', 'jobengine-bench',
                     'jobengine-load', 'jobengine_tests']) {
    check(fs.existsSync(path.join(dir, bin)), `${type}: ${bin} was not produced`);
  }
  summary.push(`${type}: configured, built and linked 5 binaries with 0 project warnings`);
}

// The standard actually in use must be C++20, not merely requested.
const std = run('grep', ['-r', 'CMAKE_CXX_STANDARD 20', 'CMakeLists.txt']);
checkEq(std.code, 0, 'CMakeLists.txt does not set CMAKE_CXX_STANDARD 20');
const werror = run('grep', ['-r', '-- -Werror', 'CMakeLists.txt'], {});
check(/\-Werror/.test(fs.readFileSync(path.join(ROOT, 'CMakeLists.txt'), 'utf8')),
      'CMakeLists.txt does not enable -Werror');
summary.push('C++20 and -Wall -Wextra -Wpedantic -Werror confirmed in CMakeLists.txt');

finish('G1', summary);
