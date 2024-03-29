/*
 * Copyright © 2017 Red Hat, Inc
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

#pragma once

#include <adwaita.h>

#include "screencast.h"

#define SCREEN_CAST_TYPE_DIALOG (screen_cast_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ScreenCastDialog, screen_cast_dialog,
                      SCREEN_CAST, DIALOG, AdwWindow)

ScreenCastDialog * screen_cast_dialog_new (const char            *app_id,
                                           ScreenCastSelection   *select,
                                           ScreenCastPersistMode  persist_mode);
