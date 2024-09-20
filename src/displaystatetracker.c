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

#include "config.h"

#include <math.h>
#include <gio/gio.h>
#include <stdint.h>

#include "shell-dbus.h"
#include "displaystatetracker.h"

enum
{
  MONITORS_CHANGED,

  N_SIGNALS
};

typedef enum _ScDisplayRotation
{
  SC_DISPLAY_ROTATION_NONE,
  SC_DISPLAY_ROTATION_90,
  SC_DISPLAY_ROTATION_180,
  SC_DISPLAY_ROTATION_270,
  SC_DISPLAY_ROTATION_FLIPPED,
  SC_DISPLAY_ROTATION_90_FLIPPED,
  SC_DISPLAY_ROTATION_180_FLIPPED,
  SC_DISPLAY_ROTATION_270_FLIPPED,
} ScDisplayRotation;

/* Equivalent to the 'layout-mode' enum in org.gnome.Mutter.DisplayConfig */
typedef enum _MetaLogicalMonitorLayoutMode
{
  META_LOGICAL_MONITOR_LAYOUT_MODE_LOGICAL = 1,
  META_LOGICAL_MONITOR_LAYOUT_MODE_PHYSICAL = 2
} MetaLogicalMonitorLayoutMode;

static guint signals[N_SIGNALS];

typedef struct _Monitor
{
  int width;
  int height;
  /* Montior number shown in the UI corresponding to the one in gnome-control-center */
  int number;
  char *connector;
  char *match_string;
  char *display_name;
} Monitor;

typedef struct _LogicalMonitor
{
  int x;
  int y;
  double scale;
  ScDisplayRotation rotation;
  gboolean is_primary;
  GList *monitors;
  MonitorIllustration illustration;
} LogicalMonitor;

struct _DisplayStateTracker
{
  GObject parent;

  guint display_config_watch_name_id;
  GCancellable *cancellable;

  OrgGnomeMutterDisplayConfig *proxy;

  GHashTable *monitors;
  GList *logical_monitors;
  uint32_t layout_mode;
};

G_DEFINE_TYPE (DisplayStateTracker, display_state_tracker, G_TYPE_OBJECT)

static DisplayStateTracker *tracker_object;

void
monitor_free (Monitor *monitor)
{
  g_free (monitor->connector);
  g_free (monitor->match_string);
  g_free (monitor->display_name);
  g_free (monitor);
}

Monitor *
monitor_dup (Monitor *monitor)
{
  Monitor *new_monitor;

  new_monitor = g_new0 (Monitor, 1);
  new_monitor->width = monitor->width;
  new_monitor->height = monitor->height;
  new_monitor->number = monitor->number;
  new_monitor->connector = g_strdup (monitor->connector);
  new_monitor->match_string = g_strdup (monitor->match_string);
  new_monitor->display_name = g_strdup (monitor->display_name);

  return new_monitor;
}


static void
logical_monitor_free (LogicalMonitor *logical_monitor)
{
  g_clear_pointer (&logical_monitor->illustration.label, g_free);
  g_clear_pointer (&logical_monitor->monitors, g_list_free);
  g_free (logical_monitor);
}

const char *
monitor_get_connector (Monitor *monitor)
{
  return monitor->connector;
}

const char *
monitor_get_match_string (Monitor *monitor)
{
  return monitor->match_string;
}

const char *
monitor_get_display_name (Monitor *monitor)
{
  return monitor->display_name;
}

GList *
logical_monitor_get_monitors (LogicalMonitor *logical_monitor)
{
  return logical_monitor->monitors;
}

int
monitor_get_number (Monitor *monitor)
{
  return monitor->number;
}

gboolean
logical_monitor_is_primary (LogicalMonitor *logical_monitor)
{
  return logical_monitor->is_primary;
}

GList *
display_state_tracker_get_logical_monitors (DisplayStateTracker *tracker)
{
  return tracker->logical_monitors;
}

const MonitorIllustration *
logical_monitor_get_illustration (LogicalMonitor *logical_monitor)
{
  return &logical_monitor->illustration;
}

