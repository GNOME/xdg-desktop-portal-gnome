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

static gboolean
handle_add_notification (XdpImplNotification *object,
                         GDBusMethodInvocation *invocation,
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

  xdp_impl_notification_complete_add_notification (object, invocation);

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
