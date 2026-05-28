# auto_explain_z/Makefile

MODULE_big = auto_explain_z
OBJS = \
	$(WIN32RES) \
	auto_explain_z.o

AEZ_TEST_MODULES = auto_explain_z_testscan
ifeq ($(AEZ_BUILD_TEST_MODULES),1)
MODULES = $(AEZ_TEST_MODULES)
endif

PGFILEDESC = "auto_explain_z - binary compressed execution plan logger"

AEZ_DUMP = auto_explain_z_dump

EXTENSION = auto_explain_z
DATA = auto_explain_z--1.0.sql

PG_CONFIG ?= $(if $(AEZ_PG_CONFIG),$(AEZ_PG_CONFIG),pg_config)

AEZ_PG_CONFIGURE := $(shell $(PG_CONFIG) --configure)
AEZ_INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)
AEZ_CPPFLAGS =
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/commands/explain_format.h' && echo yes))
AEZ_CPPFLAGS += -DAEZ_HAVE_EXPLAIN_SPLIT_HEADERS
endif
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/executor/instrument.h' && grep -q 'wal_buffers_full' '$(AEZ_INCLUDEDIR_SERVER)/executor/instrument.h' && echo yes))
AEZ_CPPFLAGS += -DAEZ_HAVE_WAL_BUFFERS_FULL
endif
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/nodes/plannodes.h' && grep -q 'winname' '$(AEZ_INCLUDEDIR_SERVER)/nodes/plannodes.h' && echo yes))
AEZ_CPPFLAGS += -DAEZ_HAVE_WINDOW_AGG_WINNAME
endif
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/access/genam.h' && grep -q 'SharedIndexScanInstrumentation' '$(AEZ_INCLUDEDIR_SERVER)/access/genam.h' && echo yes))
AEZ_CPPFLAGS += -DAEZ_HAVE_INDEX_SCAN_INSTRUMENTATION
endif
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/utils/tuplestore.h' && grep -q 'tuplestore_get_stats' '$(AEZ_INCLUDEDIR_SERVER)/utils/tuplestore.h' && echo yes))
AEZ_CPPFLAGS += -DAEZ_HAVE_TUPLESTORE_STATS
endif
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/nodes/execnodes.h' && grep -q 'mt_mergeActionLists' '$(AEZ_INCLUDEDIR_SERVER)/nodes/execnodes.h' && echo yes))
AEZ_CPPFLAGS += -DAEZ_HAVE_MERGE_ACTION_LISTS
endif
PG_CPPFLAGS += $(AEZ_CPPFLAGS)

AEZ_COMPRESSION_LIBS_RAW =
ifneq (,$(findstring --with-lz4,$(AEZ_PG_CONFIGURE)))
AEZ_COMPRESSION_LIBS_RAW += -llz4
endif
ifneq (,$(findstring --with-zstd,$(AEZ_PG_CONFIGURE)))
AEZ_COMPRESSION_LIBS_RAW += -lzstd
endif
ifeq (,$(filter -llz4,$(AEZ_COMPRESSION_LIBS_RAW)))
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/pg_config.h' && grep -q '^#define USE_LZ4 1' '$(AEZ_INCLUDEDIR_SERVER)/pg_config.h' && echo yes))
AEZ_COMPRESSION_LIBS_RAW += -llz4
endif
endif
ifeq (,$(filter -lzstd,$(AEZ_COMPRESSION_LIBS_RAW)))
ifneq (,$(shell test -f '$(AEZ_INCLUDEDIR_SERVER)/pg_config.h' && grep -q '^#define USE_ZSTD 1' '$(AEZ_INCLUDEDIR_SERVER)/pg_config.h' && echo yes))
AEZ_COMPRESSION_LIBS_RAW += -lzstd
endif
endif

