# REQUIREMENTS

## 1. Problem statement

Applications frequently need to run work **outside the request path**: work that is slow,
retryable, ordered by importance, or composed of dependent steps. Doing this inline in an
HTTP handler couples client latency to work duration, loses work on crash, and provides no
way to prioritise, retry, cap, or observe execution.

This project builds a **job processing and workflow engine**: a service that accepts units
of work over HTTP, durably records them, schedules them by priority, executes them on a
bounded pool of workers (in-process threads and/or separate worker processes), enforces
per-job timeouts and retry policies, and tracks dependent jobs as a DAG so that a job runs
only after its dependencies have succeeded.

Concretely it answers these questions for a caller:

| Question | Mechanism |
|---|---|
| "Run this for me, I do not want to wait." | `POST /jobs` returns an id immediately |
| "Did it finish?" | `GET /jobs/{id}` returns state, attempts, result, timings |
| "This one matters more." | integer priority, strict priority scheduling with FIFO tie-break |
| "It failed for a transient reason." | bounded retries with exponential backoff |
| "It hung." | per-job deadline, cooperative cancellation, lease expiry across processes |
| "Stop it." | `POST /jobs/{id}/cancel` |
| "Run B only after A succeeds." | `POST /workflows` with a validated DAG |
| "Did we lose anything when the process died?" | SQLite-backed metadata, recovery on restart |
| "How fast is it?" | `GET /metrics` plus a reproducible benchmark suite |

## 2. Non-goals (explicitly out of scope)

These are excluded deliberately, not left unfinished. Each is listed with the reason.

- **Arbitrary user-supplied code execution.** Handlers are registered in C++ at build time.
  Accepting arbitrary binaries/scripts from the network is a sandboxing problem, not a
  scheduling problem, and would dominate the project.
- **Multi-machine consensus / replicated coordinator.** There is exactly one coordinator
  process. It is a single point of failure. Implementing Raft to remove it would be a
  different project. See `docs/ARCHITECTURE.md` §9.
- **Exactly-once execution.** Not achievable without transactional side effects in the
  handler. The system provides **at-least-once execution with exactly-once result recording**
  (lease fencing). See `docs/DESIGN_DECISIONS.md` D-09.
- **Preemption of running jobs.** A running job cannot be forcibly stopped mid-instruction;
  cancellation and timeout are **cooperative** in-process. See D-07.
- **Kubernetes / cloud deployment / service mesh.** Not justified for a single coordinator
  plus N worker processes.
- **Authentication / authorisation / TLS.** The API is unauthenticated and intended for
  localhost or a trusted network. This is a real limitation, stated rather than hidden.

## 3. Minimum viable product (MVP)

MVP = Phase 1 + Phase 2. The system is useful at this point.

- M1. Submit a job with a type, JSON payload, and priority.
- M2. Thread-safe queue shared by multiple producers and multiple consumers.
- M3. Configurable worker-thread pool that executes jobs via a type→handler registry.
- M4. Job lifecycle states persisted and queryable.
- M5. Strict priority scheduling; FIFO within a priority level.
- M6. Graceful shutdown: stop accepting, drain or abandon in-flight work deterministically,
      join all threads, no leaked threads, no torn database state.
- M7. REST API: `POST /jobs`, `GET /jobs/{id}`, `GET /jobs`, `POST /jobs/{id}/cancel`,
      `GET /health`.
- M8. SQLite persistence of job metadata; jobs survive a coordinator restart.
- M9. Unit tests for queue, state machine, JSON, HTTP parsing, store.

## 4. Extended features

- E1. Retry policy: `max_attempts`, exponential backoff, capped delay, optional jitter.
- E2. Per-job timeout with cooperative cancellation and a watchdog.
- E3. Cancellation of queued and running jobs, with defined race semantics.
- E4. Terminal failure state (dead letter) after retries are exhausted.
- E5. Workflow DAG: creation, cycle rejection, dependency tracking, ready-set detection,
      failure propagation to descendants, workflow-level status, parallel execution of
      independent nodes.
- E6. Distributed execution: worker processes register, heartbeat, lease jobs over HTTP,
      and return results; the coordinator expires leases from dead workers and reassigns.
- E7. Observability: in-process counters and latency histograms exposed at `GET /metrics`.
- E8. Reproducible benchmark suite producing CSV + plots + a recorded environment.

## 5. Entities

