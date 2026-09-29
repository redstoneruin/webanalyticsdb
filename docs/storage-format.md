# Storage format version 1

The segment, frame, catalog, schema, manifest, and index codecs implement this format.
The format is not yet a compatibility promise for production data.

All integers are little-endian. Signed integers use two's-complement encoding.
Timestamps are signed Unix microseconds. Ingestion timestamps in segments must be
nonnegative. Floating-point slots use IEEE-754 binary32/binary64; nonfinite values
are rejected. Reserved bytes, null payloads, and unused padding must be zero.

Checksums are CRC-32C (Castagnoli), reflected polynomial `0x82f63b78`, initial
state `0xffffffff`, final complement. The standard `123456789` vector is
`0xe3069283`. Checksums detect accidental corruption; they are not authentication.

## Row layout

Each table has one fixed-width schema. `_time` always occupies bytes 0–7. If any
user fields are nullable, a bitmap follows: bit 0 is the first nullable field,
least significant bit first, with 1 meaning null. The bitmap contains
`ceil(nullable_fields / 8)` bytes. Unused high bits must be zero.

User field slots follow the bitmap in declaration order, without native alignment
padding. Final zero padding rounds the row width to a multiple of eight bytes.
The maximum width is 16,384 bytes; at most 255 user fields accompany `_time`.

| Type | Slot bytes | Representation |
| --- | ---: | --- |
| i8/u8/bool | 1 | Boolean is exactly 0 or 1 |
| i16/u16 | 2 | Integer |
| i32/u32/f32/symbol32 | 4 | Symbol IDs start at 1 |
| i64/u64/f64/timestamp_us | 8 | Integer or float |
| uuid | 16 | Opaque bytes in caller-provided order |
| bytes[N] | N | Exactly N bytes |
| text[N] | 4 + N | u32 UTF-8 byte length, payload, zero padding |

The schema fingerprint is 64-bit FNV-1a, initialized to
`14695981039346656037`. Feed every field, including `_time`, in order: UTF-8/ASCII
name including its terminal zero, then three little-endian u32 values for the
type tag, size, and nullable flag. Type tags are the `wadb_type` enum values in
`include/wadb.h`. The fingerprint is a consistency check; durable schema metadata
must also be validated and compiled before row decoding.

## Segment header: 128 bytes

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | ASCII `WADBSEG1` |
| 8 | 4 | Format version = 1 |
| 12 | 4 | Header size = 128 |
| 16 | 8 | Table ID, nonzero |
| 24 | 8 | Segment ID, nonzero |
| 32 | 8 | Schema fingerprint |
| 40 | 4 | Row width |
| 44 | 4 | Reserved zero |
| 48 | 8 | UTC day start, a nonnegative multiple of 86,400,000,000 |
| 56 | 8 | First row sequence, nonzero |
| 64 | 60 | Reserved zero |
| 124 | 4 | CRC-32C of bytes [0,124) |

The immutable header is followed by consecutive frames. No row or frame crosses
a segment boundary. Directory/manifest installation must be durable before rows
in a newly created segment can be acknowledged.

## Frame header: 80 bytes

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | ASCII `WADBBAT1` |
| 8 | 4 | Total frame bytes, including header and trailer |
| 12 | 4 | Header size = 80 |
| 16 | 4 | Row count, nonzero |
| 20 | 4 | Row width, equal to segment/schema width |
| 24 | 8 | First sequence, nonzero |
| 32 | 8 | Minimum ingestion timestamp |
| 40 | 8 | Maximum ingestion timestamp |
| 48 | 4 | Dictionary section size, padded to a multiple of 8 |
| 52 | 4 | Dictionary definition count |
| 56 | 4 | Row payload offset = 80 + dictionary size |
| 60 | 4 | Reserved zero |
| 64 | 8 | Schema fingerprint |
| 72 | 4 | CRC-32C of header bytes [0,72) |
| 76 | 4 | Reserved zero |

The header checksum permits bounded length validation before allocating/reading
the payload. A frame is at most 1,048,576 bytes. Checked arithmetic must establish:

```text
total_bytes = 80 + dictionary_bytes + row_count * row_width + 16
last_sequence = first_sequence + row_count - 1
```

Rows must be ordered by `_time`, with the first/last values matching the header's
minimum/maximum. All timestamps belong to the same UTC day as the segment.
Sequence ranges must be contiguous within a table. Request boundaries never
cross frames; multiple requests may share a frame.

## Dictionary section

Each definition consists of a u16 schema field index (1–255), a zero u16,
a nonzero u32 symbol ID, a u32 string byte length, and that many UTF-8 bytes.
Definitions are packed; only the end of the entire dictionary section has zero
padding to an eight-byte boundary. Each string is at most 65,536 bytes. Zero
definitions means a zero-length section.

Definitions are scoped to one column in one segment. The storage engine must
validate field types, ID continuity, duplicate definitions, and every row's ID
reference against the dictionary state. Generic framing validation only verifies
section lengths, encoding, field-number bounds, and reserved bytes.

## Frame trailer: 16 bytes

| Relative offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | ASCII `WADBEND1` |
| 8 | 4 | Repeated total frame length |
| 12 | 4 | CRC-32C of the entire frame except these last four bytes |

The CRC covers header, dictionary, rows, trailer magic, and repeated length.
Complete bytes alone do not imply that a client received an acknowledgement.
Publication to readers and successful append responses must follow durable file
synchronization. An incomplete final frame may be a recoverable interrupted
append; full-size checksum failure is corruption and must not silently discard
previously acknowledged rows.

## Metadata snapshots

