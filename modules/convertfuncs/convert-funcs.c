/*
 * Copyright (c) 2010-2014 Balabit
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
 */

#include "plugin.h"
#include "cfg.h"
#include "gsocket.h"
#include "value-pairs/value-pairs.h"
#include "str-format.h"
#include "misc.h"

static void
tf_ipv4_to_int(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  gint i;

  for (i = 0; i < argc; i++)
    {
      struct in_addr ina;

      g_inet_aton(argv[i]->str, &ina);
      g_string_append_printf(result, "%lu", (gulong) ntohl(ina.s_addr));
      if (i < argc - 1)
        g_string_append_c(result, ',');
    }
}

TEMPLATE_FUNCTION_SIMPLE(tf_ipv4_to_int);

typedef struct _SnareFormatOptions {
  GlobalConfig *cfg;
  LogTemplate *protocol_template;
  LogTemplate *event_template;
  LogTemplate *default_template;
  gchar         *delimiter;
  guint        criticality;
} SnareFormatOptions;

static void
snare_format_options_destroy(SnareFormatOptions *options)
{
  log_template_unref(options->protocol_template);
  log_template_unref(options->event_template);
  log_template_unref(options->default_template);
  g_free(options->delimiter);
  g_free(options);
}

static gboolean
tf_format_snare_prepare(LogTemplateFunction *self, LogTemplate *parent,
                gint argc, gchar *argv[],
                gpointer *state, GDestroyNotify *state_destroy,
                GError **error)
{
  GError *err = NULL;
  GString *event_template_str = g_string_sized_new(512);
  SnareFormatOptions *options = g_new0(SnareFormatOptions,1);
  GOptionContext *ctx;
  GOptionGroup *og;

  GOptionEntry snare_options[] = {
    { "delimiter", 'd', 0, G_OPTION_ARG_STRING, &options->delimiter,
      NULL, NULL },
    { "criticality", 'c', 0, G_OPTION_ARG_INT, &options->criticality,
      NULL, NULL },
    { NULL }
  };

  options->criticality = 1;
  options->cfg = parent->cfg;

  ctx = g_option_context_new ("snare-format");
  og = g_option_group_new (NULL, NULL, NULL, NULL, NULL);
  g_option_group_add_entries (og, snare_options);
  g_option_context_set_main_group (ctx, og);

  if (!g_option_context_parse (ctx, &argc, &argv, error))
    {
      g_option_context_free (ctx);
      g_free(options);
      g_string_free(event_template_str,TRUE);
      return FALSE;
    }
  g_option_context_free (ctx);
  if (!options->delimiter)
    {
      options->delimiter = g_strdup("\t");
    }
  g_string_append(event_template_str,"MSWinEventLog");
  g_string_append(event_template_str,options->delimiter);
  format_uint32_padded(event_template_str, 1, '0', 10, options->criticality);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_NAME '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_GLOBAL_COUNTER '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace \"$WEEKDAY $MONTHNAME $DAY $HOUR:$MIN:$SEC $YEAR\" '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_ID '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_PROVIDER '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_SNARE_USER '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_SID_TYPE '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_TYPE '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_HOST '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_CATEGORY '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s%s$(replace $EVENT_MESSAGE '%s' ' ')",options->delimiter,options->delimiter,options->delimiter);
  g_string_append_printf(event_template_str,"%s$(replace $EVENT_CONTAINER_COUNTER '%s' ' ')",options->delimiter,options->delimiter);
  g_string_append(event_template_str,"\n");

  options->protocol_template = log_template_new(options->cfg,"snare_protocol_template");
  log_template_compile(options->protocol_template,"<${PRI}>${BSDDATE} ${HOST} ", &err);

  options->event_template = log_template_new(options->cfg,"snare_event_template");
  log_template_compile(options->event_template,event_template_str->str, &err);

  options->default_template = log_template_new(options->cfg, "default snare template");
  log_template_compile(options->default_template, "${MESSAGE}\n", &err);

  *state = options;
  *state_destroy = (GDestroyNotify)snare_format_options_destroy;
  g_string_free(event_template_str,TRUE);
  return err == NULL;
}

