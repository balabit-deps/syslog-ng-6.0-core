/*
 * Copyright (c) 2002-2015 Balabit
 * Copyright (c) 2009-2015 Viktor Juhász
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

#ifndef NV_PROPERTY_CONTAINER_H_
#define NV_PROPERTY_CONTAINER_H_

#include "property_container.h"

typedef struct _NVPropertyContainer NVPropertyContainer;

PropertyContainer *nv_property_container_new(gpointer owner);

#endif /* NVPROPERTY_CONTAINER_H_ */
