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

#ifndef AFFILE_COMMON_H_INCLUDED
#define AFFILE_COMMON_H_INCLUDED

#include "affile.h"

struct _AFFileDestWriter
{
  LogPipe super;
  GStaticMutex lock;
  AFFileDestDriver *owner;
  gchar *filename;
  LogPipe *writer;
  time_t last_msg_stamp;
  time_t last_open_stamp;
  time_t time_reopen;
  struct iv_timer reap_timer;
  gboolean reopen_pending, queue_pending;
};


void affile_file_monitor_stop(AFFileSourceDriver *self);
void affile_file_monitor_init(AFFileSourceDriver *self, const gchar *filename);
gboolean affile_sd_open_file(AFFileSourceDriver *self, gchar *name, gint *fd);
gboolean affile_sd_monitor_callback(const gchar *filename, gpointer s, FileActionType action_type);
gboolean affile_dw_reopen_file(AFFileDestDriver *self, gchar *name, gint *fd);
gboolean affile_dw_reopen(AFFileDestWriter *self);

gboolean is_wildcard_filename(const gchar *filename);
gboolean affile_is_spurious_path(const gchar *name, const gchar *spurious_paths[]);
gchar* affile_pop_next_file(LogPipe *s, gboolean *last_item);

void affile_sd_queue(LogPipe *s, LogMessage *msg, const LogPathOptions *path_options, gpointer user_data);
void affile_sd_add_file_to_the_queue(AFFileSourceDriver *self, const gchar *filename);
void affile_sd_recover_state(LogPipe *s, GlobalConfig *cfg, LogProto *proto);
void affile_sd_reset_file_state(PersistState *state, const gchar *old_state_name);

void affile_sd_notify(LogPipe *s, LogPipe *sender, gint notify_code, gpointer user_data);
void affile_sd_monitor_pushback_filename(AFFileSourceDriver *self, const gchar *filename);
gboolean affile_sd_skip_old_messages(LogSrcDriver *s, GlobalConfig *cfg);

LogProto* affile_sd_construct_proto(AFFileSourceDriver *self, LogTransport *transport);

gboolean affile_sd_open(LogPipe *s, gboolean immediate_check);
gboolean affile_sd_init(LogPipe *s);
gboolean affile_sd_deinit(LogPipe *s);
void affile_sd_free(LogPipe *s);

void affile_dw_arm_reaper(AFFileDestWriter *self);

gchar *affile_sd_format_persist_name(const gchar *filename);
#endif /* AFFILE_COMMON_H_INCLUDED */
