# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Cwd qw(abs_path);
use FindBin;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $decoder = "$FindBin::Bin/../auto_explain_z_dump";
plan skip_all => "auto_explain_z_dump not found" unless -x $decoder;

sub sql_string
{
	my ($value) = @_;
	$value =~ s/'/''/g;
	return "'$value'";
}

sub decode_dir_raw
{
	my ($dir, $format) = @_;
	$format ||= 'json';
	my @files = sort glob("$dir/*.aez");
	ok(scalar(@files) > 0, "$dir contains auto_explain_z binary logs");

	my ($stdout, $stderr) =
	  run_command([ $decoder, '--raw', '--format', $format, @files ]);
	is($stderr, '', "decoder stderr is empty for $dir");
	return $stdout;
}

sub decode_dir_pg
{
	my ($dir, $format) = @_;
	$format ||= 'text';
	my @files = sort glob("$dir/*.aez");
	ok(scalar(@files) > 0, "$dir contains auto_explain_z binary logs");

	my ($stdout, $stderr) =
	  run_command([ $decoder, '--format', $format, @files ]);
	is($stderr, '', "postgres-style decoder stderr is empty for $dir");
	return $stdout;
}

sub decode_dir
{
	return decode_dir_raw(@_);
}

sub like_node
{
	my ($json, $node_type, $name) = @_;
	like($json, qr/"Node Type": "\Q$node_type\E"/,
		$name || "$node_type decoded");
}

sub like_any_node
{
	my ($json, $node_re, $name) = @_;
	like($json, qr/"Node Type": "(?:$node_re)"/, $name);
}

sub try_logged_query
{
	my ($node, $sql) = @_;
	return $node->psql('postgres', $sql);
}

