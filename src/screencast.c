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

#include <gio/gio.h>
#include <glib-object.h>
#include <stdint.h>
#include <gxdp.h>

#include "xdg-desktop-portal-dbus.h"

#include "screencast.h"
#include "screencastwidget.h"
#include "screencastdialog.h"
#include "gnomescreencast.h"
#include "remotedesktop.h"
#include "displaystatetracker.h"
#include "request.h"
#include "session.h"
#include "shellintrospect.h"
#include "utils.h"

#define RESTORE_FORMAT_VERSION 1
#define RESTORE_VARIANT_TYPE "(xx" SCREEN_CAST_STREAMS_VARIANT_TYPE ")"
#define MONITOR_TYPE "s"
#define WINDOW_TYPE "(ss)"
#define VIRTUAL_TYPE "b"

typedef struct _ScreenCastDialogHandle ScreenCastDialogHandle;

typedef struct _ScreenCastSession
{
  Session parent;

  GnomeScreenCastSession *gnome_screen_cast_session;
  gulong session_ready_handler_id;
  gulong session_closed_handler_id;

  char *parent_window;

  ScreenCastSelection select;

  ScreenCastPersistMode persist_mode;
  GPtrArray *streams_to_restore;
  struct {
    GVariant *data;
    int64_t creation_time;
  } restored;

  GDBusMethodInvocation *start_invocation;
  ScreenCastDialogHandle *dialog_handle;
} ScreenCastSession;

typedef struct _ScreenCastSessionClass
{
  SessionClass parent_class;
} ScreenCastSessionClass;

typedef struct _ScreenCastDialogHandle
{
  Request *request;
  ScreenCastSession *session;

  GtkWindow *dialog;
  GxdpExternalWindow *external_parent;

  int response;
} ScreenCastDialogHandle;

static GDBusConnection *impl_connection;
static GDBusInterfaceSkeleton *impl;

static GnomeScreenCast *gnome_screen_cast;

GType screen_cast_session_get_type (void);
G_DEFINE_TYPE (ScreenCastSession, screen_cast_session, session_get_type ())

static gboolean
is_screen_cast_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, screen_cast_session_get_type ());
}

static void
screen_cast_dialog_handle_free (ScreenCastDialogHandle *dialog_handle)
{
  g_signal_handlers_disconnect_by_data (dialog_handle->request, dialog_handle);
  g_signal_handlers_disconnect_by_data (dialog_handle->dialog, dialog_handle);
  g_clear_pointer (&dialog_handle->dialog, gtk_window_destroy);
  g_clear_object (&dialog_handle->external_parent);
  g_object_unref (dialog_handle->request);

  g_free (dialog_handle);
}

static void
screen_cast_dialog_handle_close (ScreenCastDialogHandle *dialog_handle)
{
  screen_cast_dialog_handle_free (dialog_handle);
}

void
serialize_screen_cast_streams_as_restore_data (GPtrArray       *streams,
                                               GVariantBuilder *impl_builder)
{
  guint i;

  g_variant_builder_open (impl_builder, G_VARIANT_TYPE ("a(uuv)"));

  for (i = 0; streams && i < streams->len; i++)
    {
      ScreenCastStreamInfo *info = g_ptr_array_index (streams, i);
      GVariant *stream_variant;
      Monitor *monitor;
      ShellWindow *window;

      switch (info->type)
        {
        case SCREEN_CAST_SOURCE_TYPE_MONITOR:
          monitor = info->data.monitor;
          stream_variant = g_variant_new (MONITOR_TYPE,
                                          monitor_get_match_string (monitor));
          break;

        case SCREEN_CAST_SOURCE_TYPE_WINDOW:
          window = info->data.window;
          stream_variant = g_variant_new (WINDOW_TYPE,
                                          shell_window_get_app_id (window),
                                          shell_window_get_title (window));
          break;

        case SCREEN_CAST_SOURCE_TYPE_VIRTUAL:
          /*
           * D-Bus doesn't accept maybe types, so just pass bogus boolean. It
           * doesn't really matter since we'll never actually read this value.
           */
          stream_variant = g_variant_new (VIRTUAL_TYPE, TRUE);
          break;
        }

      g_variant_builder_add (impl_builder,
                             "(uuv)",
                             info->id,
                             info->type,
                             stream_variant);
    }
  g_variant_builder_close (impl_builder);
}

