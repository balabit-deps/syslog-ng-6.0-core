moduledir = @moduledir@
AM_CPPFLAGS = -I$(top_srcdir)/lib -I../../lib
export top_srcdir

module_LTLIBRARIES := libsyslogformat.la
libsyslogformat_la_SOURCES = \
	syslog-format.c syslog-format.h syslog-format-plugin.c

libsyslogformat_la_CPPFLAGS = $(AM_CPPFLAGS)
libsyslogformat_la_LIBADD = $(MODULE_DEPS_LIBS)
libsyslogformat_la_LDFLAGS = $(MODULE_LDFLAGS)


include $(top_srcdir)/build/lex-rules.am
