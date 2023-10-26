/*
 * usb.c
 *
 * Copyright 2023 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE 1

#include "config.h"

#include "xdg-desktop-portal-dbus.h"

#include "utils.h"
#include "usb.h"

static gboolean
handle_acquire_devices (XdpImplUsb            *object,
                        GDBusMethodInvocation *invocation,
                        const gchar           *arg_handle,
                        const gchar           *arg_parent_window,
                        const gchar           *arg_app_id,
                        GVariant              *arg_devices,
                        GVariant              *arg_options)
{
  GVariantBuilder devices_builder;
  GVariantBuilder results_builder;
  GVariantIter *device_properties;
  GVariantIter *access_options;
  GVariantIter iter;
  const char *id;

  g_message ("Handling AccessDevices with params: %s",
             g_variant_print (arg_options, TRUE));
  g_message ("    Devices: %s", g_variant_print (arg_devices, TRUE));

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  /* TODO: implement either a dialog or something like that */

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

  xdp_impl_usb_complete_acquire_devices (object,
                                         invocation,
                                         XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS,
                                         g_variant_builder_end (&results_builder));

  return G_DBUS_METHOD_INVOCATION_HANDLED;
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
