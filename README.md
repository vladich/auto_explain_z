# auto_explain_z

`auto_explain_z` is a PostgreSQL extension for logging execution plans to a
separate compressed binary log instead of PostgreSQL's regular text log.

It is designed for installations that need to keep far more execution plans
than `auto_explain` usually makes practical. The extension keeps plan logging
off the regular log path, serializes common plan fields into compact binary
records, optionally stores repeated plan shapes as templates, and compresses
serialized data with a file-level compression setting.

## Highlights

- Separate `.aez` binary logs; plan payloads are not written to regular
  PostgreSQL log files.
- Single coordinated log file when loaded through `shared_preload_libraries`.
- Binary serialization profiles:
  - `simple`: compact baseline suitable for low-overhead text-style analysis.
  - `full`: adds richer EXPLAIN detail fields for JSON/YAML/XML-style output.
- File compression: `none`, `lz4`, and `zstd`, depending on the PostgreSQL
  build options.
- Plan template cache keyed by query id and plan shape, with a plan identifier
  stored on every record.
- Preserves useful log context: timestamp, backend pid and type, database,
  user, authenticated user, application name, client address, duration, query
  id, query text, and parameters when enabled.
- Decoder can render logged plans as `text`, `json`, `yaml`, or `xml`.
- Log rotation, retention cleanup, and a SQL rotation function are included.

## Requirements

- PostgreSQL server headers and PGXS for the target PostgreSQL installation.
- PostgreSQL built with `--with-lz4` to use `auto_explain_z.compression = lz4`.
- PostgreSQL built with `--with-zstd` to use
  `auto_explain_z.compression = zstd`.
- Python 3 for the benchmark scripts under `bench/`.
- The native `auto_explain_z_dump` decoder is built by `make`. Decoding
  compressed files requires building against a PostgreSQL installation with
  the corresponding `--with-lz4` or `--with-zstd` support.

## Installation

Build and install the extension against the target PostgreSQL installation:

```sh
make PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

Install the SQL objects in each database where you want the helper function:

```sql
CREATE EXTENSION auto_explain_z;
```

`CREATE EXTENSION` loads the module in the current session and installs
`auto_explain_z_rotate_logfile()`. For production logging, configure
`shared_preload_libraries` and restart PostgreSQL.

## Quick Start

Add the extension to `postgresql.conf`:

```conf
shared_preload_libraries = 'auto_explain_z'

auto_explain_z.log_min_duration = '100ms'
auto_explain_z.sample_rate = 0.10
auto_explain_z.profile = 'simple'
auto_explain_z.compression = 'lz4'
auto_explain_z.directory = 'auto_explain_z'
auto_explain_z.pending_buffer_size = '1MB'

auto_explain_z.template_cache = on
auto_explain_z.max_templates = 4096