static GVariant *
serialize_session_as_restore_data (ScreenCastSession *screen_cast_session,
                                   GPtrArray         *streams)
{
  GVariantBuilder restore_data_builder;
  GVariantBuilder impl_builder;
  int64_t creation_time;
  int64_t last_used_time;

  if (!streams || streams->len == 0)
    return NULL;

  last_used_time = g_get_real_time ();
  if (screen_cast_session->restored.creation_time != -1)
    creation_time = screen_cast_session->restored.creation_time;
  else
    creation_time = g_get_real_time ();

  g_variant_builder_init (&impl_builder,
                          G_VARIANT_TYPE (RESTORE_VARIANT_TYPE));
  g_variant_builder_add (&impl_builder, "x", creation_time);
  g_variant_builder_add (&impl_builder, "x", last_used_time);

  serialize_screen_cast_streams_as_restore_data (streams, &impl_builder);

  g_variant_builder_init (&restore_data_builder, G_VARIANT_TYPE ("(suv)"));
  g_variant_builder_add (&restore_data_builder, "s", "GNOME");
  g_variant_builder_add (&restore_data_builder, "u", RESTORE_FORMAT_VERSION);
  g_variant_builder_add (&restore_data_builder, "v", g_variant_builder_end (&impl_builder));
  return g_variant_builder_end (&restore_data_builder);
}

static void
cancel_start_session (ScreenCastSession *screen_cast_session,
                      int response)
{
  GVariantBuilder results_builder;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_screen_cast_complete_start (XDP_IMPL_SCREEN_CAST (impl),
                                       screen_cast_session->start_invocation,
                                       response,
                                       g_variant_builder_end (&results_builder));
}

static gboolean
on_request_handle_close_cb (XdpImplRequest         *object,
                            GDBusMethodInvocation  *invocation,
                            ScreenCastDialogHandle *dialog_handle)
{
  ScreenCastSession *screen_cast_session = dialog_handle->session;

  cancel_start_session (screen_cast_session, 2);

  g_clear_pointer (&screen_cast_session->dialog_handle,
                   screen_cast_dialog_handle_close);

  return FALSE;
}

static void
on_gnome_screen_cast_session_ready (GnomeScreenCastSession *gnome_screen_cast_session,
                                    ScreenCastSession      *screen_cast_session)
{
  GVariantBuilder streams_builder;
  GVariantBuilder results_builder;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&streams_builder, G_VARIANT_TYPE ("a(ua{sv})"));

  gnome_screen_cast_session = screen_cast_session->gnome_screen_cast_session;
  gnome_screen_cast_session_add_stream_properties (gnome_screen_cast_session,
                                                   &streams_builder);

  g_variant_builder_add (&results_builder, "{sv}",
                         "streams",
                         g_variant_builder_end (&streams_builder));

  if (screen_cast_session->persist_mode != SCREEN_CAST_PERSIST_MODE_NONE)
    {
      g_autoptr(GPtrArray) streams = g_steal_pointer (&screen_cast_session->streams_to_restore);
      GVariant *restore_data;

      restore_data = serialize_session_as_restore_data (screen_cast_session,
                                                        streams);

      if (restore_data)
        {
          g_variant_builder_add (&results_builder, "{sv}", "persist_mode",
                                 g_variant_new_uint32 (screen_cast_session->persist_mode));
          g_variant_builder_add (&results_builder, "{sv}", "restore_data", restore_data);
        }
    }

  xdp_impl_screen_cast_complete_start (XDP_IMPL_SCREEN_CAST (impl),
                                       screen_cast_session->start_invocation, 0,
                                       g_variant_builder_end (&results_builder));
  screen_cast_session->start_invocation = NULL;
}

static void
on_gnome_screen_cast_session_closed (GnomeScreenCastSession *gnome_screen_cast_session,
                                     ScreenCastSession      *screen_cast_session)
{
  session_close ((Session *)screen_cast_session, TRUE);
}

