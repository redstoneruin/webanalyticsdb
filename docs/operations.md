# Operation, backup and restore

The database directory is exclusively owned by one process. Do not run a second
server or `check` against a live directory. Tables are immutable schemas; changing
a schema means creating a new table. Retention is explicit and removes only fully
expired segments. See the [HTTP guide](http-api.md) for administration.

## Stopped backup

1. Stop ingestion clients so their pending requests can finish. Send SIGTERM to
   the database process (or Ctrl-C in its terminal) and wait for exit status 0.
   An interrupted/indeterminate client response may still represent a commit.
2. While the server is stopped, run `build/webanalyticsdb check --data SOURCE`.
   This acquires the database lock and validates all retained source frames. Its
   defaults are 1 GiB and 60 seconds per table; explicitly increase
   `--max-scan-bytes` and `--timeout-ms` for a larger database. A limit error is
   incomplete verification, not a successful check.
3. Copy the entire directory into a **new, separate backup directory**, preserving
   filenames, permissions and hierarchy. Include catalog, per-table schema and
   manifest files, all partition/segment files, and derived indexes. Do not copy
   only `.seg` files. The inert LOCK file may be copied; the operating system lock
   is not a property carried into the copy.
4. Keep the source stopped until the copy is complete. Verify the copied files
   (for example, compare cryptographic digests) and retain the original directory.
   Store a backup off the original device if it must survive device failure.

This is a stopped-copy procedure. Copying a live directory is not a supported
consistent online backup protocol. Filesystem snapshots require their own tested
coordination procedure. No replication or automatic damaged-data repair exists.

The default admin token is inside the data directory and therefore is part of a
full copy. Treat backup access as credential access. A restored server can use a
new `--token-file` outside the copied directory to create a fresh local admin token;
do not expose the old token. Token files must retain owner-only permissions.

## Restore

1. Restore into a new directory. Leave both the original and backup intact.
2. Run `build/webanalyticsdb check --data RESTORED` with sufficient scan/time
   limits. Unsupported formats, corrupt checksummed metadata, and complete corrupt
   frames fail closed. Preserve the files when investigating a failure.
3. Start `build/webanalyticsdb serve --data RESTORED --port UNUSED_PORT`. Use a
   fresh token file if rotating credentials. Sessions, job records, boot identity
   and cursor IDs from the prior server are invalid after restart.
4. Compare table names, immutable schemas, retained counts, sequence watermarks
   and representative query results to the backup record. Before switching
   producers, verify that a new append continues the expected sequence.
5. Stop the verification server cleanly and only then switch the production
   service/clients to the restored directory. Never start two owners on one path.

A restore that opens successfully is not proof that every historical row was
checked; use the full integrity command with sufficient limits. Recovery can
truncate an incomplete final frame, and a complete uncertain commit can survive.
Media corruption and loss of previously durable files require restoration from a
good copy; derived indexes alone are rebuildable.

## Verified development exercise

On September 28, 2026, the macOS development build completed this isolated test:

- Gracefully stopped a two-table database with 8,803 retained rows.
- Offline integrity verified `browser_views`: 3 rows, 1 frame, 312 source bytes;
  `browser_load`: 8,800 rows, 88 frames, 435,576 source bytes.
- Copied the stopped directory to a new temporary location and compared SHA-256
  digests of every catalog/schema/manifest/segment file.
- Ran full integrity successfully against the restored copy.
- Reopened the restored server and compared table counts, schema identities,
  sequence watermarks, and exact projected rows from the sample table.
- Appended another row to `browser_views`, durably receiving sequence 4, then
  gracefully stopped with exit status 0. The original remained unchanged.

The local run log is `build/restore-exercise.log`. This small restore exercise
validates the procedure; it does not establish large-dataset restore time or a
power-loss guarantee for untested devices/filesystems.
