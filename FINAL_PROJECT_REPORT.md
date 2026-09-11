<!-- rendered from docs/final_report.template.md -->
<!-- Generated. Edit docs/final_report.template.md, then run:
       node scripts/extract_metrics.mjs && node scripts/render_docs.mjs -->

# FINAL PROJECT REPORT

**Distributed Job Processing & Workflow Engine** - a C++20 job scheduler, workflow engine
and REST service, built in eight phases with every claim tied to a re-runnable command.

---

## 1. What was built

A service that accepts units of work over HTTP, records them durably, schedules them by
priority across a bounded worker pool, enforces retry policies and deadlines, and executes
dependency DAGs where a node runs only after all of its parents have succeeded. Execution
happens either on worker threads inside the coordinator or in separate worker processes that
lease jobs over HTTP.

Scale of the artifact, counted from the sources:

| | |
|---|---|
| C++ (engine, queue, store, HTTP, API, workers, apps) | 6952 lines |
| C++ tests | 3180 lines |
| Automated tests | 132 |
| Acceptance gates | 20 |
| Job lifecycle states | 10 |
| HTTP endpoints | 16 |
| External runtime dependencies | SQLite only |

## 2. How it was built, phase by phase

| Phase | Delivered | Verified by |
|---|---|---|
| 0 - design | requirements, architecture, decisions with options compared, schema, API contract | `docs/` reviewed against the implementation by `g18_api_drift.mjs` |
| 1 - single-node engine | job model and transition table, thread-safe priority queue, worker pool, handler registry, graceful shutdown | `QueueTest`, `EngineTest`, `g05`, `g06`, `g07` |
| 2 - API and persistence | HTTP/1.1 server and client, REST routes, SQLite store, restart recovery | `HttpParserTest`, `ApiTest`, `g08`, `g09` |
| 3 - scheduling policy | strict priority with FIFO tie-break, retries with exponential backoff, deadlines, cancellation, dead-letter state | `g06`, `g10`, `g11`, `g12` |
| 4 - workflow engine | DAG validation (Kahn), dependency release, transitive failure propagation, workflow status and cancellation | `WorkflowTest`, `g13` |
| 5 - distributed execution | worker registration, heartbeats, lease-based claim, fencing tokens, expiry and reassignment | `DistributedTest`, `g14`, `g15` |
| 6 - performance | benchmark suite with raw-sample CSVs, plots, recorded environment; group-commit optimisation | `BENCHMARKS.md`, `g16` |
| 7 - quality | sanitizers, static analysis, failure injection, stress | `g01`-`g04`, `g20`, `FAILURE_ANALYSIS.md` |
| 8 - presentation | generated CV document, interview preparation, evidence table | `g17`, `g19` |

## 3. What the measurements showed

Full tables, methodology and plots are in [BENCHMARKS.md](BENCHMARKS.md); the machine is
recorded in `results/env.txt`. Headline figures, median of three repetitions:

| Measurement | Value |
|---|---|
| Engine ceiling: fully persisted job lifecycles, 1 worker thread | 61278 jobs/s |
| Same workload, 4 / 8 worker threads | 53747 / 50876 jobs/s (it *falls* - see below) |
| I/O-bound scaling, 1 -> 8 worker threads | 163 -> 1305 jobs/s (8.01x, 100.1%) |
| CPU-bound scaling, 1 -> 8 worker threads | 5166 -> 22402 jobs/s (4.34x, 54.2%) |
| Group commit off -> on, 4 threads | 40860 -> 53286 jobs/s (1.3x) |
| Row writes batched per transaction | 6.73 |
| End-to-end p95 at a controlled 1000 jobs/s | 0.119 ms |
| API submissions, 8 connections | 35597 req/s, p50 0.212 ms, p95 0.355 ms, p99 0.457 ms |
| API status lookups, 8 connections | 120906 req/s, p99 0.135 ms |
| Distributed, 1 -> 8 worker processes | 614 -> 5693 jobs/s (9.27x) |
| Utilisation of the theoretical slot ceiling, 1 -> 8 processes | 61.4% -> 71.2% |
| Priority 9 vs priority 0 median queue wait | 634 ms vs 1950 ms |
| Fan-out workflow p50, 1 -> 4 worker threads | 9893.7 -> 2471.6 ms |

