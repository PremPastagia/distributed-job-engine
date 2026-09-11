| metric | value | unit | evidence file | how it was computed |
|---|---|---|---|---|
| `backpressure_cap10000_rejection_rate` | 0 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=10000 under a saturating submit rate |
| `backpressure_cap1000_rejection_rate` | 86.2 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=1000 under a saturating submit rate |
| `backpressure_cap100_rejection_rate` | 97.3 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=100 under a saturating submit rate |
| `backpressure_cap200000_rejection_rate` | 0 | % | `results/summary.csv` | rejected submissions divided by attempted, with max_outstanding=200000 under a saturating submit rate |
| `distributed_ceiling_utilisation_1_processes` | 61.4 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 1000 jobs/s (2 slots x one 2ms job each) |
| `distributed_ceiling_utilisation_2_processes` | 68.9 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 2000 jobs/s (4 slots x one 2ms job each) |
| `distributed_ceiling_utilisation_4_processes` | 68.2 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 4000 jobs/s (8 slots x one 2ms job each) |
| `distributed_ceiling_utilisation_8_processes` | 71.2 | % | `results/summary.csv` | throughput divided by the theoretical ceiling of 8000 jobs/s (16 slots x one 2ms job each) |
| `distributed_speedup_2_processes` | 2.24 | x | `results/summary.csv` | throughput divided by the single-process throughput |
| `distributed_speedup_4_processes` | 4.44 | x | `results/summary.csv` | throughput divided by the single-process throughput |
| `distributed_speedup_8_processes` | 9.27 | x | `results/summary.csv` | throughput divided by the single-process throughput |
| `distributed_throughput_1_processes` | 614 | jobs/s | `results/summary.csv` | median over runs with 1 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `distributed_throughput_2_processes` | 1378 | jobs/s | `results/summary.csv` | median over runs with 2 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `distributed_throughput_4_processes` | 2727 | jobs/s | `results/summary.csv` | median over runs with 4 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `distributed_throughput_8_processes` | 5693 | jobs/s | `results/summary.csv` | median over runs with 8 worker processes of 2 threads each; the coordinator runs 0 embedded workers, so every job was executed by a separate process |
| `failure_fail0pct_dead_letter_rate` | 0 | % | `results/summary.csv` | jobs that exhausted their retries, divided by jobs completed |
| `failure_fail0pct_throughput` | 53406 | jobs/s | `results/summary.csv` | median throughput with retries enabled (max_attempts=3) |
| `failure_fail30pct_dead_letter_rate` | 2.5 | % | `results/summary.csv` | jobs that exhausted their retries, divided by jobs completed |
| `failure_fail30pct_throughput` | 10550 | jobs/s | `results/summary.csv` | median throughput with retries enabled (max_attempts=3) |
| `failure_fail70pct_dead_letter_rate` | 34.5 | % | `results/summary.csv` | jobs that exhausted their retries, divided by jobs completed |
| `failure_fail70pct_throughput` | 10537 | jobs/s | `results/summary.csv` | median throughput with retries enabled (max_attempts=3) |
| `group_commit_off_throughput_1_threads` | 59440 | jobs/s | `results/summary.csv` | median throughput with --group-commit false |
| `group_commit_off_throughput_4_threads` | 40860 | jobs/s | `results/summary.csv` | median throughput with --group-commit false |
| `group_commit_off_throughput_8_threads` | 41172 | jobs/s | `results/summary.csv` | median throughput with --group-commit false |
| `group_commit_on_throughput_1_threads` | 62772 | jobs/s | `results/summary.csv` | median throughput with --group-commit true |
| `group_commit_on_throughput_4_threads` | 53286 | jobs/s | `results/summary.csv` | median throughput with --group-commit true |
| `group_commit_on_throughput_8_threads` | 51781 | jobs/s | `results/summary.csv` | median throughput with --group-commit true |
| `group_commit_rows_per_transaction_4_threads` | 6.73 | rows | `results/summary.csv` | row writes divided by committed transactions, reported by the store |
| `group_commit_speedup_1_threads` | 1.06 | x | `results/summary.csv` | on divided by off, same binary and workload, only the flag changed |
| `group_commit_speedup_4_threads` | 1.3 | x | `results/summary.csv` | on divided by off, same binary and workload, only the flag changed |
| `group_commit_speedup_8_threads` | 1.26 | x | `results/summary.csv` | on divided by off, same binary and workload, only the flag changed |
| `http_health_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_health_c1_p50_ms` | 0.02 | ms | `results/raw/http_health_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_health_c1_p95_ms` | 0.023 | ms | `results/raw/http_health_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_health_c1_p99_ms` | 0.025 | ms | `results/raw/http_health_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_health_c1_throughput` | 49724 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_health_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_health_c32_p50_ms` | 0.166 | ms | `results/raw/http_health_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_health_c32_p95_ms` | 0.25 | ms | `results/raw/http_health_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_health_c32_p99_ms` | 0.358 | ms | `results/raw/http_health_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_health_c32_throughput` | 183415 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_health_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_health_c8_p50_ms` | 0.05 | ms | `results/raw/http_health_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_health_c8_p95_ms` | 0.075 | ms | `results/raw/http_health_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_health_c8_p99_ms` | 0.087 | ms | `results/raw/http_health_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_health_c8_throughput` | 152140 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_mixed_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_mixed_c1_p50_ms` | 0.031 | ms | `results/raw/http_mixed_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_mixed_c1_p95_ms` | 0.039 | ms | `results/raw/http_mixed_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_mixed_c1_p99_ms` | 0.044 | ms | `results/raw/http_mixed_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_mixed_c1_throughput` | 32762 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_mixed_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_mixed_c32_p50_ms` | 0.375 | ms | `results/raw/http_mixed_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_mixed_c32_p95_ms` | 0.989 | ms | `results/raw/http_mixed_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_mixed_c32_p99_ms` | 1.28 | ms | `results/raw/http_mixed_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_mixed_c32_throughput` | 73315 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_mixed_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_mixed_c8_p50_ms` | 0.142 | ms | `results/raw/http_mixed_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_mixed_c8_p95_ms` | 0.335 | ms | `results/raw/http_mixed_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_mixed_c8_p99_ms` | 0.415 | ms | `results/raw/http_mixed_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_mixed_c8_throughput` | 49751 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_routes` | 16 | routes | `src/api/api.cpp` | counted from the kRoutes table, which is also what GET /routes returns |
| `http_status_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_status_c1_p50_ms` | 0.023 | ms | `results/raw/http_status_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_status_c1_p95_ms` | 0.027 | ms | `results/raw/http_status_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_status_c1_p99_ms` | 0.029 | ms | `results/raw/http_status_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_status_c1_throughput` | 42246 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_status_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_status_c32_p50_ms` | 0.178 | ms | `results/raw/http_status_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_status_c32_p95_ms` | 0.45 | ms | `results/raw/http_status_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_status_c32_p99_ms` | 0.647 | ms | `results/raw/http_status_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_status_c32_throughput` | 137400 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_status_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_status_c8_p50_ms` | 0.063 | ms | `results/raw/http_status_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_status_c8_p95_ms` | 0.106 | ms | `results/raw/http_status_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_status_c8_p99_ms` | 0.135 | ms | `results/raw/http_status_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_status_c8_throughput` | 120906 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_submit_c1_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_submit_c1_p50_ms` | 0.036 | ms | `results/raw/http_submit_c1_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_submit_c1_p95_ms` | 0.044 | ms | `results/raw/http_submit_c1_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_submit_c1_p99_ms` | 0.081 | ms | `results/raw/http_submit_c1_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_submit_c1_throughput` | 27202 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_submit_c32_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_submit_c32_p50_ms` | 0.508 | ms | `results/raw/http_submit_c32_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_submit_c32_p95_ms` | 0.841 | ms | `results/raw/http_submit_c32_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_submit_c32_p99_ms` | 1.03 | ms | `results/raw/http_submit_c32_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_submit_c32_throughput` | 54029 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `http_submit_c8_error_rate` | 0 | % | `results/summary.csv` | non-2xx responses divided by requests issued |
| `http_submit_c8_p50_ms` | 0.212 | ms | `results/raw/http_submit_c8_r*.csv` | nearest-rank p50 over 24000 raw per-request latencies |
| `http_submit_c8_p95_ms` | 0.355 | ms | `results/raw/http_submit_c8_r*.csv` | nearest-rank p95 over 24000 raw per-request latencies |
| `http_submit_c8_p99_ms` | 0.457 | ms | `results/raw/http_submit_c8_r*.csv` | nearest-rank p99 over 24000 raw per-request latencies |
| `http_submit_c8_throughput` | 35597 | req/s | `results/summary.csv` | median of the load generator's reported throughput |
| `job_states` | 10 | states | `include/jobengine/job.hpp` | counted from the JobState enumerators |
| `latency_noop_1000ps_achieved` | 1000 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_1000ps_p50_ms` | 0.089 | ms | `results/raw/latency_noop_r1000_rep*.csv` | nearest-rank p50 over 15000 raw e2e samples from intended submit time |
| `latency_noop_1000ps_p95_ms` | 0.119 | ms | `results/raw/latency_noop_r1000_rep*.csv` | nearest-rank p95 over 15000 raw e2e samples from intended submit time |
| `latency_noop_1000ps_p99_ms` | 0.153 | ms | `results/raw/latency_noop_r1000_rep*.csv` | nearest-rank p99 over 15000 raw e2e samples from intended submit time |
| `latency_noop_100ps_achieved` | 100 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_100ps_p50_ms` | 3.12 | ms | `results/raw/latency_noop_r100_rep*.csv` | nearest-rank p50 over 1800 raw e2e samples from intended submit time |
| `latency_noop_100ps_p95_ms` | 4.19 | ms | `results/raw/latency_noop_r100_rep*.csv` | nearest-rank p95 over 1800 raw e2e samples from intended submit time |
| `latency_noop_100ps_p99_ms` | 4.24 | ms | `results/raw/latency_noop_r100_rep*.csv` | nearest-rank p99 over 1800 raw e2e samples from intended submit time |
| `latency_noop_2000ps_achieved` | 2000 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_2000ps_p50_ms` | 0.019 | ms | `results/raw/latency_noop_r2000_rep*.csv` | nearest-rank p50 over 30000 raw e2e samples from intended submit time |
| `latency_noop_2000ps_p95_ms` | 0.029 | ms | `results/raw/latency_noop_r2000_rep*.csv` | nearest-rank p95 over 30000 raw e2e samples from intended submit time |
| `latency_noop_2000ps_p99_ms` | 0.056 | ms | `results/raw/latency_noop_r2000_rep*.csv` | nearest-rank p99 over 30000 raw e2e samples from intended submit time |
| `latency_noop_4000ps_achieved` | 4000 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_4000ps_p50_ms` | 0.019 | ms | `results/raw/latency_noop_r4000_rep*.csv` | nearest-rank p50 over 60000 raw e2e samples from intended submit time |
| `latency_noop_4000ps_p95_ms` | 0.028 | ms | `results/raw/latency_noop_r4000_rep*.csv` | nearest-rank p95 over 60000 raw e2e samples from intended submit time |
| `latency_noop_4000ps_p99_ms` | 0.062 | ms | `results/raw/latency_noop_r4000_rep*.csv` | nearest-rank p99 over 60000 raw e2e samples from intended submit time |
| `latency_noop_500ps_achieved` | 500 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_noop_500ps_p50_ms` | 0.676 | ms | `results/raw/latency_noop_r500_rep*.csv` | nearest-rank p50 over 7500 raw e2e samples from intended submit time |
| `latency_noop_500ps_p95_ms` | 0.861 | ms | `results/raw/latency_noop_r500_rep*.csv` | nearest-rank p95 over 7500 raw e2e samples from intended submit time |
| `latency_noop_500ps_p99_ms` | 0.89 | ms | `results/raw/latency_noop_r500_rep*.csv` | nearest-rank p99 over 7500 raw e2e samples from intended submit time |
| `latency_sleep5ms_1000ps_achieved` | 580 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_1000ps_p50_ms` | 1768.2 | ms | `results/raw/latency_sleep5ms_r1000_rep*.csv` | nearest-rank p50 over 15000 raw e2e samples from intended submit time |
| `latency_sleep5ms_1000ps_p95_ms` | 3433.6 | ms | `results/raw/latency_sleep5ms_r1000_rep*.csv` | nearest-rank p95 over 15000 raw e2e samples from intended submit time |
| `latency_sleep5ms_1000ps_p99_ms` | 3582.6 | ms | `results/raw/latency_sleep5ms_r1000_rep*.csv` | nearest-rank p99 over 15000 raw e2e samples from intended submit time |
| `latency_sleep5ms_100ps_achieved` | 100 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_100ps_p50_ms` | 13.18 | ms | `results/raw/latency_sleep5ms_r100_rep*.csv` | nearest-rank p50 over 1800 raw e2e samples from intended submit time |
| `latency_sleep5ms_100ps_p95_ms` | 16.66 | ms | `results/raw/latency_sleep5ms_r100_rep*.csv` | nearest-rank p95 over 1800 raw e2e samples from intended submit time |
| `latency_sleep5ms_100ps_p99_ms` | 16.73 | ms | `results/raw/latency_sleep5ms_r100_rep*.csv` | nearest-rank p99 over 1800 raw e2e samples from intended submit time |
| `latency_sleep5ms_2000ps_achieved` | 570 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_2000ps_p50_ms` | 6315.9 | ms | `results/raw/latency_sleep5ms_r2000_rep*.csv` | nearest-rank p50 over 30000 raw e2e samples from intended submit time |
| `latency_sleep5ms_2000ps_p95_ms` | 11910.9 | ms | `results/raw/latency_sleep5ms_r2000_rep*.csv` | nearest-rank p95 over 30000 raw e2e samples from intended submit time |
| `latency_sleep5ms_2000ps_p99_ms` | 12410.6 | ms | `results/raw/latency_sleep5ms_r2000_rep*.csv` | nearest-rank p99 over 30000 raw e2e samples from intended submit time |
| `latency_sleep5ms_4000ps_achieved` | 572 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_4000ps_p50_ms` | 15061.3 | ms | `results/raw/latency_sleep5ms_r4000_rep*.csv` | nearest-rank p50 over 60000 raw e2e samples from intended submit time |
| `latency_sleep5ms_4000ps_p95_ms` | 28470.2 | ms | `results/raw/latency_sleep5ms_r4000_rep*.csv` | nearest-rank p95 over 60000 raw e2e samples from intended submit time |
| `latency_sleep5ms_4000ps_p99_ms` | 29673.1 | ms | `results/raw/latency_sleep5ms_r4000_rep*.csv` | nearest-rank p99 over 60000 raw e2e samples from intended submit time |
| `latency_sleep5ms_500ps_achieved` | 499 | jobs/s | `results/summary.csv` | completed jobs divided by wall time |
| `latency_sleep5ms_500ps_p50_ms` | 8.18 | ms | `results/raw/latency_sleep5ms_r500_rep*.csv` | nearest-rank p50 over 7500 raw e2e samples from intended submit time |
| `latency_sleep5ms_500ps_p95_ms` | 9.96 | ms | `results/raw/latency_sleep5ms_r500_rep*.csv` | nearest-rank p95 over 7500 raw e2e samples from intended submit time |
| `latency_sleep5ms_500ps_p99_ms` | 10.21 | ms | `results/raw/latency_sleep5ms_r500_rep*.csv` | nearest-rank p99 over 7500 raw e2e samples from intended submit time |
| `priority_0_queue_wait_p50_ms` | 1950 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p50 over 6000 raw queue-wait samples at priority 0 |
| `priority_0_queue_wait_p95_ms` | 2528 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p95 over 6000 raw queue-wait samples at priority 0 |
| `priority_9_queue_wait_p50_ms` | 634 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p50 over 6000 raw queue-wait samples at priority 9 |
| `priority_9_queue_wait_p95_ms` | 1213 | ms | `results/raw/priority_0_50_9_50_r*.csv` | nearest-rank p95 over 6000 raw queue-wait samples at priority 9 |
| `priority_queue_wait_reduction_p50` | 67.5 | % | `results/raw/priority_0_50_9_50_r*.csv` | reduction in median queue wait for priority 9 relative to priority 0, same run |
| `scaling_efficiency_cpu_hash200k_2_threads` | 91.1 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_cpu_hash200k_4_threads` | 85 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_cpu_hash200k_8_threads` | 54.2 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_noop_2_threads` | 47.2 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_noop_4_threads` | 21.9 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_noop_8_threads` | 10.4 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_sleep5ms_2_threads` | 100.3 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_sleep5ms_4_threads` | 100.5 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_efficiency_sleep5ms_8_threads` | 100.1 | % | `results/summary.csv` | speedup divided by thread count |
| `scaling_speedup_cpu_hash200k_2_threads` | 1.82 | x | `results/summary.csv` | throughput at 2 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_cpu_hash200k_4_threads` | 3.4 | x | `results/summary.csv` | throughput at 4 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_cpu_hash200k_8_threads` | 4.34 | x | `results/summary.csv` | throughput at 8 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_noop_2_threads` | 0.94 | x | `results/summary.csv` | throughput at 2 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_noop_4_threads` | 0.88 | x | `results/summary.csv` | throughput at 4 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_noop_8_threads` | 0.83 | x | `results/summary.csv` | throughput at 8 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_sleep5ms_2_threads` | 2.01 | x | `results/summary.csv` | throughput at 2 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_sleep5ms_4_threads` | 4.02 | x | `results/summary.csv` | throughput at 4 threads divided by throughput at 1 thread, same workload |
| `scaling_speedup_sleep5ms_8_threads` | 8.01 | x | `results/summary.csv` | throughput at 8 threads divided by throughput at 1 thread, same workload |
| `source_lines_cpp` | 6952 | lines | `src/ include/ apps/` | newline count across the C++ sources and headers, excluding tests and third_party |
| `test_lines_cpp` | 3180 | lines | `tests/` | newline count across the test sources |
| `tests_total` | 132 | tests | `build/jobengine_tests` | counted from `jobengine_tests --gtest_list_tests` |
| `throughput_cpu_hash200k_1_worker_threads` | 5166 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w1 |
| `throughput_cpu_hash200k_2_worker_threads` | 9413 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w2 |
| `throughput_cpu_hash200k_4_worker_threads` | 17567 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w4 |
| `throughput_cpu_hash200k_8_worker_threads` | 22402 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=cpu_hash200k_w8 |
| `throughput_noop_1_worker_threads` | 61278 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w1 |
| `throughput_noop_2_worker_threads` | 57896 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w2 |
| `throughput_noop_4_worker_threads` | 53747 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w4 |
| `throughput_noop_8_worker_threads` | 50876 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=noop_w8 |
| `throughput_sleep5ms_1_worker_threads` | 163 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w1 |
| `throughput_sleep5ms_2_worker_threads` | 327 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w2 |
| `throughput_sleep5ms_4_worker_threads` | 655 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w4 |
| `throughput_sleep5ms_8_worker_threads` | 1305 | jobs/s | `results/summary.csv` | median of 3 runs, suite=worker_scaling label=sleep5ms_w8 |
| `workflow_chain4_w1_p50_ms` | 9844.9 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_chain4_w1_throughput` | 35.47 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_chain4_w4_p50_ms` | 2466.8 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_chain4_w4_throughput` | 141.63 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_chain4_w8_p50_ms` | 1232.3 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_chain4_w8_throughput` | 283.29 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_diamond6_w1_p50_ms` | 15481.7 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_diamond6_w1_throughput` | 23.66 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_diamond6_w4_p50_ms` | 3867.6 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_diamond6_w4_throughput` | 94.48 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_diamond6_w8_p50_ms` | 1915.1 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_diamond6_w8_throughput` | 190.73 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_fanout6_w1_p50_ms` | 9893.7 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_fanout6_w1_throughput` | 23.65 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_fanout6_w4_p50_ms` | 2471.6 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_fanout6_w4_throughput` | 94.81 | workflows/s | `results/summary.csv` | median workflows completed per second |
| `workflow_fanout6_w8_p50_ms` | 1241 | ms | `results/summary.csv` | median of the driver's p50 over 3 runs |
| `workflow_fanout6_w8_throughput` | 188.81 | workflows/s | `results/summary.csv` | median workflows completed per second |
