/*
 * Copyright (c) 2007-2015 Balabit
 * Copyright (c) 2007-2015 Balázs Scheidler
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

#include <config.h>
#include <stdio.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <netdb.h>
#else
#include "../../lib/compat.h"
#include <winsock2.h>
#ifdef __MINGW32__
#include <stdint.h>
#endif
#endif
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <zlib.h>
#include <signal.h>
#include <pthread.h>

#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <unistd.h>

#define MAX_MESSAGE_LENGTH 8192

#define USEC_PER_SEC      1000000

#ifndef MIN
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#endif

int rate = 1000;
int unix_socket = 0;
int use_ipv6 = 0;
int unix_socket_i = 0;
int unix_socket_x = 0;
int sock_type_s = 0;
int sock_type_d = 0;
int sock_type = SOCK_STREAM;
struct sockaddr *dest_addr;
#ifndef _WIN32
socklen_t dest_addr_len;
#else
size_t dest_addr_len;
#endif
int message_length = 256;
int interval = 10;
int number_of_messages = 0;
int csv = 0;
int quiet = 0;
int syslog_proto = 0;
int framing = 1;
int skip_tokens = 3;
int usessl;
char *read_file = NULL;
int idle_connections = 0;
int active_connections = 1;
int loop_reading = 0;
int dont_parse = 0;
static gint display_version;
char *sdata_value = NULL;
int rltp = 0;
int allow_compress = 0;
int use_port_range = 0;

/* results */
guint64 sum_count;
struct timeval sum_time;
gint raw_message_length;
SSL_CTX* ssl_ctx;

typedef struct _ActiveThreadContext
{
  gint fd;
  z_stream istream;
  z_stream ostream;
  gchar compressed_buffer[MAX_MESSAGE_LENGTH];
  int usezlib;
} ActiveThreadContext;

typedef ssize_t (*send_data_t)(void *user_data, void *buf, size_t length);

int rltp_batch_size = 100;

int *rltp_chunk_counters;

#if GLIB_SIZEOF_LONG == GLIB_SIZEOF_VOID_P
#define SLONGDEF glong
#elif GLIB_SIZEOF_VOID_P == 4
#define SLONGDEF gint32
#elif GLIB_SIZEOF_VOID_P == 8
#define SLONGDEF gint64
#else
#error "Unsupported word length, only 32 or 64 bit platforms are suppoted"
#endif

static ssize_t
write_bytes(ActiveThreadContext *ctx, void *buf, size_t length)
{
  ssize_t result = 0;
  char *outbuffer;
  size_t outlength;
  if (ctx->usezlib)
    {
      ctx->ostream.avail_in = length;
      ctx->ostream.next_in = buf;
      ctx->ostream.avail_out = MAX_MESSAGE_LENGTH;
      ctx->ostream.next_out = (guchar *)ctx->compressed_buffer;
      g_assert(deflate(&ctx->ostream, Z_SYNC_FLUSH) == Z_OK);
      outbuffer = ctx->compressed_buffer;
      outlength = MAX_MESSAGE_LENGTH - ctx->ostream.avail_out;
    }
  else
    {
      outbuffer = buf;
      outlength = length;
    }
  result = send(ctx->fd, outbuffer, outlength, 0);
  return result == outlength ? length : result;
}

static ssize_t
read_bytes(ActiveThreadContext *ctx, void *buf, size_t length)
{
  ssize_t result = 0;
  if (ctx->usezlib)
    {
      result = recv(ctx->fd, ctx->compressed_buffer, length / 2, 0);
      if (result > 0)
        {
          ctx->istream.avail_in = result;
          ctx->istream.next_in = (guchar *)ctx->compressed_buffer;
          ctx->istream.avail_out = length;
          ctx->istream.next_out = buf;
          g_assert(inflate(&ctx->istream, Z_SYNC_FLUSH) == Z_OK);
          result = length - ctx->istream.avail_out;
        }
    }
  else
    {
      result = recv(ctx->fd, buf, length, 0);
    }
  return result;
}

void
read_sock(ActiveThreadContext *ctx, char *buf, int bufsize)
{
  int rb = 0;
  rb = read_bytes(ctx, buf, bufsize - 1);
  buf[rb] = '\0';
}


static ssize_t
send_plain(void *user_data, void *buf, size_t length)
{
  ActiveThreadContext *ctx = (ActiveThreadContext *)user_data;
  int id = GPOINTER_TO_INT(g_thread_self()->data);
  gsize len = 0;
  gchar expected[100] = {0};
  int res = write_bytes(ctx, buf, length);
  if (rltp)
    {
      rltp_chunk_counters[id] += 1;
      if (rltp_chunk_counters[id] == rltp_batch_size)
        {
          char cbuf[256] = {0};
          write_bytes(ctx, ".\n",2);
          read_sock(ctx, cbuf, 256);
          len = g_snprintf(expected,100,"250 Received %d\n",rltp_chunk_counters[id]);
          if (strncmp(cbuf,expected,len)!=0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s EXPECTED: %s",cbuf,expected);
              return -1;
            }
          write_bytes(ctx, "DATA\n", 5);
          read_sock(ctx, cbuf, 256);
          if (strncmp(cbuf,"250 Ready\n",strlen("250 Ready\n"))!=0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
              return -1;
            }
          rltp_chunk_counters[id] = 0;
        }
    }
  return res;
}

