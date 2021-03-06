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
  
#ifndef SYSLOG_NG_H_INCLUDED
#define SYSLOG_NG_H_INCLUDED

#include <config.h>

#if ENABLE_DMALLOC
#define USE_DMALLOC
#endif

#if ENABLE_DEBUG
#undef YYDEBUG
#define YYDEBUG 1
#endif

#include "compat.h"
#include <glib.h>

#define PATH_SYSLOG_NG_CONF     PATH_SYSCONFDIR "/syslog-ng.conf"
#define PATH_INSTALL_DAT	PATH_SYSCONFDIR "/install.dat"

#ifndef _WIN32
#define PATH_CONTROL_SOCKET     PATH_PIDFILEDIR "/syslog-ng.ctl"
#else
#define PATH_CONTROL_SOCKET     "\\\\.\\pipe\\syslog-ng.ctl"
#endif

#define PATH_PERSIST_CONFIG     PATH_LOCALSTATEDIR "/syslog-ng.persist"
#define PATH_QDISK              PATH_LOCALSTATEDIR
#define PATH_PATTERNDB_FILE     PATH_LOCALSTATEDIR "/patterndb.xml"
#define PATH_XSDDIR             PATH_DATADIR "/xsd"

#define LOG_PRIORITY_LISTEN 0
#define LOG_PRIORITY_READER 0
#define LOG_PRIORITY_WRITER -100
#define LOG_PRIORITY_CONNECT -150

#define SAFE_STRING(x) ((x) ? (x) : "NULL")

typedef struct _LogMessage LogMessage;
typedef struct _GlobalConfig GlobalConfig;
typedef struct _AckTracker AckTracker;
typedef struct _AckRecord AckRecord;
typedef struct _Bookmark Bookmark;

/* configuration being parsed, used by the bison generated code, NULL whenever parsing is finished. */
extern GlobalConfig *configuration;
extern gchar *default_modules;

/* Global path variables. They are filled when the libsyslog-ng.so is loaded.
 * We have to do it this way to support binary relocation using
 * SYSLOGNG_PREFIX. */
extern gchar *module_path;
extern gchar *path_prefix;
extern gchar *path_datadir;
extern gchar *path_sysconfdir;
extern gchar *path_pidfiledir;
extern gchar *path_patterndb_file;
extern gchar *cfgfilename;
extern gchar *persist_file;
extern gchar *ctlfilename;

extern gchar *qdisk_dir;

#endif
