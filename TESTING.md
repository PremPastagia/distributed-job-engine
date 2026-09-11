# TESTING

Two layers, with different jobs.

**GoogleTest suites** (`tests/`) test units and in-process integration: data structures,
the state machine, the engine, the HTTP parser, the REST API over real sockets, and the
distributed protocol driven by the real `RemoteWorker` class.

**Acceptance gates** (`scripts/gates/`) test claims end to end, usually against real
processes over real sockets, and each one prints `GATE GNN PASS` only if every assertion
inside it holds. A gate is what stands behind a sentence in the CV: if the sentence says
"a killed worker's jobs are reassigned and complete", a gate kills a worker process with
`SIGKILL` and watches the jobs complete.

---

## Running everything

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/jobengine_tests                       # the whole suite
./build/jobengine_tests --gtest_filter='QueueTest.*'
./build/jobengine_tests --verbose-engine-logs # keep the engine's INFO logs

for g in scripts/gates/g*.mjs; do node "$g" || echo "FAILED: $g"; done
```

Sanitizers:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DJOBENGINE_SANITIZER=thread
cmake --build build-tsan -j && ./build-tsan/jobengine_tests

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DJOBENGINE_SANITIZER=address
cmake --build build-asan -j && ./build-asan/jobengine_tests
```

---

## What the test suites cover

| Suite | File | What it establishes |
|---|---|---|
| `JsonTest` | `tests/test_json.cpp` | scalars, nesting, escapes and surrogate pairs, integer/double distinction, round-trips, rejection of 16 malformed inputs, nesting-depth limit against hostile input |
| `JobStateTest` | `tests/test_job_state.cpp` | name round-trips, terminal states are exactly the documented five, terminal states are absorbing, self-transitions rejected, and the full transition matrix checked against an independently written table |
| `RetryPolicyTest` | `tests/test_job_state.cpp` | the exponential formula, the cap, overflow safety at attempt 1000, jitter bounds, clamping |
| `QueueTest` | `tests/test_queue.cpp` | strict priority with FIFO tie-break against an independently computed order, negative priorities, delayed entries, eligibility timestamps, tombstoned removal, re-push replacing an entry, capacity, close semantics, and a multi-producer/multi-consumer run asserting no loss and no duplicate delivery |
| `StoreTest` | `tests/test_store.cpp` | every field round-trips including a full 64-bit lease token, listings and pagination, per-attempt records with primary-key idempotency, atomic workflow insert with rollback on conflict, recovery listing, worker records, survival across reopening the file, concurrent writers and readers, and that two `:memory:` stores are independent |
| `EngineTest` | `tests/test_engine.cpp` | execution and result recording, unknown-type rejection leaving no row, a throwing handler becoming a failed job, admission control, priority and FIFO execution order, delayed jobs, no-loss/no-duplicate under 4 producers and 6 workers, graceful shutdown with work in flight, draining shutdown, restart recovery, metrics, and one-shot completion callbacks |
| `RetryTest` | `tests/test_retry_timeout.cpp` | exact retry counts, stopping on first success, retry being opt-in, measured backoff gaps, and a retrying job staying outstanding |
| `TimeoutTest` | `tests/test_retry_timeout.cpp` | cooperative deadline enforcement, timeouts consuming attempts, and a handler that ignores its cancellation flag holding its thread - the documented limit, demonstrated |
| `CancelTest` | `tests/test_retry_timeout.cpp` | queued/running/terminal cancellation, delayed jobs not resurrecting, and a 40-round cancel-versus-start race resolving to exactly one outcome |
| `WorkflowTest` | `tests/test_workflow.cpp` | linear order, parallel branches, diamond fan-in, cycle and dangling-edge rejection persisting nothing, transitive skipping on failure, retry inside a workflow, cancellation, priority within the ready set, and a 61-node fan-out |
| `HttpParserTest` | `tests/test_http.cpp` | request-line and header parsing, byte-at-a-time incremental feeding, query and percent decoding, keep-alive defaults per HTTP version, nine malformed inputs with their exact status codes, chunked-encoding rejection, header/body/target limits, and pipelined leftover bytes |
| `HttpServerTest` | `tests/test_http.cpp` | real socket round-trips, keep-alive reusing one connection for 50 requests, handler exceptions becoming 500s, and 12 concurrent clients |
| `ApiTest` | `tests/test_api.cpp` | every endpoint's success and failure contract, twelve invalid-request shapes, backpressure as 503, workflow creation and rejection reasons, metrics and route introspection, 320 concurrent submissions with unique ids, and the explicit absence of request de-duplication |
| `DistributedTest` | `tests/test_distributed.cpp` | registration and heartbeats, claim/complete, three kinds of stale-lease rejection, lease expiry with reassignment, bounded reassignment, dead-worker detection, cancellation delivered through heartbeats, and real `RemoteWorker` instances sharing a workload |

## What each acceptance gate proves

