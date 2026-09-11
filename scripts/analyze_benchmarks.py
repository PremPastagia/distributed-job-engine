#!/usr/bin/env python3
"""Turn raw benchmark samples into the tables and plots used by BENCHMARKS.md.

    python3 scripts/analyze_benchmarks.py

Reads  results/summary.csv  (one row per run, written by scripts/run_benchmarks.mjs)
       results/raw/*.csv    (one row per job or per request)
Writes results/plots/*.png
       results/tables.md
and rewrites the generated section of BENCHMARKS.md in place.

Percentiles are computed with the nearest-rank method, matching apps/bench_common.hpp, so
an offline number and the driver's own printed number are the same statistic.
"""
import csv
import math
import os
import statistics
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "results")
RAW = os.path.join(RESULTS, "raw")
PLOTS = os.path.join(RESULTS, "plots")
SUMMARY = os.path.join(RESULTS, "summary.csv")
TABLES = os.path.join(RESULTS, "tables.md")
BENCHMARKS_MD = os.path.join(ROOT, "BENCHMARKS.md")

BEGIN = "<!-- BEGIN GENERATED: do not edit below this line, run scripts/analyze_benchmarks.py -->"
END = "<!-- END GENERATED -->"


def percentile(sorted_values, p):
    """Nearest-rank percentile, identical to the C++ implementation."""
    if not sorted_values:
        return 0.0
    idx = math.ceil((p / 100.0) * len(sorted_values))
    idx = max(1, min(idx, len(sorted_values)))
    return sorted_values[idx - 1]


def load_summary():
    if not os.path.exists(SUMMARY):
        sys.exit(f"missing {SUMMARY}; run: node scripts/run_benchmarks.mjs")
    with open(SUMMARY, newline="") as f:
        return list(csv.DictReader(f))


def load_raw(rel_path):
    path = os.path.join(ROOT, rel_path)
    if not os.path.exists(path):
        return []
    rows = []
    with open(path, newline="") as f:
        lines = [l for l in f if not l.startswith("#")]
    if not lines:
        return []
    for row in csv.DictReader(lines):
        rows.append(row)
    return rows


def num(x, default=None):
    try:
        if x is None or x == "":
            return default
        return float(x)
    except (TypeError, ValueError):
        return default


def group(rows, suite):
    """label -> list of rows, for one suite, preserving first-seen order."""
    out = defaultdict(list)
    for r in rows:
        if r["suite"] == suite:
            out[r["label"]].append(r)
    return out


def agg(values):
    """Median plus spread. Benchmarks vary run to run, so a single number would be a
    claim the next run could contradict."""
    vals = [v for v in values if v is not None]
    if not vals:
        return None, None, None
    return statistics.median(vals), min(vals), max(vals)


def fmt(v, digits=1):
    return "-" if v is None else f"{v:,.{digits}f}"


def table(headers, body_rows):
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join(["---"] * len(headers)) + "|"]
    for r in body_rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


# ---------------------------------------------------------------- tables

def table_worker_scaling(rows):
    g = group(rows, "worker_scaling")
    workloads = {}
    for label, rs in g.items():
        workload, w = label.rsplit("_w", 1)
        workloads.setdefault(workload, {})[int(w)] = rs
    sections = []
    for workload, by_workers in sorted(workloads.items()):
        body = []
        baseline = None
        for w in sorted(by_workers):
            rs = by_workers[w]
            thr, lo, hi = agg([num(r["throughput_per_s"]) for r in rs])
            p50, _, _ = agg([num(r["e2e_p50_ms"]) for r in rs])
            p95, _, _ = agg([num(r["e2e_p95_ms"]) for r in rs])
            if baseline is None and thr:
                baseline = thr
            speedup = thr / baseline if (thr and baseline) else None
            efficiency = (speedup / w * 100) if speedup else None
            body.append([w, fmt(thr), f"{fmt(lo)} - {fmt(hi)}", fmt(p50, 2), fmt(p95, 2),
                         fmt(speedup, 2) + "x" if speedup else "-",
                         fmt(efficiency, 0) + "%" if efficiency else "-"])
        sections.append(f"**Workload: `{workload}`**\n\n" + table(
            ["worker threads", "throughput (jobs/s, median)", "min - max", "e2e p50 (ms)",
             "e2e p95 (ms)", "speedup vs 1", "scaling efficiency"], body))
    return "\n\n".join(sections)


