/*
 * Copyright (c) 2002-2012 Balabit
 * Copyright (c) 1998-2012 Balázs Scheidler
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
 *
 */

#ifndef AFSOCKET_H_INCLUDED
#define AFSOCKET_H_INCLUDED

#include "driver.h"
#include "logreader.h"
#include "logwriter.h"
#include "tlscontext.h"
#include "socket-options.h"

#include <iv.h>

#define AFSOCKET_DGRAM               0x0001
#define AFSOCKET_STREAM              0x0002
#define AFSOCKET_LOCAL               0x0004

#define AFSOCKET_SYSLOG_DRIVER        0x0010
#define AFSOCKET_NETWORK_DRIVER       0x0020

#define AFSOCKET_KEEP_ALIVE          0x0100
#define AFSOCKET_REQUIRE_TLS         0x0200

#define AFSOCKET_WNDSIZE_INITED      0x10000

typedef struct _AFSocketSourceDriver AFSocketSourceDriver;
typedef struct _AFSocketDestDriver AFSocketDestDriver;

struct _AFSocketSourceDriver
{
  LogSrcDriver super;
  guint32 flags;
  struct iv_fd listen_fd;
  gint fd;
  LogReaderOptions reader_options;
  LogProtoOptions proto_options;
  TLSContext *tls_context;
  gint address_family;
  GSockAddr *bind_addr;
  gchar *transport;
  LogProtoFactory *proto_factory;
  gint max_connections;
  gint num_connections;
  gint listen_backlog;
  GList *connections;
  SocketOptions *sock_options_ptr;

  /*
   * Apply transport options, set up bind_addr based on the
   * information processed during parse time. This used to be
   * constructed during the parser, however that made the ordering of
   * various options matter and behave incorrectly when the port() was
   * specified _after_ transport(). Now, it collects the information,
   * and then applies them with a separate call to apply_transport()
   * during init().
   */

  gboolean (*apply_transport)(AFSocketSourceDriver *s);

  /* optionally acquire a socket from the runtime environment (e.g. systemd) */
  gboolean (*acquire_socket)(AFSocketSourceDriver *s, gint *fd);
  gboolean (*setup_socket)(AFSocketSourceDriver *s, gint fd);
};

void afsocket_sd_set_transport(LogDriver *s, const gchar *transport);
void afsocket_sd_set_keep_alive(LogDriver *self, gint enable);
void afsocket_sd_set_max_connections(LogDriver *self, gint max_connections);
void afsocket_sd_set_tls_context(LogDriver *s, TLSContext *tls_context);

static inline gboolean
afsocket_sd_acquire_socket(AFSocketSourceDriver *s, gint *fd)
{
  if (s->acquire_socket)
    return s->acquire_socket(s, fd);
  *fd = -1;
  return TRUE;
}

static inline gboolean
afsocket_sd_apply_transport(AFSocketSourceDriver *s)
{
  return s->apply_transport(s);
}

gboolean afsocket_sd_init(LogPipe *s);
gboolean afsocket_sd_deinit(LogPipe *s);

void afsocket_sd_init_instance(AFSocketSourceDriver *self, SocketOptions *sock_options, gint family, guint32 flags);
void afsocket_sd_free(LogPipe *self);

struct _AFSocketDestDriver
{
  LogDestDriver super;
  guint32 flags;
  gint fd;
  LogPipe *writer;
  LogWriterOptions writer_options;
  LogProtoOptions  proto_options;
  TLSContext *tls_context;
  LogProtoFactory *proto_factory;
  gint address_family;
  gchar *hostname;
  gchar *transport;
  GSockAddr *bind_addr;
  GSockAddr *dest_addr;
  gchar *dest_name;
  gint time_reopen;
  struct iv_fd connect_fd;
  SocketOptions *sock_options_ptr;
  GList *server_name_list;

  /*
   * Apply transport options, set up bind_addr/dest_addr based on the
   * information processed during parse time. This used to be
   * constructed during the parser, however that made the ordering of
   * various options matter and behave incorrectly when the port() was
   * specified _after_ transport(). Now, it collects the information,
   * and then applies them with a separate call to apply_transport()
   * during init().
   */

  gboolean (*apply_transport)(AFSocketDestDriver *s);
  gboolean (*setup_socket)(AFSocketDestDriver *s, gint fd);
};


void afsocket_dd_add_failovers(LogDriver *s, GList *failover_list);
void afsocket_dd_set_tls_context(LogDriver *s, TLSContext *tls_context);

static inline gboolean
afsocket_dd_apply_transport(AFSocketDestDriver *s)
{
  return s->apply_transport(s);
}

void afsocket_dd_set_transport(LogDriver *s, const gchar *transport);
void afsocket_dd_set_keep_alive(LogDriver *self, gint enable);
void afsocket_dd_init_instance(AFSocketDestDriver *self, SocketOptions *sock_options, gint family, const gchar *hostname, guint32 flags);
gboolean afsocket_dd_init(LogPipe *s);
gboolean afsocket_dd_deinit(LogPipe *s);
void afsocket_dd_free(LogPipe *s);
gboolean afsocket_sd_set_multi_line_prefix(LogDriver *s, gchar *prefix);
gboolean afsocket_sd_set_multi_line_garbage(LogDriver *s, gchar *garbage);

#endif
