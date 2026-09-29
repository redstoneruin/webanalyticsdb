# Benchmark procedure

Build the normal, optimized binaries and validate the workload driver:

```sh
make benchmark
python3 tools/test-benchmark.py
```

`build/bench-core` links the production core library. It runs separately from the
database server and console; it does not use the server's built-in generator.
The HTTP driver is a separate Python process talking to a real server process.
Python is development tooling only. Durable acknowledgements remain enabled in
every workload. Sanitizer runs validate the driver but are never capacity results.

## Reports and fixtures

```sh
python3 tools/benchmark.py --mode both --profile smoke --repetitions 1 \
  --output build/benchmark-smoke.json
python3 tools/benchmark.py --mode both --profile standard --seconds 5 \
  --repetitions 3 --output build/benchmark-standard.json
```

The output file must not exist. Each workload creates a fresh fixture under
`build/`, verifies it after restart, and removes it afterward. Use `--work-dir`
to choose the device under test, and `--keep-data` to retain fixtures for further
inspection. Never point the driver at a production data directory. Core ingestion
requires a nonexistent directory; core probes require a benchmark fixture marker.

Reports are updated atomically after each case. `complete: false` and `error`
record an interrupted or failed run. A run is not a passing measurement merely
because the JSON file exists. `--case NAME` selects individual named cases; names
are defined in `cases()` and `http_cases()`. `--query-repeats` controls repeated
queries within one process and `--repetitions` repeats fresh datasets/processes.

The time limit and logical payload cap (`--max-bytes`, default 256 MiB) both bound
each case. Whichever is reached first stops new work, then admitted work drains.
Report the actual elapsed duration: a byte-capped run is not a sustained run of
the requested duration. Disk space is checked conservatively for framing/index
overhead plus 1 GiB of headroom. Input generation, receipts, and query output are
bounded; the driver never accumulates every latency sample in memory.

## Workloads

The fixed schema contains `_time`, an input ID, a skewed site ID, a path symbol,
duration, status, country bytes, and optional fixed padding. Widths are exactly
32, 48, 128, 512 and 8,192 bytes. Every tenth input goes to a non-primary site;
the rest go to site 1. Paths cycle through the configured cardinality. No strings
are replaced with unchecked hashes. The sequence assigned by the database can
differ from the input ID when producers race.

The standard C API matrix covers:

- Batches of 1, 64, 1,024, and the maximum legal frame for each width. The illegal
  1,024-row / 8,192-byte combination is omitted; it cannot fit in a 1 MiB frame.
  `--batch 0` computes the largest batch including worst-case new dictionary
  definitions, rather than treating payload bytes as the entire frame.
- One/eight producers and one/eight active tables, low/high string cardinality,
  equal timestamps, normal and smaller segments, different batching delays and
  frame targets, and ingestion concurrent with aggregation.
- Saturation, scheduled steady arrivals, bursts at 100 ms boundaries, and overload.
- A fixed 262,144-row retention fixture with 1 MiB segments. After ordinary
  restart verification, it measures preview, durable retirement while a cursor
  pins the files, deletion on cursor release, and restart. It checks that new
  readers see no retired rows, the old cursor still reads its snapshot, and a
  new durable append preserves sequence/time watermarks. This fixture has its
  own row cap and duration, independent of the generic throughput time limit.

The HTTP matrix covers the widths, small/large batches, concurrent tables,
steady/bursty/overloaded arrivals, high cardinality, concurrent queries and
console-equivalent traffic, and a 120-second unread SSE client workload. HTTP
batch payloads include JSON encoding and fixed-byte hex expansion. This cost
belongs to the HTTP measurement; it is not attributed to the storage encoder.

The console traffic case subscribes to SSE and fetches statistics, tables and a
20-row tail once per second. This measures its server traffic, not browser
rendering. To include an actual browser, use:

```sh
python3 tools/benchmark.py --mode http --profile standard \
  --case http_console_traffic --seconds 60 --repetitions 1 \
  --console-wait-seconds 60 --output build/benchmark-browser.json
```

The driver prints a temporary local URL and token-file path, waits for the browser
to connect, then starts ingestion. Record separately whether the browser actually
connected and which console screen was open. The wait flag alone is not evidence
that browser rendering occurred. Credentials are never included in the report.

## Accounting and interpretation

- **Offered** rows include arrivals released by the generator. Scheduled arrivals
  enter a fixed 1,024-descriptor queue. Full queues increment generator drops;
  they do not disappear from throughput accounting.
