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

#ifndef STR_FORMAT_H_INCLUDED
#define STR_FORMAT_H_INCLUDED

#include "syslog-ng.h"

gint
format_uint32_padded(GString *result, gint field_len, gchar pad_char, gint base, guint32 value);

gint
format_uint64_padded(GString *result, gint field_len, gchar pad_char, gint base, guint64 value);

gboolean
scan_iso_timestamp(const gchar **buf, gint *left, struct tm *tm);
gboolean
scan_pix_timestamp(const gchar **buf, gint *left, struct tm *tm);
gboolean
scan_linksys_timestamp(const gchar **buf, gint *left, struct tm *tm);
gboolean
scan_bsd_timestamp(const gchar **buf, gint *left, struct tm *tm);

void
str_rtrim(gchar * const string, const gchar *characters);

#endif
