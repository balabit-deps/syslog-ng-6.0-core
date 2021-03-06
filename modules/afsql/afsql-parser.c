/*
 * Copyright (c) 2002-2012 Balabit
 * Copyright (c) 1998-2012 Balázs Scheidler
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

#include "afsql.h"
#include "cfg-parser.h"
#include "afsql-grammar.h"

extern int afsql_debug;

int afsql_parse(CfgLexer *lexer, LogDriver **instance, gpointer arg);

static CfgLexerKeyword afsql_keywords[] = {
  { "sql",                KW_SQL },
  { "username",           KW_USERNAME },
  { "password",           KW_PASSWORD },
  { "database",           KW_DATABASE },
  { "table",              KW_TABLE },

  { "columns",            KW_COLUMNS },
  { "indexes",            KW_INDEXES },
  { "values",             KW_VALUES },
  { "log_fifo_size",      KW_LOG_FIFO_SIZE },
  { "frac_digits",        KW_FRAC_DIGITS },
  { "session_statements", KW_SESSION_STATEMENTS, VERSION_VALUE_3_2 },
  { "host",               KW_HOST },
  { "port",               KW_PORT },
  { "type",               KW_TYPE },
  { "default",            KW_DEFAULT },

  { "time_zone",          KW_TIME_ZONE },
  { "local_time_zone",    KW_LOCAL_TIME_ZONE },
  { "null",               KW_NULL },
  { "retry_sql_inserts",  KW_RETRIES, VERSION_VALUE_3_3 },
  { "retries",            KW_RETRIES, VERSION_VALUE_3_3 },
  { "flush_lines",        KW_FLUSH_LINES },
  { "flush_timeout",      KW_FLUSH_TIMEOUT, 0, KWS_OBSOLETE, "flush_timeout has been ignored since " VERSION_PE_4_2},
  { "flags",              KW_FLAGS },
  { "ignore_tns_config",  KW_IGNORE_TNS_CONFIG },
  { NULL }
};

CfgParser afsql_parser =
{
#if ENABLE_DEBUG
  .debug_flag = &afsql_debug,
#endif
  .name = "afsql",
  .keywords = afsql_keywords,
  .parse = (gint (*)(CfgLexer *, gpointer *, gpointer)) afsql_parse,
  .cleanup = (void (*)(gpointer)) log_pipe_unref,
};

CFG_PARSER_IMPLEMENT_LEXER_BINDING(afsql_, LogDriver **)
