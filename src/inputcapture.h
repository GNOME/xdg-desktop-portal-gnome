/*
 * Copyright © 2022 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <stdint.h>

typedef struct _InputCaptureZone
{
  uint32_t width;
  uint32_t height;
  int32_t x;
  int32_t y;
} InputCaptureZone;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (InputCaptureZone, g_free);

gboolean input_capture_init (GDBusConnection  *connection,
                             GError          **error);
