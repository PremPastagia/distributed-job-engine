#!/usr/bin/env bash
# Runs the benchmark suites one at a time, each in its own short-lived Node process.
#
# Two reasons this exists rather than a single invocation:
#  * memory: each suite's process exits and releases everything before the next starts
#    (a single long run was once killed by the OS for memory pressure);
#  * durability: results/summary.csv is appended as each suite finishes, so an interrupted
#    run keeps everything it already measured.
set -u
cd "$(dirname "$0")/.."

SUITES="worker_scaling job_duration priority queue_capacity failure_heavy group_commit latency workflow http_api distributed"
rm -f results/summary.csv results/env.txt
rm -rf results/raw
mkdir -p results results/raw

first=1
failed=""
for suite in $SUITES; do
  echo ""
  echo "########## $suite ##########"
  if [ "$first" = "1" ]; then
    node scripts/run_benchmarks.mjs --only "$suite" "$@" || failed="$failed $suite"
    first=0
  else
    node scripts/run_benchmarks.mjs --only "$suite" --append "$@" || failed="$failed $suite"
  fi
done

echo ""
if [ -n "$failed" ]; then
  echo "SUITES THAT FAILED:$failed"
  exit 1
fi
echo "all suites completed; $(($(wc -l < results/summary.csv) - 1)) rows in results/summary.csv"
