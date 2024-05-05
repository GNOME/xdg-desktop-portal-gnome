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

#include "xdg-desktop-portal-dbus.h"

#include "externalwindow.h"
#include "request.h"
#include "utils.h"
#include "usb.h"
#include "usbdialog.h"

typedef struct {
  XdpImplUsb *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GtkWindow *dialog;
  ExternalWindow *external_parent;
  GVariant *results;

  guint response;
} UsbDialogHandle;

static void
usb_dialog_handle_free (gpointer data)
{
  UsbDialogHandle *handle = data;

  g_clear_object (&handle->external_parent);
  g_clear_object (&handle->request);
  g_clear_pointer (&handle->results, g_variant_unref);

  g_free (handle);
}

static void
usb_dialog_handle_close (UsbDialogHandle *handle)
{
  g_clear_pointer (&handle->dialog, gtk_window_destroy);
  usb_dialog_handle_free (handle);
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

  usb_dialog_handle_close (handle);
}

static void
handle_usb_dialog_response (UsbDialog *dialog,
			    gint response,
			    gpointer data)
{
  UsbDialogHandle *handle = data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      G_GNUC_FALLTHROUGH;

    case GTK_RESPONSE_DELETE_EVENT:
      handle->response = 2;
      break;

    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      break;

    case GTK_RESPONSE_APPLY:
      handle->response = 0;
      break;
    }

  send_response (handle);
}

static gboolean
handle_acquire_devices (XdpImplUsb            *object,
                        GDBusMethodInvocation *invocation,
                        const gchar           *arg_handle,
                        const gchar           *arg_parent_window,
                        const gchar           *arg_app_id,
                        GVariant              *arg_devices,
                        GVariant              *arg_options)
{
  g_autoptr(Request) request = NULL;
  const char *sender;
  GVariantBuilder devices_builder;
  GVariantBuilder results_builder;
  GVariantIter *device_properties;
  GVariantIter *access_options;
  GVariantIter iter;
  ExternalWindow *external_parent = NULL;
  GdkSurface *surface;
  GtkWidget *fake_parent;
  GtkWindow *dialog;
  UsbDialogHandle *handle;
  const char *id;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);

  g_message ("Handling AccessDevices with params: %s",
             g_variant_print (arg_options, TRUE));
  g_message ("    Devices: %s", g_variant_print (arg_devices, TRUE));

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  g_variant_builder_init (&devices_builder, G_VARIANT_TYPE ("a(sa{sv})"));

  g_variant_iter_init (&iter, arg_devices);
  while (g_variant_iter_next (&iter,
                              "(&sa{sv}a{sv})",
                              &id,
                              &device_properties,
                              &access_options))
    {
      g_autoptr(GVariantIter) owned_access_options = access_options;
      GVariantDict dict;
      const char *key;
      GVariant *value;

      g_variant_dict_init (&dict, NULL);
      while (g_variant_iter_next (access_options, "{&sv}", &key, &value))
        g_variant_dict_insert_value (&dict, key, value);

      g_variant_builder_add (&devices_builder, "(s@a{sv})",
                             id,
                             g_variant_dict_end (&dict));
    }

  g_variant_builder_add (&results_builder, "{sv}", "devices", g_variant_builder_end (&devices_builder));

  handle = g_new0 (UsbDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->results = g_variant_ref_sink (g_variant_builder_end (&results_builder));
  handle->response = 2;

  if (arg_parent_window)
    {
      external_parent = create_external_window_from_handle (arg_parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   arg_parent_window);
    }

  handle->external_parent = external_parent;

  fake_parent = g_object_new (GTK_TYPE_WINDOW, NULL);
  g_object_ref_sink (fake_parent);

  dialog = (GtkWindow *)usb_dialog_new (arg_devices);
  gtk_window_set_transient_for (dialog, GTK_WINDOW (fake_parent));
  handle->dialog = g_object_ref_sink (dialog);

  g_signal_connect (dialog, "response",
                    G_CALLBACK (handle_usb_dialog_response), handle);
  gtk_widget_realize (GTK_WIDGET (dialog));

  surface = gtk_native_get_surface (GTK_NATIVE (dialog));
  if (external_parent)
    external_window_set_parent_of (external_parent, surface);

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
