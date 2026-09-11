<!-- rendered from docs/readme.template.md -->
<!-- Generated. Edit docs/readme.template.md, then run:
       node scripts/extract_metrics.mjs && node scripts/render_docs.mjs -->

# Distributed Job Processing & Workflow Engine

A job-processing service written in C++20: submit work over HTTP, have it scheduled by
priority, executed on a bounded pool of workers (threads in one process, or separate worker
processes), retried with backoff, bounded by deadlines, and composed into dependency DAGs.

6952 lines of C++20 plus 3180 lines of tests,
132 automated tests and 20 end-to-end acceptance gates. Every number in this
repository comes from a command you can re-run - see [REPRODUCIBILITY.md](REPRODUCIBILITY.md).

---

## What it does

| Capability | Where it is implemented | How it is verified |
|---|---|---|
| REST API for submission and status | `src/api/api.cpp`, `src/http/` | `tests/test_api.cpp`, `scripts/gates/g08_api.mjs` |
| Thread-safe priority queue with delayed entries | `src/queue/queue.cpp` | `tests/test_queue.cpp`, `g06_priority.mjs` |
| Configurable worker pool | `src/engine/engine.cpp` | `tests/test_engine.cpp` |
| Strict priority scheduling, FIFO within a level | `src/queue/queue.cpp` | `g06_priority.mjs` (measured per-priority queue wait) |
| Job lifecycle: 10 states, one validated transition table | `src/core/job.cpp` | `tests/test_job_state.cpp` |
| Retries with exponential backoff and a dead-letter state | `src/engine/engine.cpp` | `g10_retry.mjs` (measured backoff timing) |
| Per-job deadlines with cooperative cancellation | `src/engine/engine.cpp` | `g11_timeout.mjs` |
| Cancellation, including the cancel-vs-start race | `src/engine/engine.cpp` | `g12_cancel.mjs` |
| Graceful shutdown | `src/engine/engine.cpp` | `g07_shutdown.mjs` |
| Persistent metadata + restart recovery | `src/store/store.cpp` | `g09_restart.mjs` (survives SIGKILL) |
| Workflow DAGs with failure propagation | `src/engine/engine.cpp` | `g13_workflow.mjs` |
| Multi-process execution with leases and heartbeats | `src/worker/`, `src/engine/` | `g14_distributed.mjs`, `g15_worker_failure.mjs` |
| Observability (`/health`, `/metrics`) | `src/api/api.cpp` | `tests/test_api.cpp` |
| Reproducible benchmark suite | `scripts/run_benchmarks.mjs` | `g16_bench_artifacts.mjs` |

## Measured results

Full methodology, tables and plots are in [BENCHMARKS.md](BENCHMARKS.md); the machine is
recorded in `results/env.txt`. Median of 3 repetitions per configuration.

**Engine throughput** - a `noop` job does nothing, so this is the cost of the engine itself:
queue operations plus the four persisted state changes every job goes through.

| Worker threads | Jobs/s | vs 1 thread |
|---|---|---|
| `1` | 61278 | 1.00x |
| `4` | 53747 | 0.88x |
| `8` | 50876 | 0.83x |

Throughput *falls* as threads are added, and that is the correct result rather than a
defect: a job with no work in it has nothing to parallelise, so extra threads only add
contention on the queue and the store. The number worth quoting from this workload is the
single-threaded ceiling of **61278 complete job lifecycles
per second**, each of which is persisted.

**Scaling** is measured on workloads that contain actual work:

| Workload | 1 thread | 8 threads | Speedup | Efficiency |
|---|---|---|---|---|
| `sleep5ms` (I/O-bound stand-in) | 163 jobs/s | 1305 jobs/s | 8.01x | 100.1% |
| `cpu_hash200k` (CPU-bound) | 5166 jobs/s | 22402 jobs/s | 4.34x | 54.2% |

I/O-bound work scales essentially linearly - the efficiency figure sits marginally above
`100`% because these are medians of three noisy runs, not because the engine found extra
capacity. CPU-bound work reaches 4.34x on a machine
with `10` physical cores, which is what a CPU-bound workload sharing those cores with the
scheduler and the store should do.

**Latency, API and distribution**

Distributed throughput is reported against the ceiling each configuration could reach rather
than against the single-process run, because that run is the least efficient point: a fleet
of `1` process reaches 61.4% of its slots'
theoretical rate and a fleet of `8` reaches
71.2%, so per-job coordination cost amortises as
load rises. Quoting "efficiency vs 1 process" would produce a figure above `100`%, which
would say more about the baseline than about the system.

