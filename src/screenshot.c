#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gtk/gtk.h>
#include <gxdp.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include "shell-dbus.h"

#include "screenshotdialog.h"
#include "screenshot.h"
#include "request.h"
#include "utils.h"

static OrgGnomeShellScreenshot *shell;

typedef struct {
  XdpImplScreenshot *impl;
  GDBusMethodInvocation *invocation;
  Request *request;

  GtkWindow *dialog;
  GxdpExternalWindow *external_parent;

  int response;
  char *uri;
  double red, green, blue;
  const char *retval;
} ScreenshotDialogHandle;

static void
screenshot_dialog_handle_free (gpointer data)
{
  ScreenshotDialogHandle *handle = data;

  g_clear_object (&handle->external_parent);
  g_clear_object (&handle->request);
  g_clear_pointer (&handle->uri, g_free);

  g_free (handle);
}

static void
screenshot_dialog_handle_close (ScreenshotDialogHandle *handle)
{
  g_clear_pointer (&handle->dialog, gtk_window_destroy);
  screenshot_dialog_handle_free (handle);
}

static void
send_response (ScreenshotDialogHandle *handle)
{
  if (handle->request->exported)
    request_unexport (handle->request);

  if (strcmp (handle->retval, "url") == 0)
    {
      GVariantBuilder opt_builder;

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&opt_builder, "{sv}", "uri", g_variant_new_string (handle->uri ? handle->uri : ""));

      xdp_impl_screenshot_complete_screenshot (handle->impl,
                                               handle->invocation,
                                               handle->response,
                                               g_variant_builder_end (&opt_builder));
    }
  else
    {
      GVariantBuilder opt_builder;

      g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
      g_variant_builder_add (&opt_builder, "{sv}", "color", g_variant_new ("(ddd)",
                                                                           handle->red,
                                                                           handle->green,
                                                                           handle->blue));

      xdp_impl_screenshot_complete_pick_color (handle->impl,
                                               handle->invocation,
                                               handle->response,
                                               g_variant_builder_end (&opt_builder));

    }

  screenshot_dialog_handle_close (handle);
}

static void
screenshot_dialog_done (GtkWidget *widget,
                        int response,
                        const char *filename,
                        gpointer user_data)
{
  ScreenshotDialogHandle *handle = user_data;

  switch (response)
    {
    default:
      g_warning ("Unexpected response: %d", response);
      G_GNUC_FALLTHROUGH;

    case GTK_RESPONSE_DELETE_EVENT:
      handle->response = 2;
      break;

    case GTK_RESPONSE_CANCEL:
      handle->response = 1;
      break;

    case GTK_RESPONSE_OK:
      handle->uri = g_filename_to_uri (filename, NULL, NULL);
      handle->response = 0;
      break;
    }

  send_response (handle);
}

static gboolean
handle_close (XdpImplRequest *object,
              GDBusMethodInvocation *invocation,
              ScreenshotDialogHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_screenshot_complete_screenshot (handle->impl,
                                           handle->invocation,
                                           2,
                                           g_variant_builder_end (&opt_builder));
  screenshot_dialog_handle_close (handle);
  return FALSE;
}

static void
interactive_screenshot_taken_cb (GObject      *object,
                                 GAsyncResult *result,
                                 gpointer      data)
{
  ScreenshotDialogHandle *handle = data;
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = NULL;
  gboolean success;

  handle->response = 0;

  org_gnome_shell_screenshot_call_interactive_screenshot_finish (shell,
                                                                 &success,
                                                                 &uri,
                                                                 result,
                                                                 &error);

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("InteractiveScreenshot cancelled");
          handle->response = 1;
        }
      else
        {
          g_warning ("InteractiveScreenshot failed: %s", error->message);
          handle->response = 2;
        }
    }
  else if (!success)
    {
      g_warning ("InteractiveScreenshot didn't return a file");
      handle->response = 2;
    }
  else
    {
      handle->uri = g_steal_pointer (&uri);
      handle->response = 0;
    }

  send_response (handle);
}

