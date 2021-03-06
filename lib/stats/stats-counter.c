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

#include "stats.h"
#include "hds.h"

static void
_reset_counters(gpointer key, gpointer value, gpointer user_data)
{
  StatsCounter     *sc = (StatsCounter*)value;
  StatsCounterType type;

  for (type = 0; type < SC_TYPE_MAX; ++type)
    {
      if (type != SC_TYPE_STORED)
        {
          stats_counter_set(&sc->counters[type], 0);
        }
    }
}

void
stats_reset_counters(void)
{
  hds_lock();
  g_hash_table_foreach(counter_static_hash, _reset_counters, NULL);
  g_hash_table_foreach(counter_dynamic_hash, _reset_counters, NULL);
  hds_unlock();
}
