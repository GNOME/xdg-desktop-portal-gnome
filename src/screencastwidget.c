/*
 * Copyright © 2017-2018 Red Hat, Inc
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

#include "config.h"

#include <adwaita.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include "screencastwidget.h"
#include "displaystatetracker.h"
#include "shellintrospect.h"

enum
{
  HAS_SELECTION_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ScreenCastWidget
{
  GtkBox parent;

  GtkWidget *source_type_switcher;
  GtkWidget *source_type;
  GtkWidget *window_selection;
  GtkWidget *monitor_selection;

  GtkWidget *monitor_heading;
  GtkWidget *monitor_list;

  GtkWidget *window_heading;
  GtkWidget *window_list;

  GtkCheckButton *persist_check;
  ScreenCastPersistMode persist_mode;

  DisplayStateTracker *display_state_tracker;
  gulong monitors_changed_handler_id;

  ShellIntrospect *shell_introspect;
  gulong windows_changed_handler_id;

  guint selection_changed_timeout_id;
  gboolean allow_multiple;
  ScreenCastSourceType source_types;
};

static GQuark quark_monitor_widget_data;
static GQuark quark_window_widget_data;

G_DEFINE_TYPE (ScreenCastWidget, screen_cast_widget, GTK_TYPE_BOX)

/*
 * Auxiliary methods
 */

static GtkWidget *
create_window_widget (Window *window)
{
  GtkWidget *window_image;
  GtkWidget *check_image;
  GtkWidget *row;
  GIcon *icon = NULL;
  g_autoptr(GDesktopAppInfo) info = NULL;
  g_autofree char *escaped_name = NULL;

  info = g_desktop_app_info_new (window_get_app_id (window));
  if (info != NULL)
    icon = g_app_info_get_icon (G_APP_INFO (info));
  if (icon == NULL)
    icon = g_themed_icon_new ("application-x-executable");
  window_image = gtk_image_new_from_gicon (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (window_image), 32);

  check_image = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_hide (check_image);

  row = adw_action_row_new ();
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), window_image);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), check_image);

  escaped_name = g_markup_escape_text (window_get_title (window), -1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), escaped_name);

  g_object_set_qdata (G_OBJECT (row),
                      quark_window_widget_data,
                      window);
  g_object_set_data (G_OBJECT (row), "check", check_image);
  return row;
}

static GtkWidget *
create_monitor_widget (LogicalMonitor *logical_monitor)
{
  g_autoptr(GString) string = NULL;
  GtkWidget *check_image;
  GtkWidget *row;
  GList *l;

  check_image = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_hide (check_image);

  row = adw_action_row_new ();

  string = g_string_new (NULL);
  for (l = logical_monitor_get_monitors (logical_monitor); l; l = l->next)
    {
      Monitor *monitor = l->data;

      if (!l->prev)
        g_object_set_qdata (G_OBJECT (row),
                            quark_monitor_widget_data,
                            monitor);

      g_string_append (string, monitor_get_display_name (monitor));

      if (l->next)
        g_string_append (string, "\n");
    }

  adw_action_row_add_suffix (ADW_ACTION_ROW (row), check_image);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), string->str);
  g_object_set_data (G_OBJECT (row), "check", check_image);

  return row;
}


static GtkWidget *
create_virtual_widget (void)
{
  GtkWidget *check_image;
  GtkWidget *row;

  check_image = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_hide (check_image);

  row = adw_action_row_new ();
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), check_image);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Virtual monitor"));
  g_object_set_data (G_OBJECT (row), "check", check_image);

  return row;
}

static gboolean
should_skip_window (Window    *window,
                    GtkWindow *toplevel)
{
  g_autofree char *processed_app_id = NULL;

  if (g_strcmp0 (window_get_title (window),
                 gtk_window_get_title (toplevel)) != 0)
    return FALSE;

  processed_app_id = g_strdup (window_get_app_id (window));
  if (g_str_has_suffix (processed_app_id, ".desktop"))
    processed_app_id[strlen (processed_app_id) -
                     strlen (".desktop")] = '\0';

  if (g_strcmp0 (processed_app_id, g_get_prgname ()) != 0)
    return FALSE;

  return TRUE;
}

static void
update_windows_list (ScreenCastWidget *widget)
{
  GtkListBox *window_list = GTK_LIST_BOX (widget->window_list);
  GtkWidget *toplevel;
  GtkWidget *child;
  GPtrArray *windows;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (window_list))) != NULL)
    gtk_list_box_remove (window_list, child);

  toplevel = gtk_widget_get_ancestor (GTK_WIDGET (widget), GTK_TYPE_WINDOW);
  if (!toplevel)
    return;

  windows = shell_introspect_get_windows (widget->shell_introspect);
  for (size_t i = 0; windows && i < windows->len; i++)
    {
      Window *window = g_ptr_array_index (windows, i);
      GtkWidget *window_widget;

      if (should_skip_window (window, GTK_WINDOW (toplevel)))
        continue;

      window_widget = create_window_widget (window);
      gtk_list_box_append (window_list, window_widget);
    }
}

