# REPRODUCIBILITY

One command per claim. Nothing in this repository asks you to take a number on trust.

---

## 0. Prerequisites

| Requirement | Version used here | Notes |
|---|---|---|
| C++ compiler | Apple clang 21 (any C++20 compiler) | `std::jthread`, `<semaphore>`, `<barrier>`, atomic wait must be available |
| CMake | 4.4.3 (>= 3.16 required) | `pip install cmake` works if your package manager has none |
| SQLite3 dev headers | 3.51 (system) | macOS: Xcode command line tools. Debian/Ubuntu: `libsqlite3-dev` |
| Node.js | 24 | runs the benchmark orchestration and the acceptance gates |
| Python 3 + matplotlib | 3.13 / 3.10 | analysis and plots only |
| GoogleTest | v1.15.2, pinned | fetched by CMake on first configure; `git clone --depth 1 --branch v1.15.2 https://github.com/google/googletest.git third_party/googletest` makes the test build offline too. Only the tests need it. |

Docker is **not** required and was **not** available on the development machine; see
"Unverified" at the bottom.

## 1. Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Produces `jobengine-server`, `jobengine-worker`, `jobengine-bench`, `jobengine-load` and
`jobengine_tests` in `build/`.

## 2. Everything at once

```bash
# correctness: 20 acceptance gates, each printing GATE GNN PASS or failing loudly
for g in scripts/gates/g*.mjs; do
  echo "--- $g"; node "$g" || echo "FAILED: $g";
done

# performance: full matrix, analysis, metric extraction, document rendering
node scripts/run_benchmarks.mjs
python3 scripts/analyze_benchmarks.py
node scripts/extract_metrics.mjs
node scripts/render_docs.mjs
```

Expect roughly 25-45 minutes for the benchmark suite on a comparable machine, and a few
minutes for the gates (the sanitizer gates rebuild the project).

## 3. One command per claim

| Claim | Command | What proves it |
|---|---|---|
| Compiles clean in Release and Debug with `-Werror` | `node scripts/gates/g01_build.mjs` | both builds configured from scratch; warnings in project sources counted |
| The whole test suite passes | `node scripts/gates/g02_tests.mjs` | counts read from the GoogleTest runner, not asserted by hand |
| No data races | `node scripts/gates/g03_tsan.mjs` | checks the binary is actually TSan-linked, then counts warnings |
| No memory errors or leaks | `node scripts/gates/g04_asan.mjs` | ASan + UBSan, then the platform leak checker |
| No job lost or executed twice | `node scripts/gates/g05_no_loss.mjs` | 600 jobs over HTTP verified from the database; 5,000 through the driver |
| Priority scheduling works and costs something | `node scripts/gates/g06_priority.mjs` | per-priority queue wait computed from raw samples |
| Graceful shutdown | `node scripts/gates/g07_shutdown.mjs` | `SIGTERM` with work in flight; database read back afterwards |
| REST API contract | `node scripts/gates/g08_api.mjs` | 61 assertions against a live server process |
| Restart recovery | `node scripts/gates/g09_restart.mjs` | coordinator `SIGKILL`ed, restarted, jobs completed |
| Retry count and backoff timing | `node scripts/gates/g10_retry.mjs` | attempt timestamps compared to the formula |
| Deadlines, and their cooperative limit | `node scripts/gates/g11_timeout.mjs` | both the enforcement and the documented gap |
| Cancellation and its race | `node scripts/gates/g12_cancel.mjs` | 150 submit-then-cancel races |
| Workflow DAG behaviour | `node scripts/gates/g13_workflow.mjs` | order from timestamps, parallelism vs critical path |
| Distributed execution | `node scripts/gates/g14_distributed.mjs` | 3 worker processes, per-process execution counts |
| Worker failure recovery and fencing | `node scripts/gates/g15_worker_failure.mjs` | `SIGKILL`, reassignment, stale-result rejection |
| Benchmark artifacts exist and regenerate | `node scripts/gates/g16_bench_artifacts.mjs` | reruns a suite slice, checks every CSV, plot and header |
| Every CV number traces to data | `node scripts/gates/g17_claims.mjs` | recomputes metrics from raw samples, re-renders the document |
| Docs match the implementation | `node scripts/gates/g18_api_drift.mjs` | `/routes` compared with `docs/ARCHITECTURE.md` |
| Documents are complete and honest | `node scripts/gates/g19_docs.mjs` | placeholders, over-claims and broken links |
| Static analysis clean | `node scripts/gates/g20_static.mjs` | positive control, probe fixture, per-file analysis |

