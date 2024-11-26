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
#include "gtk/gtkcssprovider.h"
#include "screencastgeometrycontainer.h"
#include "screencast.h"
#include "shellintrospect.h"

enum
{
  HAS_SELECTION_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _ScreenCastWidget
{
  GtkBox                 parent;

  GtkWidget             *source_type_switcher;
  GtkWidget             *source_type;

  GtkWidget             *heading;

  GtkWidget             *window_selection_page;
  GtkWidget             *monitor_selection_page;

  GtkWidget             *monitor_container;
  GtkWidget             *window_list;

  GtkCheckButton        *persist_check;
  ScreenCastPersistMode  persist_mode;

  DisplayStateTracker   *display_state_tracker;
  ulong                  monitors_changed_handler_id;

  ShellIntrospect       *shell_introspect;
  GListModel            *filter_model;

  uint                   selection_changed_timeout_id;
  bool                   allow_multiple;
  ScreenCastSourceTypes  source_types;

  GtkCssProvider        *colors_provider;
};

static GQuark quark_monitor_widget_data;

G_DEFINE_TYPE (ScreenCastWidget, screen_cast_widget, GTK_TYPE_BOX)

static void schedule_selection_change (ScreenCastWidget *widget);

/*
 * Auxiliary methods
 */

static GtkWidget *
create_window_widget (ShellWindow *window)
{
  GtkWidget *window_image;
  GtkWidget *check_image;
  GtkWidget *row;
  g_autoptr(GIcon) icon = NULL;
  g_autoptr(GDesktopAppInfo) info = NULL;

  info = g_desktop_app_info_new (shell_window_get_app_id (window));
  if (info != NULL)
    icon = g_app_info_get_icon (G_APP_INFO (info));
  if (icon == NULL)
    icon = g_themed_icon_new ("application-x-executable");
  else
    icon = g_object_ref (icon);
  window_image = gtk_image_new_from_gicon (icon);
  gtk_image_set_pixel_size (GTK_IMAGE (window_image), 32);

  check_image = gtk_image_new_from_icon_name ("object-select-symbolic");
  gtk_widget_set_visible (check_image, FALSE);

  row = adw_action_row_new ();
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), window_image);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), check_image);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);

  g_object_bind_property (window, "title", row, "title", G_BINDING_SYNC_CREATE);

  g_object_set_data (G_OBJECT (row), "check", check_image);
  return row;
}

static void
foreach_widget_child (GtkWidget *parent, GFunc func, gpointer user_data)
{
  GtkWidget *child;

  for (child = gtk_widget_get_first_child (parent);
       child;
       child = gtk_widget_get_next_sibling (child))
    {
      func (child, user_data);
    }
}

static void
prepend_selected_monitor_button (GtkToggleButton *button, GList **selected_monitors)
{
  if (gtk_toggle_button_get_active (button))
    *selected_monitors = g_list_prepend (*selected_monitors, button);
}

static GList *
monitor_container_get_selected_buttons (GtkWidget *monitor_container)
{
  GList *selected_monitor_buttons = NULL;

  foreach_widget_child (monitor_container,
                        (GFunc) prepend_selected_monitor_button,
                        &selected_monitor_buttons);

  return selected_monitor_buttons;
}

static void
on_monitor_button_toggled_cb (GtkToggleButton *monitor_button, ScreenCastWidget *self)
{
  if (gtk_toggle_button_get_active (monitor_button) && !self->allow_multiple)
    {
      /* Single selection: unselect other buttons */
      g_autoptr(GList) selected_monitor_buttons = NULL;
      GList *l;

      selected_monitor_buttons = monitor_container_get_selected_buttons (self->monitor_container);

      for (l = selected_monitor_buttons; l; l = l->next)
        {
          if (l->data != monitor_button)
            gtk_toggle_button_set_active (l->data, FALSE);
        }
    }
  schedule_selection_change (self);
}