static ssize_t
send_ssl(void *user_data, void *buf, size_t length)
{
  SSL *fd = (SSL *)user_data;
  int id = GPOINTER_TO_INT(g_thread_self()->data);
  gsize len = 0;
  static gchar expected[100] = {0};
  int res = SSL_write(fd, buf, length);
  if (rltp)
    {
      rltp_chunk_counters[id] += 1;
      if (rltp_chunk_counters[id] == rltp_batch_size)
        {
          char cbuf[256];
          SSL_write(fd, ".\n",2);
          SSL_read(fd, cbuf, 256);
          len = g_snprintf(expected,100,"250 Received %d\n",rltp_chunk_counters[id]);
          if (strncmp(cbuf,expected,len)!=0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s EXPECTED: %s",cbuf,expected);
              return -1;
            }
          SSL_write(fd, "DATA\n", 5);
          SSL_read(fd, cbuf, 256);
          if (strncmp(cbuf,"250 Ready\n",strlen("250 Ready\n"))!=0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
              return -1;
            }
          rltp_chunk_counters[id] = 0;
        }
    }
  return res;
}

static unsigned long
time_val_diff_in_usec(struct timeval *t1, struct timeval *t2)
{
  return (t1->tv_sec - t2->tv_sec) * USEC_PER_SEC + (t1->tv_usec - t2->tv_usec);
}

static void
time_val_diff_in_timeval(struct timeval *res, const struct timeval *t1, const struct timeval *t2)
{
  res->tv_sec = (t1->tv_sec - t2->tv_sec);
  res->tv_usec = (t1->tv_usec - t2->tv_usec);
  if (res->tv_usec < 0)
    {
      res->tv_sec--;
      res->tv_usec += USEC_PER_SEC;
    }
}

static void
time_val_add_time_val(struct timeval *res, const struct timeval *t1, const struct timeval *t2)
{
  res->tv_sec = (t1->tv_sec + t2->tv_sec);
  res->tv_usec = (t1->tv_usec + t2->tv_usec);
  if (res->tv_usec >= 1e6)
    {
      res->tv_sec++;
      res->tv_usec -= USEC_PER_SEC;
    }
}

static ssize_t
write_chunk(send_data_t send_func, void *send_func_ud, void *buf, size_t buf_len)
{
  ssize_t rc;
  size_t pos = 0;

  while (pos < buf_len)
    {
      rc = send_func(send_func_ud, buf + pos, buf_len - pos);

      if (rc < 0)
        return -1;

      else if (rc == 0)
        {
          errno = ECONNABORTED;
          return -1;
        }
      pos += rc;
    }
  return pos;
}

static int
parse_line(const char *line, char *host, char *program, char *pid, char **msg)
{
  const char *pos0;
  const char *pos = line;
  const char *end;
  int space = skip_tokens;
  int pid_len;

  /* Find token */
  while (space)
    {
      pos = strchr(pos, ' ');

      if (!pos)
        return -1;
      pos++;

      space --;
    }

  pos = strchr(pos, ':');
  if (!pos)
    return -1;

  /* pid */
  pos0 = pos;
  if (*(--pos) == ']')
    {
      end = pos - 1;
      while (*(--pos) != '[')
        ;

      pid_len = end - pos; /* 'end' points to the last character of the pid string (not off by one), *pos = '[' -> pid length = end - pos*/
      memcpy(pid, pos + 1, pid_len);
      pid[pid_len] = '\0';
    }
  else
    {
      pid[0] = '\0';
      ++pos; /* the 'pos' has been decreased in the condition (']'), reset it to the original position */
    }

  /* Program */
  end = pos;
  while (*(--pos) != ' ');

  memcpy(program, pos + 1, end - pos - 1);
  program[end-pos-1] = '\0';

  /* Host */
  end = pos;
  while (*(--pos) != ' ')
    ;

  memcpy(host, pos + 1, end - pos - 1);
  host[end-pos-1] = '\0';

  *msg = ((char*)pos0) + 2;

  return 1;
}

static size_t
_get_now_timestamp(char *stamp, gsize stamp_size)
{
  time_t now;
  struct tm tm;

  now = time(NULL);
  localtime_r(&now, &tm);
  return strftime(stamp, stamp_size, "%Y-%m-%dT%H:%M:%S", &tm);
}

