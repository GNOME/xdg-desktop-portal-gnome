/*
 * Copyright 2022 Google LLC
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

#include "clipboard.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <stdint.h>
#include <gxdp.h>

#include "config.h"
#include "gnomescreencast.h"
#include "remotedesktop.h"
#include "request.h"
#include "session.h"
#include "shell-dbus.h"
#include "utils.h"
#include "xdg-desktop-portal-dbus.h"

static GDBusConnection *impl_connection;
static guint remote_desktop_name_watch;
static GDBusInterfaceSkeleton *impl;
static OrgGnomeMutterRemoteDesktop *remote_desktop;

static void
on_selection_owner_changed (Session  *session,
                            GVariant *arg_options)
{
  g_autoptr (GVariant) session_is_owner = NULL;
  g_autoptr (GVariant) mime_types = NULL;
  GVariantBuilder options_builder;
  GVariant *options;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);

  /* Translating mime-types and session-is-owner for portal's interface
   * specification.
   */
  mime_types = g_variant_lookup_value (arg_options, "mime-types", G_VARIANT_TYPE ("(as)"));
  if (mime_types)
    {
      g_variant_builder_add (&options_builder,
                             "{sv}",
                             "mime_types",
                             g_steal_pointer (&mime_types));
    }

  session_is_owner = g_variant_lookup_value (arg_options,
                                             "session-is-owner",
                                             G_VARIANT_TYPE_BOOLEAN);
  if (session_is_owner)
    {
      g_variant_builder_add (&options_builder,
                             "{sv}",
                             "session_is_owner",
                             g_steal_pointer (&session_is_owner));
    }

  options = g_variant_builder_end (&options_builder);

  xdp_impl_clipboard_emit_selection_owner_changed (XDP_IMPL_CLIPBOARD (impl),
                                                   session->id,
                                                   options);
}

static void
on_selection_transfer (Session    *session,
                       const char *arg_mime_type,
                       uint32_t    arg_serial)
{
  xdp_impl_clipboard_emit_selection_transfer (XDP_IMPL_CLIPBOARD (impl),
                                              session->id,
                                              arg_mime_type,
                                              arg_serial);
}

static gboolean
handle_request_clipboard (XdpImplClipboard      *object,
                          GDBusMethodInvocation *invocation,
                          const char            *arg_session_handle)
{
  RemoteDesktopSession *remote_desktop_session;
  Session *session;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to enable clipboard on non-existing %s",
                 arg_session_handle);
      goto out;
    }

  if (!is_remote_desktop_session (session))
    {
      g_warning ("Tried to enable clipboard on invalid session type");
      goto out;
    }

  remote_desktop_session = (RemoteDesktopSession *)session;

  g_signal_connect (remote_desktop_session,
                    "clipboard-selection-owner-changed",
                    G_CALLBACK (on_selection_owner_changed), NULL);
  g_signal_connect (remote_desktop_session, "clipboard-selection-transfer",
                    G_CALLBACK (on_selection_transfer), NULL);

  remote_desktop_session_request_clipboard (remote_desktop_session);

out:
  xdp_impl_clipboard_complete_request_clipboard (object, invocation);
  return TRUE;
}

static gboolean
handle_set_selection (XdpImplClipboard      *object,
                      GDBusMethodInvocation *invocation,
                      const char            *arg_session_handle,
                      GVariant              *arg_options)
{
  OrgGnomeMutterRemoteDesktopSession *session_proxy;
  RemoteDesktopSession *remote_desktop_session;
  g_autoptr (GVariant) value = NULL;
  g_autoptr (GError) error = NULL;
  GVariantBuilder options_builder;
  Session *session;
  GVariant *options;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to set selection on non-existing %s",
                 arg_session_handle);
      goto out;
    }

  if (!is_remote_desktop_session (session))
    {
      g_warning ("Tried to set selection on invalid session type");
      goto out;
    }

  remote_desktop_session = (RemoteDesktopSession *)session;
  if (!remote_desktop_session_is_clipboard_enabled (remote_desktop_session))
    {
      g_warning ("Tried to set selection with clipboard disabled");
      goto out;
    }

  session_proxy =
    remote_desktop_session_mutter_session_proxy (remote_desktop_session);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);

  /* translating mime_types signature to mime-types (which is what mutter
    expects) */
  value = g_variant_lookup_value (arg_options,
                                  "mime_types",
                                  G_VARIANT_TYPE_STRING_ARRAY);

  if (value)
    {
      g_variant_builder_add (&options_builder,
                             "{sv}",
                             "mime-types",
                             g_steal_pointer (&value));
    }

  options = g_variant_builder_end (&options_builder);

  if (!org_gnome_mutter_remote_desktop_session_call_set_selection_sync (session_proxy,
                                                                        options,
                                                                        NULL,
                                                                        &error))
    {
      g_warning ("Failed to set selection: %s", error->message);
      goto out;
    }

out:
  xdp_impl_clipboard_complete_set_selection (object, invocation);
  return TRUE;
}