| | |
|---|---|
| End-to-end p95 at a controlled `1000` jobs/s | 0.119 ms |
| REST submissions, `8` keep-alive connections | 35597 req/s, p50 0.212 ms, p95 0.355 ms, p99 0.457 ms |
| Status lookups, `8` connections | p99 0.135 ms |
| Distributed, `1` -> `8` worker processes | 614 -> 5693 jobs/s (9.27x) |
| Group commit, off -> on at `4` threads | 40860 -> 53286 jobs/s (1.3x) |
| High vs low priority median queue wait | 634 ms vs 1950 ms (67.5% lower) |
| Fan-out workflow p50, `1` -> `4` worker threads | 9893.7 -> 2471.6 ms |

## What the work actually involved

The interesting part was not writing the scheduler; it was finding out where it was wrong.

**The bottleneck was persistence, not scheduling.** Benchmarking worker-thread counts
produced the opposite of a scaling curve: throughput *fell* between 4 and 8 threads. Each
job was costing four separate SQLite transactions - insert, transition to running, attempt
record, terminal update - and every bare statement is its own implicit transaction, so they
all serialised behind SQLite's single writer and more threads only added contention. Group
commit fixed it: one flusher thread writes whatever has accumulated as a single transaction,
submissions still block until their own row is durable, and transitions are served from a
read-through cache so no reader sees a stale state. The A/B above runs from one binary with
one flag changed.

**An obvious follow-up optimisation was wrong.** Adding a commit delay to build bigger
batches made throughput monotonically *worse*, because submitters block until their batch
commits and the system falls into lock-step. The default is no delay; the knob survives
because it is how the decision was measured.

**The first latency numbers were meaningless.** A saturating submit rate makes "latency" a
measure of queue depth - a p95 of 25 seconds for 5 ms jobs. Latency is now reported only
from rate-limited runs that measure from each job's *intended* submission time.

**The benchmark suite found a starvation bug the tests did not.** At the largest
distributed configuration - 8 worker processes, each long-polling for work - every one of
the server's fixed connection threads was occupied and the API became unreachable, not
slow. The connection pool now has a floor rather than a ceiling and grows on demand, and a
regression test fills a two-thread pool and asserts both that a new client is still served
and that the growth path actually fired.

**Ten defects were found and fixed** in total, including a workflow-completion race where
two threads could both finalise the same workflow, every in-memory store silently sharing
one database, `SQLITE_LOCKED` being surfaced as a lost job, and dropped pipelined HTTP
requests. Each is written up with cause, fix and the test that now prevents it in
[FAILURE_ANALYSIS.md](FAILURE_ANALYSIS.md).

**Every published number is checked by a script.** The CV document is generated from a
template whose figures are placeholders filled from measured results, and an acceptance gate
re-renders it and recomputes each value from the raw samples. A number cannot be typed into
it by hand.

## What it deliberately does not do

Stated up front, because the honest boundary is part of the design:

- **Not exactly-once execution.** At-least-once execution with exactly-once *result
  recording* via lease fencing tokens. See `docs/DESIGN_DECISIONS.md` D-09.
- **No preemption.** A running job cannot be forcibly stopped; timeout and cancellation are
  cooperative in-process, and there is a test that demonstrates the limit (D-07).
- **One coordinator.** It is a single point of failure. State survives its death and is
  recovered on restart, but the service is down while it is down.
- **No authentication or TLS.** Intended for localhost or a trusted network.
- **Docker is provided but unverified** - Docker is not installed on the development
  machine, so `Dockerfile` has never been built (D-12). It is excluded from every claim.
- **Single machine.** Worker processes speak plain TCP with no localhost assumption, but
  running them across machines has not been tested and is not claimed.

---

## Build

Requirements: a C++20 compiler, CMake >= 3.16, and SQLite3 development headers. The library
and all four executables have no other dependencies.

Building the **tests** additionally pulls GoogleTest, pinned to `v1.15.2`, via CMake
`FetchContent` on the first configure. To build the tests with no network, clone it once
into the location CMake prefers:

```bash
git clone --depth 1 --branch v1.15.2 https://github.com/google/googletest.git third_party/googletest
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Sanitizer builds:

```bash
cmake -S . -B build-tsan  -DCMAKE_BUILD_TYPE=Debug -DJOBENGINE_SANITIZER=thread
cmake -S . -B build-asan  -DCMAKE_BUILD_TYPE=Debug -DJOBENGINE_SANITIZER=address
cmake --build build-tsan -j && ./build-tsan/jobengine_tests
```

## Run

```bash
# coordinator with 4 embedded worker threads
./build/jobengine-server --port 8080 --workers 4 --db jobengine.db

