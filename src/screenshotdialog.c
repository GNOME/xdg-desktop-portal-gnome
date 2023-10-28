#define _GNU_SOURCE 1

#include <string.h>

#include <adwaita.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

#include <glib/gi18n.h>

#include "screenshotdialog.h"
#include "xdg-desktop-portal-dbus.h"

struct _ScreenshotDialog {
  AdwWindow parent;

  GtkWidget *picture;
  GtkWidget *heading;
  GtkWidget *accept_button;
  GtkWidget *options_button;
  GtkWidget *cancel_button;
  GtkWidget *screenshot_button;
  GtkWidget *stack;
  GtkWidget *header_stack;
  GtkWidget *delay_box;

  char *filename;

  GActionGroup *actions;
  GtkAdjustment *delay_adjustment;

  guint timeout;

  OrgGnomeShellScreenshot *shell;
  GCancellable *cancellable;
  gboolean skip_dialog;
};

enum {
  DONE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

G_DEFINE_TYPE (ScreenshotDialog, screenshot_dialog, ADW_TYPE_WINDOW)

static void
update_border (ScreenshotDialog *dialog,
               const char *kind)
{
  GAction *border;
  GAction *pointer;

  border = g_action_map_lookup_action (G_ACTION_MAP (dialog->actions), "border");
  pointer = g_action_map_lookup_action (G_ACTION_MAP (dialog->actions), "pointer");

  if (strcmp (kind, "screen") == 0)
    {
      g_simple_action_set_state (G_SIMPLE_ACTION (border), g_variant_new_boolean (FALSE));
      g_simple_action_set_enabled (G_SIMPLE_ACTION (border), FALSE);
      g_simple_action_set_enabled (G_SIMPLE_ACTION (pointer), TRUE);
      gtk_widget_set_sensitive (dialog->delay_box, TRUE);
    }
  else if (strcmp (kind, "window") == 0)
    {
      g_simple_action_set_enabled (G_SIMPLE_ACTION (border), TRUE);
      g_simple_action_set_enabled (G_SIMPLE_ACTION (pointer), TRUE);
      gtk_widget_set_sensitive (dialog->delay_box, TRUE);
    }
  else
    {
      g_simple_action_set_state (G_SIMPLE_ACTION (border), g_variant_new_boolean (FALSE));
      g_simple_action_set_enabled (G_SIMPLE_ACTION (border), FALSE);
      g_simple_action_set_state (G_SIMPLE_ACTION (pointer), g_variant_new_boolean (FALSE));
      g_simple_action_set_enabled (G_SIMPLE_ACTION (pointer), FALSE);
      gtk_widget_set_sensitive (dialog->delay_box, FALSE);
    }
}

static void
change_grab (GSimpleAction *action,
             GVariant *value,
             gpointer data)
{
  ScreenshotDialog *dialog = data;
  const char *kind;

  g_simple_action_set_state (action, value);

  kind = g_variant_get_string (value, NULL);
  update_border (dialog, kind);
}

static GActionEntry entries[] = {
  { "grab", NULL, "s", "'screen'", change_grab },
  { "pointer", NULL, NULL, "false", NULL },
  { "border", NULL, NULL, "false", NULL }
};

static void
screenshot_dialog_init (ScreenshotDialog *dialog)
{
  gtk_widget_init_template (GTK_WIDGET (dialog));

  dialog->cancellable = g_cancellable_new ();

  dialog->actions = G_ACTION_GROUP (g_simple_action_group_new ());
  g_action_map_add_action_entries (G_ACTION_MAP (dialog->actions),
                                   entries,
                                   G_N_ELEMENTS (entries),
                                   dialog);
  update_border (dialog, "window");
  gtk_widget_insert_action_group (GTK_WIDGET (dialog), "dialog", dialog->actions);
}

static void
show_options (ScreenshotDialog *dialog)
{
  gtk_stack_set_visible_child_name (GTK_STACK (dialog->stack), "options");
  gtk_stack_set_visible_child_name (GTK_STACK (dialog->header_stack), "options");
}

static void
show_screenshot (ScreenshotDialog *dialog,
                 const char *filename)
{
  g_free (dialog->filename);
  dialog->filename = g_strdup (filename);

  gtk_picture_set_filename (GTK_PICTURE (dialog->picture), filename);

  gtk_stack_set_visible_child_name (GTK_STACK (dialog->stack), "screenshot");
  gtk_stack_set_visible_child_name (GTK_STACK (dialog->header_stack), "screenshot");
}

static void
maybe_show_screenshot (ScreenshotDialog *dialog,
                       const char       *filename)
{
  if (dialog->skip_dialog)
    {
      gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);
      g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_OK, filename);
    }
  else
    {
      show_screenshot (dialog, filename);
      gtk_window_present (GTK_WINDOW (dialog));
    }
}

