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

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <adwaita.h>

#include "usbdialog.h"

struct _UsbDialog
{
  AdwWindow parent;

  AdwPreferencesGroup *device_list;
  GtkLabel *heading;
  GtkWidget *stack;
};

enum
{
  RESPONSE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (UsbDialog, usb_dialog, ADW_TYPE_WINDOW)

static void
load_icons (void)
{
  static gsize initialized = FALSE;

  if (g_once_init_enter (&initialized))
    {
      GtkIconTheme *default_theme;
      g_autofree char *iconspath = NULL;

      default_theme = gtk_icon_theme_get_for_display (gdk_display_get_default ());
      iconspath = g_strconcat ("/org/freedesktop/portal/desktop/gnome/icons/", NULL);

      gtk_icon_theme_add_resource_path (default_theme, iconspath);

      g_once_init_leave (&initialized, TRUE);
    }
}

static char *
parse_udev_string (const char *string)
{
  g_autoptr(GString) parsed = NULL;

  parsed = g_string_new (string);
  g_string_replace (parsed, "\\x20", " ", 0);

  return g_string_free_and_steal (g_steal_pointer (&parsed));
}

static inline void
add_serial_number (AdwActionRow *row,
                   const char   *serial_number)
{
  g_autofree char *parsed_serial = NULL;
  GtkWidget *popover;
  GtkWidget *button;
  GtkWidget *label;
  GtkWidget *box;

  g_assert (serial_number != NULL);

  box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_size_request (box, 150, -1);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  label = gtk_label_new (_("Serial Number"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (box), label);

  parsed_serial = parse_udev_string (serial_number);
  label = gtk_label_new (serial_number);
  gtk_label_set_xalign (GTK_LABEL (label), 1.0);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (box), label);

  popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (popover), box);

  button = gtk_menu_button_new ();
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (button, "flat");
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (button), "info-outline-symbolic");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), popover);

  adw_action_row_add_suffix (row, button);
}

static void
on_allow_usb_button_clicked_cb (GtkButton *button,
                                UsbDialog *self)
{
  g_assert (USB_IS_DIALOG (self));

  g_signal_emit (self, signals[RESPONSE], 0, GTK_RESPONSE_APPLY);
}

static void
on_deny_usb_button_clicked_cb (GtkButton *button,
                               UsbDialog *self)
{
  g_assert (USB_IS_DIALOG (self));

  g_signal_emit (self, signals[RESPONSE], 0, GTK_RESPONSE_CANCEL);
}

static gboolean
usb_dialog_close_request (GtkWindow *dialog)
{
  g_assert (USB_IS_DIALOG (dialog));

  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  g_signal_emit (dialog, signals[RESPONSE], 0, GTK_RESPONSE_CANCEL);

  return TRUE;
}

static void
usb_dialog_class_init (UsbDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  window_class->close_request = usb_dialog_close_request;

  signals[RESPONSE] = g_signal_new ("response",
                                    G_TYPE_FROM_CLASS (klass),
                                    G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL, NULL,
                                    NULL,
                                    G_TYPE_NONE, 1, G_TYPE_INT);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/usbdialog.ui");

  gtk_widget_class_bind_template_child (widget_class, UsbDialog, device_list);
  gtk_widget_class_bind_template_child (widget_class, UsbDialog, heading);
  gtk_widget_class_bind_template_child (widget_class, UsbDialog, stack);

  gtk_widget_class_bind_template_callback (widget_class, on_allow_usb_button_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, on_deny_usb_button_clicked_cb);
}

static void
usb_dialog_init (UsbDialog *self)
{
  load_icons ();

  gtk_widget_init_template (GTK_WIDGET (self));
}

UsbDialog *
usb_dialog_new (const char *app_id,
                GVariant   *devices)
{
  g_autofree char *heading = NULL;
  GVariantIter *device_properties;
  GVariantIter iter;
  UsbDialog *self;

  self = g_object_new (USB_TYPE_DIALOG, NULL);

  if (app_id && strcmp (app_id, "") != 0)
    {
      g_autoptr(GAppInfo) info = NULL;
      g_autofree char *id = NULL;
      const char *display_name = NULL;

      id = g_strconcat (app_id, ".desktop", NULL);
      info = G_APP_INFO (g_desktop_app_info_new (id));
      if (info)
        display_name = g_app_info_get_display_name (info);
      else
        display_name = app_id;
      heading = g_strdup_printf (_("%s wants to access the following USB devices"),
                                 display_name);
    }
  else
    {
      heading = g_strdup (_("An app wants to access the following USB devices"));
    }

  gtk_label_set_label (GTK_LABEL (self->heading), heading);

  g_variant_iter_init (&iter, devices);
  while (g_variant_iter_next (&iter,
                              "(&sa{sv}a{sv})",
                              NULL /* &id */,
                              &device_properties,
                              NULL /* &access_options */))
    {
      g_autoptr(GHashTable) props = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      g_autofree char *subtitle = NULL;
      g_autofree char *title = NULL;
      GtkWidget *row;
      GVariant *out_value;
      const char *serial_number;
      const char *vendor;
      const char *model;
      const char *key;

      while (g_variant_iter_next (device_properties, "{&sv}", &key, &out_value))
        {
          g_autoptr(GVariant) value = g_steal_pointer (&out_value);

          g_assert (key != NULL);
          g_assert (value != NULL);

          if (g_str_equal (key, "properties"))
            {
              GVariantIter props_iter;
              const char *key2;
              GVariant *out_value2;

              g_variant_iter_init (&props_iter, value);
              while (g_variant_iter_next (&props_iter, "{&sv}", &key2, &out_value2))
                {
                  g_autoptr(GVariant) value2 = g_steal_pointer (&out_value2);

                  g_hash_table_insert (props, g_strdup (key2), g_variant_dup_string (value2, NULL));
                }
            }
        }

      g_clear_pointer (&device_properties, g_variant_iter_free);

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

      title = model ? parse_udev_string (model) : g_strdup (_("Unknown device"));
      subtitle = vendor ? parse_udev_string (vendor) : g_strdup (_("Unknown vendor"));

      serial_number = g_hash_table_lookup (props, "ID_SERIAL_SHORT");
      if (serial_number && serial_number[0])
        add_serial_number (ADW_ACTION_ROW (row), serial_number);

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
      adw_preferences_group_add (self->device_list, row);
    }

  return self;
}
