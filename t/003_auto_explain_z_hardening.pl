# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use Cwd qw(abs_path);
use File::Basename qw(basename);
use File::Copy qw(copy);
use FindBin;
use IPC::Run;
use JSON::PP;
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

sub aez_files
{
	my ($dir, $prefix) = @_;
	my @files = sort glob("$dir/*.aez");
	if (defined $prefix)
	{
		@files = grep { basename($_) =~ /^\Q$prefix\E-/ } @files;
	}
	return @files;
}

sub decode_raw_json_dir
{
	my ($dir) = @_;
	my @files = aez_files($dir);
	ok(scalar(@files) > 0, "$dir contains auto_explain_z binary logs");

	my ($stdout, $stderr) =
	  run_command([ $decoder, '--raw', '--format', 'json', @files ]);
	is($stderr, '', "raw decoder stderr is empty for $dir");
	return decode_json($stdout);
}

sub decode_pg_json_files
{
	my (@files) = @_;
	ok(scalar(@files) > 0, 'decoder received binary log files');

	my ($stdout, $stderr) =
	  run_command([ $decoder, '--format', 'json', @files ]);
	is($stderr, '', 'postgres-style json decoder stderr is empty');
	return $stdout;
}

sub write_file_prefix
{
	my ($src, $dst, $bytes) = @_;

	open(my $in, '<', $src) or die "could not open $src: $!";
	binmode($in);
	open(my $out, '>', $dst) or die "could not create $dst: $!";
	binmode($out);

	my $buf = '';
	my $read = read($in, $buf, $bytes);
	die "could not read $src: $!" unless defined $read;
	print $out $buf;

	close($out);
	close($in);
}

sub overwrite_file_start
{
	my ($path, $bytes) = @_;

	open(my $fh, '+<', $path) or die "could not open $path: $!";
	binmode($fh);
	print $fh $bytes;
	close($fh);
}

sub workload_sql
{
	my ($dir, $prefix, $worker, $queries) = @_;
	my $sql =
	  "SET auto_explain_z.directory = " . sql_string($dir) . ";\n" .
	  "SET auto_explain_z.file_prefix = " . sql_string($prefix) . ";\n" .
	  "SET auto_explain_z.log_min_duration = 0;\n" .
	  "SET auto_explain_z.profile = simple;\n" .
	  "SET auto_explain_z.compression = none;\n" .
	  "SET auto_explain_z.pending_buffer_size = 0;\n" .
	  "SET auto_explain_z.template_cache = on;\n" .
	  "SET auto_explain_z.template_min_plan_bytes = 0;\n" .
	  "SELECT pg_sleep(0.05);\n";

	for my $i (1 .. $queries)
	{
		my $lo = (($worker * 97 + $i * 13) % 900) + 1;
		my $hi = $lo + 20;
		$sql .= "SELECT count(*) FROM aez_harden WHERE id BETWEEN $lo AND $hi;\n";
	}
	return $sql;
}

sub try_psql_sql
{
	my ($node, $sql) = @_;
	my ($stdout, $stderr) = ('', '');
	my @cmd = (
		$node->installed_command('psql'),
		'--no-psqlrc',
		'--no-align',
		'--tuples-only',
		'--quiet',
		'--set' => 'ON_ERROR_STOP=1',
		'--dbname' => $node->connstr('postgres'),
		'--file' => '-');
	my $ok = IPC::Run::run \@cmd, '<', \$sql, '>' => \$stdout, '2>' => \$stderr;
	return ($ok, $stdout, $stderr);
}

my $node = PostgreSQL::Test::Cluster->new('hardening');
$node->init;

my $data_dir = abs_path($node->data_dir);
my $module_dir = "$FindBin::Bin/..";
my $extshare = "$data_dir/extshare";
my $good_dir = "$data_dir/aez_good";
my $manual_rotation_dir = "$data_dir/aez_manual_rotation";
my $size_rotation_dir = "$data_dir/aez_size_rotation";
my $filename_rotation_dir = "$data_dir/aez_filename_rotation";
my $template_dir = "$data_dir/aez_templates";
my $multi_dir = "$data_dir/aez_multi";
my $retention_dir = "$data_dir/aez_retention";

mkdir $extshare;
mkdir "$extshare/extension";
copy("$module_dir/auto_explain_z.control",
	"$extshare/extension/auto_explain_z.control")
  or die "could not stage auto_explain_z.control: $!";