sub count_aez_files
{
	my ($dir) = @_;
	my @files = glob("$dir/*.aez");
	return scalar(@files);
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

my $data_dir = abs_path($node->data_dir);
my $module_dir = "$FindBin::Bin/..";
my $fdw_file = "$data_dir/aez_fdw.csv";
my $log_dir = "$data_dir/aez_full";
my $template_dir = "$data_dir/aez_template";
my $param_template_dir = "$data_dir/aez_param_template";
my $complex_dir = "$data_dir/aez_complex";
my $complex_template_dir = "$data_dir/aez_complex_template";
my $shape_dir = "$data_dir/aez_shapes";
my $parallel_dir = "$data_dir/aez_parallel";
my $fdw_dir = "$data_dir/aez_fdw";
my $customscan_dir = "$data_dir/aez_customscan";
my $options_dir = "$data_dir/aez_options";
my $pending_dir = "$data_dir/aez_pending";
my $retention_dir = "$data_dir/aez_retention";

$node->append_conf(
	'postgresql.conf',
	qq{
dynamic_library_path = '$module_dir:\$libdir'
session_preload_libraries = 'auto_explain_z,auto_explain_z_testscan'
compute_query_id = on
auto_explain_z.log_min_duration = 0
auto_explain_z.log_analyze = on
auto_explain_z.log_timing = off
auto_explain_z.log_buffers = on
auto_explain_z.profile = full
auto_explain_z.compression = none
auto_explain_z.directory = '$log_dir'
auto_explain_z.template_cache = on
auto_explain_z.template_min_plan_bytes = 0
});
$node->start;

append_to_file($fdw_file, "1,alpha\n2,beta\n3,gamma\n");

$node->safe_psql(
	'postgres', q{
CREATE TABLE aez_t(id int primary key, payload text);
INSERT INTO aez_t SELECT g, repeat('x', 32) FROM generate_series(1, 20) g;
CREATE TABLE aez_customer(
	id int primary key,
	region int not null,
	active boolean not null,
	payload text
);
CREATE TABLE aez_order(
	id int primary key,
	customer_id int not null,
	status text not null,
	amount int not null,
	created_at int not null
);
INSERT INTO aez_customer
SELECT g, g % 9, (g % 5) <> 0, repeat('c', 24)
FROM generate_series(1, 300) g;
INSERT INTO aez_order
SELECT g,
       (g % 300) + 1,
       CASE WHEN g % 4 = 0 THEN 'paid'
            WHEN g % 4 = 1 THEN 'open'
            ELSE 'closed' END,
       (g * 17) % 1000,
       g % 365
FROM generate_series(1, 6000) g;
CREATE INDEX aez_customer_region_idx ON aez_customer(region);
CREATE INDEX aez_order_customer_idx ON aez_order(customer_id);
CREATE INDEX aez_order_status_customer_idx ON aez_order(status, customer_id);
CREATE INDEX aez_order_amount_idx ON aez_order(amount);
CREATE TABLE aez_bitmap(
	id int primary key,
	a int not null,
	b int not null,
	filler text
);
INSERT INTO aez_bitmap
SELECT g, g % 1000, (g * 7) % 1000, repeat('b', 16)
FROM generate_series(1, 20000) g;
CREATE INDEX aez_bitmap_a_idx ON aez_bitmap(a);
CREATE INDEX aez_bitmap_b_idx ON aez_bitmap(b);
CREATE TABLE aez_pair(
	id int primary key,
	customer_id int not null,
	filler text
);
INSERT INTO aez_pair
SELECT g, (g % 300) + 1, repeat('p', 16)
FROM generate_series(1, 1200) g;
CREATE INDEX aez_pair_customer_idx ON aez_pair(customer_id);
CREATE TABLE aez_mod(id int primary key, payload text);
CREATE TABLE aez_customscan(id int);
INSERT INTO aez_customscan SELECT g FROM generate_series(1, 10) g;
CREATE TABLE aez_transition_src(id int);
CREATE TABLE aez_transition_audit(cnt int);
CREATE FUNCTION aez_transition_fn() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO aez_transition_audit SELECT count(*) FROM new_table;
	RETURN NULL;
END;
$$;
CREATE TRIGGER aez_transition_trg
AFTER INSERT ON aez_transition_src
REFERENCING NEW TABLE AS new_table
FOR EACH STATEMENT EXECUTE FUNCTION aez_transition_fn();
CREATE TABLE aez_parallel AS
SELECT g AS id, g % 64 AS bucket, repeat('q', 20) AS filler
FROM generate_series(1, 100000) g;
ALTER TABLE aez_parallel SET (parallel_workers = 4);
CREATE TABLE aez_part(id int, region int, payload text) PARTITION BY RANGE (id);
CREATE TABLE aez_part_1 PARTITION OF aez_part FOR VALUES FROM (0) TO (100);
CREATE TABLE aez_part_2 PARTITION OF aez_part FOR VALUES FROM (100) TO (200);
CREATE TABLE aez_part_3 PARTITION OF aez_part FOR VALUES FROM (200) TO (300);
INSERT INTO aez_part
SELECT g, g % 7, repeat('r', 12) FROM generate_series(1, 240) g;
CREATE INDEX aez_part_id_idx ON aez_part(id);
ANALYZE aez_t;
ANALYZE aez_customer;
ANALYZE aez_order;
ANALYZE aez_bitmap;
ANALYZE aez_pair;
ANALYZE aez_mod;
ANALYZE aez_customscan;
ANALYZE aez_parallel;
ANALYZE aez_part;
});
$node->safe_psql('postgres', 'VACUUM ANALYZE aez_t;');
$node->safe_psql(
	'postgres',
	"CREATE EXTENSION file_fdw;
CREATE SERVER aez_file_srv FOREIGN DATA WRAPPER file_fdw;
CREATE FOREIGN TABLE aez_fdw(id int, label text)
SERVER aez_file_srv
OPTIONS (filename " . sql_string($fdw_file) . ", format 'csv');");
$node->safe_psql(
	'postgres', q{
SET enable_seqscan = off;
SELECT * FROM aez_t WHERE id = 7;
RESET enable_seqscan;
});

my $json = decode_dir($log_dir, 'json');
like($json, qr/"Query Text": "SELECT \* FROM aez_t WHERE id = 7;"/,
	'query text decoded from binary log');
like($json, qr/"Format Version": 13/,
	'v13 binary log decoded');
like($json, qr/"Plan Identifier": \d+/,
	'plan identifier decoded from binary log');
like($json, qr/"Compression": "none"/,
	'uncompressed binary log decoded');
like($json, qr/"Node Type": "Index Scan"/,
	'plan node decoded from binary log');
like($json, qr/"Buffers": \{/,
	'buffer counters decoded from binary log');

my $pg_text = decode_dir_pg($log_dir, 'text');
like($pg_text, qr/Query Text: SELECT \* FROM aez_t WHERE id = 7;/,
	'postgres-style text decode includes query text');
like($pg_text, qr/Index Scan using aez_t_pkey on aez_t/,
	'postgres-style text decode includes index scan');

my $pg_json = decode_dir_pg($log_dir, 'json');
like($pg_json, qr/"Plan": \{/,
	'postgres-style json decode includes plan object');
like($pg_json, qr/"Node Type": "Index Scan"/,
	'postgres-style json decode includes index scan');

my $pg_yaml = decode_dir_pg($log_dir, 'yaml');
like($pg_yaml, qr/Plan:\s*\n\s+Node Type: "Index Scan"/,
	'postgres-style yaml decode includes index scan');

my $pg_xml = decode_dir_pg($log_dir, 'xml');
like($pg_xml, qr/<Plan>\s*<Node-Type>Index Scan<\/Node-Type>/,
	'postgres-style xml decode includes index scan');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($template_dir) . q{;
SET auto_explain_z.template_cache = on;
SET auto_explain_z.template_min_plan_bytes = 0;
SELECT * FROM aez_t WHERE id = 3;
SELECT * FROM aez_t WHERE id = 3;
SELECT * FROM aez_t WHERE id = 3;
});

$json = decode_dir($template_dir, 'json');
like($json, qr/"Mode": "define"/, 'template definition decoded');
like($json, qr/"Mode": "ref"/, 'template reference decoded');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($param_template_dir) . q{;
SET auto_explain_z.profile = simple;
SET auto_explain_z.template_cache = on;
SET auto_explain_z.template_min_plan_bytes = 0;
SET auto_explain_z.log_parameter_max_length = 5;
PREPARE aez_template_param(int) AS SELECT * FROM aez_t WHERE id = $1;
EXECUTE aez_template_param(4);
EXECUTE aez_template_param(5);
EXECUTE aez_template_param(6);
});

$json = decode_dir($param_template_dir, 'json');
like($json, qr/"Mode": "ref"/,
	'parameterized template reference decoded');
like($json, qr/"Query Parameters": "\$1 = '5'"/,
	'parameterized fast template reference preserves query parameters');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($complex_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = off;
SET enable_nestloop = off;
SET enable_mergejoin = off;
WITH regional_totals AS (
	SELECT c.region, o.customer_id, sum(o.amount) AS total_amount
	FROM aez_order o
	JOIN aez_customer c ON c.id = o.customer_id
	WHERE o.status IN ('paid', 'open')
	  AND c.region BETWEEN 1 AND 6
	  AND c.active
	GROUP BY c.region, o.customer_id
)
SELECT region, count(*), sum(total_amount)
FROM regional_totals
WHERE total_amount > 2500
GROUP BY region
ORDER BY region;
SET enable_seqscan = off;
SELECT count(*)
FROM aez_order
WHERE status = 'paid'
  AND customer_id BETWEEN 10 AND 180
  AND amount > 100;
});

$json = decode_dir($complex_dir, 'json');
like($json, qr/"Node Type": "(?:Hash Join|Merge Join|Nested Loop)"/,
	'complex plan includes a join node');
like($json, qr/"Node Type": "Aggregate"/,
	'complex plan includes an aggregate node');
like($json, qr/"Node Type": "(?:Sort|Incremental Sort)"/,
	'complex plan includes a sort node');
like($json, qr/"Node Type": "(?:Bitmap Heap Scan|Index Scan|Index Only Scan)"/,
	'complex plan includes an indexed access node');
like($json, qr/"(?:Group Key|Hash Key)": /,
	'complex full-profile plan includes grouping key details');
like($json, qr/"(?:Hash Cond|Merge Cond|Join Filter)": /,
	'complex full-profile plan includes join condition details');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($complex_template_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = on;
SET auto_explain_z.template_min_plan_bytes = 0;
SET auto_explain_z.template_omit_query_text = on;
SET enable_nestloop = off;
SET enable_mergejoin = off;
PREPARE aez_complex_plan(int, int) AS
WITH regional_totals AS (
	SELECT c.region, o.customer_id, sum(o.amount) AS total_amount
	FROM aez_order o
	JOIN aez_customer c ON c.id = o.customer_id
	WHERE o.status IN ('paid', 'open')
	  AND c.region BETWEEN $1 AND $2
	  AND c.active
	GROUP BY c.region, o.customer_id
)
SELECT region, count(*), sum(total_amount)
FROM regional_totals
WHERE total_amount > 2500
GROUP BY region
ORDER BY region;
EXECUTE aez_complex_plan(1, 6);
EXECUTE aez_complex_plan(1, 6);
EXECUTE aez_complex_plan(1, 6);
});

