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

#include "csvparser.h"
#include "logparser.h"
#include "utils.h"
#include "stringutils.h"

#include <string.h>

typedef struct _LogCSVParser
{
  LogColumnParser super;
  gchar *delimiters;
  gchar *quotes_start;
  gchar *quotes_end;
  gchar *null_value;
  guint32 flags;
  GList *string_delimiters;
} LogCSVParser;

static inline gint
_is_escape_flag(guint32 flag)
{
  return ((flag & LOG_CSV_PARSER_ESCAPE_MASK) == flag);
}

static inline gint
_contains_escape_flag(guint32 flag)
{
  return (flag & LOG_CSV_PARSER_ESCAPE_MASK);
}

static inline guint32
_get_escape_flags(guint32 flags)
{
  return (flags & LOG_CSV_PARSER_ESCAPE_MASK);
}

static inline guint32
_remove_escape_flags(guint32 flags)
{
  return (flags & (~LOG_CSV_PARSER_ESCAPE_MASK));
}

guint32
log_csv_parser_normalize_escape_flags(LogColumnParser *s, guint32 new_flag)
{
  LogCSVParser *self = (LogCSVParser *) s;
  guint32 current_flags = self->flags;
  if (_is_escape_flag(new_flag))
    return (_remove_escape_flags(current_flags) | new_flag);

  if (!_contains_escape_flag(new_flag))
    return (_get_escape_flags(current_flags) | new_flag);

  return new_flag;
}

void
log_csv_parser_set_flags(LogColumnParser *s, guint32 flags)
{
  LogCSVParser *self = (LogCSVParser *) s;

  self->flags = flags;
}

void
log_csv_parser_set_delimiters(LogColumnParser *s, const gchar *delimiters)
{
  LogCSVParser *self = (LogCSVParser *) s;

  if (self->delimiters)
    g_free(self->delimiters);
  self->delimiters = g_strdup(delimiters);
}

void
log_csv_parser_set_quotes(LogColumnParser *s, const gchar *quotes)
{
  LogCSVParser *self = (LogCSVParser *) s;

  if (self->quotes_start)
    g_free(self->quotes_start);
  if (self->quotes_end)
    g_free(self->quotes_end);
  self->quotes_start = g_strdup(quotes);
  self->quotes_end = g_strdup(quotes);
}

void
log_csv_parser_set_quote_pairs(LogColumnParser *s, const gchar *quote_pairs)
{
  LogCSVParser *self = (LogCSVParser *) s;
  gint i;

  if (self->quotes_start)
    g_free(self->quotes_start);
  if (self->quotes_end)
    g_free(self->quotes_end);

  self->quotes_start = g_malloc((strlen(quote_pairs) / 2) + 1);
  self->quotes_end = g_malloc((strlen(quote_pairs) / 2) + 1);

  for (i = 0; quote_pairs[i] && quote_pairs[i+1]; i += 2)
    {
      self->quotes_start[i / 2] = quote_pairs[i];
      self->quotes_end[i / 2] = quote_pairs[i + 1];
    }
  self->quotes_start[i / 2] = 0;
  self->quotes_end[i / 2] = 0;
}

void
log_csv_parser_set_null_value(LogColumnParser *s, const gchar *null_value)
{
  LogCSVParser *self = (LogCSVParser *) s;

  if (self->null_value)
    g_free(self->null_value);
  self->null_value = g_strdup(null_value);
}

void
log_csv_parser_append_string_delimiter(LogColumnParser *s, const gchar *string_delimiter)
{
  LogCSVParser *self = (LogCSVParser *) s;

  self->string_delimiters = g_list_prepend(self->string_delimiters, (gpointer)g_strdup(string_delimiter));
}

static inline gboolean
_should_drop_as_invalid(GList *cur_column, const gchar *src, guint32 flags)
{
  return (cur_column || (src && *src)) && (flags & LOG_CSV_PARSER_DROP_INVALID);
}

static inline gboolean
_is_whitespace_char(const gchar* str)
{
  return (*str == ' ' || *str == '\t') ? TRUE : FALSE;
}

static inline void
_strip_whitespace_left(const gchar** src)
{
  while (_is_whitespace_char(*src))
    (*src)++;
}