static void
update_monitor_illustration (DisplayStateTracker *tracker,
                             LogicalMonitor      *logical_monitor)
{
  g_autoptr(GString) label = NULL;
  Monitor *first_monitor = NULL;
  MonitorIllustration *illustration;
  int illustration_width, illustration_height;
  GList *l;

  g_assert (logical_monitor != NULL);

  illustration = &logical_monitor->illustration;

  /* Build label from all monitors of the logical monitor */
  label = g_string_new (NULL);
  for (l = logical_monitor->monitors; l; l = l->next)
    {
      if (!first_monitor)
        first_monitor = l->data;
      Monitor *monitor = l->data;

      g_string_append (label, monitor_get_display_name (monitor));

      if (l->next)
        g_string_append (label, "\n");
    }

  g_assert (first_monitor != NULL);

  g_clear_pointer (&illustration->label, g_free);
  illustration->label = g_strdup (label->str);
  illustration->primary = logical_monitor->is_primary;

  /* Assign width and height (conditionally flipped) */
  if (tracker->layout_mode == META_LOGICAL_MONITOR_LAYOUT_MODE_PHYSICAL)
    {
      /* In physical layout mode dimensions are independent from scale factors. */
      illustration_width = first_monitor->width;
      illustration_height = first_monitor->height;
    }
  else
    {
      illustration_width = round (first_monitor->width / logical_monitor->scale);
      illustration_height = round (first_monitor->height / logical_monitor->scale);
    }
  switch (logical_monitor->rotation)
    {
    case SC_DISPLAY_ROTATION_90:
    case SC_DISPLAY_ROTATION_90_FLIPPED:
    case SC_DISPLAY_ROTATION_270:
    case SC_DISPLAY_ROTATION_270_FLIPPED:
      {
        int height = illustration_height;
        illustration_height = illustration_width;
        illustration_width = height;
        break;
      }
    case SC_DISPLAY_ROTATION_NONE:
    case SC_DISPLAY_ROTATION_180:
    case SC_DISPLAY_ROTATION_FLIPPED:
    case SC_DISPLAY_ROTATION_180_FLIPPED:
      break;
    }
  illustration->rect = GRAPHENE_RECT_INIT (logical_monitor->x,
                                           logical_monitor->y,
                                           illustration_width,
                                           illustration_height);
}

static void
read_current_width_and_height (GVariantIter *modes, Monitor *monitor)
{
  while (TRUE)
    {
      g_autoptr(GVariant) variant = NULL;
      g_autoptr(GVariant) properties_variant = NULL;
      gboolean is_current;

      if (!g_variant_iter_next (modes, "@(siiddada{sv})", &variant))
        break;

      g_variant_get (variant, "(siiddad@a{sv})",
                     NULL, /* mode_id */
                     &monitor->width,
                     &monitor->height,
                     NULL, /* refresh_rate */
                     NULL, /* preferred_scale */
                     NULL, /* supported_scales */
                     &properties_variant);
      if (!g_variant_lookup (properties_variant, "is-current", "b", &is_current))
        is_current = FALSE;

      if (is_current)
        return;
    }
  g_warning ("Monitor '%s' has no configuration which is-current!", monitor->display_name);
}

static void
generate_monitors (DisplayStateTracker *tracker,
                   GVariant *monitors)
{
  g_autoptr(GList) sorted_monitors = NULL;
  GList *sorted_monitors_builtin = NULL;
  GVariantIter monitors_iter;
  GVariant *monitor_variant;
  int monitor_number = 1;
  GList* item;
  Monitor *monitor;

  g_variant_iter_init (&monitors_iter, monitors);
  while ((monitor_variant = g_variant_iter_next_value (&monitors_iter)))
    {
      g_autofree char *vendor = NULL;
      g_autofree char *product = NULL;
      g_autofree char *serial = NULL;
      g_autoptr(GVariantIter) modes = NULL;
      g_autoptr(GVariant) properties = NULL;
      Monitor *m;
      gboolean builtin;

      m = g_new0 (Monitor, 1);
      g_variant_get (monitor_variant, "((ssss)a(siiddada{sv})@a{sv})",
                     &m->connector,
                     &vendor,
                     &product,
                     &serial,
                     &modes,
                     &properties);

      if (!g_variant_lookup (properties, "display-name", "s", &m->display_name))
        m->display_name = g_strdup (m->connector);

      if (!g_variant_lookup (properties, "is-builtin", "b", &builtin))
        builtin = FALSE;

      read_current_width_and_height (modes, m);

      m->match_string = g_strdup_printf ("%s:%s:%s", vendor, product, serial);

      g_hash_table_insert (tracker->monitors, m->connector, m);

      if (builtin)
        sorted_monitors_builtin = g_list_append (sorted_monitors_builtin, m);
      else
        sorted_monitors = g_list_prepend (sorted_monitors, m);

      g_variant_unref (monitor_variant);
    }

    sorted_monitors = g_list_concat (sorted_monitors_builtin, sorted_monitors);
    for (item = sorted_monitors; item != NULL; item = item->next)
      {
        monitor = item->data;
        monitor->number = monitor_number;
        monitor_number += 1;
      }
}

