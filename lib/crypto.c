/*
 * Copyright (c) 2011-2013 Balabit
 * Copyright (c) 2011-2013 Balázs Scheidler
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
 */

/* This file becomes part of libsyslog-ng-crypto.so, the shared object
 * that contains all crypto related stuff to be used by plugins. This
 * includes the TLS wrappers, random number initialization, and so on.
 */

#include "crypto.h"
#include "apphook.h"

#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdio.h>

static gboolean randfile_loaded;

void
crypto_deinit(void)
{
  char rnd_file[256];

  if (randfile_loaded)
    {
      RAND_file_name(rnd_file, sizeof(rnd_file));
      if (rnd_file[0])
        RAND_write_file(rnd_file);
    }
}

INITIALIZER(crypto_init)
{
  if (RAND_status() < 0 || getenv("RANDFILE"))
    {
      char rnd_file[256];

      RAND_file_name(rnd_file, sizeof(rnd_file));
      if (rnd_file[0])
        {
          RAND_load_file(rnd_file, -1);
          randfile_loaded = TRUE;
        }

      if (RAND_status() < 0)
        fprintf(stderr, "WARNING: a trusted random number source is not available, crypto operations will probably fail. Please set the RANDFILE environment variable.");
    }
#ifndef _WIN32
  register_application_hook(AH_SHUTDOWN, (ApplicationHookFunc) crypto_deinit, NULL);
#endif
}

/* the crypto options (seed) are handled in main.c */