$json = decode_dir($complex_template_dir, 'json');
like($json, qr/"Mode": "define"/,
	'complex template definition decoded');
like($json, qr/"Mode": "ref"/,
	'complex template reference decoded');
like($json, qr/"Node Type": "(?:Hash Join|Merge Join|Nested Loop)"/,
	'complex template reference reconstructs join node');
like($json, qr/"Node Type": "Aggregate"/,
	'complex template reference reconstructs aggregate node');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($shape_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = off;
SET auto_explain_z.log_analyze = on;
SET auto_explain_z.log_verbose = on;
SET auto_explain_z.log_buffers = on;
SET auto_explain_z.log_wal = on;
SET auto_explain_z.log_timing = on;
SET auto_explain_z.log_nested_statements = on;
SET jit = off;
SELECT 1 WHERE true;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM aez_t WHERE payload = repeat('x', 32);
RESET enable_indexscan;
RESET enable_bitmapscan;
SET enable_seqscan = off;
SELECT * FROM aez_t WHERE id = 5;
SET enable_bitmapscan = off;
SELECT id FROM aez_t WHERE id BETWEEN 1 AND 5;
RESET enable_seqscan;
RESET enable_bitmapscan;
SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT count(*)
FROM aez_order
WHERE status = 'paid'
  AND customer_id BETWEEN 10 AND 120;
SELECT count(*) FROM aez_bitmap WHERE a = 17 OR b = 23;
SELECT count(*) FROM aez_bitmap WHERE a = 17 AND b = 23;
RESET enable_seqscan;
RESET enable_indexscan;
SET enable_seqscan = off;
SELECT * FROM aez_t WHERE ctid = '(0,1)'::tid;
SELECT count(*)
FROM aez_t
WHERE ctid >= '(0,1)'::tid AND ctid < '(0,10)'::tid;
RESET enable_seqscan;
SELECT count(*) FROM aez_t TABLESAMPLE SYSTEM (100);
SELECT * FROM (VALUES (1), (2)) v(x);
SELECT * FROM generate_series(1, 3) g;
SELECT generate_series(1, 3);
WITH c AS MATERIALIZED (
	SELECT * FROM aez_t WHERE id <= 3
)
SELECT count(*) FROM c;
WITH RECURSIVE r(n) AS (
	SELECT 1
	UNION ALL
	SELECT n + 1 FROM r WHERE n < 4
)
SELECT sum(n) FROM r;
SELECT * FROM (SELECT * FROM aez_t LIMIT 5) s WHERE id > 0;
SELECT * FROM aez_order ORDER BY amount DESC LIMIT 5;
SET enable_hashagg = off;
SELECT region FROM aez_customer GROUP BY region ORDER BY region;
SELECT DISTINCT region FROM aez_customer ORDER BY region;
RESET enable_hashagg;
SELECT id, row_number() OVER (PARTITION BY region ORDER BY id)
FROM aez_customer
LIMIT 20;
SELECT region FROM aez_customer
INTERSECT
SELECT region FROM aez_customer WHERE active;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SELECT count(*)
FROM aez_order o
JOIN aez_customer c ON c.id = o.customer_id
WHERE c.region = 3;
RESET enable_mergejoin;
RESET enable_nestloop;
SET enable_hashjoin = off;
SET enable_nestloop = off;
SET enable_mergejoin = on;
SELECT count(*)
FROM aez_order o
JOIN aez_customer c ON c.id = o.customer_id;
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
WHERE o.id <= 1200;
SELECT count(*)
FROM (SELECT * FROM aez_customer WHERE region = 1) c
JOIN (SELECT * FROM aez_t) t ON true;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;
RESET enable_memoize;
SELECT count(*) FROM aez_part WHERE id BETWEEN 1 AND 220;
SET enable_sort = off;
SELECT * FROM aez_part WHERE id BETWEEN 1 AND 220 ORDER BY id LIMIT 10;
RESET enable_sort;
BEGIN;
SELECT * FROM aez_t WHERE id = 1 FOR UPDATE;
COMMIT;
INSERT INTO aez_mod VALUES (1, 'a')
ON CONFLICT (id) DO UPDATE SET payload = excluded.payload;
UPDATE aez_mod SET payload = 'b' WHERE id = 1;
DELETE FROM aez_mod WHERE id = 1;
MERGE INTO aez_mod m
USING (VALUES (2, 'm')) AS v(id, payload)
ON m.id = v.id
WHEN MATCHED THEN UPDATE SET payload = v.payload
WHEN NOT MATCHED THEN INSERT VALUES (v.id, v.payload);
INSERT INTO aez_transition_src SELECT g FROM generate_series(1, 3) g;
});

