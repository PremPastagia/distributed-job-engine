<!-- RENDERED_AT -->
<!-- Do not edit CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md directly. Edit this template and run:
       node scripts/run_benchmarks.mjs && node scripts/extract_metrics.mjs && node scripts/render_docs.mjs
     Every double-brace metric marker is replaced with a value measured from results/. A placeholder
     whose key was never measured is an error, which is what makes an invented number
     impossible rather than merely discouraged. -->

# CV Pointers - Distributed Job Processing & Workflow Engine

## 1. Project title

**Distributed Job Processing & Workflow Engine** - a C++20 job scheduler and DAG workflow
engine with a REST API, durable metadata, and multi-process execution.

## 2. One-line description

A job-processing service that accepts work over HTTP, schedules it by priority across a
bounded worker pool (threads in one process or separate worker processes), enforces retries
and deadlines, and runs dependent jobs as validated DAGs - with every published number
backed by a re-runnable benchmark.

## 3. Technology stack actually used

| Layer | Choice | Why it is honest to list |
|---|---|---|
| Language | C++20 | `std::jthread`/`stop_token`, atomic wait/notify, `<semaphore>`, `<barrier>` all compiled and used; the build sets `CMAKE_CXX_STANDARD 20` |
| Build | CMake (>= 3.16), `-Wall -Wextra -Wpedantic -Werror` | the warning policy is enforced in CI-equivalent gate `g01` |
| Concurrency | `std::thread`, `std::mutex`, `std::condition_variable`, `std::shared_mutex`, `std::atomic` | the queue, engine, store and HTTP server are all built on these directly |
| Storage | SQLite 3 (WAL, `synchronous=NORMAL`), linked against the system library | schema in `docs/ARCHITECTURE.md` §5 |
| HTTP | hand-written HTTP/1.1 subset over BSD sockets (server + client) | no framework; the supported subset and its limits are documented in D-02 |
| JSON | hand-written recursive-descent parser/serialiser | full RFC 8259 value grammar with a nesting-depth limit |
| Tests | GoogleTest, pinned to `v1.15.2` | fetched by CMake on first configure; only the test target needs it |
| Sanitizers | ThreadSanitizer, AddressSanitizer, UndefinedBehaviorSanitizer | separate CMake build types, run as gates |
| Static analysis | clang static analyzer (`--analyze`) | run over all translation units, with a positive control |
| Benchmarks | own C++ drivers + Node orchestration + Python/matplotlib analysis | Python is used only for analysis and plotting |

**Not used, deliberately:** Redis, Kubernetes, any cloud service, any web framework, any
external JSON or HTTP dependency. A `Dockerfile` exists but has never been built (D-12) and
is excluded from every claim below.

## 4. Architecture summary

```
client --HTTP--> coordinator process (jobengine-server)
                   ApiRouter -> JobEngine
                                  |- WorkflowGraph   (DAG, indegree, failure propagation)
                                  |- PriorityJobQueue (ready max-heap + delayed min-heap,
                                  |                    one mutex + one condvar)
                                  |- WorkerPool       (K threads, embedded execution)
                                  |- LeaseTable       (remote assignment + fencing tokens)
                                  |- Watchdog         (deadlines, lease expiry, liveness)
                                  `- JobStore         (SQLite WAL, group commit,
                                                       read-through write-back cache)
                          ^
                          | HTTP: register / heartbeat / claim / complete
                   jobengine-worker x N  (separate OS processes)
