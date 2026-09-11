# ARCHITECTURE

## 1. Component map

```
                          coordinator process (jobengine-server)
  ┌──────────────────────────────────────────────────────────────────────────┐
  │                                                                          │
  │   HttpServer (BSD sockets, N acceptor-dispatched connection threads)     │
  │        │                                                                 │
  │        ▼                                                                 │
  │   ApiRouter  ── /jobs /workflows /health /metrics /internal/*            │
  │        │                                                                 │
  │        ▼                                                                 │
  │   JobEngine  ─────────────────────────────────────────────────────────┐  │
  │        │                                                              │  │
  │        ├── WorkflowGraph   (DAG: indegree, adjacency, propagation)    │  │
  │        ├── PriorityJobQueue (ready max-heap + delayed min-heap)       │  │
  │        ├── WorkerPool       (K threads, embedded execution)           │  │
  │        ├── LeaseTable       (remote assignments + fencing tokens)     │  │
  │        ├── Watchdog thread  (deadlines, lease expiry, worker liveness)│  │
  │        └── Metrics          (counters + latency histograms)           │  │
  │        │                                                              │  │
  │        ▼                                                              │  │
  │   JobStore  ── SQLite (WAL) ── jobs, job_attempts, workflows,         │  │
  │                                job_dependencies, workers              │  │
  └──────────────────────────────────────────────────────────────────────┘  │
                    ▲  HTTP: register / heartbeat / claim / complete         │
                    │                                                        │
      ┌─────────────┴───────────────┬──────────────────────┐                 │
      │                             │                      │                 │
  jobengine-worker #1          worker #2              worker #N  (separate processes)
   (P threads each)
```

## 2. Request path for `POST /jobs`

1. Connection thread reads the request, parses headers + `Content-Length` body.
2. `ApiRouter` parses JSON, validates fields, rejects unknown `type` (`400`).
3. `JobEngine::submit` allocates a `job_id`, writes the row to SQLite **inside a
   transaction**, then pushes the queue entry.
   Persist-before-enqueue is deliberate: a crash between the two loses nothing that
   was acknowledged, because the client has not been acknowledged yet.
4. If the queue is at capacity the row is rolled back and `503` is returned.
5. `201 {job_id, state}` is written back.

## 3. Execution path

1. A worker (embedded thread, or a remote process via `POST /internal/claim`) pops the
   highest-priority ready entry.
2. The engine transitions `QUEUED → RUNNING`, increments `attempt`, stamps
   `started_at` and `deadline_at`, and issues a **lease token** (random 64-bit).
3. The handler runs with a `JobContext` exposing the payload, attempt number, and a
   `cancel_requested()` flag.
4. On return, the engine validates the lease token is still current, then:
   - success → `SUCCEEDED`, result stored;
   - failure and `attempt < max_attempts` → `RETRYING`, re-pushed with `ready_at = now + backoff`;
   - failure and attempts exhausted → `FAILED`;
   - cancellation flag set by the deadline watchdog → `TIMED_OUT` path (same retry rule);
   - cancellation flag set by a client cancel → `CANCELLED`.
5. Terminal states notify `WorkflowGraph`, which may release children or skip descendants.
6. The attempt row is appended to `job_attempts`; the `jobs` row is updated. Both in one
   transaction.

## 4. Concurrency model

**Threads in the coordinator**

| Thread group | Count | Role |
|---|---|---|
| HTTP acceptor | 1 | `accept()` loop, hands the fd to a connection thread |
| HTTP connection | pool with a floor of `--http-threads` (default 16), grown on demand to 256 | parse request, run handler, write response, keep-alive |
| Embedded workers | `--workers` (default: hardware concurrency) | pop from queue, execute handler |
| Watchdog | 1 | scans for expired deadlines, expired leases, dead remote workers, matured delayed entries |
| Main | 1 | signal handling, orchestrates graceful shutdown |

**Synchronisation primitives**

- `PriorityJobQueue`: one `std::mutex` + one `std::condition_variable`. Both the ready
  heap and the delayed heap live under that single lock, so "the earliest delayed job"
  and "is there a ready job" are decided atomically. Consumers block in
  `wait_until(earliest_delayed_ready_at)`, so no polling and no separate timer thread.
