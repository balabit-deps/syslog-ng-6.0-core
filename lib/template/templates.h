/*
 * Copyright (c) 2002-2012 Balabit
 * Copyright (c) 1998-2012 Balázs Scheidler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#ifndef TEMPLATES_H_INCLUDED
#define TEMPLATES_H_INCLUDED

#include "syslog-ng.h"
#include "timeutils.h"
#include "logmsg.h"
#include "type-hinting.h"

#define LTZ_LOCAL 0
#define LTZ_SEND  1
#define LTZ_MAX   2

#define LOG_TEMPLATE_ERROR log_template_error_quark()

GQuark log_template_error_quark(void);

enum LogTemplateError
{
  LOG_TEMPLATE_ERROR_FAILED,
  LOG_TEMPLATE_ERROR_COMPILE,
};

typedef enum
{
  ON_ERROR_DROP_MESSAGE        = 0x01,
  ON_ERROR_DROP_PROPERTY       = 0x02,
  ON_ERROR_FALLBACK_TO_STRING  = 0x04, /* Valid for type hinting
                                          only! */
  ON_ERROR_SILENT              = 0x08
} LogTemplateOnError;

typedef struct _LogTemplate
{
  gint ref_cnt;
  gchar *name;
  gchar *template;
  GList *compiled_template;
  gboolean escape;
  gboolean def_inline;
  GlobalConfig *cfg;
  GStaticMutex arg_lock;
  GPtrArray *arg_bufs;
  TypeHint type_hint;
} LogTemplate;

typedef struct _LogTemplateOptions
{
  gint ts_format;
  gchar *time_zone[LTZ_MAX];
  TimeZoneInfo *time_zone_info[LTZ_MAX];
  gint frac_digits;
  /* Template error handling settings */
  gint on_error;
} LogTemplateOptions;

typedef struct _LogMacroDef
{
  char *name;
  int id;
} LogMacroDef;

extern LogMacroDef macros[];

typedef struct _LogTemplateFunction LogTemplateFunction;
struct _LogTemplateFunction
{
  /* called when parsing the arguments to be compiled into an internal
   * representation if necessary.  Returns the compiled state in *state */
  gboolean (*prepare)(LogTemplateFunction *self, LogTemplate *parent, gint argc, gchar *argv[], gpointer *state, GDestroyNotify *state_destroy, GError **error);

  /* evaluate arguments, storing argument buffers in arg_bufs in case it
   * makes sense to reuse those buffers */
  void (*eval)(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id);

  /* call the function */
  void (*call)(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id, GString *result);
  gpointer arg;
};

typedef struct _TFSimpleFuncState
{
  gint number_of_templates;
  LogTemplate **templates;
} TFSimpleFuncState;

typedef void (*TFSimpleFunc)(LogMessage *msg, gint argc, GString *argv[], GString *result);

gboolean tf_simple_func_prepare(LogTemplateFunction *self, LogTemplate *parent, gint argc, gchar *argv[], gpointer *state, GDestroyNotify *state_destroy, GError **error);
gboolean tf_simple_func_compile_templates(LogTemplate *parent, gint number_of_raw_templates, gchar *raw_templates[], TFSimpleFuncState *state, GError **error);
void tf_simple_func_eval(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id);
void tf_simple_func_call(LogTemplateFunction *self, gpointer state, GPtrArray *arg_bufs, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint seq_num, const gchar *context_id, GString *result);
void tf_simple_func_free_state(gpointer state);

#define TEMPLATE_FUNCTION_PROTOTYPE(prefix) \
  gpointer                                                              \
  prefix ## _construct(Plugin *self,                                    \
                       GlobalConfig *cfg,                               \
                       gint plugin_type, const gchar *plugin_name)

#define TEMPLATE_FUNCTION_DECLARE(prefix)                               \
  TEMPLATE_FUNCTION_PROTOTYPE(prefix);

/* helper macros for template function plugins */
#define TEMPLATE_FUNCTION(prefix, prepare, eval, call, arg)             \
  TEMPLATE_FUNCTION_PROTOTYPE(prefix)                                   \
  {                                                                     \
    static LogTemplateFunction func = {                                 \
      prepare,                                                          \
      eval,                                                             \
      call,                                                             \
      arg                                                               \
    };                                                                  \
    return &func;                                                       \
  }

#define TEMPLATE_FUNCTION_SIMPLE(x) TEMPLATE_FUNCTION(x, tf_simple_func_prepare, tf_simple_func_eval, tf_simple_func_call, x)

#define TEMPLATE_FUNCTION_PLUGIN(x, tf_name) \
  {                                     \
    .type = LL_CONTEXT_TEMPLATE_FUNC,   \
    .name = tf_name,                    \
    .construct = x ## _construct,       \
  }


/* appends the formatted output into result */

void log_template_set_escape(LogTemplate *self, gboolean enable);
gboolean log_template_set_type_hint(LogTemplate *self, const gchar *hint, GError **error);
gboolean log_template_compile(LogTemplate *self, const gchar *template, GError **error);
void log_template_format(LogTemplate *self, LogMessage *lm, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id, GString *result);
void log_template_append_format(LogTemplate *self, LogMessage *lm, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id, GString *result);
void log_template_append_format_with_context(LogTemplate *self, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id, GString *result);
void log_template_format_with_context(LogTemplate *self, LogMessage **messages, gint num_messages, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id, GString *result);

/* low level macro functions */
guint log_macro_lookup(gchar *macro, gint len);
gboolean log_macro_expand(GString *result, gint id, gboolean escape, const LogTemplateOptions *opts, gint tz, gint32 seq_num, const gchar *context_id, LogMessage *msg);

LogTemplate *log_template_new(GlobalConfig *cfg, gchar *name);
LogTemplate *log_template_ref(LogTemplate *s);
void log_template_unref(LogTemplate *s);


void log_template_options_init(LogTemplateOptions *options, GlobalConfig *cfg);
void log_template_options_destroy(LogTemplateOptions *options);
void log_template_options_defaults(LogTemplateOptions *options);

void log_template_global_init(void);

gboolean log_template_on_error_parse(const gchar *on_error, gint *out);
void log_template_options_set_on_error(LogTemplateOptions *options, gint on_error);

#endif
