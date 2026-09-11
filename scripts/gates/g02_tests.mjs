// G2: the whole test suite passes; counts are read from the runner, never asserted by hand.
import { ensureBuild, runGtest, assertGtest, check, finish } from './_lib.mjs';

const buildDir = ensureBuild('build');
const r = runGtest('*', buildDir);
assertGtest(r, 'full suite', 50);

// Every area of the system must actually be covered, or "all tests pass" would be a claim
// about whichever tests happen to exist.
const expectedSuites = ['JsonTest', 'JobStateTest', 'RetryPolicyTest', 'QueueTest', 'StoreTest',
                        'EngineTest', 'TimeoutTest', 'CancelTest', 'WorkflowTest',
                        'HttpParserTest', 'HttpServerTest', 'ApiTest', 'DistributedTest'];
const listed = runGtest('*', buildDir, {}, ['--gtest_list_tests']).out;
for (const suite of expectedSuites) {
  check(listed.includes(`${suite}.`), `test suite ${suite} is missing from the binary`);
}

finish('G2', [
  `ran ${r.total} tests across ${r.suites} suites; ${r.passed} passed, ${r.failed} failed`,
  `covered suites: ${expectedSuites.join(', ')}`,
]);