| Entity | Definition | Identity | Persisted |
|---|---|---|---|
| **Job** | One unit of work: type, payload, priority, retry policy, deadline, lifecycle state, attempt counter, result/error, timestamps | `job_id` (ULID-style, lexicographically sortable) | yes (`jobs`) |
| **JobType / Handler** | A named C++ function `JobResult(const JobContext&)` registered in an executor registry | type string (e.g. `sleep`, `cpu_fib`) | no (code) |
| **Queue** | In-memory scheduling structure inside the coordinator: a ready max-heap ordered by (priority desc, sequence asc) and a delayed min-heap ordered by `ready_at` | n/a | no (rebuilt from `jobs` on restart) |
| **Worker** | A consumer of jobs. Either an in-process thread of the embedded pool, or a separate `jobengine-worker` process that leases jobs over HTTP | `worker_id` (remote only) | yes (`workers`, remote only) |
| **Lease** | Time-bounded exclusive right of one worker to execute one attempt of one job. Carries a fencing token | `(job_id, attempt, lease_token)` | yes (columns on `jobs`) |
| **RetryPolicy** | `max_attempts`, `backoff_base_ms`, `backoff_multiplier`, `backoff_max_ms`, `jitter` | embedded in Job | yes |
| **JobResult** | Terminal outcome of one attempt: status + JSON result or error string + duration | `(job_id, attempt)` | yes (`job_attempts`) |
| **Workflow** | A validated DAG of jobs submitted as one unit, with its own aggregate status | `workflow_id` | yes (`workflows`) |
| **Dependency** | A directed edge `parent → child`; child is eligible only when every parent has SUCCEEDED | `(workflow_id, parent_job_id, child_job_id)` | yes (`job_dependencies`) |

## 6. Job lifecycle states

Ten states. Every transition is validated in one place (`core/job_state.cpp`) and unit-tested.

| State | Meaning | Terminal |
|---|---|---|
| `PENDING` | created, waiting on unmet workflow dependencies | no |
| `QUEUED` | eligible now, sitting in the ready queue | no |
| `SCHEDULED` | eligible at a future time (`submit` with `delay_ms`) | no |
| `RUNNING` | leased to a worker, handler executing | no |
| `RETRYING` | previous attempt failed/timed out, waiting out backoff before re-queue | no |
| `SUCCEEDED` | handler returned success | **yes** |
| `FAILED` | retries exhausted after handler failure (dead letter) | **yes** |
| `TIMED_OUT` | retries exhausted after deadline expiry (dead letter, distinguished from FAILED) | **yes** |
| `CANCELLED` | cancelled by the client | **yes** |
| `SKIPPED` | an ancestor in the workflow reached a terminal failure state | **yes** |

Legal transitions:

```
PENDING   -> QUEUED | SCHEDULED | CANCELLED | SKIPPED
SCHEDULED -> QUEUED | CANCELLED
QUEUED    -> RUNNING | CANCELLED
RUNNING   -> SUCCEEDED | RETRYING | FAILED | TIMED_OUT | CANCELLED | QUEUED(*)
RETRYING  -> QUEUED | CANCELLED | FAILED | TIMED_OUT
(*) RUNNING -> QUEUED occurs only on lease expiry / worker death (reassignment).
terminal states have no outgoing transitions.
```

## 7. Functional requirements

- FR-1  `POST /jobs` accepts `{type, payload, priority?, max_attempts?, timeout_ms?, delay_ms?, backoff_*?}` and returns `201` with `{job_id, state}`.
- FR-2  Unknown `type` is rejected with `400` before the job is persisted.
- FR-3  `GET /jobs/{id}` returns the full record; unknown id returns `404`.
- FR-4  `GET /jobs?state=&limit=&offset=` lists jobs newest-first.
- FR-5  `POST /jobs/{id}/cancel` cancels a `PENDING|QUEUED|SCHEDULED|RETRYING` job synchronously and requests cooperative cancellation for a `RUNNING` job; cancelling a terminal job returns `409`.
- FR-6  `GET /health` returns `200` with queue depth, worker counts, and uptime.
- FR-7  `GET /metrics` returns counters and latency percentiles.
- FR-8  Higher `priority` is dequeued first; equal priority is dequeued in submission order.
- FR-9  A failed attempt is retried up to `max_attempts` with exponential backoff, then becomes `FAILED`.
- FR-10 A job exceeding `timeout_ms` has its cancellation flag set; the attempt is recorded as timed out and retried or moved to `TIMED_OUT`.
- FR-11 The queue is bounded; submission beyond capacity returns `503` rather than growing without limit.
- FR-12 `POST /workflows` validates the DAG (unknown node references and cycles are `400`), persists it, and queues the zero-indegree nodes.
- FR-13 A workflow node runs only after **all** its parents have `SUCCEEDED`.
- FR-14 If a node reaches a terminal failure state, all transitive descendants become `SKIPPED` and the workflow becomes `FAILED`.
- FR-15 On restart, jobs left `RUNNING` or `QUEUED` are recovered and re-queued; terminal jobs are untouched.
- FR-16 A remote worker registers, heartbeats, leases jobs, and reports results; a worker that stops heartbeating has its leases expired and its jobs reassigned.
- FR-17 A result carrying a stale lease token is rejected, so exactly one attempt's result is recorded per job.

## 8. Non-functional requirements

- NFR-1 No data races under ThreadSanitizer for the queue, engine, and store tests.
- NFR-2 No leaks under LeakSanitizer/ASan for the test suite.
- NFR-3 Every job submitted is eventually accounted for: no lost jobs, no double *completion*.
- NFR-4 Shutdown completes within a bounded time and joins every thread.
- NFR-5 All reported performance numbers are reproducible by a scripted command and recorded with the environment they were measured on.
- NFR-6 The build requires no package manager beyond CMake, a C++20 compiler, and system SQLite.
