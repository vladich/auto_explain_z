# Strict decoder equivalence coverage for auto_explain_z.
#
# By default this test always verifies that every planned node shape in the
# matrix can be logged and decoded.  It also compares decoded output against
# PostgreSQL EXPLAIN output byte-for-byte for the selected equivalence matrix.
# Set AEZ_DECODER_EQUIV_FULL_MATRIX=1 to run every node case through every
# format/mode instead of the smaller default equivalence matrix.

use strict;
use warnings FATAL => 'all';

use Cwd qw(abs_path);
use FindBin;
use JSON::PP;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $decoder = "$FindBin::Bin/../auto_explain_z_dump";
plan skip_all => "auto_explain_z_dump not found" unless -x $decoder;

my $full_matrix = $ENV{AEZ_DECODER_EQUIV_FULL_MATRIX} ? 1 : 0;

sub sql_string
{
	my ($value) = @_;
	$value =~ s/'/''/g;
	return "'$value'";
}

sub slurp_files
{
	my ($dir) = @_;
	my @files = sort glob("$dir/*.aez");
	ok(scalar(@files) > 0, "$dir contains auto_explain_z binary logs");
	return @files;
}

sub decode_files
{
	my ($dir, $format) = @_;
	my @files = slurp_files($dir);
	my ($stdout, $stderr) =
	  run_command([ $decoder, '--format', $format, @files ]);
	is($stderr, '', "decoder stderr is empty for $dir/$format");
	$stdout =~ s/\R\z//;
	return $stdout;
}

sub decode_json_records
{
	my ($dir) = @_;
	return decode_json(decode_files($dir, 'json'));
}

sub collect_node_types
{
	my ($node, $seen) = @_;
	return unless defined $node && ref($node) eq 'HASH';
	$seen->{ $node->{'Node Type'} } = 1 if exists $node->{'Node Type'};
	for my $child (@{ $node->{Plans} || [] })
	{
		collect_node_types($child, $seen);
	}
}

sub explain_options
{
	my ($mode, $format) = @_;
	my @options = ("FORMAT " . uc($format));

	push @options, 'ANALYZE ON' if $mode->{analyze};
	push @options, 'VERBOSE ON' if $mode->{verbose};
	push @options, 'TIMING ' . ($mode->{timing} ? 'ON' : 'OFF')
	  if $mode->{analyze};
	push @options, 'BUFFERS ' . ($mode->{buffers} ? 'ON' : 'OFF')
	  if $mode->{analyze};
	push @options, 'WAL ' . ($mode->{wal} ? 'ON' : 'OFF')
	  if $mode->{analyze};
	push @options, 'SUMMARY OFF' if $mode->{analyze};

	return join(', ', @options);
}

sub aez_mode_gucs
{
	my ($mode, $dir) = @_;
	return
	  "SET auto_explain_z.directory = " . sql_string($dir) . ";\n" .
	  "SET auto_explain_z.log_min_duration = 0;\n" .
	  "SET auto_explain_z.profile = full;\n" .
	  "SET auto_explain_z.template_cache = off;\n" .
	  "SET auto_explain_z.log_query_text = off;\n" .
	  "SET auto_explain_z.log_parameter_max_length = 0;\n" .
	  "SET auto_explain_z.log_analyze = " . ($mode->{analyze} ? 'on' : 'off') . ";\n" .
	  "SET auto_explain_z.log_verbose = " . ($mode->{verbose} ? 'on' : 'off') . ";\n" .
	  "SET auto_explain_z.log_timing = " . ($mode->{timing} ? 'on' : 'off') . ";\n" .
	  "SET auto_explain_z.log_buffers = " . ($mode->{buffers} ? 'on' : 'off') . ";\n" .
	  "SET auto_explain_z.log_wal = " . ($mode->{wal} ? 'on' : 'off') . ";\n";
}

sub planner_setup
{
	my ($case) = @_;
	return "RESET ALL;\nSET jit = off;\n" . ($case->{setup} || '');
}

sub actual_explain
{
	my ($node, $case, $mode, $format) = @_;
	my $options = explain_options($mode, $format);
	my $sql =
	  planner_setup($case) .
	  "SET auto_explain_z.log_min_duration = -1;\n" .
	  "EXPLAIN ($options) $case->{sql}";
	my $out = $node->safe_psql('postgres', $sql);
	$out =~ s/\R\z//;
	return $out;
}

