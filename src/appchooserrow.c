/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"
#include "appchooserrow.h"

struct _AppChooserRow {
  AdwActionRow parent;

  GAppInfo *info;
  gboolean selected;

  GtkWidget *icon;
};

struct _AppChooserRowClass {
  AdwActionRowClass parent_class;
};

G_DEFINE_TYPE (AppChooserRow, app_chooser_row, ADW_TYPE_ACTION_ROW)

static void
app_chooser_row_init (AppChooserRow *row)
{
  gtk_widget_init_template (GTK_WIDGET (row));
}

static void
app_chooser_row_finalize (GObject *object)
{
  AppChooserRow *row = APP_CHOOSER_ROW (object);

  g_clear_object (&row->info);

  G_OBJECT_CLASS (app_chooser_row_parent_class)->finalize (object);
}

static void
app_chooser_row_class_init (AppChooserRowClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);

  object_class->finalize = app_chooser_row_finalize;

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/appchooserrow.ui");
  gtk_widget_class_bind_template_child (widget_class, AppChooserRow, icon);
}

AppChooserRow *
app_chooser_row_new (GAppInfo *info)
{
  AppChooserRow *row;
  GIcon *icon;

  row = g_object_new (app_chooser_row_get_type (), NULL);

  g_set_object (&row->info, info);

  icon = g_app_info_get_icon (info);
  if (!icon)
    icon = g_themed_icon_new ("application-x-executable");

  gtk_image_set_from_gicon (GTK_IMAGE (row->icon), icon);
  gtk_image_set_pixel_size (GTK_IMAGE (row->icon), 32);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), g_app_info_get_name (info));

  return row;
}

GAppInfo *
app_chooser_row_get_info (AppChooserRow *row)
{
  return row->info;
}