- `WorkflowGraph`: its own `std::mutex` protecting indegree counters and adjacency.
- `JobStore`: SQLite opened in serialized threading mode, with one `std::mutex`
  serialising write transactions (SQLite allows exactly one writer at a time; the mutex
  turns `SQLITE_BUSY` contention into ordinary lock waiting).
- `Metrics`: `std::atomic<uint64_t>` counters; histograms use per-bucket atomics.
- Cancellation flags: `std::shared_ptr<std::atomic<bool>>` per running job, so the
  watchdog can set it without holding the engine lock and without a dangling reference.

**Lock ordering (deadlock avoidance)**

```
workflow_mutex  >  queue_mutex  >  store_mutex
```
Locks are acquired in that order only, and never upgraded. No handler is ever invoked
while holding any engine lock — the worker copies the job record, releases every lock,
then executes. This is what keeps a slow handler from blocking submissions.

**Why the connection pool grows.** Worker processes long-poll `/internal/claim`, so each
worker thread holds a connection open. With a fixed pool, a worker fleet as large as the
pool occupies every thread and makes the API unreachable to everyone else. The acceptor
therefore adds a connection thread whenever it accepts a connection and no thread is idle,
up to a hard cap. Under ordinary load no thread is ever added. See `FAILURE_ANALYSIS.md`
F-11.

**Graceful shutdown sequence**

1. Stop the HTTP acceptor (new connections refused); in-flight requests finish.
2. Close the queue: `pop()` stops blocking and returns `nullopt` once drained.
3. Signal cancellation to running jobs (`drain` mode waits for them instead).
4. Join watchdog, worker threads, connection threads, acceptor — in that order.
5. Flush final state to SQLite, `PRAGMA wal_checkpoint(TRUNCATE)`, close.

## 5. Storage schema (SQLite)

```sql
-- This is the schema exactly as created by src/store/store.cpp.
CREATE TABLE IF NOT EXISTS jobs (
  job_id           TEXT PRIMARY KEY,
  type             TEXT NOT NULL,
  payload          TEXT NOT NULL DEFAULT '{}',
  priority         INTEGER NOT NULL DEFAULT 0,
  state            TEXT NOT NULL,
  attempt          INTEGER NOT NULL DEFAULT 0,
  max_attempts     INTEGER NOT NULL DEFAULT 1,
  timeout_ms       INTEGER NOT NULL DEFAULT 0,
  backoff_base_ms  INTEGER NOT NULL DEFAULT 100,
  backoff_mult     REAL    NOT NULL DEFAULT 2.0,
  backoff_max_ms   INTEGER NOT NULL DEFAULT 30000,
  backoff_jitter   INTEGER NOT NULL DEFAULT 0,
  created_at_ms    INTEGER NOT NULL,
  started_at_ms    INTEGER NOT NULL DEFAULT 0,
  finished_at_ms   INTEGER NOT NULL DEFAULT 0,
  ready_at_ms      INTEGER NOT NULL DEFAULT 0,
  queue_wait_us    INTEGER NOT NULL DEFAULT 0,
  exec_us          INTEGER NOT NULL DEFAULT 0,
  result           TEXT,
  error            TEXT,
  workflow_id      TEXT,
  workflow_node    TEXT,
  pending_deps     INTEGER NOT NULL DEFAULT 0,
  lease_token      INTEGER NOT NULL DEFAULT 0,
  lease_owner      TEXT,
  lease_expires_ms INTEGER NOT NULL DEFAULT 0,
  cancel_requested INTEGER NOT NULL DEFAULT 0,
  seq              INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_jobs_state    ON jobs(state);
CREATE INDEX IF NOT EXISTS idx_jobs_created  ON jobs(created_at_ms DESC);
CREATE INDEX IF NOT EXISTS idx_jobs_workflow ON jobs(workflow_id);

CREATE TABLE IF NOT EXISTS job_attempts (
  job_id      TEXT NOT NULL,
  attempt     INTEGER NOT NULL,
  status      TEXT NOT NULL,
  started_ms  INTEGER NOT NULL,
  finished_ms INTEGER NOT NULL,
  duration_ms INTEGER NOT NULL,
  worker      TEXT,
  error       TEXT,
  PRIMARY KEY (job_id, attempt)
);

CREATE TABLE IF NOT EXISTS workflows (
  workflow_id    TEXT PRIMARY KEY,
  name           TEXT NOT NULL,
  state          TEXT NOT NULL,
  created_at_ms  INTEGER NOT NULL,
  finished_at_ms INTEGER NOT NULL DEFAULT 0,
  node_count     INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS job_dependencies (
  workflow_id   TEXT NOT NULL,
  parent_job_id TEXT NOT NULL,
  child_job_id  TEXT NOT NULL,
  PRIMARY KEY (workflow_id, parent_job_id, child_job_id)
);
CREATE INDEX IF NOT EXISTS idx_deps_parent ON job_dependencies(parent_job_id);

CREATE TABLE IF NOT EXISTS workers (
  worker_id         TEXT PRIMARY KEY,
  name              TEXT NOT NULL,
  capacity          INTEGER NOT NULL,
  state             TEXT NOT NULL,
  registered_ms     INTEGER NOT NULL,
  last_heartbeat_ms INTEGER NOT NULL,
  jobs_completed    INTEGER NOT NULL DEFAULT 0
);
```

