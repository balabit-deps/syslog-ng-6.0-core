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

#include "logsource.h"
#include "messages.h"
#include "misc.h"
#include "timeutils.h"
#include "stats.h"
#include "tags.h"
#include "compat.h"
#include "cfg.h"
#include "ack_tracker.h"

#include <string.h>


gboolean accurate_nanosleep = FALSE;

void
log_source_wakeup(LogSource *self)
{
  g_atomic_counter_set(&self->suspended, 0);
  if (self->wakeup)
    self->wakeup(self);

  msg_debug("Source has been resumed", evt_tag_str("group_name", self->options->group_name), NULL);
}

static inline void
_flow_control_window_size_adjust(LogSource *self, guint32 window_size_increment)
{
  guint32 old_window_size;

  old_window_size = g_atomic_counter_exchange_and_add(&self->window_size, window_size_increment);

  if (old_window_size == 0 || log_source_is_suspended(self))
    log_source_wakeup(self);
}

static void
_flow_control_rate_adjust(LogSource *self)
{
  /* NOTE: this is racy. msg_ack may be executing in different writer
   * threads. I don't want to lock, all we need is an approximate value of
   * the ACK rate of the last couple of seconds.  */

  guint32 cur_ack_count, last_ack_count;

  if (accurate_nanosleep && self->threaded)
    {
      cur_ack_count = ++self->ack_count;
      if ((cur_ack_count & 0x3FFF) == 0)
        {
          struct timespec now;
          glong diff;

          /* do this every once in a while, once in 16k messages should be fine */

          last_ack_count = self->last_ack_count;

          /* make sure that we have at least 16k messages to measure the rate
           * for.  Because of the race we may have last_ack_count ==
           * cur_ack_count if another thread already measured the same span */

          if (last_ack_count < cur_ack_count - 16383)
            {
              clock_gettime(CLOCK_MONOTONIC, &now);
              if (now.tv_sec > self->last_ack_rate_time.tv_sec + 6)
                {
                  /* last check was too far apart, this means the rate is quite slow. turn off sleeping. */
                  self->window_full_sleep_nsec = 0;
                  self->last_ack_rate_time = now;
                }
              else
                {
                  /* ok, we seem to have a close enough measurement, this means
                   * we do have a high rate.  Calculate how much we should sleep
                   * in case the window gets full */

                  diff = timespec_diff_nsec(&now, &self->last_ack_rate_time);
                  self->window_full_sleep_nsec = (diff / (cur_ack_count - last_ack_count));
                  if (self->window_full_sleep_nsec > 1e6)
                    {
                      /* in case we'd be waiting for 1msec for another free slot in the window, let's go to background instead */
                      self->window_full_sleep_nsec = 0;
                    }
                  else
                    {
                      /* otherwise let's wait for about 8 message to be emptied before going back to the loop, but clamp the maximum time to 0.1msec */
                      self->window_full_sleep_nsec <<= 3;
                      if (self->window_full_sleep_nsec > 1e5)
                        self->window_full_sleep_nsec = 1e5;
                    }
                  self->last_ack_count = cur_ack_count;
                  self->last_ack_rate_time = now;
                }
            }
        }
    }
}

/**
 * log_source_msg_ack:
 *
 * This is running in the same thread as the _destination_, thus care must
 * be taken when manipulating the LogSource data structure.
 **/
static void
log_source_msg_ack(LogMessage *msg, AckType ack_type)
{
  AckTracker *ack_tracker = msg->ack_record->tracker;
  ack_tracker_manage_msg_ack(ack_tracker, msg, ack_type);
}

void
log_source_flow_control_adjust(LogSource *self, guint32 window_size_increment)
{
  _flow_control_window_size_adjust(self, window_size_increment);
  _flow_control_rate_adjust(self);

  if (self->is_external_ack_required)
    log_source_wakeup(self);
}

