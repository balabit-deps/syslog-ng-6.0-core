/*
 * Copyright (c) 2007-2015 Balabit
 * Copyright (c) 2007-2015 Balázs Scheidler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "testutils.h"
#include "msg_parse_lib.h"

#include "syslog-ng.h"
#include "logmsg.h"
#include "logmsg-serialize.h"
#include "serialize.h"
#include "apphook.h"
#include "gsockaddr.h"
#include "timeutils.h"
#include "serialize.h"
#include "cfg.h"
#include "plugin.h"
#include "messages.h"

#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

const gchar *ignore_sdata_pairs[][2] = { { NULL, NULL } };
const gchar *empty_sdata_pairs[][2] = { { NULL, NULL } };

unsigned long
absolute_value(signed long diff)
{
  if (diff < 0)
    return -diff;
  else
    return diff;
}

static time_t
_get_epoch_with_bsd_year(int ts_month, int d, int h, int m, int s)
{
  struct tm *tm;
  time_t t;

  time(&t);
  tm = localtime(&t);

  tm->tm_year = determine_year_for_month(ts_month, tm);

  tm->tm_hour = h;
  tm->tm_min = m;
  tm->tm_sec = s;
  tm->tm_mday = d;
  tm->tm_mon = ts_month;
  tm->tm_isdst = -1;
  return mktime(tm);
}

void
assert_log_message_sdata_pairs(LogMessage *message, const gchar *expected_sd_pairs[][2])
{
  gint i;
  for (i = 0; expected_sd_pairs && expected_sd_pairs[i][0] != NULL;i++)
    {
      const gchar *actual_value = log_msg_get_value(message, log_msg_get_value_handle(expected_sd_pairs[i][0]), NULL);
      assert_string(actual_value, expected_sd_pairs[i][1], NULL);
    }
}

static GString *
serialize_message_into_gstring(LogMessage *message)
{
  GString *serialized_message = g_string_sized_new(0);
  SerializeArchive *sa = serialize_string_archive_new(serialized_message);
  log_msg_write(message, sa);
  serialize_archive_free(sa);
  return serialized_message;
}

static LogMessage *
unserialize_message_from_gstring(GString *serialized_message)
{
  LogMessage *message = log_msg_new_empty();
  SerializeArchive *sa = serialize_string_archive_new(serialized_message);
  log_msg_read(message, sa);
  serialize_archive_free(sa);
  return message;
}

static LogMessage *
make_a_copy_through_serialization(LogMessage *message)
{
  GString *serialized_message = serialize_message_into_gstring(message);
  LogMessage *copy = unserialize_message_from_gstring(serialized_message);
  g_string_free(serialized_message, TRUE);
  return copy;
}

void
simulate_log_readers_effect_on_timezone_offset(LogMessage *message)
{
  if (message->timestamps[LM_TS_STAMP].zone_offset == -1)
    message->timestamps[LM_TS_STAMP].zone_offset = get_local_timezone_ofs(message->timestamps[LM_TS_STAMP].tv_sec);
}

LogMessage *
parse_log_message(gchar *raw_message_str, gint parse_flags, gchar *bad_hostname_re)
{
  LogMessage *message;
  GSockAddr *addr = g_sockaddr_inet_new("10.10.10.10", 1010);
  regex_t bad_hostname;

  parse_options.flags = parse_flags;

  if (bad_hostname_re)
    {
      assert_gint(regcomp(&bad_hostname, bad_hostname_re, REG_NOSUB | REG_EXTENDED), 0,
                  "Unexpected failure of regcomp(); bad_hostname_re='%s'", bad_hostname_re);
      parse_options.bad_hostname = &bad_hostname;
    }

  message = log_msg_new(raw_message_str, strlen(raw_message_str), addr, &parse_options);

  if (bad_hostname_re)
    {
      regfree(parse_options.bad_hostname);
      parse_options.bad_hostname = NULL;
    }

  simulate_log_readers_effect_on_timezone_offset(message);
  g_sockaddr_unref(addr);
  return message;
}

void
testcase(gchar *msg,
         gint parse_flags,
         gchar *bad_hostname_re,
         gint expected_pri,
         unsigned long expected_stamp_sec,
         unsigned long expected_stamp_usec,
         unsigned long expected_stamp_ofs,
         const gchar *expected_host,
         const gchar *expected_program,
         const gchar *expected_msg,
         const gchar *expected_sd_str,
         const gchar *expected_pid,
         const gchar *expected_msgid,
         const gchar *expected_sd_pairs[][2])
{
  LogMessage *parsed_message;
  LogStamp *parsed_timestamp;
  LogMessage *unserialized_message;
  LogMessage *twice_unserialized_message;
  time_t now;
  GString *sd_str;

  testcase_begin("Testing log message parsing; parse_flags='%x', bad_hostname_re='%s', msg='%s'",
                 parse_flags, bad_hostname_re ? : "(null)", msg);

  parsed_message = parse_log_message(msg, parse_flags, bad_hostname_re);
  parsed_timestamp = &(parsed_message->timestamps[LM_TS_STAMP]);

  if (msg[0] != '<')
    {
      /* if the priority field is not found in the message, then the expected_pri value should be LOG_AUTH | LOG_EMERG */
      expected_pri = EVT_FAC_USER | EVT_PRI_NOTICE;
    }

  if (expected_stamp_sec)
    {
      if (expected_stamp_sec != 1)
        assert_guint(parsed_timestamp->tv_sec, expected_stamp_sec, "Unexpected timestamp");
      assert_guint32(parsed_timestamp->tv_usec, expected_stamp_usec, "Unexpected microseconds");
      assert_guint32(parsed_timestamp->zone_offset, expected_stamp_ofs, "Unexpected timezone offset");
    }
  else
    {
      time(&now);
      assert_true(absolute_value(parsed_timestamp->tv_sec - now) <= 5,
                  "Expected parsed message timestamp to be set to now; now='%d', timestamp->tv_sec='%d'",
                  (gint)now, (gint)parsed_timestamp->tv_sec, NULL);
    }
  assert_guint16(parsed_message->pri, expected_pri, "Unexpected message priority");
  assert_log_message_value(parsed_message, LM_V_HOST, expected_host);
  assert_log_message_value(parsed_message, LM_V_PROGRAM, expected_program);
  assert_log_message_value(parsed_message, LM_V_MESSAGE, expected_msg);
  if (expected_pid)
    assert_log_message_value(parsed_message, LM_V_PID, expected_pid);
  if (expected_msgid)
    assert_log_message_value(parsed_message, LM_V_MSGID, expected_msgid);
  if (expected_sd_str)
    {
      sd_str = g_string_sized_new(0);
      log_msg_format_sdata(parsed_message, sd_str, 0);
      assert_string(sd_str->str, expected_sd_str, "Unexpected formatted SData");
      g_string_free(sd_str, TRUE);
    }

  assert_log_message_sdata_pairs(parsed_message, expected_sd_pairs);

  unserialized_message = make_a_copy_through_serialization(parsed_message);
  assert_log_messages_equal(parsed_message, unserialized_message);
  assert_log_message_sdata_pairs(unserialized_message, expected_sd_pairs);

  twice_unserialized_message = make_a_copy_through_serialization(unserialized_message);
  log_msg_unref(unserialized_message);

  assert_log_messages_equal(parsed_message, twice_unserialized_message);
  assert_log_message_sdata_pairs(twice_unserialized_message, expected_sd_pairs);

  log_msg_unref(twice_unserialized_message);
  log_msg_unref(parsed_message);

  testcase_end();
}