auto_explain_z.log_rotation_age = '1d'
auto_explain_z.log_rotation_size = '1GB'
auto_explain_z.retention_max_files = 100
auto_explain_z.retention_max_size = '100GB'
```

If `shared_preload_libraries` already contains other libraries, append
`auto_explain_z` to the existing comma-separated list.

Restart PostgreSQL after changing `shared_preload_libraries`.

Relative `auto_explain_z.directory` values are resolved under the PostgreSQL
data directory. The PostgreSQL server process must be able to create and write
that directory.

For a controlled test session, you can also load the module manually:

```sql
LOAD 'auto_explain_z';
SET auto_explain_z.log_min_duration = 0;
SELECT count(*) FROM pg_class;
```

Manual `LOAD` is useful for development, but it does not use the shared
single-log coordination path. Use `shared_preload_libraries` for production.

## Configuration

Core logging settings:

| Setting | Default | Notes |
|---|---:|---|
| `auto_explain_z.log_min_duration` | `-1` | Minimum statement duration to log. `-1` disables logging; `0` logs all statements. |
| `auto_explain_z.sample_rate` | `1.0` | Fraction of eligible statements to log. |
| `auto_explain_z.profile` | `simple` | `simple` or `full`. |
| `auto_explain_z.compression` | `lz4` if available, else `none` | `none`, `lz4`, or `zstd`, depending on PostgreSQL build support. |
| `auto_explain_z.zstd_level` | `1` | zstd level when zstd compression is selected. |
| `auto_explain_z.log_query_text` | `on` | Store query text in AEZ records. |
| `auto_explain_z.query_text_max_length` | `-1` | Maximum stored query text length. `-1` means unlimited. |
| `auto_explain_z.log_parameter_max_length` | `-1` | Maximum stored parameter value length. `0` disables parameter logging. |
| `auto_explain_z.log_nested_statements` | `off` | Include nested statements. |
| `auto_explain_z.report_writer_errors` | `off` | Report writer failures to the regular PostgreSQL log. Off avoids regular-log traffic from plan logging. |

EXPLAIN detail settings:

| Setting | Default | Notes |
|---|---:|---|
| `auto_explain_z.log_analyze` | `off` | Collect per-node execution statistics. |
| `auto_explain_z.log_verbose` | `off` | Include verbose EXPLAIN fields supported by the selected profile. |
| `auto_explain_z.log_buffers` | `off` | Collect buffer usage. Requires `log_analyze = on`. |
| `auto_explain_z.log_wal` | `off` | Collect WAL usage. Requires `log_analyze = on`. |
| `auto_explain_z.log_timing` | `on` | Collect timing data when analyzing. |

Template settings:

| Setting | Default | Notes |
|---|---:|---|
| `auto_explain_z.template_cache` | `on` | Enable bounded plan template definitions and references. |
| `auto_explain_z.template_fast_path` | `on` | Use query-id template references without hashing the whole plan on every execution when safe. |
| `auto_explain_z.template_fast_path_recheck` | `0` | Recheck interval for the fast path. `0` disables periodic rechecks. |
| `auto_explain_z.template_omit_query_text` | `on` | Store query text on template definitions, then omit it from later references. |
| `auto_explain_z.max_templates` | `4096` | Shared cap when loaded through `shared_preload_libraries`. |
| `auto_explain_z.template_min_plan_bytes` | `0` | Minimum serialized plan size eligible for templating. |

Log file settings:

| Setting | Default | Notes |
|---|---:|---|
| `auto_explain_z.directory` | `auto_explain_z` | Directory for `.aez` files. Relative paths are under `PGDATA`. |
| `auto_explain_z.file_prefix` | `auto_explain_z` | Prefix for generated file names. |
| `auto_explain_z.log_filename` | empty | Optional `strftime` pattern included in generated file names. |
| `auto_explain_z.log_rotation_age` | `24h` | Rotate by age. `0` disables age-based rotation. |
| `auto_explain_z.log_rotation_size` | `1GB` | Rotate by serialized input size. `0` disables size-based rotation. |
| `auto_explain_z.max_file_size` | `1GB` | Deprecated alias for `log_rotation_size`. |
| `auto_explain_z.log_truncate_on_rotation` | `off` | Truncate on time-based rotation name collisions. |
| `auto_explain_z.pending_buffer_size` | `1MB` | Pending per-backend write/compression buffer. `0` flushes every record immediately. |
| `auto_explain_z.retention_max_files` | `0` | Maximum matching `.aez` files to keep. `0` disables file-count cleanup. |
| `auto_explain_z.retention_max_size` | `0` | Maximum total matching `.aez` bytes to keep. `0` disables size cleanup. |
| `auto_explain_z.retention_cleanup_interval` | `60s` | Minimum interval between cleanup scans. |

## Decoding Logs

Decode `.aez` files with `auto_explain_z_dump`:

```sh
auto_explain_z_dump --format text /path/to/file.aez
auto_explain_z_dump --format json /path/to/file.aez
auto_explain_z_dump --format yaml /path/to/file.aez
auto_explain_z_dump --format xml  /path/to/file.aez
```

To reconstruct PostgreSQL `auto_explain`-style log records, use
`--postgres-log`. The decoder stays fully offline. By default it uses
PostgreSQL's default `log_line_prefix` value, `%m [%p] `, with `GMT` timestamps.
Pass the same log-formatting values used by the source server when you need the
reconstructed records to match that server's logs:

```sh
auto_explain_z_dump --postgres-log \
  --log-line-prefix '%m [%p] ' \
  --log-timezone 'America/Los_Angeles' \
  /path/to/file.aez
