/*
 * Copyright © 2022 Red Hat, Inc
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

#include "inputcapturedialog.h"

struct _InputCaptureDialog
{
  GtkWindow parent;

  GtkWidget *accept_button;
  GtkSwitch *allow_input_capture_switch;
  GtkLabel *allow_input_capture_heading;
  GtkHeaderBar *titlebar;
};

struct _InputCaptureDialogClass
{
  GtkWindowClass *parent_class;
};

enum
{
  DONE,

  N_SIGNAL
};

static guint signals[N_SIGNAL];

G_DEFINE_TYPE (InputCaptureDialog, input_capture_dialog, GTK_TYPE_WINDOW)

static void
button_clicked (GtkWidget          *button,
                InputCaptureDialog *dialog)
{
  int response;

  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  if (button == dialog->accept_button)
    response = GTK_RESPONSE_OK;
  else
    response = GTK_RESPONSE_CANCEL;

  g_signal_emit (dialog, signals[DONE], 0, response);
}

static void
update_button_sensitivity (InputCaptureDialog *dialog)
{
  gboolean can_accept = FALSE;

  if (gtk_switch_get_active (dialog->allow_input_capture_switch))
    can_accept = TRUE;

  gtk_widget_set_sensitive (dialog->accept_button, can_accept);
}

static void
on_allow_input_capture_switch_active_changed_cb (GtkCheckButton     *checkbutton,
                                                 GParamSpec         *pspec,
                                                 InputCaptureDialog *dialog)
{
  update_button_sensitivity (dialog);
}

void
set_app_id (InputCaptureDialog *dialog,
            const char         *app_id)
{
  g_autofree char *heading = NULL;

  if (app_id && app_id[0] != '\0')
    {
      g_autofree char *id = NULL;
      g_autoptr(GAppInfo) info = NULL;
      const gchar *display_name = NULL;

      id = g_strconcat (app_id, ".desktop", NULL);
      info = G_APP_INFO (g_desktop_app_info_new (id));
      if (info)
        display_name = g_app_info_get_display_name (info);
      else
        display_name = g_strdup (app_id);
      heading = g_strdup_printf (_("The application %s wants to capture input events"),
                                 display_name);
    }
  else
    {
      heading = g_strdup (_("An application wants to capture input events"));
    }

  gtk_label_set_label (GTK_LABEL (dialog->allow_input_capture_heading), heading);
}

InputCaptureDialog *
input_capture_dialog_new (const char *app_id)
{
  InputCaptureDialog *dialog;

  dialog = g_object_new (INPUT_CAPTURE_TYPE_DIALOG, NULL);
  set_app_id (dialog, app_id);

  return dialog;
}

static void
input_capture_dialog_init (InputCaptureDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));
}

static gboolean
input_capture_dialog_close_request (GtkWindow *dialog)
{
  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_CANCEL, 0, NULL);

  return TRUE;
}

static void
input_capture_dialog_class_init (InputCaptureDialogClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  window_class->close_request = input_capture_dialog_close_request;

  signals[DONE] = g_signal_new ("done",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 1,
                                G_TYPE_INT);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/inputcapturedialog.ui");
  gtk_widget_class_bind_template_child (widget_class, InputCaptureDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, InputCaptureDialog, allow_input_capture_heading);
  gtk_widget_class_bind_template_child (widget_class, InputCaptureDialog, allow_input_capture_switch);
  gtk_widget_class_bind_template_child (widget_class, InputCaptureDialog, titlebar);
  gtk_widget_class_bind_template_callback (widget_class, button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_allow_input_capture_switch_active_changed_cb);
}