```

- One coordinator owns the queue, the database and every scheduling decision.
- Execution happens either on embedded worker threads or in separate worker processes that
  lease jobs; both paths share the same handler registry, retry accounting and metrics.
- A job's durable record is written **before** the client is acknowledged; state transitions
  after that are batched (group commit) and served from a read-through cache so a reader
  never sees a stale state.

## 5. Important engineering decisions

1. **Persist before acknowledging, batch afterwards.** Submission waits for its row to be
   committed, so an accepted job is never lost. Subsequent transitions are buffered and
   group-committed, because they are recoverable by design (at-least-once).
2. **Group commit after profiling, not before.** The first implementation issued four
   SQLite transactions per job and got *slower* with more worker threads. The A/B in §6 is
   the measured result of fixing it.
3. **In-memory queue, durable record.** Scheduling decisions happen at memory speed; the
   database holds the truth and rebuilds the queue order on restart from `(priority, seq)`.
4. **Cooperative cancellation, honestly labelled.** There is no safe way to kill a thread
   mid-execution in C++, so deadlines set a flag that handlers poll. A test demonstrates
   that a handler ignoring the flag holds its worker - the limitation is proven, not hidden.
5. **Lease fencing instead of a distributed lock.** Each attempt carries a random 64-bit
   token; a result is accepted only from the current lease holder. This gives at-least-once
   execution with exactly-once *result recording*.
6. **Reassignment consumes an attempt.** A lease expiry counts against `max_attempts`, so a
   repeatedly crashing worker cannot cause unbounded re-execution.
7. **Admission control on outstanding jobs, not queue length.** Capacity bounds all
   non-terminal work, so `PENDING` and `RETRYING` jobs cannot grow without limit either.
8. **One transition table, one lock order.** Every state change goes through
   `is_valid_transition`; locks are taken in the order workflow > queue > store, and no
   handler ever runs while an engine lock is held.

## 6. Verified metrics

Measured on the machine recorded in `results/env.txt`, median of 3 repetitions per
configuration, percentiles computed from raw per-sample CSVs with the nearest-rank method.
Read `BENCHMARKS.md` for methodology, including why saturating and rate-limited runs are
reported separately.

{{METRICS_TABLE}}

## 7. Reproducible commands for each metric

```bash
# build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# every benchmark number in the table above
node scripts/run_benchmarks.mjs            # -> results/summary.csv, results/raw/*.csv
python3 scripts/analyze_benchmarks.py      # -> results/plots/*.png, regenerates BENCHMARKS.md
node scripts/extract_metrics.mjs           # -> results/verified_metrics.json
node scripts/render_docs.mjs                 # -> this document

# a single suite
node scripts/run_benchmarks.mjs --only worker_scaling
node scripts/run_benchmarks.mjs --only group_commit      # the A/B behind the group-commit claim
node scripts/run_benchmarks.mjs --only distributed       # multi-process scaling
node scripts/run_benchmarks.mjs --only http_api          # REST latency and throughput
node scripts/run_benchmarks.mjs --only latency           # rate-limited latency curves