static gboolean
start_session (ScreenCastSession  *screen_cast_session,
               GPtrArray          *streams,
               GError            **error)
{
  GnomeScreenCastSession *gnome_screen_cast_session;

  gnome_screen_cast_session =
    gnome_screen_cast_create_session (gnome_screen_cast, NULL, error);
  if (!gnome_screen_cast_session)
    return FALSE;

  screen_cast_session->gnome_screen_cast_session = gnome_screen_cast_session;

  screen_cast_session->session_ready_handler_id =
    g_signal_connect (gnome_screen_cast_session, "ready",
                      G_CALLBACK (on_gnome_screen_cast_session_ready),
                      screen_cast_session);
  screen_cast_session->session_closed_handler_id =
    g_signal_connect (gnome_screen_cast_session, "closed",
                      G_CALLBACK (on_gnome_screen_cast_session_closed),
                      screen_cast_session);

  if (screen_cast_session->persist_mode != SCREEN_CAST_PERSIST_MODE_NONE)
    screen_cast_session->streams_to_restore = g_ptr_array_ref (streams);

  if (!gnome_screen_cast_session_record_selections (gnome_screen_cast_session,
                                                    streams,
                                                    &screen_cast_session->select,
                                                    error))
    return FALSE;

  if (!gnome_screen_cast_session_start (gnome_screen_cast_session, error))
    return FALSE;

  return TRUE;
}

static void
on_screen_cast_dialog_done_cb (GtkWidget              *widget,
                               int                     dialog_response,
                               ScreenCastPersistMode   persist_mode,
                               GPtrArray              *streams,
                               ScreenCastDialogHandle *dialog_handle)
{
  int response;

  switch (dialog_response)
    {
    default:
      g_warning ("Unexpected response: %d", dialog_response);
      G_GNUC_FALLTHROUGH;

    case GTK_RESPONSE_DELETE_EVENT:
      response = 2;
      break;

    case GTK_RESPONSE_CANCEL:
      response = 1;
      break;

    case GTK_RESPONSE_OK:
      response = 0;
      break;
    }

  if (response == 0)
    {
      ScreenCastSession *screen_cast_session = dialog_handle->session;
      g_autoptr(GError) error = NULL;

      screen_cast_session->persist_mode = MIN (screen_cast_session->persist_mode,
                                               persist_mode);

      if (!start_session (dialog_handle->session, streams, &error))
        {
          g_warning ("Failed to start session: %s", error->message);
          response = 2;
        }
    }

  if (response != 0)
    cancel_start_session (dialog_handle->session, response);

  if (dialog_handle->request->exported)
    request_unexport (dialog_handle->request);
}

static ScreenCastDialogHandle *
create_screen_cast_dialog (ScreenCastSession     *session,
                           GDBusMethodInvocation *invocation,
                           Request               *request,
                           const char            *parent_window)
{
  g_autoptr(GtkWindowGroup) window_group = NULL;
  ScreenCastDialogHandle *dialog_handle;
  GxdpExternalWindow *external_parent;
  GdkSurface *surface;
  GtkWidget *fake_parent;
  GtkWindow *dialog;

  if (parent_window)
    {
      external_parent = gxdp_external_window_new_from_handle (parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   parent_window);
    }
  else
    {
      external_parent = NULL;
    }

  fake_parent = g_object_new (GTK_TYPE_WINDOW, NULL);
  g_object_ref_sink (fake_parent);

  dialog = GTK_WINDOW (screen_cast_dialog_new (request->app_id,
                                               &session->select,
                                               session->persist_mode));
  gtk_window_set_transient_for (dialog, GTK_WINDOW (fake_parent));
  gtk_window_set_modal (dialog, TRUE);

  window_group = gtk_window_group_new ();
  gtk_window_group_add_window (window_group, dialog);

  dialog_handle = g_new0 (ScreenCastDialogHandle, 1);
  dialog_handle->session = session;
  dialog_handle->request = g_object_ref (request);
  dialog_handle->external_parent = external_parent;
  dialog_handle->dialog = dialog;

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (on_request_handle_close_cb), dialog_handle);
  g_signal_connect (dialog, "done",
                    G_CALLBACK (on_screen_cast_dialog_done_cb), dialog_handle);

  gtk_widget_realize (GTK_WIDGET (dialog));

  surface = gtk_native_get_surface (GTK_NATIVE (dialog));
  if (external_parent)
    gxdp_external_window_set_parent_of (external_parent, surface);

  gtk_window_present (dialog);

  return dialog_handle;
}