```

Use `--log-error-verbosity verbose` if the source server logged verbose stderr
records with SQLSTATE after the severity.

`--format` still controls the plan payload format inside the reconstructed log
message:

```sh
auto_explain_z_dump --postgres-log --format json \
  --log-line-prefix '%m [%p] ' \
  --log-timezone 'America/Los_Angeles' \
  /path/to/file.aez
```

Use `--raw` to inspect AEZ record headers, log context, template metadata, and
other diagnostic fields:

```sh
auto_explain_z_dump --raw /path/to/file.aez
```

Template references are resolved from template definition records in the same
file. Rotation resets the per-file template dictionary, so a `.aez` file is
self-contained for decoding.

## Rotation and Retention

AEZ rotation is modeled after PostgreSQL log rotation but applies only to AEZ
binary logs.

Generated names include `auto_explain_z.file_prefix`, optional
`auto_explain_z.log_filename`, a rotation index, and the `.aez` suffix. For
example:

```conf
auto_explain_z.file_prefix = 'auto_explain_z'
auto_explain_z.log_filename = 'aez-%Y%m%d%H%M%S'
```

can produce a file like:

```text
auto_explain_z-aez-20260525123000-0.aez
```

Manual rotation is available through SQL:

```sql
SELECT auto_explain_z_rotate_logfile();
```

The function is installed by `CREATE EXTENSION auto_explain_z`. Execute
privilege is revoked from `public` by default:

```sql
GRANT EXECUTE ON FUNCTION auto_explain_z_rotate_logfile() TO dba_role;
```

When AEZ is loaded through `shared_preload_libraries`, manual rotation records
a shared rotation request and the next backend that writes opens the next
shared binary file.

Retention cleanup is opportunistic. It runs when a backend opens or rotates a
binary log file and only considers files in `auto_explain_z.directory` whose
names match `auto_explain_z.file_prefix` and end in `.aez`.

## How Templates Work

When template caching is enabled, AEZ can write the first occurrence of a plan
shape as a template definition and later executions as compact references.

Templates are keyed by query id plus a structural plan-shape hash. The hash
includes the plan node hierarchy, relation and index identity, relevant plan
options, and full-profile expression details where those affect reconstructed
EXPLAIN output.

Every record also stores a plan identifier, so analysis tools can group records
by plan shape immediately. Template-reference records store the template id,
plan identifier, and optional per-execution metrics. When runtime metrics are
present, the decoder overlays the metrics tree onto the cached static template
tree.

In shared-log mode, template admission and template id assignment are shared
across backends under `auto_explain_z.max_templates`. Each file still contains
the template definitions needed by its references, so decoding does not depend
on another backend's private state or another log file.

## Binary Format at a Glance

The file is versioned at the file level. Internal records do not carry their
own magic numbers or format versions.

- File header: magic, format version, compression method, PostgreSQL version,
  postmaster start time, postmaster start epoch, and postmaster pid.
- File body: serialized record data. In shared-log mode compression is applied
  to flushed backend batches, not to individual records.
- Record header: compact size-control bytes, implicit record number,
  timestamp delta, profile, query flags, optional query id, duration, and
  payload length.
- Payload: log context, template metadata, static plan tree or metrics tree,
  and query-level details.

Small integer fields use size-control bits and are stored as 1, 2, 4, or 8
bytes as needed.

## Benchmarks

The current benchmark matrix compares PostgreSQL with no plan logging,
`auto_explain`, and `auto_explain_z`.

Command shape used for the latest local run shown below:

```sh
bench/auto_explain_z_bench_all_modes \
  --scale 5 --clients 8 --jobs 4 \
  --duration 60 --warmup 30 \
  --prewarm --reset-between-scenarios --burnin-baseline \
  --iterations 1 \
  --protocol prepared \
  --auto-explain-formats text,json,yaml,xml \
  --aez-profiles simple,full \
  --compressions lz4,zstd \
  --template-modes on,off \
  --json-output results/perf_full_matrix.json
