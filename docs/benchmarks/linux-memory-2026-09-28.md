# Linux operation beyond the memory budget

This is a bounded-memory and cache-behavior result, not a production capacity
claim. [Raw measurements](linux-memory-2026-09-28.json) contain commands, source
and binary hashes, every query result, resource samples, and cgroup counters.
The [benchmark procedure](../benchmarks.md) describes reproduction and accounting.

The run used Ubuntu 24.04 / GCC 13.3.0 / glibc 2.39 on aarch64 LinuxKit 5.15.49,
inside Docker Desktop on an Apple M1 Max Mac with 10 CPU cores and 64 GiB RAM.
The container was limited to 2 CPUs and **256 MiB RAM**, with its total memory/swap
allowance also set to 256 MiB. Its overlay filesystem resides on the Docker VM's
disk. Host cache residency was not controlled.

| Measured property | Result |
| --- | ---: |
| Durable rows | 1,048,576 |
| Fixed row width | 512 bytes |
| Logical payload | 512 MiB |
| Authoritative segment files | 536,995,536 bytes |
| Derived indexes | 87,432 bytes |
| Dictionary definitions inside frames | 25,040 bytes |
| Segment/frame overhead | 99,584 bytes |
| Segments | 10 |
| Ingestion elapsed, including drain | 3.024 seconds |
| Reported ingest-process peak RSS | 21,676,032 bytes (20.7 MiB) |
| Restart | 47.9 ms |
| OOM / OOM-kill events | 0 / 0 |

Four producer threads submitted 1,024-row requests. A benchmark-only clock
advanced three minutes per request to create multiple days of ordered data;
segment targets were 64 MiB. This synthetic clock is explicitly recorded and
does not change the public append API. The workload stopped at its payload cap,
so its observed 346,741 rows/second over three seconds is **not a sustained
throughput estimate**.

All 14 ordinary/repeated queries and seven queries after verified eviction
succeeded. After restart, broad count was 1,048,576 and the input-ID sum was
549,756,338,176, exactly matching successful receipts. Selected query timings:

| Query | First ordinary pass | Second ordinary pass | After verified guest-page eviction |
| --- | ---: | ---: | ---: |
| Recent 100-row tail | 1.60 ms | 1.54 ms | 2.11 ms |
| Last minute | 1.50 ms | 1.72 ms | 1.85 ms |
| Last hour | 31.1 ms | 31.4 ms | 35.1 ms |
| Last day | 963.7 ms | 741.5 ms | 834.4 ms |
| Count by minute | 1,971.4 ms | 2,033.0 ms | 1,922.2 ms |
| Top paths, site 1 | 1,986.7 ms | 2,029.8 ms | 1,921.1 ms |
| Broad count/sum | 1,940.8 ms | 2,004.0 ms | 1,905.0 ms |

The file pages were nonresident according to `mincore` before every cold query.
The cold broad query scanned 537,019,296 bytes and incurred 537,116,672 kernel
read bytes. Even the repeated broad pass incurred approximately 413 MB of kernel
reads: a 512 MiB dataset cannot stay fully cached in the 256 MiB container.
The last-hour query scanned only about 10.5 MB, showing the time index restricted
the work to the selected interval.

The cgroup recorded 18,904 memory-limit events and no OOM events; the kernel
reclaimed cache as the workload approached its limit. This kernel does not expose
`memory.peak`, so the report retains the available current-memory/events counters
and per-process RSS samples instead. Page eviction here concerns Linux guest
cache only; the host or device can cache the virtual disk. Timing differences
between single passes include normal noise and are not evidence of a cold-cache
speed advantage.
