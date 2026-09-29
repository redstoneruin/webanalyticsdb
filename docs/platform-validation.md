# Platform validation

The Linux validation image pins Ubuntu 24.04 by digest and builds the pinned
libmicrohttpd 1.0.10 source after checking its SHA-256. Compiler, libc and package
versions are captured in the run output. Package repository updates can change
those versions; this is a reproducible procedure, not a bit-for-bit build claim.
The production server uses an external TLS proxy, so this test dependency is
built without its optional HTTPS support.

From the repository root, with Docker available:

```sh
docker build -f tools/linux-test.Dockerfile -t wadb-linux-test:local .
mkdir -p build/linux-validation
docker run --rm --network none --cpus 2 --memory 2g \
  --tmpfs /wadb-full-disk:rw,size=16m,mode=0700 \
  --mount "type=bind,source=$(pwd)/build/linux-validation,target=/results" \
  wadb-linux-test:local
```

Select the desired Docker context with `docker --context NAME` if necessary.
The image contains only the explicit source/dependency/test paths allowed by
`.dockerignore`. Local databases, build products, Git history and credentials are
excluded. Runtime networking is disabled; HTTP tests use container loopback.
The only host mount is the output directory. Test databases remain temporary
inside the container, which is removed at the end.

The validation script stops on any failed step:

1. The full correctness suite, including the subprocess crash matrix.
2. AddressSanitizer and UndefinedBehaviorSanitizer builds and executions.
3. The 120-second HTTP lifecycle smoke for ongoing durable commits, fast and
   unread SSE clients, retained/stale replay, and clean server shutdown.
4. Actual allocation failure on the private 16 MiB tmpfs: reads during `ENOSPC`,
   stopped writes, recovery, exact producer IDs and a new append after recovery.

Results are written to `environment.txt`, `test.log`, `sanitize.log`,
`http-smoke.log`, `http-smoke.json`, `full-disk.log`, and `full-disk.json`. Keep the image build output alongside
them when publishing results. macOS uses `make test`, `make sanitize` and
`make http-smoke` directly; its SDK selection is documented in the README.

## September 28, 2026 results (Pacific time)

| Environment | Results |
| --- | --- |
| macOS/Darwin 27.0.0, arm64, Xcode SDK, Apple M1 Max | 76 tests passed; AddressSanitizer + UndefinedBehaviorSanitizer passed |
| Ubuntu 24.04, aarch64, LinuxKit 5.15.49, GCC 13.3.0, glibc 2.39, overlayfs | 76 tests passed; AddressSanitizer + UndefinedBehaviorSanitizer passed |

The Linux container ran through Docker Desktop 20.10.23 on the same Mac with a
2 CPU / 2 GiB container limit. The host has 10 CPU cores and 64 GiB RAM; Docker's
VM exposes roughly 16 GiB. This describes the test environment, not capacity.
The image used for this run was
`sha256:7a2a7662e1dba75b79387c11088fb197cb28460ca172b5e8b9a0fe6cd656df69`.
The full-filesystem script was added afterward and run against that same image
with the script mounted read-only; the documented workflow now includes it.

Both platforms passed the 217-point process-termination matrix and 144 injected
I/O/full-disk failures. GCC exposed a nullable JSON-string formatting issue in
the SSE test; the test now validates type and length before copying the field.

The Linux HTTP lifecycle smoke committed 777,152 rows during 120 seconds and
made progress at every one of 60 checkpoints. Its fast client received 118
statistics and 473 commit events with no reset; retained replay resumed, stale
replay reset, the unread client disconnected, and the server stopped cleanly.
These are functional workload observations, not a throughput benchmark.

The actual full-filesystem test exhausted its private 16 MiB tmpfs after 3,968
acknowledged 4,096-byte rows. The failed append returned `indeterminate_commit`,
later appends returned `read_only`, and all acknowledged producer IDs and their
sum remained readable at zero free bytes. After releasing a test-owned reserve,
offline integrity verified 3,968 rows in 31 frames; restart recovered the same
rows and accepted sequence 3,969. Both server stops exited cleanly. This tests
real allocation failure without filling the host disk.

## Scope

The post-admission-change run also passed on both platforms: **77 tests**,
AddressSanitizer and UndefinedBehaviorSanitizer. The six API and six server tests
passed ThreadSanitizer on macOS. Linux's repeated 120-second HTTP test committed
770,688 rows with progress at all 60 checkpoints; the fast stream received 119
statistics and 476 commit events without reset. Replay, slow-client closure and
clean shutdown passed. The repeated private-tmpfs test again recovered all 3,968
acknowledged rows and accepted sequence 3,969. Local run evidence is under
`build/linux-append-admission/` and `build/append-admission-*.log`.

Linux containers on a macOS host validate Linux C/POSIX behavior, compilation,
and the container filesystem's process-crash behavior. They do not establish
native Linux hardware capacity or physical power-loss guarantees. See
[failure testing](failure-testing.md) for the failure model. Independent engine
and HTTP capacity measurements are recorded separately in the
[release report](benchmarks/release-2026-09-28.md). The final benchmark driver also
passed all five contract tests on Linux and macOS, including pinned retention,
deletion and post-restart sequence continuity.
