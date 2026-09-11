# INTERVIEW PREPARATION

Every answer here is one you can defend by opening a file or running a command. Where the
honest answer is "I did not do that", it says so - an interviewer trusts a candidate who
draws the boundary before being pushed to it.

---

## A. The 90-second summary

> I built a job-processing and workflow engine in C++20. You submit work over HTTP; it is
> written durably, scheduled by priority, and executed on a bounded pool - either worker
> threads inside the coordinator or separate worker processes that lease jobs. It handles
> retries with exponential backoff, per-job deadlines, cancellation, and dependency DAGs
> where a node runs only after all its parents have succeeded. The parts I find most
> interesting are the persistence bottleneck I found by benchmarking and fixed with group
> commit, and the lease-fencing protocol that gives at-least-once execution with
> exactly-once result recording. Every number I quote is produced by a script in the repo.

---

## B. Architecture and design

**Q. Draw the system.**
One coordinator process owns the queue, the SQLite database and every scheduling decision.
Inside it: an HTTP server, a router, and the engine, which owns a priority queue, a worker
pool, a workflow graph, a lease table and a watchdog thread. Execution happens either on
embedded worker threads or in separate `jobengine-worker` processes that call
`/internal/claim` and `/internal/complete`. Both paths share the same handler registry,
retry accounting and metrics, because the worker pool consumes an abstract task source.

**Q. Why is the queue in memory when you have a database?**
Because dequeuing from a database means a write transaction per pop, and the ordering scan
costs an index walk each time. Scheduling should happen at memory speed. The database is
still the authority: a job row is committed before the client is acknowledged, and on
restart the queue is rebuilt by selecting non-terminal jobs ordered by `(priority DESC,
seq ASC)`, which reproduces the original order. The cost is that the queue is not shared
between processes, which is why remote workers ask the coordinator for work instead of
reading a shared structure - and that turns out to be what makes lease fencing possible.

**Q. Walk me through the queue data structure.**
One mutex and one condition variable guarding two heaps: a ready max-heap ordered by
`(priority DESC, seq ASC)`, and a delayed min-heap ordered by `ready_at`. Putting both under
one lock means "is anything ready?" and "when does the earliest delayed entry mature?" are
decided atomically, so a consumer can block in `wait_until(earliest_ready_at)` - no polling
loop, no separate timer thread. Cancellation uses sequence-keyed tombstones, so removal is
O(1) instead of an O(n) heap rebuild, and a later re-push of the same job (a retry) is
unaffected because it carries a fresh sequence number.

**Q. What is your lock ordering and how do you know you cannot deadlock?**
Three locks, always acquired in the order workflow > queue > store, never upgraded. The
running-state lock doubles as the job-transition lock and is never nested with the workflow
lock: anything needing both collects what it needs under one, releases, then acts. And no
handler is ever invoked while an engine lock is held - the worker copies the job record,
releases everything, then executes. That last rule is what stops a slow job from blocking
submissions. The evidence is ThreadSanitizer over the whole suite reporting zero findings,
plus every test being bounded by a timeout, so a deadlock fails the suite rather than
hanging it.

**Q. Why is the running-state lock also the transition lock?**
Because `cancel()` and "a worker starts this job" are a read-decide-write on the same row
and must be mutually exclusive, otherwise a cancel can land between a worker reading
`QUEUED` and writing `RUNNING`, and the job would be recorded cancelled while it executes.
Reusing one lock keeps the invariant in one place. The cost is that every attempt start
serialises briefly; the critical section is two store calls and no I/O wait, and the
benchmark measures what it costs rather than assuming it is free.

---

## C. The performance work

**Q. Tell me about a performance problem you found and fixed.**
I benchmarked throughput against worker-thread count expecting a scaling curve and got the
opposite: throughput *fell* between four and eight threads. That ruled out "not enough
parallelism" and pointed at contention. Counting writes showed four SQLite statements per
job - insert, transition to running, attempt record, terminal update - and each bare
statement is its own implicit transaction, so all four serialise behind SQLite's single
writer. More threads only meant more threads waiting.

The fix was group commit. A single flusher thread drains whatever has accumulated and
writes it as one transaction. Submissions still block until the batch containing their row
commits, so "durable before acknowledged" is unchanged. State transitions return
immediately and are served from a read-through cache keyed by a monotonic version, so a
reader never sees a stale state, and a write that arrives while a flush is in progress is
not dropped - the cache entry is retired only if the exact version that was written is
still the newest one.

The A/B is in the repo and runs from one binary: `--group-commit false` against
`--group-commit true`, same workload. `BENCHMARKS.md` has the numbers and
`results/summary.csv` the raw rows.

**Q. Why does the flusher not wait to build a bigger batch?**
I tested that, and it is worse. With a commit delay, batch size rose to a fixed 16 rows and
throughput fell monotonically as the delay grew. The reason is that submitters block until
their batch commits, so a delay puts the whole system in lock-step: everyone waits, one
batch commits, repeat. Throughput becomes batch size divided by delay. With no delay it
self-tunes, because whatever arrives during one transaction becomes the next batch. The knob
still exists because that is how I measured the decision.

