/*
 * Copyright © 2018 Red Hat, Inc
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
#include "shell-dbus.h"

#include "clipboard.h"
#include "remotedesktop.h"
#include "remotedesktopdialog.h"
#include "screencastdialog.h"
#include "gnomescreencast.h"
#include "request.h"
#include "session.h"
#include "utils.h"

#define RESTORE_FORMAT_VERSION 1
#define REMOTE_DESKTOP_RESTORE_VARIANT_TYPE "(xxub" SCREEN_CAST_STREAMS_VARIANT_TYPE ")"

typedef enum _GnomeRemoteDesktopDeviceType
{
  GNOME_REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD = 1 << 0,
  GNOME_REMOTE_DESKTOP_DEVICE_TYPE_POINTER = 1 << 1,
  GNOME_REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN = 1 << 2,
} GnomeRemoteDesktopDeviceType;

enum _GnomeRemoteDesktopNotifyAxisFlags
{
  GNOME_REMOTE_DESKTOP_NOTIFY_AXIS_FLAGS_FINISH = 1 << 0,
} GnomeRemoteDesktopNotifyAxisFlags;

typedef struct _RemoteDesktopDialogHandle RemoteDesktopDialogHandle;

typedef struct _RemoteDesktopSession
{
  Session parent;

  char *parent_window;
  char *mutter_session_path;
  OrgGnomeMutterRemoteDesktopSession *mutter_session_proxy;
  gulong closed_handler_id;

  GnomeScreenCastSession *gnome_screen_cast_session;
  gulong session_ready_handler_id;

  struct {
    RemoteDesktopDeviceType device_types;

    gboolean screen_cast_enable;
    ScreenCastSelection screen_cast;
  } select;

  struct {
    gboolean persist_allowed;
    RemoteDesktopDeviceType device_types;
  } shared;

  struct
  {
    gboolean clipboard_enabled;
    gboolean clipboard_requested;
    gulong selection_owner_changed_handler_id;
    gulong selection_transfer_handler_id;
  } clipboard;

  RemoteDesktopPersistMode persist_mode;
  GPtrArray *streams_to_restore;
  struct {
    GVariant *data;
    int64_t creation_time;
  } restored;

  GDBusMethodInvocation *start_invocation;
  RemoteDesktopDialogHandle *dialog_handle;
} RemoteDesktopSession;

typedef struct _RemoteDesktopSessionClass
{
  SessionClass parent_class;
} RemoteDesktopSessionClass;

enum
{
  CLIPBOARD_SELECTION_OWNER_CHANGED,
  CLIPBOARD_SELECTION_TRANSFER,

  N_SIGNAL
};

static guint signals[N_SIGNAL];

typedef struct _RemoteDesktopDialogHandle
{
  Request *request;
  RemoteDesktopSession *session;

  GtkWindow *dialog;
  GxdpExternalWindow *external_parent;

  int response;
} RemoteDesktopDialogHandle;

static GDBusConnection *impl_connection;
static guint remote_desktop_name_watch;
static GDBusInterfaceSkeleton *impl;
static OrgGnomeMutterRemoteDesktop *remote_desktop;
static GnomeScreenCast *gnome_screen_cast;

GType remote_desktop_session_get_type (void);
G_DEFINE_TYPE (RemoteDesktopSession, remote_desktop_session, session_get_type ())

static void
start_done (RemoteDesktopSession *session);

static gboolean
start_session (RemoteDesktopSession     *session,
               RemoteDesktopDeviceType   device_types,
               GPtrArray                *streams,
               gboolean                  clipboard_enabled,
               gboolean                  persist_allowed,
               GError                  **error);

static void
cancel_start_session (RemoteDesktopSession *session,
                      int response);

gboolean
is_remote_desktop_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, remote_desktop_session_get_type ());
}

void
remote_desktop_session_sources_selected (RemoteDesktopSession *session,
                                         ScreenCastSelection *selection)
{
  session->select.screen_cast_enable = TRUE;
  session->select.screen_cast = *selection;
}

static void
on_selection_owner_changed (OrgGnomeMutterRemoteDesktopSession *object,
                            GVariant *arg_options,
                            RemoteDesktopSession *session)
{
  g_signal_emit (session, signals[CLIPBOARD_SELECTION_OWNER_CHANGED], 0,
                 arg_options);
}

static void
on_selection_transfer (OrgGnomeMutterRemoteDesktopSession *object,
                       const gchar *arg_mime_type,
                       guint arg_serial,
                       RemoteDesktopSession *session)
{
  g_signal_emit (session, signals[CLIPBOARD_SELECTION_TRANSFER], 0,
                 arg_mime_type, arg_serial);
}

void
remote_desktop_session_request_clipboard (RemoteDesktopSession *session)
{
  session->clipboard.clipboard_requested = TRUE;
}

gboolean
remote_desktop_session_is_clipboard_enabled (RemoteDesktopSession *session)
{
  return session->clipboard.clipboard_enabled;
}

OrgGnomeMutterRemoteDesktopSession *
remote_desktop_session_mutter_session_proxy (RemoteDesktopSession *session)
{
  return session->mutter_session_proxy;
}

static void
remote_desktop_dialog_handle_free (RemoteDesktopDialogHandle *dialog_handle)
{
  g_clear_pointer (&dialog_handle->dialog, gtk_window_destroy);
  g_clear_object (&dialog_handle->external_parent);
  g_object_unref (dialog_handle->request);

  g_free (dialog_handle);
}

static void
remote_desktop_dialog_handle_close (RemoteDesktopDialogHandle *dialog_handle)
{
  remote_desktop_dialog_handle_free (dialog_handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              RemoteDesktopDialogHandle *dialog_handle)
{
  cancel_start_session (dialog_handle->session, 2);

  remote_desktop_dialog_handle_close (dialog_handle);

  return FALSE;
}

static void
remote_desktop_dialog_done (GtkWidget                 *widget,
                            int                        dialog_response,
                            RemoteDesktopDeviceType    device_types,
                            GPtrArray                 *streams,
                            gboolean                   clipboard_enabled,
                            gboolean                   persist_allowed,
                            RemoteDesktopDialogHandle *dialog_handle)
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
      g_autoptr(GError) error = NULL;

      if (!start_session (dialog_handle->session,
                          device_types,
                          streams,
                          clipboard_enabled,
                          persist_allowed,
                          &error))
        {
          g_warning ("Failed to start session: %s", error->message);
          response = 2;
        }
    }

  if (response != 0)
    cancel_start_session (dialog_handle->session, response);
}

static RemoteDesktopDialogHandle *
create_remote_desktop_dialog (RemoteDesktopSession *session,
                              GDBusMethodInvocation *invocation,
                              Request *request,
                              const char *parent_window)
{
  g_autoptr(GtkWindowGroup) window_group = NULL;
  RemoteDesktopDialogHandle *dialog_handle;
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

  dialog =
    GTK_WINDOW (remote_desktop_dialog_new (request->app_id,
                                           session->select.device_types,
                                           session->select.screen_cast_enable ?
                                             &session->select.screen_cast : NULL,
                                           session->clipboard.clipboard_requested,
                                           session->persist_mode));
  gtk_window_set_transient_for (dialog, GTK_WINDOW (fake_parent));
  gtk_window_set_modal (dialog, TRUE);

  window_group = gtk_window_group_new ();
  gtk_window_group_add_window (window_group, dialog);

  dialog_handle = g_new0 (RemoteDesktopDialogHandle, 1);
  dialog_handle->session = session;
  dialog_handle->request = g_object_ref (request);
  dialog_handle->external_parent = external_parent;
  dialog_handle->dialog = dialog;

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (handle_close), dialog_handle);

  g_signal_connect (dialog, "done",
                    G_CALLBACK (remote_desktop_dialog_done), dialog_handle);

  gtk_widget_realize (GTK_WIDGET (dialog));

  surface = gtk_native_get_surface (GTK_NATIVE (dialog));
  if (external_parent)
    gxdp_external_window_set_parent_of (external_parent, surface);

  gtk_window_present (dialog);

  return dialog_handle;
}

static void
on_mutter_session_closed (OrgGnomeMutterRemoteDesktopSession *mutter_session_proxy,
                          RemoteDesktopSession *remote_desktop_session)
{
  session_close ((Session *)remote_desktop_session, TRUE);
}

static RemoteDesktopSession *
remote_desktop_session_new (const char *app_id,
                            const char *session_handle,
                            const char *peer_name,
                            const char *mutter_session_path,
                            OrgGnomeMutterRemoteDesktopSession *mutter_session_proxy)
{
  RemoteDesktopSession *remote_desktop_session;

  remote_desktop_session = g_object_new (remote_desktop_session_get_type (),
                                         "id", session_handle,
                                         "peer-name", peer_name,
                                         NULL);
  remote_desktop_session->mutter_session_path =
    g_strdup (mutter_session_path);
  remote_desktop_session->mutter_session_proxy =
    g_object_ref (mutter_session_proxy);

  remote_desktop_session->closed_handler_id =
    g_signal_connect (remote_desktop_session->mutter_session_proxy,
                      "closed", G_CALLBACK (on_mutter_session_closed),
                      remote_desktop_session);

  return remote_desktop_session;
}

static gboolean
handle_create_session (XdpImplRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  g_autofree char *mutter_session_path = NULL;
  g_autoptr(GError) error = NULL;
  int response;
  OrgGnomeMutterRemoteDesktopSession *mutter_session_proxy = NULL;
  Session *session;
  GVariantBuilder results_builder;

  sender = g_dbus_method_invocation_get_sender (invocation);

  request = request_new (sender, arg_app_id, arg_handle);

  if (!org_gnome_mutter_remote_desktop_call_create_session_sync (remote_desktop,
                                                                 &mutter_session_path,
                                                                 NULL,
                                                                 &error))
    {
      g_warning ("Failed to create remote desktop session: %s", error->message);
      response = 2;
      goto out;
    }

  mutter_session_proxy =
    org_gnome_mutter_remote_desktop_session_proxy_new_sync (impl_connection,
                                                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                            "org.gnome.Mutter.RemoteDesktop",
                                                            mutter_session_path,
                                                            NULL,
                                                            &error);
  if (!mutter_session_proxy)
    {
      g_warning ("Failed to get remote desktop session proxy: %s", error->message);
      response = 2;
      goto out;
    }

  session = (Session *)remote_desktop_session_new (arg_app_id,
                                                   arg_session_handle,
                                                   sender,
                                                   mutter_session_path,
                                                   mutter_session_proxy);

  if (!session_export (session,
                       g_dbus_method_invocation_get_connection (invocation),
                       &error))
    {
      g_clear_object (&session);
      g_warning ("Failed to create remote desktop session: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

out:
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_remote_desktop_complete_create_session (object,
                                                   invocation,
                                                   response,
                                                   g_variant_builder_end (&results_builder));

  g_clear_object (&mutter_session_proxy);

  return TRUE;
}

static gboolean
handle_select_devices (XdpImplRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_handle,
                       const char *arg_session_handle,
                       const char *arg_app_id,
                       GVariant *arg_options)
{
  g_autofree char *provider = NULL;
  g_autoptr(GVariant) restore_data = NULL;
  const char *sender;
  g_autoptr(Request) request = NULL;
  RemoteDesktopSession *remote_desktop_session;
  int response;
  uint32_t device_types;
  GVariantBuilder results_builder;
  GVariant *results;
  uint32_t version;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  if (!remote_desktop_session)
    {
      g_warning ("Tried to select sources on non-existing %s", arg_session_handle);
      response = 2;
      goto out;
    }

  if (!g_variant_lookup (arg_options, "types", "u", &device_types))
    device_types = REMOTE_DESKTOP_DEVICE_TYPE_ALL;

  remote_desktop_session->select.device_types = device_types;
  response = 0;

  g_variant_lookup (arg_options, "persist_mode", "u",
                    &remote_desktop_session->persist_mode);

  if (g_variant_lookup (arg_options, "restore_data", "(suv)",
                        &provider, &version, &restore_data))
    {
      if (!g_variant_check_format_string (restore_data,
                                          REMOTE_DESKTOP_RESTORE_VARIANT_TYPE,
                                          FALSE))
        {
          g_warning ("Cannot parse restore data, ignoring");
          goto out;
        }

      if (g_strcmp0 (provider, "GNOME") == 0 &&
          version == RESTORE_FORMAT_VERSION)
        remote_desktop_session->restored.data = g_variant_ref (restore_data);
    }

out:
  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  results = g_variant_builder_end (&results_builder);
  xdp_impl_remote_desktop_complete_select_devices (object, invocation,
                                                   response, results);

  return TRUE;
}

static GVariant *
serialize_session_as_restore_data (RemoteDesktopSession *remote_desktop_session,
                                   GPtrArray            *streams)
{
  GVariantBuilder restore_data_builder;
  GVariantBuilder impl_builder;
  int64_t creation_time;
  int64_t last_used_time;

  if ((!streams || streams->len == 0) &&
      !remote_desktop_session->shared.device_types)
    return NULL;

  last_used_time = g_get_real_time ();
  if (remote_desktop_session->restored.creation_time != -1)
    creation_time = remote_desktop_session->restored.creation_time;
  else
    creation_time = g_get_real_time ();

  g_variant_builder_init (&impl_builder,
                          G_VARIANT_TYPE (REMOTE_DESKTOP_RESTORE_VARIANT_TYPE));
  g_variant_builder_add (&impl_builder, "x", creation_time);
  g_variant_builder_add (&impl_builder, "x", last_used_time);

  g_variant_builder_add (&impl_builder, "u",
                         remote_desktop_session->shared.device_types);
  g_variant_builder_add (&impl_builder, "b",
                         remote_desktop_session->clipboard.clipboard_enabled);
  serialize_screen_cast_streams_as_restore_data (streams, &impl_builder);

  g_variant_builder_init (&restore_data_builder, G_VARIANT_TYPE ("(suv)"));
  g_variant_builder_add (&restore_data_builder, "s", "GNOME");
  g_variant_builder_add (&restore_data_builder, "u", RESTORE_FORMAT_VERSION);
  g_variant_builder_add (&restore_data_builder, "v", g_variant_builder_end (&impl_builder));
  return g_variant_builder_end (&restore_data_builder);
}

static void
start_done (RemoteDesktopSession *remote_desktop_session)
{
  GVariantBuilder results_builder;
  RemoteDesktopDeviceType shared_device_types;
  GnomeScreenCastSession *gnome_screen_cast_session;
  gboolean clipboard_enabled;
  RemoteDesktopPersistMode persist_mode;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  shared_device_types = remote_desktop_session->shared.device_types;
  g_variant_builder_add (&results_builder, "{sv}",
                         "devices", g_variant_new_uint32 (shared_device_types));

  if (remote_desktop_session->clipboard.clipboard_requested)
  {
    g_autoptr (GError) error = NULL;
    GVariantBuilder options_builder;
    GVariant *options;

    remote_desktop_session->clipboard.selection_owner_changed_handler_id =
      g_signal_connect (remote_desktop_session->mutter_session_proxy,
                        "selection-owner-changed",
                        G_CALLBACK (on_selection_owner_changed),
                        remote_desktop_session);
    remote_desktop_session->clipboard.selection_transfer_handler_id =
      g_signal_connect (remote_desktop_session->mutter_session_proxy,
                        "selection-transfer",
                        G_CALLBACK (on_selection_transfer),
                        remote_desktop_session);

    g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
    options = g_variant_builder_end (&options_builder);

    if (!org_gnome_mutter_remote_desktop_session_call_enable_clipboard_sync (
      remote_desktop_session->mutter_session_proxy, options, NULL, &error))
      {
        g_warning ("Failed to enable clipboard: %s", error->message);
      }

    clipboard_enabled = remote_desktop_session->clipboard.clipboard_enabled;
    g_variant_builder_add (&results_builder, "{sv}", "clipboard_enabled",
                           g_variant_new_boolean (clipboard_enabled));
  }

  gnome_screen_cast_session = remote_desktop_session->gnome_screen_cast_session;
  if (gnome_screen_cast_session)
    {
      GVariantBuilder streams_builder;

      g_variant_builder_init (&streams_builder, G_VARIANT_TYPE ("a(ua{sv})"));
      gnome_screen_cast_session_add_stream_properties (gnome_screen_cast_session,
                                                       &streams_builder);
      g_variant_builder_add (&results_builder, "{sv}",
                             "streams",
                             g_variant_builder_end (&streams_builder));
    }

  persist_mode = remote_desktop_session->persist_mode;
  if (persist_mode != REMOTE_DESKTOP_PERSIST_MODE_NONE &&
      remote_desktop_session->shared.persist_allowed)
    {
      g_autoptr(GPtrArray) streams = NULL;
      GVariant *restore_data;

      streams = g_steal_pointer (&remote_desktop_session->streams_to_restore);
      restore_data = serialize_session_as_restore_data (remote_desktop_session,
                                                        streams);

      if (restore_data)
        {
          g_variant_builder_add (&results_builder, "{sv}", "persist_mode",
                                 g_variant_new_uint32 (persist_mode));
          g_variant_builder_add (&results_builder, "{sv}", "restore_data",
                                 restore_data);
        }
    }

  xdp_impl_remote_desktop_complete_start (XDP_IMPL_REMOTE_DESKTOP (impl),
                                          remote_desktop_session->start_invocation,
                                          0,
                                          g_variant_builder_end (&results_builder));
  remote_desktop_session->start_invocation = NULL;
}

static void
on_gnome_screen_cast_session_ready (GnomeScreenCastSession *gnome_screen_cast_session,
                                    RemoteDesktopSession *remote_desktop_session)
{
  start_done (remote_desktop_session);
}

static gboolean
open_screen_cast_session (RemoteDesktopSession  *remote_desktop_session,
                          GPtrArray             *streams,
                          GError               **error)
{
  OrgGnomeMutterRemoteDesktopSession *session_proxy =
    remote_desktop_session->mutter_session_proxy;
  GnomeScreenCastSession *gnome_screen_cast_session;
  const char *remote_desktop_session_id;

  remote_desktop_session_id =
    org_gnome_mutter_remote_desktop_session_get_session_id (session_proxy);
  gnome_screen_cast_session =
    gnome_screen_cast_create_session (gnome_screen_cast,
                                      remote_desktop_session_id,
                                      error);
  if (!gnome_screen_cast_session)
    return FALSE;

  remote_desktop_session->gnome_screen_cast_session = gnome_screen_cast_session;
  remote_desktop_session->session_ready_handler_id =
    g_signal_connect (gnome_screen_cast_session, "ready",
                      G_CALLBACK (on_gnome_screen_cast_session_ready),
                      remote_desktop_session);

  if (remote_desktop_session->persist_mode != REMOTE_DESKTOP_PERSIST_MODE_NONE)
    remote_desktop_session->streams_to_restore = g_ptr_array_ref (streams);

  if (!gnome_screen_cast_session_record_selections (gnome_screen_cast_session,
                                                    streams,
                                                    &remote_desktop_session->select.screen_cast,
                                                    error))
    return FALSE;

  return TRUE;
}

static gboolean
start_session (RemoteDesktopSession     *remote_desktop_session,
               RemoteDesktopDeviceType   device_types,
               GPtrArray                *streams,
               gboolean                  clipboard_enabled,
               gboolean                  persist_allowed,
               GError                  **error)
{
  OrgGnomeMutterRemoteDesktopSession *session_proxy;
  gboolean need_streams;

  remote_desktop_session->shared.persist_allowed = persist_allowed;
  remote_desktop_session->shared.device_types = device_types;
  remote_desktop_session->clipboard.clipboard_enabled = clipboard_enabled;

  if (streams)
    {
      if (!open_screen_cast_session (remote_desktop_session, streams, error))
        return FALSE;

      need_streams = TRUE;
    }
  else
    {
      need_streams = FALSE;
    }

  session_proxy = remote_desktop_session->mutter_session_proxy;
  if (!org_gnome_mutter_remote_desktop_session_call_start_sync (session_proxy,
                                                                NULL,
                                                                error))
    return FALSE;

  if (!need_streams)
    start_done (remote_desktop_session);

  return TRUE;
}

static void
cancel_start_session (RemoteDesktopSession *remote_desktop_session,
                      int response)
{
  GVariantBuilder results_builder;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_remote_desktop_complete_start (XDP_IMPL_REMOTE_DESKTOP (impl),
                                          remote_desktop_session->start_invocation,
                                          response,
                                          g_variant_builder_end (&results_builder));
}

static gboolean
restore_from_data (RemoteDesktopSession *remote_desktop_session)
{
  g_autoptr(GPtrArray) streams = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariantIter) screen_cast_streams_iter = NULL;
  int64_t creation_time;
  int64_t last_used_time;
  uint32_t restored_device_types;
  gboolean clipboard_enabled;

  if (!remote_desktop_session->restored.data)
    return FALSE;

  g_variant_get (remote_desktop_session->restored.data,
                 REMOTE_DESKTOP_RESTORE_VARIANT_TYPE,
                 &creation_time,
                 &last_used_time,
                 &restored_device_types,
                 &clipboard_enabled,
                 &screen_cast_streams_iter);

  if (restored_device_types & ~REMOTE_DESKTOP_DEVICE_TYPE_ALL)
    {
      g_warning ("Tried to restore bogus device type mask 0x%x",
                 restored_device_types);
      return FALSE;
    }

  streams = restore_screen_cast_streams (screen_cast_streams_iter,
                                         &remote_desktop_session->select.screen_cast);

  remote_desktop_session->restored.creation_time = creation_time;

  if (!start_session (remote_desktop_session,
                      restored_device_types, streams,
                      clipboard_enabled,
                      TRUE,
                      &error))
    {
      g_warning ("Error restoring stream from session: %s", error->message);
      return FALSE;
    }

  return TRUE;
}

static gboolean
handle_start (XdpImplRemoteDesktop *object,
              GDBusMethodInvocation *invocation,
              const char *arg_handle,
              const char *arg_session_handle,
              const char *arg_app_id,
              const char *arg_parent_window,
              GVariant *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  RemoteDesktopSession *remote_desktop_session;
  GVariantBuilder results_builder;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);
  request_export (request,
                  g_dbus_method_invocation_get_connection (invocation));

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  if (!remote_desktop_session)
    {
      g_warning ("Attempted to start non existing remote desktop session");
      goto err;
    }

  if (remote_desktop_session->dialog_handle)
    {
      g_warning ("Screen cast dialog already open");
      goto err;
    }

  remote_desktop_session->start_invocation = invocation;

  if (!restore_from_data (remote_desktop_session))
    {
      RemoteDesktopDialogHandle *dialog_handle;

      dialog_handle = create_remote_desktop_dialog (remote_desktop_session,
                                                    invocation,
                                                    request,
                                                    arg_parent_window);
      remote_desktop_session->dialog_handle = dialog_handle;
    }

  return TRUE;

err:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE ("a(ua{sv}"));
  xdp_impl_remote_desktop_complete_start (object, invocation, 2,
                                          g_variant_builder_end (&results_builder));

  return TRUE;
}

static gboolean
handle_notify_pointer_motion (XdpImplRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              double dx,
                              double dy)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  org_gnome_mutter_remote_desktop_session_call_notify_pointer_motion_relative (proxy,
                                                                               dx, dy,
                                                                               NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_motion (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_motion_absolute (XdpImplRemoteDesktop *object,
                                       GDBusMethodInvocation *invocation,
                                       const char *arg_session_handle,
                                       GVariant *arg_options,
                                       uint32_t stream,
                                       double x,
                                       double y)
{
  RemoteDesktopSession *remote_desktop_session;
  GnomeScreenCastSession *gnome_screen_cast_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;
  const char *stream_path;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;
  gnome_screen_cast_session = remote_desktop_session->gnome_screen_cast_session;

  stream_path = gnome_screen_cast_session_get_stream_path_from_id (gnome_screen_cast_session,
                                                                   stream);
  org_gnome_mutter_remote_desktop_session_call_notify_pointer_motion_absolute (proxy,
                                                                               stream_path,
                                                                               x, y,
                                                                               NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_motion_absolute (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_button (XdpImplRemoteDesktop *object,
                              GDBusMethodInvocation *invocation,
                              const char *arg_session_handle,
                              GVariant *arg_options,
                              int32_t button,
                              uint32_t state)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  org_gnome_mutter_remote_desktop_session_call_notify_pointer_button (proxy,
                                                                      button, state,
                                                                      NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_button (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_axis (XdpImplRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            double dx,
                            double dy)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;
  gboolean finish;
  unsigned int flags = 0;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  if (g_variant_lookup (arg_options, "finish", "b", &finish))
    {
      if (finish)
        flags = GNOME_REMOTE_DESKTOP_NOTIFY_AXIS_FLAGS_FINISH;
    }

  org_gnome_mutter_remote_desktop_session_call_notify_pointer_axis (proxy,
                                                                    dx, dy, flags,
                                                                    NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_axis (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_pointer_axis_discrete (XdpImplRemoteDesktop *object,
                                     GDBusMethodInvocation *invocation,
                                     const char *arg_session_handle,
                                     GVariant *arg_options,
                                     uint32_t axis,
                                     int32_t steps)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  org_gnome_mutter_remote_desktop_session_call_notify_pointer_axis_discrete (proxy,
                                                                             axis, steps,
                                                                             NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_pointer_axis_discrete (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_keyboard_keycode (XdpImplRemoteDesktop *object,
                                GDBusMethodInvocation *invocation,
                                const char *arg_session_handle,
                                GVariant *arg_options,
                                int32_t keycode,
                                uint32_t state)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  org_gnome_mutter_remote_desktop_session_call_notify_keyboard_keycode (proxy,
                                                                        keycode, state,
                                                                        NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_keyboard_keycode (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_keyboard_keysym (XdpImplRemoteDesktop *object,
                               GDBusMethodInvocation *invocation,
                               const char *arg_session_handle,
                               GVariant *arg_options,
                               int32_t keysym,
                               uint32_t state)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  org_gnome_mutter_remote_desktop_session_call_notify_keyboard_keysym (proxy,
                                                                       keysym, state,
                                                                       NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_keyboard_keysym (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_touch_down (XdpImplRemoteDesktop *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_session_handle,
                          GVariant *arg_options,
                          uint32_t stream,
                          uint32_t slot,
                          double x,
                          double y)
{
  RemoteDesktopSession *remote_desktop_session;
  GnomeScreenCastSession *gnome_screen_cast_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;
  const char *stream_path;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;
  gnome_screen_cast_session = remote_desktop_session->gnome_screen_cast_session;

  stream_path = gnome_screen_cast_session_get_stream_path_from_id (gnome_screen_cast_session,
                                                                   stream);
  org_gnome_mutter_remote_desktop_session_call_notify_touch_down (proxy,
                                                                  stream_path,
                                                                  slot,
                                                                  x, y,
                                                                  NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_touch_down (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_touch_motion (XdpImplRemoteDesktop *object,
                            GDBusMethodInvocation *invocation,
                            const char *arg_session_handle,
                            GVariant *arg_options,
                            uint32_t stream,
                            uint32_t slot,
                            double x,
                            double y)
{
  RemoteDesktopSession *remote_desktop_session;
  GnomeScreenCastSession *gnome_screen_cast_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;
  const char *stream_path;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;
  gnome_screen_cast_session = remote_desktop_session->gnome_screen_cast_session;

  stream_path = gnome_screen_cast_session_get_stream_path_from_id (gnome_screen_cast_session,
                                                                   stream);
  org_gnome_mutter_remote_desktop_session_call_notify_touch_motion (proxy,
                                                                    stream_path,
                                                                    slot,
                                                                    x, y,
                                                                    NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_touch_motion (object, invocation);
  return TRUE;
}

static gboolean
handle_notify_touch_up (XdpImplRemoteDesktop *object,
                        GDBusMethodInvocation *invocation,
                        const char *arg_session_handle,
                        GVariant *arg_options,
                        uint32_t slot)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  org_gnome_mutter_remote_desktop_session_call_notify_touch_up (proxy,
                                                                slot,
                                                                NULL, NULL, NULL);

  xdp_impl_remote_desktop_complete_notify_touch_up (object, invocation);
  return TRUE;
}

static gboolean
handle_connect_to_eis (XdpImplRemoteDesktop *object,
                       GDBusMethodInvocation *invocation,
                       const char *arg_app_id,
                       const char *arg_session_handle,
                       GVariant *arg_options)
{
  RemoteDesktopSession *remote_desktop_session;
  OrgGnomeMutterRemoteDesktopSession *proxy;
  g_autoptr(GError) error = NULL;
  g_autoptr(GUnixFDList) in_fd_list = NULL;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  GVariant *out_fd;
  GVariantBuilder options_builder;
  GVariant *options;

  remote_desktop_session =
    (RemoteDesktopSession *)lookup_session (arg_session_handle);
  proxy = remote_desktop_session->mutter_session_proxy;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options_builder, "{sv}", "device-types",
                         g_variant_new_uint32 (remote_desktop_session->shared.device_types));
  options = g_variant_builder_end (&options_builder);

  if (!org_gnome_mutter_remote_desktop_session_call_connect_to_eis_sync (proxy,
                                                                         options,
                                                                         in_fd_list,
                                                                         &out_fd,
                                                                         &out_fd_list,
                                                                         NULL,
                                                                         &error))
    {
      g_warning ("Failed to connect to EIS: %s", error->message);
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to connect to EIS");

      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_impl_remote_desktop_complete_connect_to_eis (object, invocation, out_fd_list, out_fd);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static unsigned int
gnome_device_types_xdp_device_types (unsigned int gnome_device_types)
{
  unsigned int supported_device_types = REMOTE_DESKTOP_DEVICE_TYPE_NONE;

  if (gnome_device_types & GNOME_REMOTE_DESKTOP_DEVICE_TYPE_POINTER)
    supported_device_types |= REMOTE_DESKTOP_DEVICE_TYPE_POINTER;
  if (gnome_device_types & GNOME_REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD)
    supported_device_types |= REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD;
  if (gnome_device_types & GNOME_REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN)
    supported_device_types |= REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN;

  return supported_device_types;
}

static void
remote_desktop_name_appeared (GDBusConnection *connection,
                              const char *name,
                              const char *name_owner,
                              gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  unsigned int supported_device_types;

  remote_desktop =
    org_gnome_mutter_remote_desktop_proxy_new_sync (impl_connection,
                                                    G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                    "org.gnome.Mutter.RemoteDesktop",
                                                    "/org/gnome/Mutter/RemoteDesktop",
                                                    NULL,
                                                    &error);
  if (!remote_desktop)
    {
      g_warning ("Failed to acquire org.gnome.Mutter.RemoteDesktop proxy: %s",
                 error->message);
      return;
    }

  impl = G_DBUS_INTERFACE_SKELETON (xdp_impl_remote_desktop_skeleton_new ());
  xdp_impl_remote_desktop_set_version (XDP_IMPL_REMOTE_DESKTOP (impl), 2);

  g_signal_connect (impl, "handle-create-session",
                    G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (impl, "handle-select-devices",
                    G_CALLBACK (handle_select_devices), NULL);
  g_signal_connect (impl, "handle-start",
                    G_CALLBACK (handle_start), NULL);

  g_signal_connect (impl, "handle-notify-pointer-motion",
                    G_CALLBACK (handle_notify_pointer_motion), NULL);
  g_signal_connect (impl, "handle-notify-pointer-motion-absolute",
                    G_CALLBACK (handle_notify_pointer_motion_absolute), NULL);
  g_signal_connect (impl, "handle-notify-pointer-button",
                    G_CALLBACK (handle_notify_pointer_button), NULL);
  g_signal_connect (impl, "handle-notify-pointer-axis",
                    G_CALLBACK (handle_notify_pointer_axis), NULL);
  g_signal_connect (impl, "handle-notify-pointer-axis-discrete",
                    G_CALLBACK (handle_notify_pointer_axis_discrete), NULL);
  g_signal_connect (impl, "handle-notify-keyboard-keycode",
                    G_CALLBACK (handle_notify_keyboard_keycode), NULL);
  g_signal_connect (impl, "handle-notify-keyboard-keysym",
                    G_CALLBACK (handle_notify_keyboard_keysym), NULL);
  g_signal_connect (impl, "handle-notify-touch-down",
                    G_CALLBACK (handle_notify_touch_down), NULL);
  g_signal_connect (impl, "handle-notify-touch-motion",
                    G_CALLBACK (handle_notify_touch_motion), NULL);
  g_signal_connect (impl, "handle-notify-touch-up",
                    G_CALLBACK (handle_notify_touch_up), NULL);
  g_signal_connect (impl, "handle-connect-to-eis",
                    G_CALLBACK (handle_connect_to_eis), NULL);

  supported_device_types =
    org_gnome_mutter_remote_desktop_get_supported_device_types (remote_desktop);
  g_object_set (G_OBJECT (impl),
                "available-device-types",
                gnome_device_types_xdp_device_types (supported_device_types),
                NULL);

  if (!g_dbus_interface_skeleton_export (impl,
                                         impl_connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Failed to export remote desktop portal implementation object: %s",
                 error->message);
      return;
    }

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (impl)->name);
}

static void
remote_desktop_name_vanished (GDBusConnection *connection,
                              const char *name,
                              gpointer user_data)
{
  if (impl)
    {
      g_dbus_interface_skeleton_unexport (impl);
      g_clear_object (&impl);
    }

  g_clear_object (&remote_desktop);
}

gboolean
remote_desktop_init (GDBusConnection *connection,
                     GError **error)
{
  impl_connection = connection;
  gnome_screen_cast = gnome_screen_cast_new (connection);

  remote_desktop_name_watch = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                                "org.gnome.Mutter.RemoteDesktop",
                                                G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                remote_desktop_name_appeared,
                                                remote_desktop_name_vanished,
                                                NULL,
                                                NULL);

  return TRUE;
}

static void
remote_desktop_session_close (Session *session)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)session;
  OrgGnomeMutterRemoteDesktopSession *session_proxy;
  GnomeScreenCastSession *gnome_screen_cast_session;
  g_autoptr(GError) error = NULL;

  gnome_screen_cast_session = remote_desktop_session->gnome_screen_cast_session;
  if (gnome_screen_cast_session)
    {
      g_signal_handler_disconnect (gnome_screen_cast_session,
                                   remote_desktop_session->session_ready_handler_id);
      g_clear_object (&remote_desktop_session->gnome_screen_cast_session);
    }

  session_proxy = remote_desktop_session->mutter_session_proxy;
  g_signal_handler_disconnect (session_proxy,
                               remote_desktop_session->closed_handler_id);

  if (remote_desktop_session->clipboard.clipboard_enabled)
    {
      g_signal_handler_disconnect (
          session_proxy, remote_desktop_session->clipboard
                             .selection_owner_changed_handler_id);
      g_signal_handler_disconnect (
          session_proxy, remote_desktop_session->clipboard
                             .selection_transfer_handler_id);
    }

  if (!org_gnome_mutter_remote_desktop_session_call_stop_sync (session_proxy,
                                                               NULL,
                                                               &error))
    {
      g_warning ("Failed to stop screen cast session: %s", error->message);
      return;
    }
}

static void
remote_desktop_session_finalize (GObject *object)
{
  RemoteDesktopSession *remote_desktop_session = (RemoteDesktopSession *)object;

  g_clear_pointer (&remote_desktop_session->streams_to_restore,
                   g_ptr_array_unref);
  g_clear_pointer (&remote_desktop_session->restored.data, g_variant_unref);
  g_free (remote_desktop_session->mutter_session_path);

  G_OBJECT_CLASS (remote_desktop_session_parent_class)->finalize (object);
}

static void
remote_desktop_session_init (RemoteDesktopSession *remote_desktop_session)
{
}

static void
remote_desktop_session_class_init (RemoteDesktopSessionClass *klass)
{
  GObjectClass *gobject_class;
  SessionClass *session_class;

  gobject_class = (GObjectClass *)klass;
  gobject_class->finalize = remote_desktop_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = remote_desktop_session_close;

  signals[CLIPBOARD_SELECTION_OWNER_CHANGED] =
    g_signal_new ("clipboard-selection-owner-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_VARIANT);

  signals[CLIPBOARD_SELECTION_TRANSFER] =
    g_signal_new ("clipboard-selection-transfer",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE,
                  2,
                  G_TYPE_STRING,
                  G_TYPE_INT);
}