def table_job_duration(rows):
    g = group(rows, "job_duration")
    body = []
    for label in ["0ms_noop", "1ms", "5ms", "20ms", "cpu_50k", "cpu_1m"]:
        rs = g.get(label)
        if not rs:
            continue
        thr, lo, hi = agg([num(r["throughput_per_s"]) for r in rs])
        exec_p50, _, _ = agg([num(r["exec_p50_ms"]) for r in rs])
        p50, _, _ = agg([num(r["e2e_p50_ms"]) for r in rs])
        p95, _, _ = agg([num(r["e2e_p95_ms"]) for r in rs])
        p99, _, _ = agg([num(r["e2e_p99_ms"]) for r in rs])
        body.append([f"`{label}`", rs[0]["job_type"], fmt(exec_p50, 3), fmt(thr),
                     f"{fmt(lo)} - {fmt(hi)}", fmt(p50, 2), fmt(p95, 2), fmt(p99, 2)])
    return table(["workload", "type", "measured exec p50 (ms)", "throughput (jobs/s)",
                  "min - max", "e2e p50 (ms)", "e2e p95 (ms)", "e2e p99 (ms)"], body)


def table_priority(rows):
    """Per-priority queue wait, recomputed from the raw per-job samples."""
    g = group(rows, "priority")
    body = []
    for label, rs in sorted(g.items()):
        waits = defaultdict(list)
        for r in rs:
            for raw in load_raw(r["raw_csv"]):
                p = int(raw["priority"])
                waits[p].append(int(raw["queue_wait_us"]))
        for p in sorted(waits, reverse=True):
            s = sorted(waits[p])
            body.append([f"`{label}`", p, len(s),
                         fmt(percentile(s, 50) / 1000.0, 1),
                         fmt(percentile(s, 95) / 1000.0, 1),
                         fmt(percentile(s, 99) / 1000.0, 1)])
    return table(["priority mix", "priority", "jobs", "queue wait p50 (ms)",
                  "p95 (ms)", "p99 (ms)"], body)


def table_queue_capacity(rows):
    g = group(rows, "queue_capacity")
    body = []
    for label in sorted(g, key=lambda x: int(x)):
        rs = g[label]
        thr, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
        submitted, _, _ = agg([num(r["submitted"]) for r in rs])
        rejected, _, _ = agg([num(r["rejected"]) for r in rs])
        rate, _, _ = agg([num(r["error_rate"]) for r in rs])
        p95, _, _ = agg([num(r["e2e_p95_ms"]) for r in rs])
        body.append([label, fmt(submitted, 0), fmt(rejected, 0),
                     "-" if rate is None else f"{rate*100:.2f}%", fmt(thr), fmt(p95, 2)])
    return table(["max outstanding jobs", "accepted", "rejected (HTTP 503 equivalent)",
                  "rejection rate", "throughput (jobs/s)", "e2e p95 (ms)"], body)


def table_failure(rows):
    g = group(rows, "failure_heavy")
    body = []
    for label in ["fail0pct", "fail30pct", "fail70pct"]:
        rs = g.get(label)
        if not rs:
            continue
        thr, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
        succeeded, _, _ = agg([num(r["succeeded"]) for r in rs])
        failed, _, _ = agg([num(r["failed"]) for r in rs])
        completed, _, _ = agg([num(r["completed"]) for r in rs])
        attempts = []
        for r in rs:
            raw = load_raw(r["raw_csv"])
            if raw:
                attempts.append(statistics.mean(int(x["attempt"]) for x in raw))
        mean_attempts, _, _ = agg(attempts)
        p95, _, _ = agg([num(r["e2e_p95_ms"]) for r in rs])
        rate = (failed / completed) if (failed is not None and completed) else None
        body.append([f"`{label}`", fmt(completed, 0), fmt(succeeded, 0), fmt(failed, 0),
                     "-" if rate is None else f"{rate*100:.1f}%",
                     fmt(mean_attempts, 2), fmt(thr), fmt(p95, 2)])
    return table(["workload", "jobs", "succeeded", "dead-lettered", "dead-letter rate",
                  "mean attempts/job", "throughput (jobs/s)", "e2e p95 (ms)"], body)