`PRAGMA journal_mode=WAL` (concurrent readers with one writer),
`PRAGMA synchronous=NORMAL` (a documented durability/throughput trade-off — see D-06),
`PRAGMA foreign_keys=ON`, `PRAGMA busy_timeout=5000`.

All `*_ms` columns hold **wall-clock** milliseconds. Durations are stored in
**microseconds** (`queue_wait_us`, `exec_us`) because a duration keeps its meaning across
processes while a steady-clock timestamp does not, and millisecond resolution is too coarse
to measure scheduling overhead.

**Writes are group-committed** (D-14). Submission blocks until the transaction containing
its row commits, so an acknowledged job is durable. Subsequent state transitions are
buffered, served from a read-through cache so no reader observes a stale state, and written
in batched transactions; the buffer has a high-water mark that applies backpressure rather
than growing without bound (D-15).

## 6. HTTP API

| Method | Path | Body / Query | Success | Errors |
|---|---|---|---|---|
| POST | `/jobs` | `{type, payload?, priority?, max_attempts?, timeout_ms?, delay_ms?, backoff_base_ms?, backoff_multiplier?, backoff_max_ms?, backoff_jitter?}` | `201 {job_id,state}` | `400` bad JSON / unknown type / invalid field, `503` at capacity |
| GET | `/jobs` | `?state=&limit=&offset=` | `200 {jobs:[...],count,total}` | `400` bad parameter |
| GET | `/jobs/{id}` | — | `200 <job object>` | `404` |
| GET | `/jobs/{id}/attempts` | — | `200 {job_id, attempts:[...]}` | `404` |
| POST | `/jobs/{id}/cancel` | — | `200 {job_id,state,cancel_requested}` | `404`, `409` already terminal |
| POST | `/workflows` | `{name, nodes:[{name,type,payload?,...}], edges:[{from,to}]}` | `201 {workflow_id, state, jobs:{name:job_id}}` | `400` with a `reason` (cycle, self edge, unknown node, duplicate name, unknown type), `503` at capacity |
| GET | `/workflows` | `?limit=` | `200 {workflows:[...],count}` | `400` bad parameter |
| GET | `/workflows/{id}` | — | `200 {workflow_id,name,state,node_count,nodes:[...]}` | `404` |
| POST | `/workflows/{id}/cancel` | — | `200 {workflow_id,state}` | `404`, `409` not running |
| GET | `/health` | — | `200 {status,uptime_ms,queue_depth,queue_ready,queue_delayed,running,outstanding,worker_threads,remote_workers_alive,job_types}` | — |
| GET | `/metrics` | — | `200 {counters, latency, jobs_by_state}` | — |
| GET | `/routes` | — | `200 {routes:[...]}` — the route table itself, used by the docs drift check | — |
| POST | `/internal/workers/register` | `{name, capacity}` | `200 {worker_id, heartbeat_interval_ms, lease_duration_ms}` | `400` |
| POST | `/internal/workers/heartbeat` | `{worker_id}` | `200 {ok, cancel:[job_id]}` | `404` unknown worker |
| POST | `/internal/claim` | `{worker_id, max_jobs, wait_ms}` | `200 {jobs:[{job_id,type,payload,attempt,lease_token,timeout_ms,lease_expires_ms}],count}` | `400` bad parameter |
| POST | `/internal/complete` | `{worker_id,job_id,attempt,lease_token,status,result?,error?}` | `200 {accepted:true}` or `200 {accepted:false,reason:"stale_lease"}` | `400` bad status, `404` unknown job |

