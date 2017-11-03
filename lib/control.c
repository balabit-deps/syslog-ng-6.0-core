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

#include "control.h"
#include "control_server.h"
#include "query-commands.h"
#include "gsocket.h"
#include "messages.h"
#include "stats.h"
#include "stats/stats-counter.h"
#include "misc.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <iv.h>

static ControlServer *control_server;
static GList *command_list = NULL;

void
control_register_command(const gchar *command_name, const gchar *description, CommandFunction function)
{
  Commands *new_command = g_new0(Commands, 1);
  new_command->command = command_name;
  new_command->description = description;
  new_command->func = function;
  command_list = g_list_append(command_list, new_command);
};

static GString *
control_connection_send_stats(GString *command)
{
  msg_debug("control_connection_send_stats", evt_tag_str("command", command->str), NULL);

  gchar *stats = stats_generate_csv();
  GString *result = g_string_new(stats);
  g_free(stats);
  return result;
}

static GString *
control_connection_reset_stats(GString *command)
{
  GString *result = g_string_new("The statistics of syslog-ng have been reset to 0.");
  stats_reset_counters();
  return result;
}

static GString *
control_connection_message_log(GString *command)
{
  msg_debug("control_connection_message_log", evt_tag_str("command", command->str), NULL);
  gchar **cmds = g_strsplit(command->str, " ", 3);
  gboolean on;
  int *type = NULL;
  GString *result = g_string_sized_new(128);

  if (!cmds[1])
    {
      g_string_assign(result,"Invalid arguments received, expected at least one argument");
      goto exit;
    }

  if (g_str_equal(cmds[1], "DEBUG"))
    type = &debug_flag;
  else if (g_str_equal(cmds[1], "VERBOSE")) 
    type = &verbose_flag;
  else if (g_str_equal(cmds[1], "TRACE")) 
    type = &trace_flag;

  if (type)
    {
      if (cmds[2])
        {
          on = g_str_equal(cmds[2], "ON");
          if (*type != on)
            {
              msg_info("Verbosity changed", evt_tag_str("type", cmds[1]), evt_tag_int("on", on), NULL);
              *type = on;
            }

          g_string_assign(result,"OK");
        }
      else
        {
          g_string_printf(result,"%s=%d", cmds[1], *type);
        }
    }
  else
    return g_string_new("Invalid arguments received");
exit:
  g_strfreev(cmds);
  return result;
}

static GString *
control_connection_stop_process(GString *command)
{
  GString *result = g_string_new("OK");
  main_loop_terminate();
  return result;
}

static GString *
control_connection_reload(GString *command)
{
  GString *result = g_string_new("OK");
  main_loop_reload();
  return result;
}

Commands commands[] = {
  { "STATS", NULL, control_connection_send_stats },
  { "RESET_STATS", NULL, control_connection_reset_stats },
  { "LOG", NULL, control_connection_message_log },
  { "STOP", NULL, control_connection_stop_process },
  { "RELOAD", NULL, control_connection_reload },
  { NULL, NULL, NULL },
};

static void
register_default_commands()
{
  int i;
  Commands *cmd;

  for (i = 0; commands[i].command != NULL; i++)
    {
      cmd = &commands[i];
      control_register_command(cmd->command, cmd->description, cmd->func);
    }
}

static void
register_query_commands()
{
  control_register_command("QUERY", NULL, process_query_command);
}

void
control_init(const gchar *control_name)
{
  register_default_commands();
  register_query_commands();
  control_server = control_server_new(control_name, command_list);
  control_server_start(control_server);
}

void
control_destroy(void)
{
  control_server_free(control_server);
}