static int
gen_next_message(FILE *source, char *buf, int buflen)
{
  static int lineno = 0;
  int linelen;
  char host[128], program[128], pid[16];
  char *msg = NULL;

  while (1)
    {
      if (feof(source))
        {
          if (loop_reading)
          {
            // Restart reading from the beginning of the file
            rewind(source);
          }
          else
            return -1;
        }
      char *temp = fgets(buf, buflen, source);
      if (!temp)
        {
          if (loop_reading)
          {
            // Restart reading from the beginning of the file
            rewind(source);
            temp = fgets(buf, buflen, source);
          }
          else
            return -1;
        }
      if (dont_parse)
        break;

      if (parse_line(buf, host, program, pid, &msg) > 0)
        break;

      fprintf(stderr, "\rInvalid line %d                  \n", ++lineno);
    }

  if (dont_parse)
    {
      linelen = strlen(buf);
      return linelen;
    }

  char stamp[32];
  int tslen = _get_now_timestamp(stamp, sizeof(stamp));

  if (syslog_proto)
    {
      char tmp[11];
      linelen = snprintf(buf + 10, buflen - 10, "<38>1 %.*s %s %s %s - - \xEF\xBB\xBF%s", tslen, stamp, host, program, (pid[0] ? pid : "-"), msg);
      snprintf(tmp, sizeof(tmp), "%09d ", linelen);
      memcpy(buf, tmp, 10);
      linelen += 10;
    }
  else
    {
      if (*pid)
        linelen = snprintf(buf, buflen, "<38>%.*s %s %s[%s]: %s", tslen, stamp, host, program, pid, msg);
      else
        linelen = snprintf(buf, buflen, "<38>%.*s %s %s: %s", tslen, stamp, host, program, msg);
    }

  return linelen;
}

static int
connect_server(gint id)
{
  int sock;
  struct sockaddr * s_in = g_malloc0(dest_addr_len);
  memcpy(s_in,dest_addr,dest_addr_len);
  sock = socket(s_in->sa_family, sock_type, 0);
  if (sock < 0)
    {
      fprintf(stderr, "Error creating socket: %s\n", g_strerror(errno));
      return -1;
    }
  if (use_port_range)
    {
      if (s_in->sa_family == AF_INET6)
        {
          ((struct sockaddr_in6 *)s_in)->sin6_port = htons(ntohs(((struct sockaddr_in6*)s_in)->sin6_port) + id);
        }
      else if (s_in->sa_family == AF_INET)
        {
          ((struct sockaddr_in *)s_in)->sin_port = htons(ntohs(((struct sockaddr_in*)s_in)->sin_port) + id);
        }
    }
  if (connect(sock, s_in, dest_addr_len) < 0)
    {
      fprintf(stderr, "Error connecting socket: %s\n", g_strerror(errno));
      g_free(s_in);
      close(sock);
      return -1;
    }
  g_free(s_in);
  return sock;
}

