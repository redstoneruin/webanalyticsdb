# Exploratory Mac baseline before append admission tuning

The [raw report](macos-exploratory-baseline.json) preserves 45 completed and
restart-verified cases: 31 C API workloads and 14 HTTP workloads. It is marked
`complete: false` because the final slow-reader workload encountered an expected
HTTP 429 in its monitoring client, which the initial driver incorrectly treated
as fatal. That client now counts monitoring rejections. No result for that
unfinished workload is included or inferred.

This sweep ran on an Apple M1 Max, 10 cores / 64 GiB RAM, on the host filesystem,
with durable acknowledgements and a two-second arrival window or 64 MiB logical
payload cap. It has one repetition, so these observations locate bottlenecks;
they are not sustained capacity estimates. Source/binary hashes, settings,
latency distributions and resource counters are retained in the raw report.

Selected observations for 48-byte rows:

| Workload | Observed committed rows/sec | HTTP rejection rows | Generator-drop rows |
| --- | ---: | ---: | ---: |
| C API, one producer, batch 1 | 52 | — | 0 |
| C API, one producer, batch 64 | 3,082 | — | 0 |
| C API, one producer, batch 1,024 | 57,479 | — | 0 |
| C API, eight producers, batch 64 | 16,291 | — | 0 |
| C API, steady 10,000 rows/sec | 9,948 | — | 0 |
| C API, bursts averaging 10,000 rows/sec | 10,340 | — | 0 |
| HTTP, steady 10,000 rows/sec | 9,927 | 0 | 0 |
| HTTP, bursts averaging 10,000 rows/sec | 4,635 | 11,072 | 0 |
| HTTP, eight producers/eight tables | 9,008 | 977,280 | 0 |

The high HTTP rejection count includes repeated offered requests from eight
unpaced producers; it is not a count of lost committed rows. None are retried by
the driver. Every completed workload matched its successful receipts after
restart. The C API burst rate can exceed the nominal rate over its measured
interval because the last 100 ms burst arrives before the end of that interval.

Inspection traced the HTTP burst/concurrency limit to reserving 4 MiB per request
from a 32 MiB response pool. With headroom for errors, the eighth simultaneous
request could be rejected even if it would return only a numeric append receipt.
The implementation now reserves an enforced 4 KiB bound for append responses;
other endpoints retain their general limit, and the 32-entry work queue and
global response cap remain bounded. A regression holds durable synchronization,
admits more than seven appends, fills the work queue, and checks that every
rejection creates no row. The API separately tests receipt/error bounds and
serialization enforcement. The [post-change report](release-2026-09-28.md)
contains the repeated comparisons and independent long slow-reader result.
