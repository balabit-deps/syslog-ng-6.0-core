/*
 * Copyright (c) 2002-2010 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2010 Balázs Scheidler
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
  
#include "logwriter.h"
#include "messages.h"
#include "stats.h"
#include "misc.h"
#include "hostname.h"
#include "mainloop.h"
#include "mainloop-call.h"
#include "mainloop-io-worker.h"
#include "str-format.h"

#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iv.h>
#include <iv_event.h>
#include <iv_work.h>

typedef enum
{
  /* flush modes */

  /* business as usual, flush when the buffer is full */
  LW_FLUSH_NORMAL,
  /* flush the buffer immediately please */
  LW_FLUSH_FORCE,
} LogWriterFlushMode;

struct _LogWriter
{
  LogPipe super;
  LogQueue *queue;
  guint32 flags:31;
  gint32 seq_num;
  StatsCounterItem *dropped_messages;
  StatsCounterItem *suppressed_messages;
  StatsCounterItem *processed_messages;
  StatsCounterItem *stored_messages;
  LogPipe *control;
  LogWriterOptions *options;
  GStaticMutex suppress_lock;
  LogMessage *last_msg;
  guint32 last_msg_count;
  GString *line_buffer;

  gint stats_level;
  guint16 stats_source;
  gchar *stats_id;
  gchar *stats_instance;

  struct iv_fd fd_watch;
  struct iv_timer suspend_timer;
  struct iv_task immed_io_task;
  struct iv_event queue_filled;
  MainLoopIOWorkerJob io_job;
  struct iv_timer suppress_timer;
  struct iv_timer mark_timer;
  struct iv_timer reopen_timer;
  struct timespec suppress_timer_expires;
  struct iv_timer idle_timer;
  gint mark_freq;
  gboolean suppress_timer_updated;
  gboolean work_result;
  gint pollable_state;
  LogProto *proto, *pending_proto;
  gboolean watches_running:1, suspended:1, working:1, flush_waiting_for_timeout:1;
  gboolean pending_proto_present;
  GCond *pending_proto_cond;
  GStaticMutex pending_proto_lock;
  /* messages posted, and we're not certain were properly sent out on the wire */
  gint last_notify_code;

  gint io_cond;

  gboolean has_to_poll;
  gboolean force_read;
};

/**
 * LogWriter behaviour
 * ~~~~~~~~~~~~~~~~~~~
 *
 * LogWriter is a core element of syslog-ng sending messages out to some
 * kind of destination represented by a UNIX fd. Outgoing messages are sent
 * to the target asynchronously, first by placing them to a queue and then
 * sending messages when poll() indicates that the fd is writable.
 *
 * 
 * Flow control
 * ------------
 * For a simple log writer without a disk buffer messages are placed on a
 * GQueue and they are acknowledged when the send() system call returned
 * success. This is more complex when disk buffering is used, in which case
 * messages are put to the "disk buffer" first and acknowledged immediately. 
 * (this way the reader never stops when the disk buffer area is not yet
 * full). When disk buffer reaches its limit, messages are added to the the
 * usual GQueue and messages get acknowledged when they are moved to the
 * disk buffer.
 *
 **/

static gboolean log_writer_flush(LogWriter *self, LogWriterFlushMode flush_mode);
static void log_writer_broken(LogWriter *self, gint notify_code);
static void log_writer_start_watches(LogWriter *self);
static void log_writer_stop_watches(LogWriter *self);
static void log_writer_update_watches(LogWriter *self);
static void log_writer_suspend(LogWriter *self);
static void log_writer_free_proto(LogWriter *self);
static void log_writer_set_proto(LogWriter *self, LogProto *proto);
static void log_writer_set_pending_proto(LogWriter *self, LogProto *proto, gboolean present);

static void
log_writer_msg_ack(guint num_msg_acked, gpointer user_data)
{
  LogWriter *self = (LogWriter *)user_data;
  log_queue_ack_backlog(self->queue, num_msg_acked);
}

static void
log_writer_msg_rewind(gpointer user_data)
{
  LogWriter *self = (LogWriter *)user_data;
  log_queue_rewind_backlog_all(self->queue);
}

static void
log_writer_msg_rewind_on_error(guint num_msg_rewind, gpointer user_data)
{
  LogWriter *self = (LogWriter *)user_data;
  log_queue_rewind_backlog(self->queue, num_msg_rewind);
}

static void
log_writer_free_proto(LogWriter *self)
{
  if (self->proto)
    {
      log_proto_free(self->proto);
      log_writer_set_proto(self, NULL);
    }
}


static void
log_writer_set_proto(LogWriter *self, LogProto *proto)
{
  self->proto = proto;

  if (proto)
    {
      LogProtoFlowControlFuncs flow_control_funcs;
      flow_control_funcs.ack_callback = log_writer_msg_ack;
      flow_control_funcs.rewind_callback = log_writer_msg_rewind;
      flow_control_funcs.rewind_on_error_callback = log_writer_msg_rewind_on_error;
      flow_control_funcs.user_data = self;

      log_proto_set_flow_control_funcs(self->proto, &flow_control_funcs);
    }
}

static void
log_writer_set_pending_proto(LogWriter *self, LogProto *proto, gboolean present)
{
  self->pending_proto = proto;
  self->pending_proto_present = present;
}

static void
log_writer_work_perform(gpointer s)
{
  LogWriter *self = (LogWriter *) s;

  g_assert((self->super.flags & PIF_INITIALIZED) != 0);
  self->work_result = log_writer_flush(self, LW_FLUSH_NORMAL);
}

static void
log_writer_work_finished(gpointer s)
{
  LogWriter *self = (LogWriter *) s;

  main_loop_assert_main_thread();
  self->flush_waiting_for_timeout = FALSE;

  if (self->pending_proto_present)
    {
      /* pending proto is only set in the main thread, so no need to
       * lock it before coming here. After we're syncing with the
       * log_writer_reopen() call, quite possibly coming from a
       * non-main thread. */

      g_static_mutex_lock(&self->pending_proto_lock);

      log_writer_free_proto(self);
      log_writer_set_proto(self, self->pending_proto);
      log_writer_set_pending_proto(self, NULL, FALSE);

      g_cond_signal(self->pending_proto_cond);
      g_static_mutex_unlock(&self->pending_proto_lock);
    }

  if (!self->work_result)
    {
      log_writer_broken(self, NC_WRITE_ERROR);
      if (self->proto)
        {
          log_writer_suspend(self);
          msg_notice("Suspending write operation because of an I/O error",
                     evt_tag_int("fd", log_proto_get_fd(self->proto)),
                     evt_tag_int("time_reopen", self->options->time_reopen),
                     evt_tag_id(MSG_WRITE_SUSPENDING_IO_ERROR),
                     NULL);
        }
      goto exit;
    }

  if ((self->super.flags & PIF_INITIALIZED) && self->proto)
    {
      /* reenable polling the source, but only if we're still initialized */
      log_writer_start_watches(self);
    }

exit:
  log_pipe_unref(&self->super);
}