static guint64
gen_messages(send_data_t send_func, void *send_func_ud, int thread_id, FILE *readfrom)
{
  struct timeval now, start, last_ts_format, last_throttle_check;
  char linebuf[MAX_MESSAGE_LENGTH + 1];
  char stamp[32];
  char intbuf[16];
  int linelen = 0;
  int i, run_id;
  unsigned long count = 0, last_count = 0;
  char padding[] = "PADD";
  long buckets = rate - (rate / 10);
  double diff_usec;
  struct timeval diff_tv;
  int pos_timestamp1 = 0, pos_timestamp2 = 0, pos_seq = 0;
  int rc, hdr_len = 0;
  gint64 sum_linelen = 0;
  char *testsdata = NULL;

  gettimeofday(&start, NULL);
  now = start;
  last_throttle_check = now;
  run_id = start.tv_sec;

  /* force reformat of the timestamp */
  last_ts_format = now;
  last_ts_format.tv_sec--;

   if (sdata_value)
     {
       testsdata = strdup(sdata_value);
     }
   else
     {
       testsdata = strdup("-");
     }

  if (!readfrom)
    {
      if (syslog_proto)
        {
          if (sock_type == SOCK_STREAM && framing)
            hdr_len = snprintf(linebuf, sizeof(linebuf), "%d ", message_length);

          linelen = snprintf(linebuf + hdr_len, sizeof(linebuf) - hdr_len, "<38>1 2007-12-24T12:28:51+02:00 localhost prg%05d 1234 - %s \xEF\xBB\xBFseq: %010d, thread: %04d, runid: %-10d, stamp: %-19s ", thread_id, testsdata, 0, thread_id, run_id, "");

          pos_timestamp1 = 6 + hdr_len;
          pos_seq = 68 + hdr_len + strlen(testsdata) - 1;
          pos_timestamp2 = 120 + hdr_len + strlen(testsdata) - 1;
        }
      else
        {
          if (sock_type == SOCK_STREAM && rltp)
            hdr_len = snprintf(linebuf, sizeof(linebuf), "%d ", message_length);
          else
            hdr_len = 0;
          linelen = snprintf(linebuf + hdr_len, sizeof(linebuf) - hdr_len, "<38>2007-12-24T12:28:51 localhost prg%05d[1234]: seq: %010d, thread: %04d, runid: %-10d, stamp: %-19s ", thread_id, 0, thread_id, run_id, "");
          pos_timestamp1 = 4 + hdr_len;
          pos_seq = 55 + hdr_len;
          pos_timestamp2 = 107 + hdr_len;
        }

      if (linelen > message_length)
        {
          fprintf(stderr, "Warning: message length is too small, the minimum is %d bytes\n", linelen);
          return 0;
        }
    }
  for (i = linelen; i < message_length - 1; i++)
    {
      linebuf[i + hdr_len] = padding[(i - linelen) % (sizeof(padding) - 1)];
    }
  linebuf[hdr_len + message_length - 1] = '\n';
  linebuf[hdr_len + message_length] = 0;

  /* NOTE: all threads calculate raw_message_length. This code could use some refactorization. */
  raw_message_length = linelen = strlen(linebuf);
  while (time_val_diff_in_usec(&now, &start) < ((int64_t)interval) * USEC_PER_SEC)
    {
      if(number_of_messages != 0 && count >= number_of_messages)
        {
          break;
        }
      gettimeofday(&now, NULL);

      diff_usec = time_val_diff_in_usec(&now, &last_throttle_check);
      if (buckets == 0 || diff_usec > 1e5)
        {
          /* check rate every 0.1sec */
          long new_buckets;

          new_buckets = (rate * diff_usec) / USEC_PER_SEC;
          if (new_buckets)
            {
              buckets = MIN(rate, buckets + new_buckets);
              last_throttle_check = now;
            }
        }

      if (buckets == 0)
        {
          struct timespec tspec, trem;
          long msec = (1000 / rate) + 1;

          tspec.tv_sec = msec / 1000;
          tspec.tv_nsec = (msec % 1000) * 1e6;
          while (nanosleep(&tspec, &trem) < 0 && errno == EINTR)
            {
              tspec = trem;
            }
          continue;
        }

      if (readfrom)
        {
          rc = gen_next_message(readfrom, linebuf, sizeof(linebuf));
          if (rc == -1)
            break;

          linelen = rc;
          sum_linelen = sum_linelen+rc;

        }

      if (now.tv_sec != last_ts_format.tv_sec)
        {
          /* tv_has has changed, update message timestamp & print current message rate */

          if (!readfrom)
            {
              int len;
              time_t now_time_t;
              struct tm tm;

              now_time_t = now.tv_sec;
              localtime_r(&now_time_t, &tm);
              len = strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm);
              memcpy(&linebuf[pos_timestamp1], stamp, len);
              memcpy(&linebuf[pos_timestamp2], stamp, len);
            }

          diff_usec = time_val_diff_in_usec(&now, &last_ts_format);
          if (csv)
            {
              time_val_diff_in_timeval(&diff_tv, &now, &start);
              printf("%d;%lu.%06lu;%.2lf;%lu\n", thread_id, (long) diff_tv.tv_sec, (long) diff_tv.tv_usec, (((double) (count - last_count) * USEC_PER_SEC) / diff_usec),count);

            }
          else if (!quiet)
            {
              fprintf(stderr, "count=%ld, rate = %.2f msg/sec                 \r", count, ((double) (count - last_count) * USEC_PER_SEC) / diff_usec);
            }
          last_ts_format = now;
          last_count = count;
        }

      if (!readfrom)
        {
          /* add sequence number */
          snprintf(intbuf, sizeof(intbuf), "%010ld", count);
          memcpy(&linebuf[pos_seq], intbuf, 10);
        }

      rc = write_chunk(send_func, send_func_ud, linebuf, linelen);
      if (rc < 0)
        {
          fprintf(stderr, "Send error %s, results may be skewed.\n", strerror(errno));
          break;
        }
      buckets--;
      count++;
    }

  gettimeofday(&now, NULL);
  diff_usec = time_val_diff_in_usec(&now, &start);
  time_val_diff_in_timeval(&diff_tv, &now, &start);
  if (csv)
    printf("%d;%lu.%06lu;%.2lf;%lu\n", thread_id, (long) diff_tv.tv_sec, (long) diff_tv.tv_usec, (((double) (count - last_count) * USEC_PER_SEC) / diff_usec), count);

  if (readfrom)
    {
      if (count > 0)
        raw_message_length = sum_linelen/count;
      else
        raw_message_length = 0;
    }
  free(testsdata);
  return count;
}

static void
release_ssl_transport(void *ssl_connect)
{
  SSL *ssl = (SSL *)ssl_connect;
  if (ssl)
    {
      SSL_shutdown (ssl);
      /* Clean up. */
      SSL_free (ssl);
    }
}

static void*
set_ssl_transport(int sock)
{
  int err;
  SSL *ssl;
  if (NULL == (ssl = SSL_new(ssl_ctx)))
    {
      fprintf(stderr,"Can't create ssl object!\n");
      return NULL;
    }

  SSL_set_fd (ssl, sock);
  if (-1 == (err = SSL_connect(ssl)))
    {
      fprintf(stderr, "SSL connect failed\n");
      ERR_print_errors_fp(stderr);
      release_ssl_transport(ssl);
      return NULL;
    }
  return ssl;
}

static guint64
gen_messages_ssl(ActiveThreadContext *ctx, int id, FILE *readfrom)
{
  int ret = 0;
  int err;
  SSL *ssl;

  if (NULL == (ssl = SSL_new(ssl_ctx)))
    return 1;

  SSL_set_fd (ssl, ctx->fd);
  if (-1 == (err = SSL_connect(ssl)))
    {
      fprintf(stderr, "SSL connect failed\n");
      ERR_print_errors_fp(stderr);
      return 2;
    }

  ret = gen_messages(send_ssl, ssl, id, readfrom);

  SSL_shutdown (ssl);
  /* Clean up. */
  SSL_free (ssl);

  return ret;
}