void
log_source_flow_control_suspend(LogSource *self)
{
  msg_debug("Source has been suspended", evt_tag_str("group_name", self->options->group_name), NULL);

  _flow_control_window_size_adjust(self, 1);
  log_source_suspend(self);
  _flow_control_rate_adjust(self);
}

void
log_source_mangle_hostname(LogSource *self, LogMessage *msg)
{
  gchar resolved_name[256];
  gsize resolved_name_len = sizeof(resolved_name);
  const gchar *orig_host;

  resolve_sockaddr(resolved_name, &resolved_name_len, msg->saddr, self->options->use_dns, self->options->use_fqdn, self->options->use_dns_cache, self->options->normalize_hostnames);

  log_msg_set_value(msg, LM_V_HOST_FROM, resolved_name, resolved_name_len);

  orig_host = log_msg_get_value(msg, LM_V_HOST, NULL);
  if (!self->options->keep_hostname || !orig_host || !orig_host[0])
    {
      gchar host[256];
      gint host_len = -1;
      if (G_UNLIKELY(self->options->chain_hostnames))
        {
          msg->flags |= LF_CHAINED_HOSTNAME;
          if (msg->flags & LF_LOCAL)
            {
              /* local */
              host_len = g_snprintf(host, sizeof(host), "%s@%s", self->options->group_name, resolved_name);
            }
          else if (!orig_host || !orig_host[0])
            {
              /* remote && no hostname */
              host_len = g_snprintf(host, sizeof(host), "%s/%s", resolved_name, resolved_name);
            }
          else
            {
              /* everything else, append source hostname */
              if (orig_host && orig_host[0])
                host_len = g_snprintf(host, sizeof(host), "%s/%s", orig_host, resolved_name);
              else
                {
                  strncpy(host, resolved_name, sizeof(host));
                  /* just in case it is not zero terminated */
                  host[255] = 0;
                }
            }
          if (host_len >= sizeof(host))
            host_len = sizeof(host) - 1;
          log_msg_set_value(msg, LM_V_HOST, host, host_len);
        }
      else
        {
          log_msg_set_value(msg, LM_V_HOST, resolved_name, resolved_name_len);
        }
    }
}

gboolean
log_source_init(LogPipe *s)
{
  LogSource *self = (LogSource *) s;

  stats_lock();
  stats_register_counter(self->stats_level, self->stats_source | SCS_SOURCE, self->stats_id, self->stats_instance, SC_TYPE_PROCESSED, &self->recvd_messages);
  stats_register_counter(self->stats_level, self->stats_source | SCS_SOURCE, self->stats_id, self->stats_instance, SC_TYPE_STAMP, &self->last_message_seen);
  stats_unlock();

  return TRUE;
}

gboolean
log_source_deinit(LogPipe *s)
{
  LogSource *self = (LogSource *) s;

  stats_lock();
  stats_unregister_counter(self->stats_source | SCS_SOURCE, self->stats_id, self->stats_instance, SC_TYPE_PROCESSED, &self->recvd_messages);
  stats_unregister_counter(self->stats_source | SCS_SOURCE, self->stats_id, self->stats_instance, SC_TYPE_STAMP, &self->last_message_seen);
  stats_unlock();

  return TRUE;
}

