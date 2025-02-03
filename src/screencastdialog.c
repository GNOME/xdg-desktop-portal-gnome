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

#include "config.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>

#include "screencastdialog.h"
#include "screencastwidget.h"
#include "displaystatetracker.h"

struct _ScreenCastDialog
{
  AdwWindow parent;

  GtkWidget *accept_button;
  GtkWidget *screen_cast_widget;

  gboolean multiple;
};

enum
{
  DONE,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE (ScreenCastDialog, screen_cast_dialog, ADW_TYPE_WINDOW)

/*
 * Callbacks
 */

static void
on_button_clicked_cb (GtkWidget        *button,
                      ScreenCastDialog *dialog)
{
  ScreenCastPersistMode persist_mode;
  g_autoptr(GPtrArray) streams = NULL;
  int response;

  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  if (button == dialog->accept_button)
    {
      ScreenCastWidget *screen_cast_widget =
        SCREEN_CAST_WIDGET (dialog->screen_cast_widget);

      response = GTK_RESPONSE_OK;
      streams = screen_cast_widget_get_selected_streams (screen_cast_widget);
      persist_mode = screen_cast_widget_get_persist_mode (screen_cast_widget);
    }
  else
    {
      response = GTK_RESPONSE_CANCEL;
      persist_mode = SCREEN_CAST_PERSIST_MODE_NONE;
      streams = NULL;
    }

  g_signal_emit (dialog, signals[DONE], 0, response, persist_mode, streams);
}

static void
on_has_selection_changed (ScreenCastWidget *screen_cast_widget,
                          gboolean          has_selection,
                          ScreenCastDialog *dialog)
{
  if (has_selection)
    gtk_widget_set_sensitive (dialog->accept_button, TRUE);
  else
    gtk_widget_set_sensitive (dialog->accept_button, FALSE);
}

/*
 * GtkWindow overrides
 */

static gboolean
screen_cast_dialog_close_request (GtkWindow *dialog)
{
  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  g_signal_emit (dialog, signals[DONE], 0,
                 GTK_RESPONSE_CANCEL,
                 SCREEN_CAST_PERSIST_MODE_NONE,
                 NULL);

  return TRUE;
}

static void
screen_cast_dialog_class_init (ScreenCastDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  window_class->close_request = screen_cast_dialog_close_request;

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

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/screencastdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, ScreenCastDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, ScreenCastDialog, screen_cast_widget);
  gtk_widget_class_bind_template_callback (widget_class, on_button_clicked_cb);
}

static void
screen_cast_dialog_init (ScreenCastDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));

  g_signal_connect (dialog->screen_cast_widget, "has-selection-changed",
                    G_CALLBACK (on_has_selection_changed), dialog);
}

ScreenCastDialog *
screen_cast_dialog_new (const char            *app_id,
                        ScreenCastSelection   *select,
                        ScreenCastPersistMode  persist_mode)
{
  ScreenCastDialog *dialog;
  ScreenCastWidget *screen_cast_widget;

  dialog = g_object_new (SCREEN_CAST_TYPE_DIALOG, NULL);
  screen_cast_widget = SCREEN_CAST_WIDGET (dialog->screen_cast_widget);
  screen_cast_widget_set_app_id (screen_cast_widget, app_id);
  screen_cast_widget_set_allow_multiple (screen_cast_widget, select->multiple);
  screen_cast_widget_set_source_types (screen_cast_widget,
                                       select->source_types);
  screen_cast_widget_set_persist_mode (screen_cast_widget, persist_mode);

  return dialog;
}
