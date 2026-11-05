# Phase 5B Part B — checkpoint-interval sweep

Fixed workload: 1200 transactions per run (seed 1), hard crash after the last
commit. Each row averages 5 repetitions. Times are milliseconds.

| checkpoint interval (log records) | throughput (txn/s) | recovery time (ms) | WAL bytes | log scanned (bytes) | redo records applied |
|---|---:|---:|---:|---:|---:|
| none | 477.7 | 4.51 | 425181 | 425180 | 3600 |
| 100 | 479.7 | 4.57 | 431897 | 7212 | 3600 |
| 500 | 477.8 | 4.77 | 426425 | 35532 | 3600 |
| 1,000 | 478.2 | 4.25 | 425745 | 70932 | 3600 |
| 5,000 | 477.0 | 4.57 | 425313 | 70932 | 3600 |

