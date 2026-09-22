# contrib/enforce-workers/Makefile

MODULE_big = enforce_workers
OBJS = \
	$(WIN32RES) \
	enforce_workers.o

PGFILEDESC = "enforce_workers - make parallel workers available on any relation"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/enforce-workers
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
