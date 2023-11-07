#define _GNU_SOURCE 1

#include <string.h>

#include <adwaita.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include <glib/gi18n.h>

#include "accountdialog.h"

struct _AccountDialog {
  AdwWindow parent;

  GtkWidget *heading;
  GtkWidget *accept_button;
  GtkWidget *name;
  GtkWidget *fullname;
  AdwAvatar *image;
  GtkButton *image_button;

  char *icon_file;
};

enum {
  DONE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (AccountDialog, account_dialog, ADW_TYPE_WINDOW)

static void
account_dialog_init (AccountDialog *dialog)
{
  static GtkCssProvider *provider = NULL;
  gtk_widget_init_template (GTK_WIDGET (dialog));

  if (provider == NULL)
    {
      provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_resource (provider, "/org/freedesktop/portal/desktop/gnome/accountdialog.css");
      gtk_style_context_add_provider_for_display (gtk_widget_get_display (GTK_WIDGET (dialog->image_button)),
                                                  GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

static void
account_dialog_finalize (GObject *object)
{
  AccountDialog *dialog = ACCOUNT_DIALOG (object);

  g_clear_pointer (&dialog->icon_file, g_free);

  G_OBJECT_CLASS (account_dialog_parent_class)->finalize (object);
}

static gboolean
account_dialog_close_request (GtkWindow *dialog)
{
  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_CANCEL, NULL, NULL, NULL);

  return TRUE;
}

static void
button_clicked (GtkWidget     *button,
                AccountDialog *dialog)
{
  int response;
  const char *user_name;
  const char *real_name;

  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  if (button == dialog->accept_button)
    response = GTK_RESPONSE_OK;
  else
    response = GTK_RESPONSE_CANCEL;

  user_name = gtk_editable_get_text (GTK_EDITABLE (dialog->name));
  real_name = gtk_editable_get_text (GTK_EDITABLE (dialog->fullname));
  g_signal_emit (dialog, signals[DONE], 0, response, user_name, real_name, dialog->icon_file);
}

static void
dialog_set_icon_file (AccountDialog *dialog,
                      const char    *icon_file)
{
  g_clear_pointer (&dialog->icon_file, g_free);
  dialog->icon_file = g_strdup (icon_file);

  if (icon_file)
    {
      g_autoptr(GdkTexture) texture = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GFile) file = NULL;

      file = g_file_new_for_path (icon_file);
      texture = gdk_texture_new_from_file (file, &error);
      if (error)
        g_warning ("Failed to load account %s: %s", icon_file, error->message);
      adw_avatar_set_custom_image (dialog->image, GDK_PAINTABLE (texture));
      gtk_widget_remove_css_class (GTK_WIDGET (dialog->image), "dim-label");
    }
  else
    {
      adw_avatar_set_custom_image (dialog->image, NULL);
      adw_avatar_set_icon_name (dialog->image, "camera-photo-symbolic");
      gtk_widget_add_css_class (GTK_WIDGET (dialog->image), "dim-label");
    }
}

static void
file_chooser_response (GtkWidget *widget,
                       int        response,
                       gpointer   user_data)
{
  AccountDialog *dialog = user_data;
  g_autoptr (GFile) file = NULL;
  g_autofree char *path = NULL;

  file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (widget));
  path = file ? g_file_get_path (file) : NULL;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      G_GNUC_FALLTHROUGH;

    case GTK_RESPONSE_DELETE_EVENT:
    case GTK_RESPONSE_CANCEL:
      break;

    case GTK_RESPONSE_OK:
      dialog_set_icon_file (dialog, path);
      break;

    case GTK_RESPONSE_CLOSE:
      dialog_set_icon_file (dialog, NULL);
      break;
    }

  gtk_window_destroy (GTK_WINDOW (widget));
}

static void
image_button_clicked (AccountDialog *dialog)
{
  GtkWidget *chooser;
  GtkFileFilter *filter;

  chooser = gtk_file_chooser_dialog_new (_("Select an Image"),
                                         GTK_WINDOW (dialog),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         _("Cancel"), GTK_RESPONSE_CANCEL,
                                         _("Select"), GTK_RESPONSE_OK,
                                         _("Clear"),  GTK_RESPONSE_CLOSE,
                                         NULL);
  gtk_window_set_modal (GTK_WINDOW (chooser), TRUE);

  gtk_dialog_set_default_response (GTK_DIALOG (chooser), GTK_RESPONSE_OK);

  filter = gtk_file_filter_new ();
  gtk_file_filter_set_name (filter, _("Images"));
  gtk_file_filter_add_pixbuf_formats (filter);
  gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (chooser), filter);

  g_signal_connect (chooser, "response", G_CALLBACK (file_chooser_response), dialog);

  gtk_window_present (GTK_WINDOW (chooser));
}

static void
account_dialog_class_init (AccountDialogClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (class);

  object_class->finalize = account_dialog_finalize;

  window_class->close_request = account_dialog_close_request;

  signals[DONE] = g_signal_new ("done",
                                G_TYPE_FROM_CLASS (class),
                                G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 4,
                                G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/accountdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, AccountDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, AccountDialog, heading);
  gtk_widget_class_bind_template_child (widget_class, AccountDialog, name);
  gtk_widget_class_bind_template_child (widget_class, AccountDialog, fullname);
  gtk_widget_class_bind_template_child (widget_class, AccountDialog, image);
  gtk_widget_class_bind_template_child (widget_class, AccountDialog, image_button);

  gtk_widget_class_bind_template_callback (widget_class, button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, image_button_clicked);
}

AccountDialog *
account_dialog_new (const char *app_id,
                    const char *user_name,
                    const char *real_name,
                    const char *icon_file,
                    const char *reason)
{
  AccountDialog *dialog;
  g_autofree char *heading = NULL;
  g_autoptr(GdkTexture) texture = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;

  dialog = g_object_new (account_dialog_get_type (), NULL);

  if (strcmp (app_id, "") != 0)
    {
      g_autofree char *id = NULL;
      g_autoptr(GAppInfo) info = NULL;
      id = g_strconcat (app_id, ".desktop", NULL);
      info = G_APP_INFO (g_desktop_app_info_new (id));
      heading = g_strdup_printf (_("Share your personal information with %1$s? %2$s"),
                                 g_app_info_get_display_name (info),
                                 reason ?: "");
    }
  else
    {
      heading = g_strdup_printf (_("Share your personal information with the requesting app? %s"),
                                 reason ?: "");
    }

  gtk_label_set_label (GTK_LABEL (dialog->heading), heading);
  gtk_editable_set_text (GTK_EDITABLE (dialog->name), user_name);
  gtk_editable_set_text (GTK_EDITABLE (dialog->fullname), real_name);

  file = g_file_new_for_path (icon_file);
  texture = gdk_texture_new_from_file (file, &error);
  if (error)
    {
      g_warning ("Failed to load account %s: %s", icon_file, error->message);
    }
  else
    {
      adw_avatar_set_custom_image (dialog->image, GDK_PAINTABLE (texture));
    }

  dialog->icon_file = g_strdup (icon_file);

  return dialog;
}
