# DESIGN DECISIONS

Each decision records the options considered, the choice, the reason, and the cost paid.
Decisions with a real downside say so; that downside is what an interviewer will probe.

---

## D-01 Language standard: **C++20**

| Option | Pros | Cons |
|---|---|---|
| C++17 | universally available | no `jthread`, no atomic wait/notify, no `<span>`, verbose `std::chrono` |
| **C++20** | `std::jthread` + `stop_token`, `atomic::wait/notify`, designated initialisers, `<barrier>`/`<semaphore>`, concepts | needs a recent toolchain |
| C++23 | `std::expected`, `print` | uneven library support on Apple clang / older GCC |

**Chosen: C++20.** Verified available on the build machine (Apple clang 21, all of
`<semaphore>`, `<barrier>`, `jthread`, atomic wait compile). `std::stop_token` is the
right model for cooperative cancellation, which this project needs anyway, so adopting it
avoids hand-rolling a cancellation primitive.
**Cost:** the project will not build on a pre-2021 toolchain.

---

## D-02 HTTP layer: **hand-written HTTP/1.1 subset over BSD sockets**

| Option | Pros | Cons |
|---|---|---|
| Drogon | fast, full framework, ORM | heavy dependency tree (jsoncpp, uuid, zlib, openssl), long build, most of it unused |
| Crow | header-only, ergonomic routing | pulls in standalone Asio; brings a large abstraction for a 10-endpoint API |
| Pistache | clean API | Linux-oriented; macOS support is second-class, and the dev/CI machine is macOS |
| cpp-httplib | single header, zero deps, well tested | the HTTP layer becomes a black box; still an external dependency to vendor; no client reuse advantage over writing one |
| **own implementation** | zero external deps, offline build, full control, the same socket code gives me the **worker→coordinator HTTP client** I need in Phase 5, and socket/parser correctness is exactly the systems skill this project is meant to show | I own the bugs; must be explicit about which subset of HTTP/1.1 is supported; not battle-tested against hostile input |

**Chosen: own implementation** (`src/http/`), roughly 600 lines: a `listen`/`accept` loop,
a bounded connection-thread pool, an incremental request-line/header parser,
`Content-Length` bodies, HTTP/1.1 keep-alive, and a matching blocking client.

**Connection model:** thread per connection, from a pool with a **floor** (`--http-threads`)
that grows on demand to a hard cap. A fixed pool was the original design and it was wrong:
worker processes long-poll `/internal/claim`, so a worker fleet as large as the pool
occupies every thread and the API becomes unreachable. That was found by the benchmark
suite, not by review - see `FAILURE_ANALYSIS.md` F-11.

**Explicitly supported subset** (documented, and the limits are enforced, not assumed):
request line + headers ≤ 16 KiB, `Content-Length` bodies ≤ 8 MiB, keep-alive with
`Connection: close` honoured, `SO_REUSEADDR`, `SIGPIPE` ignored, partial-read/`EINTR`
loops. **Not supported:** chunked request bodies (`411`), HTTP/2, TLS, pipelining beyond
sequential request handling, compression.
**Cost:** this is the single riskiest choice in the project. Mitigation: the parser is
unit-tested against malformed input independently of the socket layer, and the server is
exercised under a concurrent load generator.

---

## D-03 JSON: **hand-written minimal parser/serialiser**

| Option | Pros | Cons |
|---|---|---|
| nlohmann/json | ergonomic, ubiquitous | ~25k-line header, notable compile-time cost |
| RapidJSON | very fast | DOM API is verbose; another vendored dependency |
| **own** | ~400 lines, no dependency, exactly the subset the API needs, testable | must handle escapes/numbers correctly myself; not the fastest possible |

**Chosen: own.** The API surface is small and the shapes are known. Full RFC 8259 value
grammar is implemented (objects, arrays, strings with `\u` escapes and surrogate pairs,
numbers, `true`/`false`/`null`) with a nesting-depth limit to bound recursion on hostile
input. **Cost:** parsing is not optimised (naive recursive descent); the benchmark
reports API throughput including this cost rather than pretending it is free.