sub decoded_explain
{
	my ($node, $data_dir, $case, $mode, $format, $suffix) = @_;
	my $dir = "$data_dir/aez_equiv_${suffix}";
	my $sql =
	  planner_setup($case) .
	  aez_mode_gucs($mode, $dir) .
	  "$case->{sql}";

	$node->safe_psql('postgres', $sql);
	return decode_files($dir, $format);
}

sub same_execution_explain
{
	my ($node, $data_dir, $case, $mode, $format, $suffix) = @_;
	my $dir = "$data_dir/aez_equiv_${suffix}";
	my $options = explain_options($mode, $format);
	my $sql =
	  planner_setup($case) .
	  aez_mode_gucs($mode, $dir) .
	  "EXPLAIN ($options) $case->{sql}";

	my $expected = $node->safe_psql('postgres', $sql);
	$expected =~ s/\R\z//;
	return ($expected, decode_files($dir, $format));
}

sub expected_and_decoded
{
	my ($node, $data_dir, $case, $mode, $format, $suffix) = @_;

	if ($mode->{analyze})
	{
		return same_execution_explain($node, $data_dir, $case, $mode, $format,
			$suffix);
	}

	return (
		actual_explain($node, $case, $mode, $format),
		decoded_explain($node, $data_dir, $case, $mode, $format, $suffix));
}

sub compare_exact
{
	my ($got, $expected, $name) = @_;

	is($got, $expected, $name);
}

my $node = PostgreSQL::Test::Cluster->new('decoder_equivalence');
$node->init;

my $data_dir = abs_path($node->data_dir);
my $module_dir = "$FindBin::Bin/..";
my $fdw_file = "$data_dir/aez_equiv_fdw.csv";

$node->append_conf(
	'postgresql.conf',
	qq{
dynamic_library_path = '$module_dir:\$libdir'
session_preload_libraries = 'auto_explain_z,auto_explain_z_testscan'
compute_query_id = on
jit = off
auto_explain_z.log_min_duration = -1
auto_explain_z.compression = none
});
$node->start;

append_to_file($fdw_file, "1,alpha\n2,beta\n3,gamma\n");

$node->safe_psql(
	'postgres', q{
CREATE TABLE aez_t(id int primary key, payload text);
INSERT INTO aez_t SELECT g, repeat('x', 32) FROM generate_series(1, 100) g;
VACUUM ANALYZE aez_t;

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
FROM generate_series(1, 400) g;
INSERT INTO aez_order
SELECT g,
       (g % 400) + 1,
       CASE WHEN g % 4 = 0 THEN 'paid'
            WHEN g % 4 = 1 THEN 'open'
            ELSE 'closed' END,
       (g * 17) % 1000,
       g % 365
FROM generate_series(1, 8000) g;
CREATE INDEX aez_customer_region_idx ON aez_customer(region);
CREATE INDEX aez_order_customer_idx ON aez_order(customer_id);
CREATE INDEX aez_order_status_customer_idx ON aez_order(status, customer_id);
CREATE INDEX aez_order_amount_idx ON aez_order(amount);

CREATE TABLE aez_bitmap(id int primary key, a int not null, b int not null);
INSERT INTO aez_bitmap
SELECT g, g % 1000, (g * 7) % 1000
FROM generate_series(1, 20000) g;
CREATE INDEX aez_bitmap_a_idx ON aez_bitmap(a);
CREATE INDEX aez_bitmap_b_idx ON aez_bitmap(b);

CREATE TABLE aez_pair(id int primary key, customer_id int not null);
INSERT INTO aez_pair SELECT g, (g % 400) + 1 FROM generate_series(1, 1600) g;
CREATE INDEX aez_pair_customer_idx ON aez_pair(customer_id);

CREATE TABLE aez_mod(id int primary key, payload text);
INSERT INTO aez_mod VALUES (1, 'base');

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

ANALYZE aez_customer;
ANALYZE aez_order;
ANALYZE aez_bitmap;
ANALYZE aez_pair;
ANALYZE aez_mod;
ANALYZE aez_customscan;
ANALYZE aez_parallel;
ANALYZE aez_part;
});

