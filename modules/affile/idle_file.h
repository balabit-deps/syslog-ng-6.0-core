/*
 * Copyright (c) 2002-2017 Balabit
 * Copyright (c) 2017 Kokan
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

#ifndef IDLE_FILE_H_INCLUDED
#define IDLE_FILE_H_INCLUDED

#include <glib.h>
#include <time.h>

typedef struct _IdleFile IdleFile;

struct _IdleFile {
  gchar    *path;
  time_t    expires;
};


IdleFile* idle_file_new(const gchar *filename, time_t timestamp);
void idle_file_free(IdleFile *self);
void idle_file_set_expires(IdleFile *self, time_t timestamp);
gboolean idle_file_equal_path(IdleFile *a, IdleFile *b);
gboolean idle_file_expiration_before(IdleFile *a, GTimeVal timestamp);
gboolean idle_file_expire_earlier(IdleFile *a, IdleFile *b);


#endif