**Q. What is the durability cost of group commit?**
A hard crash loses the state transitions buffered since the last commit, and those jobs are
re-executed on recovery. That sits inside the at-least-once contract the system already
documents. Submissions are never in that window: `insert_jobs` waits for its batch. It is a
deliberate trade of a narrow crash window for a large throughput gain, and both halves are
written down in `docs/DESIGN_DECISIONS.md`.

**Q. How do you know your latency numbers are not lying?**
Two ways. First, percentiles come from raw per-job CSV samples using nearest-rank, not from
the in-process histogram, because bucketed percentiles are estimates. Second, latency is
only reported from rate-limited runs. My first latency figures were nonsense - a p95 of
about 25 seconds for 5 ms jobs - because the driver submitted as fast as it could, so the
queue grew without bound and "latency" was really queue depth. The rate-limited mode gives
every job an intended submission time and measures from there, so a producer falling behind
still reports the delay it caused. Saturating runs are still published, labelled as
throughput measurements.

---

## D. Distributed systems

**Q. What exactly is distributed here?**
One coordinator process and N worker processes on one machine, communicating over HTTP.
The coordinator is not replicated and there is no consensus. I do not describe this as
fault tolerant: worker failure is handled, coordinator failure is an outage from which
state - but not availability - recovers.

**Q. Describe the worker protocol.**
A worker registers and gets an id and a heartbeat interval. It long-polls
`/internal/claim`; the coordinator pops from the queue, transitions the job to `RUNNING`,
increments the attempt, issues a random 64-bit lease token and a lease deadline, and
returns the work. The worker executes and calls `/internal/complete` with the job id,
attempt and token. The coordinator accepts the result only if the token, worker and attempt
all match the current lease. Heartbeats extend leases and carry back the list of jobs the
coordinator wants cancelled.

**Q. What happens when a worker dies?**
Heartbeats stop. After the timeout the watchdog marks the worker `DEAD` and expires its
leases immediately; independently, a lease can expire on its own deadline. The job returns
to `QUEUED` and another worker claims it with a new token and the next attempt number. If
the original worker comes back and reports, the token no longer matches and the result is
rejected as `stale_lease`. `g15_worker_failure.mjs` does this with a real `SIGKILL` and
reports the detection, requeue and recovery times.

**Q. So is that exactly-once?**
No, and I would not claim it. The job may have executed twice - once on the dead worker,
once on its replacement. What is exactly-once is the *result recording*: the fencing token
means only one attempt can write the job row, and `job_attempts` has a `(job_id, attempt)`
primary key. That is the classic fencing pattern: it bounds the damage of duplicate
execution, it does not prevent it. Preventing it needs the side effect and the completion
record in one transaction, which means the handler has to participate.

**Q. What stops an endlessly crashing worker from re-running a job forever?**
A lease expiry consumes an attempt. Once `attempt >= max_attempts`, the job becomes
`FAILED` with "lease expired and retry attempts exhausted" instead of being reassigned
again. There is a test for exactly that.

**Q. How would you remove the single point of failure?**
I would not hand-roll failover, because two coordinators both believing they are primary is
worse than one being down. The honest path is to move the queue and lease state into
something already replicated - PostgreSQL with `SELECT ... FOR UPDATE SKIP LOCKED` and
leases in rows, or etcd/Raft for leader election with the queue behind it - and make the
coordinator stateless enough that any instance can take over. That is a different project,
which is why I scoped it out rather than half-building it.

---

## E. Correctness and testing

**Q. How do you know no job is lost or executed twice?**
Three independent checks at different levels. The queue property test runs four producers
and four consumers and asserts exact set equality between pushed and popped ids. The engine
test does the same through the real executor with six worker threads and a recording
handler. And the gate goes through the HTTP API and verifies from the *database* - not the
engine's counters - that every submitted job has exactly one recorded attempt. Different
levels, different failure modes.

**Q. Which bug are you proudest of finding?**
A workflow-completion race. Completion was decided by re-reading every node and checking
whether all were terminal, so two threads finishing the last two nodes could both see "all
terminal" and both finalise the workflow - I caught it because a metric read 2 instead of 1.
The fix replaced the re-scan with a counter decremented under the workflow lock, so exactly
one thread can observe the transition to zero. What I like about it is that the symptom was
a counter being wrong, and the cause was a correctness bug in the completion rule.

**Q. Did the sanitizers find anything?**
ThreadSanitizer found one, and only because it slowed the process enough to widen a window:
the watchdog cleared a worker's `alive` flag under a lock but incremented the
`workers_marked_dead` counter outside it, so an observer could see the worker gone but not
yet counted. Not a data race on data, but the two facts disagreed. Moving the increment
inside the critical section fixed it. Otherwise both sanitizer runs are clean.

