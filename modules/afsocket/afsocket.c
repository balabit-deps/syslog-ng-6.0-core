/*
 * Copyright (c) 2002-2010 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2010 Balázs Scheidler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
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

#include "afsocket.h"
#include "messages.h"
#include "driver.h"
#include "misc.h"
#include "logwriter.h"
#include "hostname.h"
#if ENABLE_SSL
#include "tlstransport.h"
#endif
#include "gprocess.h"
#include "gsocket.h"
#include "stats.h"
#include "mainloop.h"
#include "versioning.h"
#include "compat.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#ifndef G_OS_WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#if ENABLE_TCP_WRAPPER
#include <tcpd.h>
int allow_severity = 0;
int deny_severity = 0;
#endif

#ifndef _WIN32
#define closesocket close
#endif

static GHashTable *persist_hash;

typedef struct _AFSocketSourceConnection
{
  LogPipe super;
  struct _AFSocketSourceDriver *owner;
  LogPipe *reader;
  int sock;
  GSockAddr *peer_addr;
} AFSocketSourceConnection;

typedef struct _AFSocketReloadInfo
{
  LogPipe *writer;
  LogProtoFactory *proto_factory;
} AFSocketReloadInfo;

static void afsocket_sd_close_connection(AFSocketSourceDriver *self, AFSocketSourceConnection *sc);

void
afsocket_reload_info_free(AFSocketReloadInfo *reload_info)
{
  log_pipe_unref(reload_info->writer);
  g_free(reload_info);
}

gboolean
afsocket_setup_socket(gint fd, SocketOptions *sock_options, AFSocketDirection dir)
{
  if (dir & AFSOCKET_DIR_RECV)
    {
      if (sock_options->rcvbuf)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *) &sock_options->rcvbuf, sizeof(sock_options->rcvbuf));
    }
  if (dir & AFSOCKET_DIR_SEND)
    {
      if (sock_options->sndbuf)
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *) &sock_options->sndbuf, sizeof(sock_options->sndbuf));
      if (sock_options->broadcast)
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (char *) &sock_options->broadcast, sizeof(sock_options->broadcast));
    }
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *) &sock_options->keepalive, sizeof(sock_options->keepalive));
  return TRUE;
}

static gboolean
afsocket_open_socket(GSockAddr *bind_addr, int stream_or_dgram, int *fd)
{
  gint sock;

  if (stream_or_dgram)
    sock = socket(bind_addr->sa.sa_family, SOCK_STREAM, 0);
  else
    sock = socket(bind_addr->sa.sa_family, SOCK_DGRAM, 0);

  if (sock != -1)
    {
      cap_t saved_caps;
#ifndef G_OS_WIN32
      g_fd_set_nonblock(sock, TRUE);
      g_fd_set_cloexec(sock, TRUE);
#else
      u_long mode = 1;
      if (ioctlsocket(sock, FIONBIO, &mode) != 0)
        {
          msg_error("Can't set non-blocking mode for socket", NULL);
        }
#endif
      saved_caps = g_process_cap_save();
      g_process_cap_modify(CAP_NET_BIND_SERVICE, TRUE);
      g_process_cap_modify(CAP_DAC_OVERRIDE, TRUE);
      if (g_bind(sock, bind_addr) != G_IO_STATUS_NORMAL)
        {
          gchar buf[256];
          errno = getsockerror();
          g_process_cap_restore(saved_caps);
          msg_error("Error binding socket",
                    evt_tag_str("addr", g_sockaddr_format(bind_addr, buf, sizeof(buf), GSA_FULL)),
                    evt_tag_socket_error(EVT_TAG_OSERROR, errno),
                    evt_tag_id(MSG_ERROR_BINDING_SOCKET),
                    NULL);
          closesocket(sock);
          return FALSE;
        }
      g_process_cap_restore(saved_caps);

      *fd = sock;
      return TRUE;
    }
  else
    {
      errno = getsockerror();
      msg_error("Error creating socket",
                evt_tag_socket_error(EVT_TAG_OSERROR, errno),
                evt_tag_id(MSG_ERROR_CREATING_SOCKET),
                NULL);
      return FALSE;
    }
}

static gint
afsocket_sc_stats_source(AFSocketSourceConnection *self)
{
  gint source;

  if ((self->owner->flags & AFSOCKET_SYSLOG_DRIVER) == 0)
    {
      switch (self->owner->bind_addr->sa.sa_family)
        {
        case AF_UNIX:
          source = !!(self->owner->flags & AFSOCKET_STREAM) ? SCS_UNIX_STREAM : SCS_UNIX_DGRAM;
          break;
        case AF_INET:
          source = !!(self->owner->flags & AFSOCKET_STREAM) ? SCS_TCP : SCS_UDP;
          break;
#if ENABLE_IPV6
        case AF_INET6:
          source = !!(self->owner->flags & AFSOCKET_STREAM) ? SCS_TCP6 : SCS_UDP6;
          break;
#endif
        default:
          g_assert_not_reached();
          break;
        }
    }
  else
    {
      source = SCS_SYSLOG;
    }
  return source;
}

static gchar *
afsocket_sc_stats_instance(AFSocketSourceConnection *self)
{
  static gchar buf[256];

  if (!self->peer_addr)
    {
      if (self->owner->bind_addr)
        {
          g_sockaddr_format(self->owner->bind_addr, buf, sizeof(buf), GSA_ADDRESS_ONLY);
          return buf;
        }
      else
        return NULL;
    }
  if ((self->owner->flags & AFSOCKET_SYSLOG_DRIVER) == 0)
    {
      g_sockaddr_format(self->peer_addr, buf, sizeof(buf), GSA_ADDRESS_ONLY);
    }
  else
    {
      gchar peer_addr[MAX_SOCKADDR_STRING];

      g_sockaddr_format(self->peer_addr, peer_addr, sizeof(peer_addr), GSA_ADDRESS_ONLY);
      g_snprintf(buf, sizeof(buf), "%s,%s", self->owner->transport, peer_addr);
    }
  return buf;
}

static gboolean
afsocket_sc_init(LogPipe *s)
{
  AFSocketSourceConnection *self = (AFSocketSourceConnection *) s;
  gint read_flags;
  LogTransport *transport;
  LogProto *proto;
  GlobalConfig *cfg = log_pipe_get_config(&self->owner->super.super.super);
  LogProtoServerOptions *options = (LogProtoServerOptions *)&self->owner->proto_options;

  read_flags = ((self->owner->flags & AFSOCKET_DGRAM) ? LTF_RECV : 0);
  read_flags |= LTF_SOCKET;
  if (!self->reader)
    {
      if (self->owner->proto_factory->construct_transport == NULL)
        {
#if ENABLE_SSL
          if (self->owner->tls_context)
            {
              TLSSession *tls_session = tls_context_setup_session(self->owner->tls_context, cfg);
              if (!tls_session)
                return FALSE;
              transport = log_transport_tls_new(tls_session, self->sock, read_flags);
            }
          else
#endif
            transport = log_transport_plain_new(self->sock, read_flags);
        }
      else
        {
          transport = self->owner->proto_factory->construct_transport((LogProtoOptions *)options, self->sock, read_flags, self->owner->tls_context);
        }
      if (!transport)
        {
          return FALSE;
        }

      if (self->owner->reader_options.padding)
        {
          options->super.size = self->owner->reader_options.padding;
        }
      else
        {
          options->super.size = self->owner->reader_options.msg_size;
        }

      proto = self->owner->proto_factory->create(transport,(LogProtoOptions *)options, log_pipe_get_config(&self->owner->super.super.super));
      if (!proto)
        {
          log_transport_free(transport);
          return FALSE;
        }
      self->reader = log_reader_new(proto);
    }
  log_reader_set_options(self->reader, s, &self->owner->reader_options, 1, afsocket_sc_stats_source(self), self->owner->super.super.id, afsocket_sc_stats_instance(self), &self->owner->proto_options);
  log_reader_set_peer_addr(self->reader, self->peer_addr);
  log_pipe_append(self->reader, s);
  if (log_pipe_init(self->reader, cfg))
    {
      return TRUE;
    }
  else
    {
      log_pipe_unref(self->reader);
      self->reader = NULL;
    }
  return FALSE;
}

static gboolean
afsocket_sc_deinit(LogPipe *s)
{
  AFSocketSourceConnection *self = (AFSocketSourceConnection *) s;

  log_pipe_unref(&self->owner->super.super.super);
  self->owner = NULL;

  log_pipe_deinit(self->reader);
  return TRUE;
}

static void
afsocket_sc_notify(LogPipe *s, LogPipe *sender, gint notify_code, gpointer user_data)
{
  AFSocketSourceConnection *self = (AFSocketSourceConnection *) s;

  switch (notify_code)
    {
    case NC_CLOSE:
    case NC_READ_ERROR:
      {
        if (self->owner->flags & AFSOCKET_STREAM)
          afsocket_sd_close_connection(self->owner, self);
        break;
      }
    }
}

static void
afsocket_sc_set_owner(AFSocketSourceConnection *self, AFSocketSourceDriver *owner)
{
  if (self->owner)
    {
      log_pipe_unref(&self->owner->super.super.super);
    }
  self->owner = owner;
  log_pipe_ref(&owner->super.super.super);

  log_pipe_append(&self->super, &owner->super.super.super);
}

static void
afsocket_sc_kill_connection(AFSocketSourceConnection *self)
{
  /* Remove the circular reference between the connection and its
   * reader (through the connection->reader and reader->control
   * pointers these have a circular references).
   */
  log_pipe_unref(self->reader);
  self->reader = NULL;
  log_pipe_unref(&self->super);
}