```

The run covered 9 workloads and 117 measured scenarios. Results are from a
single local run with one iteration. To avoid first-run and mutable-table bias,
each workload first ran and discarded a baseline burn-in scenario. Each
measured scenario then restored the prepared `PGDATA`, ran a SQL prewarm pass
over benchmark relations and common index paths, ran one 30-second pgbench
warmup, and then ran one 60-second measured pgbench window. Negative overhead
values and cases where plan logging appears faster than baseline should still
be read as benchmark noise; use multiple iterations for a defensible TPS mean.
The byte counts are the stronger signal: AEZ reduces plan-log volume by one to
three orders of magnitude in this matrix.

| Workload | Baseline TPS | AE text overhead / bytes per tx | AE json overhead / bytes per tx | Best AEZ TPS | Best AEZ overhead | Smallest AEZ bytes per tx |
|---|---:|---:|---:|---:|---:|---:|
| select-only | 205930 | 54.10% / 287.8 | 56.99% / 601.8 | 201818 `simple zstd tpl` | 2.00% | 6.7 `simple zstd tpl` |
| simple-update | 29603 | 22.50% / 1039.7 | 25.85% / 2603.7 | 30331 `simple zstd tpl` | -2.46% | 35.2 `simple zstd tpl` |
| tpcb-like | 20426 | 24.45% / 1912.1 | 31.14% / 5142.1 | 19415 `simple lz4 tpl` | 4.95% | 59.4 `simple zstd tpl` |
| shape-simple-point | 206976 | 50.67% / 289.8 | 52.61% / 603.8 | 201845 `simple lz4 tpl` | 2.48% | 6.6 `simple zstd tpl` |
| shape-simple-range | 56707 | 12.54% / 428.6 | 16.62% / 1085.6 | 55885 `simple zstd tpl` | 1.45% | 11.4 `simple zstd tpl` |
| shape-join-agg | 10254 | -3.18% / 1300.5 | -1.29% / 3953.5 | 11206 `simple zstd tpl` | -9.29% | 10.9 `simple zstd tpl` |
| shape-complex | 3755 | 1.28% / 1962.4 | 2.98% / 5889.3 | 3773 `simple zstd tpl` | -0.48% | 11.9 `simple zstd tpl` |
| shape-very-complex | 246 | 0.27% / 4247.7 | 2.07% / 13606.2 | 245 `simple zstd tpl` | 0.48% | 15.7 `simple zstd tpl` |
| shape-node-suite | 871 | 7.01% / 1721.8 | 5.61% / 5575.0 | 989 `full lz4 tpl` | -13.57% | 25.7 `simple zstd tpl` |

For production decisions, run longer tests with explicit prewarming and
multiple iterations:

```sh
bench/auto_explain_z_bench_all_modes \
  --scale 50 --clients 32 --jobs 16 \
  --duration 60 --warmup 30 \
  --prewarm --reset-between-scenarios --burnin-baseline \
  --iterations 5 \
  --protocol prepared \
  --json-output results/perf_full_matrix.json