def table_resources(rows):
    """CPU utilisation and peak memory, reported by getrusage inside each measured run."""
    body = []
    for suite, label in [("worker_scaling", "noop_w1"), ("worker_scaling", "noop_w4"),
                         ("worker_scaling", "noop_w8"), ("worker_scaling", "sleep5ms_w4"),
                         ("worker_scaling", "cpu_hash200k_w4"),
                         ("worker_scaling", "cpu_hash200k_w8"),
                         ("job_duration", "cpu_1m"), ("job_duration", "20ms"),
                         ("failure_heavy", "fail30pct"), ("group_commit", "false_w4"),
                         ("group_commit", "true_w4"), ("workflow", "fanout6_w4")]:
        rs = group(rows, suite).get(label)
        if not rs:
            continue
        cores, _, _ = agg([num(r["cores_used"]) for r in rs])
        rss, _, _ = agg([num(r["peak_rss_mb"]) for r in rs])
        cpu, _, _ = agg([num(r["cpu_seconds"]) for r in rs])
        thr, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
        per_job = None
        if cpu is not None and thr is not None:
            completed, _, _ = agg([num(r["completed"]) for r in rs])
            if completed:
                per_job = cpu / completed * 1e6  # microseconds of CPU per job
        body.append([f"`{suite}/{label}`", fmt(cores, 2), fmt(rss, 1), fmt(cpu, 2),
                     fmt(per_job, 1) if per_job is not None else "-"])
    return table(["run", "cores busy (mean)", "peak RSS (MB)", "CPU seconds",
                  "CPU microseconds per job"], body)


def table_latency(rows):
    """Latency against a controlled offered load, recomputed from raw samples."""
    g = group(rows, "latency")
    body = []
    def sort_key(label):
        wl, rate = label.rsplit("_", 1)
        return (wl, int(rate.replace("ps", "")))
    for label in sorted(g, key=sort_key):
        rs = g[label]
        workload, rate = label.rsplit("_", 1)
        achieved, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
        samples = []
        for r in rs:
            samples.extend(int(x["e2e_us"]) for x in load_raw(r["raw_csv"])
                           if num(x.get("e2e_us")) is not None and int(x["e2e_us"]) >= 0)
        samples.sort()
        queue = []
        for r in rs:
            queue.extend(int(x["queue_wait_us"]) for x in load_raw(r["raw_csv"]))
        queue.sort()
        body.append([f"`{workload}`", rate.replace("ps", ""), fmt(achieved), len(samples),
                     fmt(percentile(samples, 50) / 1000.0, 3),
                     fmt(percentile(samples, 95) / 1000.0, 3),
                     fmt(percentile(samples, 99) / 1000.0, 3),
                     fmt(percentile(queue, 95) / 1000.0, 3)])
    return table(["workload", "offered rate (jobs/s)", "achieved (jobs/s)", "samples",
                  "e2e p50 (ms)", "e2e p95 (ms)", "e2e p99 (ms)", "queue wait p95 (ms)"], body)


def table_group_commit(rows):
    g = group(rows, "group_commit")
    by_workers = defaultdict(dict)
    for label, rs in g.items():
        enabled, w = label.rsplit("_w", 1)
        by_workers[int(w)][enabled] = rs
    body = []
    for w in sorted(by_workers):
        off = by_workers[w].get("false", [])
        on = by_workers[w].get("true", [])
        thr_off, _, _ = agg([num(r["throughput_per_s"]) for r in off])
        thr_on, _, _ = agg([num(r["throughput_per_s"]) for r in on])
        rows_tx, _, _ = agg([num(r["rows_per_transaction"]) for r in on])
        writes, _, _ = agg([num(r["store_writes"]) for r in on])
        batches, _, _ = agg([num(r["store_batches"]) for r in on])
        speedup = thr_on / thr_off if (thr_on and thr_off) else None
        body.append([w, fmt(thr_off), fmt(thr_on),
                     fmt(speedup, 2) + "x" if speedup else "-",
                     fmt(rows_tx, 2), fmt(writes, 0), fmt(batches, 0)])
    return table(["worker threads", "throughput OFF (jobs/s)", "throughput ON (jobs/s)",
                  "improvement", "rows per transaction", "row writes", "transactions"], body)


def table_workflow(rows):
    g = group(rows, "workflow")
    body = []
    for label in sorted(g):
        rs = g[label]
        shape, w = label.rsplit("_w", 1)
        thr, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
        p50, _, _ = agg([num(r["e2e_p50_ms"]) for r in rs])
        p95, _, _ = agg([num(r["e2e_p95_ms"]) for r in rs])
        p99, _, _ = agg([num(r["e2e_p99_ms"]) for r in rs])
        completed, _, _ = agg([num(r["completed"]) for r in rs])
        body.append([f"`{shape}`", w, fmt(completed, 0), fmt(thr, 2),
                     fmt(p50, 1), fmt(p95, 1), fmt(p99, 1)])
    return table(["shape", "worker threads", "workflows", "workflows/s",
                  "end-to-end p50 (ms)", "p95 (ms)", "p99 (ms)"], body)


