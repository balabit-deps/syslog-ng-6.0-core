/*
 * Copyright (c) 2002-2011 Balabit
 * Copyright (c) 1998-2011 Balázs Scheidler
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

/* The following copyright notice applies to strcasestr() */
/*-
 * Copyright (c) 1990, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Chris Torek.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by the University of
 *      California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * The following copyright notice applies to
 * conv_num(), find_string() and strptime()
 */

/*-
 * Copyright (c) 1997, 1998, 2005, 2008 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code was contributed to The NetBSD Foundation by Klaus Klein.
 * Heavily optimised by David Laight
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "compat.h"
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <glib.h>
#include <sys/socket.h>
#include <netdb.h>

#ifndef SSIZE_MAX
#define SSIZE_MAX SHRT_MAX
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <locale.h>
#include <stdint.h>
#include <iv_tls.h>

#define _ctloc(x)       (_CurrentTimeLocale->x)
/*
 * We do not implement alternate representations. However, we always
 * check whether a given modifier is allowed for a certain conversion.
 */
#define ALT_E           0x01
#define ALT_O           0x02
#define LEGAL_ALT(x)    { if (alt_format & ~(x)) return NULL; }

#define TM_YEAR_BASE   1900
#define IsLeapYear(x)   ((x % 4 == 0) && (x % 100 != 0 || x % 400 == 0))

typedef struct
{
  const char *abday[7];
  const char *day[7];
  const char *abmon[12];
  const char *mon[12];
  const char *am_pm[2];
  const char *d_t_fmt;
  const char *d_fmt;
  const char *t_fmt;
  const char *t_fmt_ampm;
} _TimeLocale;

static const _TimeLocale _DefaultTimeLocale =
{
  {
    "Sun","Mon","Tue","Wed","Thu","Fri","Sat",
  },
  {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
    "Friday", "Saturday"
  },
  {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  },
  {
    "January", "February", "March", "April", "May", "June", "July",
    "August", "September", "October", "November", "December"
  },
  {
    "AM", "PM"
  },
  "%a %b %d %H:%M:%S %Y",
  "%m/%d/%y",
  "%H:%M:%S",
  "%I:%M:%S %p"
};

static const _TimeLocale *_CurrentTimeLocale = &_DefaultTimeLocale;
static const char gmt[4] = { "GMT" };

ssize_t
readv(int fd, const struct iovec *vector, int count)
{
  size_t byte_size = 0;
  int i = 0;
  char *buffer = NULL;
  size_t to_copy = 0;
  char *data = NULL;
  ssize_t byte_read = 0;

  for ( i = 0; i < count; ++i)
    {
      if (SSIZE_MAX - byte_size < vector[i].iov_len)
        {
          SetLastError(EINVAL);
	  return -1;
        }
      byte_size += vector[i].iov_len;
    }

  buffer = _malloca(byte_size);

  byte_read = read(fd, buffer, byte_size);

  if(byte_read < 0)
    return -1;

  to_copy = byte_read;
  data = buffer;

  for (i = 0; i < count; ++i)
    {
       size_t copy = min (vector[i].iov_len, to_copy);
       memcpy(vector[i].iov_base, data, copy);
       to_copy -= copy;
       data -= copy;

       if (to_copy == 0)
         break;
    }
  _freea(buffer);
  return byte_read;
}

ssize_t
writev(int fd, const struct iovec *vector, int count)
{
  size_t byte_size = 0;
  int i = 0;
  char *buffer = NULL;
  size_t to_copy = 0;
  char *ret = NULL;
  ssize_t bytes_written = 0;

  for ( i = 0; i < count; ++i)
    {
      if (SSIZE_MAX - byte_size < vector[i].iov_len)
        {
          SetLastError(EINVAL);
	  return -1;
        }
      byte_size += vector[i].iov_len;
    }

  buffer = _malloca(byte_size);

  to_copy = byte_size;
  ret = buffer;

  for (i = 0; i < count; ++i)
    {
       size_t copy = min (vector[i].iov_len, to_copy);
       ret = memcpy(ret, vector[i].iov_base, copy);
       to_copy -= copy;
       ret += copy;
       if (to_copy == 0)
         break;
    }

  bytes_written = write(fd, buffer, byte_size);
  _freea(buffer);
  return bytes_written;
}