static Monitor *
find_monitor_by_string (const char *monitor_string)
{
  DisplayStateTracker *display_state_tracker = display_state_tracker_get ();
  GList *l;

  for (l = display_state_tracker_get_logical_monitors (display_state_tracker);
       l;
       l = l->next)
    {
      LogicalMonitor *logical_monitor = l->data;
      GList *monitors;

      for (monitors = logical_monitor_get_monitors (logical_monitor);
           monitors;
           monitors = monitors->next)
        {
          Monitor *monitor = monitors->data;

          if (g_strcmp0 (monitor_get_match_string (monitor), monitor_string) == 0)
            return monitor;
        }
    }

  return NULL;
}

static ShellWindow *
find_best_window_by_app_id_and_title (const char *app_id,
                                      const char *title)
{
  ShellIntrospect *shell_introspect = shell_introspect_get ();
  GListModel *windows;
  ShellWindow *best_match;
  glong best_match_distance;

  best_match = NULL;
  best_match_distance = G_MAXLONG;

  shell_introspect_ref_listeners (shell_introspect);
  shell_introspect_wait_for_windows (shell_introspect);

  windows = shell_introspect_get_windows (shell_introspect);
  for (guint i = 0; windows && i < g_list_model_get_n_items (windows); i++)
    {
      g_autoptr(ShellWindow) window = g_list_model_get_item (windows, i);
      glong distance;

      if (g_strcmp0 (shell_window_get_app_id (window), app_id) != 0)
        continue;

      distance = str_distance (shell_window_get_title (window), title);

      if (distance == 0)
        return window;

      if (distance < best_match_distance)
        {
          best_match = window;
          best_match_distance = distance;
        }
    }

  shell_introspect_unref_listeners (shell_introspect);

  /* If even the best match's window title is too different, don't
   * restore it.
   */
  if (best_match_distance > strlen (title) / 2)
    return NULL;

  return best_match;
}

void
screen_cast_stream_info_free (ScreenCastStreamInfo *info)
{
  switch (info->type)
    {
    case SCREEN_CAST_SOURCE_TYPE_MONITOR:
      g_clear_pointer (&info->data.monitor, monitor_free);
      break;
    case SCREEN_CAST_SOURCE_TYPE_WINDOW:
      g_clear_pointer (&info->data.window, g_object_unref);
      break;
    case SCREEN_CAST_SOURCE_TYPE_VIRTUAL:
      break;
    }

  g_free (info);
}

GPtrArray *
restore_screen_cast_streams (GVariantIter        *streams_iter,
                             ScreenCastSelection *screen_cast_selection)
{
  g_autoptr(GPtrArray) streams = NULL;
  g_autoptr(GVariant) data = NULL;
  ScreenCastSourceType source_type;
  uint32_t id;

  streams =
    g_ptr_array_new_with_free_func ((GDestroyNotify) screen_cast_stream_info_free);

  while (g_variant_iter_next (streams_iter, "(uuv)", &id, &source_type, &data))
    {
      switch (source_type)
        {
        case SCREEN_CAST_SOURCE_TYPE_MONITOR:
          {
            ScreenCastStreamInfo *info;
            const char *match_string = g_variant_get_string (data, NULL);
            Monitor *monitor = find_monitor_by_string (match_string);

            if (!(screen_cast_selection->source_types.monitor) ||
                !g_variant_check_format_string (data, MONITOR_TYPE, FALSE))
              return NULL;

            if (!monitor)
              return NULL;

            info = g_new0 (ScreenCastStreamInfo, 1);
            info->type = SCREEN_CAST_SOURCE_TYPE_MONITOR;
            info->data.monitor = monitor_dup (monitor);
            info->id = id;
            g_ptr_array_add (streams, info);
          }
          break;

        case SCREEN_CAST_SOURCE_TYPE_WINDOW:
          {
            ScreenCastStreamInfo *info;
            const char *app_id = NULL;
            const char *title = NULL;
            ShellWindow *window;

            if (!(screen_cast_selection->source_types.window) ||
                !g_variant_check_format_string (data, WINDOW_TYPE, FALSE))
              return NULL;

            g_variant_get (data, "(&s&s)", &app_id, &title);

            window = find_best_window_by_app_id_and_title (app_id, title);

            if (!window)
              return NULL;

            info = g_new0 (ScreenCastStreamInfo, 1);
            info->type = SCREEN_CAST_SOURCE_TYPE_WINDOW;
            info->data.window = shell_window_dup (window);
            info->id = id;
            g_ptr_array_add (streams, info);
          }
          break;

        case SCREEN_CAST_SOURCE_TYPE_VIRTUAL:
          {
            ScreenCastStreamInfo *info;

            if (!(screen_cast_selection->source_types.virtual_monitor) ||
                !g_variant_check_format_string (data, VIRTUAL_TYPE, FALSE))
              return NULL;

            info = g_new0 (ScreenCastStreamInfo, 1);
            info->type = SCREEN_CAST_SOURCE_TYPE_VIRTUAL;
            info->id = id;
            g_ptr_array_add (streams, info);
          }
          break;

        default:
          return NULL;
        }
    }

  return streams->len > 0 ? g_steal_pointer (&streams) : NULL;
}