static void
log_writer_io_flush_output(gpointer s)
{
  LogWriter *self = (LogWriter *) s;

  main_loop_assert_main_thread();

  log_writer_stop_watches(self);
  if (!main_loop_worker_job_quit())
    {
      log_pipe_ref(&self->super);
      if ((self->options->options & LWO_THREADED))
        {
          main_loop_io_worker_job_submit(&self->io_job);
        }
      else
        {
          /* Checking main_loop_io_worker_job_quit() helps to speed up the
           * reload process.  If reload/shutdown is requested we shouldn't do
           * anything here, a final flush will be attempted in
           * log_writer_deinit().
           *
           * Our current understanding is that it doesn't prevent race
           * conditions of any kind.
           */
          log_writer_work_perform(s);
          log_writer_work_finished(s);
        }
    }
}

static void
log_writer_io_error(gpointer s)
{
  LogWriter *self = (LogWriter *) s;

  if (self->fd_watch.handler_out == NULL && self->fd_watch.handler_in == NULL)
    {
      msg_debug("POLLERR occurred while idle",
                evt_tag_int("fd", log_proto_get_fd(self->proto)),
                NULL);
      log_writer_broken(self, NC_WRITE_ERROR);
      return;
    }
  else
    {
      /* in case we have an error state but we also asked for read/write
       * polling, the error should be handled by the I/O callback.  But we
       * need not call that explicitly as ivykis does that for us.  */
      if (self->fd_watch.handler_in)
        {
          msg_debug("POLLERR occurred while read fd",
                evt_tag_int("fd", log_proto_get_fd(self->proto)),
                NULL);
          log_writer_broken(self, NC_WRITE_ERROR);
          return;
        }
    }
  log_writer_update_watches(self);
}

static void
log_writer_io_check_eof(gpointer s)
{
  LogWriter *self = (LogWriter *) s;

  msg_error("EOF occurred while idle",
            evt_tag_int("fd", log_proto_get_fd(self->proto)),
            evt_tag_id(MSG_LOGWRITER_EOF_OCCURED),
            NULL);
  log_writer_broken(self, NC_CLOSE);
}

static void
log_writer_error_suspend_elapsed(gpointer s)
{
  LogWriter *self = (LogWriter *) s;

  self->suspended = FALSE;
  msg_notice("Error suspend timeout has elapsed, attempting to write again",
             evt_tag_int("fd", log_proto_get_fd(self->proto)),
             evt_tag_id(MSG_WRITE_SUSPEND_TIMEOUT_ELAPSED),
             NULL);
  log_writer_start_watches(self);
}

static void
log_writer_update_fd_callbacks(LogWriter *self, GIOCondition cond)
{
  main_loop_assert_main_thread();
  if (self->pollable_state > 0)
    {
      if (!iv_fd_registered(&self->fd_watch))
      {
        iv_fd_register(&self->fd_watch);
      }
      if (self->flags & LW_DETECT_EOF && (cond & G_IO_IN) == 0)
        {
          /* if output is enabled, and we're in DETECT_EOF mode, and input is
           * not needed by the log protocol, install the eof check callback to
           * destroy the connection if an EOF is received. */

          iv_fd_set_handler_in(&self->fd_watch, log_writer_io_check_eof);
        }
      else if (cond & G_IO_IN)
        {
          /* in case the protocol requested G_IO_IN, it means that it needs to
           * invoke read in the flush code, so just install the flush_output
           * handler for input */

          iv_fd_set_handler_in(&self->fd_watch, log_writer_io_flush_output);
        }
      else
        {
          /* otherwise we're not interested in input */
          iv_fd_set_handler_in(&self->fd_watch, NULL);
        }
      if (cond & G_IO_OUT)
      {
        if (self->has_to_poll)
          {
            iv_fd_set_handler_out(&self->fd_watch, log_writer_io_flush_output);
            self->has_to_poll = FALSE;
          }
        else
          {
            if (!iv_task_registered(&self->immed_io_task))
              iv_task_register(&self->immed_io_task);
          }
      }
      else
      {
        iv_fd_set_handler_out(&self->fd_watch, NULL);
        self->has_to_poll = FALSE;
      }
      if (iv_fd_registered(&self->fd_watch))
        iv_fd_set_handler_err(&self->fd_watch, log_writer_io_error);
    }
  else
    {
      /* fd is not pollable, assume it is always writable */
      if (cond & G_IO_OUT)
        {
          if (!iv_task_registered(&self->immed_io_task))
            iv_task_register(&self->immed_io_task);
        }
      else if (iv_task_registered(&self->immed_io_task))
        {
          iv_task_unregister(&self->immed_io_task);
        }
    }
}

void
log_writer_arm_suspend_timer(LogWriter *self, void (*handler)(void *), gint timeout_msec)
{
  if (iv_timer_registered(&self->suspend_timer))
    iv_timer_unregister(&self->suspend_timer);
  iv_validate_now();
  self->suspend_timer.handler = handler;
  self->suspend_timer.expires = iv_now;
  timespec_add_msec(&self->suspend_timer.expires, timeout_msec);
  iv_timer_register(&self->suspend_timer);
}

static void
log_writer_queue_filled(gpointer s)
{
  LogWriter *self = (LogWriter *) s;

  main_loop_assert_main_thread();

  /*
   * NOTE: This theory is somewhat questionable, e.g. I'm not 100% sure it
   * is the right scenario, but the race was closed.  So take this with a
   * grain of salt.
   *
   * The queue_filled callback is running in the main thread. Because of the
   * possible delay caused by iv_event_post() the callback might be
   * delivered event after stop_watches() has been called.
   *
   *   - log_writer_schedule_update_watches() is called by the reader
   *     thread, which calls iv_event_post()
   *   - the main thread calls stop_watches() in work_perform
   *   - the event is delivered in the main thread
   *
   * But since stop/start watches always run in the main thread and we do
   * too, we can check if this is the case.  A LogWriter without watches
   * running is busy writing out data to the destination, e.g.  a
   * start_watches is to be expected once log_writer_work_finished() is run
   * at the end of the deferred work, executed by the I/O threads.
   */
  if (self->watches_running)
    log_writer_update_watches((LogWriter *) s);
}

/* NOTE: runs in the source thread */
static void
log_writer_schedule_update_watches(LogWriter *self)
{
  iv_event_post(&self->queue_filled);
}

static void
log_writer_suspend(LogWriter *self)
{
  /* flush code indicates that we need to suspend our writing activities
   * until time_reopen elapses */

  log_writer_arm_suspend_timer(self, log_writer_error_suspend_elapsed, self->options->time_reopen * 1e3);
  self->suspended = TRUE;
}

