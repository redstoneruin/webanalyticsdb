# WebAnalyticsDB

Basic web analytics database.

## Setup

Prereqs: `gcc`, `make`, `ar`.

## Build

```bash
make
```

Gets you:
- `build/webanalyticsdb`: Main app.
- `build/test*`: Test binaries.
- `build/libwebanalyticsdb.a`: Static lib for core logic.

## Test

```bash
make test
```

Runs all tests in `build/`. Check output for pass/fail.

## Clean

```bash
make clean
```

Nukes `build/`.
