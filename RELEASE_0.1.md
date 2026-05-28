# auto_explain_z 0.1

Initial release of `auto_explain_z`, a PostgreSQL extension for logging
execution plans to separate compressed binary `.aez` logs instead of the regular
PostgreSQL text log.

`auto_explain_z` is intended for installations that need to retain many more
execution plans than regular `auto_explain` logging usually makes practical.
It serializes plan data into a compact binary format, optionally stores repeated
plan shapes as templates, compresses at the file level, and provides an offline
decoder for PostgreSQL-compatible output.

## Highlights

- Separate binary `.aez` plan logs; plan payloads are not written to regular
  PostgreSQL log files.
- Single coordinated log file when loaded through `shared_preload_libraries`.
- Binary serialization profiles:
  - `simple` for compact text-style plan analysis.
  - `full` for richer JSON/YAML/XML-compatible EXPLAIN detail.
- File-level compression with `none`, `lz4`, or `zstd`, depending on PostgreSQL
  build support.
- Shared plan template cache keyed by query id and plan shape.
- Stable plan identifiers on logged records for immediate grouping by plan
  shape.
- Preserves log context including timestamp, backend pid/type, database, user,
  authenticated user, application name, client address, duration, query id,
  query text, and parameters when enabled.
- Offline `auto_explain_z_dump` decoder with `text`, `json`, `yaml`, and `xml`
  rendering.
- `--postgres-log` decoder mode for reconstructing PostgreSQL-style log records
  with configurable `log_line_prefix`, timezone, and verbosity.
- Log rotation, retention cleanup, immediate flush mode, pending write buffers,
  and `auto_explain_z_rotate_logfile()`.

## Compatibility

This release supports PostgreSQL 14 through PostgreSQL 18.

Validated builds:

| Platform | PostgreSQL versions |
|---|---|
| Linux | 14.22, 15.17, 16.13, 17.9, 18.3 |
| macOS arm64 | 14.22, 15.17, 16.13, 17.9, 18.3 |

The test matrix includes byte-for-byte decoder equivalence against PostgreSQL
`EXPLAIN` for covered node shapes across:

- Formats: `text`, `json`, `yaml`, `xml`
- Modes: plain, verbose, analyze rows, analyze timing, analyze buffers,
  analyze WAL
- Plan shapes including scans, joins, aggregates, sorts, CTEs, recursive CTEs,
  partition append, parallel plans, FDW plans when available, and custom scans

## Benchmarks

The benchmark suite compares baseline PostgreSQL, regular `auto_explain`, and
`auto_explain_z` across simple, complex, templated, non-templated, compressed,
and uncompressed workloads.

In the latest documented benchmark run, typical AEZ plan-log volume was roughly:

- 30x-50x lower than regular `auto_explain` text output.
- 100x-200x lower than regular `auto_explain` JSON output.
- CPU overhead was typically under 1% in the tested templated/compressed modes,
  with benchmark noise noted in low-duration runs.

See `BENCHMARKS.md` for methodology, workload SQL, and the full result table.

## Installation

Build against the target PostgreSQL installation:

```sh
export PG_CONFIG=/path/to/pg_config
make
make install
```

Install SQL objects in the target database:

```sql
CREATE EXTENSION auto_explain_z;
```

For production logging, preload the extension and restart PostgreSQL:

```conf
shared_preload_libraries = 'auto_explain_z'

auto_explain_z.log_min_duration = '100ms'
auto_explain_z.profile = 'simple'
auto_explain_z.compression = 'lz4'
auto_explain_z.directory = 'auto_explain_z'
auto_explain_z.template_cache = on
```

Decode logs offline:

```sh
auto_explain_z_dump --format text "$AEZ_LOG_FILE"
auto_explain_z_dump --format json "$AEZ_LOG_FILE"
auto_explain_z_dump --postgres-log "$AEZ_LOG_FILE"
```

## Notes

- Compressed decoding requires building `auto_explain_z_dump` against a
  PostgreSQL installation with matching `--with-lz4` or `--with-zstd` support.
- Template references are self-contained within each `.aez` file. Rotation
  resets the per-file template dictionary so each file remains independently
  decodable.
- FDW and custom scan extension callbacks are captured as extension EXPLAIN
  text. Typed binary fields are used for core PostgreSQL plan data.
- The extension SQL control version is currently `1.0`; this GitHub release is
  product release `0.1`.

## Documentation

- `README.md` for installation, configuration, decoding, rotation, and format
  overview.
- `BENCHMARKS.md` for benchmark commands, query workloads, and results.
