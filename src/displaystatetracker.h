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

#include <gtk/gtk.h>
#include <glib-object.h>

typedef struct _Monitor Monitor;
typedef struct _LogicalMonitor LogicalMonitor;

typedef struct
{
  char *label;
  gboolean primary;
  graphene_rect_t rect;
} MonitorIllustration;


#define DISPLAY_TYPE_STATE_TRACKER display_state_tracker_get_type ()

G_DECLARE_FINAL_TYPE (DisplayStateTracker, display_state_tracker,
                      DISPLAY, STATE_TRACKER, GObject)

void                       monitor_free (Monitor *monitor);

Monitor                   *monitor_dup (Monitor *monitor);

const char                *monitor_get_connector      (Monitor *monitor);

const char                *monitor_get_match_string   (Monitor *monitor);

const char                *monitor_get_display_name   (Monitor *monitor);

int                        monitor_get_number (Monitor *monitor);

GList                     *logical_monitor_get_monitors (LogicalMonitor *logical_monitor);

const MonitorIllustration *logical_monitor_get_illustration (LogicalMonitor *logical_monitor);

gboolean                   logical_monitor_is_primary (LogicalMonitor *logical_monitor);

GList                     *display_state_tracker_get_logical_monitors (DisplayStateTracker *tracker);

DisplayStateTracker       *display_state_tracker_get (void);