typedef struct _UnescapedParserState {
  guchar *next_delim;
  guchar *next_string_delim;
  guchar *next_char_delim;
  gint delim_len;
  GList *cur_column;
  guchar current_quote;
} UnescapedParserState;

static void
unescaped_parser_state_init(UnescapedParserState *self, GList *cur_column)
{
  self->next_delim = NULL;
  self->next_string_delim = NULL;
  self->next_char_delim = NULL;
  self->delim_len = 0;
  self->cur_column = cur_column;
  self->current_quote = 0;
}

static void
unescaped_parser_state_reset_delimiters(UnescapedParserState *self)
{
  self->next_delim = NULL;
  self->next_string_delim = NULL;
  self->next_char_delim = NULL;
  self->delim_len = 0;
}

static inline guchar*
_unescaped_quoted_find_next_delim(LogCSVParser *self, UnescapedParserState *pstate, const gchar* src)
{
  /* search for end of quote */
  pstate->next_delim = (guchar *) strchr(src, pstate->current_quote);

  if (pstate->next_delim)
    {
      pstate->next_string_delim = g_string_list_find_first(self->string_delimiters,
                                                          (const char *)pstate->next_delim,
                                                          &pstate->delim_len);
      if (pstate->next_string_delim == (pstate->next_delim + 1) ||
          (strchr(self->delimiters, *(pstate->next_delim + 1)) != NULL )
         )
        {
          /* closing quote, and then a delimiter, everything is nice */
          pstate->next_delim++;
        }
    }
  else if (!pstate->next_delim)
    {
      /* complete remaining string */
      pstate->next_delim = (guchar *) src + strlen(src);
    }

  return pstate->next_delim;
}

static inline guchar*
_unescaped_unquoted_find_next_delim(LogCSVParser *self, UnescapedParserState *pstate, const gchar* src)
{
  if (self->string_delimiters)
    {
      pstate->next_string_delim =  g_string_list_find_first(self->string_delimiters,
                                                            src, 
                                                            &pstate->delim_len);
    }

  pstate->next_char_delim = (guchar *) src + strcspn(src, self->delimiters);

  if (pstate->next_string_delim && pstate->next_string_delim <= pstate->next_char_delim)
    {
      pstate->next_delim = pstate->next_string_delim;
    }
  else if (pstate->next_char_delim)
    {
      pstate->next_delim = pstate->next_char_delim; 
    }
  else
    {
      pstate->next_delim = (guchar *) src + strlen(src);
    }
  
  return pstate->next_delim;
}

static gint
_unescaped_get_column_length(LogCSVParser *self, UnescapedParserState *ps, const gchar *src)
{
  gint len;

  len = ps->next_delim - (guchar *) src;
  /* move in front of the terminating quote character */
  if (ps->current_quote && len > 0 && src[len - 1] == ps->current_quote)
    len--;
  if (len > 0 && self->flags & LOG_CSV_PARSER_STRIP_WHITESPACE)
    {
      while (len > 0 && (_is_whitespace_char(src + len - 1)))
        len--;
    }

  return len;
}

static inline void
_unescaped_move_to_next_column(UnescapedParserState *pstate, const gchar** src)
{
  *src = (gchar *) pstate->next_delim;

  if (pstate->delim_len && (pstate->next_delim == pstate->next_string_delim))
    *src += pstate->delim_len;
  else if (**src)
    (*src)++;

  pstate->cur_column = pstate->cur_column->next;
}

static guchar
_unescaped_get_current_quote(LogCSVParser *self, const gchar** src)
{
  guchar *quote = (guchar *) strchr(self->quotes_start, **src);

  if (quote != NULL)
    {
      /* ok, quote character found */
      (*src)++;
      return self->quotes_end[quote - (guchar *) self->quotes_start];
    }
  else
    {
      /* we didn't start with a quote character, no need for escaping, delimiter terminates */
      return  0;
    }
}

static inline guchar*
_unescaped_find_next_delim(LogCSVParser *self, UnescapedParserState *pstate, const gchar* src)
{
  if (pstate->current_quote)
    {
      return _unescaped_quoted_find_next_delim(self, pstate, src);
    }
  else
    {
      return _unescaped_unquoted_find_next_delim(self, pstate, src);
    }
}

