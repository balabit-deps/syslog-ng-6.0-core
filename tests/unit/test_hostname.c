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

#include "logmsg.h"
#include "cfg.h"
#include "plugin.h"
#include "testutils.h"
#include "config.h"
#include <stdio.h>

void reset_hostname(gchar *hostname,int length)
{
  memset(hostname,'t',length);
}

void test_apply_custom_domain()
{
  gchar orig_hostname[256];
  gchar max_domain[256];

  reset_hostname(orig_hostname,256);
  strcpy(orig_hostname,"testhost");
  apply_custom_domain(orig_hostname,256,NULL);
  assert_string(orig_hostname,"testhost","Bad custom domain working");

  reset_hostname(orig_hostname,256);
  strcpy(orig_hostname,"testhost.balabit");
  apply_custom_domain(orig_hostname,256,NULL);
  assert_string(orig_hostname,"testhost.balabit","Bad custom domain working");

  reset_hostname(orig_hostname,256);
  strcpy(orig_hostname,"testhost");
  apply_custom_domain(orig_hostname,256,"balabit.hu");
  assert_string(orig_hostname,"testhost.balabit.hu","Bad custom domain working");

  reset_hostname(orig_hostname,256);
  strcpy(orig_hostname,"testhost.balabitdomain.hu");
  apply_custom_domain(orig_hostname,256,"balabit.hu");
  assert_string(orig_hostname,"testhost.balabit.hu","Bad custom domain working");

  reset_hostname(orig_hostname,256);
  reset_hostname(max_domain,250);
  strncpy(&max_domain[250],"xyzga",6);
  strcpy(orig_hostname,"h");
  apply_custom_domain(orig_hostname,256,max_domain);
  assert_nstring(orig_hostname,5,"h.ttt",5,"Bad custom domain working");
  assert_nstring(&orig_hostname[250],6,"ttxyz",6,"Bad custom domain working");
}

void reset_configuration()
{
  configuration->use_fqdn = FALSE;
  configuration->normalize_hostnames = FALSE;
  g_free(configuration->custom_domain);
  configuration->custom_domain = NULL;
}

void test_format_hostname()
{
  gchar *hostname = NULL;
  gchar *result = NULL;

  reset_configuration();
  hostname = "TestHost";
  result = format_hostname(hostname,NULL,configuration);
  assert_string(result,hostname,"%s:%d(%s)",__FILE__,__LINE__,__FUNCTION__);
  g_free(result);

  reset_configuration();
  hostname = "testhost.domain_name.hu";
  result = format_hostname(hostname,NULL,configuration);
  assert_string(result,"testhost","%s:%d(%s)",__FILE__,__LINE__,__FUNCTION__);
  g_free(result);

  reset_configuration();
  hostname = "testhost";
  configuration->use_fqdn = TRUE;
  result = format_hostname(hostname,"Test.Hu",configuration);
  assert_string(result,"testhost.Test.Hu","%s:%d(%s)",__FILE__,__LINE__,__FUNCTION__);
  g_free(result);

  reset_configuration();
  hostname = "testhost.domain_name.hu";
  configuration->use_fqdn = TRUE;
  configuration->normalize_hostnames = TRUE;
  result = format_hostname(hostname,"Test.Hu",configuration);
  assert_string(result,"testhost.test.hu","%s:%d(%s)",__FILE__,__LINE__,__FUNCTION__);
  g_free(result);

  reset_configuration();
  hostname = "TestHOst.domain_name.hu";
  configuration->use_fqdn = TRUE;
  configuration->normalize_hostnames = TRUE;
  result = format_hostname(hostname,"Test.Hu",configuration);
  assert_string(result,"testhost.test.hu","%s:%d(%s)",__FILE__,__LINE__,__FUNCTION__);
  g_free(result);
}

int
main(int argc G_GNUC_UNUSED, char *argv[] G_GNUC_UNUSED)
{
  log_template_global_init();
  log_msg_registry_init();
  configuration = cfg_new(VERSION_VALUE_3_2);
  test_apply_custom_domain();
  test_format_hostname();
  return 0;
}