```

The local 60-second run above was executed one workload per invocation and then
combined, which keeps peak disk usage bounded while regular `auto_explain`
emits multi-GB text/json/yaml/xml logs. Running the whole matrix in one
temporary cluster requires enough free disk for all regular AE log files at
once.

Other benchmark entry points isolate specific axes:

```sh
bench/auto_explain_z_bench_templates
bench/auto_explain_z_bench_profiles
bench/auto_explain_z_bench_compression
bench/auto_explain_z_bench_shapes
bench/auto_explain_z_bench_formats
```

Each accepts the common `bench/auto_explain_z_bench` options for scale, clients,
duration, warmup, prewarm, reset-between-scenarios, burnin-baseline,
iterations, server paths, `pg_config`, and JSON output.

### Benchmark Workload SQL

The benchmark matrix uses the following measured transaction scripts. The
first three are PostgreSQL `pgbench` built-ins; the remaining scripts are
defined by `bench/auto_explain_z_bench`.

`select-only`:

```sql
\set aid random(1, 100000 * :scale)
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
```

`simple-update`:

```sql
\set aid random(1, 100000 * :scale)
\set bid random(1, 1 * :scale)
\set tid random(1, 10 * :scale)
\set delta random(-5000, 5000)
BEGIN;
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);
END;
```

`tpcb-like`:

```sql
\set aid random(1, 100000 * :scale)
\set bid random(1, 1 * :scale)
\set tid random(1, 10 * :scale)
\set delta random(-5000, 5000)
BEGIN;
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
UPDATE pgbench_tellers SET tbalance = tbalance + :delta WHERE tid = :tid;
UPDATE pgbench_branches SET bbalance = bbalance + :delta WHERE bid = :bid;
INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);
END;
```

`shape-simple-point`:

```sql
\set aid random(1, :naccounts)
SELECT abalance
FROM pgbench_accounts
WHERE aid = :aid;
```

`shape-simple-range`:

```sql
\set lo random(1, :naccounts - 1000)
SELECT count(*), sum(abalance)
FROM pgbench_accounts
WHERE aid BETWEEN :lo AND :lo + 999;
```

`shape-join-agg`:

```sql
\set cid random(1, :aez_customers - 200)
SELECT c.region, count(*), sum(o.amount)
FROM aez_order o
JOIN aez_customer c ON c.id = o.customer_id
WHERE o.customer_id BETWEEN :cid AND :cid + 199
  AND o.status IN ('paid', 'open')
  AND c.active
