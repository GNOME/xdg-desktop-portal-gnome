/*
 * Copyright © 2017 Red Hat, Inc
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

#include "displaystatetracker.h"
#include "shellintrospect.h"

typedef enum _ScreenCastSourceType
{
  SCREEN_CAST_SOURCE_TYPE_MONITOR = 1,
  SCREEN_CAST_SOURCE_TYPE_WINDOW = 2,
  SCREEN_CAST_SOURCE_TYPE_VIRTUAL = 4,
} ScreenCastSourceType;

typedef struct _ScreenCastSourceTypes
{
  int monitor : 1;
  int window : 1;
  int virtual_monitor : 1;
} ScreenCastSourceTypes;

ScreenCastSourceTypes screen_cast_source_types_from_flags (uint32_t flags);

typedef enum _ScreenCastCursorMode
{
  SCREEN_CAST_CURSOR_MODE_NONE = 0,
  SCREEN_CAST_CURSOR_MODE_HIDDEN = 1,
  SCREEN_CAST_CURSOR_MODE_EMBEDDED = 2,
  SCREEN_CAST_CURSOR_MODE_METADATA = 4,
} ScreenCastCursorMode;

typedef enum _ScreenCastPersistMode
{
  SCREEN_CAST_PERSIST_MODE_NONE = 0,
  SCREEN_CAST_PERSIST_MODE_TRANSIENT = 1,
  SCREEN_CAST_PERSIST_MODE_PERSISTENT = 2,
} ScreenCastPersistMode;

typedef struct _ScreenCastSelection
{
  gboolean multiple;
  ScreenCastSourceTypes source_types;
  ScreenCastCursorMode cursor_mode;
} ScreenCastSelection;

typedef struct
{
  ScreenCastSourceType type;
  uint32_t id;
  union {
    Monitor *monitor;
    Window *window;
  } data;
} ScreenCastStreamInfo;

#define SCREEN_CAST_STREAMS_VARIANT_TYPE "a(uuv)"

gboolean screen_cast_init (GDBusConnection *connection,
                           GError **error);

void screen_cast_stream_info_free (ScreenCastStreamInfo *info);

void serialize_screen_cast_streams_as_restore_data (GPtrArray       *streams,
                                                    GVariantBuilder *impl_builder);

GPtrArray * restore_screen_cast_streams (GVariantIter *streams_iter,
                                         ScreenCastSelection *screen_cast_selection);