$node->safe_psql(
	'postgres',
	"CREATE EXTENSION file_fdw;
CREATE SERVER aez_file_srv FOREIGN DATA WRAPPER file_fdw;
CREATE FOREIGN TABLE aez_fdw(id int, label text)
SERVER aez_file_srv
OPTIONS (filename " . sql_string($fdw_file) . ", format 'csv');");

my @node_cases = (
	{
		name => 'result',
		targets => ['Result'],
		sql => 'SELECT 1 WHERE true;',
	},
	{
		name => 'seq_scan',
		targets => ['Seq Scan'],
		setup => "SET enable_indexscan = off;\nSET enable_bitmapscan = off;\n",
		sql => "SELECT count(*) FROM aez_t WHERE payload = repeat('x', 32);",
	},
	{
		name => 'index_scan',
		targets => ['Index Scan'],
		setup => "SET enable_seqscan = off;\n",
		sql => 'SELECT * FROM aez_t WHERE id = 7;',
	},
	{
		name => 'index_only_scan',
		targets => ['Index Only Scan'],
		setup => "SET enable_seqscan = off;\nSET enable_bitmapscan = off;\n",
		sql => 'SELECT id FROM aez_t WHERE id BETWEEN 1 AND 5;',
	},
	{
		name => 'bitmap_or',
		targets => ['Bitmap Heap Scan', 'Bitmap Index Scan', 'BitmapOr'],
		setup => "SET enable_seqscan = off;\nSET enable_indexscan = off;\n",
		sql => 'SELECT count(*) FROM aez_bitmap WHERE a = 17 OR b = 23;',
	},
	{
		name => 'bitmap_and',
		targets => ['Bitmap Heap Scan', 'Bitmap Index Scan', 'BitmapAnd'],
		setup => "SET enable_seqscan = off;\nSET enable_indexscan = off;\n",
		sql => 'SELECT count(*) FROM aez_bitmap WHERE a = 17 AND b = 23;',
	},
	{
		name => 'tid_scan',
		targets => ['Tid Scan'],
		setup => "SET enable_seqscan = off;\n",
		sql => "SELECT * FROM aez_t WHERE ctid = '(0,1)'::tid;",
	},
	{
		name => 'tid_range_scan',
		targets => ['Tid Range Scan'],
		setup => "SET enable_seqscan = off;\n",
		sql => "SELECT count(*) FROM aez_t WHERE ctid >= '(0,1)'::tid AND ctid < '(0,10)'::tid;",
	},
	{
		name => 'sample_scan',
		targets => ['Sample Scan'],
		sql => 'SELECT count(*) FROM aez_t TABLESAMPLE SYSTEM (100);',
	},
	{
		name => 'values_scan',
		targets => ['Values Scan'],
		sql => 'SELECT * FROM (VALUES (1), (2)) v(x);',
	},
	{
		name => 'function_scan',
		targets => ['Function Scan'],
		sql => 'SELECT * FROM generate_series(1, 3) g;',
	},
	{
		name => 'project_set',
		targets => ['ProjectSet'],
		sql => 'SELECT generate_series(1, 3);',
	},
	{
		name => 'cte_scan',
		targets => ['CTE Scan'],
		sql => 'WITH c AS MATERIALIZED (SELECT * FROM aez_t WHERE id <= 3) SELECT count(*) FROM c;',
	},
	{
		name => 'recursive_union',
		targets => ['Recursive Union', 'WorkTable Scan'],
		sql => 'WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 4) SELECT sum(n) FROM r;',
	},
	{
		name => 'subquery_scan',
		targets => ['Subquery Scan'],
		sql => 'SELECT * FROM (SELECT * FROM aez_t LIMIT 5) s WHERE id > 0;',
	},
		{
			name => 'limit_sort',
			targets => ['Limit', 'Sort'],
			setup => "SET enable_indexscan = off;\nSET enable_indexonlyscan = off;\n",
			sql => 'SELECT * FROM aez_order ORDER BY amount DESC LIMIT 5;',
		},
	{
		name => 'incremental_sort',
		targets => ['Incremental Sort'],
		setup => "SET enable_sort = off;\nSET enable_incremental_sort = on;\n",
		sql => 'SELECT * FROM aez_customer ORDER BY region, id LIMIT 20;',
	},
	{
		name => 'group',
		targets => ['Group'],
		setup => "SET enable_hashagg = off;\n",
		sql => 'SELECT region FROM aez_customer GROUP BY region ORDER BY region;',
	},
	{
		name => 'aggregate',
		targets => ['Aggregate'],
		sql => "SELECT count(*), sum(amount) FROM aez_order WHERE status = 'paid';",
	},
	{
		name => 'windowagg',
		targets => ['WindowAgg'],
		sql => 'SELECT id, row_number() OVER (PARTITION BY region ORDER BY id) FROM aez_customer LIMIT 20;',
	},
	{
		name => 'unique',
		targets => ['Unique'],
		setup => "SET enable_hashagg = off;\n",
		sql => 'SELECT DISTINCT region FROM aez_customer ORDER BY region;',
	},
	{
		name => 'setop',
		targets => ['SetOp'],
		setup => "SET enable_hashagg = off;\n",
		sql => 'SELECT region FROM aez_customer INTERSECT SELECT region FROM aez_customer WHERE active;',
	},
	{
		name => 'hash_join',
		targets => ['Hash Join', 'Hash'],
		setup => "SET enable_mergejoin = off;\nSET enable_nestloop = off;\n",
		sql => 'SELECT count(*) FROM aez_order o JOIN aez_customer c ON c.id = o.customer_id WHERE c.region = 3;',
	},
	{
		name => 'merge_join',
		targets => ['Merge Join'],
		setup => "SET enable_hashjoin = off;\nSET enable_nestloop = off;\nSET enable_mergejoin = on;\n",
		sql => 'SELECT count(*) FROM aez_order o JOIN aez_customer c ON c.id = o.customer_id;',
	},
	{
		name => 'nested_loop_memoize',
		targets => ['Nested Loop', 'Memoize'],
		setup => "SET enable_hashjoin = off;\nSET enable_mergejoin = off;\nSET enable_nestloop = on;\nSET enable_memoize = on;\n",
		sql => 'SELECT count(*) FROM aez_pair p JOIN aez_customer c ON c.id = p.customer_id;',
	},
	{
		name => 'materialize',
		targets => ['Materialize'],
		setup => "SET enable_hashjoin = off;\nSET enable_mergejoin = off;\nSET enable_memoize = off;\n",
		sql => 'SELECT count(*) FROM (SELECT * FROM aez_customer WHERE region = 1) c JOIN (SELECT * FROM aez_t) t ON true;',
	},
	{
		name => 'append',
		targets => ['Append'],
		sql => 'SELECT count(*) FROM aez_part WHERE id BETWEEN 1 AND 220;',
	},
		{
			name => 'merge_append',
			targets => ['Merge Append'],
			setup => "SET enable_sort = off;\n",
			sql => 'SELECT * FROM (SELECT * FROM aez_part_1 UNION ALL SELECT * FROM aez_part_2 UNION ALL SELECT * FROM aez_part_3) s ORDER BY id LIMIT 10;',
		},
	{
		name => 'lockrows',
		targets => ['LockRows'],
		sql => 'SELECT * FROM aez_t WHERE id = 1 FOR UPDATE;',
	},
	{
		name => 'modifytable',
		targets => ['ModifyTable'],
		sql => "UPDATE aez_mod SET payload = 'u' WHERE id = 1;",
	},
		{
			name => 'gather',
			targets => ['Gather'],
			setup => "SET max_parallel_workers_per_gather = 4;\nSET min_parallel_table_scan_size = 0;\nSET parallel_setup_cost = 0;\nSET parallel_tuple_cost = 0;\n",
			sql => 'SELECT count(*) FROM aez_parallel WHERE bucket >= 0;',
		},
	{
		name => 'gather_merge',
		targets => ['Gather Merge'],
		setup => "SET max_parallel_workers_per_gather = 4;\nSET min_parallel_table_scan_size = 0;\nSET parallel_setup_cost = 0;\nSET parallel_tuple_cost = 0;\n",
		sql => 'SELECT * FROM aez_parallel ORDER BY bucket, id LIMIT 50;',
	},
	{
		name => 'foreign_scan',
		targets => ['Foreign Scan'],
		sql => 'SELECT * FROM aez_fdw WHERE id > 1;',
	},
	{
		name => 'custom_scan',
		targets => ['Custom Scan'],
		sql => 'SELECT count(*) FROM aez_customscan;',
	},
);

my ($tf_ret, $tf_stdout, $tf_stderr) = $node->psql(
	'postgres',
	q{\set ON_ERROR_STOP on
SELECT *
FROM XMLTABLE(
	'/rows/r'
	PASSING XMLPARSE(DOCUMENT '<rows><r><x>1</x></r><r><x>2</x></r></rows>')
	COLUMNS x int PATH 'x'
) AS xt;});
if ($tf_ret == 0)
{
	push @node_cases,
	  {
		name => 'table_function_scan',
		targets => ['Table Function Scan'],
		sql => q{SELECT *
FROM XMLTABLE(
	'/rows/r'
	PASSING XMLPARSE(DOCUMENT '<rows><r><x>1</x></r><r><x>2</x></r></rows>')
	COLUMNS x int PATH 'x'
) AS xt;},
	  };
}
else
{
	note("XMLTABLE unavailable, Table Function Scan equivalence skipped: $tf_stderr");
}

my @formats = qw(text json yaml xml);
my @modes = (
	{
		name => 'plain',
		analyze => 0,
		verbose => 0,
		timing => 0,
		buffers => 0,
		wal => 0,
	},
	{
		name => 'verbose',
		analyze => 0,
		verbose => 1,
		timing => 0,
		buffers => 0,
		wal => 0,
	},
	{
		name => 'analyze_rows',
		analyze => 1,
		verbose => 0,
		timing => 0,
		buffers => 0,
		wal => 0,
	},
	{
		name => 'analyze_timing',
		analyze => 1,
		verbose => 0,
		timing => 1,
		buffers => 0,
		wal => 0,
	},
	{
		name => 'analyze_buffers',
		analyze => 1,
		verbose => 0,
		timing => 0,
		buffers => 1,
		wal => 0,
	},
	{
		name => 'analyze_wal',
		analyze => 1,
		verbose => 0,
		timing => 0,
		buffers => 0,
		wal => 1,
	},
);

my $plain = $modes[0];

for my $case (@node_cases)
{
	my $suffix = "coverage_$case->{name}";
	my $dir = "$data_dir/aez_equiv_$suffix";
	$node->safe_psql(
		'postgres',
		planner_setup($case) . aez_mode_gucs($plain, $dir) . "$case->{sql}");
	my $records = decode_json_records($dir);
	my %seen;
	for my $record (@$records)
	{
		collect_node_types($record->{Plan}, \%seen);
	}
	for my $target (@{ $case->{targets} })
	{
		ok($seen{$target}, "$case->{name} decoded $target");
	}
}

# Transition-table plans are generated by a nested statement inside the trigger
# function, so they are node-coverage only rather than a direct EXPLAIN
# equivalence case.
my $transition_dir = "$data_dir/aez_equiv_named_tuplestore";
$node->safe_psql(
	'postgres',
	"RESET ALL;\nSET jit = off;\n" .
	aez_mode_gucs($plain, $transition_dir) .
	"SET auto_explain_z.log_nested_statements = on;\n" .
	"INSERT INTO aez_transition_src SELECT g FROM generate_series(1, 3) g;");
my $transition_records = decode_json_records($transition_dir);
my %transition_nodes;
for my $record (@$transition_records)
{
	collect_node_types($record->{Plan}, \%transition_nodes);
}
ok($transition_nodes{'Named Tuplestore Scan'},
	'nested transition-table statement decoded Named Tuplestore Scan');

my @equivalence_cases =
  $full_matrix
  ? @node_cases
  : grep {
	$_->{name} =~ /^(result|index_scan|bitmap_or|hash_join|modifytable|foreign_scan|custom_scan|gather)$/
  } @node_cases;

for my $case (@equivalence_cases)
{
	for my $mode (@modes)
	{
		for my $format (@formats)
		{
			my $suffix = join('_', 'strict', $case->{name}, $mode->{name}, $format);
			my ($expected, $got) =
			  expected_and_decoded($node, $data_dir, $case, $mode, $format, $suffix);
			compare_exact(
				$got, $expected,
				"decoder $format $mode->{name} output exactly matches EXPLAIN for $case->{name}");
		}
	}
}

$node->stop;

done_testing();