my $tablefunc_available = 0;
my ($tf_ret, $tf_stdout, $tf_stderr) = try_logged_query(
	$node,
	"SET auto_explain_z.directory = " . sql_string($shape_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = off;
SELECT *
FROM XMLTABLE(
	'/rows/r'
	PASSING XMLPARSE(DOCUMENT '<rows><r><x>1</x></r><r><x>2</x></r></rows>')
	COLUMNS x int PATH 'x'
) AS xt;
});
if ($tf_ret == 0)
{
	$tablefunc_available = 1;
}
else
{
	note("XMLTABLE unavailable, Table Function Scan coverage skipped: $tf_stderr");
}

$json = decode_dir($shape_dir, 'json');
like_node($json, 'Result', 'result node decoded');
like_node($json, 'Seq Scan', 'sequential scan node decoded');
like_node($json, 'Index Scan', 'index scan node decoded');
like_node($json, 'Index Only Scan', 'index-only scan node decoded');
like_node($json, 'Bitmap Heap Scan', 'bitmap heap scan node decoded');
like_node($json, 'Bitmap Index Scan', 'bitmap index scan node decoded');
like_node($json, 'BitmapAnd', 'bitmap-and node decoded');
like_node($json, 'BitmapOr', 'bitmap-or node decoded');
like_any_node($json, 'Tid Scan|Tid Range Scan', 'tid scan node decoded');
like_node($json, 'Sample Scan', 'sample scan node decoded');
like_node($json, 'Values Scan', 'values scan node decoded');
like_node($json, 'Function Scan', 'function scan node decoded');
like_node($json, 'ProjectSet', 'project-set node decoded');
like_node($json, 'CTE Scan', 'cte scan node decoded');
like_node($json, 'Recursive Union', 'recursive union node decoded');
like_node($json, 'WorkTable Scan', 'worktable scan node decoded');
like_node($json, 'Subquery Scan', 'subquery scan node decoded');
like_node($json, 'Limit', 'limit node decoded');
like_node($json, 'Sort', 'sort node decoded');
like_node($json, 'Aggregate', 'aggregate node decoded');
like_any_node($json, 'Group|Aggregate', 'group or aggregate node decoded');
like_node($json, 'WindowAgg', 'window aggregate node decoded');
like_node($json, 'Unique', 'unique node decoded');
like_any_node($json, 'SetOp|HashSetOp', 'set-operation node decoded');
like_node($json, 'Hash Join', 'hash join node decoded');
like_node($json, 'Hash', 'hash node decoded');
like_node($json, 'Merge Join', 'merge join node decoded');
like_node($json, 'Nested Loop', 'nested-loop node decoded');
like_any_node($json, 'Materialize|Memoize', 'materialize or memoize node decoded');
like_any_node($json, 'Append|Merge Append', 'append node decoded');
like_node($json, 'LockRows', 'lock-rows node decoded');
like_node($json, 'ModifyTable', 'modify-table node decoded');
like_node($json, 'Named Tuplestore Scan',
	'named tuplestore scan node decoded from transition table');