static gboolean
restore_stream_from_data (ScreenCastSession *screen_cast_session)
{
  g_autoptr(GVariantIter) array_iter = NULL;
  g_autoptr(GPtrArray) streams = NULL;
  int64_t creation_time;
  int64_t last_used_time;
  g_autoptr(GError) error = NULL;

  if (!screen_cast_session->restored.data)
    return FALSE;

  g_variant_get (screen_cast_session->restored.data,
                 RESTORE_VARIANT_TYPE,
                 &creation_time,
                 &last_used_time,
                 &array_iter);

  streams = restore_screen_cast_streams (array_iter,
                                         &screen_cast_session->select);
  if (!streams)
    return FALSE;

  screen_cast_session->restored.creation_time = creation_time;

  start_session (screen_cast_session, streams, &error);

  if (error)
    {
      g_warning ("Error restoring stream from session: %s", error->message);
      return FALSE;
    }

  return TRUE;
}

static gboolean
handle_start (XdpImplScreenCast     *object,
              GDBusMethodInvocation *invocation,
              const char            *arg_handle,
              const char            *arg_session_handle,
              const char            *arg_app_id,
              const char            *arg_parent_window,
              GVariant              *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  ScreenCastSession *screen_cast_session;
  GVariantBuilder results_builder;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);
  request_export (request,
                  g_dbus_method_invocation_get_connection (invocation));

  screen_cast_session =
    (ScreenCastSession *)lookup_session (arg_session_handle);
  if (!screen_cast_session)
    {
      g_warning ("Attempted to start non existing screen cast session");
      goto err;
    }

  if (screen_cast_session->dialog_handle)
    {
      g_warning ("Screen cast dialog already open");
      goto err;
    }

  screen_cast_session->start_invocation = invocation;

  if (!restore_stream_from_data (screen_cast_session))
    {
      ScreenCastDialogHandle *dialog_handle;

      dialog_handle = create_screen_cast_dialog (screen_cast_session,
                                                 invocation,
                                                 request,
                                                 arg_parent_window);

      screen_cast_session->dialog_handle = dialog_handle;
    }

  return TRUE;

err:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE ("a(ua{sv}"));
  xdp_impl_screen_cast_complete_start (object, invocation, 2,
                                       g_variant_builder_end (&results_builder));

  return TRUE;
}

ScreenCastSourceTypes
screen_cast_source_types_from_flags (uint32_t flags) {
  return (ScreenCastSourceTypes) {
    .monitor = (flags & SCREEN_CAST_SOURCE_TYPE_MONITOR) > 0,
    .window = (flags & SCREEN_CAST_SOURCE_TYPE_WINDOW) > 0,
    .virtual_monitor = (flags & SCREEN_CAST_SOURCE_TYPE_VIRTUAL) > 0,
  };
}

