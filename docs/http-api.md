# HTTP server and console

Build with the pinned dependencies in [dependencies.md](dependencies.md), then:

```sh
make
./build/webanalyticsdb serve --data ./data --port 8080
```

Open the exact origin printed at startup (normally `http://127.0.0.1:8080`). The
server creates `data/admin.token` with mode 0600 if it does not exist. Read that
file locally and enter the token in the console. `--token-file` overrides its
location. Existing token files must be regular, owned by the process user, have
no group/other permissions, and contain 64 lowercase hex characters with an
optional trailing newline. A symlink at the token path is rejected.

`SIGINT` or `SIGTERM` stops new work, finishes queued requests, resumes suspended
HTTP connections, stops monitoring, cancels/joins background jobs, and closes
the database. An in-flight query can delay shutdown up to its query deadline.
No HTTP shutdown endpoint is exposed.

The console assets are embedded in the executable. Editing `web/` requires
rebuilding and restarting the server, then reloading the browser. No frontend
package manager, CDN, or development server is needed.

## Authentication and transport

`POST /api/v1/session` accepts `{"token":"…"}` and exchanges the admin token for
a random HttpOnly, SameSite=Strict session cookie. Its response contains a CSRF
token; the console keeps that token only in memory. Sessions expire after 12
hours and are local to the current server process. At most 32 sessions are kept;
a new login evicts the session with the earliest expiry when all slots are full.
Login attempts are limited to ten per second across the process.

`GET /api/v1/session` returns the current session's CSRF token. Every authenticated
POST requires `X-CSRF-Token`; `POST /api/v1/logout` revokes that session. Non-browser
clients may instead send `Authorization: Bearer <admin-token>` on each request;
that authentication method does not require a CSRF header. Tokens are never
accepted in query strings. Except for the assets and session login, all endpoints,
including health and statistics, require authentication.

The Host header must match the configured origin's authority. An Origin header,
when present, must match the origin exactly; cross-site Fetch Metadata requests
are rejected. CORS is not enabled. Browser content uses a restrictive Content
Security Policy and renders field values as text.

The default listener is numeric IPv4 loopback. A non-loopback `--bind` requires
an explicit HTTPS `--origin`, for example an operator-managed TLS reverse proxy
with a private HTTP backend. The server itself does not terminate TLS. Keep the
backend inaccessible to public clients and have the proxy preserve the public
Host header, disable buffering for the SSE path, and allow long-lived streams.
Do not put credentials in URLs. Changes to the token file take effect after
restarting the process; restart also invalidates browser sessions and cursors.

## JSON and endpoint contract

Requests are strict JSON objects with unique keys. Unknown arguments are rejected.
POST requests use `Content-Type: application/json` (also accepts the exact
`application/json; charset=utf-8` form). GET arguments belong in the query string;
GET bodies and POST query arguments are rejected. Names follow the C API's ASCII
identifier rules. Integers outside JavaScript's exact range must be decimal
strings. Metadata counters and sequence/timestamp values are returned as decimal
strings; small field integers and finite floats use JSON numbers. Fixed bytes and
UUID values use hex strings. Text/symbol values preserve embedded zero characters.

Success responses contain `ok: true` and `boot_id`. Application errors contain
`{"ok":false,"error":{"code":"…","message":"…"}}`. Validation uses 400,
missing resources 404, conflicts 409, expired cursors 410, exceeded query limits
422, and retryable admission backpressure 429. I/O/corruption/read-only failures
use 503 with the specific engine code. Authentication uses 401, origin rejection
403, oversized declared bodies 413, and unsupported request media 415.

A successful append response is durable. An I/O error marked `indeterminate_commit`, a
network disconnect, a timeout, or failure to serialize/deliver the receipt can
leave committed rows without a received success response. Retrying can duplicate
records. There is no persistent idempotency contract. Inspect the persisted tail
and producer-side identifiers when reconciling uncertain outcomes.

