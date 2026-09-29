# WebAnalyticsDB

A C17 append-only analytics database with fixed-width table schemas, segmented time-series storage, durable batching, indexed queries and a live web console.

## Architecture plan

The planned v1 is implemented. See [the database design](docs/architecture.md),
[acceptance audit](docs/architecture-audit.md), and [implementation status](docs/implementation-status.md)
for its contracts, validation and limits. The [storage format](docs/storage-format.md)
specifies the binary codecs.

## Setup

Prereqs: a C17 compiler, POSIX threads, `make`, `ar`, Python 3 (asset embedding),
`pkg-config`, and GNU libmicrohttpd **1.0.10**. yyjson 0.12.0 is vendored. See
[dependency pins](docs/dependencies.md). Linux and macOS are the intended platforms. On macOS, the Makefile selects the SDK belonging to the active developer directory; `WADB_SDK=/path/to/MacOSX.sdk` overrides it.

## Build

```bash
make
```

Gets you:
- `build/webanalyticsdb`: HTTP server, embedded console, and offline integrity CLI.
- `build/test*`: Test binaries.
- `build/libwebanalyticsdb.a`: Static lib for core logic.

## Test

```bash
make test
```

Runs all tests in `build/`. Check output for pass/fail.

Tests use temporary directories and the command fails if any executable fails.
Run `make sanitize` for AddressSanitizer and UndefinedBehaviorSanitizer builds.
The [failure-testing guide](docs/failure-testing.md) describes the subprocess
crash matrix, injected I/O failures, and tested failure model.
See [platform validation](docs/platform-validation.md) for the isolated Linux
build, sanitizer, crash, and HTTP lifecycle workflow.

The library API is declared in `include/wadb.h`. It supports durable batched
appends through a bounded writer queue, table schemas, segment dictionaries,
recovery, indexed scans, filtering, grouped aggregates, time buckets, expiring
snapshot cursors, retention, integrity checks and operational metrics. See [query and lifecycle API
notes](docs/query-api.md) for ownership, limits and semantics.

Run the server and open its printed local URL:

```sh
./build/webanalyticsdb serve --data ./data --port 8080
```

The console exchanges the admin token in `data/admin.token` for an authenticated
browser session. It supports schemas/appends, queries and page exports, live
sampled rows, bounded test jobs, segment inspection, integrity jobs, and retention
preview/apply. See the [HTTP and console guide](docs/http-api.md) for authentication,
endpoint bodies, remote proxy configuration, and resource limits.

After stopping the server, verify retained source data with:

```sh
./build/webanalyticsdb check --data ./data
```

See the [stopped backup and restore procedure](docs/operations.md).
Build the independent drivers with `make benchmark`; see the [benchmark guide](docs/benchmarks.md)
for engine/HTTP workloads, rate and overload accounting, and cache/memory controls.
The [release report](docs/benchmarks/release-2026-09-28.md) contains measured capacity,
latency variability, tuning decisions and raw engine/HTTP/browser results. A separate
[larger-than-memory result](docs/benchmarks/linux-memory-2026-09-28.md) verifies cache
eviction and a dataset exceeding the Linux container's memory budget.
This development format is not yet a production compatibility promise.

## Clean

```bash
make clean
```

Nukes `build/`.
