/*
 * Copyright © 2016 Red Hat, Inc
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
 * Authors:
 *       Matthias Clasen <mclasen@redhat.com>
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gtk/gtk.h>
#include <gxdp.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include <glib/gi18n.h>

#include "xdg-desktop-portal-dbus.h"

#include "filechooser.h"
#include "request.h"
#include "utils.h"

#define FILECHOOSER_SETTINGS_SCHEMA "org.gnome.portal.filechooser"
#define FILECHOOSER_SETTINGS_PATH "/org/gnome/portal/filechooser/"

static XdpImplFileChooser *delegate = NULL;

typedef struct {
  XdpImplFileChooser *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  char *app_id;

  guint response;
  GStrv uris;
  GVariant *out_options;
} FileDialogHandle;

static void
file_dialog_handle_free (gpointer data)
{
  FileDialogHandle *handle = data;

  g_clear_pointer (&handle->app_id, g_free);
  g_clear_object (&handle->invocation);
  g_clear_object (&handle->request);
  g_clear_pointer (&handle->uris, g_strfreev);

  g_free (handle);
}

static GSettings*
get_filechooser_settings_for_app_id (const char *app_id)
{
  g_autoptr(GSettings) settings = NULL;
  g_autoptr(GString) path = NULL;

  g_assert (app_id && g_utf8_validate (app_id, -1, NULL));

  path = g_string_new (FILECHOOSER_SETTINGS_PATH);
  g_string_append (path, app_id);
  g_string_append (path, "/");

  settings = g_settings_new_with_path (FILECHOOSER_SETTINGS_SCHEMA, path->str);

  return g_steal_pointer (&settings);
}

static void
restore_last_folder (FileDialogHandle  *handle,
                     GVariant         **delegated_options)
{
  g_autoptr(GSettings) settings = NULL;
  g_autofree char *last_folder_path = NULL;

  if (!handle->app_id || *handle->app_id == '\0')
    return;

  settings = get_filechooser_settings_for_app_id (handle->app_id);
  last_folder_path = g_settings_get_string (settings, "last-folder-path");

  if (last_folder_path && *last_folder_path)
    {
      GVariantDict dict;

      g_variant_dict_init (&dict, *delegated_options);

      g_variant_dict_insert (&dict, "current_folder", "^ay", last_folder_path);

      g_variant_unref (*delegated_options);
      *delegated_options = g_variant_take_ref (g_variant_dict_end (&dict));
    }
}

static void
save_last_folder (FileDialogHandle *handle)
{
  GStrv uris = handle->uris;
  g_autofree char *path = NULL;

  if (!handle->app_id || *handle->app_id == '\0')
    return;

  for (guint i = 0; uris && uris[i]; i++)
    {
      g_autoptr(GFile) file = g_file_new_for_uri (uris[i]);

      if (g_file_is_native (file))
        {
          path = g_file_get_path (file);

          if (!g_file_test (path, G_FILE_TEST_IS_DIR))
            {
              g_autoptr(GFile) parent = g_file_get_parent (file);

              g_clear_pointer (&path, g_free);
              path = g_file_get_path (parent);
            }

          if (path)
            break;
        }
    }

  if (path)
    {
      g_autoptr(GSettings) settings = get_filechooser_settings_for_app_id (handle->app_id);
      g_settings_set_string (settings, "last-folder-path", path);
    }
}

static void
add_recent_entry (const char *app_id,
                  const char *uri)
{
  GtkRecentManager *recent;

  recent = gtk_recent_manager_get_default ();
  gtk_recent_manager_add_item (recent, uri);
}

static void
send_response (FileDialogHandle *handle)
{
  const char *method_name;
  GStrv uris = handle->uris;
  int i;

  method_name = g_dbus_method_invocation_get_method_name (handle->invocation);

  for (i = 0; uris && uris[i]; i++)
    {
      add_recent_entry (handle->request->app_id, uris[i]);
    }

  if (handle->request->exported)
    request_unexport (handle->request);

  if (strcmp (method_name, "OpenFile") == 0)
    xdp_impl_file_chooser_complete_open_file (handle->impl,
                                              handle->invocation,
                                              handle->response,
                                              handle->out_options);
  else if (strcmp (method_name, "SaveFile") == 0)
    xdp_impl_file_chooser_complete_save_file (handle->impl,
                                              handle->invocation,
                                              handle->response,
                                              handle->out_options);
  else if (strcmp (method_name, "SaveFiles") == 0)
    xdp_impl_file_chooser_complete_save_files (handle->impl,
                                               handle->invocation,
                                               handle->response,
                                               handle->out_options);
  else
    g_assert_not_reached ();

  file_dialog_handle_free (handle);
}

static void
delegate_response (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  FileDialogHandle *handle = user_data;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_file_chooser_call_open_file_finish (XDP_IMPL_FILE_CHOOSER (source),
                                                    &handle->response,
                                                    &handle->out_options,
                                                    result,
                                                    &error))
    {
      handle->response = 2;
      handle->out_options = g_variant_new ("a{sv}", NULL);
      g_dbus_error_strip_remote_error (error);
      g_warning ("Delegated FileChooser call failed: %s", error->message);
    }

  (void) g_variant_lookup (handle->out_options, "uris", "^as", &handle->uris);

  if (handle->response == 0)
    {
      save_last_folder (handle);
    }

  send_response (handle);
}


typedef struct {
  GDBusMethodInvocation *invocation;
  XdpImplRequest *request;
} RequestCloseData;

static void
file_dialog_data_free (gpointer data)
{
  RequestCloseData *_data = data;

  g_clear_object (&_data->request);
  g_free (_data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (RequestCloseData, file_dialog_data_free)

static void
delegate_request_close_callback (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  g_autoptr(RequestCloseData) data = user_data;
  g_autoptr(GError) error = NULL;

  if (!xdp_impl_request_call_close_finish (XDP_IMPL_REQUEST (object),
                                           result,
                                           &error))
    {
      g_dbus_method_invocation_return_gerror (data->invocation, error);
      return;
    }

  xdp_impl_request_complete_close (data->request, data->invocation);
}

static gboolean
handle_close (XdpImplRequest        *object,
              GDBusMethodInvocation *invocation,
              FileDialogHandle      *handle)
{
  g_autoptr(XdpImplRequest) delegate_request = NULL;
  g_autoptr(GError) error = NULL;
  RequestCloseData *data;

  delegate_request = xdp_impl_request_proxy_new_for_bus_sync (
    G_BUS_TYPE_SESSION,
    (G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION |
     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
     G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS),
    "org.gnome.Nautilus",
    handle->request->id,
    NULL, &error);

  if (!delegate_request)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  data = g_new0 (RequestCloseData, 1);
  data->invocation = invocation;
  data->request = g_object_ref (object);

  xdp_impl_request_call_close (delegate_request,
                               NULL,
                               delegate_request_close_callback,
                               data);

  return TRUE;
}

static gboolean
handle_open (XdpImplFileChooser    *object,
             GDBusMethodInvocation *invocation,
             const char            *arg_handle,
             const char            *arg_app_id,
             const char            *arg_parent_window,
             const char            *arg_title,
             GVariant              *arg_options)
{
  g_autoptr(Request) request = NULL;
  g_autoptr(GVariant) delegated_options = g_variant_ref (arg_options);
  const gchar *method_name;
  const gchar *sender;
  FileDialogHandle *handle;
  const char *path;

  method_name = g_dbus_method_invocation_get_method_name (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (FileDialogHandle, 1);
  handle->impl = object;
  handle->invocation = g_object_ref (invocation);
  handle->request = g_object_ref (request);
  handle->app_id = g_strdup (arg_app_id);

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  if (!g_variant_lookup (delegated_options, "current_folder", "^&ay", &path) &&
      !g_variant_lookup (delegated_options, "current_file", "^&ay", &path))
    {
      restore_last_folder (handle, &delegated_options);
    }

  if (strcmp (method_name, "OpenFile") == 0)
    {
      xdp_impl_file_chooser_call_open_file (delegate,
                                            arg_handle, arg_app_id, arg_parent_window, arg_title, delegated_options,
                                            NULL, delegate_response, handle);
    }
  else if (strcmp (method_name, "SaveFile") == 0)
    {
      xdp_impl_file_chooser_call_save_file (delegate,
                                            arg_handle, arg_app_id, arg_parent_window, arg_title, delegated_options,
                                            NULL, delegate_response, handle);
    }
  else if (strcmp (method_name, "SaveFiles") == 0)
    {
      xdp_impl_file_chooser_call_save_files (delegate,
                                             arg_handle, arg_app_id, arg_parent_window, arg_title, delegated_options,
                                             NULL, delegate_response, handle);
    }

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  return TRUE;
}

gboolean
file_chooser_init (GDBusConnection *bus,
                   GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_file_chooser_skeleton_new ());

  g_signal_connect (helper, "handle-open-file", G_CALLBACK (handle_open), NULL);
  g_signal_connect (helper, "handle-save-file", G_CALLBACK (handle_open), NULL);
  g_signal_connect (helper, "handle-save-files", G_CALLBACK (handle_open), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  delegate = xdp_impl_file_chooser_proxy_new_sync (bus,
                                                   G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                                                   G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START_AT_CONSTRUCTION,
                                                   "org.gnome.Nautilus",
                                                   "/org/freedesktop/portal/desktop",
                                                   NULL,
                                                   error);
  if (!delegate)
    return FALSE;

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (delegate), G_MAXINT);

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