static off_t
_get_aligned_offset(off_t unaligned_offset)
{
  static off_t alignment = 0;
  if (alignment == 0)
    {
      SYSTEM_INFO si;
      GetSystemInfo(&si);
      alignment = si.dwAllocationGranularity;
    }
  return (unaligned_offset / alignment) * alignment;
}

static int
_map_windows_error(DWORD error)
{
  int result = 0;
  switch(error)
    {
      case ERROR_DISK_FULL:
        result = ENOSPC;
        break;
      case ERROR_ACCESS_DENIED:
        result = EACCES;
        break;
      case ERROR_ALREADY_EXISTS:
        result = EEXIST;
        break;
      case ERROR_UNKNOWN_REVISION:
        result = EPERM;
        break;
      case ERROR_NOT_ENOUGH_MEMORY:
        result = ENOMEM;
        break;
      case ERROR_SUCCESS:
        result = 0;
        break;
      case ERROR_INVALID_PARAMETER:
      case ERROR_INVALID_HANDLE:
      case ERROR_MAPPED_ALIGNMENT:
      case ERROR_INVALID_ADDRESS:
      default:
        result = EINVAL;
        break;
    }
  return result;
}

void *
mmap(void *addr, size_t len, int prot, int flags,
       int fildes, off_t off)
{
  LARGE_INTEGER offset;
  DWORD flProtect = 0;
  DWORD dwDesiredAccess = 0;
  HANDLE file_mapping;
  void *ret_addr;
  off_t aligned_offset = _get_aligned_offset(off);
  offset.QuadPart = aligned_offset;

  switch (prot)
   {
     case PROT_READ:
      flProtect = PAGE_READONLY;
      dwDesiredAccess = FILE_MAP_READ;
     break;
     case PROT_READ | PROT_WRITE:
      flProtect = PAGE_READWRITE;
      dwDesiredAccess = FILE_MAP_ALL_ACCESS;
     break;
     case PROT_NONE:
      flProtect = PAGE_NOACCESS;
     break;
     case PROT_EXEC:
      flProtect = PAGE_EXECUTE;
     break;
     case PROT_EXEC | PROT_READ:
      flProtect = PAGE_EXECUTE_READ;
     break;
     case PROT_EXEC | PROT_READ | PROT_WRITE:
      flProtect = PAGE_EXECUTE_READWRITE;
      dwDesiredAccess = FILE_MAP_ALL_ACCESS;
     break;
   };

  HANDLE hFile = (HANDLE)_get_osfhandle(fildes);
  if (hFile == INVALID_HANDLE_VALUE)
    {
      return MAP_FAILED;
    }

  file_mapping = CreateFileMapping(hFile, NULL, flProtect, 0, 0, NULL);

  if (file_mapping == NULL)
    goto error;

  ret_addr = MapViewOfFile(file_mapping, dwDesiredAccess, offset.HighPart, offset.LowPart, len + (off - aligned_offset));
  if (ret_addr == NULL)
    goto error;

  CloseHandle(file_mapping);
  return ret_addr + (off - aligned_offset);

error:
  errno = _map_windows_error(GetLastError());
  return MAP_FAILED;
}

int
munmap(void *addr, size_t len)
{
  off_t add_off = GPOINTER_TO_SIZE(addr);
  off_t aligned_offset = _get_aligned_offset(add_off);

  if (!UnmapViewOfFile(GSIZE_TO_POINTER(aligned_offset)))
    {
      errno = _map_windows_error(GetLastError());
      return -1;
    }
  return 0;
}

int
madvise(void *addr, size_t len, int advice)
{
  return TRUE;
}

int
fsync (int fd)
{
  HANDLE h = (HANDLE) _get_osfhandle (fd);
  DWORD err;

  if (h == INVALID_HANDLE_VALUE)
    {
      errno = EBADF;
      return -1;
    }

  if (!FlushFileBuffers (h))
    {
      /* Translate some Windows errors into rough approximations of Unix
       * errors.  MSDN is useless as usual - in this case it doesn't
       * document the full range of errors.
       */
      err = GetLastError ();
      switch (err)
       {
         /* eg. Trying to fsync a tty. */
       case ERROR_INVALID_HANDLE:
         errno = EINVAL;
         break;

       default:
         errno = EIO;
       }
      return -1;
    }

  return 0;
}

int chown(const char *path, uid_t owner, gid_t group)
{
  return 0;
}