static void
tf_format_snare_call(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs,
             LogMessage **messages, gint num_messages, const LogTemplateOptions *opts,
             gint tz, gint seq_num, const gchar *context_id, GString *result)
{
  gint i;
  gssize event_id_len;
  static NVHandle event_id_handle = 0;
  SnareFormatOptions *options = state;

  if (!event_id_handle)
    event_id_handle = log_msg_get_value_handle("EVENT_ID");
  for(i = 0; i < num_messages; i++)
  {
    LogMessage *msg = messages[i];
    log_template_append_format(options->protocol_template, msg, opts, tz, seq_num, context_id, result);
    log_msg_get_value(msg, event_id_handle, &event_id_len);
    if (event_id_len != 0)
      {
        log_template_append_format(options->event_template, msg, opts, tz, seq_num, context_id, result);
      }
    else
      {
        log_template_append_format(options->default_template, msg, opts, tz, seq_num, context_id, result);
      }
  }
  return;
}

static void
tf_format_snare_eval (LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs,
              LogMessage **messages, gint num_messages, const LogTemplateOptions *opts,
              gint tz, gint seq_num, const gchar *context_id)
{
  return;
}

TEMPLATE_FUNCTION(tf_format_snare,tf_format_snare_prepare,tf_format_snare_eval,tf_format_snare_call,NULL);

typedef struct _StrcutOptions {
  GlobalConfig *cfg;
  gchar        *delimiters;
  gint         start_from;
  guint        length;
  LogTemplate  *compiled_template;
} StrcutOptions;

#define MAX_START_FROM 0x00FF /* It is expandable, but at first we thought that 255 is far enough */
#define MAX_LENGTH     0x00FF /* It is expandable, but at first we thought that 255 is far enough */

static void
cut_options_destroy(StrcutOptions *options)
{
  if (options->compiled_template)
    log_template_unref(options->compiled_template);
  g_free(options->delimiters);
  g_free(options);
}

static gboolean
tf_cut_prepare(LogTemplateFunction *self, LogTemplate *parent,
                gint argc, gchar *argv[],
                gpointer *state, GDestroyNotify *state_destroy,
                GError **error)
{
  StrcutOptions *options = g_new0(StrcutOptions,1);
  GOptionContext *ctx;
  GOptionGroup *og;
  gboolean result = TRUE;

  GOptionEntry cut_options[] = {
    { "delimiters", 'd', 0, G_OPTION_ARG_STRING, &options->delimiters, NULL, NULL },
    { "start_from", 's', 0, G_OPTION_ARG_INT, &options->start_from, NULL, NULL },
    { "length", 'l', 0, G_OPTION_ARG_INT, &options->length, NULL, NULL },
    { NULL }
  };

  options->start_from = 0x0100;
  options->length = 0x0100;

  ctx = g_option_context_new ("cut");
  og = g_option_group_new (NULL, NULL, NULL, NULL, NULL);
  g_option_group_add_entries (og, cut_options);
  g_option_context_set_main_group (ctx, og);

  if (!g_option_context_parse (ctx, &argc, &argv, error))
    {
      result = FALSE;
      goto exit;
    }
  if (options->delimiters == NULL)
    {
      result = FALSE;
      goto exit;
    }
  if ((options->start_from > MAX_START_FROM) || (options->start_from < -MAX_START_FROM))
    {
      result = FALSE;
      goto exit;
    }
  if (options->length > MAX_LENGTH)
    {
      result = FALSE;
      goto exit;
    }
  if (argc != 2)
    {
      result = FALSE;
      goto exit;
    }
  options->compiled_template = log_template_new(parent->cfg,NULL);
  if (!log_template_compile(options->compiled_template,argv[1],error))
    {
      result = FALSE;
      log_template_unref(options->compiled_template);
      goto exit;
    }
  *state = options;
  *state_destroy = (GDestroyNotify)cut_options_destroy;
exit:
  if (!result)
    {
      g_free(options->delimiters);
      g_free(options);
    }
  g_option_context_free(ctx);
  return result;
}

static GPtrArray *
tf_cut_get_tokens(gchar *subject,gchar *delimiters)
{
  GPtrArray *result = g_ptr_array_new();
  gchar *p = subject;
  g_ptr_array_add(result,subject);
  while(*p)
    {
      if(strchr(delimiters,*p))
        {
          g_ptr_array_add(result,p + 1);
        }
      p++;
    }
  return result;
}

