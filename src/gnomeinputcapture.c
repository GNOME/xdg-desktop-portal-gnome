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

#include "gnomeinputcapture.h"
#include "inputcapture.h"
#include "shell-dbus.h"

#include <stdint.h>
#include <gdk/gdk.h>

enum
{
  SESSION_SIGNAL_ZONES_CHANGED,
  SESSION_SIGNAL_ACTIVATED,
  SESSION_SIGNAL_DEACTIVATED,
  SESSION_SIGNAL_DISABLED,
  SESSION_SIGNAL_CLOSED,

  N_SESSION_SIGNALS
};

static guint session_signals[N_SESSION_SIGNALS];

enum
{
  ENABLED,
  DISABLED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

typedef struct _GnomeInputCaptureSession
{
  GObject parent;

  char *path;
  OrgGnomeMutterInputCaptureSession *proxy;

  uint32_t serial;
} GnomeInputCaptureSession;

typedef struct _GnomeInputCaptureSessionClass
{
  GObjectClass parent_class;
} GnomeInputCaptureSessionClass;

typedef struct _GnomeInputCapture
{
  GObject parent;

  guint input_capture_name_watch;
  OrgGnomeMutterInputCapture *proxy;

  uint32_t supported_capabilities;
} GnomeInputCapture;

typedef struct _GnomeInputCaptureClass
{
  GObjectClass parent_class;
} GnomeInputCaptureClass;

static GType gnome_input_capture_session_get_type (void);
G_DEFINE_TYPE (GnomeInputCaptureSession, gnome_input_capture_session,
               G_TYPE_OBJECT)

static GType gnome_input_capture_get_type (void);
G_DEFINE_TYPE (GnomeInputCapture, gnome_input_capture, G_TYPE_OBJECT)

static void
gnome_input_capture_session_finalize (GObject *object)
{
  GnomeInputCaptureSession *session = (GnomeInputCaptureSession *)object;

  g_clear_object (&session->proxy);
  g_free (session->path);

  G_OBJECT_CLASS (gnome_input_capture_session_parent_class)->finalize (object);
}

static void
gnome_input_capture_session_init (GnomeInputCaptureSession *gnome_input_capture_session)
{
}

static void
gnome_input_capture_session_class_init (GnomeInputCaptureSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gnome_input_capture_session_finalize;

  session_signals[SESSION_SIGNAL_ZONES_CHANGED] =
    g_signal_new ("zones-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
  session_signals[SESSION_SIGNAL_ACTIVATED] =
    g_signal_new ("activated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 4,
                  G_TYPE_UINT,
                  G_TYPE_DOUBLE,
                  G_TYPE_DOUBLE,
                  G_TYPE_UINT);
  session_signals[SESSION_SIGNAL_DEACTIVATED] =
    g_signal_new ("deactivated",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_UINT);
  session_signals[SESSION_SIGNAL_DISABLED] =
    g_signal_new ("disabled",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
  session_signals[SESSION_SIGNAL_CLOSED] =
    g_signal_new ("closed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL,
                  NULL,
                  G_TYPE_NONE, 0);
}

static void
gnome_input_capture_name_appeared (GDBusConnection *connection,
                                   const char      *name,
                                   const char      *name_owner,
                                   gpointer         user_data)
{
  GnomeInputCapture *gnome_input_capture = user_data;
  g_autoptr(GError) error = NULL;

  gnome_input_capture->proxy =
    org_gnome_mutter_input_capture_proxy_new_sync (connection,
                                                   G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                   "org.gnome.Mutter.InputCapture",
                                                   "/org/gnome/Mutter/InputCapture",
                                                   NULL,
                                                   &error);
  if (!gnome_input_capture->proxy)
    {
      g_warning ("Failed to acquire org.gnome.Mutter.InputCapture proxy: %s",
                 error->message);
      return;
    }

  gnome_input_capture->supported_capabilities =
    org_gnome_mutter_input_capture_get_supported_capabilities (
      gnome_input_capture->proxy);

  g_signal_emit (gnome_input_capture, signals[ENABLED], 0);
}

static void
gnome_input_capture_name_vanished (GDBusConnection *connection,
                                 const char *name,
                                 gpointer user_data)
{
  GnomeInputCapture *gnome_input_capture = user_data;

  g_clear_object (&gnome_input_capture->proxy);

  g_signal_emit (gnome_input_capture, signals[DISABLED], 0);
}

static void
gnome_input_capture_class_init (GnomeInputCaptureClass *klass)
{
  signals[ENABLED] = g_signal_new ("enabled",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0,
                                   NULL, NULL,
                                   NULL,
                                   G_TYPE_NONE, 0);
  signals[DISABLED] = g_signal_new ("disabled",
                                    G_TYPE_FROM_CLASS (klass),
                                    G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL, NULL,
                                    NULL,
                                    G_TYPE_NONE, 0);
}

static void
gnome_input_capture_init (GnomeInputCapture *gnome_input_capture)
{
}

GnomeInputCapture *
gnome_input_capture_new (GDBusConnection *connection)
{
  GnomeInputCapture *gnome_input_capture;

  gnome_input_capture = g_object_new (gnome_input_capture_get_type (), NULL);
  gnome_input_capture->input_capture_name_watch =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      "org.gnome.Mutter.InputCapture",
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      gnome_input_capture_name_appeared,
                      gnome_input_capture_name_vanished,
                      gnome_input_capture,
                      NULL);

  return gnome_input_capture;
}

uint32_t
gnome_input_capture_get_supported_capabilities (GnomeInputCapture *gnome_input_capture)
{
  return gnome_input_capture->supported_capabilities;
}

static void
on_input_capture_session_zones_changed (OrgGnomeMutterInputCaptureSession *session_proxy,
                                        GnomeInputCaptureSession          *gnome_input_capture_session)
{
  g_signal_emit (gnome_input_capture_session,
                 session_signals[SESSION_SIGNAL_ZONES_CHANGED], 0);
}

static void
on_input_capture_session_activated (OrgGnomeMutterInputCaptureSession *session_proxy,
                                    unsigned int                       barrier_id,
                                    unsigned int                       activation_id,
                                    GVariant                          *cursor_position,
                                    GnomeInputCaptureSession          *gnome_input_capture_session)
{
  double cursor_x, cursor_y;

  g_variant_get (cursor_position, "(dd)", &cursor_x, &cursor_y);
  g_signal_emit (gnome_input_capture_session,
                 session_signals[SESSION_SIGNAL_ACTIVATED], 0,
                 activation_id,
                 cursor_x,
                 cursor_y,
                 barrier_id);
}

static void
on_input_capture_session_deactivated (OrgGnomeMutterInputCaptureSession *session_proxy,
                                      unsigned int                       activation_id,
                                      GnomeInputCaptureSession          *gnome_input_capture_session)
{
  g_signal_emit (gnome_input_capture_session,
                 session_signals[SESSION_SIGNAL_DEACTIVATED], 0, activation_id);
}

static void
on_input_capture_session_disabled (OrgGnomeMutterInputCaptureSession *session_proxy,
                                   GnomeInputCaptureSession          *gnome_input_capture_session)
{
  g_signal_emit (gnome_input_capture_session,
                 session_signals[SESSION_SIGNAL_DISABLED], 0);
}

static void
on_input_capture_session_closed (OrgGnomeMutterInputCaptureSession *session_proxy,
                                 GnomeInputCaptureSession          *gnome_input_capture_session)
{
  g_signal_emit (gnome_input_capture_session,
                 session_signals[SESSION_SIGNAL_CLOSED], 0);
}

GnomeInputCaptureSession *
gnome_input_capture_create_session (GnomeInputCapture  *gnome_input_capture,
                                    uint32_t            capabilities,
                                    GError            **error)
{
  g_autofree char *session_path = NULL;
  GDBusConnection *connection;
  OrgGnomeMutterInputCaptureSession *session_proxy;
  GnomeInputCaptureSession *gnome_input_capture_session;

  if (!org_gnome_mutter_input_capture_call_create_session_sync (gnome_input_capture->proxy,
                                                                capabilities,
                                                                &session_path,
                                                                NULL,
                                                                error))
    return NULL;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (gnome_input_capture->proxy));
  session_proxy =
    org_gnome_mutter_input_capture_session_proxy_new_sync (connection,
                                                           G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                           "org.gnome.Mutter.InputCapture",
                                                           session_path,
                                                           NULL,
                                                           error);
  if (!session_proxy)
    return NULL;

  gnome_input_capture_session =
    g_object_new (gnome_input_capture_session_get_type (), NULL);
  gnome_input_capture_session->path = g_steal_pointer (&session_path);
  gnome_input_capture_session->proxy = g_steal_pointer (&session_proxy);

  g_signal_connect (gnome_input_capture_session->proxy,
                    "zones-changed",
                    G_CALLBACK (on_input_capture_session_zones_changed),
                    gnome_input_capture_session);
  g_signal_connect (gnome_input_capture_session->proxy,
                    "activated",
                    G_CALLBACK (on_input_capture_session_activated),
                    gnome_input_capture_session);
  g_signal_connect (gnome_input_capture_session->proxy,
                    "deactivated",
                    G_CALLBACK (on_input_capture_session_deactivated),
                    gnome_input_capture_session);
  g_signal_connect (gnome_input_capture_session->proxy,
                    "disabled",
                    G_CALLBACK (on_input_capture_session_disabled),
                    gnome_input_capture_session);
  g_signal_connect (gnome_input_capture_session->proxy,
                    "closed",
                    G_CALLBACK (on_input_capture_session_closed),
                    gnome_input_capture_session);

  return gnome_input_capture_session;
}

gboolean
gnome_input_capture_session_enable (GnomeInputCaptureSession  *gnome_input_capture_session,
                                    GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;

  if (!org_gnome_mutter_input_capture_session_call_enable_sync (session_proxy,
                                                                NULL,
                                                                error))
    return FALSE;

  return TRUE;
}

gboolean
gnome_input_capture_session_disable (GnomeInputCaptureSession  *gnome_input_capture_session,
                                     GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;

  if (!org_gnome_mutter_input_capture_session_call_disable_sync (session_proxy,
                                                                 NULL,
                                                                 error))
    return FALSE;

  return TRUE;
}

GList *
gnome_input_capture_session_get_zones (GnomeInputCaptureSession  *gnome_input_capture_session,
                                       GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;
  g_autoptr(GVariant) zones_variant = NULL;
  GVariantIter iter;
  uint32_t width, height;
  int32_t x, y;
  GList *zones = NULL;

  if (!org_gnome_mutter_input_capture_session_call_get_zones_sync (
        session_proxy,
        &gnome_input_capture_session->serial,
        &zones_variant,
        NULL, error))
    {
      g_prefix_error (error, "Failed to get zones: ");
      return NULL;
    }

  g_variant_iter_init (&iter, zones_variant);
  while (g_variant_iter_next (&iter, "(uuii)", &width, &height, &x, &y))
    {
      InputCaptureZone *rect;

      rect = g_new (InputCaptureZone, 1);
      *rect = (InputCaptureZone)  {
        .width = width,
        .height = height,
        .x = x,
        .y = y,
      };
      zones = g_list_append (zones, rect);
    }

  return zones;
}

unsigned int
gnome_input_capture_session_add_barrier (GnomeInputCaptureSession  *gnome_input_capture_session,
                                         int32_t                    x1,
                                         int32_t                    y1,
                                         int32_t                    x2,
                                         int32_t                    y2,
                                         GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;
  uint32_t serial;
  uint32_t barrier_id;

  serial = gnome_input_capture_session->serial;
  if (!org_gnome_mutter_input_capture_session_call_add_barrier_sync (
        session_proxy,
        serial,
        g_variant_new ("(iiii)", x1, y1, x2, y2),
        &barrier_id,
        NULL, error))
    {
      g_prefix_error (error, "Failed to get zones: ");
      return 0;
    }

  return barrier_id;
}

gboolean
gnome_input_capture_session_clear_barriers (GnomeInputCaptureSession  *gnome_input_capture_session,
                                            GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;

  if (!org_gnome_mutter_input_capture_session_call_clear_barriers_sync (
        session_proxy, NULL, error))
    {
      g_prefix_error (error, "Failed to clear barriers: ");
      return FALSE;
    }

  return TRUE;
}

gboolean
gnome_input_capture_session_release (GnomeInputCaptureSession  *gnome_input_capture_session,
                                     gboolean                   has_cursor_position,
                                     double                     cursor_position_x,
                                     double                     cursor_position_y,
                                     GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;
  GVariantBuilder options_builder;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);