int fchown(int fd, uid_t owner, gid_t group)
{
  return 0;
}

int fchmod(int fd, mode_t mode)
{
  return 0;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
  return 0;
}

int sigemptyset(sigset_t *set)
{
  return 0;
}

int sigaddset(sigset_t *set, int signum)
{
  return 0;
}


int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
  return 0;
}


#define MAX_SLEEP_IN_MS         4294967294UL

#define POW10_2     INT64_C(100)
#define POW10_3     INT64_C(1000)
#define POW10_4     INT64_C(10000)
#define POW10_6     INT64_C(1000000)
#define POW10_7     INT64_C(10000000)
#define POW10_9     INT64_C(1000000000)


int nanosleep(const struct timespec *request, struct timespec *remain)
{
    unsigned long ms, rc = 0;
    unsigned __int64 u64, want, real;

    union {
        unsigned __int64 ns100;
        FILETIME ft;
    }  _start, _end;

    if (request->tv_sec < 0 || request->tv_nsec < 0 || request->tv_nsec >= POW10_9) {
        errno = EINVAL;
        return -1;
    }

    if (remain != NULL) GetSystemTimeAsFileTime(&_start.ft);

    want = u64 = request->tv_sec * POW10_3 + request->tv_nsec / POW10_6;
    while (u64 > 0 && rc == 0) {
        if (u64 >= MAX_SLEEP_IN_MS) ms = MAX_SLEEP_IN_MS;
        else ms = (unsigned long) u64;

        u64 -= ms;
        rc = SleepEx(ms, TRUE);
    }

    if (rc != 0) { /* WAIT_IO_COMPLETION */
        if (remain != NULL) {
            GetSystemTimeAsFileTime(&_end.ft);
            real = (_end.ns100 - _start.ns100) / POW10_4;

            if (real >= want) u64 = 0;
            else u64 = want - real;

            remain->tv_sec = u64 / POW10_3;
            remain->tv_nsec = (long) (u64 % POW10_3) * POW10_6;
        }

        errno = EINTR;
        return -1;
    }

    return 0;
}

/* getpagesize for windows */
long
getpagesize (void)
{
    static long g_pagesize = 0;
    if (! g_pagesize) {
        SYSTEM_INFO system_info;
        GetSystemInfo (&system_info);
        g_pagesize = system_info.dwPageSize;
    }
    return g_pagesize;
}

#if (_WIN32_WINNT < 0x0600)
const char *
inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
  return compat_inet_ntop(af, src, dst, size);
}

int
inet_pton(int af, const char *src, void *dst)
{
  return compat_inet_pton(af, src, dst);
}

#endif /*_WIN32_WINNT < 0x0600*/

int
inet_aton(const char *cp, struct in_addr *dst)
{
  int s = 0;

  s = inet_addr(cp);
  if (s == INADDR_NONE)
    return 0;

  dst->s_addr = s;
  return 1;
}

static const u_char *
conv_num(const unsigned char *buf, int *dest, unsigned int llim, unsigned int ulim)
{
    unsigned int result = 0;
    unsigned char ch;
    unsigned int rulim = ulim;

    ch = *buf;
    if (ch < '0' || ch > '9')
      return NULL;

    do
      {
        result *= 10;
        result += ch - '0';
        rulim /= 10;
        ch = *++buf;
      }
    while ((result * 10 <= ulim) && rulim && ch >= '0' && ch <= '9');

    if (result < llim || result > ulim)
      return NULL;

    *dest = result;
    return buf;
}

static const u_char *
find_string(const u_char *bp, int *tgt, const char * const *n1, const char * const *n2, int c)
{
    int i;
    unsigned int len;

    for (; n1 != NULL; n1 = n2, n2 = NULL)
      {
        for (i = 0; i < c; i++, n1++)
	  {
            len = strlen(*n1);
            if (strncasecmp(*n1, (const char *)bp, len) == 0)
	      {
                *tgt = i;
                return bp + len;
              }
          }
      }

    return NULL;
}


