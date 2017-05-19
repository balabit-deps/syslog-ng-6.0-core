/*
 * Copyright (c) 2010-2015 Balabit
 * Copyright (c) 2010-2015 Balázs Scheidler
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

#include "plugin.h"
#include "filter.h"
#include "filter-expr-parser.h"
#include "cfg.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

static void
append_args(gint argc, GString *argv[], GString *result)
{
  gint i;

  for (i = 0; i < argc; i++)
    {
      g_string_append_len(result, argv[i]->str, argv[i]->len);
      if (i < argc - 1)
        g_string_append_c(result, ' ');
    }
}

static void
tf_echo(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  append_args(argc, argv, result);
}

TEMPLATE_FUNCTION_SIMPLE(tf_echo);

/*
 * $(strip $arg1 $arg2 ...)
 */
static void
tf_strip(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  gint i;

  for (i = 0; i < argc; i++)
    {
      if (argv[i]->len == 0)
        continue;

      gint spaces_end = 0;
      while (isspace(argv[i]->str[argv[i]->len - spaces_end - 1]) && spaces_end < argv[i]->len)
        spaces_end++;

      if (argv[i]->len == spaces_end)
        continue;

      gint spaces_start = 0;
      while (isspace(argv[i]->str[spaces_start]))
        spaces_start++;

      if (result->len > 0)
        g_string_append_c(result, ' ');

      g_string_append_len(result, &argv[i]->str[spaces_start], argv[i]->len - spaces_end - spaces_start);
    }
}

TEMPLATE_FUNCTION_SIMPLE(tf_strip);

static gboolean
tf_parse_int(const gchar *s, long *d)
{
  gchar *endptr;
  glong val;

  errno = 0;
  val = strtoll(s, &endptr, 10);

  if ((errno == ERANGE && (val == LONG_MAX || val == LONG_MIN))
      || (errno != 0 && val == 0))
    return FALSE;

  if (endptr == s || *endptr != '\0')
    return FALSE;

  *d = val;
  return TRUE;
}

static gboolean
tf_num_parse(gint argc, GString *argv[],
	     const gchar *func_name, glong *n, glong *m)
{
  if (argc != 2)
    {
      msg_error("Template function requires two arguments.",
		evt_tag_str("function", func_name), NULL);
      return FALSE;
    }

  if (!tf_parse_int(argv[0]->str, n))
    {
      msg_error("Parsing failed, template function's first argument is not a number",
		evt_tag_str("function", func_name),
		evt_tag_str("arg1", argv[0]->str), NULL);
      return FALSE;
    }

  if (!tf_parse_int(argv[1]->str, m))
    {
      msg_error("Parsing failed, template function's first argument is not a number",
		evt_tag_str("function", func_name),
		evt_tag_str("arg1", argv[1]->str), NULL);
      return FALSE;
    }

  return TRUE;
}

static void
tf_num_plus(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  glong n, m;

  if (!tf_num_parse(argc, argv, "+", &n, &m))
    return;

  g_string_append_printf(result, "%li", n + m);
}

TEMPLATE_FUNCTION_SIMPLE(tf_num_plus);

static void
tf_num_minus(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  glong n, m;

  if (!tf_num_parse(argc, argv, "-", &n, &m))
    return;

  g_string_append_printf(result, "%li", n - m);
}

TEMPLATE_FUNCTION_SIMPLE(tf_num_minus);

static void
tf_num_multi(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  glong n, m;

  if (!tf_num_parse(argc, argv, "*", &n, &m))
    return;

  g_string_append_printf(result, "%li", n * m);
}

TEMPLATE_FUNCTION_SIMPLE(tf_num_multi);

static void
tf_num_div(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  glong n, m;

  if (!tf_num_parse(argc, argv, "/", &n, &m))
    return;

  if (m == 0)
    {
      msg_error ("$(/): Division by zero in the template function",
		 NULL);
      return;
    }

  g_string_append_printf(result, "%li", (glong)(n / m));
}

TEMPLATE_FUNCTION_SIMPLE(tf_num_div);

static void
tf_num_mod(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  glong n, m;

  if (!tf_num_parse(argc, argv, "%", &n, &m))
    return;

  if (m == 0)
    {
      msg_error ("$(%): Division by zero in the template function",
		 NULL);
      return;
    }

  g_string_append_printf(result, "%li", n % m);
}

TEMPLATE_FUNCTION_SIMPLE(tf_num_mod);

typedef struct _TFCondState
{
  TFSimpleFuncState super;
  FilterExprNode *filter;
} TFCondState;

void
tf_cond_free_state(TFCondState *args)
{
  if (args->filter)
    filter_expr_unref(args->filter);
  tf_simple_func_free_state(args);
}

gboolean
tf_cond_prepare(LogTemplateFunction *self, LogTemplate *parent, gint argc, gchar *argv[], gpointer *state, GDestroyNotify *state_destroy, GError **error)
{
  TFCondState *args;
  CfgLexer *lexer;

  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  args = g_malloc0(sizeof(TFCondState));

  lexer = cfg_lexer_new_buffer(argv[1], strlen(argv[1]));
  if (!cfg_run_parser(parent->cfg, lexer, &filter_expr_parser, (gpointer *) &args->filter, NULL))
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE, "Error parsing conditional filter expression");
      goto error;
    }

  /* First argument is function name, second is condition, do not try to compile them as templates */
  if (!tf_simple_func_compile_templates(parent, argc - 2, &argv[2], &args->super, error))
    goto error;

  *state = args;
  *state_destroy = (GDestroyNotify) tf_cond_free_state;
  return TRUE;
 error:
  tf_cond_free_state(args);
  return FALSE;

}