static gboolean
handle_selection_write (XdpImplClipboard      *object,
                        GDBusMethodInvocation *invocation,
                        GUnixFDList           *in_fd_list,
                        const char            *arg_session_handle,
                        uint32_t               arg_serial)
{
  OrgGnomeMutterRemoteDesktopSession *session_proxy;
  RemoteDesktopSession *remote_desktop_session;
  g_autoptr (GUnixFDList) out_fd_list = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  g_autoptr (GError) error = NULL;
  GVariant *fd_handle;
  Session *session;
  int out_fd_id;
  int fd_id;
  int fd;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to write selection on non-existing %s",
                 arg_session_handle);
      goto out;
    }

  if (!is_remote_desktop_session (session))
    {
      g_warning ("Tried to write selection on invalid session type");
      goto out;
    }

  remote_desktop_session = (RemoteDesktopSession *)session;
  if (!remote_desktop_session_is_clipboard_enabled (remote_desktop_session))
    {
      g_warning ("Tried to write selection with clipboard disabled");
      goto out;
    }

  session_proxy =
    remote_desktop_session_mutter_session_proxy (remote_desktop_session);

  out_fd_list = g_unix_fd_list_new ();

  if (!org_gnome_mutter_remote_desktop_session_call_selection_write_sync (session_proxy,
                                                                          arg_serial,
                                                                          NULL,
                                                                          &fd_handle,
                                                                          &fd_list,
                                                                          NULL,
                                                                          &error))
    {
      g_warning ("Failed to selection write: %s", error->message);
      goto out;
    }

  fd_id = g_variant_get_handle (fd_handle);
  fd = g_unix_fd_list_get (fd_list, fd_id, &error);
  out_fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
  close (fd);

out:
  xdp_impl_clipboard_complete_selection_write (object,
                                               invocation,
                                               out_fd_list,
                                               g_variant_new_handle (out_fd_id));
  return TRUE;
}

static gboolean
handle_selection_write_done (XdpImplClipboard      *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_session_handle,
                             uint32_t               arg_serial,
                             gboolean               arg_success)
{
  OrgGnomeMutterRemoteDesktopSession *session_proxy;
  g_autoptr (GUnixFDList) out_fd_list = NULL;
  RemoteDesktopSession *remote_desktop_session;
  g_autoptr (GError) error = NULL;
  Session *session;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to write selection on non-existing %s",
                 arg_session_handle);
      goto out;
    }

  if (!is_remote_desktop_session (session))
    {
      g_warning ("Tried to write selection on invalid session type");
      goto out;
    }

  remote_desktop_session = (RemoteDesktopSession *)session;
  if (!remote_desktop_session_is_clipboard_enabled (remote_desktop_session))
    {
      g_warning ("Tried to write selection with clipboard disabled");
      goto out;
    }

  session_proxy =
    remote_desktop_session_mutter_session_proxy (remote_desktop_session);

  if (!org_gnome_mutter_remote_desktop_session_call_selection_write_done_sync (session_proxy,
                                                                               arg_serial,
                                                                               arg_success,
                                                                               NULL,
                                                                               &error))
    {
      g_warning ("Failed to selection write: %s", error->message);
      goto out;
    }

out:
  xdp_impl_clipboard_complete_selection_write_done (object, invocation);
  return TRUE;
}

static gboolean
handle_selection_read (XdpImplClipboard      *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList           *in_fd_list,
                       const char            *arg_session_handle,
                       const char            *arg_mime_type)
{
  OrgGnomeMutterRemoteDesktopSession *session_proxy;
  g_autoptr (GUnixFDList) out_fd_list = NULL;
  RemoteDesktopSession *remote_desktop_session;
  g_autoptr (GError) error = NULL;
  Session *session;
  GVariant *fd;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to read selection on non-existing %s",
                 arg_session_handle);
      goto out;
    }

  if (!is_remote_desktop_session (session))
    {
      g_warning ("Tried to read selection on invalid session type");
      goto out;
    }

  remote_desktop_session = (RemoteDesktopSession *)session;
  if (!remote_desktop_session_is_clipboard_enabled (remote_desktop_session))
    {
      g_warning ("Tried to read selection with clipboard disabled");
      goto out;
    }

  session_proxy =
    remote_desktop_session_mutter_session_proxy (remote_desktop_session);

  if (!org_gnome_mutter_remote_desktop_session_call_selection_read_sync (session_proxy,
                                                                         arg_mime_type,
                                                                         NULL,
                                                                         &fd,
                                                                         &out_fd_list,
                                                                         NULL,
                                                                         &error))
    {
      g_warning ("Failed to selection read: %s", error->message);
      goto out;
    }

out:
  xdp_impl_clipboard_complete_selection_read (object, invocation, out_fd_list, fd);
  return TRUE;
}

static void
remote_desktop_name_appeared (GDBusConnection *connection,
                              const char      *name,
                              const char      *name_owner,
                              gpointer         user_data)
{
  g_autoptr (GError) error = NULL;

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

  impl = G_DBUS_INTERFACE_SKELETON (xdp_impl_clipboard_skeleton_new ());

  g_signal_connect (impl, "handle-request-clipboard",
                    G_CALLBACK (handle_request_clipboard), NULL);
  g_signal_connect (impl, "handle-selection-read",
                    G_CALLBACK (handle_selection_read), NULL);
  g_signal_connect (impl, "handle-selection-write",
                    G_CALLBACK (handle_selection_write), NULL);
  g_signal_connect (impl, "handle-selection-write-done",
                    G_CALLBACK (handle_selection_write_done), NULL);
  g_signal_connect (impl, "handle-set-selection",
                    G_CALLBACK (handle_set_selection), NULL);

  if (!g_dbus_interface_skeleton_export (impl,
                                         impl_connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Failed to export clipboard portal implementation object: %s",
                 error->message);
      return;
    }

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (impl)->name);
}

static void
remote_desktop_name_vanished (GDBusConnection *connection,
                              const char      *name,
                              gpointer         user_data)
{
  if (impl)
    {
      g_dbus_interface_skeleton_unexport (impl);
      g_clear_object (&impl);
    }

  g_clear_object (&remote_desktop);
}

gboolean
clipboard_init (GDBusConnection  *connection,
                GError          **error)
{
  impl_connection = connection;

  remote_desktop_name_watch = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                                "org.gnome.Mutter.RemoteDesktop",
                                                G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                remote_desktop_name_appeared,
                                                remote_desktop_name_vanished,
                                                NULL,
                                                NULL);

  return TRUE;
}
