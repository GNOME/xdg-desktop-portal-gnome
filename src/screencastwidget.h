/*
 * Copyright © 2018 Red Hat, Inc
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

#include <gtk/gtk.h>

#include "screencast.h"

typedef struct _ScreenCastWidget ScreenCastWidget;

#define SCREEN_CAST_TYPE_WIDGET (screen_cast_widget_get_type ())
G_DECLARE_FINAL_TYPE (ScreenCastWidget, screen_cast_widget,
                      SCREEN_CAST, WIDGET, GtkBox)

void screen_cast_widget_set_app_id (ScreenCastWidget *widget,
                                    const char       *app_id);

void screen_cast_widget_set_allow_multiple (ScreenCastWidget *widget,
                                            gboolean          multiple);

void screen_cast_widget_set_source_types (ScreenCastWidget     *screen_cast_widget,
                                          ScreenCastSourceTypes source_types);

GPtrArray *screen_cast_widget_get_selected_streams (ScreenCastWidget *self);

void screen_cast_widget_set_persist_mode (ScreenCastWidget *screen_cast_widget,
                                          ScreenCastPersistMode persist_mode);

ScreenCastPersistMode
screen_cast_widget_get_persist_mode (ScreenCastWidget *screen_cast_widget);