/*
  This should be called by log_reader_free -> log_pipe_unref
  because this is the control pipe of the reader
*/
static void
afsocket_sc_free(LogPipe *s)
{
  AFSocketSourceConnection *self = (AFSocketSourceConnection *) s;
  g_sockaddr_unref(self->peer_addr);
  log_pipe_free_method(s);
}

AFSocketSourceConnection *
afsocket_sc_new(AFSocketSourceDriver *owner, GSockAddr *peer_addr, int fd)
{
  AFSocketSourceConnection *self = g_new0(AFSocketSourceConnection, 1);

  log_pipe_init_instance(&self->super);
  self->super.init = afsocket_sc_init;
  self->super.deinit = afsocket_sc_deinit;
  self->super.notify = afsocket_sc_notify;
  self->super.free_fn = afsocket_sc_free;
  log_pipe_ref(&owner->super.super.super);
  self->owner = owner;


  self->peer_addr = g_sockaddr_ref(peer_addr);
  self->sock = fd;
  return self;
}

void
afsocket_sd_set_transport(LogDriver *s, const gchar *transport)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;

  if (self->transport)
    g_free(self->transport);
  self->transport = g_strdup(transport);
  if ((strlen(transport) >= 3 && strncasecmp(transport,"udp",3)==0) || (strlen(transport) >= 5 && strncasecmp(transport,"dgram",5)==0))
    {
      self->flags = (self->flags & ~0x0003) | AFSOCKET_DGRAM;
    }
  else
    {
      self->flags = (self->flags & ~0x0003) | AFSOCKET_STREAM;
    }
}

void
afsocket_sd_add_connection(AFSocketSourceDriver *self, AFSocketSourceConnection *connection)
{
  self->connections = g_list_prepend(self->connections,connection);
}

void
afsocket_sd_remove_and_kill_connection(AFSocketSourceDriver *self, AFSocketSourceConnection *connection)
{
  self->connections = g_list_remove(self->connections, connection);

  log_pipe_deinit(&connection->super);

  afsocket_sc_kill_connection(connection);
}

static void
afsocket_sd_kill_connection_list(GList *list)
{
  GList *l, *next;

  for (l = list; l; l = next)
    {
      AFSocketSourceConnection *connection = (AFSocketSourceConnection *) l->data;

      next = l->next;
      if (connection->owner)
        {
          afsocket_sd_remove_and_kill_connection(connection->owner, connection);
        }
      else
        {
          afsocket_sc_kill_connection(connection);
        }
    }
}

void
afsocket_sd_set_keep_alive(LogDriver *s, gint enable)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;

  if (enable)
    self->flags |= AFSOCKET_KEEP_ALIVE;
  else
    self->flags &= ~AFSOCKET_KEEP_ALIVE;
}

void
afsocket_sd_set_max_connections(LogDriver *s, gint max_connections)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;

  self->max_connections = max_connections;
}

#if ENABLE_SSL
void
afsocket_sd_set_tls_context(LogDriver *s, TLSContext *tls_context)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;

  self->tls_context = tls_context;
}
#endif

