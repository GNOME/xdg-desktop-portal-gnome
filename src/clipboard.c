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

G_DEFINE_INTERFACE (ClipboardSession, clipboard_session, G_TYPE_OBJECT)

static GDBusConnection *impl_connection;
static GDBusInterfaceSkeleton *impl;

gboolean
is_clipboard_session (Session *session)
{
  return CLIPBOARD_IS_SESSION (session);
}

static void
clipboard_session_default_init (ClipboardSessionInterface *iface)
{
}

static void
clipboard_session_request_clipboard (ClipboardSession *clipboard_session)
{
  ClipboardSessionInterface *iface = CLIPBOARD_SESSION_GET_IFACE (clipboard_session);

  iface->request_clipboard (clipboard_session);
}

static OrgGnomeMutterClipboard *
clipboard_session_get_clipboard_proxy (ClipboardSession *clipboard_session)
{
  ClipboardSessionInterface *iface = CLIPBOARD_SESSION_GET_IFACE (clipboard_session);

  return iface->get_clipboard_proxy (clipboard_session);
}

static void
on_selection_owner_changed (OrgGnomeMutterClipboard *clipboard_proxy,
                            GVariant                *arg_options,
                            ClipboardSession        *clipboard_session)
{
  Session *session = (Session *) clipboard_session;
  g_autoptr (GVariant) session_is_owner = NULL;
  g_autoptr (GVariant) mime_types = NULL;
  g_autoptr (GVariant) mutter_mime_types = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr (GVariant) options = NULL;

  /* Translating mime-types and session-is-owner for portal's interface
   * specification.
   */
  mutter_mime_types = g_variant_lookup_value (arg_options, "mime-types", NULL);
  if (mutter_mime_types)
    {
      if (g_variant_is_of_type (mutter_mime_types,
                                G_VARIANT_TYPE_STRING_ARRAY))
        {
          mime_types = g_steal_pointer (&mutter_mime_types);
        }
      else if (g_variant_is_of_type (mutter_mime_types,
                                     G_VARIANT_TYPE ("(as)")))
        {
          mime_types = g_variant_get_child_value (mutter_mime_types, 0);
        }
    }
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

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  xdp_impl_clipboard_emit_selection_owner_changed (XDP_IMPL_CLIPBOARD (impl),
                                                   session->id,
                                                   options);
}

