// G13: workflow DAGs execute in dependency order, reject invalid graphs, propagate failure
// to descendants, and run independent branches in parallel.
import { ensureBuild, runGtest, assertGtest, check, checkEq, finish, startServer, stopServer,
         http, waitFor } from './_lib.mjs';

const buildDir = ensureBuild('build');
const unit = runGtest('WorkflowTest.*', buildDir);
assertGtest(unit, 'workflow tests', 8);

const server = await startServer(['--workers', '4', '--db', ':memory:']);

const createWorkflow = (name, nodes, edges) =>
    http(server.port, 'POST', '/workflows', { name, nodes, edges });
const workflowState = async (id) =>
    (await http(server.port, 'GET', `/workflows/${id}`)).json;

// 1) A linear document-ingestion style pipeline runs in order.
const linear = await createWorkflow('ingest',
    [{ name: 'extract', type: 'sleep', payload: { ms: 40 } },
     { name: 'chunk', type: 'sleep', payload: { ms: 40 } },
     { name: 'embed', type: 'sleep', payload: { ms: 40 } },
     { name: 'index', type: 'sleep', payload: { ms: 40 } }],
    [{ from: 'extract', to: 'chunk' }, { from: 'chunk', to: 'embed' },
     { from: 'embed', to: 'index' }]);
checkEq(linear.status, 201, `linear workflow rejected: ${linear.body}`);
const linearId = linear.json.workflow_id;
await waitFor(async () => (await workflowState(linearId)).state === 'SUCCEEDED', 30000);
const linearWf = await workflowState(linearId);
checkEq(linearWf.state, 'SUCCEEDED', 'the linear workflow did not succeed');
checkEq(linearWf.nodes.length, 4, 'the workflow lost a node');

// Dependency order is verified from the recorded start timestamps: every node must start
// only after each of its parents finished.
const byName = Object.fromEntries(linearWf.nodes.map((n) => [n.workflow_node, n]));
const chain = ['extract', 'chunk', 'embed', 'index'];
for (let i = 1; i < chain.length; i++) {
  const parent = byName[chain[i - 1]];
  const child = byName[chain[i]];
  check(child.started_at_ms >= parent.finished_at_ms,
        `${chain[i]} started at ${child.started_at_ms} before ${chain[i - 1]} finished at ` +
        `${parent.finished_at_ms}`);
}

// 2) Independent branches overlap: measured wall time must beat serial execution.
const NODE_MS = 300;
const t0 = Date.now();
const fan = await createWorkflow('fanout',
    [{ name: 'root', type: 'sleep', payload: { ms: NODE_MS } },
     { name: 'a', type: 'sleep', payload: { ms: NODE_MS } },
     { name: 'b', type: 'sleep', payload: { ms: NODE_MS } },
     { name: 'c', type: 'sleep', payload: { ms: NODE_MS } },
     { name: 'join', type: 'sleep', payload: { ms: NODE_MS } }],
    [{ from: 'root', to: 'a' }, { from: 'root', to: 'b' }, { from: 'root', to: 'c' },
     { from: 'a', to: 'join' }, { from: 'b', to: 'join' }, { from: 'c', to: 'join' }]);
checkEq(fan.status, 201, `fan-out workflow rejected: ${fan.body}`);
const fanId = fan.json.workflow_id;
await waitFor(async () => (await workflowState(fanId)).state === 'SUCCEEDED', 60000);
const fanElapsed = Date.now() - t0;
const serialTime = 5 * NODE_MS;
const criticalPath = 3 * NODE_MS;
check(fanElapsed < serialTime * 0.85,
      `fan-out took ${fanElapsed}ms, close to the ${serialTime}ms serial time: no parallelism`);
check(fanElapsed >= criticalPath * 0.9,
      `fan-out finished in ${fanElapsed}ms, faster than its ${criticalPath}ms critical path`);
const fanWf = await workflowState(fanId);
const branch = ['a', 'b', 'c'].map((n) => fanWf.nodes.find((x) => x.workflow_node === n));
const overlap = branch.some((x, i) => branch.some((y, j) =>
    i !== j && x.started_at_ms < y.finished_at_ms && y.started_at_ms < x.finished_at_ms));
check(overlap, 'the independent branches never overlapped in time');

// 3) Invalid graphs are rejected with a reason and persist nothing.
const before = (await http(server.port, 'GET', '/jobs')).json.total;
const invalid = [
  ['cycle_detected', [{ name: 'a', type: 'noop' }, { name: 'b', type: 'noop' }],
   [{ from: 'a', to: 'b' }, { from: 'b', to: 'a' }]],
  ['self_edge', [{ name: 'a', type: 'noop' }], [{ from: 'a', to: 'a' }]],
  ['unknown_node_reference', [{ name: 'a', type: 'noop' }], [{ from: 'a', to: 'ghost' }]],
  ['duplicate_node_name', [{ name: 'a', type: 'noop' }, { name: 'a', type: 'noop' }], []],
  ['unknown_type', [{ name: 'a', type: 'ghost_type' }], []],
];
for (const [reason, nodes, edges] of invalid) {
  const res = await createWorkflow('bad', nodes, edges);
  checkEq(res.status, 400, `${reason}: expected 400, got ${res.status}`);
  checkEq(res.json?.reason, reason, `${reason}: got reason "${res.json?.reason}"`);
}
const after = (await http(server.port, 'GET', '/jobs')).json.total;
checkEq(after, before, 'a rejected workflow created job rows');

