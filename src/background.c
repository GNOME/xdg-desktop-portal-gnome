#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <gtk/gtk.h>

#include <glib/gi18n.h>
#include <gio/gdesktopappinfo.h>

#include "xdg-desktop-portal-dbus.h"
#include "shell-dbus.h"

#include "background.h"
#include "request.h"
#include "utils.h"

static OrgGnomeShellIntrospect *shell;

typedef enum { BACKGROUND, RUNNING, ACTIVE } AppState;

static GVariant *app_state;

static const char *
app_state_to_str (AppState state)
{
  switch (state)
    {
    case BACKGROUND:
      return "BACKGROUND";
    case RUNNING:
      return "RUNNING";
    case ACTIVE:
      return "ACTIVE";
    }
  g_assert_not_reached ();
}

static GVariant *
get_app_state (void)
{
  g_autoptr(GVariant) apps = NULL;
  g_autoptr(GHashTable) app_states = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GVariantBuilder builder;
  GHashTableIter iter;
  const char *key;
  gpointer value;

  if (!org_gnome_shell_introspect_call_get_running_applications_sync (shell, &apps, NULL, NULL))
    {
      return NULL;
    }

  if (apps)
    {
      g_autoptr(GVariantIter) iter = g_variant_iter_new (apps);
      const char *app_id = NULL;
      GVariant *dict;

      while (g_variant_iter_loop (iter, "{&s@a{sv}}", &app_id, &dict))
        {
          const char *sandboxed_app_id = NULL;
          g_autofree const char **seats = NULL;
          char *app;
          AppState state = BACKGROUND;

          g_variant_lookup (dict, "sandboxed-app-id", "&s", &sandboxed_app_id);
          g_variant_lookup (dict, "active-on-seats", "^a&s", &seats);

          /* See https://gitlab.gnome.org/GNOME/gnome-shell/issues/1289 */
          if (sandboxed_app_id == NULL)
            continue;

          app = g_strdup (sandboxed_app_id);
          state = GPOINTER_TO_INT (g_hash_table_lookup (app_states, app));
          state = MAX (state, RUNNING);
          if (seats != NULL)
            state = MAX (state, ACTIVE);

          g_debug ("\tUpdating app '%s' state to %s", app, app_state_to_str (state));

          g_hash_table_insert (app_states, app, GINT_TO_POINTER (state));
        }
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  g_hash_table_iter_init (&iter, app_states);
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, (gpointer *)&value))
    g_variant_builder_add (&builder, "{sv}", key, g_variant_new_uint32 (GPOINTER_TO_UINT (value)));

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static gboolean
handle_get_app_state (XdpImplBackground *object,
                      GDBusMethodInvocation *invocation)
{
  g_debug ("background: handle GetAppState");

  if (app_state == NULL)
    app_state = get_app_state ();

  if (app_state == NULL)
    g_dbus_method_invocation_return_error (invocation,
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                           "Could not get window list");
  else
    xdp_impl_background_complete_get_app_state (object, invocation, app_state);

  return TRUE;
}

typedef enum {
  FORBID = 0,
  ALLOW  = 1,
  IGNORE = 2
} NotifyResult;

static gboolean
handle_notify_background (XdpImplBackground *object,
                          GDBusMethodInvocation *invocation,
                          const char *arg_handle,
                          const char *arg_app_id,
                          const char *arg_name)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&opt_builder, "{sv}", "result", g_variant_new_uint32 (ALLOW));

  xdp_impl_background_complete_notify_background (object,
                                                  invocation,
                                                  2,
                                                  g_variant_builder_end (&opt_builder));

  return TRUE;
}

static gboolean
needs_quoting (const char *arg)
{
  while (*arg != 0)
    {
      char c = *arg;
      if (!g_ascii_isalnum (c) &&
          !(c == '-' || c == '/' || c == '~' ||
            c == ':' || c == '.' || c == '_' ||
            c == '=' || c == '@'))
        return TRUE;
      arg++;
    }
  return FALSE;
}

char *
flatpak_quote_argv (const char *argv[],
                    gssize      len)
{
  GString *res = g_string_new ("");
  int i;

  if (len == -1)
    len = g_strv_length ((char **) argv);

  for (i = 0; i < len; i++)
    {
      if (i != 0)
        g_string_append_c (res, ' ');

      if (needs_quoting (argv[i]))
        {
          g_autofree char *quoted = g_shell_quote (argv[i]);
          g_string_append (res, quoted);
        }
      else
        g_string_append (res, argv[i]);
    }

  return g_string_free (res, FALSE);
}

typedef enum {
  AUTOSTART_FLAGS_NONE        = 0,
  AUTOSTART_FLAGS_ACTIVATABLE = 1 << 0,
} AutostartFlags;

