AM_CFLAGS = -I$(top_srcdir)/lib -I$(top_srcdir)/libtest -I../../../lib -I$(top_srcdir)/modules/dbparser -I$(top_srcdir)/modules/dbparser/tests -I.. @CFLAGS_NOWARN_POINTER_SIGN@
AM_LDFLAGS = -dlpreopen ../../syslogformat/libsyslogformat.la -dlpreopen ../..//basicfuncs/libbasicfuncs.la
LDADD = ../libsyslog-ng-patterndb.a $(top_builddir)/lib/libsyslog-ng.la $(top_builddir)/libtest/libsyslog-ng-test.a  @TOOL_DEPS_LIBS@ @OPENSSL_LIBS@

check_PROGRAMS = test_timer_wheel test_patternize test_patterndb test_radix test_patterndb_parsers

test_timer_wheel_SOURCES = test_timer_wheel.c
test_patternize_SOURCES = test_patternize.c
test_patterndb_SOURCES = test_patterndb.c
test_patterndb_parsers_SOURCES = test_patterndb_parsers.c

test_radix_SOURCES = test_radix.c

EXTRA_DIST = test_parsers_e2e.c

TESTS = $(check_PROGRAMS)