The four results that shaped the project:

1. **The bottleneck was persistence, not scheduling.** Throughput initially *fell* as worker
   threads were added, because each job cost four separate SQLite transactions that all
   serialised behind one writer. Group commit - one flusher thread writing whatever has
   accumulated as a single transaction, with a read-through cache so no reader sees a stale
   state - produced the improvement in the table above. The A/B runs from one binary with one
   flag changed, so the claim is checkable rather than remembered.

   Note that the zero-work workload *still* peaks single-threaded after that fix, and that is
   the correct answer rather than a remaining defect: a job with nothing in it has nothing to
   parallelise, so additional threads can only add contention. Scaling is therefore reported
   on workloads that contain work, where it is near-linear for I/O-bound jobs and
   core-bounded for CPU-bound ones.
2. **An "obvious" follow-up optimisation was wrong.** Adding a commit delay to build larger
   batches made throughput monotonically worse, because submitters block until their batch
   commits and the system falls into lock-step. The default is therefore no delay; the knob
   remains because it is how the decision was measured.
3. **The first latency numbers were meaningless and had to be re-derived.** A saturating
   submit rate makes "latency" a measure of queue depth. Latency is now reported only from
   rate-limited runs that measure from each job's *intended* submission time.

4. **The benchmark suite found a defect the unit tests could not.** At the largest
   distributed configuration, every worker thread across 8 processes held a long-polling
   connection, filling the server's fixed connection-thread pool and making the API
   unreachable. The pool now grows on demand, and a regression test asserts both that a new
   client is served and that the growth path actually fired.

## 4. What went wrong and what fixed it

Twelve defects and investigations are written up in
[FAILURE_ANALYSIS.md](FAILURE_ANALYSIS.md). The ones worth naming here:

- **A workflow-completion race.** Completion was decided by re-reading all nodes and checking
  whether they were all terminal, so two threads finishing the last two nodes could both
  finalise the workflow. Replaced with a counter decremented under the workflow lock, so
  exactly one thread observes the transition to zero.
- **Every in-memory store shared one database.** A fixed shared-cache name meant every
  `:memory:` store in a process attached to the same database - tests would have silently
  observed each other's rows. Now named per store, with a test that asserts isolation.
- **`SQLITE_LOCKED` was treated as a lost job.** Shared-cache table locks are not covered by
  `busy_timeout`. Added bounded retries for both `SQLITE_BUSY` and `SQLITE_LOCKED`.
- **Pipelined HTTP requests were silently dropped.** Bytes of a second request arriving in the
  same read were discarded. The parser now exposes them and the connection loop consumes them.
- **A ThreadSanitizer-only failure.** A worker could be absent from `alive_workers()` before
  being counted in `workers_marked_dead`, because the counter was incremented outside the lock
  that cleared the flag.
- **Long-polling workers starved every other HTTP client.** A fixed connection-thread pool
  meant a worker fleet as large as the pool made the API unreachable. The pool now has a
  floor rather than a ceiling.
- **One static-analysis finding, investigated rather than suppressed.** A committed probe
  fixture demonstrates that the checker misfires on any path through
  `condition_variable::wait`; the gate excludes only that signature, prints every excluded
  finding, and fails if the probe stops reproducing it.

## 5. Verification summary

| Layer | What it covers | Result |
|---|---|---|
| Unit and integration tests | JSON, state machine, queue, store, engine, retries, timeouts, cancellation, workflows, HTTP parser and server, REST API, distributed protocol | 132 tests, all passing |
| ThreadSanitizer | the whole suite, with producers, workers, HTTP threads, watchdog and store flusher all active | 0 data races |
| AddressSanitizer + UBSan | the whole suite | 0 errors |
| Leak checking | platform `leaks` tool (LeakSanitizer is unavailable on macOS/arm64) | 0 leaks |
| Static analysis | 12 translation units, with a positive control and a false-positive probe | 0 real findings |
| Acceptance gates | 20 end-to-end claims including `SIGKILL` of both a worker and the coordinator | all passing |
| Build | Release and Debug with `-Wall -Wextra -Wpedantic -Werror` | 0 warnings in project sources |