def table_http(rows):
    """Recomputes percentiles from the raw per-request samples."""
    g = group(rows, "http_api")
    body = []
    for label in sorted(g, key=lambda l: (l.split("_c")[0], int(l.split("_c")[1]))):
        rs = g[label]
        mode, c = label.rsplit("_c", 1)
        thr, lo, hi = agg([num(r["throughput_per_s"]) for r in rs])
        err, _, _ = agg([num(r["error_rate"]) for r in rs])
        samples = []
        for r in rs:
            samples.extend(int(x["latency_us"]) for x in load_raw(r["raw_csv"]))
        samples.sort()
        body.append([f"`{mode}`", c, len(samples), fmt(thr), f"{fmt(lo)} - {fmt(hi)}",
                     fmt(percentile(samples, 50) / 1000.0, 3),
                     fmt(percentile(samples, 95) / 1000.0, 3),
                     fmt(percentile(samples, 99) / 1000.0, 3),
                     "-" if err is None else f"{err*100:.3f}%"])
    return table(["endpoint mix", "connections", "requests measured", "throughput (req/s)",
                  "min - max", "p50 (ms)", "p95 (ms)", "p99 (ms)", "error rate"], body)


def table_distributed(rows):
    g = group(rows, "distributed")
    body = []
    baseline = None
    for label in sorted(g, key=lambda l: int(l.replace("proc", ""))):
        rs = g[label]
        procs = int(label.replace("proc", ""))
        thr, lo, hi = agg([num(r["throughput_per_s"]) for r in rs])
        succeeded, _, _ = agg([num(r["succeeded"]) for r in rs])
        if baseline is None and thr:
            baseline = thr
        speedup = thr / baseline if (thr and baseline) else None
        eff = (speedup / procs * 100) if speedup else None
        body.append([procs, procs * 2, fmt(succeeded, 0), fmt(thr),
                     f"{fmt(lo)} - {fmt(hi)}",
                     fmt(speedup, 2) + "x" if speedup else "-",
                     fmt(eff, 0) + "%" if eff else "-"])
    return table(["worker processes", "total worker threads", "jobs completed",
                  "throughput (jobs/s)", "min - max", "speedup vs 1 process",
                  "scaling efficiency"], body)


# ---------------------------------------------------------------- plots