like($json, qr/"WAL": \{/,
	'WAL counters decoded when log_wal is enabled');
like($json, qr/"Output": /,
	'verbose output list decoded when log_verbose is enabled');
like($json, qr/"Trigger": /,
	'trigger timing details decoded');
SKIP:
{
	skip "XMLTABLE unavailable, Table Function Scan coverage skipped", 1
	  unless $tablefunc_available;
	like_node($json, 'Table Function Scan', 'table function scan node decoded');
}

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($parallel_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = off;
SET max_parallel_workers_per_gather = 4;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SELECT bucket, count(*), sum(id)
FROM aez_parallel
GROUP BY bucket
ORDER BY bucket;
});

$json = decode_dir($parallel_dir, 'json');
like_any_node($json, 'Gather|Gather Merge',
	'parallel gather node decoded');
like($json, qr/"Workers Planned": /,
	'worker count detail decoded');
my $parallel_pg_json = decode_dir_pg($parallel_dir, 'json');
like($parallel_pg_json, qr/"Parallel Aware": true/,
	'parallel-aware child node decoded');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($fdw_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = off;
SELECT * FROM aez_fdw WHERE id > 1;
});

$json = decode_dir($fdw_dir, 'json');
like_node($json, 'Foreign Scan', 'foreign scan node decoded');
like($json, qr/"Extension Explain": /,
	'foreign scan extension EXPLAIN text decoded');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($customscan_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = off;
SELECT count(*) FROM aez_customscan;
});

