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
  AdwWindow parent;

  GtkWidget *accept_button;
  AdwSwitchRow *allow_remote_interaction_switch;
  AdwSwitchRow *allow_remote_clipboard_switch;
  GtkCheckButton *persist_check;
  GtkWidget *screen_cast_widget;
  AdwHeaderBar *titlebar;

  RemoteDesktopDeviceType device_types;

  gboolean screen_cast_enable;
  ScreenCastSelection screen_cast;

  gboolean is_screen_cast_sources_selected;
};

struct _RemoteDesktopDialogClass
{
  AdwWindowClass *parent_class;
};

enum
{
  DONE,

  N_SIGNAL
};

static guint signals[N_SIGNAL];

G_DEFINE_TYPE (RemoteDesktopDialog, remote_desktop_dialog, ADW_TYPE_WINDOW)

static RemoteDesktopDeviceType
get_selected_device_types (RemoteDesktopDialog *dialog)
{
  if (adw_switch_row_get_active (dialog->allow_remote_interaction_switch))
    return dialog->device_types;
  else
    return REMOTE_DESKTOP_DEVICE_TYPE_NONE;
}

static gboolean
get_persist (RemoteDesktopDialog *dialog)
{
  return gtk_check_button_get_active (dialog->persist_check);
}

static void
button_clicked (GtkWidget *button,
                RemoteDesktopDialog *dialog)
{
  RemoteDesktopDeviceType device_types = 0;
  gboolean enable_clipboard;
  g_autoptr(GPtrArray) streams = NULL;
  gboolean persist = FALSE;
  int response;

  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  if (button == dialog->accept_button)
    {
      ScreenCastWidget *screen_cast_widget =
        SCREEN_CAST_WIDGET (dialog->screen_cast_widget);

      response = GTK_RESPONSE_OK;
      device_types = get_selected_device_types (dialog);

      persist = get_persist (dialog);

      if (dialog->screen_cast_enable)
        streams = screen_cast_widget_get_selected_streams (screen_cast_widget);
      enable_clipboard = adw_switch_row_get_active (dialog->allow_remote_clipboard_switch);
    }
  else
    {
      response = GTK_RESPONSE_CANCEL;
      device_types = 0;
      streams = NULL;
      enable_clipboard = FALSE;
    }

  g_signal_emit (dialog, signals[DONE], 0, response,
                 device_types, streams, enable_clipboard, persist);
}

static void
update_button_sensitivity (RemoteDesktopDialog *dialog)
{
  gboolean can_accept = FALSE;

  if (dialog->is_screen_cast_sources_selected)
    can_accept = TRUE;

  if (adw_switch_row_get_active (dialog->allow_remote_interaction_switch))
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
                           ScreenCastSelection *screen_cast_select,
                           gboolean clipboard_requested,
                           RemoteDesktopPersistMode persist_mode)
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
      gtk_widget_set_visible (GTK_WIDGET (screen_cast_widget), TRUE);
    }

  if (clipboard_requested)
    {
      gtk_widget_set_visible (GTK_WIDGET (dialog->allow_remote_clipboard_switch),
                              TRUE);
      adw_switch_row_set_active (dialog->allow_remote_clipboard_switch, TRUE);
    }
  else
    {
      gtk_widget_set_visible (GTK_WIDGET (dialog->allow_remote_clipboard_switch),
                              FALSE);
      adw_switch_row_set_active (dialog->allow_remote_clipboard_switch, FALSE);
    }

  switch (persist_mode)
    {
    case REMOTE_DESKTOP_PERSIST_MODE_NONE:
      break;
    case REMOTE_DESKTOP_PERSIST_MODE_PERSISTENT:
      gtk_widget_set_visible (GTK_WIDGET (dialog->persist_check), TRUE);
      G_GNUC_FALLTHROUGH;
    case REMOTE_DESKTOP_PERSIST_MODE_TRANSIENT:
      gtk_check_button_set_active (dialog->persist_check, TRUE);
      break;
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
  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_CANCEL, 0, NULL, FALSE, FALSE);

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
                                G_TYPE_NONE, 5,
                                G_TYPE_INT,
                                G_TYPE_INT,
                                G_TYPE_PTR_ARRAY,
                                G_TYPE_BOOLEAN,
                                G_TYPE_BOOLEAN);

  g_type_ensure (SCREEN_CAST_TYPE_WIDGET);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/remotedesktopdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, allow_remote_interaction_switch);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, allow_remote_clipboard_switch);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, persist_check);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, screen_cast_widget);
  gtk_widget_class_bind_template_child (widget_class, RemoteDesktopDialog, titlebar);
  gtk_widget_class_bind_template_callback (widget_class, button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_allow_remote_interaction_switch_active_changed_cb);
}