static void
log_writer_update_watches(LogWriter *self)
{
  gint fd;
  GIOCondition cond = 0;
  gint timeout_msec = 0;
  gboolean prepare_result;
  gint idle_timeout = -1;

  main_loop_assert_main_thread();

  if (self->force_read)
    {
      self->force_read = FALSE;
      if (!iv_task_registered(&self->immed_io_task))
        iv_task_register(&self->immed_io_task);
    }

  if (iv_timer_registered(&self->idle_timer))
    {
      iv_timer_unregister(&self->idle_timer);
    }

  /* NOTE: we either start the suspend_timer or enable the fd_watch. The two MUST not happen at the same time. */
  prepare_result = log_proto_prepare(self->proto, &fd, &cond, &idle_timeout);
  self->io_cond = cond;
  if (prepare_result ||
      self->flush_waiting_for_timeout ||
      log_queue_check_items(self->queue, &timeout_msec,
                            (LogQueuePushNotifyFunc) log_writer_schedule_update_watches, self, NULL)
      )
    {
      /* flush_lines number of element is already available and throttle would permit us to send. */
      log_writer_update_fd_callbacks(self, cond);
    }
  else if (timeout_msec)
    {
      /* few elements are available, but less than flush_lines, we need to start a timer to initiate a flush */

      log_writer_update_fd_callbacks(self, 0);
      self->flush_waiting_for_timeout = TRUE;
      log_writer_arm_suspend_timer(self, (void (*)(void *)) log_writer_update_watches, timeout_msec);
    }
  else
    {
      /* no elements or no throttle space, wait for a wakeup by the queue
       * when the required number of items are added.  see the
       * log_queue_check_items and its parallel_push argument above
       */
      log_writer_update_fd_callbacks(self, 0);
    }
  if (idle_timeout > 0)
    {
      iv_validate_now();
      self->idle_timer.expires.tv_sec = iv_now.tv_sec + idle_timeout;
      iv_timer_register(&self->idle_timer);
    }
}

static void
log_writer_start_watches(LogWriter *self)
{
  gint fd;
  GIOCondition cond;
  gint idle_timeout = -1;

  if (self->watches_running)
    return;

  log_proto_prepare(self->proto, &fd, &cond, &idle_timeout);

  self->fd_watch.fd = fd;
  if (self->pollable_state < 0)
    {
      if (is_file_regular(fd))
        self->pollable_state = 0;
      else
        self->pollable_state = !iv_fd_register_try(&self->fd_watch);
    }
  else if (self->pollable_state)
    {
      iv_fd_register(&self->fd_watch);
    }

  log_writer_update_watches(self);
  self->watches_running = TRUE;
}

static void
log_writer_stop_watches(LogWriter *self)
{
  if (self->watches_running)
    {
      if (iv_timer_registered(&self->suspend_timer))
        iv_timer_unregister(&self->suspend_timer);
      if (iv_fd_registered(&self->fd_watch))
        iv_fd_unregister(&self->fd_watch);
      if (iv_task_registered(&self->immed_io_task))
        iv_task_unregister(&self->immed_io_task);

      log_queue_reset_parallel_push(self->queue);

      self->watches_running = FALSE;
    }
  else if (self->suspended)
    {
      if (iv_timer_registered(&self->suspend_timer))
        iv_timer_unregister(&self->suspend_timer);
      self->suspended = FALSE;
    }
}

/* function called using main_loop_call() in case the suppress timer needs
 * to be updated */
static void
log_writer_perform_suppress_timer_update(LogWriter *self)
{
  main_loop_assert_main_thread();

  if (iv_timer_registered(&self->suppress_timer))
    iv_timer_unregister(&self->suppress_timer);
  g_static_mutex_lock(&self->suppress_lock);
  self->suppress_timer.expires = self->suppress_timer_expires;
  self->suppress_timer_updated = TRUE;
  g_static_mutex_unlock(&self->suppress_lock);
  if (self->suppress_timer.expires.tv_sec > 0)
    iv_timer_register(&self->suppress_timer);
  log_pipe_unref(&self->super);
}

/*
 * Update the suppress timer in a deferred manner, possibly batching the
 * results of multiple updates to the suppress timer.  This is necessary as
 * suppress timer updates must run in the main thread, and updating it every
 * time a new message comes in would cause enormous latency in the fast
 * path. By collecting multiple updates
 *
 * msec == 0 means to turn off the suppress timer
 * msec >  0 to enable the timer with the specified timeout
 *
 * NOTE: suppress_lock must be held.
 */
static void
log_writer_update_suppress_timer(LogWriter *self, glong sec)
{
  gboolean invoke;
  struct timespec next_expires;

  iv_validate_now();

  /* we deliberately use nsec == 0 in order to increase the likelyhood that
   * we target the same second, in case only a fraction of a second has
   * passed between two updates.  */
  if (sec)
    {
      next_expires.tv_nsec = 0;
      next_expires.tv_sec = iv_now.tv_sec + sec;
    }
  else
    {
      next_expires.tv_sec = 0;
      next_expires.tv_nsec = 0;
    }
  /* last update was finished, we need to invoke the updater again */
  invoke = ((next_expires.tv_sec != self->suppress_timer_expires.tv_sec) || (next_expires.tv_nsec != self->suppress_timer_expires.tv_nsec)) && self->suppress_timer_updated;
  self->suppress_timer_updated = FALSE;

  if (invoke)
    {
      self->suppress_timer_expires = next_expires;
      g_static_mutex_unlock(&self->suppress_lock);
      log_pipe_ref(&self->super);
      main_loop_call((void *(*)(void *)) log_writer_perform_suppress_timer_update, self, FALSE);
      g_static_mutex_lock(&self->suppress_lock);
    }

}

/*
 * NOTE: suppress_lock must be held.
 */
static void
log_writer_last_msg_release(LogWriter *self)
{
  log_writer_update_suppress_timer(self, 0);
  if (self->last_msg)
    log_msg_unref(self->last_msg);

  self->last_msg = NULL;
  self->last_msg_count = 0;
}

/*
 * NOTE: suppress_lock must be held.
 */
static void
log_writer_last_msg_flush(LogWriter *self)
{
  LogMessage *m;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  gchar hostname[256];
  gchar buf[1024];
  gssize len;
  const gchar *p;

  msg_debug("Suppress timer elapsed, emitting suppression summary", 
            NULL);

  getlonghostname(hostname, sizeof(hostname));
  m = log_msg_new_empty();
  m->timestamps[LM_TS_STAMP] = m->timestamps[LM_TS_RECVD];
  m->pri = self->last_msg->pri;
  m->flags = LF_INTERNAL | LF_LOCAL;

  /* $RCPTID create*/
  log_msg_create_rcptid(m);

  p = log_msg_get_value(self->last_msg, LM_V_HOST, &len);
  log_msg_set_value(m, LM_V_HOST, p, len);
  p = log_msg_get_value(self->last_msg, LM_V_PROGRAM, &len);
  log_msg_set_value(m, LM_V_PROGRAM, p, len);

  len = g_snprintf(buf, sizeof(buf), "Last message '%.20s' repeated %d times, suppressed by syslog-ng on %s",
                   log_msg_get_value(self->last_msg, LM_V_MESSAGE, NULL),
                   self->last_msg_count,
                   hostname);
  log_msg_set_value(m, LM_V_MESSAGE, buf, len);

  path_options.ack_needed = FALSE;

  log_queue_push_tail(self->queue, m, &path_options);
  log_writer_last_msg_release(self);
}