void
test_log_messages_can_be_parsed(void)
{
 // failed to parse too long sd id
  testcase("<5>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [timeQuality isSynced=\"0\"][1234567890123456789012345678901234 i=\"long_33\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
      43,        //pri
      0, 0, 0,  // timestamp (sec/usec/zone)
      "", //host
      "syslog-ng", //app
      "Error processing log message (at position 117): <5>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [timeQuality isSynced=\"0\"][1234567890123456789012345678901234 i=\"long_33\"] An application event log entry...", // msg
      "", // sd str,
      0, // processid
      0, // msgid,
      empty_sdata_pairs
      );

// bad sd data unescaped "
 testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a i=\"\"ok\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           43,             // pri
           0, 0, 0,    // timestamp (sec/usec/zone)
           "",        // host
           "syslog-ng", //app
           "Error processing log message (at position 67): <132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a i=\"\"ok\"] An application event log entry...", // msg
           "", //sd_str
           0,//processid
           0,//msgid
           empty_sdata_pairs
           );

  testcase("PTHREAD support initialized", 0, NULL,
           0, 			// pri
           0, 0, 0,		// timestamp (sec/usec/zone)
           "",		// host
           "PTHREAD",		// program
           "support initialized", // msg
           NULL, NULL, NULL, NULL
           );

  testcase("<15> openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           15,             // pri
           0, 0, 0,        // timestamp (sec/usec/zone)
           "",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  testcase("<15>Jan  1 01:00:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           15,             // pri
           _get_epoch_with_bsd_year(0, 1, 1, 0, 0), 0, 3600,        // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  testcase("<15>Jan 10 01:00:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           15,             // pri
           _get_epoch_with_bsd_year(0, 10, 1, 0, 0), 0, 3600,        // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  testcase("<13>Jan  1 14:40:51 alma korte: message", 0, NULL,
       13,
       _get_epoch_with_bsd_year(0, 1, 14, 40, 51), 0, 3600,
       "",
       "alma",
       "korte: message",
       NULL, NULL, NULL, ignore_sdata_pairs
       );

  testcase("<7>2006-11-10T10:43:21.156+02:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1163148201, 156000, 7200,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  testcase("<7>2006-11-10T10:43:21.156+01:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1163151801, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  testcase("<7>2006-11-10T10:43:21.15600000000000000000000000000000000000000000000000000000000000+01:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1163151801, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<7>2006-11-10T10:43:21.15600000000 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1163151801, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  testcase("<7>2006-03-26T01:59:59.156+01:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1143334799, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  testcase("<7>2006-03-26T02:00:00.156+01:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1143334800, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<7>2006-03-26T03:00:00.156+02:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1143334800, 156000, 7200,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<7>2006-10-29T01:00:00.156+02:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162076400, 156000, 7200,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<7>2006-10-29T01:59:59.156+02:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162079999, 156000, 7200,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<7>2006-10-29T02:00:00.156+02:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162080000, 156000, 7200,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  /* the same in a foreign timezone */
  testcase("<7>2006-10-29T01:00:00.156+01:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162080000, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<7>2006-10-29T01:59:59.156+01:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<7>2006-10-29T02:00:00.156+01:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162083600, 156000, 3600,    // timestamp (sec/usec/zone)
           "bzorp",        // host
           "openvpn",        // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  /* check hostname */

  testcase("<7>2006-10-29T02:00:00.156+01:00 %bzorp openvpn[2499]: PTHREAD support initialized", LP_CHECK_HOSTNAME | LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162083600, 156000, 3600,    // timestamp (sec/usec/zone)
           "",                // host
           "%bzorp",        // openvpn
           "openvpn[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );


  testcase("<7>2006-10-29T02:00:00.156+01:00 bzorp openvpn[2499]: PTHREAD support initialized", 0, NULL,
           7,             // pri
           1162083600, 156000, 3600,    // timestamp (sec/usec/zone)
           "",                // host
           "bzorp",    // program
           "openvpn[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  testcase("<7>2006-10-29T02:00:00.156+01:00 ", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162083600, 156000, 3600,    // timestamp (sec/usec/zone)
           "",                // host
           "",        // openvpn
           "", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  testcase("<7>2006-10-29T02:00:00.156+01:00", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1162083600, 156000, 3600,    // timestamp (sec/usec/zone)
           "",                // host
           "",        // openvpn
           "", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  testcase("<7>2006-10-29T02:00:00.156+01:00 ctld snmpd[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, "^ctld",
           7,             // pri
           1162083600, 156000, 3600,    // timestamp (sec/usec/zone)
           "",                // host
           "ctld",        // openvpn
           "snmpd[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  testcase("<7> Aug 29 02:00:00.156 ctld snmpd[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, "^ctld",
           7,             // pri
           1, 156000, 7200,    // timestamp (sec/usec/zone)
           "",                // host
           "ctld",    // openvpn
           "snmpd[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );
  testcase("<7> Aug 29 02:00:00.156789 ctld snmpd[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, "^ctld",
           7,             // pri
           1, 156789, 7200,    // timestamp (sec/usec/zone)
           "",                // host
           "ctld",    // openvpn
           "snmpd[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );
  testcase("<7> Aug 29 02:00:00. ctld snmpd[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, "^ctld",
           7,             // pri
           1, 0, 7200,            // timestamp (sec/usec/zone)
           "",                // host
           "ctld",    // openvpn
           "snmpd[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );
  testcase("<7> Aug 29 02:00:00 ctld snmpd[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, "^ctld",
           7,             // pri
           1, 0, 7200,            // timestamp (sec/usec/zone)
           "",                // host
           "ctld",    // openvpn
           "snmpd[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  testcase("<38>Sep 22 10:11:56 Message forwarded from cdaix66: sshd[679960]: Accepted publickey for nagios from 1.9.1.1 port 42096 ssh2", LP_EXPECT_HOSTNAME, NULL,
           38,
           1, 0, 7200,
           "cdaix66",
           "sshd",
           "Accepted publickey for nagios from 1.9.1.1 port 42096 ssh2",
           NULL, NULL, NULL, NULL
           );

  testcase("<7>Aug 29 02:00:00 bzorp ctld/snmpd[2499]: PTHREAD support initialized", LP_EXPECT_HOSTNAME, NULL,
           7,             // pri
           1, 0, 7200,            // timestamp (sec/usec/zone)
           "bzorp",            // host
           "ctld/snmpd",    // openvpn
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );
  testcase("<190>Apr 15 2007 21:28:13: %PIX-6-302014: Teardown TCP connection 1688438 for bloomberg-net:1.2.3.4/8294 to inside:5.6.7.8/3639 duration 0:07:01 bytes 16975 TCP FINs", LP_EXPECT_HOSTNAME, "^%",
           190,
           1176665293, 0, 7200,
           "",
           "%PIX-6-302014",
           "Teardown TCP connection 1688438 for bloomberg-net:1.2.3.4/8294 to inside:5.6.7.8/3639 duration 0:07:01 bytes 16975 TCP FINs",
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  /* Dell switch */
  testcase("<190>NOV 22 00:00:33 192.168.33.8-1 CMDLOGGER[165319912]: cmd_logger_api.c(83) 13518 %% CLI:192.168.32.100:root:User  logged in", LP_EXPECT_HOSTNAME, NULL,
           190,
           _get_epoch_with_bsd_year(10, 22, 0, 0, 33), 0, 3600,
           "192.168.33.8-1",
           "CMDLOGGER",
           "cmd_logger_api.c(83) 13518 %% CLI:192.168.32.100:root:User  logged in",
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  testcase("<190>Apr 15 2007 21:28:13 %ASA: this is a Cisco ASA timestamp", LP_EXPECT_HOSTNAME, "^%",
           190,
           1176665293, 0, 7200,
           "",
           "%ASA",
           "this is a Cisco ASA timestamp",
           NULL, NULL, NULL, ignore_sdata_pairs
           );
  testcase("<190>Apr 15 21:28:13 2007 linksys app: msg", LP_EXPECT_HOSTNAME, NULL,
           190,
           1176665293, 0, 7200,
           "linksys",
           "app",
           "msg",
           NULL, NULL, NULL, ignore_sdata_pairs
           );

  // Testing ignore_ambiguous_program_field option
  testcase("PTHREAD support initialized", LP_FALLBACK_NOPARSE, NULL,
           0, 			// pri
           0, 0, 0,		// timestamp (sec/usec/zone)
           "",		// host
           "",		// program
           "PTHREAD support initialized", // msg
           NULL, NULL, NULL, NULL
           );

  testcase("<15> PTHREAD support initialized", LP_FALLBACK_NOPARSE, NULL,
           15, 			// pri
           0, 0, 0,		// timestamp (sec/usec/zone)
           "",		// host
           "",		// program
           "PTHREAD support initialized", // msg
           NULL, NULL, NULL, NULL
           );

  testcase("openvpn[2499]: PTHREAD support initialized", LP_FALLBACK_NOPARSE, NULL,
           0,                   // pri
           0, 0, 0,		// timestamp (sec/usec/zone)
           "",		// host
           "",		// program
           "openvpn[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, NULL
           );

  testcase("<7>2006-10-29T02:00:00.156+01:00", LP_FALLBACK_NOPARSE, NULL,
           7, 			// pri
           1162083600, 156000, 3600,	// timestamp (sec/usec/zone)
           "",	        	// host
           "",		// program
           "", // msg
           NULL, NULL, NULL, NULL
           );

  testcase("<7>2006-10-29T02:00:00.156+01:00 snmpd[2499]: PTHREAD support initialized", LP_FALLBACK_NOPARSE, NULL,
           7, 			// pri
           1162083600, 156000, 3600,	// timestamp (sec/usec/zone)
           "",		        // host
           "snmpd",		// program
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, NULL
           );

  testcase("<7>2006-10-29T02:00:00.156+01:00 ctld snmpd[2499]: PTHREAD support initialized", LP_FALLBACK_NOPARSE, "^ctld",
           7, 			// pri
           1162083600, 156000, 3600,	// timestamp (sec/usec/zone)
           "",		        // host
           "ctld",
           "snmpd[2499]: PTHREAD support initialized", // msg
           NULL, NULL, NULL, NULL
           );

  testcase("<7>2006-11-10T10:43:21.156+02:00 openvpn[2499]: PTHREAD support initialized", LP_FALLBACK_NOPARSE, NULL,
           7, 			// pri
           1163148201, 156000, 7200,	// timestamp (sec/usec/zone)
           "",                  // host
           "openvpn",		// program
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, NULL
           );

  testcase("<7>2006-11-10T10:43:21.156+02:00 bzorp openvpn[2499]: PTHREAD support initialized", LP_FALLBACK_NOPARSE | LP_EXPECT_HOSTNAME, NULL,
           7, 			// pri
           1163148201, 156000, 7200,	// timestamp (sec/usec/zone)
           "bzorp",		// host
           "openvpn",		// program
           "PTHREAD support initialized", // msg
           NULL, "2499", NULL, NULL
           );

  const gchar *expected_sd_pairs_test_1[][2]=
  {
    { ".SDATA.timeQuality.isSynced", "0"},
    { ".SDATA.timeQuality.tzKnown", "1"},
    { ".SDATA.exampleSDID@0.iut", "3"},
    { ".SDATA.exampleSDID@0.eventSource", "Application"},
    { ".SDATA.exampleSDID@0.eventID", "1011"},
    { ".SDATA.examplePriority@0.class", "high"},
    {  NULL , NULL}
  };

  testcase("<7>1 2006-10-29T01:59:59.156+01:00 mymachine.example.com evntslog - ID47 [exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"] \xEF\xBB\xBF" "An application event log entry...", LP_SYSLOG_PROTOCOL, NULL,
           7,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine.example.com",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"]", //sd_str
           "",//processid
           "ID47",//msgid
           expected_sd_pairs_test_1
           );

  testcase("<7>1 2006-10-29T01:59:59.156Z mymachine.example.com evntslog - ID47 [exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"] \xEF\xBB\xBF" "An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           7,             // pri
           1162087199, 156000, 0,    // timestamp (sec/usec/zone)
           "mymachine.example.com",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"]", //sd_str
           "",//processid
           "ID47",//msgid
           expected_sd_pairs_test_1
           );

  testcase("<7>1 2006-10-29T01:59:59.156123Z mymachine.example.com evntslog - ID47 [exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"] \xEF\xBB\xBF" "An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           7,             // pri
           1162087199, 156123, 0,    // timestamp (sec/usec/zone)
           "mymachine.example.com",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"]", //sd_str
           "",//processid
           "ID47",//msgid
           expected_sd_pairs_test_1
           );

  testcase("<7>1 2006-10-29T01:59:59.156Z mymachine.example.com evntslog - ID47 [ exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"] \xEF\xBB\xBF" "An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           43,             // pri
           0, 0, 0,    // timestamp (sec/usec/zone)
           "",        // host
           "syslog-ng", //app
           "Error processing log message (at position 69): <7>1 2006-10-29T01:59:59.156Z mymachine.example.com evntslog - ID47 [ exampleSDID@0 iut=\"3\" eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"] \xEF\xBB\xBF" "An application event log entry...", // msg
           "",
           NULL,//processid
           NULL,//msgid
           empty_sdata_pairs
           );

  testcase("<34>1 1987-01-01T12:00:27.000087+00:20 192.0.2.1 myproc 8710 - - %% It's time to make the do-nuts.", LP_SYSLOG_PROTOCOL, NULL,
           34,
           536499627, 87, 1200,
           "192.0.2.1",
           "myproc",
           "%% It's time to make the do-nuts.",
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"]",
           "8710",
           "",
           ignore_sdata_pairs);


  const gchar *expected_sd_pairs_test_2[][2]=
  {
    { ".SDATA.timeQuality.isSynced", "1"},
    { ".SDATA.timeQuality.tzKnown", "1"},
    { ".SDATA.exampleSDID@0.iut", "3"},
    {  NULL , NULL}
  };

  testcase("<132>1 .2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [exampleSDID@0 iut=\"3\"] [eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           132,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "evntslog", //app
           "[eventSource=\"Application\" eventID=\"1011\"][examplePriority@0 class=\"high\"] An application event log entry...", // msg
           "[timeQuality isSynced=\"1\" tzKnown=\"1\"][exampleSDID@0 iut=\"3\"]", //sd_str
           "",//processid
           "",//msgid
           expected_sd_pairs_test_2
           );

  testcase("<7>Aug 29 02:00:00 bzorp ctld/snmpd[2499]:", LP_EXPECT_HOSTNAME, NULL,
           7,           // pri
           1, 0, 7200,          // timestamp (sec/usec/zone)
           "bzorp",         // host
           "ctld/snmpd",    // openvpn
           "", // msg
           NULL, "2499", NULL, ignore_sdata_pairs
           );

  const gchar *expected_sd_pairs_test_3[][2]=
  {
    { ".SDATA.timeQuality.isSynced", "0"},
    { ".SDATA.timeQuality.tzKnown", "1"},
    { ".SDATA.origin.ip", "exchange.macartney.esbjerg"},
    { ".SDATA.meta.sequenceId", "191732"},
    { ".SDATA.EventData@18372.4.Data", "MSEXCHANGEOWAAPPPOOL.CONFIG\" -W \"\" -M 1 -AP \"MSEXCHANGEOWAAPPPOOL5244fileserver.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 mail.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 maindc.macartney.esbjerg CD- 1 6 6 0 0 1 1 6 1 " },
    {  NULL , NULL}
  };

  testcase("<134>1 2009-10-16T11:51:56+02:00 exchange.macartney.esbjerg MSExchange_ADAccess 20208 - [origin ip=\"exchange.macartney.esbjerg\"][meta sequenceId=\"191732\" sysUpTime=\"68807696\"][EventData@18372.4 Data=\"MSEXCHANGEOWAAPPPOOL.CONFIG\\\" -W \\\"\\\" -M 1 -AP \\\"MSEXCHANGEOWAAPPPOOL5244fileserver.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 mail.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 maindc.macartney.esbjerg CD- 1 6 6 0 0 1 1 6 1 \"][Keywords@18372.4 Keyword=\"Classic\"] ApplicationMSExchangeADAccess: message",
           LP_SYSLOG_PROTOCOL, NULL,
           134,             // pri
           1255686716, 0, 7200,    // timestamp (sec/usec/zone)
           "exchange.macartney.esbjerg",        // host
           "MSExchange_ADAccess", //app
           "ApplicationMSExchangeADAccess: message", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][origin ip=\"exchange.macartney.esbjerg\"][meta sequenceId=\"191732\" sysUpTime=\"68807696\"][EventData@18372.4 Data=\"MSEXCHANGEOWAAPPPOOL.CONFIG\\\" -W \\\"\\\" -M 1 -AP \\\"MSEXCHANGEOWAAPPPOOL5244fileserver.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 mail.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 maindc.macartney.esbjerg CD- 1 6 6 0 0 1 1 6 1 \"][Keywords@18372.4 Keyword=\"Classic\"]", //sd_str
           "20208",//processid
           "",//msgid
           expected_sd_pairs_test_3
           );

  const gchar *expected_sd_pairs_test_4[][2]=
  {
    { ".SDATA.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.i", "ok_32"},
    {  NULL , NULL}
  };

  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa i=\"ok_32\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           132,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa i=\"ok_32\"]", //sd_str
           "",//processid
           "",//msgid
           expected_sd_pairs_test_4
           );

  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa i=\"long_33\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           43,                         // pri
           0, 0, 0,    // timestamp (sec/usec/zone)
           "",         // host
           "syslog-ng", //app
           "Error processing log message (at position 93): <132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa i=\"long_33\"] An application event log entry...", // msg
           "", //sd_str
           0,//processid
           0,//msgid
           empty_sdata_pairs
           );

  const gchar *expected_sd_pairs_test_7[][2]=
  {
    { ".SDATA.timeQuality.isSynced", "0"},
    { ".SDATA.timeQuality.tzKnown", "1"},
    { ".SDATA.origin.ip", "exchange.macartney.esbjerg"},
    { ".SDATA.meta.sequenceId", "191732"},
    { ".SDATA.EventData@18372.4.Data", "MSEXCHANGEOWAAPPPOOL.CONFIG\" -W \"\" -M 1 -AP \"MSEXCHANGEOWAAPPPOOL5244fileserver.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 mail.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 maindc.macartney.esbjerg CD- 1 6 6 0 0 1 1 6 1 " },
    {  NULL , NULL}
  };

  testcase("<134>1 2009-10-16T11:51:56+02:00 exchange.macartney.esbjerg MSExchange_ADAccess 20208 - [origin ip=\"exchange.macartney.esbjerg\"][meta sequenceId=\"191732\" sysUpTime=\"68807696\"][EventData@18372.4 Data=\"MSEXCHANGEOWAAPPPOOL.CONFIG\\\" -W \\\"\\\" -M 1 -AP \\\"MSEXCHANGEOWAAPPPOOL5244fileserver.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 mail.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 maindc.macartney.esbjerg CD- 1 6 6 0 0 1 1 6 1 \"][Keywords@18372.4 Keyword=\"Classic\"] ApplicationMSExchangeADAccess: message",
           LP_SYSLOG_PROTOCOL, NULL,
           134,             // pri
           1255686716, 0, 7200,    // timestamp (sec/usec/zone)
           "exchange.macartney.esbjerg",        // host
           "MSExchange_ADAccess", //app
           "ApplicationMSExchangeADAccess: message", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][origin ip=\"exchange.macartney.esbjerg\"][meta sequenceId=\"191732\" sysUpTime=\"68807696\"][EventData@18372.4 Data=\"MSEXCHANGEOWAAPPPOOL.CONFIG\\\" -W \\\"\\\" -M 1 -AP \\\"MSEXCHANGEOWAAPPPOOL5244fileserver.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 mail.macartney.esbjerg CDG 1 7 7 1 0 1 1 7 1 maindc.macartney.esbjerg CD- 1 6 6 0 0 1 1 6 1 \"][Keywords@18372.4 Keyword=\"Classic\"]", //sd_str
           "20208",//processid
           "",//msgid
           expected_sd_pairs_test_7
           );

/*###########################x*/
  const gchar *expected_sd_pairs_test_7a[][2]=
  {
    { ".SDATA.timeQuality.isSynced", "0"},
    { ".SDATA.timeQuality.tzKnown", "0"},
    {  NULL , NULL}
  };

  //Testing syslog protocol message parsing if tzKnown=0 because there is no timezone information
  testcase("<134>1 2009-10-16T11:51:56 exchange.macartney.esbjerg MSExchange_ADAccess 20208 - - An application event log entry...",
           LP_SYSLOG_PROTOCOL, NULL,
           134,                         // pri
           1255686716, 0, 7200, // timestamp (sec/usec/zone)
           "exchange.macartney.esbjerg",                // host
           "MSExchange_ADAccess", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"0\"]",//sd_str
           "20208",//processid
           "",//msgid
           expected_sd_pairs_test_7a
           );

  const gchar *expected_sd_pairs_test_8[][2]=
  {
    { ".SDATA.timeQuality.isSynced", "0"},
    { ".SDATA.timeQuality.tzKnown", "1"},
    { ".SDATA.origin.enterpriseId", "1.3.6.1.4.1"},
    {  NULL , NULL}
  };

  //Testing syslog protocol message parsing if SDATA contains origin enterpriseID
  testcase("<134>1 2009-10-16T11:51:56+02:00 exchange.macartney.esbjerg MSExchange_ADAccess 20208 - [origin enterpriseId=\"1.3.6.1.4.1\"] An application event log entry...",
           LP_SYSLOG_PROTOCOL, NULL,
           134,                         // pri
           1255686716, 0, 7200, // timestamp (sec/usec/zone)
           "exchange.macartney.esbjerg",                // host
           "MSExchange_ADAccess", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][origin enterpriseId=\"1.3.6.1.4.1\"]",//sd_str
           "20208",//processid
           "",//msgid
           expected_sd_pairs_test_8
           );

  //Testing syslog protocol message parsing if size of origin software and swVersion are longer than the maximum size (48 and 32)
  //KNOWN BUG: 22045
/*  testcase("<134>1 2009-10-16T11:51:56+02:00 exchange.macartney.esbjerg MSExchange_ADAccess 20208 - [origin software=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" swVersion=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"] An application event log entry...",
           LP_SYSLOG_PROTOCOL, NULL,
           43,                         // pri
           0, 0, 0, // timestamp (sec/usec/zone)
           "",                // host
           "syslog-ng", //app
           "Error processing log message: <134>1 2009-10-16T11:51:56+02:00 exchange.macartney.esbjerg MSExchange_ADAccess 20208 - [origin software=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" swVersion=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"] An application event log entry...", // msg
           "",
           "0",//processid
           "0",//msgid
           0
           );*/

  const gchar *expected_sd_pairs_test_9[][2]=
  {
    { ".SDATA.timeQuality.isSynced", "0"},
    { ".SDATA.timeQuality.tzKnown", "1"},
    { ".SDATA.origin.enterpriseId", "1.3.6.1.4.1"},
    {  NULL , NULL}
  };

  //Testing syslog protocol message parsing if SDATA contains only SD-ID without SD-PARAM
  //KNOWN BUG: 20459
  testcase("<134>1 2009-10-16T11:51:56+02:00 exchange.macartney.esbjerg MSExchange_ADAccess 20208 - [origin enterpriseId=\"1.3.6.1.4.1\"][nosdnvpair] An application event log entry...",
           LP_SYSLOG_PROTOCOL, NULL,
           134,                         // pri
           1255686716, 0, 7200, // timestamp (sec/usec/zone)
           "exchange.macartney.esbjerg",                // host
           "MSExchange_ADAccess", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][origin enterpriseId=\"1.3.6.1.4.1\"][nosdnvpair]",//sd_str
           "20208",//processid
           "",//msgid
           expected_sd_pairs_test_9
           );

/*############################*/
}

void
test_log_messages_sdata_limits(void)
{
  const gchar *expected_sd_pairs_test_5[][2] =
  {
    {".SDATA.a.i", "]\"\\"},
    {NULL, NULL}
  };

  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a i=\"\\]\\\"\\\\\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           132,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][a i=\"\\]\\\"\\\\\"]", //sd_str
           "",//processid
           "",//msgid
           expected_sd_pairs_test_5
           );

  // failed to parse to long sd name
  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=\"long_33\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           43,             // pri
           0, 0, 0,    // timestamp (sec/usec/zone)
           "",        // host
           "syslog-ng", //app
           "Error processing log message (at position 95): <132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa=\"long_33\"] An application event log entry...", // msg
           "", //sd_str
           0,//processid
           0,//msgid
           empty_sdata_pairs
           );


  // too long sdata value gets truncated (255)
  parse_options.sdata_param_value_max = 255;
  const gchar *expected_sd_pairs_test_5b[][2]=
  {
    {".SDATA.a.i", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
    {NULL, NULL}
  };

  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a i=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           132,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][a i=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"]", //sd_str
           "",//processid
           "",//msgid
           expected_sd_pairs_test_5b
           );


  // too long sdata value gets truncated (default sdata_param_value_max)
  msg_format_options_defaults(&parse_options);
  gchar *sdata_value = g_strnfill(parse_options.sdata_param_value_max + 1, 'a');
  gchar *expected_sdata_value = g_strnfill(parse_options.sdata_param_value_max, 'a');
  gchar *log_message = g_strconcat("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a i=\"",
                        sdata_value, "\"] An application event log entry...", NULL);

  const gchar *expected_sd_pairs_test_5c[][2]=
  {
    {".SDATA.a.i", expected_sdata_value},
    {NULL, NULL}
  };

  testcase(log_message,  LP_SYSLOG_PROTOCOL, NULL,
           132,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           NULL,
           "",//processid
           "",//msgid
           expected_sd_pairs_test_5c
           );

  g_free(sdata_value);
  g_free(expected_sdata_value);
  g_free(log_message);
}

void
test_log_messages_part_limits(void)
{
  const gchar *expected_sd_pairs_test_6[][2]=
  {
    {".SDATA.a.i", "ok"},
    { NULL, NULL}
  };

  // too long hostname
  testcase("<132>1 2006-10-29T01:59:59.156+01:00 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa evntslog - - [a i=\"ok\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           43,        // pri
           0, 0, 0,    // timestamp (sec/usec/zone)
           "", //host
           "syslog-ng", //app
           "Error processing log message (at position 37): <132>1 2006-10-29T01:59:59.156+01:00 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa evntslog - - [a i=\"ok\"] An application event log entry...",        // msg
           "", //sd_str
           0,//processid
           0,//msgid
           empty_sdata_pairs
           );

  // failed to parse to long appname
  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa - - [a i=\"ok\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           132,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][a i=\"ok\"]", //sd_str
           0,//processid
           0,//msgid
           expected_sd_pairs_test_6
           );

  // failed to parse to long procid
  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa - [a i=\"ok\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           132,             // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][a i=\"ok\"]", //sd_str
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",//processid
           0,//msgid
           expected_sd_pairs_test_6
           );

  // failed to parse to long msgid
  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa [a i=\"ok\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           132,     // pri
           1162083599, 156000, 3600,    // timestamp (sec/usec/zone)
           "mymachine",        // host
           "evntslog", //app
           "An application event log entry...", // msg
           "[timeQuality isSynced=\"0\" tzKnown=\"1\"][a i=\"ok\"]", //sd_str
           0,//processid
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",//msgid
           expected_sd_pairs_test_6
           );

  // unescaped ]
  testcase("<132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a i=\"]ok\"] An application event log entry...",  LP_SYSLOG_PROTOCOL, NULL,
           43,             // pri
           0, 0, 0,    // timestamp (sec/usec/zone)
           "",        // host
           "syslog-ng", //app
           "Error processing log message (at position 66): <132>1 2006-10-29T01:59:59.156+01:00 mymachine evntslog - - [a i=\"]ok\"] An application event log entry...", // msg
           "", //sd_str
           0,//processid
           0,//msgid
           empty_sdata_pairs
           );
  // unescaped '\'
}

void
test_determine_year_for_month(gint current_month, gint month, gint expected_year)
{
  const gint base_year = 1900;
  const gint current_year = 2013;
  struct tm now;

  now.tm_sec = 0;
  now.tm_min = 0;
  now.tm_hour = 0;
  now.tm_mday = 0;
  now.tm_year = current_year - base_year;
  now.tm_mon = current_month;

  assert_gint(determine_year_for_month(month, &now), expected_year - base_year, "Guessed year is not expected");
}

int
main(int argc G_GNUC_UNUSED, char *argv[] G_GNUC_UNUSED)
{
  app_startup();
  putenv("TZ=MET-1METDST");
  tzset();
  init_and_load_syslogformat_module();

  test_log_messages_can_be_parsed();
  test_log_messages_sdata_limits();
  test_log_messages_part_limits();

  // Current year is defined in test_determine_year_for_month() function
  test_determine_year_for_month(0, 0, 2013);
  test_determine_year_for_month(0, 1, 2013);
  test_determine_year_for_month(0, 2, 2013);
  test_determine_year_for_month(0, 3, 2013);
  test_determine_year_for_month(0, 10, 2013);
  test_determine_year_for_month(0, 11, 2012);
  test_determine_year_for_month(1, 0, 2013);
  test_determine_year_for_month(1, 11, 2013);
  test_determine_year_for_month(6, 0, 2013);
  test_determine_year_for_month(6, 6, 2013);
  test_determine_year_for_month(6, 8, 2013);
  test_determine_year_for_month(6, 9, 2013);
  test_determine_year_for_month(6, 11, 2013);
  test_determine_year_for_month(11, 0, 2014);
  test_determine_year_for_month(11, 1, 2013);
  test_determine_year_for_month(11, 2, 2013);

  deinit_syslogformat_module();
  app_shutdown();
  return 0;
}


