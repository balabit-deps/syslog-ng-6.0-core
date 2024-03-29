/*
 * Copyright (c) 2016 Balabit
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

#ifndef GENERATE_AGENT_H
#define GENERATE_AGENT_H 1

#include "persist-tool.h"
#include "syslog-ng.h"
#include "mainloop.h"
#include "persist-state.h"
#include "plugin.h"
#include "state.h"
#include "cfg.h"

extern gboolean force_generate_agent;
extern gchar *generate_agent_output_dir;
extern gchar *xml_config_file;

gint generate_agent_main(int argc, char *argv[]);

#endif