static guint64
gen_messages_plain(ActiveThreadContext *ctx, int id, FILE *readfrom)
{
  return gen_messages(send_plain, ctx, id, readfrom);
}

GMutex *thread_lock;
GCond *thread_cond;
GCond *thread_finished;
GCond *thread_connected;
gboolean threads_start;
gboolean threads_stop;
gint active_finished;
gint connect_finished;
guint64 sum_count;

/* Multithreaded OPENSSL */
GMutex **ssl_lock_cs;
long *ssl_lock_count;

void ssl_locking_callback(int mode, int type, char *file, int line)
{
  if (mode & CRYPTO_LOCK)
    {
      g_mutex_lock(ssl_lock_cs[type]);
      ssl_lock_count[type]++;
    }
  else
    {
      g_mutex_unlock(ssl_lock_cs[type]);
    }
}

unsigned long ssl_thread_id(void)
{
  unsigned long ret;
#ifndef _WIN32
  ret=(unsigned long)pthread_self();
#else
  ret=GetCurrentThreadId();
#endif
  return(ret);
}

void thread_setup(void)
  {
  int i;

  ssl_lock_cs=OPENSSL_malloc(CRYPTO_num_locks() * sizeof(void *));
  ssl_lock_count=OPENSSL_malloc(CRYPTO_num_locks() * sizeof(long));
  for (i=0; i<CRYPTO_num_locks(); i++)
    {
      ssl_lock_cs[i]=g_mutex_new();
    }
  CRYPTO_set_id_callback((unsigned long (*)())ssl_thread_id);
  CRYPTO_set_locking_callback((void (*)())ssl_locking_callback);
}

gpointer
idle_thread(gpointer st)
{
  int sock;
  sock = connect_server(0);
  if (sock < 0)
    goto error;
  g_mutex_lock(thread_lock);
  connect_finished++;
  if (connect_finished == active_connections + idle_connections)
    g_cond_signal(thread_connected);

  while (!threads_start)
    g_cond_wait(thread_cond, thread_lock);


  while (!threads_stop)
    g_cond_wait(thread_cond, thread_lock);
  g_mutex_unlock(thread_lock);
  close(sock);
  return NULL;
error:
  g_mutex_lock(thread_lock);
  connect_finished++;
  g_cond_signal(thread_connected);
  g_cond_signal(thread_finished);
  g_mutex_unlock(thread_lock);
  return NULL;
}

void
read_rltp_options(ActiveThreadContext *ctx,gint *use_zlib,gint *require_tls,gint *serialization)
{
  char cbuf[256];
  char *options = NULL;
  read_sock(ctx, cbuf, 256);
  options = strstr(cbuf,"250");
  while(options)
    {
      options+=4;
      if (strncmp(options,"STARTTLS",strlen("STARTTLS")) == 0)
        {
          options+=strlen("STARTTLS") + 1;
          *require_tls = 1;
        }
      else if (strncmp(options, "ZLIB", strlen("ZLIB")) == 0)
        {
          options+=strlen("ZLIB") + 1;
          *use_zlib = 1;
        }
      else if (strncmp(options,"serialized",strlen("serialized")) == 0)
        {
          options+=strlen("serialized") + 1;
          *serialization = strtod(options,NULL);
        }
      options = strstr(options,"250");
    }
}