`lease_token` travels as a decimal **string**: it is a full 64-bit value and JSON numbers
lose precision above 2^53.

Unknown paths return `404 {"error":"no such endpoint: ..."}`; a known path with the wrong
method returns `405`, so a client integrating against the API can tell the two apart.

Job object shape:
```json
{"job_id":"01J...","type":"sleep","payload":{"ms":50},"priority":5,"state":"SUCCEEDED",
 "attempt":1,"max_attempts":3,"timeout_ms":1000,"created_at_ms":...,"started_at_ms":...,
 "finished_at_ms":...,"queue_wait_ms":...,"exec_ms":...,"result":{...},"error":null,
 "workflow_id":null}
```

## 7. Scheduling semantics

- **Strict priority, not weighted.** The ready heap is ordered by `(priority DESC, seq ASC)`.
  A continuous stream of high-priority jobs *can* starve low-priority jobs; this is a
  property of strict priority and is measured rather than hidden (see `BENCHMARKS.md`).
- **Priority affects queued jobs only.** Once a job is `RUNNING` it is not preempted by a
  higher-priority arrival.
- **Delayed eligibility.** `delay_ms` at submit (`SCHEDULED`) and retry backoff (`RETRYING`)
  use the same delayed heap; a delayed job competes on priority only after it matures.

## 8. What is and is not distributed

**Version 1 (Phases 1–4) is a single process.** "Distributed" is not claimed there.
Concurrency is threads inside one process; the queue is in memory; persistence is a local
file.

**Phase 5 adds multi-process execution.** The coordinator remains one process and owns the
queue, the database, and all scheduling decisions. Worker processes are separate OS
processes (on the same machine, or any machine that can reach the coordinator's TCP port)
that pull work over HTTP.

| Property | Status |
|---|---|
| Multiple worker processes | **yes**, tested |
| Worker on a different machine | possible (plain TCP/HTTP, no localhost assumption) — **not tested on a second machine** |
| Worker crash detection + reassignment | **yes**, tested via heartbeat expiry and lease expiry |
| Coordinator crash | **single point of failure**; state survives in SQLite and is recovered on restart, but the service is down while it is down |
| Exactly-once execution | **no** — at-least-once execution, exactly-once *result recording* via lease fencing |
| Network partition | a partitioned worker's lease expires and its job is reassigned; its late result is rejected as stale |
| Replication / consensus | **not implemented** |

Vocabulary used consistently in all measurements:
**thread** (OS thread inside a process) · **process** (`jobengine-server` or
`jobengine-worker`) · **machine** (physical host; always 1 here) · **concurrent jobs**
(jobs in `RUNNING` at one instant, bounded by total worker slots).

## 9. Failure model

| Failure | Detection | Response | Residual risk |
|---|---|---|---|
| Handler throws / returns error | return value or `catch` in the worker | attempt recorded, retry with backoff, else `FAILED` | a handler that calls `std::terminate` kills the process |
| Handler exceeds `timeout_ms` | watchdog compares `deadline_at` | cancel flag set; attempt recorded `TIMED_OUT` | a handler that never checks the flag holds its thread until it returns |
| Remote worker crashes | heartbeat older than `3 × interval` | worker marked `DEAD`, leases expired, jobs re-queued | the job may have partially executed → at-least-once |
| Remote worker is slow/partitioned | lease past `lease_expires_ms` | job re-queued to another worker | **duplicate execution is possible here**; the late result is rejected by fencing token |
| Coordinator crash | — | on restart, `RUNNING` and `QUEUED` rows are re-queued; terminal rows untouched | jobs interrupted mid-execution are re-run (at-least-once) |
| SQLite write error | return code checked on every statement | transaction rolled back, error surfaced to the caller | disk-full is not specially handled |
| Queue full | capacity check under the queue lock | `503` backpressure to the client | none |