def make_plots(rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib is not installed; skipping plots")
        return []

    os.makedirs(PLOTS, exist_ok=True)
    written = []

    def save(fig, name):
        path = os.path.join(PLOTS, name)
        fig.tight_layout()
        fig.savefig(path, dpi=130)
        plt.close(fig)
        written.append(os.path.relpath(path, ROOT))

    # 1. worker scaling
    g = group(rows, "worker_scaling")
    workloads = defaultdict(dict)
    for label, rs in g.items():
        workload, w = label.rsplit("_w", 1)
        thr, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
        workloads[workload][int(w)] = thr
    if workloads:
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.2))
        for workload, data in sorted(workloads.items()):
            xs = sorted(data)
            ys = [data[x] for x in xs]
            ax1.plot(xs, ys, marker="o", label=workload)
            base = ys[0] if ys and ys[0] else 1
            ax2.plot(xs, [(y / base) / x * 100 if y else 0 for x, y in zip(xs, ys)],
                     marker="o", label=workload)
        ax1.set_xlabel("worker threads"); ax1.set_ylabel("jobs / second")
        ax1.set_title("Throughput vs worker threads"); ax1.legend(); ax1.grid(alpha=0.3)
        ax2.axhline(100, color="grey", linestyle="--", linewidth=1)
        ax2.set_xlabel("worker threads"); ax2.set_ylabel("scaling efficiency (%)")
        ax2.set_title("Scaling efficiency vs 1 thread"); ax2.legend(); ax2.grid(alpha=0.3)
        save(fig, "worker_scaling.png")

    # 2. group commit A/B
    g = group(rows, "group_commit")
    ab = defaultdict(dict)
    for label, rs in g.items():
        enabled, w = label.rsplit("_w", 1)
        thr, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
        ab[enabled][int(w)] = thr
    if ab:
        fig, ax = plt.subplots(figsize=(6.5, 4.2))
        width = 0.35
        xs = sorted(set(k for v in ab.values() for k in v))
        for i, (enabled, data) in enumerate(sorted(ab.items())):
            offs = [x + (i - 0.5) * width for x in range(len(xs))]
            ax.bar(offs, [data.get(x) or 0 for x in xs], width,
                   label=f"group commit {'on' if enabled == 'true' else 'off'}")
        ax.set_xticks(range(len(xs))); ax.set_xticklabels(xs)
        ax.set_xlabel("worker threads"); ax.set_ylabel("jobs / second")
        ax.set_title("Effect of group commit on throughput"); ax.legend(); ax.grid(alpha=0.3, axis="y")
        save(fig, "group_commit.png")

    # 3. distributed scaling
    g = group(rows, "distributed")
    if g:
        xs, ys = [], []
        for label, rs in sorted(g.items(), key=lambda kv: int(kv[0].replace("proc", ""))):
            thr, _, _ = agg([num(r["throughput_per_s"]) for r in rs])
            xs.append(int(label.replace("proc", "")))
            ys.append(thr or 0)
        fig, ax = plt.subplots(figsize=(6.5, 4.2))
        ax.plot(xs, ys, marker="o", label="measured")
        if ys and ys[0]:
            ax.plot(xs, [ys[0] * x for x in xs], linestyle="--", color="grey", label="linear")
        ax.set_xlabel("worker processes (2 threads each)"); ax.set_ylabel("jobs / second")
        ax.set_title("Distributed throughput vs worker processes")
        ax.legend(); ax.grid(alpha=0.3)
        save(fig, "distributed_scaling.png")

    # 4. HTTP latency percentiles
    g = group(rows, "http_api")
    if g:
        modes = defaultdict(dict)
        for label, rs in g.items():
            mode, c = label.rsplit("_c", 1)
            samples = []
            for r in rs:
                samples.extend(int(x["latency_us"]) for x in load_raw(r["raw_csv"]))
            samples.sort()
            modes[mode][int(c)] = samples
        fig, axes = plt.subplots(1, len(modes), figsize=(4.2 * len(modes), 4.0), squeeze=False)
        for ax, (mode, data) in zip(axes[0], sorted(modes.items())):
            xs = sorted(data)
            for p in (50, 95, 99):
                ax.plot(xs, [percentile(data[x], p) / 1000.0 for x in xs],
                        marker="o", label=f"p{p}")
            ax.set_title(f"{mode}"); ax.set_xlabel("connections")
            ax.set_ylabel("latency (ms)"); ax.set_xscale("log", base=2)
            ax.legend(); ax.grid(alpha=0.3)
        fig.suptitle("HTTP API latency percentiles")
        save(fig, "http_latency.png")

    # 5. priority queue wait distribution
    g = group(rows, "priority")
    mixed = g.get("0:50,9:50")
    if mixed:
        waits = defaultdict(list)
        for r in mixed:
            for raw in load_raw(r["raw_csv"]):
                waits[int(raw["priority"])].append(int(raw["queue_wait_us"]) / 1000.0)
        if waits:
            fig, ax = plt.subplots(figsize=(6.5, 4.2))
            for p in sorted(waits, reverse=True):
                s = sorted(waits[p])
                ax.plot(s, [i / len(s) * 100 for i in range(len(s))],
                        label=f"priority {p} (n={len(s)})")
            ax.set_xlabel("queue wait (ms)"); ax.set_ylabel("percentile")
            ax.set_title("Queue wait by priority, 50/50 mix, 2 workers")
            ax.legend(); ax.grid(alpha=0.3)
            save(fig, "priority_queue_wait.png")

    # 5b. latency vs offered load
    g = group(rows, "latency")
    if g:
        curves = defaultdict(dict)
        for label, rs in g.items():
            workload, rate = label.rsplit("_", 1)
            samples = []
            for r in rs:
                samples.extend(int(x["e2e_us"]) for x in load_raw(r["raw_csv"])
                               if x.get("e2e_us") and int(x["e2e_us"]) >= 0)
            samples.sort()
            curves[workload][int(rate.replace("ps", ""))] = samples
        fig, axes = plt.subplots(1, len(curves), figsize=(5.2 * len(curves), 4.0), squeeze=False)
        for ax, (workload, data) in zip(axes[0], sorted(curves.items())):
            xs = sorted(data)
            for p in (50, 95, 99):
                ax.plot(xs, [percentile(data[x], p) / 1000.0 for x in xs],
                        marker="o", label=f"p{p}")
            ax.set_title(workload); ax.set_xlabel("offered load (jobs/s)")
            ax.set_ylabel("end-to-end latency (ms)"); ax.set_yscale("log")
            ax.legend(); ax.grid(alpha=0.3)
        fig.suptitle("Latency vs offered load (4 worker threads, rate-limited)")
        save(fig, "latency_vs_load.png")

    # 6. workflow latency by shape
    g = group(rows, "workflow")
    if g:
        shapes = defaultdict(dict)
        for label, rs in g.items():
            shape, w = label.rsplit("_w", 1)
            p50, _, _ = agg([num(r["e2e_p50_ms"]) for r in rs])
            shapes[shape][int(w)] = p50
        fig, ax = plt.subplots(figsize=(6.5, 4.2))
        for shape, data in sorted(shapes.items()):
            xs = sorted(data)
            ax.plot(xs, [data[x] or 0 for x in xs], marker="o", label=shape)
        ax.set_xlabel("worker threads"); ax.set_ylabel("workflow p50 latency (ms)")
        ax.set_title("Workflow completion latency by shape"); ax.legend(); ax.grid(alpha=0.3)
        save(fig, "workflow_latency.png")

    # 7. job duration: throughput and tail latency
    g = group(rows, "job_duration")
    if g:
        order = ["0ms_noop", "1ms", "5ms", "20ms", "cpu_50k", "cpu_1m"]
        labels = [l for l in order if l in g]
        thr = [agg([num(r["throughput_per_s"]) for r in g[l]])[0] or 0 for l in labels]
        p95 = [agg([num(r["e2e_p95_ms"]) for r in g[l]])[0] or 0 for l in labels]
        fig, ax1 = plt.subplots(figsize=(7.5, 4.2))
        ax1.bar(range(len(labels)), thr, color="tab:blue", alpha=0.75)
        ax1.set_xticks(range(len(labels))); ax1.set_xticklabels(labels, rotation=20)
        ax1.set_ylabel("jobs / second", color="tab:blue")
        ax2 = ax1.twinx()
        ax2.plot(range(len(labels)), p95, color="tab:red", marker="o")
        ax2.set_ylabel("e2e p95 (ms)", color="tab:red"); ax2.set_yscale("log")
        ax1.set_title("Throughput and tail latency by job duration (4 workers)")
        save(fig, "job_duration.png")

    return written