---

## D-04 Metadata store: **SQLite (system library, WAL mode)**

| Option | Pros | Cons |
|---|---|---|
| PostgreSQL | real concurrency, `SKIP LOCKED` for queues, network-accessible | needs a server process, credentials, and setup before anything runs; overkill for one coordinator |
| Redis | fast, has lists/sorted sets | not durable by default; a second daemon; would duplicate what the in-memory queue already does |
| flat file / append-only log | trivial | I would end up writing indexing, atomic update, and crash recovery — i.e. re-implementing a database |
| **SQLite** | embedded, zero setup, ACID transactions, WAL gives concurrent readers, already on every machine, trivially reproducible benchmarks | one writer at a time; not a good fit if the coordinator itself were replicated |

**Chosen: SQLite**, linked against the system `libsqlite3` (3.51 on the dev machine).
The coordinator is a single process, so SQLite's single-writer limit is not a constraint —
there is exactly one writer by construction.
**Redis was rejected** because the only thing it would add is an out-of-process queue, and
the queue is deliberately in-process (D-05).
**Cost:** the store does not scale horizontally; that is consistent with a single
coordinator and is stated in `ARCHITECTURE.md` §8.

---

## D-05 Queue: **in-memory heaps, SQLite as the durable record**

| Option | Pros | Cons |
|---|---|---|
| queue in SQLite (`SELECT ... LIMIT 1` + `UPDATE`) | survives restart with no rebuild; multiple processes could poll the same table | every dequeue is a write transaction; per-job disk I/O dominates; priority ordering needs an index scan per pop |
| Redis sorted set | fast, shared across processes | extra daemon, durability caveats |
| **in-memory heaps + SQLite record + rebuild on start** | dequeue is O(log n) with no I/O; blocking `wait_until` with no polling; the DB still holds the truth for restart | the ready order is only in the coordinator's memory, so a coordinator crash reconstructs order from the DB (`priority, seq`) rather than preserving it exactly |

**Chosen: in-memory.** Scheduling decisions happen at memory speed; durability is handled
by writing the job row *before* acknowledging the client and updating it at every state
transition. Restart rebuilds the queue by selecting non-terminal jobs ordered by
`(priority DESC, seq ASC)`, which reproduces the original ordering.
**Cost:** the queue is not shared memory between processes — remote workers must ask the
coordinator for work (`/internal/claim`) rather than reading a shared structure. That is a
deliberate trade: it keeps one authority for assignment and makes lease fencing possible.

---

## D-06 Durability setting: **`synchronous=NORMAL` in WAL mode**

`FULL` fsyncs on every commit; `NORMAL` fsyncs at checkpoints. In WAL mode, `NORMAL` can
lose the most recent transactions only on **OS/host crash or power loss**, not on process
crash — a `jobengine-server` crash still recovers every committed job.
**Chosen `NORMAL`**, because the failure this system is designed to survive is process
death, and because `FULL` would make submission throughput a measurement of the SSD's
fsync latency rather than of the engine. The setting is a command-line flag
(`--sqlite-sync`), and the benchmark records which setting produced each number.
**Cost:** on power loss, jobs committed in the last few hundred milliseconds can be lost.
Stated, not hidden.

---

## D-07 Timeout and cancellation: **cooperative, not preemptive**

There is no portable, safe way to kill a thread mid-execution in C++.
`pthread_cancel` at arbitrary points leaves locks held and destructors unrun; a
`SIGALRM`-based longjmp out of a handler is undefined behaviour in C++.