static inline gchar *
afsocket_sd_format_persist_name(AFSocketSourceDriver *self, gboolean listener_name)
{
  static gchar persist_name[128];
  gchar buf[64];

  g_snprintf(persist_name, sizeof(persist_name),
             listener_name ? "afsocket_sd_listen_fd(%s,%s)" : "afsocket_sd_connections(%s,%s)",
             !!(self->flags & AFSOCKET_STREAM) ? "stream" : "dgram",
             g_sockaddr_format(self->bind_addr, buf, sizeof(buf), GSA_FULL));
  return persist_name;
}

gboolean
afsocket_sd_process_connection(AFSocketSourceDriver *self, GSockAddr *client_addr, GSockAddr *local_addr, gint fd)
{
  gchar buf[MAX_SOCKADDR_STRING], buf2[MAX_SOCKADDR_STRING];
#if ENABLE_TCP_WRAPPER
  if (client_addr && (client_addr->sa.sa_family == AF_INET
#if ENABLE_IPV6
                   || client_addr->sa.sa_family == AF_INET6
#endif
     ))
    {
      struct request_info req;

      request_init(&req, RQ_DAEMON, "syslog-ng", RQ_FILE, fd, 0);
      fromhost(&req);
      if (hosts_access(&req) == 0)
        {

          msg_error("Syslog connection rejected by tcpd",
                    evt_tag_str("client", g_sockaddr_format(client_addr, buf, sizeof(buf), GSA_FULL)),
                    evt_tag_str("local", g_sockaddr_format(local_addr, buf2, sizeof(buf2), GSA_FULL)),
                    evt_tag_id(MSG_REJECT_CONNECTION),
                    NULL);
          return FALSE;
        }
    }

#endif

  if (self->num_connections >= self->max_connections)
    {
      msg_error("Number of allowed concurrent connections reached, rejecting connection",
                evt_tag_str("client", g_sockaddr_format(client_addr, buf, sizeof(buf), GSA_FULL)),
                evt_tag_str("local", g_sockaddr_format(local_addr, buf2, sizeof(buf2), GSA_FULL)),
                evt_tag_int("max", self->max_connections),
                evt_tag_id(MSG_REJECT_CONNECTION),
                NULL);
      return FALSE;
    }
  else
    {
      AFSocketSourceConnection *conn;

      conn = afsocket_sc_new(self, client_addr, fd);
      if (log_pipe_init(&conn->super, NULL))
        {
          afsocket_sd_add_connection(self,conn);
          self->num_connections++;
          log_pipe_append(&conn->super, &self->super.super.super);
        }
      else
        {
          afsocket_sd_remove_and_kill_connection(self, conn);
          return FALSE;
        }
    }
  return TRUE;
}

#define MAX_ACCEPTS_AT_A_TIME 30

static void
afsocket_sd_accept(gpointer s)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;
  GSockAddr *peer_addr;
  gchar buf1[256], buf2[256];
  gint new_fd;
  gboolean res;
  int accepts = 0;

  while (accepts < MAX_ACCEPTS_AT_A_TIME)
    {
      GIOStatus status;

      status = g_accept(self->fd, &new_fd, &peer_addr);
      if (status == G_IO_STATUS_AGAIN)
        {
          /* no more connections to accept */
          break;
        }
      else if (status != G_IO_STATUS_NORMAL)
        {
          msg_error("Error accepting new connection",
                    evt_tag_socket_error(EVT_TAG_OSERROR, getsockerror()),
                    NULL);
          return;
        }
      if (self->setup_socket && !self->setup_socket(self, new_fd))
        {
          closesocket(new_fd);
          return;
        }
#ifndef G_OS_WIN32
      g_fd_set_nonblock(new_fd, TRUE);
      g_fd_set_cloexec(new_fd, TRUE);
#endif

      res = afsocket_sd_process_connection(self, peer_addr, self->bind_addr, new_fd);

      if (res)
        {
          if (peer_addr->sa.sa_family != AF_UNIX)
            msg_notice("Syslog connection accepted",
                        evt_tag_int("fd", new_fd),
                        evt_tag_str("client", g_sockaddr_format(peer_addr, buf1, sizeof(buf1), GSA_FULL)),
                        evt_tag_str("local", g_sockaddr_format(self->bind_addr, buf2, sizeof(buf2), GSA_FULL)),
                        NULL);
          else
            msg_verbose("Syslog connection accepted",
                        evt_tag_int("fd", new_fd),
                        evt_tag_str("client", g_sockaddr_format(peer_addr, buf1, sizeof(buf1), GSA_FULL)),
                        evt_tag_str("local", g_sockaddr_format(self->bind_addr, buf2, sizeof(buf2), GSA_FULL)),
                        NULL);
        }
      else
        {
          closesocket(new_fd);
        }

      g_sockaddr_unref(peer_addr);
      accepts++;
    }
  return;
}

static void
afsocket_sd_close_connection(AFSocketSourceDriver *self, AFSocketSourceConnection *sc)
{
  gchar buf1[MAX_SOCKADDR_STRING], buf2[MAX_SOCKADDR_STRING];

  if (sc->peer_addr->sa.sa_family != AF_UNIX)
    msg_notice("Syslog connection closed",
               evt_tag_int("fd", sc->sock),
               evt_tag_str("client", g_sockaddr_format(sc->peer_addr, buf1, sizeof(buf1), GSA_FULL)),
               evt_tag_str("local", g_sockaddr_format(self->bind_addr, buf2, sizeof(buf2), GSA_FULL)),
               NULL);
  else
    msg_verbose("Syslog connection closed",
               evt_tag_int("fd", sc->sock),
               evt_tag_str("client", g_sockaddr_format(sc->peer_addr, buf1, sizeof(buf1), GSA_FULL)),
               evt_tag_str("local", g_sockaddr_format(self->bind_addr, buf2, sizeof(buf2), GSA_FULL)),
               NULL);

  /* Close the fd of the reader */
  log_reader_reopen(sc->reader, NULL, &sc->super, &self->reader_options, 1,  afsocket_sc_stats_source(sc), self->super.super.id, afsocket_sc_stats_instance(sc), FALSE, &self->proto_options);
  log_pipe_deinit(&sc->super);
  afsocket_sd_remove_and_kill_connection(self, sc);
  self->num_connections--;
}

