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

#include "config.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include "remotedesktop.h"
#include "remotedesktopdialog.h"
#include "screencastwidget.h"

struct _RemoteDesktopDialog
{
  GtkWindow parent;

  GtkWidget *accept_button;
  GtkSwitch *allow_remote_interaction_switch;
  GtkWidget *screen_cast_widget;
  GtkHeaderBar *titlebar;

  RemoteDesktopDeviceType device_types;

  gboolean screen_cast_enable;
  ScreenCastSelection screen_cast;

  gboolean is_screen_cast_sources_selected;
};

struct _RemoteDesktopDialogClass
{
  GtkWindowClass *parent_class;
};

enum
{
  DONE,

  N_SIGNAL
};

static guint signals[N_SIGNAL];

G_DEFINE_TYPE (RemoteDesktopDialog, remote_desktop_dialog, GTK_TYPE_WINDOW)

static RemoteDesktopDeviceType
get_selected_device_types (RemoteDesktopDialog *dialog)
{
  if (gtk_switch_get_active (dialog->allow_remote_interaction_switch))
    return dialog->device_types;
  else
    return REMOTE_DESKTOP_DEVICE_TYPE_NONE;
}

static void
button_clicked (GtkWidget *button,
                RemoteDesktopDialog *dialog)
{
  RemoteDesktopDeviceType device_types = 0;
  g_autoptr(GPtrArray) streams = NULL;
  int response;

  gtk_widget_hide (GTK_WIDGET (dialog));

  if (button == dialog->accept_button)
    {
      ScreenCastWidget *screen_cast_widget =
        SCREEN_CAST_WIDGET (dialog->screen_cast_widget);

      response = GTK_RESPONSE_OK;
      device_types = get_selected_device_types (dialog);
      streams = screen_cast_widget_get_selected_streams (screen_cast_widget);
    }
  else
    {
      response = GTK_RESPONSE_CANCEL;
      device_types = 0;
      streams = NULL;
    }

  g_signal_emit (dialog, signals[DONE], 0, response, device_types, streams);
}

static void
update_button_sensitivity (RemoteDesktopDialog *dialog)
{
  gboolean can_accept = FALSE;

  if (dialog->is_screen_cast_sources_selected)
    can_accept = TRUE;

  if (gtk_switch_get_active (dialog->allow_remote_interaction_switch))
    can_accept = TRUE;

  if (can_accept)
    gtk_widget_set_sensitive (dialog->accept_button, TRUE);
  else
    gtk_widget_set_sensitive (dialog->accept_button, FALSE);
}

static void
on_allow_remote_interaction_switch_active_changed_cb (GtkCheckButton      *checkbutton,
                                                      GParamSpec          *pspec,
                                                      RemoteDesktopDialog *dialog)
{
  update_button_sensitivity (dialog);
}

static void
on_has_selection_changed (ScreenCastWidget *screen_cast_widget,
                          gboolean has_selection,
                          RemoteDesktopDialog *dialog)
{
  dialog->is_screen_cast_sources_selected = has_selection;
  update_button_sensitivity (dialog);
}

RemoteDesktopDialog *
remote_desktop_dialog_new (const char *app_id,
                           RemoteDesktopDeviceType device_types,
                           ScreenCastSelection *screen_cast_select)
{
  RemoteDesktopDialog *dialog;

  dialog = g_object_new (REMOTE_DESKTOP_TYPE_DIALOG, NULL);
  dialog->device_types = device_types;
  if (screen_cast_select)
    {
      dialog->screen_cast_enable = TRUE;
      dialog->screen_cast = *screen_cast_select;
    }
  else
    {
      dialog->screen_cast_enable = FALSE;
    }

  if (dialog->screen_cast_enable)
    {
      ScreenCastWidget *screen_cast_widget =
        SCREEN_CAST_WIDGET (dialog->screen_cast_widget);

      screen_cast_widget_set_allow_multiple (screen_cast_widget,
                                             screen_cast_select->multiple);
      screen_cast_widget_set_source_types (screen_cast_widget,
                                           screen_cast_select->source_types);

      g_signal_connect (screen_cast_widget, "has-selection-changed",
                        G_CALLBACK (on_has_selection_changed), dialog);
      gtk_widget_show (GTK_WIDGET (screen_cast_widget));

      if (__builtin_popcount (screen_cast_select->source_types) > 1 &&
          (screen_cast_select->source_types & SCREEN_CAST_SOURCE_TYPE_WINDOW))
        {
          gtk_header_bar_set_title_widget (dialog->titlebar,
                                           screen_cast_widget_get_stack_switcher (screen_cast_widget));
        }

    }

  return dialog;
}

static void
remote_desktop_dialog_init (RemoteDesktopDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));
}

static gboolean
remote_desktop_dialog_close_request (GtkWindow *dialog)
{
  gtk_widget_hide (GTK_WIDGET (dialog));

  g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_CANCEL, 0, NULL);

  return TRUE;
}

static void
remote_desktop_dialog_class_init (RemoteDesktopDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  window_class->close_request = remote_desktop_dialog_close_request;

  signals[DONE] = g_signal_new ("done",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 3,
                                G_TYPE_INT,
                                G_TYPE_INT,
                                G_TYPE_PTR_ARRAY);

  g_type_ensure (SCREEN_CAST_TYPE_WIDGET);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/remotedesktopdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, allow_remote_interaction_switch);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, screen_cast_widget);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, titlebar);
  gtk_widget_class_bind_template_callback (widget_class, button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_allow_remote_interaction_switch_active_changed_cb);
}
