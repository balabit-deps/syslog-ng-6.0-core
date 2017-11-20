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