**Q. You have a static analysis warning you exclude. Defend that.**
The analyzer reports `recv` as being called inside a critical section, and the lock is
provably released first. Rather than assume it was a false positive, I wrote a probe fixture
with seven cases: the checker correctly stays quiet for `lock_guard`, a scoped
`unique_lock`, and an explicit `unlock()`, and correctly fires when a lock genuinely is held
across a blocking call - but it fires on any path that went through
`condition_variable::wait`, even when the lock is released. The gate runs that probe every
time, excludes only findings matching that signature, prints each excluded finding rather
than hiding it, and fails if the probe ever stops reproducing the behaviour. I also
restructured the code so the lock scope is obvious anyway.

**Q. What is weakest about your testing?**
No fuzzing - the parsers face a fixed corpus of malformed inputs, which is weaker than it
sounds. No multi-hour soak, so a slow leak or drift would not have been caught. No
multi-machine test. And LeakSanitizer is unavailable on this platform, so leak coverage
comes from the macOS `leaks` tool, which reports totals rather than allocation origins.

---

## F. C++ specifics

**Q. Which C++20 features did you actually use, and why?**
`std::jthread` and `stop_token` for cooperative cancellation, because that is exactly the
model the engine needs and hand-rolling a cancellation primitive would have been worse.
`std::atomic` wait/notify, `<semaphore>` and `<barrier>` are available and were verified to
compile; designated initialisers and `std::string_view` throughout for the parsing code.
I avoided anything with uneven library support so the project builds on an ordinary
toolchain.

**Q. How is cancellation actually delivered to a handler?**
Each running job has a `shared_ptr<atomic<bool>>` cancellation flag and a separate reason
code. The watchdog sets the reason then the flag; the handler polls it through
`JobContext::cancel_requested()` or `sleep_or_cancel()`, which sleeps in 5 ms slices so
cancellation latency is bounded by the slice rather than by the sleep duration. `shared_ptr`
matters here: the watchdog can set the flag without holding the engine lock and without any
risk of touching a destroyed object.

**Q. Why not `pthread_cancel` or a signal for timeouts?**
Because neither is safe in C++. Asynchronous cancellation at an arbitrary point leaves locks
held and destructors unrun, and `longjmp`-ing out of a handler past non-trivial destructors
is undefined behaviour. Cooperative cancellation is the same contract Go's `context` and
Java's interrupt flag use. The cost is that a handler ignoring the flag holds its thread, and
there is a test that demonstrates precisely that rather than leaving it as a footnote.

**Q. How do you avoid dangling references when a handler runs?**
The handler is a `std::function` copied out of the registry under a shared lock before
execution, not a reference into the map - the registry could be modified and the reference
would dangle. The job record is copied too. The only shared state during execution is the
cancellation flag, which is a `shared_ptr`.

**Q. What about the two clocks?**
Scheduling uses `steady_clock`, because a wall-clock step must not make a delayed job fire
early or late. Persistence and anything human-facing uses `system_clock`. Durations that
must survive a process boundary are stored as durations in microseconds, not as steady
timestamps, because a steady timestamp is meaningless in another process. The `Job` struct
names the fields `*_wall_ms` and `*_steady` so the distinction is visible at every use site.

---

## G. Questions where the answer is "no"

Worth rehearsing, because volunteering them reads as confidence rather than weakness.

| Question | Answer |
|---|---|
| Is it production-grade? | No. It has never run in production, has no authentication, no TLS, and one coordinator. It is a correct, measured implementation of a well-scoped system. |
| Does it scale horizontally? | Execution does, across worker processes, and I measured the curve. Scheduling and storage do not: one coordinator, one SQLite file. |
| Is it fault tolerant? | Worker failure is handled and tested. Coordinator failure is an outage. I would not use the phrase. |
| Exactly-once? | No. At-least-once execution, exactly-once result recording. |
| Does it run on Kubernetes / in Docker? | A `Dockerfile` is included but has never been built - Docker is not installed on my machine. I excluded it from every claim. |
| Have you run it across machines? | No. The protocol has no localhost assumption, but untested is untested. |
| Is the HTTP server safe on the open internet? | No. It implements a documented subset with explicit limits and is tested against malformed input, but it has not been fuzzed or hardened. Put it behind a proxy. |
| How big is it really? | Around five thousand lines of C++ plus tests. It is a well-tested medium-sized system, not a large one. |

---

## H. Three things to bring up unprompted

1. **The benchmark that disproved my own assumption.** Adding worker threads made
   throughput worse. Finding that, understanding why (four transactions per job behind one
   writer), fixing it with group commit and keeping the A/B reproducible from a single flag
   is the strongest engineering story in the project.
2. **The claim-verification pipeline.** The CV document is generated from a template whose
   numbers are `{{placeholders}}` filled from measured results, and a gate re-renders it and
   recomputes every value from the raw samples. It is impossible for a number in my CV to be
   one I typed rather than measured.
3. **The limits written down before anyone asks.** At-least-once, cooperative cancellation,
   a single coordinator, an unverified Dockerfile. Each one is in the README, the design
   decisions, and the CV document's "not safe to mention" table.
