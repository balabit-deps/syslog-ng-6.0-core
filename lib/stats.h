/*
 * Copyright (c) 2002-2012 Balabit
 * Copyright (c) 1998-2012 Balázs Scheidler
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

#ifndef STATS_H_INCLUDED
#define STATS_H_INCLUDED

#include "syslog-ng.h"
#include "cfg.h"
#include "mainloop.h"
#include "property_container.h"
#include "hds.h"

extern GHashTable *counter_static_hash;
extern GHashTable *counter_dynamic_hash;

typedef enum
{
  SC_TYPE_MIN,
  SC_TYPE_DROPPED=0, /* number of messages dropped */
  SC_TYPE_PROCESSED, /* number of messages processed */
  SC_TYPE_STORED,    /* number of messages on disk */
  SC_TYPE_SUPPRESSED,/* number of messages suppressed */
  SC_TYPE_STAMP,     /* timestamp */
  SC_TYPE_MAX
} StatsCounterType;

enum
{
  SCS_SOURCE         = 0x0100,
  SCS_DESTINATION    = 0x0200,
  SCS_FILE           = 1,
  SCS_PIPE           = 2,
  SCS_TCP            = 3,
  SCS_UDP            = 4,
  SCS_TCP6           = 5,
  SCS_UDP6           = 6,
  SCS_UNIX_STREAM    = 7,
  SCS_UNIX_DGRAM     = 8,
  SCS_SYSLOG         = 9,
  SCS_INTERNAL       = 10,
  SCS_LOGSTORE       = 11,
  SCS_PROGRAM        = 12,
  SCS_SQL            = 13,
  SCS_SUN_STREAMS    = 14,
  SCS_USERTTY        = 15,
  SCS_GROUP          = 16,
  SCS_CENTER         = 17,
  SCS_HOST           = 18,
  SCS_GLOBAL         = 19,
  SCS_MONGODB        = 20,
  SCS_CLASS          = 21,
  SCS_RULE_ID        = 22,
  SCS_TAG            = 23,
  SCS_SEVERITY       = 24,
  SCS_FACILITY       = 25,
  SCS_SENDER         = 26,
  SCS_SNMP           = 27,
  SCS_SMTP           = 28,
  SCS_JOURNALD       = 29,
  SCS_JAVA           = 30,
  SCS_MAX,
  SCS_SOURCE_MASK    = 0xff
};

typedef struct _StatsCounterItem
{
  Property super;
  gint value;
  GString *value_str;
} StatsCounterItem;

typedef struct _StatsCounter
{
  PropertyContainer super;
  StatsCounterItem counters[SC_TYPE_MAX];
  guint16 ref_cnt;
  guint16 source;
  gchar *id;
  gchar *instance;
  guint16 live_mask;
  guint16 dynamic:1;
} StatsCounter;

extern gint current_stats_level;
extern gint current_stats_max_dynamic;

void stats_generate_log(void);
gchar *stats_generate_csv(void);
void stats_register_counter(gint level, gint source, const gchar *id, const gchar *instance, StatsCounterType type, StatsCounterItem **counter);
StatsCounter *
stats_register_dynamic_counter(gint stats_level, gint source, const gchar *id, const gchar *instance, StatsCounterType type, StatsCounterItem **counter, gboolean *new);
void stats_instant_inc_dynamic_counter(gint stats_level, gint source_mask, const gchar *id, const gchar *instance, time_t timestamp);
void stats_register_associated_counter(StatsCounter *handle, StatsCounterType type, StatsCounterItem **counter);
void stats_unregister_counter(gint source, const gchar *id, const gchar *instance, StatsCounterType type, StatsCounterItem **counter);
void stats_unregister_dynamic_counter(StatsCounter *handle, StatsCounterType type, StatsCounterItem **counter);
void stats_cleanup_orphans(void);

void stats_counter_inc_pri(guint16 pri);

void stats_set_stats_level(gint stats_level);
void stats_set_max_dynamic(gint stats_max_dynamic);
void stats_reinit(GlobalConfig *cfg);
void stats_init(void);
void stats_destroy(void);

static inline gboolean
stats_check_level(gint level)
{
  return (current_stats_level >= level);
}

static inline gint
stats_number_of_current_dynamic(void)
{
  return g_hash_table_size(counter_dynamic_hash);
}

static inline gint
stats_number_of_max_dynamic(void)
{
  return current_stats_max_dynamic;
}

static inline gboolean
stats_check_dynamic_cluster_limit(guint number_of_clusters)
{
  if (current_stats_max_dynamic < 0)
    return TRUE;

  return (number_of_clusters < current_stats_max_dynamic);
}

static inline void
stats_lock()
{
  hds_lock();
}

static inline void
stats_unlock()
{
  hds_unlock();
}

static inline void
stats_counter_add(StatsCounterItem *counter, gint add)
{
  if (counter)
    g_atomic_int_add(&counter->value, add);
}

static inline void
stats_counter_inc(StatsCounterItem *counter)
{
  if (counter)
    g_atomic_int_inc(&counter->value);
}

static inline void
stats_counter_dec(StatsCounterItem *counter)
{
  if (counter)
    g_atomic_int_add(&counter->value, -1);
}

/* NOTE: this is _not_ atomic and doesn't have to be as sets would race anyway */
static inline void
stats_counter_set(StatsCounterItem *counter, guint32 value)
{
  if (counter)
    counter->value = value;
}

/* NOTE: this is _not_ atomic and doesn't have to be as sets would race anyway */
static inline guint32
stats_counter_get(StatsCounterItem *counter)
{
  guint32 result = 0;

  if (counter)
    result = counter->value;
  return result;
}
#endif
