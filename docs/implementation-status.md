# Implementation status

The planned v1 implementation and acceptance work are complete. This tracks the
delivered database, its evidence and the limits of the tested guarantees. See
the [architecture audit](architecture-audit.md) for the requirement mapping.

- [x] 0: reliable build and isolated tests; legacy-data disposition
- [x] 1: public schema API, portable codec, byte-level storage specification
- [x] 2: durable storage and subprocess crash-transition matrix
- [x] 3: sparse indexes, committed snapshots, cursors, filters, projection, buckets and aggregation
- [x] 4: dictionaries, retention, integrity, metrics and independent benchmark harness
- [x] 5: HTTP API, authentication, SSE, console and jobs; browser and slow-client/overload checks
- [x] 6: published benchmark report and measured tuning; Linux/macOS validation and stopped-copy restore

Initial inspection found no existing data files requiring migration. The old
`WebEvent`/`Timeline` implementation and its tests have been replaced. No legacy
file is automatically imported. The new tests cover its append/reopen/range and
bucket behavior, plus the new storage and query contracts.

## Working library

- Immutable fixed schemas, explicit little-endian rows, canonical padding/nulls,
  validation and CRC-32C segment/frame/metadata codecs.
- Durable table catalog, schema and manifest installation; exclusive process
  ownership; UTC-day/size/frame-count rotation; recovery of complete append
  prefixes; clock rollback clamping and durable sequence continuity.
- A dedicated FIFO writer with bounded reservations, cross-request group commits,
  configurable batching, overload rejection, and indeterminate-write handling.
- Per-column segment dictionaries with provisional interning, atomic definitions
  alongside rows, bounded allocation, rollback and budget-driven rotation.
- Sorted segment directories, per-frame sparse indexes and 4 KiB index page
  readers. Normal startup checks sealed metadata without replaying historical
  rows. Damaged indexes rebuild from validated source data; transient indexes
  keep reads available if derived files cannot be installed.
- Bounded reader snapshots and a bounded immutable dictionary cache. Indexed
  range and sequence scans, typed conjunctions, projection, exact counts,
  checked sums, numeric min/max/averages, time buckets and grouped top results.
- Expiring scan cursors that copy query inputs, pin the original committed prefix,
  share scan budgets across pages and reject stale IDs explicitly.
- Retention preview/configuration/apply, durable retirement before file deletion,
  conservative reader pins, restart cleanup and preserved sequence/time watermarks.
- Commit/query counters, logarithmic latency histograms, retained table totals,
  paginated segment metadata and a bounded commit-notification ring with gap
  reporting. Overview counters do not scan historical rows.
- Full source integrity checks with budgets and cancellation. Detected corruption
  preserves source files and prevents further writes to the affected table.

`docs/storage-format.md` specifies the binary layout. `docs/query-api.md` explains
ownership, null/overflow/bucket semantics, resource accounting, cursors, retention
and integrity. `include/wadb.h` is the public interface.

## Measured capacity and limits

The [release report](benchmarks/release-2026-09-28.md) publishes the hardware,
commands, raw data, 60 repeated cases, longer engine/HTTP runs, actual console
rendering, memory/cache results and retention timing. Measurements led to a
bounded HTTP append-admission improvement; batching/frame/segment defaults were
retained based on the comparisons. The paced C API runs recorded material queue
delay despite achieving the requested average throughput; the report preserves
that result. No universal throughput/latency SLO, long endurance certification or
physical power-loss guarantee across devices/filesystems is claimed.

Retired manifest entries and empty partition directories currently remain. They
are bounded by the manifest's one-million-record limit; tombstone compaction is
not implemented. Snapshot pins are table-wide, so a reader can conservatively
delay deletion of other retired segments in that table.

## Validation

The replacement suite contains **77 tests**. `make test` and `make sanitize`
pass on macOS and Linux (AddressSanitizer + UndefinedBehaviorSanitizer). Query,
index, writer, statistics and maintenance suites also pass under ThreadSanitizer.

The tests include narrow indexed reads before/after restart; rebuilding damaged
index headers, interior records and dictionary checkpoints; unavailable derived
index storage; query rebuild budgets; owned results; null and overflow behavior;
seeded aggregate comparison against a reference model; cursor expiry and stable
pagination across appends/rotation; concurrent appends/retention/reads; and
restart after durable retirement before deletion. These correctness checks
complement the separately reported performance and operation exercises.

The subprocess matrix covers 217 termination points and 144 injected `EIO` or
`ENOSPC` failures across initial catalog installation, table creation, first
append, size/day rotation, retention configuration/deletion, and interrupted-tail
repair. Every case reopens, checks exact rows/symbols and sequence/time continuity,
runs integrity, appends again, and reopens again. See [failure testing](failure-testing.md)
for the protocol and its process-crash limitations; this is not a device power-loss
test.

Linux validation used GCC 13.3.0 / glibc 2.39 on aarch64 Ubuntu 24.04 under Docker
Desktop, with 2 CPU and 2 GiB container limits. The 120-second HTTP lifecycle test
passed there as well. A separate 16 MiB tmpfs test reached actual zero-free-space
`ENOSPC`, kept all 3,968 acknowledged rows readable, stopped writes, recovered with
successful integrity, and accepted sequence 3,969. See [platform validation](platform-validation.md)
for commands, environment details and the distinction from native capacity or
physical power-loss tests. ThreadSanitizer results above are from macOS.