static inline void
_increment_dynamic_stats_counters(const gchar *source_name, LogMessage *msg)
{
  StatsCounterItem *processed_counter, *stamp;
  StatsCounter *handle;
  gboolean new;

  if (stats_check_level(2))
    {
      stats_lock();

      handle = stats_register_dynamic_counter(2, SCS_HOST | SCS_SOURCE, NULL, log_msg_get_value(msg, LM_V_HOST, NULL), SC_TYPE_PROCESSED, &processed_counter, &new);
      stats_register_associated_counter(handle, SC_TYPE_STAMP, &stamp);
      stats_counter_inc(processed_counter);
      stats_counter_set(stamp, msg->timestamps[LM_TS_RECVD].tv_sec);
      stats_unregister_dynamic_counter(handle, SC_TYPE_PROCESSED, &processed_counter);
      stats_unregister_dynamic_counter(handle, SC_TYPE_STAMP, &stamp);

      if (stats_check_level(3))
        {
          stats_instant_inc_dynamic_counter(3, SCS_SENDER | SCS_SOURCE, NULL, log_msg_get_value(msg, LM_V_HOST_FROM, NULL), msg->timestamps[LM_TS_RECVD].tv_sec);
          stats_instant_inc_dynamic_counter(3, SCS_PROGRAM | SCS_SOURCE, NULL, log_msg_get_value(msg, LM_V_PROGRAM, NULL), -1);

          stats_instant_inc_dynamic_counter(3, SCS_HOST | SCS_SOURCE, source_name, log_msg_get_value(msg, LM_V_HOST, NULL), msg->timestamps[LM_TS_RECVD].tv_sec);
          stats_instant_inc_dynamic_counter(3, SCS_SENDER | SCS_SOURCE, source_name, log_msg_get_value(msg, LM_V_HOST_FROM, NULL), msg->timestamps[LM_TS_RECVD].tv_sec);
        }

      stats_unlock();
    }
}

static void
log_source_override_host(LogSource *self, LogMessage *msg)
{
  if (self->options->host_override_len < 0)
    self->options->host_override_len = strlen(self->options->host_override);
  log_msg_set_value(msg, LM_V_HOST, self->options->host_override, self->options->host_override_len);
}

static void
log_source_override_program(LogSource *self, LogMessage *msg)
{
  if (self->options->program_override_len < 0)
    self->options->program_override_len = strlen(self->options->program_override);
  log_msg_set_value(msg, LM_V_PROGRAM, self->options->program_override, self->options->program_override_len);
}


static gboolean
_invoke_mangle_callbacks(LogPipe *s, LogMessage *msg, const LogPathOptions *path_options)
{
  LogSource *self = (LogSource *) s;
  GList *next_item = NULL;

  next_item = g_list_first(self->options->source_queue_callbacks);
  while(next_item)
  {
    if(next_item->data)
      {
        if(!((mangle_callback) (next_item->data))(log_pipe_get_config(s),msg,self))
          {
            log_msg_drop(msg, path_options, AT_PROCESSED);
            return FALSE;
          }
      }
    next_item = next_item->next;
  }
  return TRUE;
}

static void
log_source_queue(LogPipe *s, LogMessage *msg, const LogPathOptions *path_options, gpointer user_data)
{
  LogSource *self = (LogSource *) s;
  LogPathOptions local_options = *path_options;
  gint old_window_size;
  gint i;

  msg_set_context(msg);

  if (msg->timestamps[LM_TS_STAMP].tv_sec == -1 || !self->options->keep_timestamp)
    msg->timestamps[LM_TS_STAMP] = msg->timestamps[LM_TS_RECVD];

  g_assert(msg->timestamps[LM_TS_STAMP].zone_offset != -1);

  ack_tracker_track_msg(self->ack_tracker, msg);

  /* NOTE: we start by enabling flow-control, thus we need an acknowledgement */
  local_options.ack_needed = TRUE;
  log_msg_ref(msg); // TODO: move to register_msg?
  log_msg_add_ack(msg, &local_options);
  msg->ack_func = log_source_msg_ack;

  old_window_size = g_atomic_counter_exchange_and_add(&self->window_size, -1);

  if (G_UNLIKELY(old_window_size == 1))
    msg_debug("Source has been suspended", evt_tag_str("group_name", self->options->group_name), NULL);

  /*
   * NOTE: this assertion validates that the source is not overflowing its
   * own flow-control window size, decreased above, by the atomic statement.
   *
   * If the _old_ value is zero, that means that the decrement operation
   * above has decreased the value to -1.
   */
  g_assert(old_window_size > 0);

  /* $RCPTID create*/
  log_msg_create_rcptid(msg);

  /* $HOST setup */
  log_source_mangle_hostname(self, msg);


  if (self->options->use_syslogng_pid)
    {
      log_msg_set_value(msg, LM_V_PID, get_pid_string(), -1);
    }
  /* source specific tags */
  if (self->options->tags)
    {
      for (i = 0; i < self->options->tags->len; i++)
        {
          log_msg_set_tag_by_id(msg, g_array_index(self->options->tags, LogTagId, i));
        }
    }

  log_msg_set_tag_by_id(msg, self->options->source_group_tag);

  if (!_invoke_mangle_callbacks(&self->super, msg, &local_options))
    return;

  if (self->options->program_override)
    log_source_override_program(self, msg);

  if (self->options->host_override)
    log_source_override_host(self, msg);

  /* stats counters */
  _increment_dynamic_stats_counters(self->options->group_name, msg);

  stats_counter_inc_pri(msg->pri);
  stats_counter_inc(self->recvd_messages);
  stats_counter_set(self->last_message_seen, msg->timestamps[LM_TS_RECVD].tv_sec);

  /* message setup finished, send it out */

  log_pipe_forward_msg(s, msg, &local_options);

  msg_set_context(NULL);

  if (accurate_nanosleep && self->threaded && self->window_full_sleep_nsec > 0 && !log_source_free_to_send(self))
    {
      struct timespec ts;

      /* wait one 0.1msec in the hope that the buffer clears up */
      ts.tv_sec = 0;
      ts.tv_nsec = self->window_full_sleep_nsec;
      nanosleep(&ts, NULL);
    }
}

