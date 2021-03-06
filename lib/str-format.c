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

#include "str-format.h"

#include <string.h>
#include <ctype.h>

static gchar digits[] = "0123456789abcdef";

static inline gint
format_uint64_base10_rev(gchar *result, gsize result_len, guint64 value)
{
  gchar *p;

  p = result;
  while (result_len > 0 && value > 0)
    {
      *p = digits[value % 10];
      value /= 10;
      p++;
      result_len--;
    }
  return p - result;
}

static inline gint
format_uint32_base10_rev(gchar *result, gsize result_len, guint32 value)
{
  gchar *p;
  gboolean first = TRUE;

  p = result;
  while (first || (result_len > 0 && value > 0))
    {
      *p = digits[value % 10];
      value /= 10;
      p++;
      result_len--;
      first = FALSE;
    }
  return p - result;
}

static inline gint
format_uint32_base16_rev(gchar *result, gsize result_len, guint32 value)
{
  gchar *p;
  gboolean first = TRUE;

  p = result;
  while (first || (result_len > 0 && value > 0))
    {
      *p = digits[value & 0x0F];
      value >>= 4;
      p++;
      result_len--;
      first = FALSE;
    }
  return p - result;
}

static inline gint
format_uint64_base16_rev(gchar *result, gsize result_len, guint64 value)
{
  gchar *p;

  p = result;
  while (result_len > 0 && value > 0)
    {
      *p = digits[value & 0x0F];
      value >>= 4;
      p++;
      result_len--;
    }
  return p - result;
}

gint
format_uint32_padded(GString *result, gint field_len, gchar pad_char, gint base, guint32 value)
{
  gchar num[32];
  gint len, i, pos;

  if (base == 10)
    len = format_uint32_base10_rev(num, sizeof(num), value);
  else if (base == 16)
    len = format_uint32_base16_rev(num, sizeof(num), value);
  else
    return 0;

  if (field_len == 0 || field_len < len)
    field_len = len;

  pos = result->len;
  if (G_UNLIKELY(result->allocated_len < pos + field_len + 1))
    {
      g_string_set_size(result, pos + field_len);
    }
  else
    {
      result->len += field_len;
      result->str[pos + field_len] = 0;
    }

  memset(result->str + pos, pad_char, field_len - len);
  for (i = 0; i < len; i++)
    {
      result->str[pos + field_len - i - 1] = num[i];
    }
  return field_len;
}

gint
format_uint64_padded(GString *result, gint field_len, gchar pad_char, gint base, guint64 value)
{
  gchar num[64];
  gint len, i, pos;

  if (base == 10)
    len = format_uint64_base10_rev(num, sizeof(num), value);
  else if (base == 16)
    len = format_uint64_base16_rev(num, sizeof(num), value);
  else
    return 0;

  if (field_len == 0 || field_len < len)
    field_len = len;

  pos = result->len;
  if (G_UNLIKELY(result->allocated_len < pos + field_len + 1))
    {
      g_string_set_size(result, pos + field_len);
    }
  else
    {
      result->len += field_len;
      result->str[pos + field_len] = 0;
    }

  memset(result->str + pos, pad_char, field_len - len);
  for (i = 0; i < len; i++)
    {
      result->str[pos + field_len - i - 1] = num[i];
    }
  return field_len;
}

gboolean
scan_uint32(const gchar **buf, gint *left, gint field_width, guint32 *num)
{
  guint32 result;

  result = 0;

  while (*left > 0 && field_width > 0)
    {
      if ((**buf) >= '0' && (**buf) <= '9')
        result = result * 10 + ((**buf) - '0');
      else if (!g_ascii_isspace(**buf))
        return FALSE;
      (*buf)++;
      (*left)--;
      field_width--;
    }
  if (field_width != 0)
    return FALSE;
  *num = result;
  return TRUE;
}

gboolean
scan_int(const gchar **buf, gint *left, gint field_width, gint *num)
{
  guint32 value;

  if (!scan_uint32(buf, left, field_width, &value))
    return FALSE;
  *num = (gint) value;
  return TRUE;
}

gboolean
scan_expect_char(const gchar **buf, gint *left, gchar value)
{
  if (*left == 0)
    return FALSE;
  if ((**buf) != value)
    return FALSE;
  (*buf)++;
  (*left)--;
  return TRUE;
}