static void
update_monitors_list (ScreenCastWidget *widget)
{
  GtkListBox *monitor_list = GTK_LIST_BOX (widget->monitor_list);
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (monitor_list))) != NULL)
    gtk_list_box_remove (monitor_list, child);

  if (widget->source_types & SCREEN_CAST_SOURCE_TYPE_MONITOR)
    {
      GList *logical_monitors;
      GList *l;

      logical_monitors =
        display_state_tracker_get_logical_monitors (widget->display_state_tracker);
      for (l = logical_monitors; l; l = l->next)
        {
          LogicalMonitor *logical_monitor = l->data;
          GtkWidget *monitor_widget;

          monitor_widget = create_monitor_widget (logical_monitor);
          gtk_list_box_append (monitor_list, monitor_widget);
        }
    }

  if (widget->source_types & SCREEN_CAST_SOURCE_TYPE_VIRTUAL)
    gtk_list_box_append (monitor_list, create_virtual_widget ());
}

static gboolean
is_row_selected (GtkListBoxRow *row)
{
  GtkWidget *check_image = g_object_get_data (G_OBJECT (row), "check");
  return gtk_widget_get_visible (check_image);
}

static void
set_row_is_selected (GtkListBoxRow *row,
                     gboolean is_selected)
{
  GtkWidget *check_image = g_object_get_data (G_OBJECT (row), "check");
  gtk_widget_set_visible (check_image, is_selected);
}

static void
unselect_rows (GtkListBox *listbox)
{
  GtkWidget *child;

  for (child = gtk_widget_get_first_child (GTK_WIDGET (listbox));
       child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkListBoxRow *row;

      if (!GTK_IS_LIST_BOX_ROW (child))
        continue;

      row = GTK_LIST_BOX_ROW (child);

      set_row_is_selected (row, FALSE);
      gtk_list_box_unselect_row (listbox, row);
    }
}

static void
on_windows_changed (ShellIntrospect  *shell_introspect,
                    ScreenCastWidget *widget)
{
  update_windows_list (widget);
}

static void
connect_windows_changed_listener (ScreenCastWidget *widget)
{
  g_assert (!widget->windows_changed_handler_id);
  widget->windows_changed_handler_id =
    g_signal_connect (widget->shell_introspect,
                          "windows-changed",
                          G_CALLBACK (on_windows_changed),
                          widget);
  shell_introspect_ref_listeners (widget->shell_introspect);
}

static void
disconnect_windows_changed_listener (ScreenCastWidget *widget)
{
  g_assert (widget->windows_changed_handler_id);
  g_signal_handler_disconnect (widget->shell_introspect,
                               widget->windows_changed_handler_id);
  widget->windows_changed_handler_id = 0;
  shell_introspect_unref_listeners (widget->shell_introspect);
}

static void
on_stack_switch (GtkStack   *stack,
                 GParamSpec *pspec,
                 gpointer   *data)
{
  ScreenCastWidget *widget = (ScreenCastWidget *)data;
  GtkWidget *visible_child;

  unselect_rows (GTK_LIST_BOX (widget->monitor_list));
  unselect_rows (GTK_LIST_BOX (widget->window_list));

  visible_child = gtk_stack_get_visible_child (stack);
  if (visible_child == widget->window_selection)
    {
      if (!widget->windows_changed_handler_id)
        connect_windows_changed_listener (widget);
    }
  else
    {
      if (widget->windows_changed_handler_id)
        disconnect_windows_changed_listener (widget);
    }
}

static void
on_row_activated (GtkListBox    *box,
                  GtkListBoxRow *row,
                  gpointer      *data)
{
  if (!row)
    return;

  if (is_row_selected (row))
    {
      set_row_is_selected (row, FALSE);
      gtk_list_box_unselect_row (box, row);
    }
  else
    {
      set_row_is_selected (row, TRUE);
      gtk_list_box_select_row (box, row);
    }
}

static void
update_selected_rows (GtkListBox *listbox)
{
  GtkWidget *child;

  for (child = gtk_widget_get_first_child (GTK_WIDGET (listbox));
       child;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkListBoxRow *row;

      if (!GTK_IS_LIST_BOX_ROW (child))
        continue;

      row = GTK_LIST_BOX_ROW (child);
      set_row_is_selected (row, gtk_list_box_row_is_selected (row));
    }
}

