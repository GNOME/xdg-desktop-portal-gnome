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

#define _GNU_SOURCE 1

#include "config.h"

#include <string.h>

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <gio/gdesktopappinfo.h>

#include "appchooserdialog.h"
#include "appchooserrow.h"

#define LOCATION_MAX_LENGTH 40
#define INITIAL_LIST_SIZE 3

struct _AppChooserDialog {
  AdwWindow parent;

  GtkWidget *cancel_button;
  GtkWidget *open_button;
  GtkWidget *list;
  GtkWidget *heading;

  char *content_type;

  char **choices;

  GtkWidget *selected_row;

  GAppInfo *info;
};

struct _AppChooserDialogClass {
  AdwWindowClass parent_class;

  void (* close) (AppChooserDialog *dialog);
};

enum {
  CLOSE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (AppChooserDialog, app_chooser_dialog, ADW_TYPE_WINDOW)

static void
app_chooser_dialog_init (AppChooserDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));
}

static void
app_chooser_dialog_finalize (GObject *object)
{
  AppChooserDialog *dialog = APP_CHOOSER_DIALOG (object);

  g_free (dialog->content_type);
  g_strfreev (dialog->choices);

  G_OBJECT_CLASS (app_chooser_dialog_parent_class)->finalize (object);
}

GAppInfo *
app_chooser_dialog_get_info (AppChooserDialog *dialog)
{
  return dialog->info;
}

static void
close_dialog (AppChooserDialog *dialog,
              GAppInfo *info)
{
  dialog->info = info;
  g_signal_emit (dialog, signals[CLOSE], 0);
}

static void
row_activated (GtkListBox *list,
               GtkWidget *row,
               AppChooserDialog *dialog)
{
  GAppInfo *info = NULL;

  if (dialog->selected_row)
    info = app_chooser_row_get_info (APP_CHOOSER_ROW (dialog->selected_row));
  close_dialog (dialog, info);
}

static void
row_selected (GtkListBox *list,
              GtkWidget *row,
              AppChooserDialog *dialog)
{
  gtk_widget_set_sensitive (dialog->open_button, TRUE);
  dialog->selected_row = row;
}

static void
cancel_clicked (GtkWidget *button,
                AppChooserDialog *dialog)
{
  close_dialog (dialog, NULL);
}

static void
open_clicked (GtkWidget *button,
              AppChooserDialog *dialog)
{
  GAppInfo *info = NULL;

  if (dialog->selected_row)
    info = app_chooser_row_get_info (APP_CHOOSER_ROW (dialog->selected_row));
  close_dialog (dialog, info);
}

static gboolean
close_dialog_binding_cb (GtkWidget *dialog,
                         GVariant *args,
                         gpointer user_data)
{
  close_dialog (APP_CHOOSER_DIALOG (dialog), NULL);
  return GDK_EVENT_STOP;
}

static gboolean
app_chooser_close_request (GtkWindow *window)
{
  close_dialog (APP_CHOOSER_DIALOG (window), NULL);

  return TRUE;
}

static void
app_chooser_dialog_close (AppChooserDialog *dialog)
{
  gtk_window_close (GTK_WINDOW (dialog));
}

static void
app_chooser_dialog_class_init (AppChooserDialogClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = app_chooser_dialog_finalize;

  window_class->close_request = app_chooser_close_request;

  class->close = app_chooser_dialog_close;

  signals[CLOSE] = g_signal_new ("close",
                                G_TYPE_FROM_CLASS (class),
                                G_SIGNAL_ACTION | G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 0);

  gtk_widget_class_add_binding (widget_class, GDK_KEY_Escape, 0, close_dialog_binding_cb, NULL);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/appchooserdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, open_button);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, list);
  gtk_widget_class_bind_template_child (widget_class, AppChooserDialog, heading);

  gtk_widget_class_bind_template_callback (widget_class, row_activated);
  gtk_widget_class_bind_template_callback (widget_class, row_selected);
  gtk_widget_class_bind_template_callback (widget_class, cancel_clicked);
  gtk_widget_class_bind_template_callback (widget_class, open_clicked);
}