static GtkWidget *
create_monitor_button (Monitor *monitor, const MonitorIllustration *monitor_illustration)
{
  GtkWidget *button;
  GtkWidget *inscription;

  inscription = gtk_inscription_new (monitor_illustration->label);
  gtk_inscription_set_text_overflow (GTK_INSCRIPTION (inscription), GTK_INSCRIPTION_OVERFLOW_ELLIPSIZE_END);
  gtk_inscription_set_xalign (GTK_INSCRIPTION (inscription), 0.5);
  gtk_inscription_set_min_chars (GTK_INSCRIPTION (inscription), 5);
  gtk_widget_set_halign (inscription, GTK_ALIGN_FILL);
  gtk_widget_set_valign (inscription, GTK_ALIGN_FILL);
  gtk_widget_add_css_class (inscription, "text");

  button = gtk_toggle_button_new ();
  gtk_button_set_child ( GTK_BUTTON (button), inscription);
  gtk_widget_add_css_class (button, "monitor");
  if (monitor_illustration->primary)
    gtk_widget_add_css_class (button, "primary");
  gtk_accessible_update_relation (GTK_ACCESSIBLE (button),
                                  GTK_ACCESSIBLE_RELATION_LABELLED_BY, inscription, NULL,
                                  -1);
  g_object_set_qdata_full (G_OBJECT (button),
                           quark_monitor_widget_data,
                           monitor ? monitor_dup (monitor) : NULL,
                           (GDestroyNotify) monitor_free);

  return button;
}

static gboolean
should_show_window (ShellWindow *window,
                    ScreenCastWidget *self)
{
  GtkWidget *toplevel;
  g_autofree char *processed_app_id = NULL;

  toplevel = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);

  if (toplevel && g_strcmp0 (shell_window_get_title (window),
                             gtk_window_get_title (GTK_WINDOW (toplevel))) != 0)
    return TRUE;

  processed_app_id = g_strdup (shell_window_get_app_id (window));
  if (g_str_has_suffix (processed_app_id, ".desktop"))
    processed_app_id[strlen (processed_app_id) -
                     strlen (".desktop")] = '\0';

  if (g_strcmp0 (processed_app_id, g_get_prgname ()) != 0)
    return TRUE;

  return FALSE;
}

static void
auto_select_singular_monitor (ScreenCastWidget *self)
{
  /* Automatically select monitor if it is the only option */
  if (self->source_types.monitor || self->source_types.virtual_monitor)
    {
      AdwViewStackPage *selected_page;

      selected_page = adw_view_stack_pages_get_selected_page (
              ADW_VIEW_STACK_PAGES (adw_view_stack_get_pages (ADW_VIEW_STACK (self->source_type))));
      if (selected_page == ADW_VIEW_STACK_PAGE (self->monitor_selection_page))
        {
          GtkWidget *monitor_button;

          monitor_button = gtk_widget_get_first_child (self->monitor_container);
          if (monitor_button && gtk_widget_get_next_sibling (monitor_button) == NULL)
            gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (monitor_button), TRUE);
        }
    }
  schedule_selection_change (self);
}

static void
update_monitor_container (ScreenCastWidget *widget)
{
  GList *logical_monitors;
  GList *l;
  GtkWidget *monitor_container;

  if (!(widget->source_types.monitor || widget->source_types.virtual_monitor))
    return;

  monitor_container = widget->monitor_container;

  /* Clear monitor container */
  screen_cast_geometry_container_remove_all (SCREEN_CAST_GEOMETRY_CONTAINER (monitor_container));

  if (widget->source_types.monitor)
    {
      logical_monitors =
        display_state_tracker_get_logical_monitors (widget->display_state_tracker);

      for (l = logical_monitors; l; l = l->next)
        {
          Monitor *first_monitor;
          const MonitorIllustration *illustration;
          GtkWidget *monitor_button;
          LogicalMonitor *logical_monitor = l->data;

          illustration = logical_monitor_get_illustration (logical_monitor);

          /* Add illustrative monitor GtkToggleButton to container */
          first_monitor = logical_monitor_get_monitors (logical_monitor)->data;
          g_assert_nonnull (first_monitor);
          monitor_button = create_monitor_button (first_monitor, illustration);
          g_signal_connect (monitor_button, "toggled",
                            G_CALLBACK (on_monitor_button_toggled_cb),
                            widget);

          screen_cast_geometry_container_add (SCREEN_CAST_GEOMETRY_CONTAINER (monitor_container),
                                              monitor_button,
                                              &illustration->rect);
        }
  }

  if (widget->source_types.virtual_monitor)
    {
      GtkWidget *monitor_button;
      MonitorIllustration illustration =
        {
          .label = _("Virtual Monitor"),
          .primary = false,
          .rect = GRAPHENE_RECT_INIT (0, 0, 1280, 720),
        };

      monitor_button = create_monitor_button (NULL, &illustration);
      g_signal_connect (monitor_button, "toggled",
                        G_CALLBACK (on_monitor_button_toggled_cb),
                        widget);

      screen_cast_geometry_container_add_with_hint (SCREEN_CAST_GEOMETRY_CONTAINER (monitor_container),
                                                    monitor_button,
                                                    &illustration.rect,
                                                    SCREEN_CAST_GEOMETRY_CONTAINER_HINT_BOTTOM);
    }

  if (screen_cast_geometry_container_get_child_count (SCREEN_CAST_GEOMETRY_CONTAINER (monitor_container)) == 1)
    gtk_widget_add_css_class (monitor_container, "singular");
  else
    gtk_widget_remove_css_class (monitor_container, "singular");

  auto_select_singular_monitor (widget);
}