static gboolean
emit_selection_change_in_idle_cb (gpointer data)
{
  ScreenCastWidget *widget = (ScreenCastWidget *)data;
  GList *selected_monitor_rows;
  GList *selected_window_rows;

  /* Update the selected rows */
  update_selected_rows (GTK_LIST_BOX (widget->monitor_list));
  update_selected_rows (GTK_LIST_BOX (widget->window_list));

  selected_monitor_rows = gtk_list_box_get_selected_rows (GTK_LIST_BOX (widget->monitor_list));
  selected_window_rows = gtk_list_box_get_selected_rows (GTK_LIST_BOX (widget->window_list));
  g_signal_emit (widget, signals[HAS_SELECTION_CHANGED], 0,
                 !!selected_monitor_rows || !!selected_window_rows);
  g_list_free (selected_monitor_rows);
  g_list_free (selected_window_rows);

  widget->selection_changed_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

static void
schedule_selection_change (ScreenCastWidget *widget)
{
  if (widget->selection_changed_timeout_id > 0)
    return;

  widget->selection_changed_timeout_id =
    g_idle_add (emit_selection_change_in_idle_cb, widget);
}

static void
on_selected_rows_changed (GtkListBox       *box,
                          ScreenCastWidget *widget)
{
  /* GtkListBox activates rows after selecting them, which prevents
   * us from emitting the HAS_SELECTION_CHANGED signal here */
  schedule_selection_change (widget);
}

static void
on_monitors_changed (DisplayStateTracker *display_state_tracker,
                     ScreenCastWidget    *widget)
{
  update_monitors_list (widget);
}

/*
 * GObject overrides
 */

static void
screen_cast_widget_finalize (GObject *object)
{
  ScreenCastWidget *widget = SCREEN_CAST_WIDGET (object);

  g_signal_handler_disconnect (widget->display_state_tracker,
                               widget->monitors_changed_handler_id);

  if (widget->windows_changed_handler_id)
    disconnect_windows_changed_listener (widget);

  if (widget->selection_changed_timeout_id > 0)
    {
      g_source_remove (widget->selection_changed_timeout_id);
      widget->selection_changed_timeout_id = 0;
    }

  G_OBJECT_CLASS (screen_cast_widget_parent_class)->finalize (object);
}

static void
screen_cast_widget_class_init (ScreenCastWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = screen_cast_widget_finalize;

  signals[HAS_SELECTION_CHANGED] = g_signal_new ("has-selection-changed",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL, NULL,
                                                 NULL,
                                                 G_TYPE_NONE, 1,
                                                 G_TYPE_BOOLEAN);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/screencastwidget.ui");
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, persist_check);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, source_type_switcher);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, source_type);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_selection);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_selection);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_heading);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_list);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_heading);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_list);

  quark_monitor_widget_data = g_quark_from_static_string ("-monitor-widget-connector-quark");
  quark_window_widget_data = g_quark_from_static_string ("-window-widget-connector-quark");
}

static void
screen_cast_widget_init (ScreenCastWidget *widget)
{
  gtk_widget_init_template (GTK_WIDGET (widget));

  screen_cast_widget_set_app_id (widget, NULL);
  screen_cast_widget_set_allow_multiple (widget, FALSE);

  g_signal_connect (widget->source_type, "notify::visible-child",
                    G_CALLBACK (on_stack_switch),
                    widget);
  g_signal_connect (widget->monitor_list, "row-activated",
                    G_CALLBACK (on_row_activated),
                    NULL);
  g_signal_connect (widget->window_list, "row-activated",
                    G_CALLBACK (on_row_activated),
                    NULL);
  g_signal_connect (widget->monitor_list, "selected-rows-changed",
                    G_CALLBACK (on_selected_rows_changed),
                    widget);
  g_signal_connect (widget->window_list, "selected-rows-changed",
                    G_CALLBACK (on_selected_rows_changed),
                    widget);

  widget->display_state_tracker = display_state_tracker_get ();
  widget->monitors_changed_handler_id =
    g_signal_connect (widget->display_state_tracker,
                      "monitors-changed",
                      G_CALLBACK (on_monitors_changed),
                      widget);
  widget->shell_introspect = shell_introspect_get ();

  update_monitors_list (widget);
  update_windows_list (widget);

  gtk_widget_show (widget->monitor_list);
  gtk_widget_show (widget->window_list);
}

GtkWidget *
screen_cast_widget_get_stack_switcher (ScreenCastWidget *widget)
{
  return widget->source_type_switcher;
}

