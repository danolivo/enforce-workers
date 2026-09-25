# contrib/enforce_workers/Makefile

MODULE_big = enforce_workers
OBJS = \
	$(WIN32RES) \
	enforce_workers.o \
	nlguard.o \
	seqguard.o \
	idxdefer.o

EXTENSION = enforce_workers
DATA = enforce_workers--1.0.sql

PGFILEDESC = "enforce_workers - planner overrides for parallelism and nested loops"

REGRESS = enforce_workers nlguard seqguard idxdefer

# idxdefer's test verifies the rebuilt indexes with amcheck when it is there.
EXTRA_INSTALL = contrib/amcheck

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/enforce_workers
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