char *
strptime(const char *buf, const char *fmt, struct tm *tm)
{
  unsigned char c;
  const unsigned char *bp;
  int alt_format, i, split_year = 0;
  const char *new_fmt;

  bp = (const u_char *)buf;

  while (bp != NULL && (c = *fmt++) != '\0')
    {
      alt_format = 0;
      i = 0;

      if (isspace(c))
        {
          while (isspace(*bp))
            bp++;
          continue;
        }

      if (c != '%')
        goto literal;
again:
        switch (c = *fmt++)
	  {
            case '%':
literal:
              if (c != *bp++)
                return NULL;
              LEGAL_ALT(0);
              continue;

            case 'E':
              LEGAL_ALT(0);
              alt_format |= ALT_E;
              goto again;

            case 'O':
              LEGAL_ALT(0);
              alt_format |= ALT_O;
              goto again;

            case 'c':
              new_fmt = _ctloc(d_t_fmt);
              goto recurse;

            case 'D':
              new_fmt = "%m/%d/%y";
              LEGAL_ALT(0);
              goto recurse;

            case 'F':
              new_fmt = "%Y-%m-%d";
              LEGAL_ALT(0);
              goto recurse;

            case 'R':
              new_fmt = "%H:%M";
              LEGAL_ALT(0);
              goto recurse;

            case 'r':
              new_fmt =_ctloc(t_fmt_ampm);
              LEGAL_ALT(0);
              goto recurse;

            case 'T':
              new_fmt = "%H:%M:%S";
              LEGAL_ALT(0);
              goto recurse;

	    case 'X':
              new_fmt =_ctloc(t_fmt);
              goto recurse;

            case 'x':
              new_fmt =_ctloc(d_fmt);
              recurse:
              bp = (const u_char *)strptime((const char *)bp, new_fmt, tm);
              LEGAL_ALT(ALT_E);
            continue;

            case 'A':
            case 'a':
              bp = find_string(bp, &tm->tm_wday, _ctloc(day), _ctloc(abday), 7);
              LEGAL_ALT(0);
            continue;

            case 'B':
            case 'b':
            case 'h':
              bp = find_string(bp, &tm->tm_mon, _ctloc(mon), _ctloc(abmon), 12);
              LEGAL_ALT(0);
            continue;

            case 'C':
              i = 20;
              bp = conv_num(bp, &i, 0, 99);

              i = i * 100 - TM_YEAR_BASE;
              if (split_year)
                i += tm->tm_year % 100;
              split_year = 1;
              tm->tm_year = i;
              LEGAL_ALT(ALT_E);
            continue;

            case 'd':
            case 'e':
              bp = conv_num(bp, &tm->tm_mday, 1, 31);
              LEGAL_ALT(ALT_O);
            continue;

            case 'k':
              LEGAL_ALT(0);
              /* FALLTHROUGH */
            case 'H':
              bp = conv_num(bp, &tm->tm_hour, 0, 23);
              LEGAL_ALT(ALT_O);
            continue;

            case 'l':
              LEGAL_ALT(0);
            case 'I':
              bp = conv_num(bp, &tm->tm_hour, 1, 12);
              if (tm->tm_hour == 12)
                tm->tm_hour = 0;
              LEGAL_ALT(ALT_O);
            continue;

            case 'j':
              i = 1;
              bp = conv_num(bp, &i, 1, 366);
              tm->tm_yday = i - 1;
              LEGAL_ALT(0);
            continue;

            case 'M':
              bp = conv_num(bp, &tm->tm_min, 0, 59);
              LEGAL_ALT(ALT_O);
            continue;

            case 'm':
              i = 1;
              bp = conv_num(bp, &i, 1, 12);
              tm->tm_mon = i - 1;
              LEGAL_ALT(ALT_O);
            continue;

            case 'p':
              bp = find_string(bp, &i, _ctloc(am_pm), NULL, 2);
              if (tm->tm_hour > 11)
                return NULL;
              tm->tm_hour += i * 12;
              LEGAL_ALT(0);
            continue;

            case 'S':
              bp = conv_num(bp, &tm->tm_sec, 0, 61);
              LEGAL_ALT(ALT_O);
            continue;

            case 'U':
            case 'W':
               bp = conv_num(bp, &i, 0, 53);
               LEGAL_ALT(ALT_O);
             continue;

             case 'w':
               bp = conv_num(bp, &tm->tm_wday, 0, 6);
               LEGAL_ALT(ALT_O);
             continue;

             case 'Y':
               i = TM_YEAR_BASE;
               bp = conv_num(bp, &i, 0, 9999);
               tm->tm_year = i - TM_YEAR_BASE;
               LEGAL_ALT(ALT_E);
             continue;

             case 'y':
               bp = conv_num(bp, &i, 0, 99);

               if (split_year)
	         {
                   i += (tm->tm_year / 100) * 100;
		 }
               else
	         {
                   split_year = 1;
                   if (i <= 68)
                     i = i + 2000 - TM_YEAR_BASE;
                   else
                     i = i + 1900 - TM_YEAR_BASE;
                 }
               tm->tm_year = i;
             continue;

            case 'Z':
              tzset();
              if (strncmp((const char *)bp, gmt, 3) == 0)
	        {
                  tm->tm_isdst = 0;
#ifdef TM_GMTOFF
                  tm->TM_GMTOFF = 0;
#endif
#ifdef TM_ZONE
                  tm->TM_ZONE = gmt;
#endif
                  bp += 3;
              }
            else
	      {
                const unsigned char *ep;

                ep = find_string(bp, &i, (const char * const *)tzname, NULL, 2);
                if (ep != NULL)
		  {
                    tm->tm_isdst = i;
#ifdef TM_GMTOFF
                    tm->TM_GMTOFF = -(timezone);
#endif
#ifdef TM_ZONE
                    tm->TM_ZONE = tzname[i];
#endif
                  }
                bp = ep;
              }
            continue;

            case 'n':
            case 't':
              while (isspace(*bp))
                bp++;
              LEGAL_ALT(0);
            continue;

            default:
              return NULL;
          }
    }
    return (char *) bp;
}