static int
tf_cut_get_valid_index(int max,int index)
{
  int result = 0;
  if (index >= 0)
    {
      result = MIN(max,index);
    }
  else
    {
      result = MAX(0, max + index + 1);
    }
  return result;
}

static void
tf_cut_call(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs,
             LogMessage **messages, gint num_messages, const LogTemplateOptions *opts,
             gint tz, gint seq_num, const gchar *context_id, GString *result)
{
  StrcutOptions *options = (StrcutOptions *)state;
  int i;
  for(i = 0; i < num_messages; i++)
    {
      LogMessage *msg = messages[i];
      GString *template = g_string_sized_new(256);
      GPtrArray *tokens = NULL;
      int start_index = options->start_from;
      int end_index = 0;
      int max_start_index = 0;
      int max_end_index = 0;
      gchar *end = NULL;
      gchar *start = NULL;

      log_template_format(options->compiled_template,msg,opts,tz,seq_num,context_id,template);
      tokens = tf_cut_get_tokens(template->str,options->delimiters);

      max_start_index = tokens->len - 1;
      max_end_index = tokens->len;

      start_index = tf_cut_get_valid_index(max_start_index,options->start_from);
      start = g_ptr_array_index(tokens,start_index);

      end_index = tf_cut_get_valid_index(max_end_index, start_index + options->length);
      if (end_index < max_end_index)
        {
          end = g_ptr_array_index(tokens,end_index);
          g_string_append_len(result,start,end - start - 1);
        }
      else
        {
          g_string_append(result,start);
        }

      g_string_free(template,TRUE);
      g_ptr_array_free(tokens,TRUE);
    }
  return;
}

static void
tf_cut_eval (LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs,
              LogMessage **messages, gint num_messages, const LogTemplateOptions *opts,
              gint tz, gint seq_num, const gchar *context_id)
{
  return;
}

TEMPLATE_FUNCTION(tf_cut,tf_cut_prepare,tf_cut_eval,tf_cut_call, NULL);

static void
tf_replace(LogMessage *msg, gint argc, GString *argv[], GString *result)
{
  gchar *value = argv[0]->str;
  if ((argc != 3) || (argv[1]->len != 1) || (argv[2]->len != 1))
  {
    g_string_append(result,"replace: syntax error. Syntax is: replace subject search_character replace_character");
    return;
  }
  gchar from = argv[1]->str[0];
  gchar to = argv[2]->str[0];
  gchar *p = value;
  while(*p)
  {
    if(*p == from)
      g_string_append_c(result,to);
    else
      g_string_append_c(result,*p);
    p++;
  }
  return;
}

TEMPLATE_FUNCTION_SIMPLE(tf_replace);

static void
tf_lowercase(LogMessage *msg, int argc, GString *argv[], GString *result)
{
  int i;
  for(i = 0; i < argc; i++)
    {
      gchar *value = argv[i]->str;
      gchar *converted = g_utf8_strdown(value, argv[i]->len);
      g_string_append(result,converted);
      g_free(converted);
    }
  return;
}

TEMPLATE_FUNCTION_SIMPLE(tf_lowercase);

static Plugin convert_func_plugins[] =
{
  TEMPLATE_FUNCTION_PLUGIN(tf_ipv4_to_int, "ipv4-to-int"),
  TEMPLATE_FUNCTION_PLUGIN(tf_format_snare, "format-snare"),
  TEMPLATE_FUNCTION_PLUGIN(tf_replace,"replace"),
  TEMPLATE_FUNCTION_PLUGIN(tf_lowercase,"lowercase"),
  TEMPLATE_FUNCTION_PLUGIN(tf_cut,"cut")
};

gboolean
convertfuncs_module_init(GlobalConfig *cfg, CfgArgs *args)
{
  plugin_register(cfg, convert_func_plugins, G_N_ELEMENTS(convert_func_plugins));
  return TRUE;
}

#ifndef STATIC
const ModuleInfo module_info =
{
  .canonical_name = "convertfuncs",
  .version = VERSION,
  .description = "The convertfuncs module provides template functions that perform some kind of data conversion from one representation to the other.",
  .core_revision = SOURCE_REVISION,
  .plugins = convert_func_plugins,
  .plugins_len = G_N_ELEMENTS(convert_func_plugins),
};
#endif