static gboolean
handle_enable_autostart (XdpImplBackground *object,
                         GDBusMethodInvocation *invocation,
                         const char *arg_app_id,
                         gboolean arg_enable,
                         const char * const *arg_commandline,
                         guint arg_flags)
{
  gboolean result = FALSE;
  g_autofree char *dir = NULL;
  g_autofree char *desktop_id = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *commandline = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GDesktopAppInfo) app_info = NULL;
  const char *name;

  g_debug ("background: handle EnableAutostart");

  desktop_id = g_strconcat (arg_app_id, ".desktop", NULL);
  dir = g_build_filename (g_get_user_config_dir (), "autostart", NULL);
  path = g_build_filename (dir, desktop_id, NULL);

  if (!arg_enable)
    {
      unlink (path);
      g_debug ("Removed %s", path);
      goto out;
    }

  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      g_warning ("Failed to create dirs %s", dir);
      goto out;
    }

  name = arg_app_id;
  app_info = g_desktop_app_info_new (desktop_id);
  commandline = flatpak_quote_argv ((const char **)arg_commandline, -1);

  if (app_info)
    name = g_app_info_get_name (G_APP_INFO (app_info));

  keyfile = g_key_file_new ();

  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         G_KEY_FILE_DESKTOP_KEY_TYPE,
                         "Application");
  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         G_KEY_FILE_DESKTOP_KEY_NAME,
                         name);
  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         G_KEY_FILE_DESKTOP_KEY_EXEC,
                         commandline);
  if (arg_flags & AUTOSTART_FLAGS_ACTIVATABLE)
    g_key_file_set_boolean (keyfile,
                            G_KEY_FILE_DESKTOP_GROUP,
                            G_KEY_FILE_DESKTOP_KEY_DBUS_ACTIVATABLE,
                            TRUE);

  if (app_info && g_desktop_app_info_has_key (app_info, "X-SnapInstanceName"))
    {
      g_autofree char *instance = NULL;
      g_autofree char *app = NULL;

      instance = g_desktop_app_info_get_string (app_info, "X-SnapInstanceName");
      g_key_file_set_string (keyfile,
                             G_KEY_FILE_DESKTOP_GROUP,
                             "X-SnapInstanceName",
                             instance);

      app = g_desktop_app_info_get_string (app_info, "X-SnapAppName");
      g_key_file_set_string (keyfile,
                             G_KEY_FILE_DESKTOP_GROUP,
                             "X-SnapAppName",
                             app);
    }
  else
    {
      g_key_file_set_string (keyfile,
                             G_KEY_FILE_DESKTOP_GROUP,
                             "X-Flatpak",
                             arg_app_id);
    }

  g_key_file_set_string (keyfile,
                         G_KEY_FILE_DESKTOP_GROUP,
                         "X-XDPG-Autostart",
                         arg_app_id);

  if (!g_key_file_save_to_file (keyfile, path, &error))
    {
      g_warning ("Failed to save %s: %s", path, error->message);
      goto out;
    }

  g_debug ("Wrote autostart file %s", path);

  result = TRUE;

out:
  xdp_impl_background_complete_enable_autostart (object, invocation, result);

  return TRUE;
}

static gboolean
compare_app_states (GVariant *a, GVariant *b)
{
  GVariantIter *iter;
  const char *app_id = NULL;
  GVariant *value = NULL;
  gboolean changed;

  changed = FALSE;

  iter = g_variant_iter_new (a);
  while (!changed && g_variant_iter_next (iter, "{&sv}", &app_id, &value))
    {
      guint v1, v2;

      g_variant_get (value, "u", &v1);

      if (!g_variant_lookup (b, app_id, "u", &v2))
        v2 = BACKGROUND;

      if ((v1 == BACKGROUND && v2 != BACKGROUND) ||
          (v1 != BACKGROUND && v2 == BACKGROUND))
        {
          g_debug ("App %s changed state: %s != %s", app_id,
                   app_state_to_str (v1), app_state_to_str (v2));
          changed = TRUE;
        }
      g_clear_pointer (&value, g_variant_unref);
    }
  g_variant_iter_free (iter);

  return changed;
}

static void
running_applications_changed (OrgGnomeShellIntrospect *object,
                              GDBusInterfaceSkeleton *helper)
{
  GVariant *new_app_state;
  gboolean changed = FALSE;

  g_debug ("Received running-applications-changed from gnome-shell");

  new_app_state = get_app_state ();

  if (app_state == NULL || new_app_state == NULL)
    changed = TRUE;
  else if (compare_app_states (app_state, new_app_state) ||
           compare_app_states (new_app_state, app_state))
    changed = TRUE;

  if (app_state)
    g_variant_unref (app_state);
  app_state = new_app_state;

  if (changed)
    {
      g_debug ("Emitting RunningApplicationsChanged");
      xdp_impl_background_emit_running_applications_changed (XDP_IMPL_BACKGROUND (helper));
    }
}

gboolean
background_init (GDBusConnection *bus,
                 GError **error)
{
  GDBusInterfaceSkeleton *helper;

  shell = org_gnome_shell_introspect_proxy_new_sync (bus,
                                                     G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                     "org.gnome.Shell",
                                                     "/org/gnome/Shell/Introspect",
                                                     NULL,
                                                     NULL);
  helper = G_DBUS_INTERFACE_SKELETON (xdp_impl_background_skeleton_new ());

  g_signal_connect (shell, "running-applications-changed", G_CALLBACK (running_applications_changed), helper);

  g_signal_connect (helper, "handle-get-app-state", G_CALLBACK (handle_get_app_state), NULL);
  g_signal_connect (helper, "handle-notify-background", G_CALLBACK (handle_notify_background), NULL);
  g_signal_connect (helper, "handle-enable-autostart", G_CALLBACK (handle_enable_autostart), NULL);

  if (!g_dbus_interface_skeleton_export (helper,
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (helper)->name);

  return TRUE;
}