/**
 * Remember the last message for dup detection.
 *
 * NOTE: suppress_lock must be held.
 **/
static void
log_writer_last_msg_record(LogWriter *self, LogMessage *lm)
{
  if (self->last_msg)
    log_msg_unref(self->last_msg);

  log_msg_ref(lm);
  self->last_msg = lm;
  self->last_msg_count = 0;
}

static gboolean
log_writer_last_msg_timer(gpointer pt)
{
  LogWriter *self = (LogWriter *) pt;

  main_loop_assert_main_thread();

  g_static_mutex_lock(&self->suppress_lock);
  log_writer_last_msg_flush(self);
  g_static_mutex_unlock(&self->suppress_lock);

  return FALSE;
}

/**
 * log_writer_last_msg_check:
 *
 * This function is called to suppress duplicate messages from a given host.
 *
 * Returns TRUE to indicate that the message was consumed.
 **/
static gboolean
log_writer_last_msg_check(LogWriter *self, LogMessage *lm, const LogPathOptions *path_options)
{
  g_static_mutex_lock(&self->suppress_lock);
  if (self->last_msg)
    {
      if (self->last_msg->timestamps[LM_TS_RECVD].tv_sec >= lm->timestamps[LM_TS_RECVD].tv_sec - self->options->suppress &&
          strcmp(log_msg_get_value(self->last_msg, LM_V_MESSAGE, NULL), log_msg_get_value(lm, LM_V_MESSAGE, NULL)) == 0 &&
          strcmp(log_msg_get_value(self->last_msg, LM_V_HOST, NULL), log_msg_get_value(lm, LM_V_HOST, NULL)) == 0 &&
          strcmp(log_msg_get_value(self->last_msg, LM_V_PROGRAM, NULL), log_msg_get_value(lm, LM_V_PROGRAM, NULL)) == 0 &&
          strcmp(log_msg_get_value(self->last_msg, LM_V_PID, NULL), log_msg_get_value(lm, LM_V_PID, NULL)) == 0 &&
          strcmp(log_msg_get_value(lm, LM_V_MESSAGE, NULL), "-- MARK --") != 0)
        {
          stats_counter_inc(self->suppressed_messages);
          self->last_msg_count++;
          
          if (self->last_msg_count == 1)
            {
              /* we only create the timer if this is the first suppressed message, otherwise it is already running. */

              log_writer_update_suppress_timer(self, self->options->suppress);
            }
          g_static_mutex_unlock(&self->suppress_lock);

          msg_debug("Suppressing duplicate message",
                    evt_tag_str("host", log_msg_get_value(lm, LM_V_HOST, NULL)),
                    evt_tag_str("msg", log_msg_get_value(lm, LM_V_MESSAGE, NULL)),
                    NULL);
          log_msg_drop(lm, path_options, AT_PROCESSED);
          return TRUE;
        }

      if (self->last_msg_count)
        log_writer_last_msg_flush(self);
      else
        log_writer_last_msg_release(self);
    }

  log_writer_last_msg_record(self, lm);
  g_static_mutex_unlock(&self->suppress_lock);
  return FALSE;
}

void
log_writer_arm_mark_timer(LogWriter *self)
{
  main_loop_assert_main_thread();
  if (iv_timer_registered(&self->mark_timer))
    {
      iv_timer_unregister(&self->mark_timer);
    }
  iv_validate_now();
  self->mark_timer.expires = iv_now;
  self->mark_timer.expires.tv_sec += self->options->mark_freq; /* mark_freq measures in [sec] */
  iv_timer_register(&self->mark_timer);
}

void
log_writer_mark_timeout(void *cookie)
{
  LogWriter *self = (LogWriter *)cookie;
  LogPathOptions path_options = {FALSE,FALSE, NULL};
  gchar hostname[256];
  gsize hostname_len = sizeof(hostname);
  LogMessage *msg = log_msg_new_mark();
  GTimeVal current_time;

  main_loop_assert_main_thread();

  /* timeout: there was no new message on the writer or it is in periodical mode */
  resolve_sockaddr(hostname, &hostname_len, msg->saddr, self->options->use_dns, self->options->use_fqdn, self->options->use_dns_cache, self->options->normalize_hostnames);

  log_msg_set_value(msg, LM_V_HOST, hostname, strlen(hostname));

  /* set the current time int the message stamp */
  cached_g_current_time(&current_time);
  msg->timestamps[LM_TS_STAMP].tv_sec = current_time.tv_sec;
  msg->timestamps[LM_TS_STAMP].tv_usec = current_time.tv_usec;
  msg->timestamps[LM_TS_STAMP].zone_offset = get_local_timezone_ofs(msg->timestamps[LM_TS_STAMP].tv_sec);

  msg->timestamps[LM_TS_RECVD].tv_sec = current_time.tv_sec;
  msg->timestamps[LM_TS_RECVD].tv_usec = current_time.tv_usec;
  msg->timestamps[LM_TS_RECVD].zone_offset = msg->timestamps[LM_TS_STAMP].zone_offset;
  /* $RCPTID create*/
  log_msg_create_rcptid(msg);

  log_queue_push_tail(self->queue, msg, &path_options);
  /* the timer has to be continued - because the ivykis stops the current (expired) timer, we have to start a new one
     with the previous cookie (-> new allocation is not needed) */
  log_writer_arm_mark_timer(self);
}

/*
 * NOTE: start the mark timer if needed (e.g. the mark-mode requires it).
 * Schedules a function call in the main thread to do the actual setup.
 */
static void
log_writer_start_mark_timer(LogWriter *self)
{
  gint mark_mode = self->options->mark_mode;
  if ((mark_mode == MM_DST_IDLE || mark_mode == MM_HOST_IDLE || mark_mode == MM_PERIODICAL) && self->options->mark_freq > 0)
    {
      /* start the timer */
      main_loop_call((void * (*)(void *)) log_writer_arm_mark_timer, self, FALSE);
    }
}

/* NOTE: runs in the reader thread */
static void
log_writer_queue(LogPipe *s, LogMessage *lm, const LogPathOptions *path_options, gpointer user_data)
{
  LogWriter *self = (LogWriter *) s;
  LogPathOptions local_options;
  gint mark_mode = self->options->mark_mode;


  if (!path_options->flow_control_requested &&
      (self->suspended || !(self->flags & LW_SOFT_FLOW_CONTROL)))
    {
      /* NOTE: this code ACKs the message back if there's a write error in
       * order not to hang the client in case of a disk full */

      path_options = log_msg_break_ack(lm, path_options, &local_options);
    }
  if (self->options->suppress > 0 && log_writer_last_msg_check(self, lm, path_options))
    return;

  if (mark_mode != MM_INTERNAL && (lm->flags & LF_INTERNAL) && (lm->flags & LF_MARK))
    {
      /* skip the internal MARK messages */
      log_msg_drop(lm, path_options, AT_PROCESSED);
      return;
    }

  if (mark_mode == MM_DST_IDLE || (mark_mode == MM_HOST_IDLE && (lm->flags & LF_LOCAL)))
    {
      /* in case of periodical marks the timer has already been started in log_writer_init()
         we must not start a new one in order to avoid duplicate marks */
      log_writer_start_mark_timer(self);
    }

  stats_counter_inc(self->processed_messages);
  log_queue_push_tail(self->queue, lm, path_options);
}

