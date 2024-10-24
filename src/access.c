#include "config.h"

#include <glib/gi18n.h>

#include <adwaita.h>
#include <gxdp.h>

#include "xdg-desktop-portal-dbus.h"

#include "request.h"
#include "utils.h"

typedef struct {
  XdpImplAccess *impl;
  GDBusMethodInvocation *invocation;
  Request *request;

  GtkWidget *dialog;
  GxdpExternalWindow *external_parent;
  GHashTable *choices;

  int response;
} AccessDialogHandle;

static void
access_dialog_handle_free (gpointer data)
{
  AccessDialogHandle *handle = data;

  g_clear_object (&handle->external_parent);
  g_clear_object (&handle->request);
  g_clear_object (&handle->dialog);
  g_clear_pointer (&handle->choices, g_hash_table_destroy);

  g_free (handle);
}

static void
access_dialog_handle_close (AccessDialogHandle *handle)
{
  gtk_window_destroy (GTK_WINDOW (handle->dialog));
  access_dialog_handle_free (handle);
}

static void
send_response (AccessDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->request->exported)
    request_unexport (handle->request);

  if (handle->response == 0 && handle->choices != NULL)
    {
      GVariantBuilder choice_builder;
      GHashTableIter iter;
      const char *key;
      GtkWidget *widget;

      g_variant_builder_init (&choice_builder, G_VARIANT_TYPE_VARDICT);
      g_hash_table_iter_init (&iter, handle->choices);
      while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&widget))
        {
          g_auto (GStrv) str = NULL;

          str = g_strsplit (key, ":", -1);

          if (g_strv_length (str) > 1)
            {
              if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (widget)))
                continue;

              g_variant_builder_add (&choice_builder, "{sv}", str[0], g_variant_new_string (str[1]));
            }
          else
            {
              gboolean active;

              active = gtk_check_button_get_active (GTK_CHECK_BUTTON (widget));
              g_variant_builder_add (&choice_builder, "{sv}", str[0], g_variant_new_string (active ? "true" : "false"));
            }
        }

      g_variant_builder_add (&opt_builder, "{sv}", "choices", g_variant_builder_end (&choice_builder));
    }

  xdp_impl_access_complete_access_dialog (handle->impl,
                                          handle->invocation,
                                          handle->response,
                                          g_variant_builder_end (&opt_builder));

  access_dialog_handle_close (handle);
}

static void
access_dialog_response (GtkWidget  *widget,
                        const char *response,
                        gpointer    user_data)
{
  AccessDialogHandle *handle = user_data;

  if (strcmp (response, "close") == 0)
    handle->response = 2;
  else if (strcmp (response, "deny") == 0)
    handle->response = 1;
  else if (strcmp (response, "grant") == 0)
    handle->response = 0;
  else
    g_assert_not_reached ();

  send_response (handle);
}

static void
add_choice (GtkWidget  *box,
            GVariant   *choice,
            GHashTable *table)
{
  g_autoptr(GVariant) options = NULL;
  const char *selected;
  const char *name;
  const char *id;

  g_variant_get (choice, "(&s&s@a(ss)&s)", &id, &name, &options, &selected);

  if (g_variant_n_children (options) == 0)
    {
      GtkWidget *row, *button;

      row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);

      button = gtk_check_button_new ();
      if (strcmp (selected, "true") == 0)
        gtk_check_button_set_active (GTK_CHECK_BUTTON (button), TRUE);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), button);
      adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), button);
      g_hash_table_insert (table, g_strdup (id), button);

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (box), row);
    }
  else
    {
      GtkWidget *group = NULL;
      GtkWidget *list;

      list = adw_preferences_group_new ();
      adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (list), name);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (box), list);

      for (size_t i = 0; i < g_variant_n_children (options); i++)
        {
          const char *option_id;
          const char *option_name;
          GtkWidget *row, *radio;

          g_variant_get_child (options, i, "(&s&s)", &option_id, &option_name);

          row = adw_action_row_new ();

          radio = gtk_check_button_new_with_label (option_name);
          g_hash_table_insert (table, g_strconcat (id, ":", option_id, NULL), radio);

          if (group)
            gtk_check_button_set_group (GTK_CHECK_BUTTON (radio),
                                        GTK_CHECK_BUTTON (group));
          else
            group = radio;

          if (strcmp (selected, option_id) == 0)
            gtk_check_button_set_active (GTK_CHECK_BUTTON (radio), TRUE);
          adw_action_row_add_prefix (ADW_ACTION_ROW (row), radio);
          adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), radio);

          adw_preferences_group_add (ADW_PREFERENCES_GROUP (box), row);
        }
    }
}

