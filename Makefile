# auto_explain_z/Makefile

MODULE_big = auto_explain_z
OBJS = \
	$(WIN32RES) \
	auto_explain_z.o
MODULES = auto_explain_z_testscan

PGFILEDESC = "auto_explain_z - binary compressed execution plan logger"

SCRIPTS = auto_explain_z_bench auto_explain_z_dump

SHLIB_LINK += $(filter -llz4 -lzstd, $(LIBS))

TAP_TESTS = 1

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