static inline gboolean
_is_greedy_mode_on(LogCSVParser *self, GList **cur_column)
{
  return (*cur_column &&
         (*cur_column)->next == NULL &&
         self->flags & LOG_CSV_PARSER_GREEDY
         ) != 0 ? TRUE : FALSE;
}

static inline void
_parse_remaining_message(LogCSVParser *self, GList **cur_column, LogMessage *msg, const gchar **src)
{
  /* greedy mode, the last column gets it all, without taking escaping, quotes or anything into account */
  log_msg_set_value_by_name(msg, (gchar *) (*cur_column)->data, *src, -1);
  *cur_column = NULL;
  *src = NULL;
}

static gboolean
log_csv_parser_process_unescaped(LogCSVParser *self, LogMessage *msg, const gchar* src)
{
  gint len;
  UnescapedParserState pstate;
  unescaped_parser_state_init(&pstate, self->super.columns);
  /* no escaping, no need to keep state, we split input and trim if necessary */

  while (pstate.cur_column && *src)
    {
      unescaped_parser_state_reset_delimiters(&pstate);
      pstate.current_quote = _unescaped_get_current_quote(self, &src);

      if (self->flags & LOG_CSV_PARSER_STRIP_WHITESPACE)
        {
          _strip_whitespace_left(&src);
        }
        
      pstate.next_delim = _unescaped_find_next_delim(self, &pstate, src);

      len = _unescaped_get_column_length(self, &pstate, src);

      if (self->null_value && strncmp(src, self->null_value, len) == 0)
        log_msg_set_value_by_name(msg, (gchar *) pstate.cur_column->data, "", 0);
      else
        log_msg_set_value_by_name(msg, (gchar *) pstate.cur_column->data, src, len);
      _unescaped_move_to_next_column(&pstate, &src);

      if (_is_greedy_mode_on(self, &pstate.cur_column))
        {
          _parse_remaining_message(self, &pstate.cur_column, msg, &src);

        }
    }

  if (_should_drop_as_invalid(pstate.cur_column, src, self->flags))
    {
      /* there are unfilled variables, OR not all of the input was processed
       * and "drop-invalid" flag is specified */
      return FALSE;
    }
  return TRUE;
}

static inline gchar
_escaped_get_current_quote(LogCSVParser *self, const gchar *src)
{
  gchar *quote = strchr(self->quotes_start, *src);

  if (quote != NULL)
    {
      /* ok, quote character found */
      return self->quotes_end[quote - self->quotes_start];
    }
  else
    {
      /* we didn't start with a quote character, no need for escaping, delimiter terminates */
      return 0;
    }
}

typedef enum _UnescapedParserFSAState
  {
    PS_COLUMN_START,
    PS_WHITESPACE,
    PS_VALUE,
    PS_DELIMITER
  } UnescapedParserFSAState;

typedef struct _EscapedParserState
{
  LogMessage *msg;
  const gchar *src;
  UnescapedParserFSAState state;
  GString *current_value;
  GList *current_column;
  gchar current_quote;
  gint delim_len;
  gboolean store_value;
} EscapedParserState;

static inline void
_escaped_parser_state_init(EscapedParserState *self, LogMessage *msg, GList *cur_column, const gchar *src)
{
  self->msg = msg;
  self->src = src;
  self->state = PS_COLUMN_START;
  self->current_value = g_string_sized_new(128);
  self->current_quote = 0;
  self->current_column = cur_column;
  self->delim_len = 0;
  self->store_value = FALSE;
}

static inline void
_escaped_quoted_process_next_char(LogCSVParser *self, EscapedParserState *pstate)
{
  /* quoted value */
  if ((self->flags & LOG_CSV_PARSER_ESCAPE_BACKSLASH) && *pstate->src == '\\' && *(pstate->src+1))
    {
      pstate->src++;
      g_string_append_c(pstate->current_value, *pstate->src);
      return;
    }
  else if (self->flags & LOG_CSV_PARSER_ESCAPE_DOUBLE_CHAR && *pstate->src == pstate->current_quote && *(pstate->src+1) == pstate->current_quote)
    {
      (pstate->src)++;
      g_string_append_c(pstate->current_value, *pstate->src);
      return;
    }
  if (*pstate->src == pstate->current_quote)
    {
      /* end of column */
      pstate->current_quote = 0;
      pstate->state = PS_DELIMITER;

      if (self->string_delimiters &&  g_string_list_find_first(self->string_delimiters, pstate->src, &pstate->delim_len) != ((guchar*)pstate->src + 1))
        pstate->delim_len = 0;

      return;
    }
  g_string_append_c(pstate->current_value, *pstate->src);
}