static gboolean
is_row_selected (GtkListBoxRow *row)
{
  GtkWidget *check_image = g_object_get_data (G_OBJECT (row), "check");
  return gtk_widget_get_visible (check_image);
}

static void
set_row_is_selected (ScreenCastWidget *widget,
                     GtkListBoxRow    *row,
                     gboolean          is_selected)
{
  GtkWidget *check_image = g_object_get_data (G_OBJECT (row), "check");
  gtk_widget_set_visible (check_image, is_selected);
  if (is_selected)
    {
      gtk_widget_add_css_class (GTK_WIDGET (row), "window-selected");
      gtk_accessible_update_state (GTK_ACCESSIBLE (row),
                                   GTK_ACCESSIBLE_STATE_SELECTED, TRUE,
                                   -1);
    }
  else
    {
      gtk_widget_remove_css_class (GTK_WIDGET (row), "window-selected");
      gtk_accessible_reset_state (GTK_ACCESSIBLE (row),
                                  GTK_ACCESSIBLE_STATE_SELECTED);
    }
  g_object_set_data (G_OBJECT (row), "window-selected",
                     GINT_TO_POINTER (is_selected));
  schedule_selection_change (widget);
}

static void
unselect_row (GtkListBoxRow    *row,
              ScreenCastWidget *widget)
{
  set_row_is_selected (widget, row, FALSE);
}

static void
untoggle_button (GtkToggleButton *button, GtkFixed *monitor_container)
{
  gtk_toggle_button_set_active (button, FALSE);
}

static void
reset_selection (ScreenCastWidget *self)
{

  foreach_widget_child (self->monitor_container, (GFunc) untoggle_button, self->monitor_container);
  foreach_widget_child (self->window_list, (GFunc) unselect_row, self);

  auto_select_singular_monitor (self);
}

static void
on_stack_switch (ScreenCastWidget *self,
                 GParamSpec *pspec,
                 AdwViewStack   *stack)
{
  reset_selection (self);
}

static void
on_row_activated (ScreenCastWidget *self,
                  GtkListBoxRow    *row,
                  GtkListBox       *box)
{
  if (!row)
    return;

  if (is_row_selected (row))
    set_row_is_selected (self, row, FALSE);
  else
    {
      if (!self->allow_multiple)
        foreach_widget_child (GTK_WIDGET (box), (GFunc) unselect_row, self);

      set_row_is_selected (self, row, TRUE);
    }
}

GList *
get_selected_windows (ScreenCastWidget *widget)
{
  unsigned int i;
  GList *windows = NULL;

  for (i = 0; i < g_list_model_get_n_items (widget->filter_model); i++)
    {
      GtkListBoxRow *row =
        gtk_list_box_get_row_at_index (GTK_LIST_BOX (widget->window_list), i);

      if (g_object_get_data (G_OBJECT (row), "window-selected"))
        {
          windows = g_list_append (windows,
                                   g_list_model_get_item (widget->filter_model, i));
        }
    }

  return windows;
}

