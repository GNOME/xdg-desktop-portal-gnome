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

#include "screencastgeometrycontainer.h"
#include "gtk/gtkcustomlayout.h"
#include "gtk/gtklayoutmanager.h"
#include <math.h>

/**
 * ScreenCastGeometryContainer:
 *
 * `ScreenCastGeometryContainer` is a container which arranges its child widgets
 * according to a fixed geometry.
 *
 * Each child widget is confined to a rectangle. The relative proportions of
 * rectangles remains constant. The container avoids shrinking its children
 * beyond their minimum size.
 */

struct _ScreenCastGeometryContainer {
  GtkWidget parent_instance;

  GHashTable *child_geometries;
  graphene_rect_t bounds_cache;
};

typedef struct _ScreenCastGeometryContainer ScreenCastGeometryContainer;

G_DEFINE_FINAL_TYPE (ScreenCastGeometryContainer, screen_cast_geometry_container, GTK_TYPE_WIDGET)

static GtkSizeRequestMode
layout_request_mode (GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

/**
 * Find the minimum container scale.
 * All children need to fit in the container with at least their minimum size.
 */
static void
measure_scale (ScreenCastGeometryContainer *self, double *minimum_scale, double *natural_scale)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, self->child_geometries);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GtkWidget *child = key;
      graphene_rect_t *child_geometry = value;
      int child_min = 0, child_nat = 0;

      if (!gtk_widget_should_layout (child))
        continue;

      gtk_widget_measure (child, GTK_ORIENTATION_HORIZONTAL, -1, &child_min, &child_nat, NULL, NULL);
      *minimum_scale = MAX (*minimum_scale, (double) child_min / child_geometry->size.width);
      *natural_scale = MAX (*natural_scale, (double) child_nat / child_geometry->size.width);

      gtk_widget_measure (child, GTK_ORIENTATION_VERTICAL, -1, &child_min, &child_nat, NULL, NULL);
      *minimum_scale = MAX (*minimum_scale, (double) child_min / child_geometry->size.height);
      *natural_scale = MAX (*natural_scale, (double) child_nat / child_geometry->size.height);
    }
}

static void
layout_measure (GtkWidget      *widget,
                GtkOrientation  orientation,
                int             for_size,
                int            *minimum,
                int            *natural,
                int            *minimum_baseline,
                int            *natural_baseline)
{
  ScreenCastGeometryContainer *self;

  self = SCREEN_CAST_GEOMETRY_CONTAINER (widget);

  gboolean horizontal = orientation == GTK_ORIENTATION_HORIZONTAL;
  graphene_size_t *size_wh = &self->bounds_cache.size;
  float size = horizontal ? size_wh->width : size_wh->height;
  float size_opp = horizontal ? size_wh->height : size_wh->width;

  if (!(size > 0.0f && size_opp > 0.0f))
    return;

  double for_scale = for_size >= 0 && size_opp > 0.0f ? (double) for_size / size_opp : 0.0f;
  double minimum_scale = for_scale;
  double natural_scale = for_scale;

  measure_scale (self, &minimum_scale, &natural_scale);

  *minimum = ceil (minimum_scale * size);
  *natural = ceil (natural_scale * size);
}

static void
layout_allocate (GtkWidget    *widget,
                 int           width,
                 int           height,
                 int           baseline)
{
  ScreenCastGeometryContainer *self;
  graphene_rect_t *bounds;
  GHashTableIter iter;
  gpointer key, value;

  self = SCREEN_CAST_GEOMETRY_CONTAINER (widget);

  if (!g_hash_table_size (self->child_geometries))
    return;

  bounds = &self->bounds_cache;

  /* Scale and translate child widgets to fit inside container */
  double scale = MIN ((double) width / bounds->size.width, (double) height / bounds->size.height);
  double translate_x = ((double) width - scale * bounds->size.width) / 2.0;
  double translate_y = ((double) height - scale * bounds->size.height) / 2.0;

  g_hash_table_iter_init (&iter, self->child_geometries);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      GtkWidget *child = key;
      graphene_rect_t *r = value;
      double x1, x2, y1, y2;

      x1 = scale * (r->origin.x-bounds->origin.x) + translate_x;
      y1 = scale * (r->origin.y-bounds->origin.y) + translate_y;
      x2 = x1 + scale * r->size.width;
      y2 = y1 + scale * r->size.height;

      if (gtk_widget_should_layout (child))
        gtk_widget_size_allocate (child,
                                  &(GtkAllocation)
                                  {
                                  .x = ceil (x1),
                                  .y = ceil (y1),
                                  .width = ceil (x2) - ceil (x1),
                                  .height = ceil (y2) - ceil (y1)
                                  },
                                  -1);
    }
}