int clock_gettime(int clock_id, struct timespec *res)
{
  struct timeval tv;
  int result = gettimeofday(&tv, NULL);
  if (result == 0)
  {
    res->tv_sec = tv.tv_sec;
    res->tv_nsec = (long int)(tv.tv_usec * 1000);
  }
  return result;
}

int
setsock_timeout(int sock, int opt_name, struct timeval *socket_timeout)
{
  DWORD timeout = ((socket_timeout->tv_sec*1000)+(socket_timeout->tv_usec/1000));
  return setsockopt((SOCKET)sock, SOL_SOCKET, opt_name, (const char*) &timeout, sizeof(DWORD));
}

int getsockerror()
{
  int res = WSAGetLastError();
  if (res == WSAEWOULDBLOCK)
  {
    return EAGAIN;
  }
  else if (res == WSAEINTR)
  {
    return EINTR;
  }
  else
  {
    return res;
  }
}

void init_signals()
{
  return;
}

int kill(pid_t pid, int sig)
{
       return 0;
}

int res_init()
{
       return 0;
}

pid_t waitpid(pid_t pid, int *status, int options)
{
       return pid;
}

int alarm(unsigned int seconds)
{
       return 0;
}

void IV_SIGNAL_INIT(struct iv_signal *this)
{
       return;
}

int iv_signal_register(struct iv_signal *this)
{
       return 0;
}

void iv_signal_unregister(struct iv_signal *this)
{
       return;
}

void syslog(int pri, const char *bufp, ...)
{
       return;
}

void openlog(const char *ident, int option, int facility)
{
       return;
}

#else

int
setsock_timeout(int sock, int opt_name, struct timeval *socket_timeout)
{
  return setsockopt(sock, SOL_SOCKET, opt_name, (const void*) socket_timeout, sizeof(struct timeval));
}

int
getsockerror()
{
  return errno;
}

void
init_signals()
{
  struct sigaction sa;

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, NULL);
}

#endif /* _WIN32 */

const char *
_compat_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                    char *host,
                    size_t hostlen)
{
  int error = getnameinfo(sa, salen, host, hostlen, NULL, 0, NI_NUMERICHOST);
  if (error)
    {
      if (error == EAI_OVERFLOW)
        errno = ENOSPC;
      return NULL;
    }
  return host;
}

const char *
compat_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
  switch (af)
    {
    case AF_INET:
      {
        struct sockaddr_in sa4;
        memset(&sa4, 0, sizeof(sa4));
        sa4.sin_family = AF_INET;
        memcpy(&sa4.sin_addr, src, sizeof(sa4.sin_addr));
        return _compat_getnameinfo((struct sockaddr *)&sa4, sizeof(sa4), dst, size);
      }
#if ENABLE_IPV6
    case AF_INET6:
      {
        struct sockaddr_in6 sa6;
        memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        memcpy(&sa6.sin6_addr, src, sizeof(sa6.sin6_addr));
        return _compat_getnameinfo((struct sockaddr *)&sa6, sizeof(sa6), dst, size);
      }
#endif
    default:
      {
        errno = EAFNOSUPPORT;
        return NULL;
      }
    }
}