static gboolean
emit_selection_change_in_idle_cb (ScreenCastWidget *widget)
{
  g_autoptr(GList) selected_monitor_buttons = NULL;
  g_autolist(ShellWindow) selected_windows = NULL;

  selected_monitor_buttons = monitor_container_get_selected_buttons (widget->monitor_container);
  selected_windows = get_selected_windows (widget);
  g_signal_emit (widget, signals[HAS_SELECTION_CHANGED], 0,
                 !!selected_monitor_buttons || !!selected_windows);

  widget->selection_changed_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

static void
schedule_selection_change (ScreenCastWidget *widget)
{
  if (widget->selection_changed_timeout_id > 0)
    return;

  widget->selection_changed_timeout_id =
    g_idle_add (G_SOURCE_FUNC (emit_selection_change_in_idle_cb), widget);
}

static void
monitors_changed_cb (ScreenCastWidget *self, gconstpointer user_data)
{
  update_monitor_container (self);
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

  shell_introspect_unref_listeners (widget->shell_introspect);

  if (widget->selection_changed_timeout_id > 0)
    {
      g_source_remove (widget->selection_changed_timeout_id);
      widget->selection_changed_timeout_id = 0;
    }

  g_clear_object (&widget->filter_model);

  g_clear_object (&widget->colors_provider);

  G_OBJECT_CLASS (screen_cast_widget_parent_class)->finalize (object);
}
static void
screen_cast_widget_map (GtkWidget *widget)
{
  static GtkCssProvider *provider = NULL;

  GTK_WIDGET_CLASS (screen_cast_widget_parent_class)->map (widget);

  if (provider == NULL)
    {
      provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_resource (provider,
                                           "/org/freedesktop/portal/desktop/gnome/screencastwidget.css");
      gtk_style_context_add_provider_for_display (gtk_widget_get_display (widget),
                                                  GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

static void
screen_cast_widget_root (GtkWidget *widget)
{
  ScreenCastWidget *self = SCREEN_CAST_WIDGET (widget);
  GTK_WIDGET_CLASS (screen_cast_widget_parent_class)->root (widget);

  update_monitor_container (self);
  reset_selection (self);
}

static void
screen_cast_widget_class_init (ScreenCastWidgetClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = screen_cast_widget_finalize;
  widget_class->map = screen_cast_widget_map;
  widget_class->root = screen_cast_widget_root;

  signals[HAS_SELECTION_CHANGED] = g_signal_new ("has-selection-changed",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL, NULL,
                                                 NULL,
                                                 G_TYPE_NONE, 1,
                                                 G_TYPE_BOOLEAN);

  g_type_ensure (SCREEN_CAST_TYPE_GEOMETRY_CONTAINER);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/screencastwidget.ui");
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, persist_check);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, source_type_switcher);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, source_type);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_selection_page);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_selection_page);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, heading);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, monitor_container);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastWidget, window_list);

  quark_monitor_widget_data = g_quark_from_static_string ("-monitor-widget-connector-quark");
}

static void
adjust_to_color_scheme (ScreenCastWidget *self, gpointer user_data)
{
  char *resource_path;

  resource_path = adw_style_manager_get_dark (adw_style_manager_get_default ())
    ? "/org/freedesktop/portal/desktop/gnome/screencastwidget-dark.css"
    : "/org/freedesktop/portal/desktop/gnome/screencastwidget-light.css";

  gtk_css_provider_load_from_resource (self->colors_provider, resource_path);
}