gpointer
active_thread(gpointer st)
{

  int id = GPOINTER_TO_INT(st);
  gint sock;
  guint64 count;
  struct timeval start, end, diff_tv;
  FILE *readfrom = NULL;
  GString *ehlo = g_string_sized_new(64);
  GString *sync = g_string_sized_new(64);
  void *ssl_transport = NULL;
  ActiveThreadContext *ctx = g_new0(ActiveThreadContext, 1);

  sock = connect_server(id);
  if (sock < 0)
    goto error;
  g_mutex_lock(thread_lock);
  connect_finished++;
  if (connect_finished == active_connections + idle_connections)
    g_cond_signal(thread_connected);

  while (!threads_start)
    g_cond_wait(thread_cond, thread_lock);
  g_mutex_unlock(thread_lock);

  int use_zlib = 0,
      require_tls = 0,
      serialization = 0;

  ctx->fd = sock;

  if (allow_compress)
      {
        g_assert(inflateInit(&ctx->istream) == Z_OK);
        g_assert(deflateInit(&ctx->ostream, 5) == Z_OK);
      }

  if (rltp)
    {
      char cbuf[256];
      g_string_sprintf(ehlo,"EHLO\n");
      /* session id */
      read_sock(ctx, cbuf ,256);
      write_bytes(ctx, ehlo->str, ehlo->len);
      read_rltp_options(ctx, &use_zlib, &require_tls, &serialization);
      if (require_tls && usessl)
        {
          write_bytes(ctx, "STARTTLS\n",9);
          read_sock(ctx, cbuf ,256);
          ssl_transport = set_ssl_transport(ctx->fd);
          if (!ssl_transport)
            {
              fprintf(stderr,"Can't create ssl connection\n");
              exit(1);
            }
          g_string_sprintf(ehlo,"EHLO\n");
          SSL_write(ssl_transport, ehlo->str, ehlo->len);
          SSL_read(ssl_transport, cbuf, 256);
          if (strncmp(cbuf,"250",3) !=0 )
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
            }
          g_string_sprintf(sync,"SYNC loggen_%d\n",id);
          SSL_write(ssl_transport, sync->str, sync->len);
          SSL_read(ssl_transport, cbuf, 256);
          if (strncmp(cbuf,"250 ",4) !=0 )
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
            }
          SSL_write(ssl_transport, "DATA\n", 5);
          SSL_read(ssl_transport, cbuf ,256);
          if (strncmp(cbuf,"250 ",4)!=0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
            }
        }
      else if (allow_compress && use_zlib)
        {
          write_bytes(ctx, "ZLIB\n", 5);
          read_sock(ctx, cbuf, 256);
          if (strncmp(cbuf,"250 ",4) != 0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
              return NULL;
            }
          ctx->usezlib = TRUE;
          goto use_plain_transport;
        }
      else
        {
use_plain_transport:
          g_string_sprintf(sync,"SYNC loggen_%d\n",id);
          write_bytes(ctx, sync->str, sync->len);
          read_sock(ctx, cbuf, 256);
          if (strncmp(cbuf,"250 ",4) != 0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
              return NULL;
            }
          write_bytes(ctx, "DATA\n", 5);
          read_sock(ctx, cbuf ,256);
          if (strncmp(cbuf,"250 ",4)!=0)
            {
              fprintf(stderr,"EXIT BAD SERVER REPLY: %s\n",cbuf);
              return NULL;
            }
        }
    }

  if (read_file != NULL)
    {
      if (read_file[0] == '-' && read_file[1] == '\0')
        {
          readfrom = stdin;
        }
      else
        {
          readfrom = fopen(read_file, "r");
          if (!readfrom)
            {
              const int bufsize = 1024;
              char cbuf[bufsize];
              snprintf(cbuf, bufsize, "fopen: %s", read_file);
              perror(cbuf);
              return NULL;
            }
        }
    }

  gettimeofday(&start, NULL);
  if (!rltp)
    {
      count = (usessl ? gen_messages_ssl : gen_messages_plain)(ctx, id, readfrom);
    }
  else if (usessl)
    {
      count = gen_messages(send_ssl, ssl_transport, id, readfrom);
    }
  else
    {
      count = gen_messages_plain(ctx, id, readfrom);
    }
  if (rltp && rltp_chunk_counters[id])
    {
      char cbuf[256];
      write_bytes(ctx, ".\n", 2);
      read_sock(ctx, cbuf, 256);
      rltp_chunk_counters[id] = 0;
    }
  shutdown(ctx->fd, SHUT_RDWR);
  gettimeofday(&end, NULL);
  time_val_diff_in_timeval(&diff_tv, &end, &start);

  g_mutex_lock(thread_lock);
  sum_count += count;
  time_val_add_time_val(&sum_time, &sum_time, &diff_tv);
  active_finished++;
  if (active_finished == active_connections)
    g_cond_signal(thread_finished);
  g_mutex_unlock(thread_lock);
  close(sock);
  if (readfrom && readfrom != stdin)
    fclose(readfrom);
  return NULL;
error:
  g_mutex_lock(thread_lock);
  connect_finished++;
  active_finished++;
  g_cond_signal(thread_connected);
  g_cond_signal(thread_finished);
  g_mutex_unlock(thread_lock);
  return NULL;
}