static void
afsocket_sd_start_watches(AFSocketSourceDriver *self)
{
  IV_FD_INIT(&self->listen_fd);
  self->listen_fd.fd = self->fd;
  self->listen_fd.cookie = self;
  iv_fd_register(&self->listen_fd);
  iv_fd_set_handler_in(&self->listen_fd,afsocket_sd_accept);
}

static void
afsocket_sd_stop_watches(AFSocketSourceDriver *self)
{
  if (iv_fd_registered (&self->listen_fd))
    iv_fd_unregister(&self->listen_fd);
}

gboolean
afsocket_sd_init(LogPipe *s)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;
  LogProtoServerOptions *options = (LogProtoServerOptions *)&self->proto_options;

  gint sock;
  gboolean res = FALSE;
  GlobalConfig *cfg = log_pipe_get_config(s);

  log_proto_check_server_options((LogProtoServerOptions *)&self->proto_options);
  if (!log_src_driver_init_method(s))
    return FALSE;

  if (!afsocket_sd_apply_transport(self))
    return FALSE;

  g_assert(self->transport);
  g_assert(self->bind_addr);

  if ((self->flags & (AFSOCKET_STREAM + AFSOCKET_WNDSIZE_INITED)) == AFSOCKET_STREAM)
    {
      /* distribute the window evenly between each of our possible
       * connections.  This is quite pessimistic and can result in very low
       * window sizes. Increase that but warn the user at the same time
       */

      self->reader_options.super.init_window_size /= self->max_connections;
      if (self->reader_options.super.init_window_size < 100)
        {
          msg_warning("WARNING: window sizing for network sources has changed in " VERSION_3_3 ". The window size is the value of the log_iw_size() option divided by the value of max_connections(). If log_iw_size()/max_connections() is lower than 100, it is automatically increased to 100. Adjust the log_fifo_size() option if needed to avoid message loss.",
                      evt_tag_int("orig_window_size", self->reader_options.super.init_window_size),
                      evt_tag_int("new_window_size", 100),
                      evt_tag_int("min_log_fifo_size", 100 * self->max_connections),
                      NULL);
          self->reader_options.super.init_window_size = 100;
        }
      self->flags |= AFSOCKET_WNDSIZE_INITED;
    }
  log_reader_options_init(&self->reader_options, cfg, self->super.super.group);

  /* fetch persistent connections first */
  if ((self->flags & AFSOCKET_KEEP_ALIVE))
    {
      GList *p = self->connections = cfg_persist_config_fetch(cfg, afsocket_sd_format_persist_name(self, FALSE));

      self->num_connections = 0;
      while (p)
        {
          afsocket_sc_set_owner((AFSocketSourceConnection *) p->data, self);
          if (log_pipe_init((LogPipe *) p->data, NULL))
            {
              self->num_connections++;
              p = p->next;
            }
          else
            {
              AFSocketSourceConnection *sc = (AFSocketSourceConnection *)p->data;
              p = p->next;

              afsocket_sd_remove_and_kill_connection(self, sc);
            }
        }
   }

  /* ok, we have connection list, check if we need to open a listener */
  sock = -1;
  if (self->flags & AFSOCKET_STREAM)
    {
      if (self->flags & AFSOCKET_KEEP_ALIVE)
        {
          /* NOTE: this assumes that fd 0 will never be used for listening fds,
           * main.c opens fd 0 so this assumption can hold */
          sock = GPOINTER_TO_UINT(cfg_persist_config_fetch(cfg, afsocket_sd_format_persist_name(self, TRUE))) - 1;
        }

      if (sock == -1)
        {
          if (!afsocket_sd_acquire_socket(self, &sock))
            return self->super.super.optional;
          if (sock == -1 && !afsocket_open_socket(self->bind_addr, !!(self->flags & AFSOCKET_STREAM), &sock))
            return self->super.super.optional;
        }

      /* set up listening source */
      if (listen(sock, self->listen_backlog) < 0)
        {
          msg_error("Error during listen()",
                    evt_tag_socket_error(EVT_TAG_OSERROR, getsockerror()),
                    NULL);
          closesocket(sock);
          return FALSE;
        }

      if (self->setup_socket && !self->setup_socket(self, sock))
        {
          closesocket(sock);
          return FALSE;
        }

      self->fd = sock;
      afsocket_sd_start_watches(self);
      res = TRUE;
    }
  else
    {
      if (options->opts.prefix_pattern)
        msg_warning("UDP protocol and multi-line-prefix option together are not supported",evt_tag_str("source", self->super.super.group) , NULL);

      if (!self->connections)
        {
          if (!afsocket_sd_acquire_socket(self, &sock))
            return self->super.super.optional;
          if (sock == -1 && !afsocket_open_socket(self->bind_addr, !!(self->flags & AFSOCKET_STREAM), &sock))
            return self->super.super.optional;
        }
      self->fd = -1;

      if (!self->setup_socket(self, sock))
        {
          closesocket(sock);
          return FALSE;
        }

      /* we either have self->connections != NULL, or sock contains a new fd */
      if (self->connections || afsocket_sd_process_connection(self, NULL, self->bind_addr, sock))
        res = TRUE;
    }
  return res;
}

static void
afsocket_sd_close_fd(gpointer value)
{
  gint fd = GPOINTER_TO_UINT(value) - 1;
  closesocket(fd);
}

static void
afsocket_sd_regex_free(pcre *regex)
{
  if (regex)
    {
      pcre_free(regex);
    }
}

