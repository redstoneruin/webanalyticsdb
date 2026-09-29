# Query and lifecycle API

The C17 API is in `include/wadb.h`. Handles belong to the database; callers must
finish concurrent operations before `wadb_close`. Schemas remain immutable.
`wadb_query` returns owned cells and byte strings, released by `wadb_result_free`.
`wadb_scan` is the lower-level callback API; its byte values are borrowed only
until the callback returns. Query results remain valid after database close.

## Structured queries

Start with `wadb_query_options_default`, then set the desired fields. Time ranges
are half-open ingestion intervals `[start_time, end_time)`. Projection defaults
to every field including `_time`. Scan output follows sequence order. A limit
counts matching output rows, not rows inspected before filtering.

```c
wadb_query_options query;
wadb_query_options_default(&query);
query.scan.start_time = start_us;
query.scan.end_time = end_us;
query.scan.limit = 20;

wadb_filter filter = {"site_id", WADB_EQ, {.as.u64 = 42}};
const char *groups[] = {"path"};
wadb_aggregate aggregates[] = {
    {WADB_COUNT_ALL, NULL, "views"},
    {WADB_AVG, "duration_ms", "average_duration_ms"}
};
query.filters = &filter;
query.filter_count = 1;
query.group_by = groups;
query.group_count = 1;
query.aggregates = aggregates;
query.aggregate_count = 2;
query.order_by = "views";
query.descending = true;

wadb_result *result = NULL;
wadb_status status = wadb_query(table, &query, &result, &error);
if (status == WADB_OK) {
    /* result->values[row * result->column_count + column] */
    wadb_result_free(result);
}
```

Filters are conjunctions of typed `EQ`, `NE`, `LT`, `LE`, `GT`, `GE`, `IS_NULL`,
or `IS_NOT_NULL`. The field determines the value union member. Out-of-range,
nonfinite, over-capacity, and invalid UTF-8 values are rejected. Float32 filter
constants are rounded to float32, matching stored values. Ordinary comparisons
never match null; use an explicit null operator. Byte strings compare in byte
order and symbols compare by their strings across segment dictionaries.

Aggregates require unique output names. Group columns precede aggregate columns.
If buckets are enabled, `_bucket` precedes both. Projection cannot be mixed with
aggregates. `COUNT_ALL` counts matching rows; `COUNT(field)` counts non-null
values. `SUM`, `MIN`, `MAX`, and `AVG` currently require numeric fields and ignore
nulls. Counts return u64. Integer sums return checked i64/u64; floating sums and
averages return finite f64. Integer overflow or nonfinite results return
`WADB_LIMIT`; failed aggregate queries return no partial result. Floating
averages use a weighted online calculation and remain approximate floating-point
results. MIN/MAX retain the field's numeric type. Empty counts are zero; empty
sums, minima, maxima, and averages are null.

`bucket_width_us` specifies a positive fixed duration; the default origin is
Unix epoch and `bucket_origin_us` can change it. Bucket starts are mathematically
floored, including negative boundaries. A bucket overlaps the requested range
but never includes out-of-range rows. Queries with buckets and no dimensions
emit empty intervals; queries with dimensions emit observed combinations only.
An empty ungrouped, unbucketed aggregate returns one row. An empty grouped or
bucketed interval returns no rows. Boundaries that cannot fit i64 fail explicitly.

Group output defaults to first occurrence. `order_by` selects an output column;
nulls sort last in both directions and ties keep occurrence order. Set
`order_by = "_bucket"` for time-ordered grouped buckets. The row limit is applied
after aggregation and sorting; `total_groups` reports the exact pre-limit count.
It is not a limit on the source rows contributing to aggregates.

## Bounds and snapshots

Defaults are 10,000 output rows, 10,000 groups, 10,000 buckets, 64 MiB of query
memory, 64 MiB of scan work, and a 30-second execution deadline. High cardinality
fails with `WADB_LIMIT` rather than silently approximating. Memory accounting
includes the compiled plan, group/hash buffers, result cells, and copied strings;
it excludes allocator overhead, operating-system cache, and the separate engine
snapshot/dictionary budgets. Deadlines are checked between I/O/processing steps;
an in-progress filesystem call is not forcibly interrupted.