**Chosen model:**
- every handler receives a `JobContext` with `cancel_requested()` / `stop_token`;
- a watchdog thread sets the flag when `now > deadline_at` or a client cancels;
- the handler is expected to poll it at natural boundaries (the same contract as Go's
  `context.Context` or Java's interrupt flag);
- the engine records the attempt as `TIMED_OUT`/`CANCELLED` when the handler returns.

**A handler that ignores the flag occupies its worker thread until it returns.** That is
a real limitation, it is documented, and there is a test that demonstrates exactly this
behaviour (`TimeoutTest.UncooperativeHandlerHoldsThread`) so the claim is honest.
**Where a hard timeout *is* enforced:** across processes. A remote worker that hangs loses
its lease, and the coordinator reassigns the job regardless of what that worker is doing.

---

## D-08 Retry: **bounded exponential backoff, retry the job not the attempt**

`delay(n) = min(backoff_base_ms × multiplier^(n-1), backoff_max_ms)`, optional full jitter
(`U[0, delay]`), disabled by default so tests are deterministic.
Retries are **at the job level**: a retried job re-enters the ready queue and may be picked
up by a different worker, so a retry after a worker crash works naturally.
**Retry is only safe for idempotent handlers.** The engine cannot know whether a handler
is idempotent, so `max_attempts` defaults to **1** (no retry) and retry is opt-in per job.
This is the safe default; the alternative (retry by default) silently duplicates side
effects for non-idempotent work.

---

## D-09 Delivery semantics: **at-least-once execution, exactly-once result recording**

Claimed precisely:

- **Not exactly-once execution.** A worker can execute a job, die before reporting, and the
  job is then re-run. Any system without transactional side effects in the handler has this
  property.
- **At-least-once execution:** every non-cancelled job is executed until it reaches a
  terminal state or exhausts its attempts.
- **Exactly-once result recording:** each attempt carries a random 64-bit **lease token**.
  `/internal/complete` accepts a result only if the token matches the job's current lease
  and the lease has not expired. A late result from a superseded worker is rejected with
  `{"accepted":false,"reason":"stale_lease"}`. Therefore the `jobs` row is written by
  exactly one attempt, and `job_attempts` never gains two rows for the same `(job_id, attempt)`
  (enforced by the primary key).

This is the classic fencing-token pattern. It bounds the *damage* of duplicate execution;
it does not prevent duplicate execution.

---

## D-10 Test framework: **GoogleTest, pinned and fetched**

| Option | Pros | Cons |
|---|---|---|
| Catch2 v3 | single-header v2 was convenient; v3 is a real library anyway | slower compiles historically |
| doctest | very fast compiles | smaller ecosystem, fewer assertion/death-test facilities |
| **GoogleTest** | value/typed parameterised tests, `EXPECT_*` matchers, death tests, xml output for reporting, the framework most C++ teams actually use | larger dependency |

**Chosen: GoogleTest, pinned to v1.15.2.** CMake resolves it in two ways, in this order:

1. a vendored checkout at `third_party/googletest`, if one is present - this makes the build
   work with no network at all;
2. otherwise `FetchContent` with the pinned tag `v1.15.2`.

The vendored copy is **not committed**: it is 5.4 MB of third-party source whose only role
is to be compiled, and committing it would put someone else's repository inside this one.
The first build therefore needs network access; after that it is cached by CMake. To build
entirely offline, clone it once:

```bash
git clone --depth 1 --branch v1.15.2 https://github.com/google/googletest.git third_party/googletest
```

**Cost:** a fresh clone cannot run the tests without either network access or that one
command. The library build and all four executables need neither.

---

## D-11 Worker model: **one pool abstraction, two sources of work**

Rather than writing a separate remote-worker engine, `WorkerPool` consumes an abstract
`TaskSource`:
- `LocalTaskSource` — pops directly from the in-process `PriorityJobQueue` (Phases 1–4);
- `RemoteTaskSource` — long-polls `POST /internal/claim` and reports via `/internal/complete`
  (Phase 5).

The handler registry, timeout handling, retry accounting, and metrics are identical in both
modes, so distributed mode reuses code that the single-node tests already cover.
**Cost:** the abstraction adds a virtual call per job; negligible against any real handler,
and the benchmark measures the local path with the abstraction in place, so the reported
overhead is the real one.

---

## D-12 Docker: **Dockerfile provided, NOT verified**

Docker is not installed on this machine (`docker: command not found`). A `Dockerfile` and
`docker-compose.yml` are included because they are short and useful, but they have **never
been built or run**. They are therefore marked *unverified* in `REPRODUCIBILITY.md` and are
**excluded from every CV claim**. Building them is listed in `ROADMAP.md` as open work.

---

## D-13 Benchmark methodology: **raw samples to CSV, percentiles computed offline**

In-process histograms are approximations (bucket boundaries). For reported numbers the
benchmark writes **one row per job/request** with nanosecond timestamps to CSV, and Python
computes exact percentiles from the raw samples. The `/metrics` histogram exists for live
observability, but no CV number is taken from it.
Each run records: git commit, CPU model, core count, OS version, compiler version, build
type, and every parameter of the workload, into the CSV header and `results/env.txt`.

---

## D-14 Persistence batching: **group commit, after measuring the alternative**

The first implementation wrote each job state change with a bare `UPDATE`/`INSERT`. SQLite
runs every bare statement in its own implicit transaction, so a single job cost **four
commits** (insert, transition to `RUNNING`, attempt record, terminal update), and all of
them serialise behind SQLite's single writer. Measured consequence: throughput *fell* as
worker threads were added, because more threads only added contention.

| Option | Effect |
|---|---|
| leave it | throughput bounded by commit rate, and negatively correlated with thread count |
| one transaction per job | 4 commits become 1, but a submitter still pays a full commit each |
| **group commit** | a single flusher thread writes whatever has accumulated as ONE transaction; concurrent producers coalesce |
| drop durability entirely | fastest, but then "jobs survive a restart" would be false |

**Chosen: group commit**, with the durability boundary drawn deliberately:

- `insert_job` / `insert_jobs` **block** until the batch containing their row commits, so a
  client is acknowledged only after its job is durable. Concurrent submitters coalesce into
  one transaction instead of paying a commit each.
- `update_job` / `append_attempt` return immediately. Their rows go into a buffer and a
  **read-through cache** keyed by a monotonically increasing version, so a reader always
  sees the newest state, and a write that arrives while a transaction is open is not
  discarded (the cache entry is retired only when the exact version written is still the
  newest).
- Queries that must be exact across all jobs (listings, counts, recovery) flush first.

**Cost:** a hard crash loses buffered state transitions, and those jobs are re-executed on
recovery — which is inside the at-least-once contract of D-09. Submissions are never in that
window.

**No commit delay.** Adding a delay to grow batches was measured and made throughput
monotonically worse: submitters block until their batch commits, so a delay puts the system
in lock-step (batch size froze at 16 rows while throughput fell with every increase in the
delay). With no delay the batch size self-tunes, because whatever arrives during one
transaction becomes the next one. The `--commit-delay-us` knob is retained precisely because
it is how the decision was measured.

---

## D-15 Write buffer bound: **backpressure instead of unbounded growth**

Buffering transition writes raises an obvious question: what happens when the submit rate
exceeds the commit rate? Without an answer, the buffer and its read-through cache grow until
the process dies — which is not hypothetical; an early benchmark run on this machine was
killed by the OS for memory pressure.

`StoreConfig::max_buffered_rows` (default 20,000) is a high-water mark. Past it, a
transition write waits up to 50 ms for the flusher to catch up before enqueuing. That turns
unbounded memory growth into ordinary backpressure on the producer, and the bounded wait
means a stalled flusher degrades throughput rather than blocking the engine forever.

This sits alongside the engine's own admission control, which bounds the number of
*outstanding jobs*; together they bound both the in-memory job state and the write buffer.