$json = decode_dir($customscan_dir, 'json');
like_node($json, 'Custom Scan', 'custom scan node decoded');
like($json, qr/auto_explain_z test custom scan/,
	'custom scan extension EXPLAIN text decoded');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($options_dir) . q{;
SET auto_explain_z.profile = full;
SET auto_explain_z.template_cache = off;
SET auto_explain_z.log_query_text = on;
SET auto_explain_z.log_parameter_max_length = 3;
SET auto_explain_z.log_analyze = on;
SET auto_explain_z.log_timing = on;
SET auto_explain_z.log_buffers = on;
SET auto_explain_z.log_wal = on;
SET auto_explain_z.log_verbose = on;
PREPARE aez_param(text) AS SELECT $1::text AS payload;
EXECUTE aez_param('abcdef');
INSERT INTO aez_mod VALUES (3, 'wal');
SET auto_explain_z.log_query_text = off;
SELECT * FROM aez_t WHERE id = 2;
});

$json = decode_dir($options_dir, 'json');
like($json, qr/"Query Parameters": "\$1 = 'abc\.\.\.'/,
	'query parameters decoded with truncation');
like($json, qr/"Startup Time": /,
	'node timing decoded when log_timing is enabled');
like($json, qr/"Buffers": \{/,
	'buffer counters decoded when log_buffers is enabled');
like($json, qr/"WAL": \{/,
	'WAL counters decoded when log_wal is enabled');
like($json, qr/"Output": /,
	'verbose fields decoded when log_verbose is enabled');
unlike($json, qr/"Query Text": "SELECT \* FROM aez_t WHERE id = 2;"/,
	'query text can be suppressed by log_query_text');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($pending_dir) . q{;
SET auto_explain_z.profile = simple;
SET auto_explain_z.template_cache = off;
SET auto_explain_z.pending_buffer_size = 0;
SET auto_explain_z.compression = none;
SELECT count(*) FROM aez_t WHERE id <= 10;
});

$json = decode_dir($pending_dir, 'json');
like($json, qr/"Profile": "simple"/,
	'simple profile decoded with immediate flush');
like($json, qr/"Compression": "none"/,
	'uncompressed immediate-flush log decoded');

for my $compression ('none', 'lz4', 'zstd')
{
	my $dir = "$data_dir/aez_compression_$compression";
	my ($ret, $stdout, $stderr) = try_logged_query(
		$node,
		"SET auto_explain_z.directory = " . sql_string($dir) . ";
SET auto_explain_z.profile = simple;
SET auto_explain_z.template_cache = off;
SET auto_explain_z.pending_buffer_size = 8;
SET auto_explain_z.compression = " . sql_string($compression) . q{;
SELECT count(*) FROM aez_order WHERE customer_id BETWEEN 1 AND 50;
});
	if ($ret == 0)
	{
		$json = decode_dir($dir, 'json');
		like($json, qr/"Compression": "\Q$compression\E"/,
			"$compression-compressed binary log decoded");
	}
	else
	{
		note("$compression compression unavailable, decode coverage skipped: $stderr");
	}
}

for my $i (1 .. 6)
{
	$node->safe_psql(
		'postgres',
		"SET auto_explain_z.directory = " . sql_string($retention_dir) . q{;
SET auto_explain_z.max_file_size = 1;
SET auto_explain_z.retention_max_files = 2;
SET auto_explain_z.retention_cleanup_interval = 0;
SELECT count(*) FROM aez_t WHERE id <= 10;
});
}

my $retained = count_aez_files($retention_dir);
ok($retained > 0, 'retention test produced binary logs');
ok($retained <= 2, 'retention_max_files pruned old binary logs');

my $text = decode_dir($retention_dir, 'text');
like($text, qr/(Seq Scan|Index Scan|Aggregate)/,
	'text decoder renders retained binary logs');

$node->stop;

done_testing();