# in another terminal: submit a job
curl -s -XPOST localhost:8080/jobs \
  -d '{"type":"sleep","payload":{"ms":250},"priority":5,"max_attempts":3,"timeout_ms":2000}'
# -> {"job_id":"j_01J...","state":"QUEUED"}

curl -s localhost:8080/jobs/j_01J...
curl -s localhost:8080/jobs/j_01J.../attempts
curl -s -XPOST localhost:8080/jobs/j_01J.../cancel
curl -s 'localhost:8080/jobs?state=SUCCEEDED&limit=10'
curl -s localhost:8080/health
curl -s localhost:8080/metrics
```

A workflow (a job runs only after all its dependencies have succeeded):

```bash
curl -s -XPOST localhost:8080/workflows -d '{
  "name": "document-ingestion",
  "nodes": [
    {"name":"extract",  "type":"sleep", "payload":{"ms":100}},
    {"name":"chunk",    "type":"sleep", "payload":{"ms":100}},
    {"name":"embed_a",  "type":"cpu_hash", "payload":{"iterations":200000}},
    {"name":"embed_b",  "type":"cpu_hash", "payload":{"iterations":200000}},
    {"name":"index",    "type":"sleep", "payload":{"ms":50}}
  ],
  "edges": [
    {"from":"extract","to":"chunk"},
    {"from":"chunk","to":"embed_a"}, {"from":"chunk","to":"embed_b"},
    {"from":"embed_a","to":"index"}, {"from":"embed_b","to":"index"}
  ]
}'
curl -s localhost:8080/workflows/w_01J...
```

Distributed mode - the coordinator schedules, separate processes execute:

```bash
./build/jobengine-server --port 8080 --workers 0 --db jobengine.db &
./build/jobengine-worker --host 127.0.0.1 --port 8080 --name w1 --threads 4 &
./build/jobengine-worker --host 127.0.0.1 --port 8080 --name w2 --threads 4 &
```

## Benchmarks

```bash
node scripts/run_benchmarks.mjs          # writes results/summary.csv + results/raw/
python3 scripts/analyze_benchmarks.py    # writes plots and regenerates BENCHMARKS.md
node scripts/extract_metrics.mjs         # derives the verified metric set
```

Measured results, with the machine they were measured on, are in
[BENCHMARKS.md](BENCHMARKS.md).

## Tests and acceptance gates

```bash
./build/jobengine_tests                       # unit + integration suite
node scripts/gates/g02_tests.mjs              # same, with counts read from the runner
node scripts/gates/g03_tsan.mjs               # ThreadSanitizer
node scripts/gates/g14_distributed.mjs        # real multi-process run
```

Each `scripts/gates/gNN_*.mjs` script verifies one claim end to end and prints
`GATE GNN PASS` only if every assertion inside it holds. [TESTING.md](TESTING.md) lists
what each one proves; [TEST_RESULTS.md](TEST_RESULTS.md) records the last run.

## Repository layout

```
include/jobengine/   public headers (json, job, queue, store, engine, http, api, worker)
src/core/            JSON, time/ids/logging, job model and state machine, metrics
src/queue/           thread-safe priority queue with delayed entries
src/store/           SQLite metadata store with group commit
src/engine/          scheduler, worker pool, retries, deadlines, workflow DAG, leases
src/http/            HTTP/1.1 server and client over BSD sockets
src/api/             REST route table and handlers
src/worker/          remote worker process logic
apps/                jobengine-server, -worker, -bench, -load
tests/               GoogleTest suites
scripts/             benchmark suite, analysis, and the acceptance gates
docs/                requirements, architecture, design decisions, roadmap
results/             benchmark output: summary.csv, raw samples, plots, env.txt
```

## Documents

- [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md) - problem, MVP, entities, states, NFRs
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - components, concurrency model, schema, API
- [docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md) - options compared and costs paid
- [docs/ROADMAP.md](docs/ROADMAP.md) - phases and what is deliberately left open
- [TESTING.md](TESTING.md) / [TEST_RESULTS.md](TEST_RESULTS.md) / [FAILURE_ANALYSIS.md](FAILURE_ANALYSIS.md)
- [BENCHMARKS.md](BENCHMARKS.md) - methodology and measured results
- [REPRODUCIBILITY.md](REPRODUCIBILITY.md) - one command per claim
- [FINAL_PROJECT_REPORT.md](FINAL_PROJECT_REPORT.md) - what was built, found and fixed
- [INTERVIEW_PREPARATION.md](INTERVIEW_PREPARATION.md) - questions and defensible answers