GROUP BY c.region
ORDER BY c.region;
```

`shape-complex`:

```sql
\set cid random(1, :aez_customers - 1000)
WITH regional_totals AS (
    SELECT c.region, o.customer_id, sum(o.amount) AS total_amount,
           count(*) AS order_count
    FROM aez_order o
    JOIN aez_customer c ON c.id = o.customer_id
    WHERE o.customer_id BETWEEN :cid AND :cid + 999
      AND o.status IN ('paid', 'open')
      AND c.active
    GROUP BY c.region, o.customer_id
)
SELECT region, count(*), sum(total_amount), avg(order_count)
FROM regional_totals
WHERE total_amount > 1000
GROUP BY region
ORDER BY region;
```

`shape-very-complex`:

```sql
\set cid random(1, :aez_customers - 1500)
WITH filtered_orders AS MATERIALIZED (
    SELECT o.id, o.customer_id, c.region, o.status, o.amount, o.created_at
    FROM aez_order o
    JOIN aez_customer c ON c.id = o.customer_id
    WHERE o.customer_id BETWEEN :cid AND :cid + 1499
      AND o.status IN ('paid', 'open')
      AND c.active
),
event_rollup AS (
    SELECT e.order_id, count(*) AS event_count, sum(e.value) AS event_value
    FROM aez_event e
    JOIN filtered_orders fo ON fo.id = e.order_id
    WHERE e.event_type <> 'ignore'
    GROUP BY e.order_id
),
ranked AS (
    SELECT fo.region, fo.status, fo.customer_id,
           fo.amount + coalesce(er.event_value, 0) AS total_score,
           coalesce(er.event_count, 0) AS event_count,
           row_number() OVER (
               PARTITION BY fo.region, fo.status
               ORDER BY fo.amount DESC, fo.created_at DESC
           ) AS rn
    FROM filtered_orders fo
    LEFT JOIN event_rollup er ON er.order_id = fo.id
    WHERE EXISTS (
        SELECT 1
        FROM aez_customer c2
        WHERE c2.id = fo.customer_id
          AND c2.active
    )
)
SELECT region, status, count(*), sum(total_score), avg(event_count), max(rn)
FROM ranked
WHERE rn <= 20
GROUP BY region, status
ORDER BY region, status;
```

`shape-node-suite/point`:

```sql
\set aid random(1, :naccounts)
SET enable_seqscan = off;
SELECT abalance
FROM pgbench_accounts
WHERE aid = :aid;
RESET enable_seqscan;
```

`shape-node-suite/range`:

```sql
\set lo random(1, :naccounts - 1000)
SELECT count(*), sum(abalance)
FROM pgbench_accounts
WHERE aid BETWEEN :lo AND :lo + 999;
```

`shape-node-suite/bitmap`:

```sql
\set aval random(1, 999)
\set bval random(1, 999)
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT count(*)
FROM aez_bitmap
WHERE a = :aval OR b = :bval;
SELECT count(*)
FROM aez_bitmap
WHERE a = :aval AND b = :bval;
RESET enable_indexscan;
RESET enable_seqscan;
```

`shape-node-suite/tid-sample`:

```sql
SET enable_seqscan = off;
SELECT aid
FROM pgbench_accounts
WHERE ctid = '(0,1)'::tid;
SELECT count(*)
FROM pgbench_accounts
WHERE ctid >= '(0,1)'::tid AND ctid < '(0,20)'::tid;
RESET enable_seqscan;
SELECT count(*)
FROM pgbench_accounts TABLESAMPLE SYSTEM (1);
```

`shape-node-suite/values-function-cte`:

```sql
SELECT * FROM (VALUES (1), (2), (3)) v(x);
SELECT * FROM generate_series(1, 8) g;
SELECT generate_series(1, 4);
WITH c AS MATERIALIZED (
    SELECT aid
    FROM pgbench_accounts
    WHERE aid <= 10
)
SELECT count(*) FROM c;
WITH RECURSIVE r(n) AS (
    SELECT 1
    UNION ALL
    SELECT n + 1 FROM r WHERE n < 8
)
SELECT sum(n) FROM r;
```

`shape-node-suite/sort-unique-setop-window`:

```sql
SELECT *
FROM (
    SELECT id, region
    FROM aez_customer
    ORDER BY region, id
    LIMIT 40
) s
WHERE region >= 0;
SELECT DISTINCT region
FROM aez_customer
ORDER BY region;
SELECT region
FROM aez_customer
INTERSECT
SELECT region
FROM aez_customer
WHERE active;
SELECT id, row_number() OVER (PARTITION BY region ORDER BY id)
FROM aez_customer
LIMIT 40;
```

`shape-node-suite/join-variants`:

```sql
\set cid random(1, :aez_customers - 1000)
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT count(*)
FROM aez_order o
JOIN aez_customer c ON c.id = o.customer_id
WHERE o.customer_id BETWEEN :cid AND :cid + 999
  AND c.active;
