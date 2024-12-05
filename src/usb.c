/*
 * usb.c
 *
 * Copyright 2023 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 * Copyright © 2024 GNOME Foundation Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *       Hubert Figuière <hub@figuiere.net>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <gxdp.h>

#include "xdg-desktop-portal-dbus.h"

#include "request.h"
#include "utils.h"
#include "usb.h"
#include "usbdialog.h"

typedef struct
{
  XdpImplUsb *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GtkWindow *dialog;
  GxdpExternalWindow *external_parent;
  GVariant *results;

  guint response;
} UsbDialogHandle;

static void
usb_dialog_handle_free (gpointer data)
{
  UsbDialogHandle *handle = data;

  g_clear_pointer (&handle->dialog, gtk_window_destroy);
  g_clear_object (&handle->external_parent);
  g_clear_object (&handle->request);
  g_clear_pointer (&handle->results, g_variant_unref);

  g_free (handle);
}

static void
send_response (UsbDialogHandle *handle)
{
  if (handle->request->exported)
    request_unexport (handle->request);

  xdp_impl_usb_complete_acquire_devices (handle->impl,
                                         handle->invocation,
                                         handle->response,
                                         handle->results);
}

static void
on_usb_dialog_response_cb (UsbDialog       *dialog,
                           guint            response,
                           UsbDialogHandle *handle)
{
  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      G_GNUC_FALLTHROUGH;

    case GTK_RESPONSE_DELETE_EVENT:
      handle->response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;
      break;

    case GTK_RESPONSE_CANCEL:
      handle->response = XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
      break;

    case GTK_RESPONSE_APPLY:
      handle->response = XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS;
      break;
    }

  send_response (handle);
  usb_dialog_handle_free (handle);
}

static gboolean
on_request_handle_close_cb (XdpImplRequest        *object,
                            GDBusMethodInvocation *invocation,
                            UsbDialogHandle       *handle)
{
  usb_dialog_handle_free (handle);

  return FALSE;
}

static gboolean
handle_acquire_devices (XdpImplUsb            *object,
                        GDBusMethodInvocation *invocation,
                        const char            *arg_handle,
                        const char            *arg_parent_window,
                        const char            *arg_app_id,
                        GVariant              *arg_devices,
                        GVariant              *arg_options)
{
  g_auto(GVariantBuilder) devices_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(sa{sv})"));
  g_auto(GVariantBuilder) results_builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
  g_autoptr(Request) request = NULL;
  GxdpExternalWindow *external_parent = NULL;
  UsbDialogHandle *handle;
  GVariantIter iter;
  GdkSurface *surface;
  GtkWidget *fake_parent;
  GtkWindow *dialog;
  GVariant *access_options;
  const char *sender;
  const char *id;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);

  g_variant_iter_init (&iter, arg_devices);
  while (g_variant_iter_next (&iter,
                              "(&sa{sv}@a{sv})",
                              &id,
                              NULL,
                              &access_options))
    {
      g_autoptr(GVariantDict) dict = g_variant_dict_new (access_options);

      g_variant_builder_add (&devices_builder, "(s@a{sv})",
                             id,
                             g_variant_dict_end (dict));

      g_clear_pointer (&access_options, g_variant_unref);
    }

  g_variant_builder_add (&results_builder, "{sv}", "devices", g_variant_builder_end (&devices_builder));

  handle = g_new0 (UsbDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
  handle->response = XDG_DESKTOP_PORTAL_RESPONSE_OTHER;

  if (arg_parent_window)
    {
      external_parent = gxdp_external_window_new_from_handle (arg_parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   arg_parent_window);
    }

  handle->external_parent = external_parent;

  fake_parent = g_object_new (GTK_TYPE_WINDOW, NULL);
  g_object_ref_sink (fake_parent);

  dialog = (GtkWindow *) usb_dialog_new (arg_app_id, arg_devices);
  gtk_window_set_transient_for (dialog, GTK_WINDOW (fake_parent));
  handle->dialog = g_object_ref_sink (dialog);

  g_signal_connect (request,
                    "handle-close",
                    G_CALLBACK (on_request_handle_close_cb),
                    handle);

  g_signal_connect (dialog,
                    "response",
                    G_CALLBACK (on_usb_dialog_response_cb),
                    handle);

  gtk_widget_realize (GTK_WIDGET (dialog));

  surface = gtk_native_get_surface (GTK_NATIVE (dialog));
  if (external_parent)
    gxdp_external_window_set_parent_of (external_parent, surface);

  gtk_window_present (dialog);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  return TRUE;
}

gboolean
usb_init (GDBusConnection  *bus,
          GError          **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_usb_skeleton_new ());
  g_signal_connect (helper, "handle-acquire-devices", G_CALLBACK (handle_acquire_devices), NULL);
  xdp_impl_usb_set_version (XDP_IMPL_USB (helper), 1);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