copy("$module_dir/auto_explain_z--1.0.sql",
	"$extshare/extension/auto_explain_z--1.0.sql")
  or die "could not stage auto_explain_z--1.0.sql: $!";

$node->append_conf(
	'postgresql.conf',
	qq{
dynamic_library_path = '$module_dir:\$libdir'
extension_control_path = '$extshare:\$system'
shared_preload_libraries = 'auto_explain_z'
compute_query_id = on
jit = off
auto_explain_z.log_min_duration = -1
auto_explain_z.compression = none
});
$node->start;

my $extversion = $node->safe_psql(
	'postgres',
	q{
CREATE EXTENSION auto_explain_z;
SELECT extversion FROM pg_extension WHERE extname = 'auto_explain_z';
});
is($extversion, '1.0', 'CREATE EXTENSION auto_explain_z registers metadata');
my $rotate_acl = $node->safe_psql(
	'postgres',
	q{
SELECT has_function_privilege('public',
	'auto_explain_z_rotate_logfile()', 'EXECUTE');
});
is($rotate_acl, 'f',
	'auto_explain_z_rotate_logfile execute is revoked from public');

$node->safe_psql(
	'postgres', q{
CREATE TABLE aez_harden(id int primary key, payload text);
INSERT INTO aez_harden
SELECT g, repeat('h', 64) FROM generate_series(1, 1000) g;
ANALYZE aez_harden;
});

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($good_dir) . q{;
SET auto_explain_z.file_prefix = 'good';
SET auto_explain_z.log_min_duration = 0;
SET auto_explain_z.profile = full;
SET auto_explain_z.compression = none;
SET auto_explain_z.pending_buffer_size = 0;
SET auto_explain_z.template_cache = off;
SELECT count(*) FROM aez_harden WHERE id BETWEEN 10 AND 30;
});

my @good_files = aez_files($good_dir, 'good');
ok(scalar(@good_files) == 1, 'single uncompressed source file created');
my $good_json = decode_pg_json_files(@good_files);
like($good_json, qr/"Node Type": "Aggregate"/,
	'known-good uncompressed file decodes before corruption checks');
my $good_raw = decode_raw_json_dir($good_dir);
for my $record (@$good_raw)
{
	is($record->{Template}->{Mode}, 'none',
		'template_cache=off emits standalone records only');
}

my $rotate_result = $node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($manual_rotation_dir) . q{;
SET auto_explain_z.file_prefix = 'manual';
SET auto_explain_z.log_min_duration = 0;
SET auto_explain_z.profile = simple;
SET auto_explain_z.compression = none;
SET auto_explain_z.pending_buffer_size = 0;
SET auto_explain_z.template_cache = off;
SELECT count(*) FROM aez_harden WHERE id BETWEEN 31 AND 40;
SELECT auto_explain_z_rotate_logfile();
SELECT count(*) FROM aez_harden WHERE id BETWEEN 41 AND 50;
});
like($rotate_result, qr/^t$/m,
	'auto_explain_z_rotate_logfile reports a successful rotation request');
my @manual_rotation_files = aez_files($manual_rotation_dir, 'manual');
ok(scalar(@manual_rotation_files) >= 2,
	'manual rotation creates a new shared binary log file');
my $manual_rotation_json = decode_pg_json_files(@manual_rotation_files);
like($manual_rotation_json, qr/"Node Type": "Aggregate"/,
	'decoder reads manually rotated files');

my $external_rotate_result = $node->safe_psql(
	'postgres',
	q{SELECT auto_explain_z_rotate_logfile();});
like($external_rotate_result, qr/^t$/m,
	'external backend can request shared log rotation');
$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($manual_rotation_dir) . q{;
SET auto_explain_z.file_prefix = 'manual';
SET auto_explain_z.log_min_duration = 0;
SET auto_explain_z.profile = simple;
SET auto_explain_z.compression = none;
SET auto_explain_z.pending_buffer_size = 0;
SET auto_explain_z.template_cache = off;
SELECT count(*) FROM aez_harden WHERE id BETWEEN 51 AND 60;
});
@manual_rotation_files = aez_files($manual_rotation_dir, 'manual');
ok(scalar(@manual_rotation_files) >= 3,
	'external rotation request creates the next shared binary log');