  if (has_cursor_position)
    {
      g_variant_builder_add (&options_builder, "{sv}",
                             "cursor_position",
                             g_variant_new ("(dd)",
                                            cursor_position_x,
                                            cursor_position_y));
    }

  if (!org_gnome_mutter_input_capture_session_call_release_sync (
        session_proxy,
        g_variant_builder_end (&options_builder),
        NULL, error))
    {
      g_prefix_error (error, "Failed to release: ");
      return FALSE;
    }

  return TRUE;
}

gboolean
gnome_input_capture_session_close (GnomeInputCaptureSession  *gnome_input_capture_session,
                                   GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;

  return org_gnome_mutter_input_capture_session_call_close_sync (session_proxy,
                                                                 NULL,
                                                                 error);
}

uint32_t
gnome_input_capture_session_get_serial (GnomeInputCaptureSession *gnome_input_capture_session)
{
  return gnome_input_capture_session->serial;
}

gboolean
gnome_input_capture_connect_to_eis (GnomeInputCaptureSession  *gnome_input_capture_session,
                                    GUnixFDList              **fd_list,
                                    GVariant                 **fd_variant,
                                    GError                   **error)
{
  OrgGnomeMutterInputCaptureSession *session_proxy =
    gnome_input_capture_session->proxy;
  g_autoptr(GUnixFDList) in_fd_list = NULL;

  if (!org_gnome_mutter_input_capture_session_call_connect_to_eis_sync (session_proxy,
                                                                        in_fd_list,
                                                                        fd_variant,
                                                                        fd_list,
                                                                        NULL,
                                                                        error))
    {
      g_prefix_error (error, "Failed to connect to EIS: ");
      return FALSE;
    }

  return TRUE;
}