void
screen_cast_widget_set_app_id (ScreenCastWidget *widget,
                               const char       *app_id)
{
  g_autofree char *monitor_heading = NULL;
  g_autofree char *window_heading = NULL;

  if (app_id && strcmp (app_id, "") != 0)
    {
      g_autofree char *id = NULL;
      g_autoptr(GAppInfo) info = NULL;
      const gchar *display_name = NULL;

      id = g_strconcat (app_id, ".desktop", NULL);
      info = G_APP_INFO (g_desktop_app_info_new (id));
      if (info)
        display_name = g_app_info_get_display_name (info);
      else
        display_name = app_id;
      monitor_heading = g_strdup_printf (_("Select monitor to share with %s"),
                                         display_name);
      window_heading = g_strdup_printf (_("Select window to share with %s"),
                                        display_name);
    }
  else
    {
      monitor_heading = g_strdup (_("Select monitor to share with the requesting application"));
      window_heading = g_strdup (_("Select window to share with the requesting application"));
    }

  gtk_label_set_label (GTK_LABEL (widget->monitor_heading), monitor_heading);
  gtk_label_set_label (GTK_LABEL (widget->window_heading), window_heading);
}

void
screen_cast_widget_set_allow_multiple (ScreenCastWidget *widget,
                                       gboolean          multiple)
{
  widget->allow_multiple = multiple;

  gtk_list_box_set_selection_mode (GTK_LIST_BOX (widget->monitor_list),
                                   multiple ? GTK_SELECTION_MULTIPLE
                                            : GTK_SELECTION_SINGLE);
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (widget->window_list),
                                   multiple ? GTK_SELECTION_MULTIPLE
                                            : GTK_SELECTION_SINGLE);
}

void
screen_cast_widget_set_source_types (ScreenCastWidget     *screen_cast_widget,
                                     ScreenCastSourceType  source_types)
{
  screen_cast_widget->source_types = source_types;

  if (source_types & SCREEN_CAST_SOURCE_TYPE_MONITOR)
    gtk_widget_show (screen_cast_widget->monitor_selection);

  if (source_types & SCREEN_CAST_SOURCE_TYPE_WINDOW)
    gtk_widget_show (screen_cast_widget->window_selection);

  if (source_types & SCREEN_CAST_SOURCE_TYPE_VIRTUAL)
    gtk_widget_show (screen_cast_widget->monitor_selection);

  if (__builtin_popcount (source_types) > 1)
    gtk_widget_show (screen_cast_widget->source_type_switcher);

  update_monitors_list (screen_cast_widget);
}

GPtrArray *
screen_cast_widget_get_selected_streams (ScreenCastWidget *self)
{
  ScreenCastStreamInfo *info;
  g_autoptr(GPtrArray) streams = NULL;
  g_autoptr(GList) selected_monitor_rows = NULL;
  g_autoptr(GList) selected_window_rows = NULL;
  uint32_t id = 0;
  GList *l;

  streams = g_ptr_array_new_with_free_func (g_free);

  selected_monitor_rows =
    gtk_list_box_get_selected_rows (GTK_LIST_BOX (self->monitor_list));
  selected_window_rows =
    gtk_list_box_get_selected_rows (GTK_LIST_BOX (self->window_list));

  if (!selected_monitor_rows && !selected_window_rows)
    return g_steal_pointer (&streams);

  for (l = selected_monitor_rows; l; l = l->next)
    {

      Monitor *monitor;

      monitor = g_object_get_qdata (G_OBJECT (l->data), quark_monitor_widget_data);

      if (monitor)
        {
          info = g_new0 (ScreenCastStreamInfo, 1);
          info->type = SCREEN_CAST_SOURCE_TYPE_MONITOR;
          info->data.monitor = monitor;
          info->id = id++;
          g_ptr_array_add (streams, info);
        }
      else
        {
          info = g_new0 (ScreenCastStreamInfo, 1);
          info->type = SCREEN_CAST_SOURCE_TYPE_VIRTUAL;
          info->id = id++;
          g_ptr_array_add (streams, info);
        }
    }

  for (l = selected_window_rows; l; l = l->next)
    {
      Window *window;

      window = g_object_get_qdata (G_OBJECT (l->data), quark_window_widget_data);

      info = g_new0 (ScreenCastStreamInfo, 1);
      info->type = SCREEN_CAST_SOURCE_TYPE_WINDOW;
      info->data.window = window;
      info->id = id++;
      g_ptr_array_add (streams, info);
    }

  return g_steal_pointer (&streams);
}

void
screen_cast_widget_set_persist_mode (ScreenCastWidget      *screen_cast_widget,
                                     ScreenCastPersistMode  persist_mode)
{
  screen_cast_widget->persist_mode = persist_mode;

  gtk_widget_set_visible (GTK_WIDGET (screen_cast_widget->persist_check),
                                      persist_mode != SCREEN_CAST_PERSIST_MODE_NONE);
}

ScreenCastPersistMode
screen_cast_widget_get_persist_mode (ScreenCastWidget *screen_cast_widget)
{
  if (!gtk_check_button_get_active (screen_cast_widget->persist_check))
    return SCREEN_CAST_PERSIST_MODE_NONE;

  return screen_cast_widget->persist_mode;
}
