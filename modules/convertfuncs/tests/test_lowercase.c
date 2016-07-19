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

#include "logmsg.h"
#include "cfg.h"
#include "plugin.h"
#include "testutils.h"
#include "config.h"
#include <stdio.h>

extern gchar *module_path;
extern gchar *path_prefix;

int test_lowercase_template_function()
{
  gchar *template_string;
  LogTemplate *template = log_template_new(configuration,NULL);
  GError *err = NULL;
  LogMessage *msg;
  GString *result = g_string_sized_new(512);

  template_string = "$(lowercase $HOST)";

  assert_true(log_template_compile(template,template_string,&err),"CAN't COMPILE!");

  msg = log_msg_new_empty();
  log_msg_set_value(msg,LM_V_HOST,"almafA",-1);
  log_template_format(template, msg, NULL, 0, 0, "TEST", result);
  assert_string(result->str,"almafa","Bad formatting");

  template_string = "$(lowercase \"$HOST Almafa\")";
  assert_true(log_template_compile(template,template_string,&err),"CAN't COMPILE!");
  log_template_format(template, msg, NULL, 0, 0, "TEST", result);
  assert_string(result->str,"almafa almafa","Bad formatting");

  template_string = "$(lowercase \"$HOST ÁRVÍZTŰRŐ TÜKÖRFÚRÓGÉP\")";
  assert_true(log_template_compile(template,template_string,&err),"CAN't COMPILE!");
  log_template_format(template, msg, NULL, 0, 0, "TEST", result);
  assert_string(result->str,"almafa árvíztűrő tükörfúrógép","Bad formatting");

  template_string = "$(lowercase ${HOST}.ÁRVÍZTŰRŐ.TÜKÖRFÚRÓGÉP)";
  assert_true(log_template_compile(template,template_string,&err),"CAN't COMPILE!");
  log_template_format(template, msg, NULL, 0, 0, "TEST", result);
  assert_string(result->str,"almafa.árvíztűrő.tükörfúrógép","Bad formatting");

  log_msg_unref(msg);
  log_template_unref(template);
  g_string_free(result, TRUE);
  return 0;
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
  assert_true(plugin_load_module("convertfuncs", configuration, NULL),"Can't find convertfuncs plugin in: %s",module_path);
  test_lowercase_template_function();
  return 0;
}