static void
on_selection_transfer (OrgGnomeMutterClipboard *clipboard_proxy,
                       const char              *arg_mime_type,
                       uint32_t                 arg_serial,
                       ClipboardSession        *clipboard_session)
{
  Session *session = (Session *) clipboard_session;

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
  Session *session;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to enable clipboard on non-existing %s",
                 arg_session_handle);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Non-existing session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!is_clipboard_session (session))
    {
      g_warning ("Tried to enable clipboard on invalid session type");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  clipboard_session_request_clipboard (CLIPBOARD_SESSION (session));

  xdp_impl_clipboard_complete_request_clipboard (object,
                                                 g_steal_pointer (&invocation));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_selection (XdpImplClipboard      *object,
                      GDBusMethodInvocation *invocation,
                      const char            *arg_session_handle,
                      GVariant              *arg_options)
{
  OrgGnomeMutterClipboard *clipboard_proxy;
  g_autoptr (GVariant) value = NULL;
  g_autoptr (GError) error = NULL;
  g_auto(GVariantBuilder) options_builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  Session *session;
  g_autoptr(GVariant) options = NULL;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to set selection on non-existing %s",
                 arg_session_handle);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Non-existing session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!is_clipboard_session (session))
    {
      g_warning ("Tried to set selection on invalid session");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  clipboard_proxy =
    clipboard_session_get_clipboard_proxy (CLIPBOARD_SESSION (session));
  if (!clipboard_proxy)
    {
      g_warning ("Tried to set selection with clipboard disabled");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Clipboard disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

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

  options = g_variant_ref_sink (g_variant_builder_end (&options_builder));

  if (!org_gnome_mutter_clipboard_call_set_selection_sync (clipboard_proxy,
                                                           options,
                                                           NULL,
                                                           &error))
    {
      g_warning ("Failed to set selection: %s", error->message);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to set selection");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_impl_clipboard_complete_set_selection (object,
                                             g_steal_pointer (&invocation));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_selection_write (XdpImplClipboard      *object,
                        GDBusMethodInvocation *invocation,
                        GUnixFDList           *in_fd_list,
                        const char            *arg_session_handle,
                        uint32_t               arg_serial)
{
  OrgGnomeMutterClipboard *clipboard_proxy;
  g_autoptr (GUnixFDList) out_fd_list = NULL;
  g_autoptr (GUnixFDList) fd_list = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) fd_handle = NULL;
  Session *session;
  int out_fd_id;
  int fd_id;
  int fd;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to write selection on non-existing %s",
                 arg_session_handle);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Non-existing session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!is_clipboard_session (session))
    {
      g_warning ("Tried to write selection on invalid session type");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  clipboard_proxy =
    clipboard_session_get_clipboard_proxy (CLIPBOARD_SESSION (session));
  if (!clipboard_proxy)
    {
      g_warning ("Tried to write selection with clipboard disabled");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Clipboard disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  out_fd_list = g_unix_fd_list_new ();

  if (!org_gnome_mutter_clipboard_call_selection_write_sync (clipboard_proxy,
                                                             arg_serial,
                                                             NULL,
                                                             &fd_handle,
                                                             &fd_list,
                                                             NULL,
                                                             &error))
    {
      g_warning ("Failed to selection write: %s", error->message);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to selection write");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  fd_id = g_variant_get_handle (fd_handle);
  fd = g_unix_fd_list_get (fd_list, fd_id, &error);
  out_fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
  close (fd);

  xdp_impl_clipboard_complete_selection_write (object,
                                               g_steal_pointer (&invocation),
                                               out_fd_list,
                                               g_variant_new_handle (out_fd_id));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_selection_write_done (XdpImplClipboard      *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_session_handle,
                             uint32_t               arg_serial,
                             gboolean               arg_success)
{
  OrgGnomeMutterClipboard *clipboard_proxy;
  g_autoptr (GUnixFDList) out_fd_list = NULL;
  g_autoptr (GError) error = NULL;
  Session *session;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to write selection on non-existing %s",
                 arg_session_handle);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Non-existing session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!is_clipboard_session (session))
    {
      g_warning ("Tried to complete write selection on invalid session");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  clipboard_proxy =
    clipboard_session_get_clipboard_proxy (CLIPBOARD_SESSION (session));
  if (!clipboard_proxy)
    {
      g_warning ("Tried to complete write selection with clipboard disabled");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Clipboard disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!org_gnome_mutter_clipboard_call_selection_write_done_sync (clipboard_proxy,
                                                                  arg_serial,
                                                                  arg_success,
                                                                  NULL,
                                                                  &error))
    {
      g_warning ("Failed to selection write: %s", error->message);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to selection write");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_impl_clipboard_complete_selection_write_done (object,
                                                    g_steal_pointer (&invocation));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_selection_read (XdpImplClipboard      *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList           *in_fd_list,
                       const char            *arg_session_handle,
                       const char            *arg_mime_type)
{
  OrgGnomeMutterClipboard *clipboard_proxy;
  g_autoptr (GUnixFDList) out_fd_list = NULL;
  g_autoptr (GError) error = NULL;
  Session *session;
  g_autoptr (GVariant) fd = NULL;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to read selection on non-existing %s",
                 arg_session_handle);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,
                                             "Non-existing session");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!is_clipboard_session (session))
    {
      g_warning ("Tried to read selection on invalid session");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Invalid session type");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  clipboard_proxy =
    clipboard_session_get_clipboard_proxy (CLIPBOARD_SESSION (session));
  if (!clipboard_proxy)
    {
      g_warning ("Tried to read selection with clipboard disabled");
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Clipboard disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  if (!org_gnome_mutter_clipboard_call_selection_read_sync (clipboard_proxy,
                                                            arg_mime_type,
                                                            NULL,
                                                            &fd,
                                                            &out_fd_list,
                                                            NULL,
                                                            &error))
    {
      g_warning ("Failed to selection read: %s", error->message);
      g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to selection read");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_impl_clipboard_complete_selection_read (object,
                                              g_steal_pointer (&invocation),
                                              out_fd_list,
                                              fd);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

gboolean
clipboard_init (GDBusConnection  *connection,
                GError          **error)
{
  impl_connection = connection;

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
                                         error))
    {
      g_clear_object (&impl);
      g_prefix_error (error,
                      "Failed to export clipboard portal implementation: ");
      return FALSE;
    }

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (impl)->name);

  return TRUE;
}

gboolean
clipboard_add_session (ClipboardSession *clipboard_session)
{
  OrgGnomeMutterClipboard *clipboard_proxy;
  GVariantBuilder options_builder;
  GVariant *options;
  g_autoptr (GError) error = NULL;

  clipboard_proxy = clipboard_session_get_clipboard_proxy (clipboard_session);
  g_return_val_if_fail (clipboard_proxy, FALSE);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  options = g_variant_builder_end (&options_builder);
  if (!org_gnome_mutter_clipboard_call_enable_sync (clipboard_proxy,
                                                    options,
                                                    NULL,
                                                    &error))
    {
      g_warning ("Failed to enable clipboard integration: %s", error->message);
      return FALSE;
    }

  g_signal_connect_data (clipboard_proxy,
                         "selection-owner-changed",
                         G_CALLBACK (on_selection_owner_changed),
                         clipboard_session, NULL,
                         G_CONNECT_DEFAULT);
  g_signal_connect_data (clipboard_proxy,
                         "selection-transfer",
                         G_CALLBACK (on_selection_transfer),
                         clipboard_session, NULL,
                         G_CONNECT_DEFAULT);
  return TRUE;
}

void
clipboard_remove_session (ClipboardSession *clipboard_session)
{
  OrgGnomeMutterClipboard *clipboard_proxy;
  g_autoptr (GError) error = NULL;

  clipboard_proxy =
    clipboard_session_get_clipboard_proxy (clipboard_session);
  g_return_if_fail (clipboard_proxy);

  if (!org_gnome_mutter_clipboard_call_disable_sync (clipboard_proxy,
                                                     NULL,
                                                     &error))
    {
      g_warning ("Failed to disable clipboard integration: %s", error->message);
      return;
    }

  g_signal_handlers_disconnect_by_func (clipboard_proxy,
                                        G_CALLBACK (on_selection_owner_changed),
                                        clipboard_session);
  g_signal_handlers_disconnect_by_func (clipboard_proxy,
                                        G_CALLBACK (on_selection_transfer),
                                        clipboard_session);
}
