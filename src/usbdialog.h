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

#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#define USB_TYPE_DIALOG (usb_dialog_get_type ())
G_DECLARE_FINAL_TYPE (UsbDialog, usb_dialog, USB, DIALOG, AdwWindow)

UsbDialog *usb_dialog_new (const char *app_id,
                           GVariant   *devices);