static void
log_writer_append_value(GString *result, LogMessage *lm, NVHandle handle, gboolean use_nil, gboolean append_space)
{
  const gchar *value;
  gssize value_len;

  value = log_msg_get_value(lm, handle, &value_len);
  if (use_nil && value_len == 0)
    g_string_append_c(result, '-');
  else
    {
      gchar *space;
      
      space = strchr(value, ' ');
      
      if (!space)
        g_string_append_len(result, value, value_len);
      else
        g_string_append_len(result, value, space - value);
    }
  if (append_space)
    g_string_append_c(result, ' ');
}

static void
log_writer_do_padding(LogWriter *self, GString *result)
{
  if (!self->options->padding)
    return;

  if(G_UNLIKELY(self->options->padding < result->len))
    {
      msg_warning("Padding is too small to hold the full message",
               evt_tag_int("padding", self->options->padding),
               evt_tag_int("msg_size", result->len),
               NULL);
      g_string_set_size(result, self->options->padding);
      return;
    }
  /* store the original length of the result */
  gint len = result->len;
  gint padd_bytes = self->options->padding - result->len;
  /* set the size to the padded size, this will allocate the string */
  g_string_set_size(result, self->options->padding);
  memset(result->str + len - 1, '\0', padd_bytes);
}

void
log_writer_format_log(LogWriter *self, LogMessage *lm, GString *result)
{
  LogTemplate *template = NULL;
  LogStamp *stamp;
  guint32 seq_num;
  static NVHandle meta_seqid = 0;

  /* First truncate the result string */
  result->str[0] = 0;
  result->len = 0;
  if (!meta_seqid)
    meta_seqid = log_msg_get_value_handle(".SDATA.meta.sequenceId");

  if (lm->flags & LF_LOCAL)
    {
      seq_num = self->seq_num;
    }
  else
    {
      const gchar *seqid;
      gssize seqid_length;

      seqid = log_msg_get_value(lm, meta_seqid, &seqid_length);
      APPEND_ZERO(seqid, seqid, seqid_length);
      if (seqid[0])
        seq_num = strtol(seqid, NULL, 10);
      else
        seq_num = 0;
    }
  
  /* no template was specified, use default */
  stamp = &lm->timestamps[LM_TS_STAMP];

  g_string_truncate(result, 0);

  if ((self->flags & LW_SYSLOG_PROTOCOL) || (self->options->options & LWO_SYSLOG_PROTOCOL))
    {
      gint len;
       
      /* we currently hard-wire version 1 */
      g_string_append_c(result, '<');
      format_uint32_padded(result, 0, 0, 10, lm->pri);
      g_string_append_c(result, '>');
      g_string_append_c(result, '1');
      g_string_append_c(result, ' ');
 
      log_stamp_append_format(stamp, result, TS_FMT_ISO, 
                              time_zone_info_get_offset(self->options->template_options.time_zone_info[LTZ_SEND], stamp->tv_sec),
                              self->options->template_options.frac_digits);
      g_string_append_c(result, ' ');
      
      log_writer_append_value(result, lm, LM_V_HOST, TRUE, TRUE);
      log_writer_append_value(result, lm, LM_V_PROGRAM, TRUE, TRUE);
      log_writer_append_value(result, lm, LM_V_PID, TRUE, TRUE);
      log_writer_append_value(result, lm, LM_V_MSGID, TRUE, TRUE);

#if 0
      if (lm->flags & LF_LOCAL)
        {
          gchar sequence_id[16];
          
          g_snprintf(sequence_id, sizeof(sequence_id), "%d", seq_num);
          log_msg_update_sdata(lm, "meta", "sequenceId", sequence_id);
        }
#endif
      len = result->len;
      log_msg_append_format_sdata(lm, result, seq_num);
      if (len == result->len)
        {
          /* NOTE: sd_param format did not generate any output, take it as an empty SD string */
          g_string_append_c(result, '-');
        }
       
      if (self->options->template)
        {
          g_string_append_c(result, ' ');
          if (lm->flags & LF_UTF8)
            g_string_append_len(result, "\xEF\xBB\xBF", 3);
          log_template_append_format(self->options->template, lm, 
                                     &self->options->template_options,
                                     LTZ_SEND,
                                     seq_num, NULL,
                                     result);
        }
      else
        {
          const gchar *p;
          gssize len;

          p = log_msg_get_value(lm, LM_V_MESSAGE, &len);
          g_string_append_c(result, ' ');
          if (len != 0)
            {
              if (lm->flags & LF_UTF8)
                g_string_append_len(result, "\xEF\xBB\xBF", 3);

              g_string_append_len(result, p, len);
            }
        }
      g_string_append_c(result, '\n');
      log_writer_do_padding(self, result);
    }
  else
    {

      if (self->options->template)
        {
          template = self->options->template;
        }
      else if (self->flags & LW_FORMAT_FILE)
        {
          template = self->options->file_template;
        }
      else if ((self->flags & LW_FORMAT_PROTO))
        {
          template = self->options->proto_template;
        }
      
      if (template)
        {
          log_template_format(template, lm, 
                              &self->options->template_options,
                              LTZ_SEND,
                              seq_num, NULL,
                              result);

        }
      else 
        {
          const gchar *p;
          gssize len;

          if (self->flags & LW_FORMAT_FILE)
            {
              log_stamp_format(stamp, result, self->options->template_options.ts_format,
                               time_zone_info_get_offset(self->options->template_options.time_zone_info[LTZ_SEND], stamp->tv_sec),
                               self->options->template_options.frac_digits);
            }
          else if (self->flags & LW_FORMAT_PROTO)
            {
	      g_string_append_c(result, '<');
	      format_uint32_padded(result, 0, 0, 10, lm->pri);
	      g_string_append_c(result, '>');

              /* always use BSD timestamp by default, the use can override this using a custom template */
              log_stamp_append_format(stamp, result, TS_FMT_BSD,
                                      time_zone_info_get_offset(self->options->template_options.time_zone_info[LTZ_SEND], stamp->tv_sec),
                                      self->options->template_options.frac_digits);
            }
          g_string_append_c(result, ' ');

          p = log_msg_get_value(lm, LM_V_HOST, &len);
          g_string_append_len(result, p, len);
          g_string_append_c(result, ' ');

          if ((lm->flags & LF_LEGACY_MSGHDR))
            {
              p = log_msg_get_value(lm, LM_V_LEGACY_MSGHDR, &len);
              g_string_append_len(result, p, len);
            }
          else
            {
              p = log_msg_get_value(lm, LM_V_PROGRAM, &len);
              if (len > 0)
                {
                  g_string_append_len(result, p, len);
                  p = log_msg_get_value(lm, LM_V_PID, &len);
                  if (len > 0)
                    {
                      g_string_append_c(result, '[');
                      g_string_append_len(result, p, len);
                      g_string_append_c(result, ']');
                    }
                  g_string_append_len(result, ": ", 2);
                }
            }
          p = log_msg_get_value(lm, LM_V_MESSAGE, &len);
          g_string_append_len(result, p, len);
          g_string_append_c(result, '\n');
          log_writer_do_padding(self, result);
        }
    }
  if (self->options->options & LWO_NO_MULTI_LINE)
    {
      gchar *p;

      p = result->str;
      /* NOTE: the size is calculated to leave trailing new line */
      while ((p = find_cr_or_lf(p, result->str + result->len - p - 1)))
        {
          *p = ' ';
          p++;
        }

    }
}

