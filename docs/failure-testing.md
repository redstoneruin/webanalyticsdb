# Failure testing

`make test` includes a subprocess crash matrix. `make build/test_crash &&
build/test_crash` runs it independently. The production library contains no
fault-injection state or entry points; only the separate archive under
`build/crash/` enables `WADB_TEST_FAULTS`.

The harness first records the real syscall boundaries of a successful operation.
For every recorded boundary, it creates a fresh fixture, opens it in a child
process, and terminates that process immediately before or after the syscall.
There is no database shutdown or buffered cleanup in the terminated process.
All parent database threads are joined before `fork`, and every child has a
timeout. The parent drains its diagnostic pipe while the child runs.

The same harness injects `EIO` before each instrumented syscall and `ENOSPC`
before file creation and writes. These failures use the actual error-handling
paths. Existing storage tests additionally exercise partial writes, interrupted
write retries, failed synchronization, and complete corrupt frames.

| Operation | Termination points | Injected errors |
| --- | ---: | ---: |
| Initial database/catalog installation | 17 | 11 |
| Table directory, schema, manifest and catalog installation | 34 | 23 |
| First segment and first append | 26 | 18 |
| Rotation at the segment size limit | 35 | 25 |
| Rotation across a UTC day boundary | 36 | 25 |
| Retirement of an expired prefix | 22 | 13 |
| Retirement of all data, including the active segment | 27 | 16 |
| Retention configuration replacement | 10 | 7 |
| Recovery truncation of an incomplete active tail | 10 | 6 |
| **Total** | **217** | **144** |

These counts describe the September 28, 2026 macOS and Linux runs. The test discovers
boundaries dynamically and checks that each repeated execution reaches the same
prefix; counts can change when the protocol or platform changes. Each scenario
also runs once to successful acknowledgement followed by immediate process exit.

Every restart must satisfy an independent data model:

- All previously acknowledged, unexpired rows retain their exact values, symbol
  strings, and consecutive sequence numbers. An interrupted batch is absent or
  complete; it cannot appear as a partial request.
- A table creation resolves to absent or fully installed. Orphan directories are
  never imported, and creating the absent table again succeeds.
- Only the expired prefix may disappear during retention. Manifest retirement
  and preserved watermarks agree; restart finishes deletion without resurrection.
- Schema fingerprints match the requested fields, at most one active segment
  exists, and it is the final manifest entry. A full integrity scan succeeds.
- Appending after recovery starts at the next sequence and clamps a deliberately
  rolled-back clock to the recovered watermark. A second restart preserves that
  append and the exact remaining data.
- Indeterminate authoritative metadata updates stop mutation in the failed
  process until recovery.

The instrumented operations cover temporary-file creation, writes, durable file
synchronization, atomic rename, directory synchronization, table/partition/segment
creation, retention unlink, and recovery truncation. Instrumentation expands to
the original syscall in normal builds, preserving its return value and errno.

## What this establishes

This is a **process-termination and syscall-error model**. The operating system
and device remain powered, and dirty cache contents may survive a terminated
process. It does not emulate arbitrary write reordering, lost device caches,
filesystem journal damage, or a physical power interruption. The software orders
file/directory synchronization before acknowledgement; guarantees under power
loss still depend on separately tested filesystem and device behavior.

Injected `ENOSPC` verifies rejection and recovery without consuming the user's
disk. A separate [Linux validation](platform-validation.md) smoke fills a private
16 MiB tmpfs to zero free bytes, verifies exact acknowledged rows remain readable,
checks that writes stop, and exercises integrity/restart and subsequent append.
That adds a real full-filesystem allocation test; tmpfs does not model physical
media flushing or a full disk-backed filesystem's journal behavior.
Complete corruption remains a fail-closed condition covered by the
storage, index, query, and integrity suites; it is not treated as a torn tail.