static gboolean
_escaped_is_delimiter(LogCSVParser *self, EscapedParserState *pstate)
{
  return (self->string_delimiters &&  g_string_list_find_first(self->string_delimiters, pstate->src, &pstate->delim_len) == (guchar*)pstate->src) || strchr(self->delimiters, *pstate->src);
}

static inline gint
_escaped_get_column_length(LogCSVParser *self, GString *current_value)
{
  gint len = current_value->len;

  if (self->flags & LOG_CSV_PARSER_STRIP_WHITESPACE)
    {
      while (len > 0 && _is_whitespace_char(current_value->str + len -1))
        len--;
    }

  return len;
}

static inline void
_escaped_store_value(LogCSVParser *self, EscapedParserState *ps)
{
  gint len = _escaped_get_column_length(self, ps->current_value);
  if (self->null_value && strcmp(ps->current_value->str, self->null_value) == 0)
    log_msg_set_value_by_name(ps->msg, (gchar *) ps->current_column->data, "", 0);
  else
    log_msg_set_value_by_name(ps->msg, (gchar *) ps->current_column->data, ps->current_value->str, len);
}

static inline void
_escaped_reset_variables(LogCSVParser *self, EscapedParserState *ps)
{
  g_string_truncate(ps->current_value, 0);
  ps->current_column = ps->current_column->next;
  ps->state = PS_COLUMN_START;
  ps->store_value = FALSE;

  if (ps->delim_len > 0)
    ps->src += ps->delim_len - 1;

  ps->delim_len = 0;
}

#define has_more_data(ps) (ps.current_column && *ps.src)

static gboolean
log_csv_parser_process_escaped(LogCSVParser *self, LogMessage *msg, const gchar* src)
{
  EscapedParserState pstate;

  _escaped_parser_state_init(&pstate, msg, self->super.columns, src);

  while (has_more_data(pstate))
    {
      switch (pstate.state)
        {
        case PS_COLUMN_START:
          /* check for quote character */
          pstate.state = PS_WHITESPACE;
          pstate.current_quote = _escaped_get_current_quote(self, pstate.src);
          if (!pstate.current_quote)
            continue;

          break;
        case PS_WHITESPACE:
          if ((self->flags & LOG_CSV_PARSER_STRIP_WHITESPACE) && _is_whitespace_char(pstate.src))
            {
              break;
            }
          pstate.state = PS_VALUE;
          /* fallthrough */
        case PS_VALUE:
          if (pstate.current_quote)
            {
              _escaped_quoted_process_next_char(self, &pstate);
              break;
            }
          else
            {
              /* unquoted value */
              if (_escaped_is_delimiter(self, &pstate))
                {
                  pstate.state = PS_DELIMITER;
                  continue;
                }
              g_string_append_c(pstate.current_value, *pstate.src);
            }
          break;
        case PS_DELIMITER:
          pstate.store_value = TRUE;
          break;
        }
      pstate.src++;
      if (*pstate.src == 0 || pstate.store_value)
        {
          _escaped_store_value(self, &pstate);
          _escaped_reset_variables(self, &pstate);

          if (_is_greedy_mode_on(self, &pstate.current_column))
            {
              _parse_remaining_message(self, &pstate.current_column, pstate.msg, &pstate.src);

            }
        }
    }

  g_string_free(pstate.current_value, TRUE);

  if (_should_drop_as_invalid(pstate.current_column, pstate.src, self->flags))
    {
      /* there are unfilled variables, OR not all of the input was processed
       * and "drop-invalid" flag is specified */
      return FALSE;
    }
  return TRUE;
}

static inline gboolean
_should_not_escape(guint32 flags)
{
  return ((flags & LOG_CSV_PARSER_ESCAPE_NONE) ||
         ((flags & LOG_CSV_PARSER_ESCAPE_MASK) == 0));
}