static GOptionEntry loggen_options[] = {
  { "rate", 'r', 0, G_OPTION_ARG_INT, &rate, "Number of messages to generate per second", "<msg/sec/active connection>" },
  { "inet", 'i', 0, G_OPTION_ARG_NONE, &unix_socket_i, "Use IP-based transport (TCP, UDP)", NULL },
  { "unix", 'x', 0, G_OPTION_ARG_NONE, &unix_socket_x, "Use UNIX domain socket transport", NULL },
  { "stream", 'S', 0, G_OPTION_ARG_NONE, &sock_type_s, "Use stream socket (TCP and unix-stream)", NULL },
  { "ipv6", '6', 0, G_OPTION_ARG_NONE, &use_ipv6, "Use AF_INET6 sockets instead of AF_INET (can use both IPv4 & IPv6)", NULL },
  { "dgram", 'D', 0, G_OPTION_ARG_NONE, &sock_type_d, "Use datagram socket (UDP and unix-dgram)", NULL },
  { "size", 's', 0, G_OPTION_ARG_INT, &message_length, "Specify the size of the syslog message", "<size>" },
  { "interval", 'I', 0, G_OPTION_ARG_INT, &interval, "Number of seconds to run the test for", "<sec>" },
  { "syslog-proto", 'P', 0, G_OPTION_ARG_NONE, &syslog_proto, "Use the new syslog-protocol message format (see also framing)", NULL },
  { "sdata", 'p', 0, G_OPTION_ARG_STRING, &sdata_value, "Send the given sdata (e.g. \"[test name=\\\"value\\\"]) in case of syslog-proto", NULL },
  { "no-framing", 'F', G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &framing, "Don't use syslog-protocol style framing, even if syslog-proto is set", NULL },
  { "active-connections", 0, 0, G_OPTION_ARG_INT, &active_connections, "Number of active connections to the server (default = 1)", "<number>" },
  { "idle-connections", 0, 0, G_OPTION_ARG_INT, &idle_connections, "Number of inactive connections to the server (default = 0)", "<number>" },
  { "use-ssl", 'U', 0, G_OPTION_ARG_NONE, &usessl, "Use ssl layer", NULL },
  { "read-file", 'R', 0, G_OPTION_ARG_STRING, &read_file, "Read log messages from file", "<filename>" },
  { "loop-reading", 'l', 0, G_OPTION_ARG_NONE, &loop_reading, "Read the file specified in read-file option in loop (it will restart the reading if reached the end of the file)", NULL },
  { "skip-tokens", 0, 0, G_OPTION_ARG_INT, &skip_tokens, "Skip the given number of tokens (delimined by a space) at the beginning of each line (default value: 3)", "<number>" },
  { "csv", 'C', 0, G_OPTION_ARG_NONE, &csv, "Produce CSV output", NULL },
  { "number", 'n', 0, G_OPTION_ARG_INT, &number_of_messages, "Number of messages to generate", "<number>" },
  { "quiet", 'Q', 0, G_OPTION_ARG_NONE, &quiet, "Don't print the msg/sec data", NULL },
  { "version",   'V', 0, G_OPTION_ARG_NONE, &display_version, "Display version number (" PACKAGE " " COMBINED_VERSION ")", NULL },
  { "rltp",   'L', 0, G_OPTION_ARG_NONE, &rltp, "RLTP transport", NULL },
  { "rltp-batch-size", 'b', 0, G_OPTION_ARG_INT, &rltp_batch_size, "Number of messages send in one rltp package (default value: 100)", NULL },
  { "rltp-compress", 0, 0, G_OPTION_ARG_NONE, &allow_compress, "Use zlib compression in case of rltp transport is used", NULL },
  { "use-port-range", 'e', 0, G_OPTION_ARG_NONE, &use_port_range, "Use custom port for each connections from the specified port to the number of active connections (e.g. from 500-509 if the specifed port is 500 and active-connection=10", NULL },
  { NULL }
};

static GOptionEntry file_option_entries[] =
{
  { "read-file", 'R', 0, G_OPTION_ARG_STRING, &read_file, "Read log messages from file", "<filename>" },
  { "loop-reading",'l',0, G_OPTION_ARG_NONE, &loop_reading, "Read the file specified in read-file option in loop (it will restart the reading if reached the end of the file)", NULL },
  { "dont-parse", 'd', 0, G_OPTION_ARG_NONE, &dont_parse, "Don't parse the lines coming from the readed files. Loggen will send the whole lines as it is in the readed file", NULL },
  { "skip-tokens", 0, 0, G_OPTION_ARG_INT, &skip_tokens, "Skip the given number of tokens (delimined by a space) at the beginning of each line (default value: 3)", "<number>" },
  { NULL }
};

void
version(void)
{
  printf(PACKAGE " " COMBINED_VERSION "\n");
}

