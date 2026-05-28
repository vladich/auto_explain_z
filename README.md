# auto_explain_z

`auto_explain_z` is a PostgreSQL extension for logging execution plans to a
separate compressed binary log instead of PostgreSQL's regular text log.

It is designed for installations that need to keep far more execution plans
than `auto_explain` usually makes practical. The extension keeps plan logging
off the regular log path, serializes common plan fields into compact binary
records, optionally stores repeated plan shapes as templates, and compresses
serialized data with a file-level compression setting.

Benchmark results, benchmark scripts, and benchmark workload SQL are documented
in [BENCHMARKS.md](BENCHMARKS.md).

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
- The native `auto_explain_z_dump` decoder is built by `make`. Decoding
  compressed files requires building against a PostgreSQL installation with
  the corresponding `--with-lz4` or `--with-zstd` support.

## Installation

Build and install the extension against the target PostgreSQL installation:

```sh
export PG_CONFIG
make
make install
```

Install the SQL objects in each database where you want the helper function:

```sql
CREATE EXTENSION auto_explain_z;
```

`CREATE EXTENSION` loads the module in the current session and installs
`auto_explain_z_rotate_logfile()`. Configure
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
single-log coordination path. Use `shared_preload_libraries` for regular usage.

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
export AEZ_LOG_FILE
auto_explain_z_dump --format text "$AEZ_LOG_FILE"
auto_explain_z_dump --format json "$AEZ_LOG_FILE"
auto_explain_z_dump --format yaml "$AEZ_LOG_FILE"
auto_explain_z_dump --format xml  "$AEZ_LOG_FILE"
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
  "$AEZ_LOG_FILE"
```

Use `--log-error-verbosity verbose` if the source server logged verbose stderr
records with SQLSTATE after the severity.

`--format` still controls the plan payload format inside the reconstructed log
message:

```sh
auto_explain_z_dump --postgres-log --format json \
  --log-line-prefix '%m [%p] ' \
  --log-timezone 'America/Los_Angeles' \
  "$AEZ_LOG_FILE"
```

Use `--raw` to inspect AEZ record headers, log context, template metadata, and
other diagnostic fields:

```sh
auto_explain_z_dump --raw "$AEZ_LOG_FILE"
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

Benchmark methodology, current local results, entry-point scripts, and workload
SQL are documented in [BENCHMARKS.md](BENCHMARKS.md).

## Testing

Run the TAP suite with an installed PostgreSQL build:

```sh
export PG_CONFIG
make installcheck
```

For PostgreSQL builds without installed TAP support, point the standalone test
runner at a matching PostgreSQL source tree:

```sh
export PG_CONFIG
export PG_TEST_PERL_DIR
make tapcheck
```

Run the full decoder equivalence matrix:

```sh
export PG_CONFIG
export PG_TEST_PERL_DIR
make fulltapcheck
```

The decoder tests compare AEZ-decoded output with PostgreSQL EXPLAIN output
for supported formats and EXPLAIN modes.

## Scope

`auto_explain_z` is deliberately separate from PostgreSQL's bundled
`auto_explain` extension. It installs its own executor hooks, owns the
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