static GtkWidget *
create_access_dialog (GxdpExternalWindow  *parent,
                      const char          *title,
                      const char          *subtitle,
                      const char          *body,
                      GVariant            *options,
                      GHashTable         **choice_table)
{
  g_autoptr(GtkWindowGroup) window_group = NULL;
  g_autoptr(GVariant) choices = NULL;
  GtkWidget *dialog;
  GtkWidget *fake_parent;
  GtkWidget *extra;
  const char *deny_label;
  const char *grant_label;
  gboolean modal;

  if (!g_variant_lookup (options, "modal", "b", &modal))
    modal = TRUE;

  if (!g_variant_lookup (options, "deny_label", "&s", &deny_label))
    deny_label = _("Deny");

  if (!g_variant_lookup (options, "grant_label", "&s", &grant_label))
    grant_label = _("Allow");

  choices = g_variant_lookup_value (options, "choices", G_VARIANT_TYPE ("a(ssa(ss)s)"));

  fake_parent = g_object_new (GTK_TYPE_WINDOW, NULL);
  g_object_ref_sink (fake_parent);

  dialog = adw_message_dialog_new (GTK_WINDOW (fake_parent), title, subtitle);
  gtk_window_set_transient_for (GTK_WINDOW (dialog), GTK_WINDOW (fake_parent));
  gtk_window_set_default_size (GTK_WINDOW (dialog), 450, 200);
  gtk_window_set_modal (GTK_WINDOW (dialog), modal);

  adw_message_dialog_add_responses (ADW_MESSAGE_DIALOG (dialog),
                                    "deny", deny_label,
                                    "grant", grant_label,
                                    NULL);

  adw_message_dialog_set_default_response (ADW_MESSAGE_DIALOG (dialog), "deny");

  extra = adw_preferences_group_new ();
  adw_message_dialog_set_extra_child (ADW_MESSAGE_DIALOG (dialog), extra);

  if (choices)
    {
      *choice_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      for (size_t i = 0; i < g_variant_n_children (choices); i++)
        add_choice (extra, g_variant_get_child_value (choices, i), *choice_table);
    }

  if (!g_str_equal (body, ""))
    {
      GtkWidget *body_label;

      body_label = gtk_label_new (body);
      gtk_widget_set_halign (body_label, GTK_ALIGN_CENTER);
      gtk_label_set_justify (GTK_LABEL (body_label), GTK_JUSTIFY_CENTER);
      gtk_label_set_xalign (GTK_LABEL (body_label), 0.5);
      gtk_label_set_wrap (GTK_LABEL (body_label), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (body_label), 50);
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (extra), body_label);
    }

  window_group = gtk_window_group_new ();
  gtk_window_group_add_window (window_group, GTK_WINDOW (dialog));

  gtk_widget_realize (dialog);

  gxdp_external_window_set_parent_of (parent, gtk_native_get_surface (GTK_NATIVE (dialog)));

  gtk_window_present (GTK_WINDOW (dialog));

  return dialog;
}

static void
shell_access_dialog_ready_cb (GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  GDBusMethodInvocation *invocation = user_data;
  g_autoptr(GVariant) params = NULL;
  g_autoptr(GError) error = NULL;

  params = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source_object),
                                          result,
                                          &error);

  if (error)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    g_dbus_method_invocation_return_value (invocation, params);
}

static gboolean
handle_access_dialog (XdpImplAccess         *object,
                      GDBusMethodInvocation *invocation,
                      const char            *arg_handle,
                      const char            *arg_app_id,
                      const char            *arg_parent_window,
                      const char            *arg_title,
                      const char            *arg_subtitle,
                      const char            *arg_body,
                      GVariant              *arg_options)
{
  GxdpExternalWindow *external_parent = NULL;

 if (arg_parent_window)
    {
      external_parent = gxdp_external_window_new_from_handle (arg_parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   arg_parent_window);
    }

  if (external_parent != NULL) /* use window-modal dialog */
    {
      g_autoptr(Request) request = NULL;
      AccessDialogHandle *handle;
      GHashTable *choice_table = NULL;
      GtkWidget *dialog = NULL;
      const char *sender;

      sender = g_dbus_method_invocation_get_sender (invocation);
      request = request_new (sender, arg_app_id, arg_handle);

      dialog = create_access_dialog (external_parent,
                                     arg_title,
                                     arg_subtitle,
                                     arg_body,
                                     arg_options,
                                     &choice_table);

      handle = g_new0 (AccessDialogHandle, 1);
      handle->impl = object;
      handle->invocation = invocation;
      handle->request = g_object_ref (request);
      handle->dialog = g_object_ref (dialog);
      handle->external_parent = external_parent;
      handle->choices = choice_table;

      g_signal_connect (dialog, "response", G_CALLBACK (access_dialog_response), handle);

      request_export (request, g_dbus_method_invocation_get_connection (invocation));
    }
  else /* otherwise delegate to gnome-shell */
    {
      GDBusConnection *conn;

      conn = g_dbus_method_invocation_get_connection (invocation);
      g_dbus_connection_call (conn,
                              "org.gnome.Shell",
                              DESKTOP_PORTAL_OBJECT_PATH,
                              "org.freedesktop.impl.portal.Access",
                              "AccessDialog",
                              g_variant_new ("(osssss@a{sv})",
                                             arg_handle,
                                             arg_app_id,
                                             arg_parent_window,
                                             arg_title,
                                             arg_subtitle,
                                             arg_body,
                                             arg_options),
                              NULL,
                              G_DBUS_CALL_FLAGS_NONE,
                              G_MAXINT,
                              NULL,
                              shell_access_dialog_ready_cb,
                              invocation);
    }

  return TRUE;
}

gboolean
access_init (GDBusConnection *bus,
             GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_access_skeleton_new ());

  g_signal_connect (helper, "handle-access-dialog", G_CALLBACK (handle_access_dialog), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