gboolean
afsocket_sd_deinit(LogPipe *s)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);
  LogProtoServerOptions *options = (LogProtoServerOptions *)&self->proto_options;

  if ((self->flags & AFSOCKET_KEEP_ALIVE) == 0 || !cfg->persist)
    {
      afsocket_sd_kill_connection_list(self->connections);
    }
  else
    {
      GList *p;

      /* for AFSOCKET_STREAM source drivers this is a list, for
       * AFSOCKET_DGRAM this is a single connection */

      for (p = self->connections; p; p = p->next)
        {
          log_pipe_deinit((LogPipe *) p->data);
        }
      cfg_persist_config_add(cfg, afsocket_sd_format_persist_name(self, FALSE), self->connections, (GDestroyNotify) afsocket_sd_kill_connection_list, FALSE);
    }
  self->connections = NULL;

  if (self->flags & AFSOCKET_STREAM)
    {
      afsocket_sd_stop_watches(self);
      if ((self->flags & AFSOCKET_KEEP_ALIVE) == 0)
        {
          msg_verbose("Closing listener fd",
                      evt_tag_int("fd", self->fd),
                      NULL);
          closesocket(self->fd);
        }
      else
        {
          /* NOTE: the fd is incremented by one when added to persistent config
           * as persist config cannot store NULL */

          cfg_persist_config_add(cfg, afsocket_sd_format_persist_name(self, TRUE), GUINT_TO_POINTER(self->fd + 1), afsocket_sd_close_fd, FALSE);
        }
    }
  else if (self->flags & AFSOCKET_DGRAM)
    {
      /* we don't need to close the listening fd here as we have a
       * single connection which will close it */

      ;
    }

  if (!log_src_driver_deinit_method(s))
    return FALSE;

  afsocket_sd_regex_free(options->opts.prefix_matcher);
  afsocket_sd_regex_free(options->opts.garbage_matcher);
  if (options->opts.prefix_pattern)
    g_free(options->opts.prefix_pattern);
  if (options->opts.garbage_pattern)
    g_free(options->opts.garbage_pattern);

  return TRUE;
}

static void
afsocket_sd_notify(LogPipe *s, LogPipe *sender, gint notify_code, gpointer user_data)
{
  switch (notify_code)
    {
    case NC_CLOSE:
    case NC_READ_ERROR:
      {
        g_assert_not_reached();
        break;
      }
    }
}

static gboolean
afsocket_sd_setup_socket(AFSocketSourceDriver *self, gint fd)
{
  return afsocket_setup_socket(fd, self->sock_options_ptr, AFSOCKET_DIR_RECV);
}

void
afsocket_sd_free(LogPipe *s)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;

  log_reader_options_destroy(&self->reader_options);
  g_sockaddr_unref(self->bind_addr);
  self->bind_addr = NULL;
  g_free(self->transport);
#if ENABLE_SSL
  if(self->tls_context)
    {
      tls_context_unref(self->tls_context);
    }
#endif

  log_src_driver_free(s);
}

void
afsocket_sd_init_instance(AFSocketSourceDriver *self, SocketOptions *sock_options, gint family, guint32 flags)
{
  log_src_driver_init_instance(&self->super);

  self->super.super.super.init = afsocket_sd_init;
  self->super.super.super.deinit = afsocket_sd_deinit;
  self->super.super.super.free_fn = afsocket_sd_free;
  self->super.super.super.notify = afsocket_sd_notify;
  self->sock_options_ptr = sock_options;
  self->setup_socket = afsocket_sd_setup_socket;
  self->address_family = family;
  self->max_connections = 10;
  self->listen_backlog = 255;
  self->flags = flags | AFSOCKET_KEEP_ALIVE;
  log_reader_options_defaults(&self->reader_options);
  if (self->flags & AFSOCKET_STREAM)
    self->reader_options.super.init_window_size = 1000;

  if (self->flags & AFSOCKET_LOCAL)
    {
      static gboolean warned = FALSE;

      self->reader_options.parse_options.flags |= LP_LOCAL;
      if (!cfg_check_current_config_version(VERSION_VALUE_3_2))
        {
          if (!warned)
            {
              msg_warning("WARNING: the expected message format is being changed for unix-domain transports to improve "
                          "syslogd compatibity with syslog-ng " VERSION_3_2 ". If you are using custom "
                          "applications which bypass the syslog() API, you might "
                          "need the 'expect-hostname' flag to get the old behaviour back", NULL);
              warned = TRUE;
            }
        }
      else
        {
          self->reader_options.parse_options.flags &= ~LP_EXPECT_HOSTNAME;
        }
    }
  if ((self->flags & AFSOCKET_SYSLOG_DRIVER))
    {
      self->reader_options.parse_options.flags |= LP_SYSLOG_PROTOCOL;
    }
}

/* socket destinations */

void
afsocket_dd_set_transport(LogDriver *s, const gchar *transport)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;

  if (self->transport)
    g_free(self->transport);
  self->transport = g_strdup(transport);
  if ((strlen(transport) >= 3 && strncasecmp(transport,"udp",3)==0) || (strlen(transport) >= 5 && strncasecmp(transport,"dgram",5)==0))
    {
      self->flags = (self->flags & ~0x0003) | AFSOCKET_DGRAM;
    }
  else
    {
      self->flags = (self->flags & ~0x0003) | AFSOCKET_STREAM;
    }
}

#if ENABLE_SSL
void
afsocket_dd_set_tls_context(LogDriver *s, TLSContext *tls_context)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;

  self->tls_context = tls_context;
}
#endif

gchar *
afsocket_dd_get_next_dest(AFSocketDestDriver *self)
{
  gchar *result=self->hostname;
  if(self->server_name_list)
  {
    if(self->server_name_list->next)
      {
        self->server_name_list = self->server_name_list->next;
      }
    else
      {
        self->server_name_list = g_list_first(self->server_name_list);
      }
    result = (gchar *)self->server_name_list->data;
  }
  return result;
}

void
afsocket_dd_add_failovers(LogDriver *s, GList *failover_list)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;
  if(!self->server_name_list)
    {
      self->server_name_list = g_list_append(self->server_name_list, self->hostname);
    }
  while(failover_list)
    {
      self->server_name_list = g_list_append(self->server_name_list, g_strdup(failover_list->data));
      g_free(failover_list->data);
      failover_list = g_list_delete_link(failover_list,failover_list);
    }
  self->server_name_list = g_list_first(self->server_name_list);
  self->hostname = self->server_name_list->data;
}

void
afsocket_dd_set_keep_alive(LogDriver *s, gint enable)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;

  if (enable)
    self->flags |= AFSOCKET_KEEP_ALIVE;
  else
    self->flags &= ~AFSOCKET_KEEP_ALIVE;
}


static gchar *
afsocket_dd_format_persist_name(AFSocketDestDriver *self, gboolean qfile)
{
  static gchar persist_name[128];

  g_snprintf(persist_name, sizeof(persist_name),
             qfile ? "afsocket_dd_qfile(%s,%s)" : "afsocket_dd_connection(%s,%s)",
             !!(self->flags & AFSOCKET_STREAM) ? "stream" : "dgram",
             self->dest_name);
  return persist_name;
}

