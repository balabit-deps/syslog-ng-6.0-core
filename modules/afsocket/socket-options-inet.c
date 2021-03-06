/*
 * Copyright (c) 2002-2013 Balabit
 * Copyright (c) 1998-2013 Balázs Scheidler
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
#include "socket-options-inet.h"
#include "gsockaddr.h"
#include "messages.h"

#include <string.h>
#ifndef __WIN32
#include <netinet/tcp.h>
#endif

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

#ifndef SOL_IPV6
#define SOL_IPV6 IPPROTO_IPV6
#endif

#ifndef SOL_TCP
#define SOL_TCP IPPROTO_TCP
#endif

static gboolean
socket_options_inet_setup_socket(SocketOptions *s, gint fd, GSockAddr *addr, AFSocketDirection dir)
{
  SocketOptionsInet *self = (SocketOptionsInet *) s;
  gint off = 0;

  if (!socket_options_setup_socket_method(s, fd, addr, dir))
    return FALSE;

  switch (addr->sa.sa_family)
    {
    case AF_INET:
    {
      struct ip_mreq mreq;

      if (IN_MULTICAST(ntohl(g_sockaddr_inet_get_address(addr).s_addr)))
        {
          if (dir & AFSOCKET_DIR_RECV)
            {
              memset(&mreq, 0, sizeof(mreq));
              mreq.imr_multiaddr = g_sockaddr_inet_get_address(addr);
              mreq.imr_interface.s_addr = INADDR_ANY;
              setsockopt(fd, SOL_IP, IP_ADD_MEMBERSHIP, (char *)&mreq, sizeof(mreq));
              setsockopt(fd, SOL_IP, IP_MULTICAST_LOOP, (char *)&off, sizeof(off));
            }
          if (dir & AFSOCKET_DIR_SEND)
            {
              if (self->ttl)
                setsockopt(fd, SOL_IP, IP_MULTICAST_TTL, (char *)&self->ttl, sizeof(self->ttl));
            }

        }
      else
        {
          if (self->ttl && (dir & AFSOCKET_DIR_SEND))
            setsockopt(fd, SOL_IP, IP_TTL, (char *)&self->ttl, sizeof(self->ttl));
        }
      if (self->tos && (dir & AFSOCKET_DIR_SEND))
        setsockopt(fd, SOL_IP, IP_TOS, (char *)&self->tos, sizeof(self->tos));

      break;
    }
#if ENABLE_IPV6
    case AF_INET6:
    {
      struct ipv6_mreq mreq6;

      if (IN6_IS_ADDR_MULTICAST(&g_sockaddr_inet6_get_sa(addr)->sin6_addr))
        {
          if (dir & AFSOCKET_DIR_RECV)
            {
              memset(&mreq6, 0, sizeof(mreq6));
              mreq6.ipv6mr_multiaddr = *g_sockaddr_inet6_get_address(addr);
              mreq6.ipv6mr_interface = 0;
              setsockopt(fd, SOL_IPV6, IPV6_JOIN_GROUP, (char *)&mreq6, sizeof(mreq6));
              setsockopt(fd, SOL_IPV6, IPV6_MULTICAST_LOOP, (char *)&off, sizeof(off));
            }
          if (dir & AFSOCKET_DIR_SEND)
            {
              if (self->ttl)
                setsockopt(fd, SOL_IPV6, IPV6_MULTICAST_HOPS, (char *)&self->ttl, sizeof(self->ttl));
            }
        }
      else
        {
          if (self->ttl && (dir & AFSOCKET_DIR_SEND))
            setsockopt(fd, SOL_IPV6, IPV6_UNICAST_HOPS, (char *)&self->ttl, sizeof(self->ttl));
        }
      break;
    }
#endif
    }
  return TRUE;
}

SocketOptionsInet *
socket_options_inet_new_instance(void)
{
  SocketOptionsInet *self = g_new0(SocketOptionsInet, 1);

  socket_options_init_instance(&self->super);
  self->super.setup_socket = socket_options_inet_setup_socket;
  self->super.so_keepalive = TRUE;
  return self;
}

SocketOptions *
socket_options_inet_new(void)
{
  return &socket_options_inet_new_instance()->super;
}