static gboolean
handle_select_sources (XdpImplScreenCast     *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_handle,
                       const char            *arg_session_handle,
                       const char            *arg_app_id,
                       GVariant              *arg_options)
{
  g_autofree gchar *provider = NULL;
  g_autoptr(GVariant) restore_data = NULL;
  ScreenCastSession *screen_cast_session;
  Session *session;
  int response;
  uint32_t types;
  gboolean multiple;
  ScreenCastCursorMode cursor_mode;
  ScreenCastSelection select;
  GVariantBuilder results_builder;
  GVariant *results;
  uint32_t version;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to select sources on non-existing %s", arg_session_handle);
      response = 2;
      goto out;
    }

  if (!g_variant_lookup (arg_options, "multiple", "b", &multiple))
    multiple = FALSE;

  if (!g_variant_lookup (arg_options, "types", "u", &types))
    types = SCREEN_CAST_SOURCE_TYPE_MONITOR;

  if (!(types & (SCREEN_CAST_SOURCE_TYPE_MONITOR |
                 SCREEN_CAST_SOURCE_TYPE_WINDOW |
                 SCREEN_CAST_SOURCE_TYPE_VIRTUAL)))
    {
      g_warning ("Unknown screen cast source type");
      response = 2;
      goto out;
    }

  if (!g_variant_lookup (arg_options, "cursor_mode", "u", &cursor_mode))
    cursor_mode = SCREEN_CAST_CURSOR_MODE_HIDDEN;

  switch (cursor_mode)
    {
    case SCREEN_CAST_CURSOR_MODE_HIDDEN:
    case SCREEN_CAST_CURSOR_MODE_EMBEDDED:
    case SCREEN_CAST_CURSOR_MODE_METADATA:
      break;
    default:
      g_warning ("Unknown screen cast cursor mode");
      response = 2;
      goto out;
    }

  select.multiple = multiple;
  select.source_types = screen_cast_source_types_from_flags (types);
  select.cursor_mode = cursor_mode;

  if (is_screen_cast_session (session))
    {
      ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;

      screen_cast_session->select = select;
      response = 0;
    }
  else if (is_remote_desktop_session (session))
    {
      RemoteDesktopSession *remote_desktop_session =
        (RemoteDesktopSession *)session;

      remote_desktop_session_sources_selected (remote_desktop_session, &select);
      response = 0;
    }
  else
    {
      g_warning ("Tried to select sources on invalid session type");
      response = 2;
    }

  screen_cast_session = (ScreenCastSession *)session;
  g_variant_lookup (arg_options, "persist_mode", "u", &screen_cast_session->persist_mode);

  if (g_variant_lookup (arg_options, "restore_data", "(suv)", &provider, &version, &restore_data))
    {
      if (!g_variant_check_format_string (restore_data, RESTORE_VARIANT_TYPE, FALSE))
        {
          g_warning ("Cannot parse restore data, ignoring");
          goto out;
        }

      if (g_strcmp0 (provider, "GNOME") == 0 && version == RESTORE_FORMAT_VERSION)
        screen_cast_session->restored.data = g_variant_ref (restore_data);
    }

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  results = g_variant_builder_end (&results_builder);
  xdp_impl_screen_cast_complete_select_sources (object, invocation,
                                                response, results);

  return TRUE;
}

static gboolean
handle_create_session (XdpImplScreenCast     *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_handle,
                       const char            *arg_session_handle,
                       const char            *arg_app_id,
                       GVariant              *arg_options)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  int response;
  Session *session;
  GVariantBuilder results_builder;

  session = g_object_new (screen_cast_session_get_type (),
                          "id", arg_session_handle,
                          "peer-name", sender,
                          NULL);

  if (!session_export (session,
                       g_dbus_method_invocation_get_connection (invocation),
                       &error))
    {
      g_clear_object (&session);
      g_warning ("Failed to create screen cast session: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_screen_cast_complete_create_session (object,
                                                invocation,
                                                response,
                                                g_variant_builder_end (&results_builder));

  return TRUE;
}