static gchar *
afsocket_dd_format_state_name(AFSocketDestDriver *self)
{
  static gchar persist_name[1024];
  static gchar buf[256];
  getlonghostname(buf,sizeof(buf));
  g_snprintf(persist_name, sizeof(persist_name),
             "afsocket_dd_connection(%s,%s,%s)",
             !!(self->flags & AFSOCKET_STREAM) ? "stream" : "dgram",
             self->dest_name,
             buf);
  return persist_name;
}


static gint
afsocket_dd_stats_source(AFSocketDestDriver *self)
{
  gint source = 0;

  if ((self->flags & AFSOCKET_SYSLOG_DRIVER) == 0)
    {
      switch (self->dest_addr->sa.sa_family)
        {
        case AF_UNIX:
          source = !!(self->flags & AFSOCKET_STREAM) ? SCS_UNIX_STREAM : SCS_UNIX_DGRAM;
          break;
        case AF_INET:
          source = !!(self->flags & AFSOCKET_STREAM) ? SCS_TCP : SCS_UDP;
          break;
#if ENABLE_IPV6
        case AF_INET6:
          source = !!(self->flags & AFSOCKET_STREAM) ? SCS_TCP6 : SCS_UDP6;
          break;
#endif
        default:
          g_assert_not_reached();
          break;
        }
    }
  else
    {
      source = SCS_SYSLOG;
    }
  return source;
}

static gchar *
afsocket_dd_stats_instance(AFSocketDestDriver *self)
{
  if ((self->flags & AFSOCKET_SYSLOG_DRIVER) == 0)
    {
      return self->dest_name;
    }
  else
    {
      static gchar buf[256];

      g_snprintf(buf, sizeof(buf), "%s,%s", self->transport, self->dest_name);
      return buf;
    }
}

#if ENABLE_SSL
static gint
afsocket_dd_tls_verify_callback(gint ok, X509_STORE_CTX *ctx, gpointer user_data)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) user_data;

  if (ok && ctx->current_cert == ctx->cert && self->hostname && (self->tls_context->verify_mode & TVM_TRUSTED))
    {
      ok = tls_verify_certificate_name(ctx->cert, self->hostname);
    }

  return ok;
}
#endif

static void afsocket_dd_connected(AFSocketDestDriver *self);
static void afsocket_dd_reconnect(AFSocketDestDriver *self);

static void
afsocket_dd_init_watches(AFSocketDestDriver *self)
{
  IV_FD_INIT(&self->connect_fd);
  self->connect_fd.cookie = self;
}

static void
afsocket_dd_start_watches(AFSocketDestDriver *self)
{
  main_loop_assert_main_thread();

  self->connect_fd.fd = self->fd;
  iv_fd_register(&self->connect_fd);
  iv_fd_set_handler_out(&self->connect_fd,(void (*)(void *))afsocket_dd_connected);
}

static void
afsocket_dd_stop_watches(AFSocketDestDriver *self)
{
  main_loop_assert_main_thread();

  if (iv_fd_registered(&self->connect_fd))
    {
      iv_fd_unregister(&self->connect_fd);

      /* need to close the fd in this case as it wasn't established yet */
      msg_verbose("Closing connecting fd",
                  evt_tag_int("fd", self->fd),
                  NULL);
      closesocket(self->fd);
    }
}

static void
afsocket_dd_connected(AFSocketDestDriver *self)
{
  gchar buf1[256], buf2[256];
  int error = 0;
  socklen_t errorlen = sizeof(error);
  LogTransport *transport;
  LogProto *proto;
  guint32 transport_flags = 0;
  GlobalConfig *cfg = log_pipe_get_config((LogPipe *)self);

  main_loop_assert_main_thread();

  if (iv_fd_registered(&self->connect_fd))
    iv_fd_unregister(&self->connect_fd);
  transport_flags |= LTF_SOCKET;
  if (self->flags & AFSOCKET_STREAM)
    {
      transport_flags |= LTF_SHUTDOWN;
      if (getsockopt(self->fd, SOL_SOCKET, SO_ERROR, (char *) &error, &errorlen) == -1)
        {
          errno = getsockerror();
          msg_error("getsockopt(SOL_SOCKET, SO_ERROR) failed for connecting socket",
                    evt_tag_int("fd", self->fd),
                    evt_tag_str("server", g_sockaddr_format(self->dest_addr, buf2, sizeof(buf2), GSA_FULL)),
                    evt_tag_socket_error(EVT_TAG_OSERROR, errno),
                    evt_tag_int("time_reopen", self->time_reopen),
                    evt_tag_id(MSG_SYSLOG_SERVER_CONNECTION_FAILED),
                    NULL);
          goto error_reconnect;
        }
      if (error)
        {
           msg_error("Syslog connection failed",
                    evt_tag_int("fd", self->fd),
                    evt_tag_str("server", g_sockaddr_format(self->dest_addr, buf2, sizeof(buf2), GSA_FULL)),
                    evt_tag_socket_error(EVT_TAG_OSERROR, error),
                    evt_tag_int("time_reopen", self->time_reopen),
                    evt_tag_id(MSG_SYSLOG_SERVER_CONNECTION_FAILED),
                    NULL);
          goto error_reconnect;
        }
    }
  msg_notice("Syslog connection established",
              evt_tag_int("fd", self->fd),
              evt_tag_str("server", g_sockaddr_format(self->dest_addr, buf2, sizeof(buf2), GSA_FULL)),
              evt_tag_str("local", g_sockaddr_format(self->bind_addr, buf1, sizeof(buf1), GSA_FULL)),
              evt_tag_id(MSG_SERVER_CONNECTION_ESTABLISHED),
              NULL);


  if (self->proto_factory->construct_transport)
    {
      transport = self->proto_factory->construct_transport(&self->proto_options,self->fd,transport_flags, self->tls_context);
    }
  else
    {
#if ENABLE_SSL
      if (self->tls_context)
        {
          TLSSession *tls_session;

          tls_session = tls_context_setup_session(self->tls_context, cfg);
          if (!tls_session)
            {
              goto error_reconnect;
            }

          log_pipe_ref(&self->super.super.super);
          tls_session_set_verify(tls_session, afsocket_dd_tls_verify_callback, self, (GDestroyNotify)log_pipe_unref);
          transport = log_transport_tls_new(tls_session, self->fd, transport_flags);
        }
      else
#endif
        transport = log_transport_plain_new(self->fd, transport_flags);
    }
  if (!transport)
    {
      goto error_reconnect;
    }

  self->proto_options.super.size = self->writer_options.flush_lines;
  proto = self->proto_factory->create(transport,&self->proto_options, log_pipe_get_config(&self->super.super.super));
  if (!proto)
    {
      goto error_reconnect;
    }
  log_proto_set_keep_one_message(proto, TRUE);
  log_proto_restart_with_state(proto,cfg->state,afsocket_dd_format_state_name(self));
  log_writer_reopen(self->writer, proto, &self->proto_options);
  return;
 error_reconnect:
  closesocket(self->fd);
  self->fd = -1;
  log_writer_reopen(self->writer, NULL, NULL);
  return;
}

