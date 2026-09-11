# FAILURE ANALYSIS

Two kinds of failure are covered here: **defects found in this system during development**,
and **failures deliberately injected** to see how the system behaves. Each defect entry says
how it was found, what actually went wrong, and what now stops it from coming back.

---

## Part 1 - defects found and fixed

### F-01 Concurrency: workflow completion was decided by a re-scan, and two threads could both win

- **Found by**: `WorkflowTest.AFailedNodeSkipsEveryDescendant` reporting
  `workflows_failed == 2` when exactly one workflow had failed.
- **Cause**: `update_workflow_state()` re-read every node of the workflow, counted terminal
  ones, and finalised if they were all terminal. Two worker threads finishing the last two
  nodes could both observe "all terminal" before either wrote, so both finalised the
  workflow and both incremented the counter. A classic read-modify-write race across a
  non-atomic pair of store calls.
- **Fix**: completion is now decided by an in-memory counter (`wf_remaining_`) decremented
  under the workflow lock, so exactly one thread can observe the transition to zero and it
  alone finalises. `recover()` rebuilds the counter from the store so a workflow interrupted
  by a restart is still finalised.
- **Prevention**: the original test now asserts the counter, and the diamond/fan-in tests
  exercise the multi-parent path that made the race reachable.

### F-02 Storage: every `:memory:` store shared one database

- **Found by**: reading the store's `open()` while writing `StoreTest`; the shared-cache URI
  was a fixed name.
- **Cause**: an in-memory SQLite database is private per connection unless it is opened with
  a shared cache and a name. The name was the constant `jobengine_mem`, so *every* store
  created in a process attached to the *same* database. Tests would have silently observed
  each other's rows, and a passing suite would have meant nothing.
- **Fix**: the name now carries a per-store atomic counter.
- **Prevention**: `StoreTest.TwoInMemoryStoresDoNotShareData` asserts that a row written to
  one store is invisible to another.

### F-03 Storage: `SQLITE_LOCKED` under concurrency was surfaced as a lost job

- **Found by**: the first benchmark run, which printed `database table is locked: jobs` and
  reported hundreds of "rejected" submissions.
- **Cause**: two compounding problems. Shared-cache mode uses *table* locks, and a reader
  holding one makes the writer fail with `SQLITE_LOCKED` - which `busy_timeout` does not
  cover, because it only handles `SQLITE_BUSY`. And every statement failure was treated as a
  hard error instead of a retryable one.
- **Fix**: a bounded retry with backoff around `step()` and `exec()` for both `SQLITE_BUSY`
  and `SQLITE_LOCKED`, plus `PRAGMA read_uncommitted=1` on reader connections **for
  in-memory stores only** - a file-backed store uses WAL, where readers never block the
  writer and the pragma is not set.
- **Prevention**: `StoreTest.ConcurrentWritersAndReadersStayConsistent` runs six threads
  through insert/update/read/update and asserts zero failures.

### F-04 Queue: liveness counters guessed which heap an entry was in

- **Found by**: review while implementing cancellation.
- **Cause**: the queue keeps a ready heap and a delayed heap, and `size()` is computed from
  two counters. When an entry was tombstoned, the code decremented "whichever counter was
  non-zero", which is only correct by luck. A cancelled delayed job could decrement the ready
  counter, making the reported queue depth wrong and, with enough operations, inconsistent.
- **Fix**: the index now records *where* each entry lives (`{seq, delayed}`), updated when a
  delayed entry is promoted, so removal always decrements the right counter.
- **Prevention**: `QueueTest.RemoveWorksForDelayedEntriesToo` and
  `QueueTest.ConcurrentRemovesAndPopsStayConsistent`, which asserts that every entry is
  either removed or popped, never both and never neither.

### F-05 HTTP: bytes of a pipelined second request were silently discarded

- **Found by**: review of the connection loop while writing the parser tests.
- **Cause**: a client is allowed to put two requests in one `write()`. The parser consumed
  the first and the server threw away the remainder of the buffer, so the second request was
  lost and the client hung until its timeout.
- **Fix**: `HttpRequestParser::leftover()` exposes the unconsumed bytes and the connection
  loop feeds them to the next request.
- **Prevention**: `HttpParserTest.ExposesBytesBelongingToTheNextPipelinedRequest`.

### F-06 Observability: a worker could be invisible but not yet counted as dead

- **Found by**: `DistributedTest.AWorkerThatStopsHeartbeating...` failing **only** under
  ThreadSanitizer, which slowed the process enough to widen the window.
- **Cause**: the watchdog cleared `alive` under the lock but incremented
  `workers_marked_dead` afterwards, outside it. An observer could see `alive_workers() == 0`
  while the counter still read zero - the two facts disagreed.
- **Fix**: the counter is incremented inside the same critical section that clears the flag.
- **Prevention**: the test that exposed it, and it is part of the ThreadSanitizer gate, where
  timing distortion makes such windows reachable rather than theoretical.