static void
screenshot_done (GObject *source,
                 GAsyncResult *result,
                 gpointer data)
{
  ScreenshotDialog *dialog = data;
  gboolean success;
  g_autofree char *filename = NULL;
  g_autoptr(GError) error = NULL;

  if (!org_gnome_shell_screenshot_call_screenshot_finish (dialog->shell,
                                                          &success,
                                                          &filename,
                                                          result,
                                                          &error))
    {
      g_print ("Failed to get screenshot: %s\n", error->message);
      return;
    }

  maybe_show_screenshot (dialog, filename);
}

static void
screenshot_window_done (GObject *source,
                        GAsyncResult *result,
                        gpointer data)
{
  ScreenshotDialog *dialog = data;
  gboolean success;
  g_autofree char *filename = NULL;
  g_autoptr(GError) error = NULL;

  if (!org_gnome_shell_screenshot_call_screenshot_window_finish (dialog->shell,
                                                                 &success,
                                                                 &filename,
                                                                 result,
                                                                 &error))
    {
      g_print ("Failed to get window screenshot: %s\n", error->message);
      return;
    }

  maybe_show_screenshot (dialog, filename);
}

static void
screenshot_area_done (GObject *source,
                      GAsyncResult *result,
                      gpointer data)
{
  ScreenshotDialog *dialog = data;
  gboolean success;
  g_autofree char *filename = NULL;
  g_autoptr(GError) error = NULL;

  if (!org_gnome_shell_screenshot_call_screenshot_area_finish (dialog->shell,
                                                               &success,
                                                               &filename,
                                                               result,
                                                               &error))
    {
      g_print ("Failed to get area screenshot: %s\n", error->message);
      return;
    }

  maybe_show_screenshot (dialog, filename);
}


static gboolean
do_take_screenshot (gpointer data)
{
  ScreenshotDialog *dialog = data;
  GAction *action;
  g_autoptr(GVariant) grab = NULL;
  g_autoptr(GVariant) pointer = NULL;
  g_autoptr(GVariant) border = NULL;
  const char *kind;
  gboolean include_pointer;
  gboolean include_border;

  action = g_action_map_lookup_action (G_ACTION_MAP (dialog->actions), "grab");
  grab = g_action_get_state (action);
  kind = g_variant_get_string (grab, NULL);

  action = g_action_map_lookup_action (G_ACTION_MAP (dialog->actions), "pointer");
  pointer = g_action_get_state (action);
  include_pointer = g_variant_get_boolean (pointer);

  action = g_action_map_lookup_action (G_ACTION_MAP (dialog->actions), "border");
  border = g_action_get_state (action);
  include_border = g_variant_get_boolean (border);

  if (strcmp (kind, "screen") == 0)
    {
      org_gnome_shell_screenshot_call_screenshot (dialog->shell,
                                                  include_pointer,
                                                  TRUE,
                                                  "Screenshot",
                                                  dialog->cancellable,
                                                  screenshot_done,
                                                  dialog);
    }
  else if (strcmp (kind, "window") == 0)
    {
      org_gnome_shell_screenshot_call_screenshot_window (dialog->shell,
                                                         include_border,
                                                         include_pointer,
                                                         TRUE,
                                                         "Screenshot",
                                                         dialog->cancellable,
                                                         screenshot_window_done,
                                                         dialog);
    }
  else
    {
      g_print ("Should not get here, screenshot kind: %s\n", kind);
    }

  dialog->timeout = 0;

  return G_SOURCE_REMOVE;
}

static void
select_area_done (GObject *source,
                  GAsyncResult *result,
                  gpointer data)
{
  ScreenshotDialog *dialog = data;
  g_autoptr(GError) error = NULL;
  gint x, y, w, h;

  if (!org_gnome_shell_screenshot_call_select_area_finish (dialog->shell,
                                                           &x, &y, &w, &h,
                                                           result,
                                                           &error))
    {
      g_print ("Failed to select area: %s\n", error->message);
      return;
    }

  org_gnome_shell_screenshot_call_screenshot_area (dialog->shell,
                                                   x, y, w, h,
                                                   TRUE,
                                                   "Screenshot",
                                                   dialog->cancellable,
                                                   screenshot_area_done,
                                                   dialog);
}