static gboolean
afsocket_dd_start_connect(AFSocketDestDriver *self)
{
  int sock, rc;
  gchar buf1[MAX_SOCKADDR_STRING], buf2[MAX_SOCKADDR_STRING];

  main_loop_assert_main_thread();
  if (!afsocket_open_socket(self->bind_addr, !!(self->flags & AFSOCKET_STREAM), &sock))
    {
      return FALSE;
    }

  if (self->setup_socket && !self->setup_socket(self, sock))
    {
      closesocket(sock);
      return FALSE;
    }

  rc = g_connect(sock, self->dest_addr);
  if (rc == G_IO_STATUS_NORMAL)
    {
      self->fd = sock;
      afsocket_dd_connected(self);
    }
  else if (rc == G_IO_STATUS_ERROR && errno == EINPROGRESS)
    {
      /* we must wait until connect succeeds */

      self->fd = sock;
      afsocket_dd_start_watches(self);
    }
#ifdef _WIN32
  else if (rc == G_IO_STATUS_AGAIN)
    {
      self->fd = sock;
      afsocket_dd_start_watches(self);
    }
#endif
  else
    {
      /* error establishing connection */
      errno = getsockerror();
      msg_error("Connection failed",
                evt_tag_int("fd", sock),
                evt_tag_str("server", g_sockaddr_format(self->dest_addr, buf2, sizeof(buf2), GSA_FULL)),
                evt_tag_str("local", g_sockaddr_format(self->bind_addr, buf1, sizeof(buf1), GSA_FULL)),
                evt_tag_socket_error(EVT_TAG_OSERROR, errno),
                evt_tag_id(MSG_SYSLOG_SERVER_CONNECTION_FAILED),
                NULL);
      closesocket(sock);
      return FALSE;
    }

  return TRUE;
}

static void
afsocket_dd_reconnect(AFSocketDestDriver *self)
{
  if (!afsocket_dd_start_connect(self))
    {
      log_writer_reopen(self->writer, NULL, NULL);
    }
}

gboolean
afsocket_dd_check_persist_usage(AFSocketDestDriver *self, const gchar *persist_name)
{
  LogPipe *writer = g_hash_table_lookup(persist_hash, persist_name);
  if (!writer)
    {
      return TRUE;
    }
  else if (writer != self->writer)
    {
      return FALSE;
    }
  return TRUE;
}

gboolean
afsocket_dd_init(LogPipe *s)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);
  gchar *persist_name;
  AFSocketReloadInfo *reload_info;

  if (!log_dest_driver_init_method(s))
    return FALSE;

  if (!afsocket_dd_apply_transport(self))
    return FALSE;

  /* these fields must be set up by apply_transport, so let's check if it indeed did */
  g_assert(self->transport);
  g_assert(self->bind_addr);
  g_assert(self->dest_addr);
  g_assert(self->hostname);
  g_assert(self->dest_name);

  if (cfg)
    {
      self->time_reopen = cfg->time_reopen;
    }

  /* NOTE: we try to keep our LogWriter instance persistent as that
   * contains a reference to our underlying connection. This way
   * the connection is not closed when syslog-ng is reloaded.
   */
  log_writer_options_init(&self->writer_options, cfg, 0);
  reload_info = cfg_persist_config_fetch(cfg, afsocket_dd_format_persist_name(self, FALSE));

  if (reload_info)
    {
      if (reload_info->proto_factory->create == self->proto_factory->create)
        {
          self->writer = reload_info->writer;
        }
      else
        {
          log_pipe_unref(reload_info->writer);
        }

      g_free(reload_info);
    }

  if (!self->writer)
    {
      /* NOTE: we open our writer with no fd, so we can send messages down there
       * even while the connection is not established */

        self->writer = log_writer_new(LW_FORMAT_PROTO | LW_KEEP_ONE_PENDING |
#if ENABLE_SSL
                                    (((self->flags & AFSOCKET_STREAM) && !self->tls_context) ? LW_DETECT_EOF : 0) |
#else
                                    ((self->flags & AFSOCKET_STREAM) ? LW_DETECT_EOF : 0) |
#endif
                                    (self->flags & AFSOCKET_SYSLOG_DRIVER ? LW_SYSLOG_PROTOCOL : 0));

    }
  log_writer_set_options((LogWriter *) self->writer,
                          &self->super.super.super,
                          &self->writer_options,
                          0,
                          afsocket_dd_stats_source(self),
                          self->super.super.id,
                          afsocket_dd_stats_instance(self),
                          &self->proto_options);

  persist_name =  afsocket_dd_format_persist_name(self, TRUE);
  if (!afsocket_dd_check_persist_usage(self, persist_name))
    {
      msg_error("Persist name is already used", evt_tag_str("persist_name", persist_name), NULL);
      return FALSE;
    }
  g_hash_table_insert(persist_hash, g_strdup(persist_name), self->writer);
  log_writer_set_queue(self->writer, log_dest_driver_acquire_queue(&self->super, persist_name));

  if (!log_pipe_init(self->writer, NULL))
    {
      log_pipe_unref(self->writer);
      return FALSE;
    }
  log_pipe_append(&self->super.super.super, self->writer);

  if (!log_writer_opened((LogWriter *) self->writer))
    afsocket_dd_reconnect(self);
  return TRUE;
}

gboolean
afsocket_dd_deinit(LogPipe *s)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;
  GlobalConfig *cfg = log_pipe_get_config(s);
  AFSocketReloadInfo *reload_info;

  afsocket_dd_stop_watches(self);

  if (self->writer)
    log_pipe_deinit(self->writer);

  if (self->flags & AFSOCKET_KEEP_ALIVE)
    {
      reload_info = g_new(AFSocketReloadInfo, 1);
      reload_info->writer = self->writer;
      reload_info->proto_factory = self->proto_factory;

      cfg_persist_config_add(cfg, afsocket_dd_format_persist_name(self, FALSE), reload_info, (GDestroyNotify) afsocket_reload_info_free, FALSE);
      self->writer = NULL;
    }

  g_hash_table_remove(persist_hash, afsocket_dd_format_persist_name(self, TRUE));

  if (!log_dest_driver_deinit_method(s))
    return FALSE;

  return TRUE;
}