## Independent benchmarks

`make benchmark` builds a standalone C driver; `tools/benchmark.py` orchestrates
fresh C API and external HTTP workloads and restart verification. It measures
scheduled/saturating arrivals, bounded generator queues and drops, committed
throughput, latency, CPU/RSS, OS I/O counters where available, source/index/dictionary
overhead, query scans and restart. Five benchmark contract tests pass; the C driver
also passes its smoke workloads under AddressSanitizer/UndefinedBehaviorSanitizer.
Engine/HTTP smoke runs pass on macOS and Linux, including verified Linux file-page
eviction. These small validation cases are not capacity claims.

A [512 MiB workload inside a 256 MiB Linux container](benchmarks/linux-memory-2026-09-28.md)
passed with 1,048,576 durable rows, exact count/sum after restart, 21 successful
ordinary/repeated/cold queries, and no OOM events. Its raw evidence includes page
residency checks and cgroup/resource samples. This establishes operation beyond
the container's RAM budget; host/device caches were not made cold. See the
[benchmark guide](benchmarks.md) for all workload definitions and limitations.

An [exploratory Mac sweep](benchmarks/macos-exploratory-baseline.md) preserved 45
restart-verified cases and exposed excess HTTP admission rejection: every append
reserved a 4 MiB response even for its small numeric receipt. Appends now reserve
an enforced 4 KiB response bound, with queue/global limits preserved. API and
blocked-sync overload regressions cover the bound and rejection-before-mutation
contract. All 60 repeated post-change comparisons and longer runs passed receipt
verification; the [release report](benchmarks/release-2026-09-28.md) interprets
throughput, latency, memory, storage overhead, query limits and tuning tradeoffs.
The actual-browser run passed for 60 seconds: 901,952 durable rows (15,030 rows/s),
no append rejections/failures, overview/live rendering, and exact count/sum after
restart. Its resource and query evidence is in `benchmarks/macos-browser.json`.
The final exploratory slow-reader case did not
complete because the initial monitor treated a normal 429 as fatal; this driver
behavior has been corrected, and no result for that case is claimed. Its separate
120-second replacement committed all 1.2 million scheduled rows without drops or
rejections, disconnected the unread client, and verified the data after restart.
The five driver contract tests pass on both Linux and macOS; the new retention
driver also passes AddressSanitizer and UndefinedBehaviorSanitizer.

## Working server and console

- A real `serve` executable with embedded assets, four HTTP database workers,
  bounded uploads and pre-reserved response memory, plus offline `check` CLI.
- GNU libmicrohttpd 1.0.10 is dynamically linked and checked at build time;
  yyjson 0.12.0 is vendored with its license and hashes. See dependencies.md.
- Owner-only admin token files, bounded expiring sessions, constant-time secret
  comparisons, CSRF and exact-origin checks, explicit HTTPS proxy configuration
  for remote binding, and bounded login attempts.
- Typed JSON table/append/query/cursor/retention/segment APIs. Strict parsing,
  duplicate/unknown-argument rejection, length-aware strings, exact 64-bit values,
  and query limits. Network uncertainty never implies absence of a commit.
- A bounded boot-scoped SSE replay ring, periodic counter snapshots, commit
  notifications, reset semantics, stream limits, and shutdown that resumes
  suspended connections before stopping the HTTP library.
- Two bounded background job slots for seeded generators and integrity checks;
  cooperative cancellation, bounded history, durable commit and thread-CPU counts.
- Browser screens for overview, schemas and appends, a query builder/advanced
  JSON with cursors and bounded exports, live sampling with gap counts, test jobs,
  segments, integrity, retention previews, and recent console errors.

The browser smoke workflow passed on September 28, 2026 using isolated temporary
storage: login, fixed-schema creation, durable append, two-page query and cursor
release, a generated workload, sampled live rows and pause, job stop, integrity
submission, retention preview, and session/SSE reconnect after a page reload.
No browser console errors were reported. This is functional validation, not an
independent capacity measurement. Server and job tests also pass ThreadSanitizer.
The full HTTP contract and exact limits are in `docs/http-api.md`.

The stopped-copy restore procedure in `docs/operations.md` passed on a two-table,
8,803-row temporary database, including file digest comparisons, full integrity,
exact query results, schema/watermark checks, and a new append at sequence 4 in
the restored sample table. A browser-style disconnected SSE regression exposed
and fixed a shutdown abort; its test and 12 separate process reproductions pass.

The independent HTTP lifecycle smoke (`python3 tools/http-stress-smoke.py`)
passed a 120-second run: 545,728 rows committed with progress at all 60 checkpoints;
the fast SSE reader received 118 statistics and 472 commit messages with no reset;
stale replay reset, retained replay resumed, the unread client disconnected, and
shutdown exited cleanly. These workload counts are correctness observations, not
capacity measurements. The standard suite also blocks durable synchronization
to verify HTTP admission backpressure: every rejected append leaves no row behind.
