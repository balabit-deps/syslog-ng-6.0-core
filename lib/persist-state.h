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

#ifndef PERSIST_STATE_H_INCLUDED
#define PERSIST_STATE_H_INCLUDED

#include "syslog-ng.h"

typedef struct _PersistState PersistState;
typedef guint32 PersistEntryHandle;
typedef struct _rcptcfg rcptcfg;

typedef enum _PersistStateMode {
  persist_mode_normal = 0,
  persist_mode_dump,
  persist_mode_edit
} PersistStateMode;

gpointer persist_state_map_entry(PersistState *self, PersistEntryHandle handle);
void persist_state_unmap_entry(PersistState *self, PersistEntryHandle handle);

PersistEntryHandle persist_state_alloc_entry(PersistState *self, const gchar *persist_name, gsize alloc_size);
PersistEntryHandle persist_state_lookup_entry(PersistState *self, const gchar *persist_name, gsize *size, guint8 *version);

gchar *persist_state_lookup_string(PersistState *self, const gchar *key, gsize *length, guint8 *version);
gboolean persist_state_rename_entry(PersistState *self, const gchar *old_key, const gchar *new_key);
void persist_state_alloc_string(PersistState *self, const gchar *persist_name, const gchar *value, gssize len);

void persist_state_free_entry(PersistEntryHandle handle);

gboolean persist_state_start(PersistState *self);
gboolean persist_state_commit(PersistState *self);
void persist_state_cancel(PersistState *self);
void persist_state_set_dump_mode(PersistState *self, gboolean dump);
void persist_state_set_mode(PersistState *self, PersistStateMode mode);

PersistState *persist_state_new(const gchar *filename);
void persist_state_free(PersistState *self);

void persist_state_set_global_error_handler(PersistState *self, void (*handler)(void));


PersistState *persist_state_get_rcptcfg_state(void);
PersistEntryHandle persist_state_get_rcptcfg_handle(void);

void persist_state_set_rcptcfg_state(PersistState *state);
void persist_state_set_rcptcfg_handle(PersistEntryHandle handle);

gint persist_state_get_version(PersistState *self);
gsize persist_state_get_allocated_size(PersistState *self, PersistEntryHandle handle);

GList *persist_state_get_key_list(PersistState *self);

#endif