static gboolean
handle_screenshot (XdpImplScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_handle,
                   const char *arg_app_id,
                   const char *arg_parent_window,
                   GVariant *arg_options)
{
  g_autoptr(Request) request = NULL;
  const char *sender;
  ScreenshotDialogHandle *handle;
  gboolean interactive;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);

  if (!g_variant_lookup (arg_options, "interactive", "b", &interactive))
    interactive = FALSE;

  handle = g_new0 (ScreenshotDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->retval = "url";

  if (interactive)
    {
      org_gnome_shell_screenshot_call_interactive_screenshot (shell, NULL,
                                                              interactive_screenshot_taken_cb,
                                                              handle);
    }
  else
    {
      GxdpExternalWindow *external_parent = NULL;
      GdkSurface *surface;
      GtkWindow *dialog;
      GtkWidget *fake_parent;
      gboolean permission_store_checked;
      gboolean modal;

      if (!g_variant_lookup (arg_options, "modal", "b", &modal))
        modal = TRUE;
      if (!g_variant_lookup (arg_options, "permission_store_checked", "b", &permission_store_checked))
        permission_store_checked = FALSE;

      if (arg_parent_window)
        {
          external_parent = gxdp_external_window_new_from_handle (arg_parent_window);
          if (!external_parent)
            g_warning ("Failed to associate portal window with parent window '%s'",
                       arg_parent_window);
        }

      fake_parent = g_object_new (GTK_TYPE_WINDOW, NULL);
      g_object_ref_sink (fake_parent);

      dialog = GTK_WINDOW (screenshot_dialog_new (arg_app_id,
                                                  permission_store_checked,
                                                  shell));
      gtk_window_set_transient_for (dialog, GTK_WINDOW (fake_parent));
      gtk_window_set_modal (dialog, modal);

      handle->dialog = g_object_ref_sink (dialog);
      handle->external_parent = external_parent;

      g_signal_connect (dialog, "done", G_CALLBACK (screenshot_dialog_done), handle);

      gtk_widget_realize (GTK_WIDGET (dialog));

      surface = gtk_native_get_surface (GTK_NATIVE (dialog));
      if (external_parent)
        gxdp_external_window_set_parent_of (external_parent, surface);
    }

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  return TRUE;
}

static void
color_picked (GObject *object,
              GAsyncResult *res,
              gpointer data)
{
  ScreenshotDialogHandle *handle = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) result = NULL;

  handle->response = 0;
  handle->red = handle->green = handle->blue = 0;

  if (!org_gnome_shell_screenshot_call_pick_color_finish (shell, &result, res, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("PickColor cancelled");
          handle->response = 1;
        }
      else
        {
          g_warning ("PickColor failed: %s", error->message);
          handle->response = 2;
        }
    }
  else if (!g_variant_lookup (result, "color", "(ddd)",
                              &handle->red,
                              &handle->green,
                              &handle->blue))
    {
      g_warning ("PickColor didn't return a color");
      handle->response = 2;
    }

  send_response (handle);
}

static gboolean
handle_pick_color (XdpImplScreenshot *object,
                   GDBusMethodInvocation *invocation,
                   const char *arg_handle,
                   const char *arg_app_id,
                   const char *arg_parent_window,
                   GVariant *arg_options)
{
  g_autoptr(Request) request = NULL;
  const char *sender;
  ScreenshotDialogHandle *handle;

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new0 (ScreenshotDialogHandle, 1);
  handle->impl = object;
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->dialog = NULL;
  handle->external_parent = NULL;
  handle->retval = "color";
  handle->response = 2;

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  org_gnome_shell_screenshot_call_pick_color (shell, NULL, color_picked, handle);

  return TRUE;
}

gboolean
screenshot_init (GDBusConnection *bus,
                 GError **error)
{
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_screenshot_skeleton_new ());
  xdp_impl_screenshot_set_version (XDP_IMPL_SCREENSHOT (helper), 2);

  g_signal_connect (helper, "handle-screenshot", G_CALLBACK (handle_screenshot), NULL);
  g_signal_connect (helper, "handle-pick-color", G_CALLBACK (handle_pick_color), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  shell = org_gnome_shell_screenshot_proxy_new_sync (bus,
                                                     G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                     "org.gnome.Shell.Screenshot",
                                                     "/org/gnome/Shell/Screenshot",
                                                     NULL,
                                                     error);
  if (shell == NULL)
    return FALSE;

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (shell), G_MAXINT);

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