static int
monitor_number_compare (Monitor *a, Monitor *b)
{
  return a->number - b->number;
}

static void
generate_logical_monitors (DisplayStateTracker *tracker,
                           GVariant *logical_monitors)
{
  GVariantIter logical_monitors_iter;
  GVariant *logical_monitor_variant;

  g_variant_iter_init (&logical_monitors_iter, logical_monitors);
  while ((logical_monitor_variant = g_variant_iter_next_value (&logical_monitors_iter)))
    {
      LogicalMonitor *logical_monitor;
      g_autoptr(GVariantIter) monitors_iter = NULL;
      GVariant *monitor_variant;

      logical_monitor = g_new0 (LogicalMonitor, 1);
      g_variant_get (logical_monitor_variant, "(iiduba(ssss)a{sv})",
                     &logical_monitor->x,
                     &logical_monitor->y,
                     &logical_monitor->scale,
                     &logical_monitor->rotation,
                     &logical_monitor->is_primary,
                     &monitors_iter,
                     NULL /* properties */);

      while ((monitor_variant = g_variant_iter_next_value (monitors_iter)))
        {
          g_autofree char *connector = NULL;
          Monitor *monitor;

          g_variant_get (monitor_variant, "(ssss)",
                         &connector,
                         NULL /* vendor */,
                         NULL /* product */,
                         NULL /* serial */);

          monitor = g_hash_table_lookup (tracker->monitors, connector);
          g_assert (monitor);
          logical_monitor->monitors = g_list_insert_sorted (logical_monitor->monitors,
                                                            monitor,
                                                            (GCompareFunc) monitor_number_compare);

          g_variant_unref (monitor_variant);
        }

      logical_monitor->illustration.label = NULL;
      update_monitor_illustration (tracker, logical_monitor);

      tracker->logical_monitors = g_list_append (tracker->logical_monitors, logical_monitor);

      g_variant_unref (logical_monitor_variant);
    }
}

static void
display_state_tracker_clear_logical_monitors (DisplayStateTracker *self)
{
  if (self->logical_monitors != NULL)
    {
      g_list_free_full (self->logical_monitors, (GDestroyNotify) logical_monitor_free);
      self->logical_monitors = NULL;
    }
}