static void
log_writer_broken(LogWriter *self, gint notify_code)
{
  log_writer_stop_watches(self);
  self->last_notify_code = notify_code;
  if (self->proto)
    {
      log_proto_msg_rewind_on_error(self->proto);
    }
  log_pipe_notify(self->control, &self->super, notify_code, self);
}

static inline gboolean
log_writer_process_handshake(LogWriter *self)
{
  LogProtoStatus status;

  status = log_proto_handshake(self->proto);
  if (status != LPS_SUCCESS && status != LPS_AGAIN)
    return FALSE;
  if (status == LPS_AGAIN)
    {
      self->has_to_poll = TRUE;
    }
  return TRUE;
}

static void
log_writer_realloc_line_buffer(LogWriter *self)
{
  self->line_buffer->str = g_malloc(self->line_buffer->allocated_len);
  self->line_buffer->str[0] = 0;
  self->line_buffer->len = 0;
}

static gboolean
log_writer_flush_finalize(LogWriter *self)
{
  LogProtoStatus status = log_proto_flush(self->proto);
  if (status != LPS_SUCCESS && status != LPS_AGAIN)
    return FALSE;

  if (status == LPS_AGAIN)
    {
      self->has_to_poll = TRUE;
    }
  return TRUE;
}

static gboolean
log_writer_write_message(LogWriter *self, LogMessage *msg, LogPathOptions *path_options, gboolean *write_error)
{
  gboolean consumed = FALSE;

  *write_error = FALSE;

  log_msg_refcache_start_consumer(msg, path_options);
  msg_set_context(msg);

  log_writer_format_log(self, msg, self->line_buffer);

  if (!(msg->flags & LF_INTERNAL))
    {
      msg_debug("Outgoing message",
            evt_tag_str("message",self->line_buffer->str),
            NULL);
    }

  if (self->line_buffer->len)
    {
      LogProtoStatus status = log_proto_post(self->proto, msg, (guchar *)self->line_buffer->str, self->line_buffer->len, &consumed);

      if (consumed)
        log_writer_realloc_line_buffer(self);

      if (status == LPS_ERROR)
        {
          if ((self->options->options & LWO_IGNORE_ERRORS) != 0)
            {
              if (!consumed)
                {
                  g_free(self->line_buffer->str);
                  log_writer_realloc_line_buffer(self);
                  consumed = TRUE;
                }
            }
          else
            {
              *write_error = TRUE;
              consumed = FALSE;
            }
        }
    }
  else
    {
      msg_debug("Error posting log message as template() output resulted in an empty string, skipping message",
                NULL);
      consumed = TRUE;
    }

  if (consumed)
    {
      if (msg->flags & LF_LOCAL)
        step_sequence_number(&self->seq_num);

      log_msg_unref(msg);
      msg_set_context(NULL);
      log_msg_refcache_stop();

      return TRUE;
  }
  else
    {
      msg_debug("Can't send the message rewind backlog",
                evt_tag_str("message",self->line_buffer->str),
                NULL);

      log_queue_rewind_backlog(self->queue, 1);

      log_msg_unref(msg);
      msg_set_context(NULL);
      log_msg_refcache_stop();

      return FALSE;
    }
}



static inline LogMessage *
log_writer_queue_pop_message(LogWriter *self, LogPathOptions *path_options, gboolean force_flush)
{
  if (force_flush)
    return log_queue_pop_head_ignore_throttle(self->queue, path_options);
  else
    return log_queue_pop_head(self->queue, path_options);
}

/*
 * Write messages to the underlying file descriptor using the installed
 * LogProto instance.  This is called whenever the output is ready to accept
 * further messages, and once during config deinitialization, in order to
 * flush messages still in the queue, in the hope that most of them can be
 * written out.
 *
 * In threaded mode, this function is invoked as part of the "output" task
 * (in essence, this is the function that performs the output task).
 * @flush_mode specifies how hard LogWriter is trying to send messages to
 * the actual destination:
 *
 *
 * LW_FLUSH_NORMAL    - business as usual, flush when the buffer is full
 * LW_FLUSH_FORCE     - flush the buffer immediately please
 *
 */
gboolean
log_writer_flush(LogWriter *self, LogWriterFlushMode flush_mode)
{
  gboolean write_error = FALSE;

  if (!self->proto)
    return FALSE;

  if (log_proto_handshake_in_progress(self->proto))
    {
      return log_writer_process_handshake(self);
    }
  /* NOTE: in case we're reloading or exiting we flush all queued items as
   * long as the destination can consume it.  This is not going to be an
   * infinite loop, since the reader will cease to produce new messages when
   * main_loop_io_worker_job_quit() is set. */

  while ((!main_loop_worker_job_quit() || flush_mode == LW_FLUSH_FORCE) && !write_error)
    {
      LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
      LogMessage *msg = log_writer_queue_pop_message(self, &path_options, flush_mode == LW_FLUSH_FORCE);

      if (!msg)
        break;

      if (!log_writer_write_message(self, msg, &path_options, &write_error))
        break;
    }

  if (write_error)
    return FALSE;

  return log_writer_flush_finalize(self);
}

gboolean
log_writer_reopen_elapsed(gpointer user_data);

void
log_writer_idle_time_elapsed(gpointer user_data);

static void
log_writer_init_watches(LogWriter *self)
{
  IV_FD_INIT(&self->fd_watch);
  self->fd_watch.cookie = self;

  IV_TASK_INIT(&self->immed_io_task);
  self->immed_io_task.cookie = self;
  self->immed_io_task.handler = log_writer_io_flush_output;

  IV_TIMER_INIT(&self->suspend_timer);
  self->suspend_timer.cookie = self;

  IV_TIMER_INIT(&self->suppress_timer);
  self->suppress_timer.cookie = self;
  self->suppress_timer.handler = (void (*)(void *)) log_writer_last_msg_timer;

  IV_TIMER_INIT(&self->mark_timer);
  self->mark_timer.cookie = self;
  self->mark_timer.handler = (void (*)(void *)) log_writer_mark_timeout;

  IV_TIMER_INIT(&self->reopen_timer);
  self->reopen_timer.cookie = self;
  self->reopen_timer.handler = (void (*)(void *)) log_writer_reopen_elapsed;

  IV_TIMER_INIT(&self->idle_timer);
  self->idle_timer.cookie = self;
  self->idle_timer.handler = (void (*)(void *)) log_writer_idle_time_elapsed;

  IV_EVENT_INIT(&self->queue_filled);
  self->queue_filled.cookie = self;
  self->queue_filled.handler = log_writer_queue_filled;

  main_loop_io_worker_job_init(&self->io_job);
  self->io_job.user_data = self;
  self->io_job.work = (void (*)(void *)) log_writer_work_perform;
  self->io_job.completion = (void (*)(void *)) log_writer_work_finished;
}

