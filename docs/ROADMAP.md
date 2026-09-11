# ROADMAP

Status legend: ☐ not started · ◐ in progress · ☑ delivered and verified by a named gate

| Phase | Scope | Exit criterion | Verified by | Status |
|---|---|---|---|---|
| 0 | Requirements, architecture, decisions, schema, API contract | the design documents exist and match what is implemented | `g18_api_drift.mjs`, `g19_docs.mjs` | ☑ |
| 1 | Single-process engine: job model, state machine, thread-safe priority queue, worker pool, graceful shutdown | concurrency tests pass; TSan clean | `g02`, `g03`, `g05`, `g06`, `g07` | ☑ |
| 2 | HTTP server and client, REST API, SQLite persistence, restart recovery | API contract holds; a job survives a coordinator `SIGKILL` | `g08`, `g09` | ☑ |
| 3 | Priority semantics, retries with backoff, deadlines, cancellation, dead letter | ordering, retry counts, backoff timing, timeout and cancel races all measured | `g06`, `g10`, `g11`, `g12` | ☑ |
| 4 | Workflow DAG: validation, dependency release, failure propagation, cancellation | order verified from timestamps; parallelism measured against the critical path | `g13` | ☑ |
| 5 | Distributed execution: registration, heartbeats, leases, fencing, reassignment | multi-process run completes every job; a killed worker's work is reassigned | `g14`, `g15` | ☑ |
| 6 | Benchmark suite: scaling, durations, priorities, capacity, CPU/IO, failures, workflows, HTTP, distributed | CSVs, plots and a recorded environment, all regenerable | `g16` | ☑ |
| 7 | Sanitizers, static analysis, failure injection, documentation of results | TSan/ASan/UBSan clean; static analysis clean; results recorded from an actual run | `g01`, `g03`, `g04`, `g20` | ☑ |
| 8 | Generated CV document, final report, interview preparation, evidence table | every published number recomputed from raw samples | `g17`, `g19` | ☑ |

## Work deliberately left open

Each of these is a decision, not a loose end. They are repeated in `FINAL_PROJECT_REPORT.md`
and in the "not safe to mention" table of the CV document.

| Item | Why it is open | What closing it would take |
|---|---|---|
| **Coordinator is a single point of failure** | replicating it is a different project, and hand-rolled failover is worse than an honest outage | move the queue and lease state into a replicated store (PostgreSQL with `SKIP LOCKED`, or a Raft-backed coordinator) and make the coordinator stateless enough to be replaced |
| **`Dockerfile` is unverified** | Docker is not installed on the development machine | build and run it on a machine that has Docker, then either claim it or delete it |
| **No multi-machine test** | everything was measured on one host | run the coordinator and workers on separate hosts and re-measure; the protocol already makes no localhost assumption |
| **Cooperative cancellation only** | there is no safe way to stop a thread mid-execution in C++ | run untrusted handlers out of process, where a hard kill is available |
| **No fuzzing** | the parsers face a fixed corpus of malformed inputs | run libFuzzer or AFL against the HTTP and JSON parsers |
| **No soak test** | the longest continuous run is the benchmark suite | a multi-hour run watching RSS, queue depth and latency for drift |
| **I/O-bound workload is a sleep-based stand-in** | benchmarking real file I/O would mostly measure the page cache | add a workload with a controlled working-set size larger than memory |
| **No authentication or TLS** | out of scope; it is a scheduling project, not a gateway | terminate TLS and authenticate at a reverse proxy, or add both to the HTTP layer |
