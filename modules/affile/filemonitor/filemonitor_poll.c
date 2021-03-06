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

#include "filemonitor_poll.h"
#include "filemonitor_unix.h"
#include "filemonitor_win.h"

#include "messages.h"
#include "mainloop.h"
#include "timeutils.h"
#include "misc.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <iv.h>
#include <stdlib.h>
#include <sys/stat.h>

typedef struct _MonitorPoll
{
  MonitorBase super;
  struct iv_timer poll_timer;
} MonitorPoll;

static void file_monitor_poll_timer_callback(gpointer s);

static MonitorPoll *
monitor_source_poll_new(FileMonitor *monitor)
{
  MonitorPoll *self = g_new0(MonitorPoll,1);
  self->super.base_dir = NULL;
  IV_TIMER_INIT(&self->poll_timer);
  self->poll_timer.cookie = self;
  self->poll_timer.handler = file_monitor_poll_timer_callback;

  self->super.file_monitor = monitor;
  return self;
}

static inline void
_check_monitored_directory(FileMonitor *monitor, const gchar *dir)
{
  FMListDirectoryCallbacks cbs = {
#ifndef G_OS_WIN32
    .file = file_monitor_chk_file,
#else
    .file = file_monitor_chk_file_windows,
#endif
    .recurse_directory = file_monitor_watch_directory
  };

  file_monitor_list_directory(monitor, dir, &cbs);
}

static gboolean
file_monitor_process_poll_event(FileMonitor *monitor, MonitorPoll *source)
{
  msg_trace("file_monitor_process_poll_event", NULL);

  _check_monitored_directory(monitor, source->super.base_dir);

  return TRUE;
}

static void
file_monitor_poll_timer_callback(gpointer s)
{
  MonitorPoll *self = (MonitorPoll *)s;
  FileMonitor *file_monitor = self->super.file_monitor;

  file_monitor_process_poll_event(file_monitor, self);

  if(iv_timer_registered(&self->poll_timer))
    iv_timer_unregister(&self->poll_timer);
  iv_validate_now();
  self->poll_timer.expires = iv_now;
  timespec_add_msec(&self->poll_timer.expires, self->super.file_monitor->options->poll_freq);
  iv_timer_register(&self->poll_timer);
}

static gboolean
file_monitor_poll_init(MonitorPoll *self, const gchar *base_dir)
{
  self->super.base_dir = g_strdup(base_dir);
  return TRUE;
}

static void
monitor_poll_free(MonitorPoll *self)
{
  if (self->super.base_dir)
    g_free(self->super.base_dir);
  if (iv_timer_registered(&self->poll_timer))
    iv_timer_unregister(&self->poll_timer);

  g_free(self);
}

static MonitorBase *
file_monitor_create_poll(FileMonitor *self, const gchar *base_dir)
{
  MonitorPoll *source = monitor_source_poll_new(self);

  msg_debug("Initializing poll-based file monitoring method", evt_tag_str("directory", base_dir), NULL);
  if(!file_monitor_poll_init(source,base_dir))
    {
      monitor_poll_free(source);
      return NULL;
    }
  return &source->super;
}

static void
file_monitor_poll_start(FileMonitor *self, MonitorBase *source, const gchar *base_dir)
{
  MonitorPoll *p_source = (MonitorPoll *)source;

  _check_monitored_directory(self, base_dir);

  if(iv_timer_registered(&p_source->poll_timer))
    iv_timer_unregister(&p_source->poll_timer);
  iv_validate_now();
  p_source->poll_timer.expires = iv_now;
  timespec_add_msec(&p_source->poll_timer.expires, p_source->super.file_monitor->options->poll_freq);
  iv_timer_register(&p_source->poll_timer);
}

static gboolean
file_monitor_poll_watch_directory(FileMonitor *self, const gchar *filename_pattern)
{
  MonitorBase *source = NULL;
  g_assert(self);

  gchar *base_dir = file_monitor_resolve_base_directory_from_pattern(self, filename_pattern);

  if (!base_dir)
    return FALSE;

  if (file_monitor_is_dir_monitored(self, base_dir))
    {
      /* We are monitoring this directory already, so ignore it */
      g_free(base_dir);
      return TRUE;
    }
  else
    {
      msg_debug("Monitoring new directory", evt_tag_str("basedir", base_dir), NULL);
    }

  source = file_monitor_create_poll(self, base_dir);

  if (source == NULL)
    {
      msg_error("Failed to initialize file monitoring", evt_tag_str("basedir", base_dir), NULL);
      g_free(base_dir);
      return FALSE;
    }

  self->sources = g_slist_append(self->sources, source);
  file_monitor_poll_start(self, source, base_dir);
  g_free(base_dir);
  return TRUE;
}

static void
file_monitor_poll_destroy(gpointer source, gpointer monitor)
{
  MonitorPoll *self = (MonitorPoll *)source;
  monitor_poll_free(self);
}

static gboolean
file_monitor_poll_stop(FileMonitor *self)
{
  if (self->sources)
    {
      g_slist_foreach(self->sources, file_monitor_poll_destroy, NULL);
      g_slist_free(self->sources);
      self->sources = NULL;
    }
  return TRUE;
}

static void
file_monitor_poll_deinit(FileMonitor *self)
{
  file_monitor_poll_stop(self);
}

/**
 * file_monitor_new:
 *
 * This function constructs a new FileMonitor object.
 **/
FileMonitor *
file_monitor_poll_new(FileMonitorOptions *options)
{
  FileMonitor *self = g_new0(FileMonitor, 1);

  self->options = options;
  self->watch_directory = file_monitor_poll_watch_directory;
#ifndef G_OS_WIN32
  self->raise_caps = file_monitor_unix_raise_caps;
#endif
  self->stop = file_monitor_poll_stop;
  self->deinit = file_monitor_poll_deinit;
  self->free_fn = file_monitor_free_method;

  return self;
}