static void
update_geometry_bounds (ScreenCastGeometryContainer *self)
{

  if (g_hash_table_size (self->child_geometries))
    {
      GHashTableIter iter;
      float min_x, min_y, max_x, max_y;
      gpointer value;

      /* Find bounding box which includes all children */
      min_x = min_y = INFINITY;
      max_x = max_y = -INFINITY;
      g_hash_table_iter_init (&iter, self->child_geometries);
      while (g_hash_table_iter_next (&iter, NULL, &value))
        {
          graphene_rect_t *r = value;

          min_x = MIN (min_x, r->origin.x);
          max_x = MAX (max_x, r->origin.x + r->size.width);
          min_y = MIN (min_y, r->origin.y);
          max_y = MAX (max_y, r->origin.y + r->size.height);
        }

      graphene_rect_init (&self->bounds_cache, min_x, min_y, max_x - min_x, max_y - min_y);
    }
  else
    {
      self->bounds_cache = GRAPHENE_RECT_INIT_ZERO;
    }
}

void
screen_cast_geometry_container_add (ScreenCastGeometryContainer *self,
                                    GtkWidget *widget,
                                    const graphene_rect_t *geometry)
{
  screen_cast_geometry_container_add_with_hint (self,
                                                widget,
                                                geometry,
                                                SCREEN_CAST_GEOMETRY_CONTAINER_HINT_NONE);
}

static void
layout_changed (ScreenCastGeometryContainer *self)
{
  GtkLayoutManager *layout_manager;

  layout_manager = gtk_widget_get_layout_manager (GTK_WIDGET (self));
  gtk_layout_manager_layout_changed (layout_manager);
}

void
screen_cast_geometry_container_add_with_hint (ScreenCastGeometryContainer *self,
                                              GtkWidget *widget,
                                              const graphene_rect_t *geometry,
                                              ScreenCastGeometryContainerHint hint)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (widget != NULL);
  g_return_if_fail (geometry != NULL);
  g_assert_cmpfloat (geometry->size.width, >, 0.0f);
  g_assert_cmpfloat (geometry->size.height, >, 0.0f);

  graphene_rect_t *r = g_hash_table_lookup (self->child_geometries, widget);
  if (r == NULL)
  {
    r = g_new (graphene_rect_t, 1);
    *r = *geometry;
    g_hash_table_insert (self->child_geometries, widget, r);
  }

  switch (hint)
    {
    case SCREEN_CAST_GEOMETRY_CONTAINER_HINT_NONE:
      break;
    case SCREEN_CAST_GEOMETRY_CONTAINER_HINT_BOTTOM:
      {
        r->origin.x = (self->bounds_cache.size.width - r->size.width) / 2.0f;
        r->origin.y = self->bounds_cache.origin.y + self->bounds_cache.size.height;
        break;
      }
    }
  update_geometry_bounds (self);
  gtk_widget_set_parent (widget, GTK_WIDGET (self));

  layout_changed (self);
}

uint
screen_cast_geometry_container_get_child_count (ScreenCastGeometryContainer *self)
{
  return g_hash_table_size (self->child_geometries);
}

void
screen_cast_geometry_container_remove (ScreenCastGeometryContainer *self,
                                       GtkWidget *widget)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (widget != NULL);

  if (g_hash_table_remove (self->child_geometries, widget))
    {
      update_geometry_bounds (self);
      layout_changed (self);
    }
}

void
screen_cast_geometry_container_remove_all (ScreenCastGeometryContainer *self)
{
  g_return_if_fail (self != NULL);

  g_hash_table_remove_all (self->child_geometries);

  update_geometry_bounds (self);
  layout_changed (self);
}

static void
screen_cast_geometry_container_dispose (GObject *object)
{
  ScreenCastGeometryContainer *self;

  self = SCREEN_CAST_GEOMETRY_CONTAINER (object);

  screen_cast_geometry_container_remove_all (self);
  gtk_widget_set_layout_manager (GTK_WIDGET (self), NULL);

  G_OBJECT_CLASS (screen_cast_geometry_container_parent_class)->dispose (object);
}

static void
screen_cast_geometry_container_finalize (GObject *object)
{
  ScreenCastGeometryContainer *self;

  self = SCREEN_CAST_GEOMETRY_CONTAINER (object);
  g_clear_pointer (&self->child_geometries, g_hash_table_unref);

  G_OBJECT_CLASS (screen_cast_geometry_container_parent_class)->finalize (object);
}

static void
screen_cast_geometry_container_class_init (ScreenCastGeometryContainerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->dispose = screen_cast_geometry_container_dispose;
  object_class->finalize = screen_cast_geometry_container_finalize;

  gtk_widget_class_set_css_name (GTK_WIDGET_CLASS (class), "geometry-container");
}

static void
screen_cast_geometry_container_init (ScreenCastGeometryContainer *self)
{
  GtkLayoutManager *layout_manager;

  layout_manager = gtk_custom_layout_new (layout_request_mode, layout_measure, layout_allocate);
  gtk_widget_set_layout_manager (GTK_WIDGET (self), layout_manager);

  self->child_geometries = g_hash_table_new_full (g_direct_hash,
                                                  g_direct_equal,
                                                  (GDestroyNotify) gtk_widget_unparent,
                                                  (GDestroyNotify) g_free);
  update_geometry_bounds (self);
}

ScreenCastGeometryContainer *
screen_cast_geometry_container_new (void)
{
  return g_object_new (SCREEN_CAST_TYPE_GEOMETRY_CONTAINER, NULL);
}