my $large_query_text = 'x' x 4096;
$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($size_rotation_dir) . q{;
SET auto_explain_z.file_prefix = 'size';
SET auto_explain_z.log_filename = '';
SET auto_explain_z.log_min_duration = 0;
SET auto_explain_z.profile = simple;
SET auto_explain_z.compression = none;
SET auto_explain_z.pending_buffer_size = 0;
SET auto_explain_z.template_cache = off;
SET auto_explain_z.log_rotation_size = 1;
SELECT length(} . sql_string($large_query_text) . q{);
SELECT length(} . sql_string($large_query_text) . q{);
SELECT length(} . sql_string($large_query_text) . q{);
});
my @size_rotation_files = aez_files($size_rotation_dir, 'size');
ok(scalar(@size_rotation_files) >= 2,
	'log_rotation_size rotates binary log files');
my $size_rotation_json = decode_pg_json_files(@size_rotation_files);
like($size_rotation_json, qr/"Node Type": "Result"/,
	'decoder reads size-rotated files');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($filename_rotation_dir) . q{;
SET auto_explain_z.file_prefix = 'named';
SET auto_explain_z.log_filename = 'aez-%Y%m%d%H%M%S';
SET auto_explain_z.log_min_duration = 0;
SET auto_explain_z.profile = simple;
SET auto_explain_z.compression = none;
SET auto_explain_z.pending_buffer_size = 0;
SET auto_explain_z.template_cache = off;
SELECT count(*) FROM aez_harden WHERE id BETWEEN 51 AND 60;
});
my @filename_rotation_files = aez_files($filename_rotation_dir, 'named');
ok(scalar(@filename_rotation_files) == 1,
	'log_filename pattern creates a matching binary log file');
like(basename($filename_rotation_files[0]),
	qr/^named-aez-\d{14}-0\.aez$/,
	'log_filename strftime pattern is included in the generated file name');

my $bad_magic = "$data_dir/aez_bad_magic.aez";
copy($good_files[0], $bad_magic) or die "could not copy $good_files[0]: $!";
overwrite_file_start($bad_magic, "\0\0\0\0");
command_fails_like(
	[ $decoder, '--format', 'json', $bad_magic ],
	qr/bad file magic/,
	'decoder rejects a file with bad magic');

my $truncated = "$data_dir/aez_truncated.aez";
my $good_size = -s $good_files[0];
ok($good_size > 64, 'known-good file is large enough to truncate');
write_file_prefix($good_files[0], $truncated, int($good_size / 2));
command_fails_like(
	[ $decoder, '--format', 'json', $truncated ],
	qr/(unexpected end of data|bad record|decompress|trailing payload)/,
	'decoder rejects a truncated file');

$node->safe_psql(
	'postgres',
	"SET auto_explain_z.directory = " . sql_string($template_dir) . q{;
SET auto_explain_z.file_prefix = 'template';
SET auto_explain_z.log_min_duration = 0;
SET auto_explain_z.profile = simple;
SET auto_explain_z.log_analyze = off;
SET auto_explain_z.template_cache = on;
SET auto_explain_z.template_min_plan_bytes = 0;
SET auto_explain_z.template_omit_query_text = on;
PREPARE aez_hardening_template(int, int) AS
	SELECT count(*) FROM aez_harden WHERE id BETWEEN $1 AND $2;
EXECUTE aez_hardening_template(1, 10);
EXECUTE aez_hardening_template(11, 20);
EXECUTE aez_hardening_template(21, 30);
EXECUTE aez_hardening_template(31, 40);
});

my $template_records = decode_raw_json_dir($template_dir);
my %defined_templates;
my $template_refs = 0;
for my $record (@$template_records)
{
	my $template = $record->{Template} || next;
	my $mode = $template->{Mode} || '';
	my $id = $template->{ID};

	if ($mode eq 'define')
	{
		$defined_templates{$id} = 1;
	}
	elsif ($mode eq 'ref')
	{
		$template_refs++;
		ok($defined_templates{$id},
			"template reference $id has an earlier definition in the same file");
	}
}
ok($template_refs > 0, 'templated workload emitted reference records');