static void
screen_cast_widget_init (ScreenCastWidget *widget)
{
  GtkExpression *expression;
  g_autoptr(GtkFilter) filter = NULL;

  widget->selection_changed_timeout_id = 0;

  gtk_widget_init_template (GTK_WIDGET (widget));

  screen_cast_widget_set_app_id (widget, NULL);
  screen_cast_widget_set_allow_multiple (widget, FALSE);
  screen_cast_widget_set_source_types (widget, screen_cast_source_types_from_flags (0));
  screen_cast_widget_set_persist_mode (widget, SCREEN_CAST_PERSIST_MODE_NONE);

  widget->display_state_tracker = display_state_tracker_get ();
  widget->monitors_changed_handler_id =
    g_signal_connect_swapped (widget->display_state_tracker,
                              "monitors-changed",
                              G_CALLBACK (monitors_changed_cb),
                              widget);
  widget->shell_introspect = shell_introspect_get ();

  widget->colors_provider = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (gtk_widget_get_display (GTK_WIDGET (widget)),
                                              GTK_STYLE_PROVIDER (widget->colors_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  adjust_to_color_scheme (widget, adw_style_manager_get_default ());

  shell_introspect_ref_listeners (widget->shell_introspect);

  expression = gtk_cclosure_expression_new (G_TYPE_BOOLEAN, NULL, 0,
                                            NULL,
                                            G_CALLBACK (should_show_window),
                                            widget, NULL);

  filter = GTK_FILTER (gtk_bool_filter_new (expression));
  widget->filter_model = G_LIST_MODEL (gtk_filter_list_model_new (NULL, NULL));
  gtk_filter_list_model_set_model (GTK_FILTER_LIST_MODEL (widget->filter_model),
                                   shell_introspect_get_windows (widget->shell_introspect));
  gtk_filter_list_model_set_filter (GTK_FILTER_LIST_MODEL (widget->filter_model), filter);

  gtk_list_box_bind_model (GTK_LIST_BOX (widget->window_list),
                           widget->filter_model,
                           (GtkListBoxCreateWidgetFunc) create_window_widget,
                           NULL, NULL);

  g_signal_connect_swapped (widget->filter_model,
                            "items-changed",
                            G_CALLBACK (schedule_selection_change),
                            widget);
  g_object_connect (widget->window_list,
                    "swapped-signal::row-activated", G_CALLBACK (on_row_activated), widget,
                    NULL);

  g_signal_connect_swapped (widget->source_type,
                            "notify::visible-child",
                            G_CALLBACK (on_stack_switch),
                            widget);

  g_signal_connect_object (adw_style_manager_get_default (),
                           "notify::dark",
                           G_CALLBACK (adjust_to_color_scheme),
                           widget,
                           G_CONNECT_SWAPPED);
}

void
screen_cast_widget_set_app_id (ScreenCastWidget *widget,
                               const char       *app_id)
{
  g_autofree char *heading = NULL;

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
      heading = g_strdup_printf (_("%s wants to share your screen. Choose what you'd like to share."),
                                 display_name);
    }
  else
    {
      heading = g_strdup (_("An app wants to share your screen. Choose what you'd like to share."));
    }

  gtk_label_set_label (GTK_LABEL (widget->heading), heading);
}

void
screen_cast_widget_set_allow_multiple (ScreenCastWidget *widget,
                                       gboolean          multiple)
{
  widget->allow_multiple = multiple;

  gtk_list_box_set_selection_mode (GTK_LIST_BOX (widget->window_list),
                                   multiple ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);

  if (!multiple)
    reset_selection (widget);
}

void
screen_cast_widget_set_source_types (ScreenCastWidget     *screen_cast_widget,
                                     ScreenCastSourceTypes source_types)
{
  gboolean monitors_visible, windows_visible;

  screen_cast_widget->source_types = source_types;

  monitors_visible = source_types.monitor || source_types.virtual_monitor;
  windows_visible = source_types.window;

  adw_view_stack_page_set_visible (ADW_VIEW_STACK_PAGE (screen_cast_widget->monitor_selection_page),
                                   monitors_visible);
  adw_view_stack_page_set_visible (ADW_VIEW_STACK_PAGE (screen_cast_widget->window_selection_page),
                                   windows_visible);
  gtk_widget_set_visible (screen_cast_widget->source_type_switcher, monitors_visible && windows_visible);

  /* Prefer showing monitor page */
  adw_view_stack_pages_set_selected_page (
    ADW_VIEW_STACK_PAGES (adw_view_stack_get_pages (ADW_VIEW_STACK (screen_cast_widget->source_type))),
    ADW_VIEW_STACK_PAGE (monitors_visible
                         ? screen_cast_widget->monitor_selection_page
                         : screen_cast_widget->window_selection_page));

  if (monitors_visible)
    update_monitor_container (screen_cast_widget);
}

GPtrArray *
screen_cast_widget_get_selected_streams (ScreenCastWidget *self)
{
  ScreenCastStreamInfo *info;
  g_autoptr(GPtrArray) streams = NULL;
  g_autoptr(GList) selected_monitor_buttons = NULL;
  g_autolist(ShellWindow) selected_windows = NULL;
  uint32_t id = 0;
  GList *l;

  streams =
    g_ptr_array_new_with_free_func ((GDestroyNotify) screen_cast_stream_info_free);

  selected_monitor_buttons = monitor_container_get_selected_buttons (self->monitor_container);

  selected_windows = get_selected_windows (self);

  if (!selected_monitor_buttons && !selected_windows)
    return g_steal_pointer (&streams);

  for (l = selected_monitor_buttons; l; l = l->next)
    {

      Monitor *monitor;

      monitor = g_object_get_qdata (G_OBJECT (l->data), quark_monitor_widget_data);

      if (monitor)
        {
          info = g_new0 (ScreenCastStreamInfo, 1);
          info->type = SCREEN_CAST_SOURCE_TYPE_MONITOR;
          info->data.monitor = monitor_dup (monitor);
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

  for (l = selected_windows; l; l = l->next)
    {
      ShellWindow *window = l->data;

      info = g_new0 (ScreenCastStreamInfo, 1);
      info->type = SCREEN_CAST_SOURCE_TYPE_WINDOW;
      info->data.window = g_object_ref (window);
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
                          persist_mode == SCREEN_CAST_PERSIST_MODE_PERSISTENT);
}

ScreenCastPersistMode
screen_cast_widget_get_persist_mode (ScreenCastWidget *screen_cast_widget)
{
  if (!gtk_check_button_get_active (screen_cast_widget->persist_check))
    return SCREEN_CAST_PERSIST_MODE_NONE;

  return screen_cast_widget->persist_mode;
}