Every metadata file ends with a u32 CRC-32C covering all preceding bytes. Files
are installed by temporary-file write, full file synchronization, rename, and
directory synchronization. Reserved bytes are zero. Numeric IDs and names in
different files must agree; referenced metadata must exist. An absent catalog
in a nonempty tables directory is an error, not permission to reset the database.

The `catalog` has a 32-byte header, sorted u64 table IDs, and its checksum:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `WADBCAT1` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Header bytes = 32 |
| 16 | 4 | Table count, at most 1024 |
| 20 | 4 | Reserved |
| 24 | 8 | Next table ID, greater than every referenced ID |

Each table has an immutable `schema` file: 128-byte header, one 80-byte record
per user field, then checksum. `_time` is implicit in schema records.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `WADBSCH1` |
| 8 | 4 | Version = 1 |
| 12 | 4 | User field count |
| 16 | 8 | Table ID |
| 24 | 8 | Compiled schema fingerprint |
| 32 | 4 | Compiled row width |
| 36 | 4 | Null bitmap bytes |
| 40 | 64 | Zero-terminated table name, zero-padded |
| 104 | 24 | Reserved |

Each field record is a 64-byte zero-padded name, u32 type tag, u32 size, u32
nullable flag (0 or 1), and four reserved bytes. Readers recompile the schema
and compare its fingerprint, row width, and bitmap size with the header.

Each table's `manifest` is an 80-byte header, 64-byte segment records sorted by
ID, then checksum. It describes lifecycle transitions, not every append.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `WADBMAN1` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Header bytes = 80 |
| 16 | 8 | Table ID |
| 24 | 8 | Next segment ID |
| 32 | 8 | Sequence watermark preserved through retention |
| 40 | 8 | Ingestion time watermark preserved through retention |
| 48 | 8 | Retention duration in microseconds; zero disables expiry |
| 56 | 8 | Nonzero manifest generation |
| 64 | 4 | Segment record count, at most 1,000,000 |
| 68 | 12 | Reserved |

| Segment record offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | Segment ID |
| 8 | 8 | UTC day start |
| 16 | 8 | First sequence |
| 24 | 8 | Last sequence; zero when empty |
| 32 | 8 | Minimum time; zero when empty |
| 40 | 8 | Maximum time; zero when empty |
| 48 | 8 | Committed file bytes at manifest publication |
| 56 | 4 | State: 1 active, 2 sealed, 3 retired |
| 60 | 4 | Reserved |

At most the final segment is active. Its persisted byte boundary can lag its
complete durable frames; recovery validates and advances that boundary. Sealed
segment length and summary must exactly match the manifest. A new segment is
durably created before a manifest can reference it. Rotation atomically changes
the previous active segment to sealed and installs the new active segment.
Unreferenced files/directories are excluded from the live database and their IDs
are skipped on creation, preserving evidence from interrupted operations.

Retention publishes state 3 and the highest retired sequence/time together before
unlinking any files. Reader snapshots keep retired files pinned. Startup never
reimports retired files and completes pending deletion. An expired active segment
can transition directly to retired under the writer lock: its complete committed
boundary and final watermarks become durable in the same manifest. New appends
create a new active segment. Retired manifest records currently remain as
permanent tombstones and count toward the one-million-record limit.

## Derived index

A sealed segment's `.idx` contains a 128-byte header, one 64-byte record per
committed frame, and a complete segment dictionary checkpoint. Active indexes
stay in memory and are reconstructed from committed frames on restart. Rotate
before exceeding 65,536 frames per segment, even if the byte target is not reached.

| Header offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `WADBIDX1` |
| 8 | 4 | Version = 1 |
| 12 | 4 | Header size = 128 |
| 16 | 8 | Table ID |
| 24 | 8 | Segment ID |
| 32 | 8 | Schema fingerprint |
| 40 | 8 | Covered segment byte length |
| 48 | 8 | Frame record count, at most 65,536 |
| 56 | 8 | Dictionary offset = 128 + count * 64 |
| 64 | 8 | Padded dictionary byte length, at most 64 MiB |
| 72 | 4 | Dictionary definition count |
| 76 | 4 | Reserved |
| 80 | 8 | Segment minimum time |
| 88 | 8 | Segment maximum time |
| 96 | 8 | First sequence |
| 104 | 8 | Last sequence |
| 112 | 4 | Row width |
| 116 | 4 | CRC-32C of complete dictionary section |
| 120 | 4 | Reserved |
| 124 | 4 | CRC-32C of header bytes [0,124) |

| Frame record offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | Frame minimum time |
| 8 | 8 | Frame maximum time |
| 16 | 8 | First sequence |
| 24 | 4 | Row count |
| 28 | 4 | New dictionary definition count |
| 32 | 8 | Frame byte offset in segment |
| 40 | 4 | Complete frame byte length |
| 44 | 4 | Row payload offset relative to frame |
| 48 | 4 | Source frame checksum, copied from its trailer |
| 52 | 4 | CRC-32C of this frame's padded dictionary section |
| 56 | 4 | Reserved |
| 60 | 4 | CRC-32C of record bytes [0,60) |

The checkpoint uses the frame dictionary encoding, with all definitions in
original insertion order followed by fewer than eight zero padding bytes. IDs
remain scoped to the segment and column. Readers verify header identity and
endpoints, page index records in 4 KiB blocks, and validate record checksums when
accessed. Missing or damaged indexes rebuild from validated source frames.

Index installation uses atomic durable replacement, but indexes remain derived:
if installation fails because storage is unavailable, a bounded transient index
can serve the query. Rebuilding during a query consumes that query's scan-byte
and time budgets. A source checksum failure fails the read and stops table
writes; it is never repaired by replacing an index. Full integrity checks bypass
indexes and caches and validate every retained committed frame and row.