# ---------------------------------------------------------------- main

def main():
    rows = load_summary()
    env = ""
    env_path = os.path.join(RESULTS, "env.txt")
    if os.path.exists(env_path):
        with open(env_path) as f:
            env = f.read().strip()

    plots = make_plots(rows)

    sections = [
        ("Environment", "```\n" + env + "\n```"),
        ("1. Worker-thread scaling (single process)", table_worker_scaling(rows)),
        ("2. Job duration", table_job_duration(rows)),
        ("3. Priority scheduling", table_priority(rows)),
        ("4. Queue capacity and backpressure", table_queue_capacity(rows)),
        ("5. Failure-heavy workloads", table_failure(rows)),
        ("6. Latency under controlled load", table_latency(rows)),
        ("6b. CPU and memory", table_resources(rows)),
        ("7. Group commit (persistence batching)", table_group_commit(rows)),
        ("8. Workflow workloads", table_workflow(rows)),
        ("9. HTTP API", table_http(rows)),
        ("10. Distributed execution (separate worker processes)", table_distributed(rows)),
    ]

    body = []
    for title, content in sections:
        body.append(f"### {title}\n\n{content}\n")
    if plots:
        body.append("### Plots\n\n" + "\n".join(f"![{os.path.basename(p)}]({p})" for p in plots) + "\n")

    generated = "\n".join(body)
    with open(TABLES, "w") as f:
        f.write(generated)

    if os.path.exists(BENCHMARKS_MD):
        with open(BENCHMARKS_MD) as f:
            doc = f.read()
        if BEGIN in doc and END in doc:
            head = doc.split(BEGIN)[0]
            tail = doc.split(END)[1]
            doc = head + BEGIN + "\n\n" + generated + "\n" + END + tail
            with open(BENCHMARKS_MD, "w") as f:
                f.write(doc)
            print(f"updated {os.path.relpath(BENCHMARKS_MD, ROOT)}")
        else:
            print(f"WARNING: {BENCHMARKS_MD} has no generated-section markers; wrote tables only")

    print(f"wrote {os.path.relpath(TABLES, ROOT)} and {len(plots)} plot(s)")
    for p in plots:
        print(f"  {p}")


if __name__ == "__main__":
    main()