RESET enable_mergejoin;
RESET enable_nestloop;
SET enable_hashjoin = off;
SET enable_nestloop = off;
SET enable_mergejoin = on;
SELECT count(*)
FROM aez_order o
JOIN aez_customer c ON c.id = o.customer_id
WHERE o.customer_id BETWEEN :cid AND :cid + 999;
RESET enable_hashjoin;
RESET enable_nestloop;
RESET enable_mergejoin;
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_nestloop = on;
SET enable_memoize = on;
SELECT count(*)
FROM aez_order o
JOIN aez_customer c ON c.id = o.customer_id
WHERE o.customer_id BETWEEN :cid AND :cid + 999;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
RESET enable_memoize;
```

`shape-node-suite/partition-lock-modify`:

```sql
\set aid random(1, :naccounts)
\set mid random(1, 1000000)
BEGIN;
SELECT aid
FROM pgbench_accounts
WHERE aid = :aid
FOR UPDATE;
COMMIT;
SELECT count(*)
FROM aez_part
WHERE id BETWEEN 1 AND 220;
INSERT INTO aez_mod VALUES (:mid, 'a')
ON CONFLICT (id) DO UPDATE SET payload = excluded.payload;
UPDATE aez_mod SET payload = 'b' WHERE id = :mid;
DELETE FROM aez_mod WHERE id = :mid;
```

`shape-node-suite/parallel`:

```sql
SET max_parallel_workers_per_gather = 4;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SELECT bucket, count(*), sum(id)
FROM aez_parallel
GROUP BY bucket
ORDER BY bucket;
```

`shape-node-suite/complex`:

```sql
\set cid random(1, :aez_customers - 1000)
WITH regional_totals AS (
    SELECT c.region, o.customer_id, sum(o.amount) AS total_amount,
           count(*) AS order_count
    FROM aez_order o
    JOIN aez_customer c ON c.id = o.customer_id
    WHERE o.customer_id BETWEEN :cid AND :cid + 999
      AND o.status IN ('paid', 'open')
      AND c.active
    GROUP BY c.region, o.customer_id
)
SELECT region, count(*), sum(total_amount), avg(order_count)
FROM regional_totals
WHERE total_amount > 1000
GROUP BY region
ORDER BY region;
```

`shape-node-suite/very-complex`:

```sql
\set cid random(1, :aez_customers - 1500)
WITH filtered_orders AS MATERIALIZED (
    SELECT o.id, o.customer_id, c.region, o.status, o.amount, o.created_at
    FROM aez_order o
    JOIN aez_customer c ON c.id = o.customer_id
    WHERE o.customer_id BETWEEN :cid AND :cid + 1499
      AND o.status IN ('paid', 'open')
      AND c.active
),
event_rollup AS (
    SELECT e.order_id, count(*) AS event_count, sum(e.value) AS event_value
    FROM aez_event e
    JOIN filtered_orders fo ON fo.id = e.order_id
    WHERE e.event_type <> 'ignore'
    GROUP BY e.order_id
),
ranked AS (
    SELECT fo.region, fo.status, fo.customer_id,
           fo.amount + coalesce(er.event_value, 0) AS total_score,
           coalesce(er.event_count, 0) AS event_count,
           row_number() OVER (
               PARTITION BY fo.region, fo.status
               ORDER BY fo.amount DESC, fo.created_at DESC
           ) AS rn
    FROM filtered_orders fo
    LEFT JOIN event_rollup er ON er.order_id = fo.id
)
SELECT region, status, count(*), sum(total_score), avg(event_count), max(rn)
FROM ranked
WHERE rn <= 20
GROUP BY region, status
ORDER BY region, status;
```

## Testing

Run the TAP suite with an installed PostgreSQL build:

```sh
make installcheck PG_CONFIG=/path/to/pg_config
```

For PostgreSQL builds without installed TAP support, point the standalone test
runner at a matching PostgreSQL source tree:

```sh
make tapcheck PG_CONFIG=/path/to/pg_config \
  PG_TEST_PERL_DIR=/path/to/postgresql/src/test/perl
```

Run the full decoder equivalence matrix:

```sh
make fulltapcheck PG_CONFIG=/path/to/pg_config \
  PG_TEST_PERL_DIR=/path/to/postgresql/src/test/perl
```

The decoder tests compare AEZ-decoded output with PostgreSQL EXPLAIN output
for supported formats and EXPLAIN modes.

## Scope

`auto_explain_z` is deliberately separate from PostgreSQL's
`contrib/auto_explain`. It installs its own executor hooks, owns the
`auto_explain_z` GUC namespace, and writes plan payloads only to `.aez` binary
files.

The full profile covers common core EXPLAIN details and runtime side channels,
including trigger and JIT summaries, sort and incremental sort stats, hash and
hash aggregate stats, memoize stats, tuplestore storage stats, index search
counts, bitmap heap block counts, ModifyTable conflict and MERGE counters, and
runtime subplan pruning counts.

FDW, direct foreign modify, foreign modify, and custom scan EXPLAIN callbacks
are captured as compressed text side-channel details because PostgreSQL exposes
those extension points as arbitrary formatter output rather than typed plan
fields.