static inline gboolean
_should_escape(guint32 flags)
{
  return flags & (LOG_CSV_PARSER_ESCAPE_BACKSLASH+LOG_CSV_PARSER_ESCAPE_DOUBLE_CHAR);
}

static gboolean
log_csv_parser_process(LogParser *s, LogMessage **pmsg, const LogPathOptions *path_options, const gchar *input, gsize input_len)
{
  LogCSVParser *self = (LogCSVParser *) s;
  LogMessage *msg = *pmsg;
  const gchar *src = input;

  if (_should_not_escape(self->flags))
    return log_csv_parser_process_unescaped(self, msg, src);
  else if (_should_escape(self->flags))
    return log_csv_parser_process_escaped(self, msg, src);
  return FALSE;
}

static LogPipe *
log_csv_parser_clone(LogProcessPipe *s)
{
  LogCSVParser *self = (LogCSVParser *) s;
  LogCSVParser *cloned;
  GList *l;

  cloned = (LogCSVParser *) log_csv_parser_new();
  g_free(cloned->delimiters);
  g_free(cloned->quotes_start);
  g_free(cloned->quotes_end);
  if (cloned->string_delimiters)
    g_list_free_full(cloned->string_delimiters, g_free);

  cloned->delimiters = g_strdup(self->delimiters);
  cloned->quotes_start = g_strdup(self->quotes_start);
  cloned->quotes_end = g_strdup(self->quotes_end);
  cloned->null_value = self->null_value ? g_strdup(self->null_value) : NULL;
  cloned->flags = self->flags;
  cloned->string_delimiters = self->string_delimiters;

  cloned->super.super.template = log_template_ref(self->super.super.template);
  for (l = self->super.columns; l; l = l->next)
    {
      cloned->super.columns = g_list_append(cloned->super.columns, g_strdup(l->data));
    }
  return &cloned->super.super.super.super;
}

static void
log_csv_parser_free(LogPipe *s)
{
  LogCSVParser *self = (LogCSVParser *) s;

  if (self->quotes_start)
    g_free(self->quotes_start);
  if (self->quotes_end)
    g_free(self->quotes_end);
  if (self->null_value)
    g_free(self->null_value);
  if (self->delimiters)
    g_free(self->delimiters);
  if (self->string_delimiters)
    g_list_free_full(self->string_delimiters, g_free);
  log_column_parser_free_method(s);
}

/*
 * Parse comma-separated values from a log message.
 */
LogColumnParser *
log_csv_parser_new(void)
{
  LogCSVParser *self = g_new0(LogCSVParser, 1);

  log_column_parser_init_instance(&self->super);
  self->super.super.super.super.free_fn = log_csv_parser_free;
  self->super.super.super.clone = log_csv_parser_clone;
  self->super.super.process = log_csv_parser_process;
  log_csv_parser_set_delimiters(&self->super, " ");
  log_csv_parser_set_quote_pairs(&self->super, "\"\"''");
  self->flags = LOG_CSV_PARSER_FLAGS_DEFAULT;
  return &self->super;
}

guint32
log_csv_parser_lookup_flag(const gchar *flag)
{
  if (strcmp(flag, "escape-none") == 0)
    return LOG_CSV_PARSER_ESCAPE_NONE;
  else if (strcmp(flag, "escape-backslash") == 0)
    return LOG_CSV_PARSER_ESCAPE_BACKSLASH;
  else if (strcmp(flag, "escape-double-char") == 0)
    return LOG_CSV_PARSER_ESCAPE_DOUBLE_CHAR;
  else if (strcmp(flag, "strip-whitespace") == 0)
    return LOG_CSV_PARSER_STRIP_WHITESPACE;
  else if (strcmp(flag, "greedy") == 0)
    return LOG_CSV_PARSER_GREEDY;
  else if (strcmp(flag, "drop-invalid") == 0)
    return LOG_CSV_PARSER_DROP_INVALID;
  msg_error("Unknown CSV parser flag", evt_tag_str("flag", flag), NULL);
  return 0;
}

guint32
log_csv_parser_get_flags(LogColumnParser *s)
{
  LogCSVParser *self = (LogCSVParser *) s;

  return self->flags;
}