## 4. Reproducing a specific benchmark

```bash
# worker-thread scaling (the throughput numbers)
node scripts/run_benchmarks.mjs --only worker_scaling

# the group-commit A/B: same binary, same workload, one flag
./build/jobengine-bench --workers 4 --jobs 20000 --submitters 4 --type noop --group-commit false
./build/jobengine-bench --workers 4 --jobs 20000 --submitters 4 --type noop --group-commit true

# latency under a controlled offered load (not a saturating run)
./build/jobengine-bench --workers 4 --jobs 5000 --type sleep --payload '{"ms":5}' --rate 500

# REST API latency
./build/jobengine-server --port 8080 --workers 8 --db :memory: &
./build/jobengine-load --port 8080 --connections 8 --requests 8000 --mode submit --out /tmp/load.csv

# multi-process scaling
node scripts/run_benchmarks.mjs --only distributed
```

Every driver prints its own summary line and, with `--out`, writes one CSV row per job or
request with an environment header.

## 5. Where each artifact comes from

| Artifact | Produced by | Consumed by |
|---|---|---|
| `results/summary.csv` | `scripts/run_benchmarks.mjs` | analysis, metric extraction, `g16`, `g17` |
| `results/raw/*.csv` | the C++ drivers via `--out` | percentile computation in analysis and `g17` |
| | *(not tracked in git: ~1 MB per run, >100 MB per suite - regenerate with `node scripts/run_benchmarks.mjs`)* | |
| `results/env.txt` | `scripts/run_benchmarks.mjs` | `BENCHMARKS.md`, `g16` |
| `results/plots/*.png` | `scripts/analyze_benchmarks.py` | `BENCHMARKS.md` |
| `results/tables.md` | `scripts/analyze_benchmarks.py` | injected into `BENCHMARKS.md` |
| `results/verified_metrics.json` | `scripts/extract_metrics.mjs` | `scripts/render_docs.mjs`, `g17` |
| `CV_POINTERS_*.md` | `scripts/render_docs.mjs` from `docs/cv_pointers.template.md` | `g17` re-renders and compares |

The CV document is **generated**. A number cannot be typed into it: the template contains
`{{metric_key}}` placeholders, rendering fails if a key was never measured, and `g17`
re-renders the document and recomputes the values from the raw samples.

## 6. Determinism and variation

- **Correctness gates are deterministic.** They assert states and invariants, not timings,
  except where a timing *is* the claim (backoff gaps, deadline enforcement, failure
  detection), and those use bounds wide enough for scheduling jitter but tight enough to
  fail if the behaviour regressed.
- **Benchmarks are not deterministic.** Run them on an idle machine. During development the
  same configuration differed by more than 2x when a compile was running alongside. The
  suite records the environment, repeats each configuration 3 times, and reports medians
  with the min-max range.
- **On a fresh clone, run the benchmark suite before `g17_claims.mjs`.** The raw per-sample
  CSVs are not tracked (over 100 MB for a full suite), and that gate recomputes its
  percentiles from them. Everything derived from them - `summary.csv`, `env.txt`,
  `verified_metrics.json` and the plots - *is* tracked, so the published numbers and their
  provenance travel with the repository.
- **Re-running benchmarks changes the numbers.** After a re-run, regenerate the derived
  documents: `python3 scripts/analyze_benchmarks.py && node scripts/extract_metrics.mjs &&
  node scripts/render_docs.mjs`. `g17` fails if you forget.

## 7. Unverified

- **`Dockerfile` and `docker-compose.yml` have never been built or run.** Docker is not
  installed on the development machine. They are provided for convenience and are excluded
  from every claim in `CV_POINTERS_DISTRIBUTED_JOB_ENGINE.md`.
- **Multi-machine operation is untested.** The worker speaks plain TCP to a host and port
  with no localhost assumption, so it should work, but "should" is not a measurement.
- **LeakSanitizer is unavailable on macOS/arm64.** Leak checking uses the platform `leaks`
  tool instead, which reports totals rather than per-allocation origins.