### F-07 Performance: persistence, not scheduling, was the bottleneck

- **Found by**: benchmarking worker counts and seeing throughput *fall* from 4 to 8 threads.
- **Cause**: four SQLite writes per job (insert, transition to running, attempt record,
  terminal update), each a bare statement and therefore its own implicit transaction. All of
  them serialise behind one writer, so extra threads added contention and nothing else.
- **Fix**: group commit. A flusher thread drains whatever has accumulated and writes it as
  one transaction. Submissions still block until the batch containing their row commits, so
  "durable before acknowledged" is unchanged; state transitions return immediately and are
  served from a read-through cache keyed by a monotonic version, so a reader never sees a
  stale state and a write arriving during a flush is not dropped.
- **Cost accepted**: a hard crash loses buffered transitions, and those jobs are re-executed
  on recovery - inside the at-least-once contract already documented.
- **Prevention**: `StoreTest.GroupCommitMakesAWriteVisibleImmediately` hammers the
  read-through path 200 times, and the benchmark suite keeps the A/B runnable from one binary.

### F-08 Benchmarking: the first latency numbers measured backlog, not latency

- **Found by**: a p95 of ~25 seconds for 5 ms jobs, which is not a latency figure.
- **Cause**: the driver submitted as fast as it could. With an offered load above capacity
  the queue grows without bound and "latency" becomes a measure of how long the queue is.
- **Fix**: `--rate` gives every job an intended submission time and measures latency from
  that time, so a producer that falls behind still reports the delay it caused. Saturating
  and rate-limited runs are now reported as separate sections of `BENCHMARKS.md`.
- **Prevention**: the methodology section states which regime each table belongs to.

### F-09 Tuning: an "obvious" optimisation made things worse

- **Found by**: testing a commit delay to grow batch sizes.
- **Result**: batch size rose to exactly 16 rows and stayed there while throughput fell
  monotonically with the delay. Because submitters block until their batch commits, a delay
  puts the whole system in lock-step: everyone waits, one batch commits, repeat. With no
  delay it self-tunes, because whatever arrives during one transaction becomes the next batch.
- **Outcome**: the default is no delay. The knob was kept precisely because it is how the
  decision was measured.

### F-11 HTTP: long-polling workers could starve every other client

- **Found by**: the benchmark suite, at the largest distributed configuration. With 8 worker
  processes of 2 threads each, the run died on `GET /metrics` returning nothing.
- **Cause**: the HTTP server used a *fixed* pool of connection threads, and each worker
  thread holds its connection open to long-poll `/internal/claim`. 8 processes x 2 threads =
  16 persistent connections against 16 connection threads, so every thread was permanently
  occupied and any new connection sat in the accept backlog indefinitely. The API was, in
  effect, unreachable - not slow, unreachable - while the worker fleet was large enough.
  The smaller configurations (1, 2 and 4 processes) never reached the threshold, which is
  why it only appeared at the end of a long run.
- **Fix**: the pool now has a floor rather than a ceiling. When a connection is accepted and
  no thread is idle, the acceptor adds one, up to `max_connection_threads` (default 256).
  Under ordinary load nothing is starved and no thread is ever added, so the behaviour of
  the measured paths is unchanged; the growth path exists for exactly this situation.
  `--http-threads` still sets the floor, and the benchmark now sizes it for the worker fleet
  so the measurement is about scheduling rather than about pool growth.
- **Prevention**: `HttpServerTest.LongLivedConnectionsDoNotStarveNewOnes` fills a two-thread
  pool with requests that block, then asserts that a fresh client is still served promptly
  **and** that `threads_spawned_on_demand()` is non-zero - so the test cannot pass by luck
  if the growth path is ever removed.
- **Also fixed**: the benchmark harness crashed on the failed request instead of recording a
  degraded row, which lost every result after it. A failed metrics fetch is now a warning
  and an empty column.

### F-12 Storage: `flush()` could return before an attempt-only batch was committed

- **Found by**: the ThreadSanitizer gate, as an intermittent failure of
  `StoreTest.AttemptsArePerAttemptAndIdempotent` - three of five runs - with **zero** race
  warnings. The sanitizer did not detect a race; it slowed execution enough to open a window
  that the ordinary build almost always closed before anyone looked.
- **Cause**: the group-commit flusher tracked how many rows were inside the open transaction
  as `inserts.size() + updates.size()`, omitting attempt rows. A batch made only of attempt
  records therefore reported zero rows in flight, so `flush()` saw "nothing pending, nothing
  in flight" and returned while that transaction was still open. A caller that flushed and
  then read `job_attempts` could see nothing at all. `GET /jobs/{id}/attempts` is exactly
  that sequence.
- **Fix**: count attempt rows in the in-flight total.
- **Prevention**: `StoreTest.FlushWaitsForAttemptOnlyBatches` writes attempts without
  touching the job row - so the batch contains nothing else - and repeats 40 times. The full
  suite now passes 132/132 on five consecutive ThreadSanitizer runs.
