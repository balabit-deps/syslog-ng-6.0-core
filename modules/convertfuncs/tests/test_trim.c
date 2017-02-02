/*
 * Copyright (c) 2017 Balabit
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

#include "logmsg.h"
#include "cfg.h"
#include "plugin.h"
#include "testutils.h"
#include "config.h"
#include <stdio.h>

extern gchar *module_path;

static LogMessage *msg;
static LogTemplate *template;

static void
assert_trimmed_value(const gchar *value, const gchar *expected_trimmed_value)
{
  GString *result = g_string_sized_new(512);

  log_msg_set_value(msg, LM_V_HOST, value, -1);
  log_template_format(template, msg, NULL, 0, 0, "TEST", result);

  assert_string(result->str, expected_trimmed_value, "Bad formatting");

  g_string_free(result, TRUE);
}

static void
test_trim_template_function()
{
  gchar *template_string;
  GError *err = NULL;

  msg = log_msg_new_empty();
  template = log_template_new(configuration, NULL);

  template_string = "$(trim)";
  assert_true(log_template_compile(template, template_string, &err), "CAN't COMPILE!");
  assert_trimmed_value("", "trim: syntax error. Syntax is: trim subject");

  template_string = "$(trim $HOST $HOST)";
  assert_true(log_template_compile(template, template_string, &err), "CAN't COMPILE!");
  assert_trimmed_value("", "trim: syntax error. Syntax is: trim subject");

  template_string = "$(trim $HOST)";
  assert_true(log_template_compile(template, template_string, &err), "CAN't COMPILE!");

  assert_trimmed_value("", "");
  assert_trimmed_value("a", "a");
  assert_trimmed_value(" ", "");
  assert_trimmed_value("   a   ", "a");
  assert_trimmed_value("  a  b   ", "a  b");
  assert_trimmed_value("  ÁRVÍZTŰRŐ  TÜKÖRFÚRÓGÉP  ", "ÁRVÍZTŰRŐ  TÜKÖRFÚRÓGÉP");
  assert_trimmed_value(" \n\t\r  a  b \n\t\r ", "a  b");

  log_msg_unref(msg);
  log_template_unref(template);
}

int main(void)
{
  log_template_global_init();
  log_msg_registry_init();
  configuration = cfg_new(VERSION_VALUE_3_2);
#ifdef _WIN32
  g_free(module_path);
  module_path = g_strdup("../");
#endif
  assert_true(plugin_load_module("convertfuncs", configuration, NULL), "Can't find convertfuncs plugin in: %s", module_path);
  test_trim_template_function();
  log_msg_registry_deinit();
  return 0;
}