static inline void
_create_ack_tracker_if_not_exists(LogSource *self, gboolean pos_tracked)
{
  if (!self->ack_tracker)
    {
      if (pos_tracked)
        self->ack_tracker = late_ack_tracker_new(self);
      else
        self->ack_tracker = early_ack_tracker_new(self);
    }
}

void
log_source_set_options(LogSource *self, LogSourceOptions *options, gint stats_level, gint stats_source, const gchar *stats_id, const gchar *stats_instance, gboolean threaded, gboolean pos_tracked)
{
  /* NOTE: we don't adjust window_size even in case it was changed in the
   * configuration and we received a SIGHUP.  This means that opened
   * connections will not have their window_size changed. */

  if (g_atomic_counter_get(&self->window_size) == -1)
    g_atomic_counter_set(&self->window_size, options->init_window_size);
  self->options = options;
  self->stats_level = stats_level;
  self->stats_source = stats_source;
  if (self->stats_id)
    g_free(self->stats_id);
  self->stats_id = stats_id ? g_strdup(stats_id) : NULL;
  if (self->stats_instance)
    g_free(self->stats_instance);
  self->stats_instance = stats_instance ? g_strdup(stats_instance): NULL;
  self->threaded = threaded;
  self->pos_tracked = pos_tracked;
  _create_ack_tracker_if_not_exists(self, pos_tracked);
}

void
log_source_init_instance(LogSource *self)
{
  log_pipe_init_instance(&self->super);
  self->super.queue = log_source_queue;
  self->super.free_fn = log_source_free;
  self->super.init = log_source_init;
  self->super.deinit = log_source_deinit;
  g_atomic_counter_set(&self->window_size, -1);
  self->ack_tracker = NULL;
}

void
log_source_free(LogPipe *s)
{
  LogSource *self = (LogSource *) s;

  ack_tracker_free(self->ack_tracker);
  self->ack_tracker = NULL;

  g_free(self->stats_id);
  g_free(self->stats_instance);
  log_pipe_free_method(s);
}

void
log_source_options_defaults(LogSourceOptions *options)
{
  options->init_window_size = 100;
  options->keep_hostname = -1;
  options->chain_hostnames = -1;
  options->use_dns = -1;
  options->use_fqdn = -1;
  options->use_dns_cache = -1;
  options->normalize_hostnames = -1;
  options->keep_timestamp = -1;
  options->tags = NULL;
  options->read_old_records = TRUE;
}