void
tf_cond_eval(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint seq_num, const gchar *context_id)
{
  return;
}

gboolean
tf_grep_prepare(LogTemplateFunction *self, LogTemplate *parent, gint argc, gchar *argv[], gpointer *state, GDestroyNotify *state_destroy, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (argc < 3)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE, "$(grep) requires at least two arguments");
      return FALSE;
    }
  return tf_cond_prepare(self, parent, argc, argv, state, state_destroy, error);
}


void
tf_grep_call(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint seq_num, const gchar *context_id, GString *result)
{
  gint i, msg_ndx;
  gboolean first = TRUE;
  TFCondState *args = (TFCondState *) state;

  for (msg_ndx = 0; msg_ndx < num_messages; msg_ndx++)
    {
      LogMessage *msg = messages[msg_ndx];

      if (filter_expr_eval(args->filter, msg))
        {
          for (i = 0; i < args->super.number_of_templates; i++)
            {
              if (!first)
                g_string_append_c(result, ',');
              log_template_append_format(args->super.templates[i], msg, opts, tz, seq_num, context_id, result);
              first = FALSE;
            }
        }
    }
}

gboolean
tf_if_prepare(LogTemplateFunction *self, LogTemplate *parent, gint argc, gchar *argv[], gpointer *state, GDestroyNotify *state_destroy, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

  if (argc != 4)
    {
      g_set_error(error, LOG_TEMPLATE_ERROR, LOG_TEMPLATE_ERROR_COMPILE, "$(if) requires three arguments");
      return FALSE;
    }
  return tf_cond_prepare(self, parent, argc, argv, state, state_destroy, error);
}

TEMPLATE_FUNCTION(tf_grep, tf_grep_prepare, tf_cond_eval, tf_grep_call, NULL);

void
tf_if_call(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint seq_num, const gchar *context_id, GString *result)
{
  TFCondState *args = (TFCondState *) state;

  if (filter_expr_eval_with_context(args->filter, messages, num_messages))
    {
      log_template_append_format_with_context(args->super.templates[0], messages, num_messages, opts, tz, seq_num, context_id, result);
    }
  else
    {
      log_template_append_format_with_context(args->super.templates[1], messages, num_messages, opts, tz, seq_num, context_id, result);
    }
}

TEMPLATE_FUNCTION(tf_if, tf_if_prepare, tf_cond_eval, tf_if_call, NULL);


void
tf_indent_multi_line(LogMessage *msg, gint argc, GString *argv[], GString *text)
{
  gchar *p, *new_line;

  /* append the message text(s) to the template string */
  append_args(argc, argv, text);

  /* look up the \n-s and insert a \t after them */
  p = text->str;
  new_line = memchr(p, '\n', text->len);
  while(new_line)
    {
      if (*(gchar *)(new_line + 1) != '\t')
        {
          g_string_insert_c(text, new_line - p + 1, '\t');
        }
      new_line = memchr(new_line + 1, '\n', p + text->len - new_line);
    }
}

TEMPLATE_FUNCTION_SIMPLE(tf_indent_multi_line);


gboolean
tf_context_length_prepare(LogTemplateFunction *self, LogTemplate *parent, gint argc, gchar *argv[], gpointer *state, GDestroyNotify *state_destroy, GError **error)
{
  return TRUE;
}

void
tf_context_length_call(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint seq_num, const gchar *context_id, GString *result)
{
  g_string_append_printf(result, "%d", num_messages);
}

void
tf_context_length_eval(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint seq_num, const gchar *context_id)
{
  return;
}

TEMPLATE_FUNCTION(tf_context_length, tf_context_length_prepare, tf_context_length_eval, tf_context_length_call, NULL);

static Plugin basicfuncs_plugins[] =
{
  TEMPLATE_FUNCTION_PLUGIN(tf_echo, "echo"),
  TEMPLATE_FUNCTION_PLUGIN(tf_strip, "strip"),
  TEMPLATE_FUNCTION_PLUGIN(tf_grep, "grep"),
  TEMPLATE_FUNCTION_PLUGIN(tf_if, "if"),
  TEMPLATE_FUNCTION_PLUGIN(tf_indent_multi_line, "indent-multi-line"),
  TEMPLATE_FUNCTION_PLUGIN(tf_num_plus, "+"),
  TEMPLATE_FUNCTION_PLUGIN(tf_num_minus, "-"),
  TEMPLATE_FUNCTION_PLUGIN(tf_num_multi, "*"),
  TEMPLATE_FUNCTION_PLUGIN(tf_num_div, "/"),
  TEMPLATE_FUNCTION_PLUGIN(tf_num_mod, "%"),
  TEMPLATE_FUNCTION_PLUGIN(tf_context_length, "context-length")
};

gboolean
basicfuncs_module_init(GlobalConfig *cfg, CfgArgs *args)
{
  plugin_register(cfg, basicfuncs_plugins, G_N_ELEMENTS(basicfuncs_plugins));
  return TRUE;
}

const ModuleInfo module_info =
{
  .canonical_name = "basicfuncs",
  .version = VERSION,
  .description = "The basicfuncs module provides various template functions for syslog-ng.",
  .core_revision = SOURCE_REVISION,
  .plugins = basicfuncs_plugins,
  .plugins_len = G_N_ELEMENTS(basicfuncs_plugins),
};
