/*
 * Copyright (c) 2014 Balabit
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

#include "tlswincrypt.h"
#include <openssl/engine.h>

#define CAPI_ENGINE_PATH "lib\\engines\\capieay32"

static X509*
get_X509_cert_from_cert_context(PCCERT_CONTEXT cert)
{
  const unsigned char *p;
  X509 *x;
  p = cert->pbCertEncoded;
  x = d2i_X509(NULL, &p, cert->cbCertEncoded);
  return x;
}

gboolean
load_all_trusted_ca_from_container(X509_STORE *ca_storage, gchar *container_name)
{
  DWORD flags = CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG;
  PCCERT_CONTEXT cert = NULL;
  HCERTSTORE hSystemStore;
  hSystemStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_A, 0, 0, flags, container_name);
  if (hSystemStore == NULL)
    {
      return FALSE;
    }
  cert = CertEnumCertificatesInStore(hSystemStore, NULL);
  while (cert)
    {
      X509_STORE_add_cert(ca_storage, get_X509_cert_from_cert_context(cert));
      cert = CertEnumCertificatesInStore(hSystemStore, cert);
    }
  CertCloseStore(hSystemStore, 0);
  return TRUE;
}

gboolean
load_all_trusted_ca_certificates(SSL_CTX *ctx)
{
  X509_STORE *ca_storage = SSL_CTX_get_cert_store(ctx);
  gchar *stores[] = {"Root", "CA", NULL};
  gint i;

  g_assert(ca_storage);

  for (i = 0; stores[i] != NULL; i++)
    {
      if(!load_all_trusted_ca_from_container(ca_storage, stores[i]))
        {
          return FALSE;
        }
    }
  return TRUE;
}

gboolean
load_all_crls(SSL_CTX *ctx)
{
  HCERTSTORE hSystemStore;
  DWORD flags = CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG;
  X509_STORE *ca_storage = SSL_CTX_get_cert_store(ctx);
  PCCRL_CONTEXT pCrl = NULL;
  gboolean crl_found = FALSE;

  g_assert(ca_storage);

  hSystemStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_A, 0, 0, flags, "MY");
  if (hSystemStore == NULL)
    {
      return FALSE;
    }
  pCrl = CertEnumCRLsInStore(hSystemStore, NULL);
  while (pCrl)
    {
      const unsigned char *p;
      X509_CRL *x;
      p = pCrl->pbCrlEncoded;
      x = d2i_X509_CRL(NULL, &p, pCrl->cbCrlEncoded);
      X509_STORE_add_crl(ca_storage, x);
      pCrl = CertEnumCRLsInStore(hSystemStore, pCrl);
      crl_found = TRUE;
    }
  CertCloseStore(hSystemStore, 0);
  return crl_found;
}

static int
get_storage_id_by_flag(DWORD flag)
{
  int result = 0;
  switch (flag)
    {
      case CERT_SYSTEM_STORE_LOCAL_MACHINE:
        result = 1;
        break;
      case CERT_SYSTEM_STORE_SERVICES:
        result = 2;
        break;
      case CERT_SYSTEM_STORE_CURRENT_USER:
        result = 0;
        break;
      default:
        {
          g_assert_not_reached();
        }
    }
  return result;
}

static X509*
get_X509_cert_by_subject(const gchar *cert_subject, ENGINE *e)
{
  PCCERT_CONTEXT cert = NULL;
  X509 *result = NULL;
  DWORD storages[] = {
                      CERT_SYSTEM_STORE_LOCAL_MACHINE,
                      CERT_SYSTEM_STORE_SERVICES,
                      CERT_SYSTEM_STORE_CURRENT_USER,
                     };
  int i;

  for (i = 0; i < 3; i++)
    {
      DWORD flags = CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG | storages[i];
      HCERTSTORE hSystemStore;
      DWORD encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

      hSystemStore = CertOpenStore(CERT_STORE_PROV_SYSTEM_A, 0, 0, flags, "MY");
      if (hSystemStore == NULL)
        {
          continue;
        }
      cert = CertFindCertificateInStore(hSystemStore, encoding, 0, CERT_FIND_SUBJECT_STR_A, cert_subject, NULL);
      if (cert)
        {
          int id = get_storage_id_by_flag(storages[i]);
          result = get_X509_cert_from_cert_context(cert);

          CertCloseStore(hSystemStore, 0);
          ENGINE_ctrl_cmd(e,"store_flags", id, NULL, NULL, 0);
          break;
        }
      CertCloseStore(hSystemStore, 0);
    }

  if (cert == NULL)
    {
      msg_error("Can't find cert",
                evt_tag_str("Searched subject",cert_subject),
                evt_tag_id(MSG_CANT_FIND_CERTIFICATE),
                NULL);
    }
  return result;
}

static gchar *
get_capi_engine_path()
{
  GString *string_result = NULL;
  char *result;
  char currDirectory[_MAX_PATH];
  char *pIdx;
  GetModuleFileName(NULL, currDirectory, _MAX_PATH);
  pIdx = strrchr(currDirectory, '\\');
  if (pIdx)
    *pIdx = '\0';
  pIdx = strrchr(currDirectory, '\\');
  if (pIdx && ((strcmp((pIdx + 1),"bin")==0) || (strcmp((pIdx + 1),"lib")==0)))
      {
        *pIdx = '\0';
      }
  string_result = g_string_new(currDirectory);
  g_string_append_printf(string_result,"\\%s",CAPI_ENGINE_PATH);
  result = string_result->str;
  g_string_free(string_result, FALSE);
  return result;
}

static ENGINE *
init_capi_engine()
{
  ENGINE *e = NULL;
  const char *engine_id = "dynamic";
  gchar *engine_path = get_capi_engine_path();

  ENGINE_load_builtin_engines();
  e = ENGINE_by_id(engine_id);
  if (!e)
    {
      msg_error("Can't load dynamic engine",evt_tag_id(MSG_CANT_LOAD_DYNAMIC_ENGINE), NULL);
      return NULL;
    }

  if (!ENGINE_ctrl_cmd_string(e, "SO_PATH", engine_path, 0))
    {
      msg_error("Can't find capi engine",evt_tag_str("searched file",engine_path), evt_tag_id(MSG_CANT_FIND_CAPI_ENGINE), NULL);
      g_free(engine_path);
      ENGINE_free(e);
      return NULL;
    }
  if (!ENGINE_ctrl_cmd_string(e, "LOAD", NULL, 0))
    {
      msg_error("Can't load capi engine",evt_tag_id(MSG_CANT_LOAD_CAPI_ENGINE), NULL);
      ENGINE_free(e);
      return NULL;
    }
  g_free(engine_path);
  ENGINE_init(e);
  return e;
}

gboolean
load_certificate(SSL_CTX *ctx,const gchar *cert_subject)
{
  EVP_PKEY *private_key = NULL;
  X509 *cert = NULL;
  ENGINE *e = NULL;

  e = init_capi_engine();
  if (e == NULL)
    {
      return FALSE;
    }

  cert = get_X509_cert_by_subject(cert_subject, e);
  if (cert == NULL)
    {
      return FALSE;
    }

  private_key = ENGINE_load_private_key(e, cert_subject, NULL, NULL);
  if (private_key == NULL)
    {
      msg_error("Can't find private key", evt_tag_id(MSG_CANT_FIND_PRIVATE_KEY), NULL);
      return FALSE;
    }

  SSL_CTX_use_PrivateKey(ctx, private_key);
  SSL_CTX_use_certificate(ctx, cert);
  return TRUE;
}
