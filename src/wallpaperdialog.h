/*
 * Copyright © 2019 Red Hat, Inc
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
 *       Felipe Borges <feborges@redhat.com>
 */

#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#define WALLPAPER_TYPE_DIALOG (wallpaper_dialog_get_type ())
G_DECLARE_FINAL_TYPE (WallpaperDialog, wallpaper_dialog, WALLPAPER, DIALOG, AdwWindow)

WallpaperDialog * wallpaper_dialog_new (const char *picture_uri,
                                        const char *app_id);

const gchar     * wallpaper_dialog_get_uri (WallpaperDialog *dialog);
