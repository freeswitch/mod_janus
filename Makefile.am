-include $(top_srcdir)/build/modmake.rulesam
AUTOMAKE_OPTIONS = foreign subdir-objects
MODNAME=mod_janus

LIBS := $(if $(switch_builddir),$(switch_builddir)/libfreeswitch.la,)

mod_LTLIBRARIES = mod_janus.la
mod_janus_la_SOURCES  = globals.c cJSON.c http.c api.c servers.c hash.c mod_janus.c
mod_janus_la_CFLAGS   = $(AM_CFLAGS) $(FREESWITCH_CFLAGS)
mod_janus_la_LDFLAGS  = -avoid-version -module -no-undefined -shared $(FREESWITCH_LIBS) $(OPENSSL_LIBS) $(MOSQUITTO_LIBS)
mod_janus_la_LIBADD   = $(LIBS)