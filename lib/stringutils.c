/*
 * Copyright (c) 2015 Balabit
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

#include "stringutils.h"
#include "misc.h"

#include <string.h>

typedef struct _StringSlice {
  char *start;
  int length;
  const char *string;
} StringSlice;

static void
_find_string(gpointer data, gpointer u_data)
{
  const char *item = (const char*) data;
  StringSlice *user_data = (StringSlice*) u_data;

  if (!user_data->start)
  {
    user_data->start = strstr(user_data->string, item);
    user_data->length = (user_data->start) ? strlen(item) : 0;
  }
}

/* searches for str in list and returns the first occurence, otherwise NULL */
guchar*
g_string_list_find_first(GList *list, const char * str, int *result_length)
{
  StringSlice user_data = {NULL, 0, str};

  g_list_foreach(list, _find_string, (gpointer) &user_data);

  *result_length = user_data.length;

  return (guchar*) user_data.start;
}

gchar *
normalize_key(const gchar* buffer)
{
  return normalize_option_name(strdup(buffer));
}