int
compat_inet_pton(int af, const char *src, void *dst)
{
  struct addrinfo hints, *res;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = af;

  if (getaddrinfo(src, NULL, &hints, &res) != 0)
    {
      errno = EAFNOSUPPORT;
      return -1;
    }

  int ok = 0;
  if (res && (af == res->ai_family))
    {
      switch (af)
        {
        case AF_INET:
          {
            struct sockaddr_in *sa4 = (struct sockaddr_in *)res->ai_addr;
            memcpy(dst, &sa4->sin_addr, sizeof(sa4->sin_addr));
            ok = 1;
            break;
          }
#if ENABLE_IPV6
        case AF_INET6:
          {
            struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(dst, &sa6->sin6_addr, sizeof(sa6->sin6_addr));
            ok = 1;
            break;
          }
#endif
        default:
          {
            errno = EAFNOSUPPORT;
            ok = -1;
          }
        }

    }

  freeaddrinfo(res);
  return ok;
}

EVTTAG *
evt_tag_socket_error(const char *name, int value)
{
#ifdef __WIN32
  return evt_tag_win32_error(name, value);
#else
  return evt_tag_errno(name, value);
#endif
}

#if !HAVE_PREAD || HAVE_BROKEN_PREAD

ssize_t
bb__pread(int fd, void *buf, size_t count, off_t offset)
{
  ssize_t ret;
  off_t old_offset;

  old_offset = lseek(fd, 0, SEEK_CUR);
  if (old_offset == -1)
    return -1;

  if (lseek(fd, offset, SEEK_SET) < 0)
    return -1;

  ret = read(fd, buf, count);
  if (ret < 0)
    return -1;

  if (lseek(fd, old_offset, SEEK_SET) < 0)
    return -1;
  return ret;
}

ssize_t
bb__pwrite(int fd, const void *buf, size_t count, off_t offset)
{
  ssize_t ret;
  off_t old_offset;

  old_offset = lseek(fd, 0, SEEK_CUR);
  if (old_offset == -1)
    return -1;

  if (lseek(fd, offset, SEEK_SET) < 0)
    return -1;

  ret = write(fd, buf, count);
  if (ret < 0)
    return -1;

  if (lseek(fd, old_offset, SEEK_SET) < 0)
    return -1;
  return ret;
}
#endif /* !HAVE_PREAD || HAVE_BROKEN_PREAD */

#if !HAVE_STRCASESTR
char *
strcasestr(const char *haystack, const char *needle)
{
  char c;
  size_t len;

  if ((c = *needle++) != 0)
    {
      c = tolower((unsigned char) c);
      len = strlen(needle);

      do
        {
          for (; *haystack && tolower((unsigned char) *haystack) != c; haystack++)
            ;
          if (!(*haystack))
            return NULL;
          haystack++;
        }
      while (strncasecmp(haystack, needle, len) != 0);
      haystack--;
    }
  return (char *) haystack;
}
#endif

#if !HAVE_MEMRCHR
const void *
memrchr(const void *s, int c, size_t n)
{
  const unsigned char *p = (unsigned char *) s + n - 1;

  while (p >= (unsigned char *) s)
    {
      if (*p == c)
        return p;
      p--;
    }
  return NULL;
}
#endif

#ifdef _AIX
intmax_t __strtollmax(const char *__nptr, char **__endptr, int __base)
{
  return strtoll(__nptr, __endptr, __base);
}
#endif

#ifndef HAVE_CLOCK_GETTIME
#ifndef _WIN32
int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
  struct timeval tv;
  int ret = 0;

  ret = gettimeofday(&tv, NULL);
  tp->tv_sec = tv.tv_sec;
  tp->tv_nsec = tv.tv_usec * 1000;

  return ret;
}
#endif
#endif

#ifndef HAVE_LOCALTIME_R
struct tm *
localtime_r(const time_t *timer, struct tm *result)
{
    struct tm *tmp = localtime(timer);

    if (tmp) {
        *result = *tmp;
        return result;
    }
    return tmp;
}
#endif

