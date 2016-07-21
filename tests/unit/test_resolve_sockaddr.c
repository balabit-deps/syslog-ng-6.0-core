/*
 * Copyright (c) 2016 Balabit
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

#include "misc.h"
#include "dnscache.h"
#include "apphook.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <pthread.h>

#define DEFAULT_PORT 16514
#define THREAD_NUM 5

#ifdef __GNUC__
#define PRETTY_FUNCTION __PRETTY_FUNCTION__
#else
#define PRETTY_FUNCTION "[]"
#endif

#define TEST_ASSERT(assertion, msg) if (!(assertion)) \
                                      { \
                                        fprintf(stderr,"\n[TID:%x] %s :: %s\n\n", (unsigned int)pthread_self(),  PRETTY_FUNCTION,msg); \
                                        exit(1); \
                                      }

typedef struct _NameResolvingOptions
{
  gboolean use_dns;
  gboolean use_fqdn;
  gboolean use_dns_cache;
  gboolean normalize_hostnames;
}NameResolvingOptions;

struct _IpProcessorFunctor;
typedef void(*DispatchProcessIp)(struct _IpProcessorFunctor *arg, const char *ip);

typedef struct _IpProcessorFunctor
{
  DispatchProcessIp process_ip;
  const char *name;
  void *process_ip_arg;
} IpProcessorFunctor;

void for_each_ip(const char *first_ip, const char *last_ip, const IpProcessorFunctor *fun)
{
  uint32_t start_addr = 0;
  uint32_t end_addr = 0;
  uint32_t i, act_ip;
  char ip_addr_str[INET_ADDRSTRLEN] = {0};

  fprintf(stderr, "for_each_ip(%s, %s, %s)\n", first_ip, last_ip, fun->name);

  assert(inet_pton(AF_INET, first_ip, &start_addr) == 1);
  assert(inet_pton(AF_INET, last_ip, &end_addr) == 1);
  start_addr = htonl(start_addr);
  end_addr = htonl(end_addr);
  for (i = start_addr; i <= end_addr; i++)
    {
      act_ip = htonl(i);
      inet_ntop(AF_INET, &act_ip, ip_addr_str, sizeof(ip_addr_str));
      fun->process_ip((struct _IpProcessorFunctor *)fun, ip_addr_str);
    }
}

void append_ip_to_hosts_file(IpProcessorFunctor *args, const char *ip)
{
  FILE *fp = (FILE *)args->process_ip_arg;
  int ip_parts[4];
  sscanf(ip, "%d.%d.%d.%d", &ip_parts[0], &ip_parts[1], &ip_parts[2], &ip_parts[3]);
  fprintf(fp, "%s\ttesthost_%d-%d-%d-%d\n", ip, ip_parts[0], ip_parts[1], ip_parts[2], ip_parts[3]);
}

void create_test_hosts_file(const char *hostfile_name, const char *first_ip, const char *last_ip)
{
  FILE *fp = fopen(hostfile_name, "wt");
  IpProcessorFunctor ip_fwrite_func;
  ip_fwrite_func.process_ip_arg = fp;
  ip_fwrite_func.process_ip = append_ip_to_hosts_file;
  ip_fwrite_func.name = "create_test_hosts_file";

  for_each_ip(first_ip, last_ip, &ip_fwrite_func);

  fflush(fp);
  fclose(fp);
}

void set_default_name_resolving_options(NameResolvingOptions *resolving_options)
{
  resolving_options->use_dns = TRUE;
  resolving_options->use_fqdn = FALSE;
  resolving_options->use_dns_cache = FALSE;
  resolving_options->normalize_hostnames = FALSE;
}

void resolve_ip_address(const gchar *ip, gchar *hostname, gsize *hostname_length,
                        const NameResolvingOptions *resolving_options)
{
  gchar result[256] = {0};
  gsize result_len = sizeof(result);
  GSockAddr *sock_addr = g_sockaddr_inet_new((gchar *)ip, DEFAULT_PORT);

  resolve_sockaddr(result, &result_len,
                   sock_addr,
                   resolving_options->use_dns,
                   resolving_options->use_fqdn,
                   resolving_options->use_dns_cache,
                   resolving_options->normalize_hostnames);

   g_sockaddr_inet_free(sock_addr);
  *hostname_length = result_len;
  if (hostname_length > 0)
    strncpy(hostname, result, result_len);
}

void should_fail_when_resolved_hostname_not_match(const gchar *ip, const gchar *expected_hostname,
                                                  const NameResolvingOptions *resolving_options)
{
  gchar hname[256] = {0};
  gsize hname_length = 0;
  resolve_ip_address(ip, hname, &hname_length, resolving_options);
  TEST_ASSERT(hname_length > 0, "hostname length > 0 failed");
  TEST_ASSERT(strcmp(hname, expected_hostname) == 0, "hostname != expected_hostname");
}

void should_fail_when_resolved_hostname_not_acceptable(const gchar *ip, const gchar *expected_hostname,
                                                  const NameResolvingOptions *resolving_options)
{
  gchar hname[256] = {0};
  gsize hname_length = 0;
  resolve_ip_address(ip, hname, &hname_length, resolving_options);
  TEST_ASSERT(hname_length > 0, "hostname length > 0 failed");
  TEST_ASSERT((strcmp(hname, expected_hostname) == 0) ||
              (strcmp(ip, hname) == 0), "hostname != expected_hostname");
}

void try_to_resolve_ip_with_dns_cache(IpProcessorFunctor *args, const char *ip)
{
  NameResolvingOptions nro;
  set_default_name_resolving_options(&nro);
  nro.use_dns_cache = TRUE;
  int ip_parts[4];
  char expected_hostname[256];
  sscanf(ip, "%d.%d.%d.%d", &ip_parts[0], &ip_parts[1], &ip_parts[2], &ip_parts[3]);
  sprintf(expected_hostname, "testhost_%d-%d-%d-%d", ip_parts[0], ip_parts[1], ip_parts[2], ip_parts[3]);
  should_fail_when_resolved_hostname_not_acceptable(ip, expected_hostname, &nro);
}

void try_to_resolve_ip_without_dns_cache(IpProcessorFunctor *args, const char *ip)
{
  NameResolvingOptions nro;
  set_default_name_resolving_options(&nro);
  nro.use_dns_cache = FALSE;
  int ip_parts[4];
  char expected_hostname[256];
  sscanf(ip, "%d.%d.%d.%d", &ip_parts[0], &ip_parts[1], &ip_parts[2], &ip_parts[3]);
  sprintf(expected_hostname, "testhost_%d-%d-%d-%d", ip_parts[0], ip_parts[1], ip_parts[2], ip_parts[3]);
  should_fail_when_resolved_hostname_not_match(ip, expected_hostname, &nro);
}

void test_resolve_sockaddr_with_resolvable_address_without_dns_cache()
{
  NameResolvingOptions nro;
  set_default_name_resolving_options(&nro);
  should_fail_when_resolved_hostname_not_match("127.0.0.1", "localhost", &nro);
}

void test_resolve_sockaddr_with_unresolvable_address_without_dns_cache()
{
  NameResolvingOptions nro;
  set_default_name_resolving_options(&nro);
  should_fail_when_resolved_hostname_not_match("0.0.0.0", "0.0.0.0", &nro);
}

void test_resolve_sockaddr_with_resolvable_address_with_dns_cache()
{
  NameResolvingOptions nro;
  set_default_name_resolving_options(&nro);
  nro.use_dns_cache = TRUE;
  create_test_hosts_file("/tmp/hosts", "127.0.0.2", "127.0.0.50");
  dns_cache_global_init(100, 60, 10, "/tmp/hosts");
  dns_cache_tls_init();
  should_fail_when_resolved_hostname_not_match("127.0.0.2", "testhost_127-0-0-2", &nro);
  dns_cache_tls_deinit();
  dns_cache_global_deinit();
}

void test_resolve_sockaddr_with_unresolvable_address_with_dns_cache()
{
  NameResolvingOptions nro;
  set_default_name_resolving_options(&nro);
  nro.use_dns_cache = TRUE;
  create_test_hosts_file("/tmp/hosts", "127.0.0.2", "127.0.0.50");
  dns_cache_global_init(100, 60, 10, "/tmp/hosts");
  dns_cache_tls_init();
  should_fail_when_resolved_hostname_not_match("0.0.0.0", "0.0.0.0", &nro);
  dns_cache_tls_deinit();
  dns_cache_global_deinit();
}

void test_resolve_sockaddr_available_in_dns_cache()
{
  IpProcessorFunctor processor;
  const char *first_ip = "127.0.0.2";
  const char *last_ip = "127.0.127.255";

  processor.process_ip = try_to_resolve_ip_with_dns_cache;
  processor.name = "test_resolve_sockaddr_available_in_dns_cache";

  create_test_hosts_file("/tmp/hosts", first_ip, last_ip);

  dns_cache_global_init(35000, 60, 60, "/tmp/hosts");
  dns_cache_tls_init();

  for_each_ip(first_ip, last_ip, &processor);

  dns_cache_tls_deinit();
  dns_cache_global_deinit();
}

typedef struct _CheckIpThreadFunArgs
{
  const char *first_ip;
  const char *last_ip;
  const IpProcessorFunctor *ip_processor;
} CheckIpThreadFunArgs;

void* check_all_ip_in_dns_cache_thread_fun(void *args)
{
  CheckIpThreadFunArgs *fun_args = (CheckIpThreadFunArgs *)args;
  dns_cache_tls_init();
  for_each_ip(fun_args->first_ip, fun_args->last_ip, fun_args->ip_processor);
  dns_cache_tls_deinit();

  return NULL;
}

void test_resolve_sockaddr_available_in_dns_cache_threaded()
{
  pthread_t threads[THREAD_NUM];
  int i;
  IpProcessorFunctor processor;

  const char *first_ip = "127.0.0.2";
  const char *last_ip = "127.0.127.127";

  processor.name = "test_resolve_sockaddr_available_in_dns_cache_threaded";

  CheckIpThreadFunArgs thread_args =
  {
    .first_ip = first_ip,
    .last_ip = last_ip,
    .ip_processor = &processor
  };

  processor.process_ip = try_to_resolve_ip_with_dns_cache;

  create_test_hosts_file("/tmp/hosts", first_ip, last_ip);

  dns_cache_global_init(32768, 10, 10, "/tmp/hosts");

  for (i=0; i < THREAD_NUM; i++)
    pthread_create(&threads[i], NULL, check_all_ip_in_dns_cache_thread_fun, (void *) &thread_args);

  for (i=0; i < THREAD_NUM; i++)
    pthread_join(threads[i], NULL);

  dns_cache_global_deinit();
}

int main(int argc, char **argv)
{
  app_startup();

  test_resolve_sockaddr_with_resolvable_address_without_dns_cache();
  test_resolve_sockaddr_with_unresolvable_address_without_dns_cache();
  test_resolve_sockaddr_with_resolvable_address_with_dns_cache();
  test_resolve_sockaddr_with_unresolvable_address_with_dns_cache();
  test_resolve_sockaddr_available_in_dns_cache();
  test_resolve_sockaddr_available_in_dns_cache_threaded();

  return 0;
}
