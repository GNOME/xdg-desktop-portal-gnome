#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gtk/gtk.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "xdg-desktop-portal-dbus.h"
#include "shell-dbus.h"

#include "notification.h"
#include "request.h"
#include "utils.h"

static OrgGtkNotifications *gtk_notifications;

static void
notification_added (GObject      *source,
                    GAsyncResult *result,
                    gpointer      data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;

  if (!org_gtk_notifications_call_add_notification_finish (gtk_notifications, result, &error))
    g_warning ("Error from gnome-shell: %s", error->message);
}

static char *
app_path_for_id (const gchar *app_id)
{
  char *path;
  gint i;

  path = g_strconcat ("/", app_id, NULL);
  for (i = 0; path[i]; i++)
    {
      if (path[i] == '.')
        path[i] = '/';
      if (path[i] == '-')
        path[i] = '_';
    }

  return path;
}

static gboolean
handle_add_notification (XdpImplNotification *object,
                         GDBusMethodInvocation *invocation,
                         GUnixFDList *fds,
                         const gchar *arg_app_id,
                         const gchar *arg_id,
                         GVariant *arg_notification)
{
  org_gtk_notifications_call_add_notification (gtk_notifications,
                                               arg_app_id,
                                               arg_id,
                                               arg_notification,
                                               NULL,
                                               notification_added,
                                               NULL);

  xdp_impl_notification_complete_add_notification (object, invocation, NULL);

  return TRUE;
}

static gboolean
handle_remove_notification (XdpImplNotification *object,
                            GDBusMethodInvocation *invocation,
                            const gchar *arg_app_id,
                            const gchar *arg_id)
{
  org_gtk_notifications_call_remove_notification (gtk_notifications,
                                                  arg_app_id,
                                                  arg_id,
                                                  NULL,
                                                  NULL,
                                                  NULL);


  xdp_impl_notification_complete_remove_notification (object, invocation);

  return TRUE;
}

static void
handle_action_invoked (OrgGtkNotifications *object,
                       const gchar         *arg_app_id,
                       const gchar         *arg_notification_id,
                       const gchar         *arg_name,
                       GVariant            *arg_parameter,
                       GVariant            *arg_platform_data)
{
  GDBusConnection *connection = NULL;
  g_autofree char *object_path = NULL;
  g_autoptr(GVariant) activation_token = NULL;
  GVariantBuilder pdata;

  connection = g_dbus_proxy_get_connection (G_DBUS_PROXY (object));
  object_path = app_path_for_id (arg_app_id);
  g_variant_builder_init (&pdata, G_VARIANT_TYPE_VARDICT);

  activation_token = g_variant_lookup_value (arg_platform_data, "activation-token", G_VARIANT_TYPE_STRING);

  if (activation_token)
    {
      /* Used by `GTK` < 4.10 */
      g_variant_builder_add (&pdata, "{sv}",
                             "desktop-startup-id", activation_token);
      /* Used by `GTK` and `QT` */
      g_variant_builder_add (&pdata, "{sv}",
                             "activation-token", activation_token);
    }

  g_dbus_connection_call (connection,
                          arg_app_id,
                          object_path,
                          "org.freedesktop.Application",
                          "Activate",
                          g_variant_new ("(@a{sv})",
                                         g_variant_builder_end (&pdata)),
                          NULL,
                          G_DBUS_CALL_FLAGS_NONE,
                          -1, NULL, NULL, NULL);

  g_dbus_connection_emit_signal (connection,
                                 NULL,
                                 "/org/freedesktop/portal/desktop",
                                 "org.freedesktop.impl.portal.Notification",
                                 "ActionInvoked",
                                 g_variant_new ("(sss@av)",
                                                arg_app_id,
                                                arg_notification_id,
                                                arg_name,
                                                arg_parameter),
                                 NULL);
}

gboolean
notification_init (GDBusConnection *bus,
                   GError **error)
{
  GDBusInterfaceSkeleton *helper;

  gtk_notifications = org_gtk_notifications_proxy_new_sync (bus,
                                                            G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                            "org.gtk.Notifications",
                                                            "/org/gtk/Notifications",
                                                            NULL,
                                                            NULL);

  g_signal_connect (gtk_notifications, "action-invoked", G_CALLBACK (handle_action_invoked), NULL);

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_notification_skeleton_new ());

  g_signal_connect (helper, "handle-add-notification", G_CALLBACK (handle_add_notification), NULL);
  g_signal_connect (helper, "handle-remove-notification", G_CALLBACK (handle_remove_notification), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