static void
afsocket_dd_notify(LogPipe *s, LogPipe *sender, gint notify_code, gpointer user_data)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;
  gchar buf[MAX_SOCKADDR_STRING];

  switch (notify_code)
    {
    case NC_CLOSE:
    case NC_WRITE_ERROR:
      msg_notice("Syslog connection broken",
                 evt_tag_int("fd", self->fd),
                 evt_tag_str("server", g_sockaddr_format(self->dest_addr, buf, sizeof(buf), GSA_FULL)),
                 evt_tag_int("time_reopen", self->time_reopen),
                 evt_tag_id(MSG_SERVER_CONNECTION_BROKEN),
                 NULL);
      log_writer_reopen(self->writer, NULL, NULL);
      break;

    case NC_REOPEN_REQUIRED:
      msg_notice("Syslog try to reconnect",
                evt_tag_int("fd", self->fd),
                evt_tag_str("server", g_sockaddr_format(self->dest_addr, buf, sizeof(buf), GSA_FULL)),
                evt_tag_int("time_reopen", self->time_reopen),
                evt_tag_id(MSG_SERVER_TRY_RECONNECT),
                NULL);
      if (self->dest_addr->sa.sa_family != AF_UNIX)
        {
          // Try to connect to the next server
          gchar *old_server = g_strdup(self->hostname);
          self->hostname = afsocket_dd_get_next_dest(self);
          resolve_hostname(&self->dest_addr, self->hostname);
          if (strcmp(old_server, self->hostname) == 0)
            /* no failover server */
            msg_warning("The server is unaccessible, trying again the host",
                        evt_tag_str("host", self->hostname),
                        evt_tag_id(MSG_SERVER_UNACCESSIBLE),
                        NULL);
          else
            msg_warning("Current server is unaccessible, switching to a failover host",
                      evt_tag_str("current", old_server),
                      evt_tag_str("failover", self->hostname),
                      evt_tag_id(MSG_SERVER_UNACCESSIBLE),
                      NULL);
          g_free(old_server);
        }
      afsocket_dd_reconnect(self);
      break;
    }
}

static gboolean
afsocket_dd_setup_socket(AFSocketDestDriver *self, gint fd)
{
  return afsocket_setup_socket(fd, self->sock_options_ptr, AFSOCKET_DIR_SEND);
}

void
afsocket_dd_free(LogPipe *s)
{
  AFSocketDestDriver *self = (AFSocketDestDriver *) s;
  if (self->server_name_list)
    {
      while (self->server_name_list)
        {
          g_free(self->server_name_list->data);
          self->server_name_list = g_list_delete_link(self->server_name_list, self->server_name_list);
        }
    }
  else
    {
      g_free(self->hostname);
    }

  log_writer_options_destroy(&self->writer_options);
  g_sockaddr_unref(self->bind_addr);
  g_sockaddr_unref(self->dest_addr);
  log_pipe_unref(self->writer);
  g_free(self->dest_name);
  g_free(self->transport);
#if ENABLE_SSL
  if(self->tls_context)
    {
      tls_context_unref(self->tls_context);
    }
#endif
  g_hash_table_unref(persist_hash);
  log_dest_driver_free(s);
}

void
afsocket_dd_init_instance(AFSocketDestDriver *self, SocketOptions *sock_options, gint family, const gchar *hostname, guint32 flags)
{
  log_dest_driver_init_instance(&self->super);
  if (!persist_hash)
    {
      persist_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
  else
    {
      g_hash_table_ref(persist_hash);
    }

  log_writer_options_defaults(&self->writer_options);
  self->super.super.super.init = afsocket_dd_init;
  self->super.super.super.deinit = afsocket_dd_deinit;
  self->super.super.super.free_fn = afsocket_dd_free;
  self->super.super.super.notify = afsocket_dd_notify;
  self->setup_socket = afsocket_dd_setup_socket;
  self->sock_options_ptr = sock_options;
  self->address_family = family;
  self->flags = flags  | AFSOCKET_KEEP_ALIVE;

  self->hostname = g_strdup(hostname);

  self->writer_options.mark_mode = MM_GLOBAL;
  afsocket_dd_init_watches(self);
}

gboolean
afsocket_sd_set_multi_line_prefix(LogDriver *s, gchar *prefix)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;
  LogProtoServerOptions *options = (LogProtoServerOptions *)&self->proto_options;
  const gchar *error;
  gint erroroffset;
  /*Are we need any options?*/
  int pcreoptions = PCRE_EXTENDED;

  if (options->opts.prefix_pattern)
    g_free(options->opts.prefix_pattern);
  options->opts.prefix_pattern = g_strdup(prefix);
  options->opts.prefix_matcher = pcre_compile(prefix, pcreoptions, &error, &erroroffset, NULL);
  if (!options->opts.prefix_matcher)
    {
      msg_error("Bad regexp",evt_tag_str("multi_line_prefix", prefix), evt_tag_id(MSG_BAD_REGEXP), NULL);
      return FALSE;
    }

  return TRUE;
}

gboolean
afsocket_sd_set_multi_line_garbage(LogDriver *s, gchar *garbage)
{
  AFSocketSourceDriver *self = (AFSocketSourceDriver *) s;
  LogProtoServerOptions *options = (LogProtoServerOptions *)&self->proto_options;
  const gchar *error;
  gint erroroffset;
  /*Are we need any options?*/
  int pcreoptions = PCRE_EXTENDED;

  if (options->opts.garbage_pattern)
    g_free(options->opts.garbage_pattern);
  options->opts.garbage_pattern = g_strdup(garbage);
  options->opts.garbage_matcher = pcre_compile(garbage, pcreoptions, &error, &erroroffset, NULL);
  if (!options->opts.garbage_matcher)
    {
      msg_error("Bad regexp",evt_tag_str("multi_line_garbage", garbage), evt_tag_id(MSG_BAD_REGEXP), NULL);
      return FALSE;
    }

  return TRUE;
}