Scan work charges complete candidate frames, decoded dictionary sections, and
source bytes read to rebuild indexes. Repeated page reads count again against a
cursor's shared budget; cache hits do not waive the logical dictionary charge.
Index metadata pages are bounded separately and reported as `index_bytes`.
`rebuild_bytes`, `dictionary_bytes`, `frames_scanned`, and `segments_scanned`
explain additional work. `rows_scanned` includes rows excluded by time bounds or
filters. `matched_rows` reports rows passing predicates. A narrow numeric query
with an intact index reads its relevant frame instead of replaying earlier rows.

The database defaults to 32 concurrent pinned snapshots, 64 MiB of snapshot
metadata, and a 64 MiB sealed-dictionary cache. A full pinned cache or exhausted
reader reservation returns `WADB_BACKPRESSURE`. Active reader dictionaries are
private and bounded by the configured segment dictionary limit. Index readers
use one 4 KiB metadata page each, or a bounded transient rebuilt index. All limits
are available in the public option structs where applicable.

## Pagination

Use `wadb_cursor_open(table, &query, ttl_ms, &id, &error)` for a scan query, then
`wadb_cursor_next(db, id, &result, &error)` until `has_more` is false. TTL is
absolute and at most five minutes. The cursor copies filters/projection and
pins the original committed snapshot. Later appends never appear in its pages,
even if timestamps are equal or segments rotate. Aggregates use `wadb_query`.

A page exactly filling the limit may conservatively report `has_more` even if
later rows will all be filtered out; its next page can be empty. Finished cursors
close automatically. Call `wadb_cursor_close` when abandoning a query. Expired
or closed IDs return `WADB_EXPIRED`; they never silently start a new snapshot.
A concurrent next/close on a busy cursor returns backpressure. Call
`wadb_cursor_expire` from a periodic server maintenance loop; query/scan/cursor
entry points also reap expired cursors. IDs are valid only within one database
open and need a server boot identity when exposed over HTTP.

## Retention and integrity

`wadb_set_retention` persists a duration in microseconds; zero disables retirement.
Preview and apply use an explicit nonnegative Unix-microsecond `now`. A segment
is eligible only when its maximum time is strictly below `now - retention_us`.
Partially expired segments stay intact and are reported separately. An idle
active segment can be retired once every committed row qualifies.

Apply first durably records retired states and sequence/time watermarks. New
readers immediately omit those segments. Existing snapshots, including cursors,
keep their files until the table's last pinned reader releases them. This
initial implementation uses conservative table-wide pins. The last release
attempts deletion; startup or another retention apply retries failures. Pending
segment counts are conservative after a partial deletion failure. Empty day
directories and manifest tombstones are retained. Watermarks survive retirement
of every row, so a restart cannot reuse sequences or move ingestion time back.

`wadb_integrity_check` checks all retained committed source data independently
of indexes and dictionary caches. Its default limit is 1 GiB and 60 seconds;
callers can increase the budget or provide a cancellation callback. It returns
verified segment/frame/row counts and bytes inspected, including partial progress
on error. Corruption leaves source bytes intact and makes the table read-only.
An integrity check detects damage; it does not repair lost durable data.

## Monitoring

`wadb_get_stats` returns counters for this database open, queue/cache/snapshot
usage, approximate latency percentiles and filesystem free space. Append latency
runs from validation entry to the writer making its result ready; successful
requests populate that histogram. Sync latency measures the row-file sync call,
including failed calls. Query latency measures executed scans/pages, including
failures; it does not include HTTP parsing or a future server worker queue.
Percentiles are upper bounds of logarithmic microsecond bins, not exact samples.

`wadb_get_table_stats` returns retained source rows/bytes, segment counts,
watermarks, published dictionary memory and health flags. Source byte counts
include segment headers and committed frames, excluding derived index files,
orphan files and allocator overhead. Retention updates totals at publication;
restart reconstructs them from metadata/recovered tails. `wadb_list_segments`
provides bounded pages of segment metadata including retired tombstones.

`wadb_read_notifications` reads a bounded ring of 1,024 committed-frame metadata
records. Publishing a commit never waits for a monitoring client. Overwritten
records produce `gap=true`; committed database rows are unaffected. Notifications
are published only after durable acknowledgement conditions are satisfied. Ring
IDs and operational counters restart on each open. The HTTP layer must add a
boot identity, periodically send snapshots, and keep its sampled event feed
separate from exact persisted-tail queries.