- **Submitted** rows reached the append API. **Committed** rows received a durable
  success response. Admission rejections and failures have separate counters.
  Indeterminate errors abort a capacity run; automatic retries could duplicate
  data and would invalidate receipt accounting.
- Arrival-to-ack latency includes waiting in the driver queue, input preparation,
  database queueing and synchronization. API/HTTP-call latency is also reported.
  A late generator retains the original scheduled timestamp, preventing a slow
  closed-loop client from hiding queue delay. Saturation has no external schedule.
- Percentiles are upper bounds from a bounded histogram with approximately 3.2%
  resolution above 32 microseconds. Rejected requests are included in the overall
  latency distribution; inspect rejection counts alongside percentiles.
- Throughput uses the wall interval including the final ingestion drain. Resource
  samples are taken approximately once per second. Peak RSS and CPU are process
  measurements; C API figures include the standalone generator. HTTP reports
  client CPU separately from server CPU and server lifetime peak RSS.
- Kernel I/O bytes come from Linux `/proc/<pid>/io` or, for the native C driver,
  macOS `proc_pid_rusage`. Unsupported counters are null, never assumed zero.
  These are OS accounting, not flash-device write amplification. HTTP macOS I/O
  counters are currently unavailable; Linux provides the complete set.
- Source payload, dictionary definitions, framing, derived index and final
  metadata file sizes are reported separately. Final file footprint and cumulative
  kernel write bytes answer different questions; do not conflate them.
- Raw sequential write references synchronize each 256 KiB or 1 MiB block on the same filesystem.
  Interpret it at its reported block size; different synchronization granularity
  changes the result. It is not a competing database benchmark.

Every C API case is reopened in a separate process. Every HTTP case stops and
restarts its server. Broad count and input-ID sum are compared with successful
receipts for every table. The driver contract tests additionally read exact IDs,
symbols, skewed sites and timestamps independently through HTTP, check scheduled
arrival accounting, and reject fixture overwrite/unowned probes.

## Queries, time, and caches

Core probes measure recent tail, minute/hour/day ranges, counts by minute, top
paths for site 1, and broad count/sum. Byte, frame, index and dictionary read
counters accompany latency. High-cardinality queries can hit the configured
group budget; their explicit error is retained rather than relabeled as success.
Broad receipt verification must always succeed for a completed run.

Normal workloads use the real ingestion clock. Explicit
`--synthetic-clock-step-us` cases use a benchmark-only internal clock callback to
create time ties or multi-day ranges quickly. They are labeled in the report and
do not add historical timestamp insertion to the database API. All other storage,
queue, dictionary and synchronization code is the production implementation.

Reopening a process does **not** make the OS cache cold. On Linux, `--cold` clears
the engine's unpinned dictionary cache and advises eviction of only this fixture's
segment/index files. It checks page residency with `mincore` before and after
eviction and refuses a cold-read claim if any of those pages remain resident.
Each cold query gets a fresh eviction. No global cache-drop command is used.

Repeated ordinary queries show warmed/repeated execution, but a dataset larger
than RAM still cannot be fully cached. Active metadata remains resident as it
would in a running database. A Docker result concerns the guest's page cache;
host filesystem/device caches can remain warm. Do not call that physical-disk
cold-cache performance.

## Dataset larger than the memory budget

Use the pinned [Linux image](platform-validation.md) and a new container with
256 MiB RAM and no additional swap allowance, then load 512 MiB of logical rows:

```sh
docker build -f tools/linux-test.Dockerfile -t wadb-linux-test:local .
mkdir -p build/benchmarks
docker run --rm --network none --cpus 2 --memory 256m --memory-swap 256m \
  --mount "type=bind,source=$(pwd)/build/benchmarks,target=/results" \
  wadb-linux-test:local python3 tools/benchmark.py --profile large \
  --max-bytes 536870912 --repetitions 1 --query-repeats 2 --cold \
  --output /results/linux-large.json
```

This profile uses 512-byte rows, 1,024-row batches, four producers, 64 MiB segments,
and a labeled synthetic clock spanning multiple days. It requires the configured
dataset to exceed the effective process/container memory budget and checks the
actual source-file size afterward. Reports include the cgroup limit, memory
peak/events, process RSS samples, query byte counts, and verified cold eviction.
This validates operation beyond a constrained RAM budget; it does not substitute
for a native production-machine capacity test.