/*
 * NOTE: options_init and options_destroy are a bit weird, because their
 * invocation is not completely symmetric:
 *
 *   - init is called from driver init (e.g. affile_sd_init),
 *   - destroy is called from driver free method (e.g. affile_sd_free, NOT affile_sd_deinit)
 *
 * The reason:
 *   - when initializing the reloaded configuration fails for some reason,
 *     we have to fall back to the old configuration, thus we cannot dump
 *     the information stored in the Options structure.
 *
 * For the reasons above, init and destroy behave the following way:
 *
 *   - init is idempotent, it can be called multiple times without leaking
 *     memory, and without loss of information
 *   - destroy is only called once, when the options are indeed to be destroyed
 *
 * As init allocates memory, it has to take care about freeing memory
 * allocated by the previous init call (or it has to reuse those).
 *
 */
void
log_source_options_init(LogSourceOptions *options, GlobalConfig *cfg, const gchar *group_name)
{
  gchar *host_override, *program_override;
  gchar *source_group_name;
  gboolean read_old_records;
  GArray *tags;

  host_override = options->host_override;
  options->host_override = NULL;
  program_override = options->program_override;
  options->program_override = NULL;

  tags = options->tags;
  options->tags = NULL;

  read_old_records = options->read_old_records;

  /***********************************************************************
   * PLEASE NOTE THIS. please read the comment at the top of the function
   ***********************************************************************/
  log_source_options_destroy(options);

  options->tags = tags;

  options->host_override = host_override;
  options->host_override_len = -1;
  options->program_override = program_override;
  options->program_override_len = -1;
  options->read_old_records = read_old_records;


  options->source_queue_callbacks = cfg->source_mangle_callback_list;

  if (options->keep_hostname == -1)
    options->keep_hostname = cfg->keep_hostname;
  if (options->chain_hostnames == -1)
    options->chain_hostnames = cfg->chain_hostnames;
  if (options->use_dns == -1)
    options->use_dns = cfg->use_dns;
  if (options->use_fqdn == -1)
    options->use_fqdn = cfg->use_fqdn;
  if (options->use_dns_cache == -1)
    options->use_dns_cache = cfg->use_dns_cache;
  if (options->normalize_hostnames == -1)
    options->normalize_hostnames = cfg->normalize_hostnames;
  if (options->keep_timestamp == -1)
    options->keep_timestamp = cfg->keep_timestamp;
  options->group_name = group_name;

  source_group_name = g_strdup_printf(".source.%s", group_name);
  options->source_group_tag = log_tags_get_by_name(source_group_name);
  g_free(source_group_name);

  if (options->use_dns == 0) {
    if (options->use_dns_cache != 0) {
      msg_warning("With use-dns(no), dns-cache() will be forced to 'no' too!",
                  evt_tag_str("source-group", group_name),
                  NULL);
    }
    options->use_dns_cache = 0;
  }
}

void
log_source_options_destroy(LogSourceOptions *options)
{
  if (options->program_override)
    g_free(options->program_override);
  if (options->host_override)
    g_free(options->host_override);
  if (options->tags)
    {
      g_array_free(options->tags, TRUE);
      options->tags = NULL;
    }
}

void
log_source_options_set_tags(LogSourceOptions *options, GList *tags)
{
  LogTagId id;

  if (!options->tags)
    options->tags = g_array_new(FALSE, FALSE, sizeof(LogTagId));

  while (tags)
    {
      id = log_tags_get_by_name((gchar *) tags->data);
      g_array_append_val(options->tags, id);

      g_free(tags->data);
      tags = g_list_delete_link(tags, tags);
    }
}

void
log_source_global_init(void)
{
  accurate_nanosleep = check_nanosleep();
  if (!accurate_nanosleep)
    {
      msg_debug("nanosleep() is not accurate enough to introduce minor stalls on the reader side, multi-threaded performance may be affected", NULL);
    }
}