# correctness claims, each printing GATE GNN PASS only if every assertion holds
node scripts/gates/g02_tests.mjs           # whole test suite
node scripts/gates/g03_tsan.mjs            # ThreadSanitizer
node scripts/gates/g04_asan.mjs            # AddressSanitizer + UBSan + leak check
node scripts/gates/g05_no_loss.mjs         # no lost or duplicated jobs
node scripts/gates/g06_priority.mjs        # priority ordering, measured
node scripts/gates/g09_restart.mjs         # SIGKILL the coordinator, recover, complete
node scripts/gates/g10_retry.mjs           # retry counts and measured backoff
node scripts/gates/g13_workflow.mjs        # DAG order, parallelism, failure propagation
node scripts/gates/g14_distributed.mjs     # 3 worker processes, per-process job counts
node scripts/gates/g15_worker_failure.mjs  # SIGKILL a worker, reassign, fence the late result
node scripts/gates/g17_claims.mjs          # re-derives every number in this document
```

## 8. Limitations

Each of these is a boundary of the work, stated so that an interviewer hears it from me
first:

- **At-least-once, not exactly-once.** A worker can execute a job and die before reporting.
  Fencing prevents a duplicate *result* from being recorded; it does not prevent duplicate
  *execution*. Handlers must be idempotent to use retries safely, which is why retry is
  opt-in and `max_attempts` defaults to `1`.
- **The coordinator is a single point of failure.** Its state survives in SQLite and is
  recovered on restart, but the service is unavailable while it is down. No replication,
  no consensus.
- **Cancellation and timeouts are cooperative in-process.** A handler that never polls its
  flag runs to completion; `TimeoutTest.UncooperativeHandlerHoldsItsThreadPastTheDeadline`
  demonstrates exactly this. Across processes, lease expiry does give a hard bound.
- **Single machine.** Everything was measured on one host over loopback. The worker
  protocol is plain TCP with no localhost assumption, but multi-machine operation is
  untested and is not claimed.
- **No authentication, authorisation or TLS.** Localhost or trusted network only.
- **Docker is unverified.** Docker is not installed on the development machine.
- **Strict priority can starve low-priority work.** This is a property of strict priority;
  §6 measures what the low-priority class pays rather than denying the effect.
- **Group commit trades a crash window for throughput.** A hard crash loses state
  transitions buffered since the last commit; those jobs are re-executed on recovery, which
  the at-least-once contract already covers. Submissions are never in that window.
- **Benchmarks are single-machine and sensitive to background load.** A concurrent build
  changed throughput by more than 2x during development, which is why repetitions and the
  environment are recorded.
- **Not fuzzed, not soak-tested.** The parsers are tested against a fixed corpus of
  malformed inputs; there is no multi-hour stability run.

## 9. Interview questions and defensible answers

**Q. Why write your own HTTP server instead of using Crow or cpp-httplib?**
Because the HTTP layer is small (about 600 lines for the subset I need) and writing it gave
me the client for the worker protocol as well, a dependency-free offline build, and direct
control of the parsing limits. The cost is real: it is not battle-tested against hostile
input, so I bounded the header size, body size and target length explicitly, rejected
chunked bodies with `411` instead of guessing, and unit-tested the parser against malformed
inputs separately from the socket layer. If this were going to face the public internet I
would put it behind a reverse proxy or swap in cpp-httplib.

**Q. Walk me through what happens when a worker dies mid-job.**
The worker holds a lease with an expiry and a random 64-bit token, and heartbeats to the
coordinator. When heartbeats stop for longer than the timeout, the watchdog marks the worker
`DEAD` and expires its leases immediately; a lease can also expire on its own deadline. The
job goes back to `QUEUED` and another worker claims it with a **new** token and the next
attempt number. If the original worker comes back and reports, `/internal/complete` compares
the token, rejects the result as `stale_lease`, and the job row is untouched. So the *result*
is recorded exactly once, but the job may have *executed* twice. `g15_worker_failure.mjs`
does this with a real `SIGKILL` and measures the detection and recovery times in §6.

**Q. Your throughput did not scale with worker threads at first. What did you do?**
I benchmarked worker counts and found throughput actually falling from 4 to 8 threads. The
engine was doing four SQLite writes per job - insert, transition to running, attempt record,
terminal update - and each bare statement is its own implicit transaction, so all of them
serialised behind one writer and more threads only added contention. I added group commit: a
single flusher thread drains whatever has accumulated and writes it as one transaction,
submitters block until the batch containing their row commits (so durability on acknowledge
is unchanged), and transitions are buffered behind a read-through cache so a reader never
sees a stale state. §6 has the controlled A/B - the same binary and workload, only the flag
changed.

**Q. Why is the flusher not on a timer?**
I measured that. A commit delay makes the batch bigger but throughput strictly worse, because
submitters are blocked waiting for the commit, so the system falls into lock-step: delay,
one batch, repeat. With no delay it self-tunes - whatever arrives while one transaction is
open becomes the next batch. The knob still exists (`--commit-delay-us`) because that is how
I measured it.

**Q. How do you know no job is lost or run twice?**
Three independent checks. The queue-level property test runs 4 producers and 4 consumers and
asserts exact set equality between pushed and popped ids. The engine-level test does the same
through the real executor with 6 worker threads. And `g05_no_loss.mjs` goes through the HTTP
API and verifies from the *database* - not from the engine's own counters - that every job
has exactly one recorded attempt.

**Q. What is your locking strategy, and how do you know it does not deadlock?**
Three locks with a fixed order: workflow > queue > store. The running-state lock doubles as
the job-transition lock and is never nested with the workflow lock - anything that needs both
collects what it needs, releases, and then acts. No handler is ever invoked while an engine
lock is held, which is what stops a slow job from blocking submissions. The evidence is
ThreadSanitizer over the whole suite with zero findings, plus every test being bounded by a
timeout so a deadlock fails rather than hangs.

**Q. Is priority scheduling fair?**
No, and deliberately. It is strict priority with FIFO within a level, so a continuous stream
of high-priority work can starve low priority. I measured the effect rather than hiding it:
§6 shows the queue wait of each class in the same run. If fairness mattered I would move to
weighted fair queueing or add ageing, which is a small change to the comparator.

**Q. Why SQLite and not PostgreSQL?**
There is exactly one writer by construction - a single coordinator - so SQLite's
single-writer limit costs nothing, and it removes a server, credentials and setup from the
reproduction steps. If the coordinator were replicated, the answer changes immediately:
SQLite would be the wrong choice and PostgreSQL with `SELECT ... FOR UPDATE SKIP LOCKED`
would be the natural queue.

**Q. What would you do differently or next?**
Three things, in order. First, remove the single coordinator - the honest fix is a
replicated queue, not a hand-rolled failover. Second, replace cooperative timeouts for
untrusted work with out-of-process execution, so a hostile handler cannot hold a thread.
Third, fuzz the HTTP and JSON parsers; they are tested against a fixed corpus, which is
weaker than it sounds.

## 10. CV bullets

Three versions. All numbers come from `results/`, and `scripts/gates/g17_claims.mjs`
re-derives every one of them from the raw samples.

### Conservative version

- Built a job-processing and workflow engine in `C++20` ({{source_lines_cpp}} lines of C++
  across a scheduler, thread-safe priority queue, SQLite store, `HTTP/1.1` server and REST
  API), verified by {{tests_total}} GoogleTest cases with zero ThreadSanitizer,
  AddressSanitizer and UndefinedBehaviorSanitizer findings.
- Implemented a {{job_states}}-state job lifecycle behind a single validated transition
  table, with exponential-backoff retries, per-job deadlines, cooperative cancellation and a
  dead-letter state, sustaining {{throughput_noop_1_worker_threads}} fully persisted job
  lifecycles per second.
- Added dependency workflows with DAG validation (Kahn's algorithm), transitive failure
  propagation and parallel execution of independent branches, cutting a fan-out workflow's
  median completion time from {{workflow_fanout6_w1_p50_ms}} ms on one worker thread to
  {{workflow_fanout6_w4_p50_ms}} ms on `4`.
- Extended the engine to multiple worker processes coordinated by lease-based assignment,
  heartbeats and fencing tokens, and verified recovery by killing a worker process with
  `SIGKILL` and confirming its jobs were reassigned and completed.

### Strong but fully defensible version

- Engineered a distributed job processing and workflow engine in `C++20`
  ({{source_lines_cpp}} lines of C++, {{test_lines_cpp}} lines of tests) with a
  mutex/condition-variable priority queue, a configurable worker pool, SQLite-backed durable
  metadata and a hand-written `HTTP/1.1` server; {{tests_total}} tests pass clean under
  ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer, and the engine sustains
  {{throughput_noop_1_worker_threads}} persisted job lifecycles per second.
- Profiled the engine and identified persistence rather than scheduling as the bottleneck -
  every job state transition was its own SQLite transaction, so throughput *fell* as worker
  threads were added; implemented group commit with a read-through write-back cache, raising
  throughput from {{group_commit_off_throughput_4_threads}} to
  {{group_commit_on_throughput_4_threads}} jobs/s ({{group_commit_speedup_4_threads}}x) at
  `4` worker threads by batching {{group_commit_rows_per_transaction_4_threads}} row writes
  per transaction, with the A/B reproducible from one binary by flipping a single flag.
- Measured worker-pool scaling across workload classes rather than one flattering case:
  I/O-bound jobs scale {{scaling_speedup_sleep5ms_8_threads}}x on `8` threads
  ({{scaling_efficiency_sleep5ms_8_threads}}% efficiency) and CPU-bound jobs
  {{scaling_speedup_cpu_hash200k_8_threads}}x, while a zero-work job peaks single-threaded
  because it has nothing to parallelise and only adds contention.
- Implemented lease-based distributed execution (a coordinator plus independent worker
  processes, heartbeats and `64`-bit fencing tokens) giving at-least-once execution with
  exactly-once result recording; throughput scaled from
  {{distributed_throughput_1_processes}} to {{distributed_throughput_8_processes}} jobs/s
  across `1` to `8` worker processes ({{distributed_speedup_8_processes}}x, reaching
  {{distributed_ceiling_utilisation_8_processes}}% of the theoretical ceiling those worker
  slots allow), and a `SIGKILL`ed worker's jobs were detected, reassigned and completed while
  its late result was rejected as a stale lease.
- Found and fixed a connection-starvation defect that the unit tests missed and the
  benchmark suite exposed: long-polling workers occupied every fixed connection thread and
  made the REST API unreachable; replaced the fixed pool with one that grows on demand and
  added a regression test that asserts the growth path fires.
- Served a REST API sustaining {{http_submit_c8_throughput}} job submissions/s at
  {{http_submit_c8_p50_ms}} ms p50 / {{http_submit_c8_p95_ms}} ms p95 /
  {{http_submit_c8_p99_ms}} ms p99 over `8` keep-alive connections with
  {{http_submit_c8_error_rate}}% errors, and implemented strict priority scheduling that cut
  median queue wait for high-priority jobs by {{priority_queue_wait_reduction_p50}}%
  ({{priority_9_queue_wait_p50_ms}} ms against {{priority_0_queue_wait_p50_ms}} ms in the
  same saturated run).

### Compact two-bullet version

- Built a `C++20` distributed job processing and workflow engine ({{source_lines_cpp}} lines
  of C++; thread-safe priority queue, worker pool, SQLite durability, REST API, DAG
  workflows, lease-based multi-process execution) sustaining
  {{throughput_noop_1_worker_threads}} persisted job lifecycles/s,
  {{http_submit_c8_throughput}} API submissions/s at {{http_submit_c8_p95_ms}} ms p95, and
  {{scaling_speedup_sleep5ms_8_threads}}x scaling on `8` worker threads;
  {{tests_total}} tests pass clean under ThreadSanitizer and AddressSanitizer.
- Profiled and removed the persistence bottleneck with group commit
  ({{group_commit_off_throughput_4_threads}} to {{group_commit_on_throughput_4_threads}}
  jobs/s, {{group_commit_speedup_4_threads}}x), scaled execution to
  {{distributed_throughput_8_processes}} jobs/s across `8` worker processes
  ({{distributed_speedup_8_processes}}x), and implemented fencing-token leases
  with heartbeat-based failure detection, verified by killing worker processes mid-job and
  confirming reassignment, completion and rejection of the stale result.

## 11. Evidence table

A claim is safe to mention only when a command in this repository reproduces it.

| CV claim | Evidence file | Test / benchmark command | Verified metric | Safe to mention? |
|---|---|---|---|---|
| Builds clean with `-Werror` in Release and Debug | `scripts/gates/g01_build.mjs` | `node scripts/gates/g01_build.mjs` | 0 warnings in project sources | **Yes** |
| {{tests_total}} tests pass | `tests/`, `scripts/gates/g02_tests.mjs` | `node scripts/gates/g02_tests.mjs` | `tests_total` = {{tests_total}} | **Yes** |
| Zero data races | `scripts/gates/g03_tsan.mjs` | `node scripts/gates/g03_tsan.mjs` | 0 ThreadSanitizer warnings | **Yes** |
| Zero memory errors or leaks | `scripts/gates/g04_asan.mjs` | `node scripts/gates/g04_asan.mjs` | 0 ASan/UBSan errors, 0 leaks | **Yes** |
| No job lost or executed twice | `scripts/gates/g05_no_loss.mjs` | `node scripts/gates/g05_no_loss.mjs` | every job has exactly 1 attempt record | **Yes** |
| Priority scheduling reduces high-priority queue wait | `results/raw/priority_*.csv` | `node scripts/gates/g06_priority.mjs` | `priority_queue_wait_reduction_p50` = {{priority_queue_wait_reduction_p50}}% | **Yes** |
| Graceful shutdown leaves nothing running | `scripts/gates/g07_shutdown.mjs` | `node scripts/gates/g07_shutdown.mjs` | 0 rows left `RUNNING` | **Yes** |
| REST API contract incl. 4xx/409/503 | `scripts/gates/g08_api.mjs` | `node scripts/gates/g08_api.mjs` | 61 live assertions | **Yes** |
| Survives coordinator `SIGKILL` | `scripts/gates/g09_restart.mjs` | `node scripts/gates/g09_restart.mjs` | 51 jobs recovered and completed | **Yes** |
| Retry count and exponential backoff | `scripts/gates/g10_retry.mjs` | `node scripts/gates/g10_retry.mjs` | measured gaps vs the formula | **Yes** |
| Deadline enforcement, and its cooperative limit | `scripts/gates/g11_timeout.mjs` | `node scripts/gates/g11_timeout.mjs` | `TIMED_OUT` recorded; limit demonstrated | **Yes** |
| Cancellation incl. the start race | `scripts/gates/g12_cancel.mjs` | `node scripts/gates/g12_cancel.mjs` | 150 races, one outcome each | **Yes** |
| DAG order, parallelism, failure propagation | `scripts/gates/g13_workflow.mjs` | `node scripts/gates/g13_workflow.mjs` | `workflow_fanout6_w4_p50_ms` = {{workflow_fanout6_w4_p50_ms}} ms | **Yes** |
| Work distributed across worker processes | `scripts/gates/g14_distributed.mjs` | `node scripts/gates/g14_distributed.mjs` | per-process execution counts | **Yes** |
| Worker failure detection and reassignment | `scripts/gates/g15_worker_failure.mjs` | `node scripts/gates/g15_worker_failure.mjs` | detection, requeue and recovery times | **Yes** |
| Throughput on `4` worker threads | `results/summary.csv` | `node scripts/run_benchmarks.mjs --only worker_scaling` | `throughput_noop_4_worker_threads` = {{throughput_noop_4_worker_threads}} jobs/s | **Yes** |
| Group-commit improvement | `results/summary.csv` | `node scripts/run_benchmarks.mjs --only group_commit` | `group_commit_speedup_4_threads` = {{group_commit_speedup_4_threads}}x | **Yes** |
| Multi-process scaling | `results/summary.csv` | `node scripts/run_benchmarks.mjs --only distributed` | `distributed_speedup_8_processes` = {{distributed_speedup_8_processes}}x | **Yes** |
| API latency percentiles | `results/raw/http_*.csv` | `node scripts/run_benchmarks.mjs --only http_api` | `http_submit_c8_p99_ms` = {{http_submit_c8_p99_ms}} ms | **Yes** |
| Static analysis clean | `scripts/gates/g20_static.mjs` | `node scripts/gates/g20_static.mjs` | 0 real findings, 1 documented false positive | **Yes** |
| "Production-grade" / "highly scalable" | - | - | - | **No** - no production deployment, no load beyond one machine |
| "Fault tolerant" | - | - | - | **No** - worker failure is handled; the coordinator is a single point of failure |
| "Exactly-once processing" | - | - | - | **No** - at-least-once execution; only result *recording* is exactly-once |
| "Runs across multiple machines" | - | - | - | **No** - protocol allows it, but it was never tested off one host |
| "Dockerised deployment" | `Dockerfile` | - | - | **No** - written but never built; Docker is not installed here |
| "Low latency" as an unqualified phrase | `results/raw/latency_*.csv` | `node scripts/run_benchmarks.mjs --only latency` | cite the measured p50/p95/p99 instead | **No** - quote the number, not the adjective |