static void
get_current_state_cb (GObject *source_object,
                      GAsyncResult *res,
                      gpointer user_data)
{
  OrgGnomeMutterDisplayConfig *proxy =
    ORG_GNOME_MUTTER_DISPLAY_CONFIG (source_object);
  DisplayStateTracker *tracker = user_data;
  g_autoptr(GVariant) monitors = NULL;
  g_autoptr(GVariant) logical_monitors = NULL;
  g_autoptr(GVariant) properties = NULL;
  g_autoptr(GError) error = NULL;

  if (!org_gnome_mutter_display_config_call_get_current_state_finish (proxy,
                                                                      NULL,
                                                                      &monitors,
                                                                      &logical_monitors,
                                                                      &properties,
                                                                      res,
                                                                      &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to get current display state: %s", error->message);
      return;
    }

  if (!g_variant_lookup (properties, "layout-mode", "u", &tracker->layout_mode))
    tracker->layout_mode = META_LOGICAL_MONITOR_LAYOUT_MODE_LOGICAL;

  display_state_tracker_clear_logical_monitors (tracker);
  g_hash_table_remove_all (tracker->monitors);

  generate_monitors (tracker, monitors);
  generate_logical_monitors (tracker, logical_monitors);

  g_signal_emit (tracker, signals[MONITORS_CHANGED], 0);
}

static void
sync_state (DisplayStateTracker *tracker)
{
  org_gnome_mutter_display_config_call_get_current_state (tracker->proxy,
                                                          tracker->cancellable,
                                                          get_current_state_cb,
                                                          tracker);
}

static void
on_monitors_changed (OrgGnomeMutterDisplayConfig *proxy,
                     DisplayStateTracker *tracker)
{
  sync_state (tracker);
}

static void
on_display_config_proxy_acquired (GObject      *object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  DisplayStateTracker *tracker;
  g_autoptr(OrgGnomeMutterDisplayConfig) proxy = NULL;
  g_autoptr(GError) error = NULL;

  tracker = DISPLAY_STATE_TRACKER (user_data);

  proxy = org_gnome_mutter_display_config_proxy_new_for_bus_finish (result, &error);
  if (!proxy)
    {
      g_warning ("Failed to acquire org.gnome.Mutter.DisplayConfig proxy: %s",
                 error->message);
      return;
    }

  g_clear_object (&tracker->proxy);
  tracker->proxy = g_object_ref (proxy);

  g_signal_connect (proxy, "monitors-changed",
                    G_CALLBACK (on_monitors_changed),
                    tracker);

  sync_state (tracker);
}

static void
display_state_tracker_clear_cancel (DisplayStateTracker *tracker)
{
  if (tracker->cancellable)
    {
      g_cancellable_cancel (tracker->cancellable);
      g_clear_object (&tracker->cancellable);
    }
}

static void
on_display_config_name_appeared (GDBusConnection *connection,
                                 const char *name,
                                 const char *name_owner,
                                 gpointer user_data)
{
  DisplayStateTracker *tracker;

  tracker = DISPLAY_STATE_TRACKER (user_data);
  display_state_tracker_clear_cancel (tracker);
  tracker->cancellable = g_cancellable_new ();

  org_gnome_mutter_display_config_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     "org.gnome.Mutter.DisplayConfig",
                                                     "/org/gnome/Mutter/DisplayConfig",
                                                     tracker->cancellable,
                                                     on_display_config_proxy_acquired,
                                                     tracker);
}

static void
on_display_config_name_vanished (GDBusConnection *connection,
                                 const char *name,
                                 gpointer user_data)
{
  DisplayStateTracker *tracker;

  tracker = DISPLAY_STATE_TRACKER (user_data);
  display_state_tracker_clear_cancel (DISPLAY_STATE_TRACKER (user_data));
  g_clear_object (&tracker->proxy);
}


static void
display_state_tracker_finalize (GObject *object)
{
  DisplayStateTracker *tracker;

  tracker = DISPLAY_STATE_TRACKER (object);
  display_state_tracker_clear_cancel (tracker);
  display_state_tracker_clear_logical_monitors (tracker);
  g_clear_pointer (&tracker->monitors, g_hash_table_destroy);
  g_clear_object (&tracker->proxy);
  if (tracker->display_config_watch_name_id)
  {
    g_bus_unwatch_name (tracker->display_config_watch_name_id);
    tracker->display_config_watch_name_id = 0;
  }

  G_OBJECT_CLASS (display_state_tracker_parent_class)->finalize (object);
}

static void
display_state_tracker_init (DisplayStateTracker *self)
{
  self->monitors = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) monitor_free);
  self->logical_monitors = NULL;
  self->proxy = NULL;
  self->cancellable = NULL;

  self->display_config_watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      "org.gnome.Mutter.DisplayConfig",
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_display_config_name_appeared,
                      on_display_config_name_vanished,
                      self, NULL);
}

static void
display_state_tracker_class_init (DisplayStateTrackerClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = display_state_tracker_finalize;

  signals[MONITORS_CHANGED] = g_signal_new ("monitors-changed",
                                            G_TYPE_FROM_CLASS (klass),
                                            G_SIGNAL_RUN_LAST,
                                            0,
                                            NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);
}

DisplayStateTracker *
display_state_tracker_get (void)
{
  if (tracker_object != NULL)
    {
      g_object_ref (tracker_object);
    }
  else
    {
      tracker_object = g_object_new (DISPLAY_TYPE_STATE_TRACKER, NULL);
      g_object_add_weak_pointer (G_OBJECT (tracker_object), (gpointer *) &tracker_object);
    }

  return DISPLAY_STATE_TRACKER (tracker_object);
}
