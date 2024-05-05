/*
 * Copyright © 2024 GNOME Foundation Inc.
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
 *       Hubert Figuière <hub@figuiere.net>
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <adwaita.h>

#include "usbdialog.h"

struct _UsbDialog {
  AdwWindow parent;

  GtkWidget *stack;
  AdwPreferencesGroup *device_list;
};

enum {
  RESPONSE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (UsbDialog, usb_dialog, ADW_TYPE_WINDOW)

static void
usb_dialog_allow (UsbDialog *self)
{
  g_signal_emit (self, signals[RESPONSE], 0, GTK_RESPONSE_APPLY);
}

static void
usb_dialog_deny (UsbDialog *self)
{
  g_signal_emit (self, signals[RESPONSE], 0, GTK_RESPONSE_CANCEL);
}

static void
usb_dialog_finalize (GObject *object)
{
  G_OBJECT_CLASS (usb_dialog_parent_class)->finalize (object);
}

static void
usb_dialog_init (UsbDialog *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

static void
usb_dialog_class_init (UsbDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = usb_dialog_finalize;

  signals[RESPONSE] = g_signal_new ("response",
                                    G_TYPE_FROM_CLASS (klass),
                                    G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL, NULL,
                                    NULL,
                                    G_TYPE_NONE, 1, G_TYPE_INT);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/usbdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, UsbDialog, stack);
  gtk_widget_class_bind_template_child (widget_class, UsbDialog, device_list);

  gtk_widget_class_bind_template_callback (widget_class, usb_dialog_deny);
  gtk_widget_class_bind_template_callback (widget_class, usb_dialog_allow);
}

UsbDialog *
usb_dialog_new (GVariant *arg_devices)
{
  UsbDialog *self;
  GVariantIter *device_properties;
  GVariantIter iter;

  self = g_object_new (usb_dialog_get_type (), NULL);

  g_variant_iter_init (&iter, arg_devices);
  while (g_variant_iter_next (&iter,
                              "(&sa{sv}a{sv})",
                              NULL /* &id */,
                              &device_properties,
                              NULL /* &access_options */))
    {
      GtkWidget *row;
      const char *key;
      GVariant *value;
      const char *model;
      const char *vendor;
      const char *device_file = NULL;
      g_autofree char *title;
      g_autoptr(GHashTable) props = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

      while (g_variant_iter_next (device_properties, "{&sv}", &key, &value))
        {
	  if (g_str_equal (key, "device-file"))
	    device_file = g_variant_get_string (value, NULL);
	  else if (g_str_equal (key, "properties"))
	    {
	      GVariantIter props_iter;
	      const char *key2;
	      GVariant *value2;

	      g_variant_iter_init (&props_iter, value);
	      while (g_variant_iter_next (&props_iter, "{&sv}", &key2, &value2))
	        {
		  g_hash_table_insert (props, g_strdup (key2), g_strdup (g_variant_get_string (value2, NULL)));
		  g_variant_unref (value2);
		}
	    }
	  g_variant_unref (value);
	}

      row = adw_action_row_new ();
      vendor = g_hash_table_lookup (props, "ID_VENDOR_FROM_DATABASE");
      if (!vendor)
	vendor = g_hash_table_lookup (props, "ID_VENDOR_ENC");
      if (!vendor)
	vendor = g_hash_table_lookup (props, "ID_VENDOR_ID");

      model = g_hash_table_lookup (props, "ID_MODEL_FROM_DATABASE");
      if (!model)
	model = g_hash_table_lookup (props, "ID_MODEL_ENC");
      if (!model)
	model = g_hash_table_lookup (props, "ID_MODEL_ID");
      title = g_strdup_printf("%s %s", vendor, model);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), device_file);
      adw_preferences_group_add (self->device_list, row);
    }

  return self;
}