/*ignoring the G_GUINT64_FORMAT warning*/
#pragma GCC diagnostic ignored "-Wformat"
int
main(int argc, char *argv[])
{
  int ret = 0;
  GError *error = NULL;
  GOptionContext *ctx = NULL;
  int i;
  guint64 diff_usec;
  GOptionGroup *group;

  g_thread_init(NULL);
  tzset();
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#else
  WSADATA wa;
  WSAStartup(0x0202,&wa);
#endif

  ctx = g_option_context_new(" target port");
  g_option_context_add_main_entries(ctx, loggen_options, 0);


  group = g_option_group_new("file", "File options", "Show file options", NULL, NULL);
  g_option_group_add_entries(group, file_option_entries);
  g_option_context_add_group(ctx, group);

  if (!g_option_context_parse(ctx, &argc, &argv, &error))
    {
      fprintf(stderr, "Option parsing failed: %s\n", error->message);
      return 1;
    }

  if (display_version)
    {
      version();
      return 0;
    }

  if (active_connections <= 0)
    {
      fprintf(stderr, "Minimum value of active-connections must be greater than 0\n");
      return 1;
    }

  if (unix_socket_i)
    unix_socket = 0;
  if (unix_socket_x)
    unix_socket = 1;

  if (sock_type_d)
    sock_type = SOCK_DGRAM;
  if (sock_type_s)
    sock_type = SOCK_STREAM;

  if (message_length > MAX_MESSAGE_LENGTH)
    {
      fprintf(stderr, "Message size too large, limiting to %d\n", MAX_MESSAGE_LENGTH);
      message_length = MAX_MESSAGE_LENGTH;
    }

  if (usessl)
    {
      /* Initialize SSL library */
      OpenSSL_add_ssl_algorithms();
      if (NULL == (ssl_ctx = SSL_CTX_new(SSLv23_client_method())))
        return 1;
    }
    SSL_load_error_strings();
    ERR_load_crypto_strings();
  if (syslog_proto || rltp)
    framing = 1;

  if (read_file != NULL)
    {
      if (read_file[0] == '-' && read_file[1] == '\0')
        {
          if (active_connections > 1)
            {
              fprintf(stderr, "Warning: more than one active connection is not allowed if reading from stdin was specified. active-connections = '%d', new active-connections = '1'\n", active_connections);
              active_connections = 1;
            }
        }
    }

  /* skip the program name after parsing - argc is at least 1 with argv containing the program name
     we will only have the parameters after the '--' (if any) */
  --argc;
  ++argv;

  if (unix_socket && usessl)
    {
      fprintf(stderr, "Error: trying to use SSL on a Unix Domain Socket\n");
      return 1;
    }
  if (!unix_socket)
    {
      if (argc < 2)
        {
          fprintf(stderr, "No target server or port specified\n");
          return 1;
        }

      if (1)
        {
#if HAVE_GETADDRINFO
          struct addrinfo hints;
          struct addrinfo *res;

          memset(&hints, 0, sizeof(hints));
          hints.ai_family = use_ipv6 ? AF_INET6 : AF_INET;
          hints.ai_socktype = sock_type;
#ifdef AI_ADDRCONFIG
          hints.ai_flags = AI_ADDRCONFIG;
#endif
          hints.ai_protocol = 0;
          if (getaddrinfo(argv[0], argv[1], &hints, &res) != 0)
            {
              fprintf(stderr, "Name lookup error\n");
              return 2;
            }

          dest_addr = res->ai_addr;
          dest_addr_len = res->ai_addrlen;
#else
          struct hostent *he;
          struct servent *se;
          static struct sockaddr_in s_in;

          he = gethostbyname(argv[0]);
          if (!he)
            {
              fprintf(stderr, "Name lookup error\n");
              return 2;
            }
          s_in.sin_family = AF_INET;
          s_in.sin_addr = *(struct in_addr *) he->h_addr;

          se = getservbyname(argv[1], sock_type == SOCK_STREAM ? "tcp" : "udp");
          if (se)
            s_in.sin_port = se->s_port;
          else
            s_in.sin_port = htons(atoi(argv[1]));

          dest_addr = (struct sockaddr *) &s_in;
          dest_addr_len = sizeof(s_in);
#endif
        }
    }
  else
    {
#ifndef _WIN32
      static struct sockaddr_un saun;

      saun.sun_family = AF_UNIX;
      strncpy(saun.sun_path, argv[0], sizeof(saun.sun_path));

      dest_addr = (struct sockaddr *) &saun;
      dest_addr_len = sizeof(saun);
#endif
    }
  if (active_connections + idle_connections > 10000)
    {
      fprintf(stderr, "Loggen doesn't support more than 10k threads.\n");
      return 2;
    }
  thread_setup();
  /* used for startup & to signal inactive threads to exit */
  thread_cond = g_cond_new();
  /* active threads signal when they are ready */
  thread_finished = g_cond_new();
  thread_connected = g_cond_new();
  /* mutex used for both cond vars */
  thread_lock = g_mutex_new();
  if(csv)
    {
      printf("ThreadId;Time;Rate;Count\n");
    }

  for (i = 0; i < idle_connections; i++)
    {
      if (!g_thread_create_full(idle_thread, NULL, 1024 * 64, FALSE, FALSE, G_THREAD_PRIORITY_NORMAL, NULL))
        goto stop_and_exit;
    }
  rltp_chunk_counters = g_malloc0(active_connections * sizeof(int));
  for (i = 0; i < active_connections; i++)
    {
      if (!g_thread_create_full(active_thread, GINT_TO_POINTER(i), 1024 * 64, FALSE, FALSE, G_THREAD_PRIORITY_NORMAL, NULL))
        goto stop_and_exit;
    }

  g_mutex_lock(thread_lock);
  while (connect_finished < active_connections + idle_connections)
    g_cond_wait(thread_connected, thread_lock);

  /* tell everyone to start */
  threads_start = TRUE;
  g_cond_broadcast(thread_cond);

  /* wait until active ones finish */
  while (active_finished < active_connections)
    g_cond_wait(thread_finished, thread_lock);

  /* tell inactive ones to exit (active ones exit automatically) */
  threads_stop = TRUE;
  g_cond_broadcast(thread_cond);
  g_mutex_unlock(thread_lock);

  sum_time.tv_sec /= active_connections;
  sum_time.tv_usec /= active_connections;
  diff_usec = sum_time.tv_sec * USEC_PER_SEC + sum_time.tv_usec;

  if (diff_usec == 0)
    {
      diff_usec = 1;
    }

  fprintf(stderr, "average rate = %.2f msg/sec, count=%"G_GUINT64_FORMAT", time=%ld.%03ld, (average) msg size=%ld, bandwidth=%.2f kB/sec\n",
        (double)sum_count * USEC_PER_SEC / diff_usec, sum_count,
        (long int)sum_time.tv_sec, (long int)sum_time.tv_usec / 1000,
        (long int)raw_message_length, (double)sum_count * raw_message_length * (USEC_PER_SEC / 1024) / diff_usec);

stop_and_exit:
  SSL_CTX_free (ssl_ctx);
  threads_start = TRUE;
  threads_stop = TRUE;
  g_mutex_lock(thread_lock);
  g_cond_broadcast(thread_cond);
  g_mutex_unlock(thread_lock);

  return ret;
}
#pragma GCC diagnostic warning "-Wformat"