| Gate | Claim it stands behind |
|---|---|
| `g01_build.mjs` | configures and builds Release and Debug with `-Wall -Wextra -Wpedantic -Werror`, producing all five binaries with zero warnings in project sources |
| `g02_tests.mjs` | the whole suite passes, with counts read from the runner's own output, and every expected suite is present in the binary |
| `g03_tsan.mjs` | the test binary is genuinely linked against ThreadSanitizer and reports zero data races |
| `g04_asan.mjs` | AddressSanitizer and UndefinedBehaviorSanitizer report zero errors; the platform leak checker reports zero leaks |
| `g05_no_loss.mjs` | 600 jobs over HTTP and 5,000 through the driver: every job executed exactly once, verified from the database and from per-attempt records, not from the engine's own counters |
| `g06_priority.mjs` | strict priority ordering, plus a measurement showing priority 9 waiting less than priority 0 at p50 and p95 in the same run |
| `g07_shutdown.mjs` | `SIGTERM` with work in flight exits 0 within its bound, leaves nothing `RUNNING`, and accounts for every job |
| `g08_api.mjs` | 61 contract assertions against a live server, including 16 invalid-request cases and 120 simultaneous submissions |
| `g09_restart.mjs` | a `SIGKILL`ed coordinator loses nothing: 50 queued and 1 running job are recovered on restart and completed, with the interrupted attempt still counted |
| `g10_retry.mjs` | exact retry counts, early stop on success, and measured backoff gaps compared against the formula computed independently in the gate |
| `g11_timeout.mjs` | deadlines enforced and recorded as `TIMED_OUT`, timeouts consuming attempts, and the uncooperative-handler limit demonstrated |
| `g12_cancel.mjs` | cancellation of queued, delayed and running jobs, 409 on terminal jobs, and 150 cancel-versus-start races each settling in exactly one state |
| `g13_workflow.mjs` | dependency order verified from recorded timestamps, measured parallelism against the critical path, five rejection reasons, transitive skipping, and workflow cancellation |
| `g14_distributed.mjs` | a coordinator with **zero** embedded workers plus three separate worker processes completes 400 jobs, with per-process execution counts proving the work was actually spread |
| `g15_worker_failure.mjs` | a `SIGKILL`ed worker's jobs are detected, requeued and completed by a replacement, and the dead worker's late result is rejected by its fencing token |
| `g16_bench_artifacts.mjs` | the benchmark suite reruns and emits the CSVs, headers, plots and generated tables that `BENCHMARKS.md` depends on |
| `g17_claims.mjs` | every number in the CV document is recomputed from raw samples, the document is re-rendered from its template, and no number in the CV bullets lacks a measurement |
| `g18_api_drift.mjs` | the routes the server advertises are reachable and exactly match the routes documented in `docs/ARCHITECTURE.md` |
| `g19_docs.mjs` | every deliverable document exists, is substantial, has no placeholders, no unqualified over-claims, and no broken cross-references |
| `g20_static.mjs` | the clang static analyzer finds no defect, with a positive control proving it is running and a committed probe fixture proving that the one excluded finding is a known checker false positive |

## Concurrency-specific coverage

| Hazard | How it is checked |
|---|---|
| Data races | ThreadSanitizer over the whole suite (`g03`), which exercises 4-6 producer threads, 6-8 worker threads, HTTP connection threads, the watchdog, and the store's flusher |
| Deadlock | a fixed lock order (`workflow > queue > store`; the running-state lock is never nested with the workflow lock), no handler invoked while any engine lock is held, and every test bounded by a timeout so a deadlock fails rather than hangs |
| Lost or duplicated work | `g05`, plus `QueueTest` and `EngineTest` asserting exact set equality between submitted and executed ids |
| Starvation | strict priority can starve low priority by design; `g06` measures the cost instead of denying it |
| Priority inversion | not applicable: no priority is inherited, and no lock is held across job execution |
| Torn state on shutdown | `g07` reads the database after the process is gone and asserts nothing is left `RUNNING` |
| Database inconsistency | `StoreTest` rollback tests, the `(job_id, attempt)` primary key, and `g09` reading the file back after both processes exited |
| Memory errors and leaks | AddressSanitizer, UndefinedBehaviorSanitizer and the platform leak checker (`g04`) |

## Known gaps in the testing

Stated rather than left to be discovered:

- **No multi-machine test.** Everything runs on one host. The worker protocol makes no
  localhost assumption, but "works across machines" is untested and is not claimed.
- **No Docker verification.** Docker is not installed on the development machine, so the
  `Dockerfile` has never been built or run (`docs/DESIGN_DECISIONS.md` D-12).
- **LeakSanitizer is unavailable on this platform** (macOS/arm64). Leak coverage comes from
  the platform `leaks` tool against the ordinary Release binary instead, which is weaker
  than LSan's per-allocation reporting.
- **No fuzzing.** The JSON and HTTP parsers are tested against a fixed corpus of malformed
  inputs, not a fuzzer.
- **No sustained soak test.** The longest continuous run is the benchmark suite; there is
  no multi-hour stability run, so slow leaks or drift would not have been caught.
- **Clock assumptions are untested.** Scheduling uses the steady clock, but no test forces
  a wall-clock step to prove the two are correctly separated.