static void
take_screenshot (ScreenshotDialog *dialog)
{
  GAction *action;
  g_autoptr(GVariant) grab = NULL;
  const char *kind;

  action = g_action_map_lookup_action (G_ACTION_MAP (dialog->actions), "grab");
  grab = g_action_get_state (action);
  kind = g_variant_get_string (grab, NULL);

  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  if (strcmp (kind, "area") == 0)
    {
      org_gnome_shell_screenshot_call_select_area (dialog->shell,
                                                   dialog->cancellable,
                                                   select_area_done,
                                                   dialog);
    }
  else
    {
      guint interval;

      interval = (guint)gtk_adjustment_get_value (dialog->delay_adjustment);
      dialog->timeout = g_timeout_add_seconds (interval, do_take_screenshot, dialog);
    }
}

static void
button_clicked (GtkWidget *button,
                ScreenshotDialog *dialog)
{
  int response;

  if (button == dialog->accept_button)
    response = GTK_RESPONSE_OK;
  else
    response = GTK_RESPONSE_CANCEL;

  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);
  g_signal_emit (dialog, signals[DONE], 0, response, dialog->filename);
}

static void
screenshot_dialog_finalize (GObject *object)
{
  ScreenshotDialog *dialog = SCREENSHOT_DIALOG (object);

  if (dialog->timeout)
    g_source_remove (dialog->timeout);

  g_cancellable_cancel (dialog->cancellable);
  g_object_unref (dialog->cancellable);

  g_object_unref (dialog->actions);
  g_free (dialog->filename);

  g_object_unref (dialog->shell);

  G_OBJECT_CLASS (screenshot_dialog_parent_class)->finalize (object);
}

static gboolean
screenshot_dialog_close_request (GtkWindow *dialog)
{
  gtk_widget_set_visible (GTK_WIDGET (dialog), FALSE);

  g_signal_emit (dialog, signals[DONE], 0, GTK_RESPONSE_CANCEL, NULL);

  return TRUE;
}

static void
screenshot_dialog_class_init (ScreenshotDialogClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (class);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = screenshot_dialog_finalize;

  window_class->close_request = screenshot_dialog_close_request;

  signals[DONE] = g_signal_new ("done",
                                G_TYPE_FROM_CLASS (class),
                                G_SIGNAL_RUN_LAST,
                                0,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 2,
                                G_TYPE_INT, G_TYPE_STRING);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/freedesktop/portal/desktop/gnome/screenshotdialog.ui");
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, accept_button);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, cancel_button);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, options_button);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, screenshot_button);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, heading);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, picture);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, stack);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, header_stack);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, delay_adjustment);
  gtk_widget_class_bind_template_child (widget_class, ScreenshotDialog, delay_box);
  gtk_widget_class_bind_template_callback (widget_class, button_clicked);
  gtk_widget_class_bind_template_callback (widget_class, show_options);
  gtk_widget_class_bind_template_callback (widget_class, take_screenshot);
}

ScreenshotDialog *
screenshot_dialog_new (const char *app_id,
                       gboolean permission_store_checked,
                       OrgGnomeShellScreenshot *shell)
{
  g_autoptr(GtkWindowGroup) window_group = NULL;
  ScreenshotDialog *dialog;
  g_autofree char *heading = NULL;

  dialog = g_object_new (screenshot_dialog_get_type (), NULL);

  if (strcmp (app_id, "") != 0)
    {
      g_autofree char *id = NULL;
      g_autoptr(GAppInfo) info = NULL;
      id = g_strconcat (app_id, ".desktop", NULL);
      info = G_APP_INFO (g_desktop_app_info_new (id));
      if (info)
        heading = g_strdup_printf (_("Share this screenshot with %s?"), g_app_info_get_display_name (info));
    }

  if (heading == NULL)
    heading = g_strdup (_("Share this screenshot with the requesting app?"));

  gtk_label_set_label (GTK_LABEL (dialog->heading), heading);

  window_group = gtk_window_group_new ();
  gtk_window_group_add_window (window_group, GTK_WINDOW (dialog));

  dialog->shell = g_object_ref (shell);
  dialog->skip_dialog = permission_store_checked;

  do_take_screenshot (dialog);

  return dialog;
}