static void
on_gnome_screen_cast_enabled (GnomeScreenCast *gnome_screen_cast)
{
  g_autoptr(GError) error = NULL;

  impl = G_DBUS_INTERFACE_SKELETON (xdp_impl_screen_cast_skeleton_new ());
  xdp_impl_screen_cast_set_version (XDP_IMPL_SCREEN_CAST (impl), 5);

  g_signal_connect (impl, "handle-create-session",
                    G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (impl, "handle-select-sources",
                    G_CALLBACK (handle_select_sources), NULL);
  g_signal_connect (impl, "handle-start",
                    G_CALLBACK (handle_start), NULL);

  g_object_set (G_OBJECT (impl),
                "available-source-types", SCREEN_CAST_SOURCE_TYPE_MONITOR |
                                          SCREEN_CAST_SOURCE_TYPE_WINDOW |
                                          SCREEN_CAST_SOURCE_TYPE_VIRTUAL,
                "available-cursor-modes", SCREEN_CAST_CURSOR_MODE_HIDDEN |
                                          SCREEN_CAST_CURSOR_MODE_EMBEDDED |
                                          SCREEN_CAST_CURSOR_MODE_METADATA,
                NULL);

  if (!g_dbus_interface_skeleton_export (impl,
                                         impl_connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Failed to export screen cast portal implementation object: %s",
                 error->message);
      return;
    }

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (impl)->name);
}

static void
on_gnome_screen_cast_disabled (GDBusConnection *connection,
                               const char *name,
                               gpointer user_data)
{
  if (impl)
    {
      g_debug ("unproviding %s", g_dbus_interface_skeleton_get_info (impl)->name);

      g_dbus_interface_skeleton_unexport (impl);
      g_clear_object (&impl);
    }
}

static void
screen_cast_session_close (Session *session)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)session;
  GnomeScreenCastSession *gnome_screen_cast_session;
  g_autoptr(GError) error = NULL;

  gnome_screen_cast_session = screen_cast_session->gnome_screen_cast_session;
  if (gnome_screen_cast_session)
    {
      g_signal_handler_disconnect (gnome_screen_cast_session,
                                   screen_cast_session->session_ready_handler_id);
      g_signal_handler_disconnect (gnome_screen_cast_session,
                                   screen_cast_session->session_closed_handler_id);
      if (!gnome_screen_cast_session_stop (gnome_screen_cast_session,
                                           &error))
        g_warning ("Failed to close GNOME screen cast session: %s",
                   error->message);
      g_clear_object (&screen_cast_session->gnome_screen_cast_session);
    }

  g_clear_pointer (&screen_cast_session->dialog_handle,
                   screen_cast_dialog_handle_close);
}

static void
screen_cast_session_finalize (GObject *object)
{
  ScreenCastSession *screen_cast_session = (ScreenCastSession *)object;

  g_clear_pointer (&screen_cast_session->streams_to_restore, g_ptr_array_unref);
  g_clear_pointer (&screen_cast_session->restored.data, g_variant_unref);
  g_clear_object (&screen_cast_session->gnome_screen_cast_session);

  G_OBJECT_CLASS (screen_cast_session_parent_class)->finalize (object);
}

static void
screen_cast_session_init (ScreenCastSession *screen_cast_session)
{
  screen_cast_session->persist_mode = SCREEN_CAST_PERSIST_MODE_NONE;
  screen_cast_session->restored.creation_time = -1;
}

static void
screen_cast_session_class_init (ScreenCastSessionClass *klass)
{
  GObjectClass *gobject_class;
  SessionClass *session_class;

  gobject_class = (GObjectClass *)klass;
  gobject_class->finalize = screen_cast_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = screen_cast_session_close;
}

gboolean
screen_cast_init (GDBusConnection  *connection,
                  GError          **error)
{
  /*
   * Ensure ShellIntrospect and DisplayStateTracker are initialized before
   * any screencast session is created to avoid race conditions when restoring
   * previous streams.
   */
  display_state_tracker_get ();
  shell_introspect_get ();

  impl_connection = connection;
  gnome_screen_cast = gnome_screen_cast_new (connection);

  g_signal_connect (gnome_screen_cast, "enabled",
                    G_CALLBACK (on_gnome_screen_cast_enabled), NULL);
  g_signal_connect (gnome_screen_cast, "disabled",
                    G_CALLBACK (on_gnome_screen_cast_disabled), NULL);

  return TRUE;
}