static gboolean
log_writer_init(LogPipe *s)
{
  LogWriter *self = (LogWriter *) s;

  if (self->queue == NULL)
    {
      return FALSE;
    }
  iv_event_register(&self->queue_filled);

  if ((self->options->options & LWO_NO_STATS) == 0 && !self->dropped_messages)
    {
      stats_lock();
      stats_register_counter(self->stats_level, self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_DROPPED, &self->dropped_messages);
      if (self->options->suppress > 0)
        stats_register_counter(self->stats_level, self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_SUPPRESSED, &self->suppressed_messages);
      stats_register_counter(self->stats_level, self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_PROCESSED, &self->processed_messages);
      
      stats_register_counter(self->stats_level, self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_STORED, &self->stored_messages);
      stats_unlock();
    }
  self->suppress_timer_updated = TRUE;
  log_queue_set_counters(self->queue, self->stored_messages, self->dropped_messages);

#ifdef _WIN32
  self->force_read = TRUE;
#endif

  if (self->proto)
    {
      LogProto *proto;

      proto = self->proto;
      log_writer_set_proto(self, NULL);
      log_writer_reopen(&self->super, proto, NULL);
    }
  log_writer_start_mark_timer(self);
  return TRUE;
}

static gboolean
log_writer_deinit(LogPipe *s)
{
  LogWriter *self = (LogWriter *) s;

  main_loop_assert_main_thread();

  log_queue_reset_parallel_push(self->queue);
  if (!log_writer_flush(self, LW_FLUSH_FORCE))
    {
      log_writer_broken(self, NC_WRITE_ERROR);
    }
  /* FIXME: by the time we arrive here, it must be guaranteed that no
   * _queue() call is running in a different thread, otherwise we'd need
   * some kind of locking. */

  log_writer_stop_watches(self);
  iv_event_unregister(&self->queue_filled);

  if (iv_timer_registered(&self->suppress_timer))
    iv_timer_unregister(&self->suppress_timer);

  if (iv_timer_registered(&self->mark_timer))
    iv_timer_unregister(&self->mark_timer);

  if (iv_timer_registered(&self->reopen_timer))
    iv_timer_unregister(&self->reopen_timer);

  if (iv_timer_registered(&self->idle_timer))
    iv_timer_unregister(&self->idle_timer);

  log_queue_set_counters(self->queue, NULL, NULL);

  stats_lock();
  stats_unregister_counter(self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_DROPPED, &self->dropped_messages);
  stats_unregister_counter(self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_SUPPRESSED, &self->suppressed_messages);
  stats_unregister_counter(self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_PROCESSED, &self->processed_messages);
  stats_unregister_counter(self->stats_source | SCS_DESTINATION, self->stats_id, self->stats_instance, SC_TYPE_STORED, &self->stored_messages);
  stats_unlock();
  
  return TRUE;
}

static void
log_writer_free(LogPipe *s)
{
  LogWriter *self = (LogWriter *) s;

  log_writer_free_proto(self);

  if (self->line_buffer)
    g_string_free(self->line_buffer, TRUE);

  log_queue_unref(self->queue);
  if (self->last_msg)
    log_msg_unref(self->last_msg);
  g_free(self->stats_id);
  g_free(self->stats_instance);
  g_static_mutex_free(&self->suppress_lock);
  g_static_mutex_free(&self->pending_proto_lock);
  g_cond_free(self->pending_proto_cond);

  log_pipe_free_method(s);
}

/* FIXME: this is inherently racy */
gboolean
log_writer_has_pending_writes(LogWriter *self)
{
  return !log_queue_is_empty_racy(self->queue) || !self->watches_running;
}

gboolean
log_writer_reopen_elapsed(gpointer user_data)
{
  LogWriter *self = (LogWriter *)user_data;
  log_pipe_notify(self->control, &self->super, NC_REOPEN_REQUIRED, self);
  return FALSE;
}

void
log_writer_idle_time_elapsed(gpointer user_data)
{
  LogWriter *self = (LogWriter *)user_data;
  msg_error("Server response time elapsed, close the connection", evt_tag_id(MSG_LOGWRITER_RESPONSE_TIMEOUT), NULL);
  log_pipe_notify(self->control, &self->super, NC_CLOSE, self);
}

gboolean
log_writer_opened(LogWriter *self)
{
  return self->proto != NULL;
}

/* run in the main thread in reaction to a log_writer_reopen to change
 * the destination LogProto instance. It needs to be ran in the main
 * thread as it reregisters the watches associated with the main
 * thread. */
void
log_writer_reopen_deferred(gpointer s)
{
  gpointer *args = (gpointer *) s;
  LogWriter *self = args[0];
  LogProto *proto = args[1];
  LogProtoOptions *proto_options = args[2];

  if (!proto)
    {
      iv_validate_now();
      self->reopen_timer.expires = iv_now;
      self->reopen_timer.expires.tv_sec += self->options->time_reopen;
      if (iv_timer_registered(&self->reopen_timer))
        {
          iv_timer_unregister(&self->reopen_timer);
        }
      iv_timer_register(&self->reopen_timer);
    }

  if (self->io_job.working)
    {
      /* NOTE: proto can be NULL but it is present... */
      log_writer_set_pending_proto(self, proto, TRUE);
      return;
    }

  log_writer_stop_watches(self);
  log_writer_free_proto(self);
  log_writer_set_proto(self, proto);

  if (proto)
    {
      log_proto_set_options(proto, proto_options);
      log_writer_start_watches(self);
    }
}

/*
 * This function can be called from any threads, from the main thread
 * as well as I/O worker threads. It takes care about going to the
 * main thread to actually switch LogProto under this writer.
 *
 * The writer may still be operating, (e.g. log_pipe_deinit/init is
 * not needed).
 *
 * In case we're running in a non-main thread, then by the time this
 * function returns, the reopen has finished. In case it is called
 * from the main thread, this function may defer updating self->proto
 * until the worker thread has finished. The reason for this
 * difference is:
 *
 *   - if LogWriter is busy, then updating the LogProto instance is
 *     deferred to log_writer_work_finished(), but that runs in the
 *     main thread.
 *
 *   - normally, even this deferred update is waited for, but in case
 *     we're in the main thread, we can't block.
 *
 * This situation could probably be improved, maybe the synchonous
 * return of log_writer_reopen() is not needed by call sites, but I
 * was not sure, and right before release I didn't want to take the
 * risky approach.
 */