| Method / path | Request / result |
| --- | --- |
| GET `/api/v1/health` | Ready/read-only state |
| GET `/api/v1/stats` | Counters, latency histograms, limits, boot identity and feed cursor |
| GET `/api/v1/tables` | Retained table totals, no historical data scan |
| POST `/api/v1/tables` | `name`, `fields`, optional `retention_us`; returns schema and row width |
| GET `/api/v1/tables/{id}` | Schema, retention and storage totals |
| POST `/api/v1/tables/{id}/append` | `rows`: objects or arrays in user-field order, excluding `_time` |
| POST `/api/v1/tables/{id}/query` | Structured query described below |
| GET `/api/v1/tables/{id}/tail` | `after_sequence`, `limit`; omitting after_sequence selects recent rows |
| GET `/api/v1/tables/{id}/segments` | `after_id`, `limit`; increasing segment-ID page including tombstones |
| POST `/api/v1/cursors/{id}/next` | `boot_id`; returns the next page of the same snapshot |
| POST `/api/v1/cursors/{id}/close` | `boot_id`; releases the snapshot |
| POST `/api/v1/tables/{id}/retention-preview` | Optional `now_us`; previews the saved policy |
| POST `/api/v1/tables/{id}/retention` | Optional `retention_us`, `now_us`, `apply` (default false) |
| POST `/api/v1/tables/{id}/integrity-check` | Optional byte/time budgets; returns a job ID (202) |
| POST `/api/v1/test-jobs` | Creates a dedicated table and generator job (202) |
| GET `/api/v1/jobs` | Up to 32 recent jobs, including integrity results |
| GET `/api/v1/jobs/{id}` | State and counters for a job |
| POST `/api/v1/jobs/{id}/stop` | Requests cooperative cancellation |
| GET `/api/v1/stream` | Authenticated SSE described below |

`/test-jobs` and `/test-jobs/{id}` also support GET; `/test-jobs/{id}/stop` aliases
the job stop endpoint. Jobs and job IDs are process-local. A generator always
creates a new table; an existing name is a conflict. Test tables remain after the
job completes or the process restarts. Histories are bounded and older finished
job records can disappear. A failed job can leave its newly created empty table.

Example table and append bodies:

```json
{"name":"page_views","fields":[{"name":"site_id","type":"u32"},{"name":"path","type":"symbol32"},{"name":"occurred_at","type":"timestamp_us","nullable":true}]}
```

```json
{"rows":[{"site_id":1,"path":"/","occurred_at":"1790630000000000"}]}
```

Query arguments map to the [C query contract](query-api.md): `start_us`, `end_us`,
`after_sequence`, `limit`, `max_scan_bytes`, `timeout_ms`, `projection`, `filters`,
`group_by`, `aggregates`, `bucket_width_us`, `bucket_origin_us`, `max_groups`,
`max_buckets`, `memory_limit_bytes`, `order_by`, `descending`, `cursor`, `ttl_ms`.
Filter operators are `eq`, `ne`, `lt`, `le`, `gt`, `ge`, `is_null`, `is_not_null`.
Aggregate operators are `count_all`, `count`, `sum`, `min`, `max`, `avg`, each with
a result `name`; all but count_all also require `field`.

```json
{"group_by":["path"],"filters":[{"field":"site_id","op":"eq","value":1}],"aggregates":[{"op":"count_all","name":"views"}],"order_by":"views","descending":true,"limit":20}
```

Raw scan queries can request `cursor:true`. The first response already contains
a page. Pass its cursor ID and boot identity to next/close. Pages share one
snapshot and scan-byte budget. Default TTL is 60 seconds, maximum five minutes;
TTL is absolute and not extended by reading pages. A page request advances the
cursor even if its network response is lost. Restart the query to reconcile a
lost page. Aggregate queries cannot use cursors. `has_more` is conservative: the
final page can be empty. Export buttons export only the displayed page/result.
CSV export prefixes text that could be interpreted as a spreadsheet formula;
JSON export preserves the exact returned strings.

Retention preview and apply are separate operations. The console saves a policy,
previews with an explicit time, then applies the same cutoff after confirmation.
It checks for a changed policy first; this is not a transactional compare-and-swap
against another administrator changing policy concurrently. Source row mutation
remains append-only; retention removes whole segments and preserves watermarks.