- **Worth noting**: this is the second bug in this project that only a sanitizer build
  surfaced (see F-06). Neither was a data race. Slowing the program down is a way of finding
  ordering assumptions, not just races.

### F-10 Static analysis: one finding, investigated rather than suppressed

- **Found by**: `g20_static.mjs` reporting `recv` called "inside a critical section".
- **Investigation**: the lock is released before the call. A probe fixture
  (`scripts/gates/fixtures/block_in_critical_section_probe.cpp`) shows the checker handles
  `lock_guard`, scoped `unique_lock` and explicit `unlock()` correctly, but reports a false
  positive on any path that went through `std::condition_variable::wait`.
- **Outcome**: the code was still restructured (`next_connection()`) so the lock's scope is
  obvious, and the gate excludes only findings that match the demonstrated signature - and
  prints each excluded finding rather than hiding it. If the probe ever stops reproducing the
  false positive, the gate fails and demands the exclusion be removed.

---

## Part 2 - injected failures and observed behaviour

Each row is exercised by a gate or a test, not by inspection.

| Injected failure | How it is injected | Observed behaviour | Where |
|---|---|---|---|
| Handler returns an error | `fail` handler | attempt recorded `FAILED`, retried with backoff, `FAILED` (dead letter) when exhausted | `g10` |
| Handler throws a C++ exception | `throw` handler | caught at the worker boundary, becomes a failed attempt; the process survives | `EngineTest.AHandlerThatThrows...` |
| Handler exceeds its deadline | 10 s job with a 200 ms deadline | cancellation flag set, attempt recorded `TIMED_OUT`, retried or dead-lettered | `g11` |
| Handler ignores cancellation | `sleep_uncooperative` | job still recorded `TIMED_OUT`, but the worker thread is held until the handler returns - the documented limit | `g11` |
| Client cancels a queued job | `POST /jobs/{id}/cancel` | removed from the queue, `CANCELLED`, queue depth and admission slot released, never executed | `g12` |
| Client cancels at the moment of start | 150 submit-then-cancel races | each job settles in exactly one terminal state; a job cancelled while queued never executed | `g12` |
| Queue saturated | submit past `max_outstanding` | `503` with an error message, no row created, admission slot released | `g08`, benchmark section on capacity |
| Worker process killed mid-job | `SIGKILL` on `jobengine-worker` | missed heartbeats detected, worker marked `DEAD`, leases expired, jobs requeued and completed by a replacement | `g15` |
| Killed worker reports late | stale lease token replayed | `{"accepted":false,"reason":"stale_lease"}`; the job row is untouched | `g15` |
| Worker silently slow (no crash) | claim, then never report | lease expires, job reassigned with a new token and the next attempt number | `DistributedTest.AnExpiredLease...` |
| Worker repeatedly abandons a job | claim and abandon until attempts run out | job becomes `FAILED` with "lease expired and retry attempts exhausted" - reassignment is bounded | `DistributedTest.ReassignmentStopsWhen...` |
| Coordinator killed hard | `SIGKILL` on `jobengine-server` | on restart, `RUNNING` and `QUEUED` rows are recovered and completed; the interrupted attempt still counts, so retries stay bounded | `g09` |
| Coordinator shut down with work in flight | `SIGTERM` | exits 0 within its bound, nothing left `RUNNING`, every job accounted for as succeeded or recoverable | `g07` |
| Upstream workflow node fails | `fail` node inside a DAG | every transitive descendant becomes `SKIPPED` and never executes; unrelated branches still complete; workflow `FAILED` | `g13` |
| Malformed HTTP request | 9 malformed shapes | specific status (`400`, `411`, `413`, `414`, `431`, `505`) with a JSON error body; the connection is not left half-parsed | `HttpParserTest` |
| Malformed JSON body | 16 malformed documents | `400` with a parse error message; deeply nested input rejected by the depth limit | `JsonTest`, `g08` |
| Handler throws inside an HTTP request | `/boom` route in the test server | `500` with the exception text; the connection and process survive | `HttpServerTest` |

## Part 3 - failures that are NOT handled

Stated because an unlisted gap reads as an oversight rather than a decision.

- **Coordinator loss is an outage.** Nothing fails over. State survives, availability does not.
- **Disk full or I/O error** is surfaced as a store error and logged; there is no retry
  policy, no alerting and no graceful degradation for it.
- **A handler that blocks forever in-process** holds its worker thread permanently. Only the
  out-of-process path has a hard bound.
- **A handler that calls `std::terminate` or corrupts memory** takes the process down; the
  worker boundary catches exceptions, not aborts.
- **Clock steps.** Scheduling uses the steady clock and persistence uses the wall clock, and
  they are kept deliberately separate, but no test forces an NTP-style step to prove it.
- **Network partition between coordinator and worker** is handled the same way as a crash
  (lease expiry, reassignment), which means duplicate execution is possible - the
  at-least-once contract. There is no attempt to distinguish the two.
