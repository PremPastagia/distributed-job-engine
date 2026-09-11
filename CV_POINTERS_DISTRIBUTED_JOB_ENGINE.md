<!-- rendered from docs/cv_pointers.template.md -->
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

| metric | value | unit | evidence file | how it was computed |
|---|---|---|---|---|
| `backpressure_cap10000_rejection_rate` | 0 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=10000 under a saturating submit rate |
| `backpressure_cap1000_rejection_rate` | 86.2 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=1000 under a saturating submit rate |
| `backpressure_cap100_rejection_rate` | 97.3 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=100 under a saturating submit rate |
| `backpressure_cap200000_rejection_rate` | 0 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=200000 under a saturating submit rate |
| `distributed_ceiling_utilisation_1_processes` | 61.4 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 1000 jobs/s (2 slots x one 2ms job each) |
| `distributed_ceiling_utilisation_2_processes` | 68.9 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 2000 jobs/s (4 slots x one 2ms job each) |
| `distributed_ceiling_utilisation_4_processes` | 68.2 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 4000 jobs/s (8 slots x one 2ms job each) |
| `distributed_ceiling_utilisation_8_processes` | 71.2 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 8000 jobs/s (16 slots x one 2ms job each) |
| `distributed_speedup_2_processes` | 2.24 | x | `results/summary.csv` | throughput divided by the single-process throughput |
| `distributed_speedup_4_processes` | 4.44 | x | `results/summary.csv` | throughput divided by the single-process throughput |
| `distributed_speedup_8_processes` | 9.27 | x | `results/summary.csv` | throughput divided by the single-process throughput |
| `distributed_throughput_1_processes` | 614 | jobs/s | `results/summary.csv` | median over runs with 1 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `distributed_throughput_2_processes` | 1378 | jobs/s | `results/summary.csv` | median over runs with 2 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `distributed_throughput_4_processes` | 2727 | jobs/s | `results/summary.csv` | median over runs with 4 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `distributed_throughput_8_processes` | 5693 | jobs/s | `results/summary.csv` | median over runs with 8 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `failure_fail0pct_dead_letter_rate` | 0 | % | `results/summary.csv` | jobs that exhausted their retries, divided by jobs completed |
| `failure_fail0pct_throughput` | 53406 | jobs/s | `results/summary.csv` | median throughput with retries enabled (max_attempts=3) |
| `failure_fail30pct_dead_letter_rate` | 2.5 | % | `results/summary.csv` | jobs that exhausted their retries, divided by jobs completed |
| `failure_fail30pct_throughput` | 10550 | jobs/s | `results/summary.csv` | median throughput with retries enabled (max_attempts=3) |
| `failure_fail70pct_dead_letter_rate` | 34.5 | % | `results/summary.csv` | jobs that exhausted their retries, divided by jobs completed |
| `failure_fail70pct_throughput` | 10537 | jobs/s | `results/summary.csv` | median throughput with retries enabled (max_attempts=3) |
| `group_commit_off_throughput_1_threads` | 59440 | jobs/s | `results/summary.csv` | median throughput with --group-commit false |
| `group_commit_off_throughput_4_threads` | 40860 | jobs/s | `results/summary.csv` | median throughput with --group-commit false |
| `group_commit_off_throughput_8_threads` | 41172 | jobs/s | `results/summary.csv` | median throughput with --group-commit false |
| `group_commit_on_throughput_1_threads` | 62772 | jobs/s | `results/summary.csv` | median throughput with --group-commit true |
| `group_commit_on_throughput_4_threads` | 53286 | jobs/s | `results/summary.csv` | median throughput with --group-commit true |
| `group_commit_on_throughput_8_threads` | 51781 | jobs/s | `results/summary.csv` | median throughput with --group-commit true |
| `group_commit_rows_per_transaction_4_threads` | 6.73 | rows | `results/summary.csv` | row writes divided by committed transactions, reported by the store |
| `group_commit_speedup_1_threads` | 1.06 | x | `results/summary.csv` | on divided by off, same binary and workload, only the flag changed |
| `group_commit_speedup_4_threads` | 1.3 | x | `results/summary.csv` | on divided by off, same binary and workload, only the flag changed |
| `group_commit_speedup_8_threads` | 1.26 | x | `results/summary.csv` | on divided by off, same binary and workload, only the flag changed |
| `http_health_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_health_c1_p50_ms` | 0.02 | ms | `results/raw/http_health_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_health_c1_p95_ms` | 0.023 | ms | `results/raw/http_health_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_health_c1_p99_ms` | 0.025 | ms | `results/raw/http_health_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_health_c1_throughput` | 49724 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_health_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_health_c32_p50_ms` | 0.166 | ms | `results/raw/http_health_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_health_c32_p95_ms` | 0.25 | ms | `results/raw/http_health_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_health_c32_p99_ms` | 0.358 | ms | `results/raw/http_health_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_health_c32_throughput` | 183415 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_health_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_health_c8_p50_ms` | 0.05 | ms | `results/raw/http_health_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_health_c8_p95_ms` | 0.075 | ms | `results/raw/http_health_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_health_c8_p99_ms` | 0.087 | ms | `results/raw/http_health_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_health_c8_throughput` | 152140 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_mixed_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_mixed_c1_p50_ms` | 0.031 | ms | `results/raw/http_mixed_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_mixed_c1_p95_ms` | 0.039 | ms | `results/raw/http_mixed_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_mixed_c1_p99_ms` | 0.044 | ms | `results/raw/http_mixed_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_mixed_c1_throughput` | 32762 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_mixed_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_mixed_c32_p50_ms` | 0.375 | ms | `results/raw/http_mixed_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_mixed_c32_p95_ms` | 0.989 | ms | `results/raw/http_mixed_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_mixed_c32_p99_ms` | 1.28 | ms | `results/raw/http_mixed_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_mixed_c32_throughput` | 73315 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_mixed_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_mixed_c8_p50_ms` | 0.142 | ms | `results/raw/http_mixed_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_mixed_c8_p95_ms` | 0.335 | ms | `results/raw/http_mixed_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_mixed_c8_p99_ms` | 0.415 | ms | `results/raw/http_mixed_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_mixed_c8_throughput` | 49751 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_routes` | 16 | routes | `src/api/api.cpp` | counted from the kRoutes table, which is also what GET /routes returns |
| `http_status_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_status_c1_p50_ms` | 0.023 | ms | `results/raw/http_status_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_status_c1_p95_ms` | 0.027 | ms | `results/raw/http_status_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_status_c1_p99_ms` | 0.029 | ms | `results/raw/http_status_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_status_c1_throughput` | 42246 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_status_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_status_c32_p50_ms` | 0.178 | ms | `results/raw/http_status_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_status_c32_p95_ms` | 0.45 | ms | `results/raw/http_status_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_status_c32_p99_ms` | 0.647 | ms | `results/raw/http_status_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_status_c32_throughput` | 137400 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_status_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_status_c8_p50_ms` | 0.063 | ms | `results/raw/http_status_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_status_c8_p95_ms` | 0.106 | ms | `results/raw/http_status_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_status_c8_p99_ms` | 0.135 | ms | `results/raw/http_status_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_status_c8_throughput` | 120906 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_submit_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_submit_c1_p50_ms` | 0.036 | ms | `results/raw/http_submit_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_submit_c1_p95_ms` | 0.044 | ms | `results/raw/http_submit_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_submit_c1_p99_ms` | 0.081 | ms | `results/raw/http_submit_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_submit_c1_throughput` | 27202 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_submit_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_submit_c32_p50_ms` | 0.508 | ms | `results/raw/http_submit_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_submit_c32_p95_ms` | 0.841 | ms | `results/raw/http_submit_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_submit_c32_p99_ms` | 1.03 | ms | `results/raw/http_submit_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_submit_c32_throughput` | 54029 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_submit_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_submit_c8_p50_ms` | 0.212 | ms | `results/raw/http_submit_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_submit_c8_p95_ms` | 0.355 | ms | `results/raw/http_submit_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_submit_c8_p99_ms` | 0.457 | ms | `results/raw/http_submit_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_submit_c8_throughput` | 35597 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `job_states` | 10 | states | `include/jobengine/job.hpp` | counted from the JobState enumerators |
| `latency_noop_1000ps_achieved` | 1000 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_1000ps_p50_ms` | 0.089 | ms | `results/raw/latency_noop_r1000_rep*.csv` | nearest-rank p50 over 15000 raw e2e samples from intended submit time |
| `latency_noop_1000ps_p95_ms` | 0.119 | ms | `results/raw/latency_noop_r1000_rep*.csv` | nearest-rank p95 over 15000 raw e2e samples from intended submit time |
| `latency_noop_1000ps_p99_ms` | 0.153 | ms | `results/raw/latency_noop_r1000_rep*.csv` | nearest-rank p99 over 15000 raw e2e samples from intended submit time |
| `latency_noop_100ps_achieved` | 100 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_100ps_p50_ms` | 3.12 | ms | `results/raw/latency_noop_r100_rep*.csv` | nearest-rank p50 over 1800 raw e2e samples from intended submit time |
| `latency_noop_100ps_p95_ms` | 4.19 | ms | `results/raw/latency_noop_r100_rep*.csv` | nearest-rank p95 over 1800 raw e2e samples from intended submit time |
| `latency_noop_100ps_p99_ms` | 4.24 | ms | `results/raw/latency_noop_r100_rep*.csv` | nearest-rank p99 over 1800 raw e2e samples from intended submit time |
| `latency_noop_2000ps_achieved` | 2000 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_2000ps_p50_ms` | 0.019 | ms | `results/raw/latency_noop_r2000_rep*.csv` | nearest-rank p50 over 30000 raw e2e samples from intended submit time |
| `latency_noop_2000ps_p95_ms` | 0.029 | ms | `results/raw/latency_noop_r2000_rep*.csv` | nearest-rank p95 over 30000 raw e2e samples from intended submit time |
| `latency_noop_2000ps_p99_ms` | 0.056 | ms | `results/raw/latency_noop_r2000_rep*.csv` | nearest-rank p99 over 30000 raw e2e samples from intended submit time |
| `latency_noop_4000ps_achieved` | 4000 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_4000ps_p50_ms` | 0.019 | ms | `results/raw/latency_noop_r4000_rep*.csv` | nearest-rank p50 over 60000 raw e2e samples from intended submit time |
| `latency_noop_4000ps_p95_ms` | 0.028 | ms | `results/raw/latency_noop_r4000_rep*.csv` | nearest-rank p95 over 60000 raw e2e samples from intended submit time |
| `latency_noop_4000ps_p99_ms` | 0.062 | ms | `results/raw/latency_noop_r4000_rep*.csv` | nearest-rank p99 over 60000 raw e2e samples from intended submit time |
| `latency_noop_500ps_achieved` | 500 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_500ps_p50_ms` | 0.676 | ms | `results/raw/latency_noop_r500_rep*.csv` | nearest-rank p50 over 7500 raw e2e samples from intended submit time |
| `latency_noop_500ps_p95_ms` | 0.861 | ms | `results/raw/latency_noop_r500_rep*.csv` | nearest-rank p95 over 7500 raw e2e samples from intended submit time |
| `latency_noop_500ps_p99_ms` | 0.89 | ms | `results/raw/latency_noop_r500_rep*.csv` | nearest-rank p99 over 7500 raw e2e samples from intended submit time |
| `latency_sleep5ms_1000ps_achieved` | 580 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_1000ps_p50_ms` | 1768.2 | ms | `results/raw/latency_sleep5ms_r1000_rep*.csv` | nearest-rank p50 over 15000 raw e2e samples from intended submit time |
| `latency_sleep5ms_1000ps_p95_ms` | 3433.6 | ms | `results/raw/latency_sleep5ms_r1000_rep*.csv` | nearest-rank p95 over 15000 raw e2e samples from intended submit time |
| `latency_sleep5ms_1000ps_p99_ms` | 3582.6 | ms | `results/raw/latency_sleep5ms_r1000_rep*.csv` | nearest-rank p99 over 15000 raw e2e samples from intended submit time |
| `latency_sleep5ms_100ps_achieved` | 100 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_100ps_p50_ms` | 13.18 | ms | `results/raw/latency_sleep5ms_r100_rep*.csv` | nearest-rank p50 over 1800 raw e2e samples from intended submit time |
| `latency_sleep5ms_100ps_p95_ms` | 16.66 | ms | `results/raw/latency_sleep5ms_r100_rep*.csv` | nearest-rank p95 over 1800 raw e2e samples from intended submit time |
| `latency_sleep5ms_100ps_p99_ms` | 16.73 | ms | `results/raw/latency_sleep5ms_r100_rep*.csv` | nearest-rank p99 over 1800 raw e2e samples from intended submit time |
| `latency_sleep5ms_2000ps_achieved` | 570 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_2000ps_p50_ms` | 6315.9 | ms | `results/raw/latency_sleep5ms_r2000_rep*.csv` | nearest-rank p50 over 30000 raw e2e samples from intended submit time |
| `latency_sleep5ms_2000ps_p95_ms` | 11910.9 | ms | `results/raw/latency_sleep5ms_r2000_rep*.csv` | nearest-rank p95 over 30000 raw e2e samples from intended submit time |
| `latency_sleep5ms_2000ps_p99_ms` | 12410.6 | ms | `results/raw/latency_sleep5ms_r2000_rep*.csv` | nearest-rank p99 over 30000 raw e2e samples from intended submit time |
| `latency_sleep5ms_4000ps_achieved` | 572 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_4000ps_p50_ms` | 15061.3 | ms | `results/raw/latency_sleep5ms_r4000_rep*.csv` | nearest-rank p50 over 60000 raw e2e samples from intended submit time |
| `latency_sleep5ms_4000ps_p95_ms` | 28470.2 | ms | `results/raw/latency_sleep5ms_r4000_rep*.csv` | nearest-rank p95 over 60000 raw e2e samples from intended submit time |
| `latency_sleep5ms_4000ps_p99_ms` | 29673.1 | ms | `results/raw/latency_sleep5ms_r4000_rep*.csv` | nearest-rank p99 over 60000 raw e2e samples from intended submit time |
| `latency_sleep5ms_500ps_achieved` | 499 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_500ps_p50_ms` | 8.18 | ms | `results/raw/latency_sleep5ms_r500_rep*.csv` | nearest-rank p50 over 7500 raw e2e samples from intended submit time |
| `latency_sleep5ms_500ps_p95_ms` | 9.96 | ms | `results/raw/latency_sleep5ms_r500_rep*.csv` | nearest-rank p95 over 7500 raw e2e samples from intended submit time |
| `latency_sleep5ms_500ps_p99_ms` | 10.21 | ms | `results/raw/latency_sleep5ms_r500_rep*.csv` | nearest-rank p99 over 7500 raw e2e samples from intended submit time |
| `priority_0_queue_wait_p50_ms` | 1950 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p50 over 6000 raw queue-wait samples at priority 0 |
| `priority_0_queue_wait_p95_ms` | 2528 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p95 over 6000 raw queue-wait samples at priority 0 |
| `priority_9_queue_wait_p50_ms` | 634 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p50 over 6000 raw queue-wait samples at priority 9 |
| `priority_9_queue_wait_p95_ms` | 1213 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p95 over 6000 raw queue-wait samples at priority 9 |
| `priority_queue_wait_reduction_p50` | 67.5 | % | `results/raw/priority_0_50_9_50_r*.csv` | reduction in median queue wait for priority 9 relative to priority 0, same run |
| `scaling_efficiency_cpu_hash200k_2_threads` | 91.1 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_cpu_hash200k_4_threads` | 85 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_cpu_hash200k_8_threads` | 54.2 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_noop_2_threads` | 47.2 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_noop_4_threads` | 21.9 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_noop_8_threads` | 10.4 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_sleep5ms_2_threads` | 100.3 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_sleep5ms_4_threads` | 100.5 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_sleep5ms_8_threads` | 100.1 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_speedup_cpu_hash200k_2_threads` | 1.82 | x | `results/summary.csv` | throughput at 2 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_cpu_hash200k_4_threads` | 3.4 | x | `results/summary.csv` | throughput at 4 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_cpu_hash200k_8_threads` | 4.34 | x | `results/summary.csv` | throughput at 8 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_noop_2_threads` | 0.94 | x | `results/summary.csv` | throughput at 2 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_noop_4_threads` | 0.88 | x | `results/summary.csv` | throughput at 4 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_noop_8_threads` | 0.83 | x | `results/summary.csv` | throughput at 8 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_sleep5ms_2_threads` | 2.01 | x | `results/summary.csv` | throughput at 2 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_sleep5ms_4_threads` | 4.02 | x | `results/summary.csv` | throughput at 4 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_sleep5ms_8_threads` | 8.01 | x | `results/summary.csv` | throughput at 8 threads divided by throughput at 1 thread, same workload |
| `source_lines_cpp` | 6952 | lines | `src/ include/ apps/` | newline count across the C++ sources and headers, excluding tests and third_party |
| `test_lines_cpp` | 3180 | lines | `tests/` | newline count across the test sources |
| `tests_total` | 132 | tests | `build/jobengine_tests` | counted from `jobengine_tests --gtest_list_tests` |
| `throughput_cpu_hash200k_1_worker_threads` | 5166 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w1 |
| `throughput_cpu_hash200k_2_worker_threads` | 9413 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w2 |
| `throughput_cpu_hash200k_4_worker_threads` | 17567 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w4 |
| `throughput_cpu_hash200k_8_worker_threads` | 22402 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w8 |
| `throughput_noop_1_worker_threads` | 61278 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w1 |
| `throughput_noop_2_worker_threads` | 57896 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w2 |
| `throughput_noop_4_worker_threads` | 53747 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w4 |
| `throughput_noop_8_worker_threads` | 50876 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w8 |
| `throughput_sleep5ms_1_worker_threads` | 163 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w1 |
| `throughput_sleep5ms_2_worker_threads` | 327 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w2 |
| `throughput_sleep5ms_4_worker_threads` | 655 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w4 |
| `throughput_sleep5ms_8_worker_threads` | 1305 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w8 |
| `workflow_chain4_w1_p50_ms` | 9844.9 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_chain4_w1_throughput` | 35.47 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_chain4_w4_p50_ms` | 2466.8 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_chain4_w4_throughput` | 141.63 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_chain4_w8_p50_ms` | 1232.3 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_chain4_w8_throughput` | 283.29 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_diamond6_w1_p50_ms` | 15481.7 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_diamond6_w1_throughput` | 23.66 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_diamond6_w4_p50_ms` | 3867.6 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_diamond6_w4_throughput` | 94.48 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_diamond6_w8_p50_ms` | 1915.1 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_diamond6_w8_throughput` | 190.73 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_fanout6_w1_p50_ms` | 9893.7 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_fanout6_w1_throughput` | 23.65 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_fanout6_w4_p50_ms` | 2471.6 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_fanout6_w4_throughput` | 94.81 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_fanout6_w8_p50_ms` | 1241 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_fanout6_w8_throughput` | 188.81 | workflows/s | `results/summary.csv` | median workflows completed per second |

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

- Built a job-processing and workflow engine in `C++20` (6952 lines of C++
  across a scheduler, thread-safe priority queue, SQLite store, `HTTP/1.1` server and REST
  API), verified by 132 GoogleTest cases with zero ThreadSanitizer,
  AddressSanitizer and UndefinedBehaviorSanitizer findings.
- Implemented a 10-state job lifecycle behind a single validated transition
  table, with exponential-backoff retries, per-job deadlines, cooperative cancellation and a
  dead-letter state, sustaining 61278 fully persisted job
  lifecycles per second.
- Added dependency workflows with DAG validation (Kahn's algorithm), transitive failure
  propagation and parallel execution of independent branches, cutting a fan-out workflow's
  median completion time from 9893.7 ms on one worker thread to
  2471.6 ms on `4`.
- Extended the engine to multiple worker processes coordinated by lease-based assignment,
  heartbeats and fencing tokens, and verified recovery by killing a worker process with
  `SIGKILL` and confirming its jobs were reassigned and completed.

### Strong but fully defensible version

- Engineered a distributed job processing and workflow engine in `C++20`
  (6952 lines of C++, 3180 lines of tests) with a
  mutex/condition-variable priority queue, a configurable worker pool, SQLite-backed durable
  metadata and a hand-written `HTTP/1.1` server; 132 tests pass clean under
  ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer, and the engine sustains
  61278 persisted job lifecycles per second.
- Profiled the engine and identified persistence rather than scheduling as the bottleneck -
  every job state transition was its own SQLite transaction, so throughput *fell* as worker
  threads were added; implemented group commit with a read-through write-back cache, raising
  throughput from 40860 to
  53286 jobs/s (1.3x) at
  `4` worker threads by batching 6.73 row writes
  per transaction, with the A/B reproducible from one binary by flipping a single flag.
- Measured worker-pool scaling across workload classes rather than one flattering case:
  I/O-bound jobs scale 8.01x on `8` threads
  (100.1% efficiency) and CPU-bound jobs
  4.34x, while a zero-work job peaks single-threaded
  because it has nothing to parallelise and only adds contention.
- Implemented lease-based distributed execution (a coordinator plus independent worker
  processes, heartbeats and `64`-bit fencing tokens) giving at-least-once execution with
  exactly-once result recording; throughput scaled from
  614 to 5693 jobs/s
  across `1` to `8` worker processes (9.27x, reaching
  71.2% of the theoretical ceiling those worker
  slots allow), and a `SIGKILL`ed worker's jobs were detected, reassigned and completed while
  its late result was rejected as a stale lease.
- Found and fixed a connection-starvation defect that the unit tests missed and the
  benchmark suite exposed: long-polling workers occupied every fixed connection thread and
  made the REST API unreachable; replaced the fixed pool with one that grows on demand and
  added a regression test that asserts the growth path fires.
- Served a REST API sustaining 35597 job submissions/s at
  0.212 ms p50 / 0.355 ms p95 /
  0.457 ms p99 over `8` keep-alive connections with
  0% errors, and implemented strict priority scheduling that cut
  median queue wait for high-priority jobs by 67.5%
  (634 ms against 1950 ms in the
  same saturated run).

### Compact two-bullet version

- Built a `C++20` distributed job processing and workflow engine (6952 lines
  of C++; thread-safe priority queue, worker pool, SQLite durability, REST API, DAG
  workflows, lease-based multi-process execution) sustaining
  61278 persisted job lifecycles/s,
  35597 API submissions/s at 0.355 ms p95, and
  8.01x scaling on `8` worker threads;
  132 tests pass clean under ThreadSanitizer and AddressSanitizer.
- Profiled and removed the persistence bottleneck with group commit
  (40860 to 53286
  jobs/s, 1.3x), scaled execution to
  5693 jobs/s across `8` worker processes
  (9.27x), and implemented fencing-token leases
  with heartbeat-based failure detection, verified by killing worker processes mid-job and
  confirming reassignment, completion and rejection of the stale result.

## 11. Evidence table

A claim is safe to mention only when a command in this repository reproduces it.

| CV claim | Evidence file | Test / benchmark command | Verified metric | Safe to mention? |
|---|---|---|---|---|
| Builds clean with `-Werror` in Release and Debug | `scripts/gates/g01_build.mjs` | `node scripts/gates/g01_build.mjs` | 0 warnings in project sources | **Yes** |
| 132 tests pass | `tests/`, `scripts/gates/g02_tests.mjs` | `node scripts/gates/g02_tests.mjs` | `tests_total` = 132 | **Yes** |
| Zero data races | `scripts/gates/g03_tsan.mjs` | `node scripts/gates/g03_tsan.mjs` | 0 ThreadSanitizer warnings | **Yes** |
| Zero memory errors or leaks | `scripts/gates/g04_asan.mjs` | `node scripts/gates/g04_asan.mjs` | 0 ASan/UBSan errors, 0 leaks | **Yes** |
| No job lost or executed twice | `scripts/gates/g05_no_loss.mjs` | `node scripts/gates/g05_no_loss.mjs` | every job has exactly 1 attempt record | **Yes** |
| Priority scheduling reduces high-priority queue wait | `results/raw/priority_*.csv` | `node scripts/gates/g06_priority.mjs` | `priority_queue_wait_reduction_p50` = 67.5% | **Yes** |
| Graceful shutdown leaves nothing running | `scripts/gates/g07_shutdown.mjs` | `node scripts/gates/g07_shutdown.mjs` | 0 rows left `RUNNING` | **Yes** |
| REST API contract incl. 4xx/409/503 | `scripts/gates/g08_api.mjs` | `node scripts/gates/g08_api.mjs` | 61 live assertions | **Yes** |
| Survives coordinator `SIGKILL` | `scripts/gates/g09_restart.mjs` | `node scripts/gates/g09_restart.mjs` | 51 jobs recovered and completed | **Yes** |
| Retry count and exponential backoff | `scripts/gates/g10_retry.mjs` | `node scripts/gates/g10_retry.mjs` | measured gaps vs the formula | **Yes** |
| Deadline enforcement, and its cooperative limit | `scripts/gates/g11_timeout.mjs` | `node scripts/gates/g11_timeout.mjs` | `TIMED_OUT` recorded; limit demonstrated | **Yes** |
| Cancellation incl. the start race | `scripts/gates/g12_cancel.mjs` | `node scripts/gates/g12_cancel.mjs` | 150 races, one outcome each | **Yes** |
| DAG order, parallelism, failure propagation | `scripts/gates/g13_workflow.mjs` | `node scripts/gates/g13_workflow.mjs` | `workflow_fanout6_w4_p50_ms` = 2471.6 ms | **Yes** |
| Work distributed across worker processes | `scripts/gates/g14_distributed.mjs` | `node scripts/gates/g14_distributed.mjs` | per-process execution counts | **Yes** |
| Worker failure detection and reassignment | `scripts/gates/g15_worker_failure.mjs` | `node scripts/gates/g15_worker_failure.mjs` | detection, requeue and recovery times | **Yes** |
| Throughput on `4` worker threads | `results/summary.csv` | `node scripts/run_benchmarks.mjs --only worker_scaling` | `throughput_noop_4_worker_threads` = 53747 jobs/s | **Yes** |
| Group-commit improvement | `results/summary.csv` | `node scripts/run_benchmarks.mjs --only group_commit` | `group_commit_speedup_4_threads` = 1.3x | **Yes** |
| Multi-process scaling | `results/summary.csv` | `node scripts/run_benchmarks.mjs --only distributed` | `distributed_speedup_8_processes` = 9.27x | **Yes** |
| API latency percentiles | `results/raw/http_*.csv` | `node scripts/run_benchmarks.mjs --only http_api` | `http_submit_c8_p99_ms` = 0.457 ms | **Yes** |
| Static analysis clean | `scripts/gates/g20_static.mjs` | `node scripts/gates/g20_static.mjs` | 0 real findings, 1 documented false positive | **Yes** |
| "Production-grade" / "highly scalable" | - | - | - | **No** - no production deployment, no load beyond one machine |
| "Fault tolerant" | - | - | - | **No** - worker failure is handled; the coordinator is a single point of failure |
| "Exactly-once processing" | - | - | - | **No** - at-least-once execution; only result *recording* is exactly-once |
| "Runs across multiple machines" | - | - | - | **No** - protocol allows it, but it was never tested off one host |
| "Dockerised deployment" | `Dockerfile` | - | - | **No** - written but never built; Docker is not installed here |
| "Low latency" as an unqualified phrase | `results/raw/latency_*.csv` | `node scripts/run_benchmarks.mjs --only latency` | cite the measured p50/p95/p99 instead | **No** - quote the number, not the adjective |