TAP_TESTS = 1
EXTRA_CLEAN = __pycache__ log results \
	$(AEZ_DUMP) $(AEZ_DUMP).o $(AEZ_DUMP).bc \
	$(addsuffix $(DLSUFFIX), $(AEZ_TEST_MODULES)) \
	$(addsuffix .o, $(AEZ_TEST_MODULES)) \
	$(addsuffix .bc, $(AEZ_TEST_MODULES))

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

AEZ_COMPRESSION_LIBS := $(filter-out $(LIBS),$(AEZ_COMPRESSION_LIBS_RAW))
AEZ_MATH_LIB := $(if $(filter -lm,$(LIBS)),,-lm)

SHLIB_LINK += $(filter -llz4 -lzstd, $(LIBS)) $(AEZ_COMPRESSION_LIBS)

all: $(AEZ_DUMP)

$(AEZ_DUMP): $(AEZ_DUMP).o
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) $(AEZ_COMPRESSION_LIBS) $(AEZ_MATH_LIB) -o $@

install: install-aez-dump
install-aez-dump: $(AEZ_DUMP) installdirs
	$(MKDIR_P) '$(DESTDIR)$(bindir)'
	$(INSTALL_PROGRAM) $(AEZ_DUMP)$(X) '$(DESTDIR)$(bindir)/'

uninstall: uninstall-aez-dump
uninstall-aez-dump:
	rm -f '$(DESTDIR)$(bindir)/$(AEZ_DUMP)$(X)'

PG_TEST_PERL_DIR ?= $(AEZ_PG_TEST_PERL_DIR)
AEZ_MODULE_DIR ?= $(CURDIR)
AEZ_DUMP_PATH ?= $(CURDIR)/$(AEZ_DUMP)
TAP_PROVE ?= prove
TAP_PERL5LIB ?= $(shell perl -MConfig -e 'print join(":", grep { -d $$_ } ("$$ENV{HOME}/perl5/lib/perl5", "$$ENV{HOME}/perl5/lib/perl5/$$Config{archname}"))')
TAP_ENV ?=

.PHONY: aez-test-modules tapcheck fulltapcheck
aez-test-modules:
	$(MAKE) AEZ_BUILD_TEST_MODULES=1 $(addsuffix $(DLSUFFIX), $(AEZ_TEST_MODULES))

tapcheck: all aez-test-modules
	@if test -z "$(PG_TEST_PERL_DIR)"; then \
		echo "PG_TEST_PERL_DIR not set; export PG_TEST_PERL_DIR or AEZ_PG_TEST_PERL_DIR"; \
		exit 2; \
	fi
	@echo "# +++ tapcheck using $(PG_TEST_PERL_DIR) +++"
	rm -rf '$(CURDIR)'/tmp_check
	$(MKDIR_P) '$(CURDIR)'/tmp_check
	$(TAP_ENV) PATH="$(bindir):$(CURDIR):$$PATH" \
	LD_LIBRARY_PATH='$(libdir):'$$LD_LIBRARY_PATH \
	DYLD_LIBRARY_PATH='$(libdir):'$$DYLD_LIBRARY_PATH \
	PG_CONFIG='$(PG_CONFIG)' \
	AEZ_MODULE_DIR='$(AEZ_MODULE_DIR)' \
	AEZ_DUMP='$(AEZ_DUMP_PATH)' \
	PERL5LIB='$(PG_TEST_PERL_DIR):$(CURDIR):$(TAP_PERL5LIB):'$$PERL5LIB \
	PG_REGRESS='$(top_builddir)/src/test/regress/pg_regress' \
	TESTLOGDIR='$(CURDIR)/tmp_check/log' \
	TESTDATADIR='$(CURDIR)/tmp_check' \
	$(TAP_PROVE) -I '$(PG_TEST_PERL_DIR)' -I '$(CURDIR)' $(PROVE_FLAGS) $(if $(PROVE_TESTS),$(PROVE_TESTS),t/*.pl)

fulltapcheck:
	$(MAKE) tapcheck TAP_ENV='AEZ_DECODER_EQUIV_FULL_MATRIX=1' PROVE_TESTS='t/002_auto_explain_z_decoder_equivalence.pl'
