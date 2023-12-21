/*
 * Copyright © 2023 Jan Zickermann
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

G_BEGIN_DECLS

#define SCREEN_CAST_TYPE_GEOMETRY_CONTAINER (screen_cast_geometry_container_get_type ())

G_DECLARE_FINAL_TYPE (ScreenCastGeometryContainer, screen_cast_geometry_container,
                      SCREEN_CAST, GEOMETRY_CONTAINER, GtkWidget)

typedef enum {
  SCREEN_CAST_GEOMETRY_CONTAINER_HINT_NONE,
  SCREEN_CAST_GEOMETRY_CONTAINER_HINT_BOTTOM
} ScreenCastGeometryContainerHint;

ScreenCastGeometryContainer *
screen_cast_geometry_container_new             (void);

void
screen_cast_geometry_container_add             (ScreenCastGeometryContainer *self,
                                                GtkWidget *widget,
                                                const graphene_rect_t *geometry);

void
screen_cast_geometry_container_add_with_hint   (ScreenCastGeometryContainer *self,
                                                GtkWidget *widget,
                                                const graphene_rect_t *geometry,
                                                ScreenCastGeometryContainerHint hint);

void
screen_cast_geometry_container_remove          (ScreenCastGeometryContainer *self,
                                                GtkWidget *widget);

void
screen_cast_geometry_container_remove_all      (ScreenCastGeometryContainer *self);

uint
screen_cast_geometry_container_get_child_count (ScreenCastGeometryContainer *self);

G_END_DECLS