static void
ensure_default_in_initial_list (const char **choices,
                                const char  *default_id)
{
  int i;
  guint n_choices;

  if (default_id == NULL)
    return;

  n_choices = g_strv_length ((char **)choices);
  if (n_choices <= INITIAL_LIST_SIZE)
    return;

  for (i = 0; i < INITIAL_LIST_SIZE; i++)
    {
      if (strcmp (choices[i], default_id) == 0)
        return;
    }

  for (i = INITIAL_LIST_SIZE; i < n_choices; i++)
    {
      if (strcmp (choices[i], default_id) == 0)
        {
          const char *tmp = choices[0];
          choices[0] = choices[i];
          choices[i] = tmp;
          return;
        }
    }

  g_warning ("default_id not found\n");
}

AppChooserDialog *
app_chooser_dialog_new (const char **choices,
                        const char *default_id,
                        const char *content_type,
                        const char *location)
{
  AppChooserDialog *dialog;
  GtkCssProvider *provider;
  int n_choices;
  int i;

  dialog = g_object_new (app_chooser_dialog_get_type (), NULL);

  dialog->content_type = g_strdup (content_type);

  if (location)
    {
      g_autofree char *heading = NULL;

      heading = g_strdup_printf (_("Choose which application to use to open <b>%s</b>."), location);
      gtk_label_set_markup (GTK_LABEL (dialog->heading), heading);
    }
  else
    {
      gtk_label_set_label (GTK_LABEL (dialog->heading), _("Choose an application."));
    }

  ensure_default_in_initial_list (choices, default_id);

  dialog->choices = g_strdupv ((char **)choices);

  n_choices = g_strv_length ((char **)choices);
  gtk_window_set_default_widget (GTK_WINDOW (dialog), dialog->open_button);
  for (i = 0; i < n_choices; i++)
    {
      g_autofree char *desktop_id = g_strconcat (choices[i], ".desktop", NULL);
      g_autoptr(GAppInfo) info = G_APP_INFO (g_desktop_app_info_new (desktop_id));
      GtkWidget *row;

      if (!info)
        continue;

      row = GTK_WIDGET (app_chooser_row_new (info));
      gtk_widget_set_visible (row, TRUE);
      gtk_list_box_insert (GTK_LIST_BOX (dialog->list), row, -1);

      if (default_id && strcmp (choices[i], default_id) == 0)
        {
          gtk_list_box_select_row (GTK_LIST_BOX (dialog->list), GTK_LIST_BOX_ROW (row));
          gtk_widget_set_sensitive (dialog->open_button, TRUE);
          dialog->selected_row = row;
        }
    }

  return dialog;
}

void
app_chooser_dialog_update_choices (AppChooserDialog  *dialog,
                                   const char       **choices)
{
  int i;
  GPtrArray *new_choices;

  new_choices = g_ptr_array_new ();
  g_ptr_array_set_size (new_choices, g_strv_length (dialog->choices));
  for (i = 0; dialog->choices[i]; i++)
    new_choices->pdata[i] = dialog->choices[i];

  for (i = 0; choices[i]; i++)
    {
      if (g_strv_contains ((const char * const *)dialog->choices, choices[i]))
        continue;

      g_ptr_array_add (new_choices, g_strdup (choices[i]));

      g_autofree char *desktop_id = NULL;
      g_autoptr(GAppInfo) info = NULL;
      GtkWidget *row;

      desktop_id = g_strconcat (choices[i], ".desktop", NULL);
      info = G_APP_INFO (g_desktop_app_info_new (desktop_id));

      if (!info)
        continue;

      row = GTK_WIDGET (app_chooser_row_new (info));
      gtk_widget_set_visible (row, TRUE);
      gtk_list_box_insert (GTK_LIST_BOX (dialog->list), row, -1);
    }

  g_ptr_array_add (new_choices, NULL);

  g_free (dialog->choices);
  dialog->choices = (char **) g_ptr_array_free (new_choices, FALSE);
}