void
log_writer_reopen(LogPipe *s, LogProto *proto, LogProtoOptions *proto_options)
{
  LogWriter *self = (LogWriter *) s;
  gpointer args[] = { s, proto, proto_options };

  main_loop_call((MainLoopTaskFunc) log_writer_reopen_deferred, args, TRUE);

  if (!main_loop_is_main_thread())
    {
      g_static_mutex_lock(&self->pending_proto_lock);
      while (self->pending_proto_present)
        {
          g_cond_wait(self->pending_proto_cond, g_static_mutex_get_mutex(&self->pending_proto_lock));
        }
      g_static_mutex_unlock(&self->pending_proto_lock);
    }
}

void
log_writer_set_options(LogWriter *self, LogPipe *control, LogWriterOptions *options, gint stats_level, gint stats_source, const gchar *stats_id, const gchar *stats_instance,LogProtoOptions *proto_options)
{
  self->control = control;
  self->options = options;

  self->stats_level = stats_level;
  self->stats_source = stats_source;

  if (self->stats_id)
    g_free(self->stats_id);
  self->stats_id = stats_id ? g_strdup(stats_id) : NULL;
  if (self->stats_instance)
    g_free(self->stats_instance);
  self->stats_instance = stats_instance ? g_strdup(stats_instance) : NULL;
  if (self->proto)
    {
      log_proto_set_options(self->proto,proto_options);
    }
}

LogPipe *
log_writer_new(guint32 flags)
{
  LogWriter *self = g_new0(LogWriter, 1);
  
  log_pipe_init_instance(&self->super);
  self->super.init = log_writer_init;
  self->super.deinit = log_writer_deinit;
  self->super.queue = log_writer_queue;
  self->super.free_fn = log_writer_free;
  self->flags = flags;
  self->line_buffer = g_string_sized_new(128);
  self->pollable_state = -1;
  init_sequence_number(&self->seq_num);

  log_writer_init_watches(self);
  g_static_mutex_init(&self->suppress_lock);
  g_static_mutex_init(&self->pending_proto_lock);
  self->pending_proto_cond = g_cond_new();

  return &self->super;
}

/* consumes the reference */
void
log_writer_set_queue(LogPipe *s, LogQueue *queue)
{
  LogWriter *self = (LogWriter *)s;

  log_queue_unref(self->queue);
  self->queue = log_queue_ref(queue);
  log_queue_set_use_backlog(self->queue, TRUE);
}

void 
log_writer_options_defaults(LogWriterOptions *options)
{
  options->template = NULL;
  options->flush_lines = -1;
  options->flush_timeout = -1;
  log_template_options_defaults(&options->template_options);
  options->time_reopen = -1;
  options->suppress = -1;
  options->padding = 0;
  options->mark_mode = MM_GLOBAL;
  options->mark_freq = -1;
}

void 
log_writer_options_set_template_escape(LogWriterOptions *options, gboolean enable)
{
  if (options->template && options->template->def_inline)
    {
      log_template_set_escape(options->template, enable);
    }
  else
    {
      msg_error("Macro escaping can only be specified for inline templates", NULL);
    }
}


/*
 * NOTE: options_init and options_destroy are a bit weird, because their
 * invocation is not completely symmetric:
 *
 *   - init is called from driver init (e.g. affile_dd_init), 
 *   - destroy is called from driver free method (e.g. affile_sd_free, NOT affile_dd_deinit)
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
log_writer_options_init(LogWriterOptions *options, GlobalConfig *cfg, guint32 option_flags)
{
  LogTemplate *template;
  gchar *time_zone[2];
  TimeZoneInfo *time_zone_info[2];
  gint i;

  template = log_template_ref(options->template);

  for (i = 0; i < LTZ_MAX; i++)
    {
      time_zone[i] = options->template_options.time_zone[i];
      time_zone_info[i] = options->template_options.time_zone_info[i];
      options->template_options.time_zone[i] = NULL;
      options->template_options.time_zone_info[i] = NULL;
    }

  log_writer_options_destroy(options);
  log_template_options_destroy(&options->template_options);
  
  /* restroe the config */
  options->template = template;
  for (i = 0; i < LTZ_MAX; i++)
    {
      options->template_options.time_zone[i] = time_zone[i];
      options->template_options.time_zone_info[i] = time_zone_info[i];
    }
  log_template_options_init(&options->template_options, cfg);
  options->options |= option_flags;
    
  if (options->flush_lines == -1)
    options->flush_lines = cfg->flush_lines;
  if (options->flush_timeout == -1)
    options->flush_timeout = cfg->flush_timeout;
  if (options->suppress == -1)
    options->suppress = cfg->suppress;
  if (options->time_reopen == -1)
    options->time_reopen = cfg->time_reopen;
  options->file_template = log_template_ref(cfg->file_template);
  options->proto_template = log_template_ref(cfg->proto_template);
  if (cfg->threaded)
    options->options |= LWO_THREADED;
  /* per-destination MARK messages */
  if (options->mark_mode == MM_GLOBAL)
    /* get the global option */
    options->mark_mode = cfg->mark_mode;
  if (options->mark_freq == -1)
    /* not initialized, use the global mark freq */
    options->mark_freq = cfg->mark_freq;

   options->use_dns = cfg->use_dns;
   options->use_fqdn = cfg->use_fqdn;
   options->use_dns_cache = cfg->use_dns_cache;
   options->normalize_hostnames = cfg->normalize_hostnames;
}

void
log_writer_options_destroy(LogWriterOptions *options)
{
  log_template_options_destroy(&options->template_options);
  log_template_unref(options->template);
  log_template_unref(options->file_template);
  log_template_unref(options->proto_template);
}

gint
log_writer_options_lookup_flag(const gchar *flag)
{
  if (strcmp(flag, "syslog_protocol") == 0 || strcmp(flag, "syslog-protocol") == 0)
    return LWO_SYSLOG_PROTOCOL;
  if (strcmp(flag, "no-multi-line") == 0 || strcmp(flag, "no_multi_line") == 0)
    return LWO_NO_MULTI_LINE;
  if (strcmp(flag, "threaded") == 0)
#if ENABLE_THREADED
    return LWO_THREADED;
#else
    msg_warning("Threaded not supported on this platform",evt_tag_id(MSG_THREADED_ISNT_AVAILABLE),NULL);
    return 0;
#endif
  if (strcmp(flag, "ignore-errors") == 0 || strcmp(flag, "ignore_errors") == 0)
    return LWO_IGNORE_ERRORS;
  msg_error("Unknown dest writer flag", evt_tag_str("flag", flag), evt_tag_id(MSG_LOGWRITER_UNKNOWN_FLAG), NULL);
  return 0;
}

void
log_writer_options_set_mark_mode(LogWriterOptions *options, gchar *mark_mode)
{
  gint mm = cfg_get_mark_mode(mark_mode);
  if (mm == -1)
    {
      msg_error("Wrong destination mark mode",
                 evt_tag_str("mark_mode", mark_mode),
                 evt_tag_id(MSG_LOGWRITER_WRONG_MARKMODE),
                 NULL);
    }

  options->mark_mode = mm;
}
