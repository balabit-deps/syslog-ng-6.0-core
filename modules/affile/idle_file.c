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

#include "idle_file.h"

#include <string.h>

IdleFile* idle_file_new(gchar *filename, time_t timestamp)
{
    IdleFile *self = g_new0(IdleFile, 1);
    self->path = g_strdup(filename);
    self->expires = timestamp;

    return self;
}

void idle_file_free(IdleFile *self)
{
   g_free(self->path);
   g_free(self);
}

void idle_file_set_expires(IdleFile *self, time_t timestamp)
{
   self->expires = timestamp;
}

gboolean idle_file_equal_path(IdleFile *a, IdleFile *b)
{
   return !!strcmp(a->path, b->path);
}

gboolean idle_file_expiration_before(IdleFile *a, GTimeVal timestamp)
{
   return difftime(a->expires, timestamp.tv_sec) > 0;
}

gboolean idle_file_expire_earlier(IdleFile *a, IdleFile *b)
{
   return difftime(a->expires, b->expires) > 0;
}