// 4) A failed node skips every descendant while an unrelated branch still completes.
const partial = await createWorkflow('partial',
    [{ name: 'a', type: 'noop' }, { name: 'bad', type: 'fail' },
     { name: 'c', type: 'noop' }, { name: 'd', type: 'noop' }, { name: 'e', type: 'noop' }],
    [{ from: 'a', to: 'bad' }, { from: 'bad', to: 'c' }, { from: 'c', to: 'd' },
     { from: 'a', to: 'e' }]);
checkEq(partial.status, 201, 'the partial-failure workflow was rejected');
const partialId = partial.json.workflow_id;
await waitFor(async () => (await workflowState(partialId)).state === 'FAILED', 30000);
const partialWf = await workflowState(partialId);
checkEq(partialWf.state, 'FAILED', 'a workflow with a failed node did not fail');
const states = Object.fromEntries(partialWf.nodes.map((n) => [n.workflow_node, n.state]));
checkEq(states.a, 'SUCCEEDED', 'the root node should have succeeded');
checkEq(states.bad, 'FAILED', 'the failing node should be FAILED');
checkEq(states.c, 'SKIPPED', 'the direct descendant of a failed node must be SKIPPED');
checkEq(states.d, 'SKIPPED', 'skipping must be transitive');
checkEq(states.e, 'SUCCEEDED', 'an unrelated branch must not be skipped');
const cAttempts = partialWf.nodes.find((n) => n.workflow_node === 'c').attempt;
checkEq(cAttempts, 0, 'a skipped node must never have been executed');

// 5) A node that recovers on retry does not fail the workflow.
const retrying = await createWorkflow('retrying',
    [{ name: 'a', type: 'noop' },
     { name: 'flaky', type: 'fail', payload: { fail_until_attempt: 2 }, max_attempts: 3,
       backoff_base_ms: 10, backoff_multiplier: 1.0 },
     { name: 'c', type: 'noop' }],
    [{ from: 'a', to: 'flaky' }, { from: 'flaky', to: 'c' }]);
const retryId = retrying.json.workflow_id;
await waitFor(async () => ['SUCCEEDED', 'FAILED'].includes((await workflowState(retryId)).state),
              30000);
checkEq((await workflowState(retryId)).state, 'SUCCEEDED',
        'a node that recovered on retry still failed the workflow');

// 6) Workflow cancellation.
const cancellable = await createWorkflow('cancelme',
    [{ name: 'a', type: 'sleep', payload: { ms: 5000 } },
     { name: 'b', type: 'sleep', payload: { ms: 5000 } }],
    [{ from: 'a', to: 'b' }]);
const cancelId = cancellable.json.workflow_id;
await waitFor(async () => (await workflowState(cancelId)).nodes.some((n) => n.state === 'RUNNING'),
              10000);
checkEq((await http(server.port, 'POST', `/workflows/${cancelId}/cancel`, '')).status, 200,
        'workflow cancellation failed');
await waitFor(async () => (await workflowState(cancelId)).nodes.every(
    (n) => ['CANCELLED', 'SKIPPED', 'FAILED', 'TIMED_OUT', 'SUCCEEDED'].includes(n.state)), 20000);
const cancelledWf = await workflowState(cancelId);
checkEq(cancelledWf.state, 'CANCELLED', 'the workflow was not marked cancelled');
check(cancelledWf.nodes.every((n) => n.state !== 'RUNNING' && n.state !== 'QUEUED'),
      'a node was left active after the workflow was cancelled');

const finalHealth = (await http(server.port, 'GET', '/health')).json;
checkEq(finalHealth.outstanding, 0, 'workflow jobs were left outstanding');

await stopServer(server);

finish('G13', [
  `unit: ${unit.passed}/${unit.total} workflow tests passed`,
  `linear 4-node pipeline: every node started only after its parent finished`,
  `fan-out (root -> a|b|c -> join, ${NODE_MS}ms nodes): ${fanElapsed}ms elapsed vs ` +
  `${serialTime}ms serial and ${criticalPath}ms critical path, branches overlapped`,
  `invalid graphs rejected with reasons: ${invalid.map((i) => i[0]).join(', ')}, nothing persisted`,
  `partial failure: descendants SKIPPED transitively, unrelated branch SUCCEEDED, workflow FAILED`,
  `workflow cancellation left no node active`,
]);