#ifdef _WIN32
struct iv_fd_thr_info {
  struct iv_fd  *handled_socket;
};

static struct iv_tls_user iv_fd_tls_user = {
  .sizeof_state = sizeof(struct iv_fd_thr_info),
};

static void iv_fd_tls_init(void) __attribute__((constructor));
static void iv_fd_tls_init(void)
{
  iv_tls_user_register(&iv_fd_tls_user);
}

static int iv_fd_set_event_mask(struct iv_fd *this)
{
  if (this->fd == INVALID_SOCKET)
    return -1;

  return WSAEventSelect(this->fd, this->handle.handle,
           this->event_mask);
}

static void iv_fd_got_event(void *_s)
{
  struct iv_fd_thr_info *tinfo =
    iv_tls_user_ptr(&iv_fd_tls_user);
  struct iv_fd *this = (struct iv_fd *)_s;
  WSANETWORKEVENTS ev;
  int ret;
  int i;

  ret = WSAEnumNetworkEvents(this->fd, this->handle.handle, &ev);
  if (ret) {
    iv_fatal("iv_fd_got_event: WSAEnumNetworkEvents "
       "returned %d", ret);
  }

  tinfo->handled_socket = this;
  for (i = 0; i < FD_MAX_EVENTS; i++) {
    if (ev.lNetworkEvents & (1 << i)) {
      this->handler[i](this->handle.cookie, i, ev.iErrorCode[i]);
      if (tinfo->handled_socket == this && ev.iErrorCode[i] != 0 && this->handler_err)
        {
          this->handler_err(this->cookie);
        }
      if (tinfo->handled_socket == NULL)
        return;
    }
  }
  tinfo->handled_socket = NULL;

  iv_fd_set_event_mask(this);
}

int _iv_fd_register(struct iv_fd *this)
{
  HANDLE hnd;
  int i;

  hnd = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (hnd == NULL)
    return -1;

  IV_HANDLE_INIT(&this->handle);
  this->handle.handle = hnd;
  this->handle.cookie = this;
  this->handle.handler = iv_fd_got_event;
  this->event_mask = 0;

  for (i = 0; i < FD_MAX_EVENTS; i++) {
    if (this->handler[i] != NULL)
      this->event_mask |= 1 << i;
  }
  /*
   * Call WSAEventSelect() even if the event mask is zero,
   * as it implicitly sets the socket to nonblocking mode.
   */
  if (iv_fd_set_event_mask(this) != 0)
    {
      CloseHandle(this->handle.handle);
      return -1;
    }

  iv_handle_register(&this->handle);
  return 0;
}

void iv_fd_register(struct iv_fd *this)
{
  if (_iv_fd_register(this))
    iv_fatal("iv_fd_regiter: can't create event for socket");
  return;
}

int iv_fd_register_try(struct iv_fd *this)
{
  return _iv_fd_register(this);
}

int iv_fd_registered(struct iv_fd *this)
{
  return iv_handle_registered(&this->handle);
}

void iv_fd_unregister(struct iv_fd *this)
{
  struct iv_fd_thr_info *tinfo =
    iv_tls_user_ptr(&iv_fd_tls_user);

  if (tinfo->handled_socket == this)
    tinfo->handled_socket = NULL;

  if (this->event_mask) {
    this->event_mask = 0;
    iv_fd_set_event_mask(this);
    this->handler_err = NULL;
  }

  iv_handle_unregister(&this->handle);
  CloseHandle(this->handle.handle);
}

void iv_fd_set_handler(struct iv_fd *this, int event,
             void (*handler)(void *, int, int))
{
  struct iv_fd_thr_info *tinfo =
    iv_tls_user_ptr(&iv_fd_tls_user);
  long old_mask;

  if (event >= FD_MAX_EVENTS) {
    iv_fatal("iv_fd_set_handler: called with "
       "event == %d", event);
  }

  old_mask = this->event_mask;
  if (this->handler[event] == NULL && handler != NULL)
    this->event_mask |= 1 << event;
  else if (this->handler[event] != NULL && handler == NULL)
    this->event_mask &= ~(1 << event);

  this->handler[event] = handler;

  if (tinfo->handled_socket != this && old_mask != this->event_mask)
    iv_fd_set_event_mask(this);
}