## Resource limits and monitoring

| Resource | Bound |
| --- | ---: |
| HTTP connections / per-connection parser memory | 128 / 64 KiB |
| Connection inactivity timeout | 15 seconds |
| Database HTTP workers / work queue entries | 4 / 32 |
| Request body / global upload capacity | 4 MiB / 64 MiB |
| JSON allocation / decoded append values per worker | 32 MiB / 32 MiB |
| Serialized response / outstanding response reservations | 4 MiB general, 4 KiB append receipts / 32 MiB total |
| HTTP query scratch / source scan / deadline / output rows | 64 MiB / 1 GiB / 30 seconds / 10,000 |
| SSE clients / message ring / message bytes | 8 / 128 / 32 KiB |
| Background jobs / retained job records | 2 / 32 |
| Generator rate / duration / batch / cardinality / offered rows | 1M/s / 5 min / 4,096 / 100K / 10M |
| Integrity job source bytes / deadline | 64 GiB / 5 min |

Response space is reserved before dispatching database work. Appends reserve an
enforced 4 KiB bound for their numeric receipt or bounded error; other endpoints
reserve the general 4 MiB bound. This can reject
admission before the nominal 32-entry queue fills (four-MiB reservations plus
space for errors). Slow clients retain their reservations until the HTTP library
releases the response. Partial/streaming uploads that exceed their byte budget
are disconnected immediately before dispatch; a known excessive Content-Length
gets a 413 response. Memory accounting is bounded but the listed categories are
separate budgets; their sum is not a single global process RSS limit.

Fetch `/stats` first and subscribe to `/stream?boot=<boot_id>&after=<feed_cursor>`.
SSE IDs have the form `boot_id:message_id`. EventSource's `Last-Event-ID` takes
precedence on reconnect. The server replays retained messages; a stale cursor,
different boot, or lost ring suffix produces a `reset`. The console fetches a
new snapshot on reset. Stats are published approximately every second; commit
metadata is batched at approximately 250 ms. These intervals are unrelated to
write durability. A connection that falls behind receives a reset and closes;
idle/stalled clients time out. Stream sockets request a 16 KiB send buffer;
the operating system can round or adjust that size. HTTP parsing failures can
use the HTTP library's own error body instead of the JSON application envelope. The writer never waits for a monitoring client.

SSE commit messages carry table IDs and sequence ranges, not row payloads. When
the Live events page is visible, the console fetches at most 20 recent persisted
rows per change notification with at most one request in flight and a 250 ms
minimum timer interval. It deduplicates sequences, keeps at most 100 displayed
rows and reports skipped sequence gaps. Pausing/resuming also reports rows
skipped during the pause. This is a sampled observability view; use the exact
persisted-tail/query API to inspect every row. Table metadata refreshes every
five seconds; overview counters do not count rows by scanning the data.

Generator jobs pace an offered row rate but may finish below that target when
the server is saturated. Counters distinguish attempted rows, durable commits,
and rejected batches. CPU nanoseconds measure the generator/job thread only,
not writer or HTTP/query threads. Job throughput shown in the console is not a
capacity benchmark. Use an independent process for the release measurements.

## Verification tools

`make test` includes socket-level authentication, origins/CSRF, request limits,
exact API results, SSE reconnection, disconnected keepalive streams, active-job
shutdown, and overload rejection before mutation. `make sanitize` repeats these
under AddressSanitizer and UndefinedBehaviorSanitizer.

For the longer lifecycle check, run:

```sh
make http-smoke
```

`tools/http-stress-smoke.py` starts its own server on an ephemeral loopback port,
uses only a temporary database, and runs for two minutes to fill OS buffers. It
verifies ongoing durable writes, a continuously reading SSE client, stale and
retained replay, slow-client disconnection, and clean shutdown. It removes its
own temporary data afterward. Results are written to `build/http-stress-smoke.json`.
This test uses the in-process generator and is not a throughput capacity report.
