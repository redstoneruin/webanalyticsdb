# Architecture acceptance audit

This maps the v1 architecture to implementation and validation. The design's
explicit follow-ups—SQL, joins, event-time backfill, schema evolution, columnar
projections, rollups, distributed writers and online backup—are outside v1.

| Contract | Implementation | Evidence |
| --- | --- | --- |
| C library and executable; reliable build; isolated fixtures | `include/wadb.h`, Makefile, `src/main.c` | 77 tests, Linux/macOS builds and sanitizers; no prototype data found requiring import |
| Immutable fixed schemas, canonical portable encoding, bounded strings and nulls | `src/core/schema.c`, `encoding.c`; `docs/storage-format.md` | Schema/format golden bytes, malformed inputs, overflow and symbol tests |
| Server ingestion clock, per-table ties/sequence ordering, durable watermarks | `src/ingest/writer.c`, `src/storage/segments.c`, `metadata.c` | Storage/writer tests for clock rollback, ties, reopen, atomic batches and retention continuity |
| Segmented append-only row log, day/size rotation, no row WAL | `src/storage/format.c`, `segments.c` | Exact repeated appends, boundaries, checksums, crash matrix and independent receipt verification |
| Bounded FIFO writer and group synchronization | `src/ingest/writer.c`, `src/platform/files.c` | Concurrent append/grouping tests, blocked-sync admission tests, measured batching workloads |
| Durable metadata creation, rotation and retirement | `src/storage/metadata.c`, `retention.c`, `src/core/database.c` | 217 process-termination points and 144 injected failures; complete corruption fails closed |
| Atomic segment dictionaries, bounded interning and sealed read cache | `src/storage/dictionary.c`, `src/query/cache.c` | Recovery/rollback tests, grouping across segments, high-cardinality benchmarks |
| Sparse time/sequence indexes, repairable derived files, bounded pages | `src/storage/index.c`, `src/query/scan.c` | Header/interior/checkpoint corruption, unavailable index storage, indexed range byte counts |
| Stable committed snapshots and owned query output | `src/query/snapshot.c`, `query.c`, `cursor.c` | Concurrent readers/appends/retention, reference-model results, cursor expiry and pagination |
| Projection, typed predicates, exact aggregates, buckets and grouping | `src/query/plan.c`, `query.c` | Null/overflow/empty bucket semantics, seeded reference model, time ties and boundaries |
| Whole-segment retention and reader-delayed deletion | `src/storage/retention.c` | Concurrent maintenance tests, crash recovery, independent lifecycle timing with a pinned cursor |
| Integrity checks, counters/histograms and bounded notifications | `src/storage/integrity.c`, `src/core/stats.c` | Corruption/budget/cancellation tests, statistics tests, HTTP lifecycle checks |
| Small pinned HTTP/JSON dependencies, authentication and same-origin management | `src/server/server.c`, `api.c`, `auth.c`, `json.c` | API/auth/socket tests, CSRF/origin/limits, exact 64-bit transport and strict parsing |
| Bounded SSE replay, resets, slow clients and clean shutdown | `src/server/server.c` | Two-minute streams, retained/stale replay, disconnected keepalive regression, slow readers and overload |
| Six console views, bounded jobs, real-time rendering | `web/`, `src/server/jobs.c` | Browser create/append/query/cursor/job/integrity/retention/reload workflow; browser capacity run |
| Independent C API and HTTP measurements | `tools/bench-core.c`, `benchmark.py`, `benchmark_http.py` | Five driver contract tests, driver sanitizers, restart count/sum checks and published raw reports |
| Full disk and stopped-copy restore | `tools/full-disk-smoke.py`, `docs/operations.md` | Actual 16 MiB tmpfs exhaustion/recovery; two-table 8,803-row copy, digest, integrity and next-sequence exercise |

## Concrete v1 choices

The benchmark tools live in `tools/`, rather than the illustrative `bench/`
directory. The API uses `wadb_table_stats`, `wadb_query_options` and explicit
ownership rules documented in the public header and query guide. Endpoint names
and exact request bodies are specified in the HTTP guide; the architecture's
endpoint list was illustrative.

Snapshots copy bounded segment/index metadata and pin at table granularity.
That can delay deletion of unrelated retired segments in the same table. Sealed
index readers use one 4 KiB page each plus OS caching, not a second global index
cache. Dictionary caching has its own bound. Retired manifest entries remain
within the one-million-record limit; tombstone compaction is a later lifecycle
optimization. Neither choice changes the append-only row or retention contract.

The filesystem tests establish process-crash behavior on the recorded APFS and
Linux container filesystems. They do not simulate physical power removal or
recover corrupted durable media. Indeterminate responses may have committed;
the API has no persistent idempotency promise. The format remains a development
format, with no compatibility guarantee across future versions.

Capacity conclusions, test durations and measurement limits are recorded in the
[release report](benchmarks/release-2026-09-28.md), rather than inferred from
correctness-test throughput. Average throughput is not a latency guarantee: the
paced C API run recorded a multi-second backlog that the report retains explicitly.