gboolean
scan_day_abbrev(const gchar **buf, gint *left, gint *wday)
{
  *wday = -1;

  if (*left < 3)
    return FALSE;

  switch (**buf)
    {
    case 'S':
      if (strncasecmp(*buf, "Sun", 3) == 0)
        *wday = 0;
      else if (strncasecmp(*buf, "Sat", 3) == 0)
        *wday = 6;
      break;
    case 'M':
      if (strncasecmp(*buf, "Mon", 3) == 0)
        *wday = 1;
      break;
    case 'T':
      if (strncasecmp(*buf, "Tue", 3) == 0)
        *wday = 2;
      else if (strncasecmp(*buf, "Thu", 3) == 0)
        *wday = 4;
      break;
    case 'W':
      if (strncasecmp(*buf, "Wed", 3) == 0)
        *wday = 3;
      break;
    case 'F':
      if (strncasecmp(*buf, "Fri", 3) == 0)
        *wday = 5;
      break;
    default:
      return FALSE;
    }

  (*buf) += 3;
  (*left) -= 3;
  return TRUE;
}

gboolean
scan_month_abbrev(const gchar **buf, gint *left, gint *mon)
{
  *mon = -1;

  if (*left < 3)
    return FALSE;

  switch (**buf)
    {
    case 'J':
      if (strncasecmp(*buf, "Jan", 3) == 0)
        *mon = 0;
      else if (strncasecmp(*buf, "Jun", 3) == 0)
        *mon = 5;
      else if (strncasecmp(*buf, "Jul", 3) == 0)
        *mon = 6;
      break;
    case 'F':
      if (strncasecmp(*buf, "Feb", 3) == 0)
        *mon = 1;
      break;
    case 'M':
      if (strncasecmp(*buf, "Mar", 3) == 0)
        *mon = 2;
      else if (strncasecmp(*buf, "May", 3) == 0)
        *mon = 4;
      break;
    case 'A':
      if (strncasecmp(*buf, "Apr", 3) == 0)
        *mon = 3;
      else if (strncasecmp(*buf, "Aug", 3) == 0)
        *mon = 7;
      break;
    case 'S':
      if (strncasecmp(*buf, "Sep", 3) == 0)
        *mon = 8;
      break;
    case 'O':
      if (strncasecmp(*buf, "Oct", 3) == 0)
        *mon = 9;
      break;
    case 'N':
      if (strncasecmp(*buf, "Nov", 3) == 0)
        *mon = 10;
      break;
    case 'D':
      if (strncasecmp(*buf, "Dec", 3) == 0)
        *mon = 11;
      break;
    default:
      return FALSE;
    }

  (*buf) += 3;
  (*left) -= 3;
  return TRUE;
}


/* this function parses the date/time portion of an ISODATE */
gboolean
scan_iso_timestamp(const gchar **buf, gint *left, struct tm *tm)
{
  /* YYYY-MM-DDTHH:MM:SS */
  if (!scan_int(buf, left, 4, &tm->tm_year) ||
      !scan_expect_char(buf, left, '-') ||
      !scan_int(buf, left, 2, &tm->tm_mon) ||
      !scan_expect_char(buf, left, '-') ||
      !scan_int(buf, left, 2, &tm->tm_mday) ||
      !scan_expect_char(buf, left, 'T') ||
      !scan_int(buf, left, 2, &tm->tm_hour) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_min) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_sec))
    return FALSE;
  tm->tm_year -= 1900;
  tm->tm_mon -= 1;
  return TRUE;
}

gboolean
scan_pix_timestamp(const gchar **buf, gint *left, struct tm *tm)
{
  /* PIX/ASA timestamp, expected format: MMM DD YYYY HH:MM:SS */
  if (!scan_month_abbrev(buf, left, &tm->tm_mon) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 2, &tm->tm_mday) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 4, &tm->tm_year) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 2, &tm->tm_hour) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_min) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_sec))
    return FALSE;
  tm->tm_year -= 1900;
  return TRUE;
}

gboolean
scan_linksys_timestamp(const gchar **buf, gint *left, struct tm *tm)
{
  /* LinkSys timestamp, expected format: MMM DD HH:MM:SS YYYY */

  if (!scan_month_abbrev(buf, left, &tm->tm_mon) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 2, &tm->tm_mday) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 2, &tm->tm_hour) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_min) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_sec) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 4, &tm->tm_year))
    return FALSE;
  tm->tm_year -= 1900;
  return TRUE;
}

gboolean
scan_bsd_timestamp(const gchar **buf, gint *left, struct tm *tm)
{
  /* RFC 3164 timestamp, expected format: MMM DD HH:MM:SS ... */
  if (!scan_month_abbrev(buf, left, &tm->tm_mon) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 2, &tm->tm_mday) ||
      !scan_expect_char(buf, left, ' ') ||
      !scan_int(buf, left, 2, &tm->tm_hour) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_min) ||
      !scan_expect_char(buf, left, ':') ||
      !scan_int(buf, left, 2, &tm->tm_sec))
    return FALSE;
  return TRUE;
}

void
str_rtrim(gchar * const string, const gchar *characters)
{
  gint i;

  for (i = strlen(string) - 1; i >= 0 && strchr(characters, string[i]) != NULL; --i)
    string[i] = 0;
}
