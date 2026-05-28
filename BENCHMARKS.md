# auto_explain_z Benchmarks

The current benchmark matrix compares PostgreSQL with no plan logging,
`auto_explain`, and `auto_explain_z`.

Python 3 is required for the benchmark scripts under `bench/`.

Set path inputs through the environment before running benchmarks:

```sh
export AEZ_EXTENSION_ROOT
export PG_CONFIG
export AEZ_BENCH_RESULTS
```

For uninstalled PostgreSQL source-tree runs, also set `AEZ_POSTGRES_SOURCE`.
If `auto_explain` was built outside the target installation's library
directory, set `AEZ_AUTO_EXPLAIN_LIBRARY_DIR`.

A typical templated and compressed execution plan takes from a few bytes to the
low tens of bytes using ZSTD compression. LZ4 compression is, on average,
1.5x - 2x less compact, but incurs slightly less CPU overhead. Typical CPU
overhead is less than 1%, and I/O overhead is ~30x-50x lower than that of the
EXPLAIN text format and ~100x-200x lower than the JSON format.

Command shape used for the latest local run shown below:

```sh
"$AEZ_EXTENSION_ROOT/bench/auto_explain_z_bench_all_modes" \
  --scale 5 --clients 8 --jobs 4 \
  --duration 60 --warmup 30 \
  --prewarm --reset-between-scenarios --burnin-baseline \
  --iterations 1 \
  --protocol prepared \
  --auto-explain-formats text,json,yaml,xml \
  --aez-profiles simple,full \
  --compressions lz4,zstd \
  --template-modes on,off \
  --json-output "$AEZ_BENCH_RESULTS"
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
"$AEZ_EXTENSION_ROOT/bench/auto_explain_z_bench_all_modes" \
  --scale 50 --clients 32 --jobs 16 \
  --duration 60 --warmup 30 \
  --prewarm --reset-between-scenarios --burnin-baseline \
  --iterations 5 \
  --protocol prepared \
  --json-output "$AEZ_BENCH_RESULTS"
```

The local 60-second run above was executed one workload per invocation and then
combined, which keeps peak disk usage bounded while regular `auto_explain`
emits multi-GB text/json/yaml/xml logs. Running the whole matrix in one
temporary cluster requires enough free disk for all regular AE log files at
once.

Other benchmark entry points isolate specific axes:

```sh
"$AEZ_EXTENSION_ROOT/bench/auto_explain_z_bench_templates"
"$AEZ_EXTENSION_ROOT/bench/auto_explain_z_bench_profiles"
"$AEZ_EXTENSION_ROOT/bench/auto_explain_z_bench_compression"
"$AEZ_EXTENSION_ROOT/bench/auto_explain_z_bench_shapes"
"$AEZ_EXTENSION_ROOT/bench/auto_explain_z_bench_formats"
```

Each accepts the common `bench/auto_explain_z_bench` options for scale, clients,
duration, warmup, prewarm, reset-between-scenarios, burnin-baseline,
iterations, server paths, `pg_config`, and JSON output.

## Benchmark Workload SQL

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
