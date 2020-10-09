AUTOMAKE_OPTIONS = foreign subdir-objects
MODNAME=mod_janus

mod_LTLIBRARIES = mod_janus.la
mod_janus_la_SOURCES  = globals.c http.c api.c servers.c hash.c mod_janus.c
mod_janus_la_CFLAGS   = $(FREESWITCH_CFLAGS)
mod_janus_la_LDFLAGS  = -avoid-version -module -no-undefined -shared $(FREESWITCH_LIBS) $(OPENSSL_LIBS) -lks