## 6. Limitations

These are design boundaries, not unfinished work:

- **At-least-once execution, not exactly-once.** A worker can execute a job and die before
  reporting. Fencing tokens make *result recording* exactly-once; they do not prevent
  duplicate execution. Retry is therefore opt-in, and `max_attempts` defaults to 1.
- **The coordinator is a single point of failure.** Its state survives its death and is
  recovered on restart, but the service is unavailable while it is down. No replication, no
  consensus - deliberately, because a hand-rolled failover is worse than an honest outage.
- **Timeouts and cancellation are cooperative in-process.** A handler that never polls its
  flag runs to completion and holds its worker thread; there is a test that demonstrates
  exactly this rather than leaving it as a footnote. Across processes, lease expiry does give
  a hard bound.
- **Single machine.** Every measurement is one host over loopback. The worker protocol is
  plain TCP with no localhost assumption, but multi-machine operation was **not tested** and
  is not claimed.
- **Docker is unverified.** Docker is not installed on the development machine, so the
  `Dockerfile` has never been built or run, and it is excluded from every claim.
- **No authentication, authorisation or TLS.** Localhost or a trusted network only.
- **Strict priority can starve low-priority work**, which is measured rather than denied.
- **Group commit trades a narrow crash window for throughput.** Buffered state transitions
  are lost on a hard crash and those jobs are re-executed - inside the at-least-once contract.
  Submissions are never in that window.
- **Not fuzzed, not soak-tested.** Parsers face a fixed corpus of malformed inputs; the
  longest continuous run is the benchmark suite.

## 7. What I would do next

1. **Remove the single point of failure properly.** Move the queue and lease state into a
   replicated store - PostgreSQL with `SELECT ... FOR UPDATE SKIP LOCKED`, or a Raft-backed
   coordinator - so any instance can take over. Not hand-rolled failover.
2. **Out-of-process execution for untrusted handlers**, so a hostile or buggy handler cannot
   hold a worker thread and a hard timeout becomes possible in-process too.
3. **Fuzz the HTTP and JSON parsers.** A fixed corpus is weaker than it sounds for code that
   parses bytes off a socket.
4. **A real soak test** - hours, not seconds - to catch slow leaks and drift that the current
   suite cannot see.
5. **Verify the Docker path** on a machine that has Docker, then either claim it or delete it.

## 8. Evidence

Every claim in this project is traceable to a file and a command. The full
claim-by-claim table - including the claims that are **not** safe to make and why - lives in
[CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md](CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md) §11 and is
not repeated here.

The short version:

| Area | Reproduce with | Result |
|---|---|---|
| Build, tests, sanitizers, static analysis | `node scripts/gates/g01_build.mjs` … `g04`, `g20` | 0 warnings, 132 tests passing, 0 races, 0 memory errors, 0 leaks, 0 static findings |
| Correctness of scheduling, retries, deadlines, cancellation, workflows | `g05`, `g06`, `g10`-`g13` | every behaviour asserted end to end, several against independently computed expectations |
| Durability and failure recovery | `g07`, `g09`, `g15` | survives `SIGTERM` and `SIGKILL` of the coordinator, and `SIGKILL` of a worker mid-job |
| Distribution | `g14` | 400 jobs completed by 3 separate worker processes, with per-process counts proving the spread |
| Performance | `node scripts/run_benchmarks.mjs` | [BENCHMARKS.md](BENCHMARKS.md), from raw per-sample CSVs |
| Claim integrity | `g17` | every number in the CV document recomputed from raw samples and re-rendered from its template |

The phrases this project does **not** use about itself - "production-grade", "fault
tolerant", "exactly-once", "runs across machines" - are listed with their reasons in the CV
document's evidence table.