my @jobs;
for my $worker (1 .. 4)
{
	my $sql = workload_sql($multi_dir, 'multi', $worker, 20);
	my ($stdout, $stderr) = ('', '');
	my @cmd = (
		$node->installed_command('psql'),
		'--no-psqlrc',
		'--no-align',
		'--tuples-only',
		'--quiet',
		'--set' => 'ON_ERROR_STOP=1',
		'--dbname' => $node->connstr('postgres'),
		'--file' => '-');
	my $h = IPC::Run::start \@cmd, '<', \$sql, '>' => \$stdout, '2>' => \$stderr;
	push @jobs,
	  {
		handle => $h,
		stdout => \$stdout,
		stderr => \$stderr,
		worker => $worker,
	  };
}

for my $job (@jobs)
{
	ok($job->{handle}->finish,
		"concurrent worker $job->{worker} psql finished successfully");
	is(${ $job->{stderr} }, '',
		"concurrent worker $job->{worker} stderr is empty");
}

my @multi_files = aez_files($multi_dir, 'multi');
is(scalar(@multi_files), 1, 'concurrent backends wrote one shared binary log');
my $multi_json = decode_pg_json_files(@multi_files);
like($multi_json, qr/"Node Type": "Aggregate"/,
	'decoder reads logs produced by concurrent backends');
my $multi_raw = decode_raw_json_dir($multi_dir);
my %multi_pids;
for my $record (@$multi_raw)
{
	$multi_pids{ $record->{'Log Context'}->{PID} } = 1
	  if defined $record->{'Log Context'}->{PID};
}
ok(scalar(keys %multi_pids) >= 4,
	'shared binary log preserves backend pid context');

for my $compression ('lz4', 'zstd')
{
	my $compressed_dir = "$data_dir/aez_shared_compressed_$compression";
	my $large_query_text = 'z' x 4096;
	my ($ok, $stdout, $stderr) = try_psql_sql(
		$node,
		"SET auto_explain_z.directory = " . sql_string($compressed_dir) . q{;
	SET auto_explain_z.file_prefix = 'shared-compressed-} . $compression . q{';
	SET auto_explain_z.log_min_duration = 0;
	SET auto_explain_z.profile = simple;
	SET auto_explain_z.pending_buffer_size = 1;
	SET auto_explain_z.template_cache = off;
	SET auto_explain_z.compression = } . sql_string($compression) . q{;
	SELECT length(} . sql_string($large_query_text) . q{);
	SELECT length(} . sql_string($large_query_text) . q{);
	SELECT length(} . sql_string($large_query_text) . q{);
	});

	if ($ok)
	{
		my @compressed_files =
		  aez_files($compressed_dir, "shared-compressed-$compression");
		is(scalar(@compressed_files), 1,
			"$compression shared compressed workload wrote one binary log");
		my ($compressed_json, $compressed_stderr) =
		  run_command([ $decoder, '--raw', '--format', 'json', @compressed_files ]);
		is($compressed_stderr, '',
			"$compression shared compressed raw decoder stderr is empty");
		like($compressed_json, qr/"Compression": "\Q$compression\E"/,
			"$compression shared compressed binary log decodes");
	}
	else
	{
		note("$compression compression unavailable, decode coverage skipped: $stderr");
	}
}

mkdir $retention_dir;
append_to_file("$retention_dir/otherprefix-keep.aez", "keep");
for my $i (1 .. 6)
{
	$node->safe_psql(
		'postgres',
		"SET auto_explain_z.directory = " . sql_string($retention_dir) . q{;
SET auto_explain_z.file_prefix = 'owned';
SET auto_explain_z.log_min_duration = 0;
SET auto_explain_z.profile = simple;
SET auto_explain_z.compression = none;
SET auto_explain_z.pending_buffer_size = 0;
SET auto_explain_z.max_file_size = 1;
SET auto_explain_z.retention_max_files = 2;
SET auto_explain_z.retention_cleanup_interval = 0;
SELECT count(*) FROM aez_harden WHERE id <= 50;
});
}

ok(-e "$retention_dir/otherprefix-keep.aez",
	'retention cleanup leaves unrelated prefixes untouched');
my @owned_files = aez_files($retention_dir, 'owned');
ok(scalar(@owned_files) > 0, 'retention test kept at least one owned file');
ok(scalar(@owned_files) <= 2, 'retention_max_files applies to matching prefix');
my $retention_json = decode_pg_json_files(@owned_files);
like($retention_json, qr/"Node Type": "Aggregate"/,
	'decoder reads retained rotated files');

$node->stop;

done_testing();
