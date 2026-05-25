# auto_explain_z/Makefile

MODULE_big = auto_explain_z
OBJS = \
	$(WIN32RES) \
	auto_explain_z.o
MODULES = auto_explain_z_testscan

PGFILEDESC = "auto_explain_z - binary compressed execution plan logger"

SCRIPTS = \
	auto_explain_z_bench \
	auto_explain_z_bench_all_modes \
	auto_explain_z_bench_compression \
	auto_explain_z_bench_formats \
	auto_explain_z_bench_profiles \
	auto_explain_z_bench_shapes \
	auto_explain_z_bench_templates \
	auto_explain_z_dump

EXTENSION = auto_explain_z
DATA = auto_explain_z--1.0.sql

SHLIB_LINK += $(filter -llz4 -lzstd, $(LIBS))

TAP_TESTS = 1
EXTRA_CLEAN = __pycache__ log results

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

PG_TEST_PERL_DIR ?= $(firstword $(wildcard $(top_srcdir)/src/test/perl $(top_builddir)/src/test/perl ../postgres/src/test/perl ../postgres-*/src/test/perl))
TAP_PROVE ?= prove
TAP_PERL5LIB ?= $(shell perl -MConfig -e 'print join(":", grep { -d $$_ } ("$$ENV{HOME}/perl5/lib/perl5", "$$ENV{HOME}/perl5/lib/perl5/$$Config{archname}"))')
TAP_ENV ?=

.PHONY: tapcheck fulltapcheck
tapcheck: all
	@if test -z "$(PG_TEST_PERL_DIR)"; then \
		echo "PG_TEST_PERL_DIR not found; set PG_TEST_PERL_DIR=/path/to/postgresql/src/test/perl"; \
		exit 2; \
	fi
	@echo "# +++ tapcheck using $(PG_TEST_PERL_DIR) +++"
	rm -rf '$(CURDIR)'/tmp_check
	$(MKDIR_P) '$(CURDIR)'/tmp_check
	$(TAP_ENV) PATH="$(bindir):$(CURDIR):$$PATH" \
	PERL5LIB='$(PG_TEST_PERL_DIR):$(CURDIR):$(TAP_PERL5LIB):'$$PERL5LIB \
	PG_REGRESS='$(top_builddir)/src/test/regress/pg_regress' \
	TESTLOGDIR='$(CURDIR)/tmp_check/log' \
	TESTDATADIR='$(CURDIR)/tmp_check' \
	$(TAP_PROVE) -I '$(PG_TEST_PERL_DIR)' -I '$(CURDIR)' $(PROVE_FLAGS) $(if $(PROVE_TESTS),$(PROVE_TESTS),t/*.pl)

fulltapcheck:
	$(MAKE) tapcheck TAP_ENV='AEZ_DECODER_EQUIV_FULL_MATRIX=1' PROVE_TESTS='t/002_auto_explain_z_decoder_equivalence.pl'