void IV_FD_INIT(struct iv_fd *this)
{
  int i;

  this->fd = INVALID_SOCKET;
  this->cookie = NULL;
  for (i = 0; i < FD_MAX_EVENTS; i++)
    this->handler[i] = NULL;
  IV_HANDLE_INIT(&this->handle);
}

static void iv_sock_handler_in(void *cookie, int event, int error)
{
  struct iv_fd *sock = cookie;

  sock->handler_in(sock->cookie);
}


void iv_fd_set_handler_in(struct iv_fd *this, void (*handler_in)(void *))
{
  void (*h)(void *, int, int);

  this->handler_in = handler_in;

  h = (handler_in != NULL) ? iv_sock_handler_in : NULL;
  iv_fd_set_handler(this, FD_READ_BIT, h);
  iv_fd_set_handler(this, FD_OOB_BIT, h);
  iv_fd_set_handler(this, FD_ACCEPT_BIT, h);
}

static void iv_sock_handler_out(void *cookie, int event, int error)
{
  struct iv_fd *sock = cookie;

  sock->handler_out(sock->cookie);
}


void iv_fd_set_handler_out(struct iv_fd * this, void (*handler_out)(void *))
{
  void (*h)(void *, int, int);

  this->handler_out = handler_out;

  h = (handler_out != NULL) ? iv_sock_handler_out : NULL;
  iv_fd_set_handler(this, FD_WRITE_BIT, h);
  iv_fd_set_handler(this, FD_CONNECT_BIT, h);
}

void iv_sock_handler_err(void *cookie, int event, int error)
{
  struct iv_fd *sock = cookie;

  sock->handler_err(sock->cookie);
}

void iv_fd_set_handler_err(struct iv_fd *this, void (*handler_err)(void *))
{
  void (*h)(void *, int, int);

  this->handler_err = handler_err;

  h = (handler_err != NULL) ? iv_sock_handler_err : NULL;
  iv_fd_set_handler(this, FD_CLOSE_BIT, h);
}

#endif


#define DEFAULT_PROCESSOR_COUNT 2

#ifndef _WIN32

long
get_processor_count()
{
#ifdef _SC_NPROCESSORS_ONLN
  return sysconf(_SC_NPROCESSORS_ONLN);
#else
  return DEFAULT_PROCESSOR_COUNT;
#endif /*_SC_NPROCESSORS_ONLN*/
}

#else

long
get_processor_count()
{
  SYSTEM_INFO system_info;
  GetSystemInfo(&system_info);
  return (long)system_info.dwNumberOfProcessors;
}

#endif /*_WIN32*/
#if !defined(strtok_r) || defined(USE_MYSTRTOK_R)
char *
mystrtok_r(char *str, const char *delim, char **saveptr)
{
  char *it;
  char *head;

  if (str)
    *saveptr = str;

  if (!*saveptr)
    return NULL;

  it = *saveptr;

 /*find the first non-delimiter*/
  it += strspn(it, delim);

  head = it;

  if (!it || !*it)
    {
      *saveptr = NULL;
      return NULL;
    }

  /* find the first delimiter */
  it = strpbrk(it, delim);
  /* skip all the delimiters */
  while (it && *it && strchr(delim, *it)) {*it='\0';it++;}

  *saveptr = it;

  return head;
}
#endif

#ifndef HAVE_STRTOK_R_SUPPORT
inline char *strtok_r(char *string, const char *delim, char **saveptr)
{
  return mystrtok_r(string, delim, saveptr);
}
#endif

/* getutent/endutent support */

#if !defined(HAVE_GETUTENT) && !defined(HAVE_GETUTXENT) && defined(HAVE_UTMP_H) && !defined(_WIN32)

static int utent_fd = -1;

#ifndef _PATH_UTMP
#define _PATH_UTMP "/var/log/utmp"
#endif

struct utmp *getutent(void)
{
  static struct utmp ut;
  int rc;

  if (utent_fd == -1)
    utent_fd = open(_PATH_UTMP, O_RDONLY | O_NOCTTY);

  if (utent_fd == -1)
    return NULL;

  rc = read(utent_fd, &ut, sizeof(ut));

  if (rc <= 0)
    {
      close(utent_fd);
      utent_fd = -1;
      return NULL;
    }
  else
    return &ut;
}

void endutent(void)
{
  if (utent_fd != -1)
    {
      close(utent_fd);
      utent_fd = -1;
    }
}
#endif
