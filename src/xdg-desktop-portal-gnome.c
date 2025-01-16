/*
 * Copyright © 2016 Red Hat, Inc
 * Copyright © 2021 Endless OS Foundation
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
 *       Georges Basile Stavracas Neto <georges@endlessos.org>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#define _GNU_SOURCE 1

#include "config.h"

#include <errno.h>
#include <locale.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <adwaita.h>
#include <gxdp.h>

#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gunixfdlist.h>

#include <glib-unix.h>
#include <glib/gi18n.h>
#include <locale.h>

#include "xdg-desktop-portal-dbus.h"

#include "access.h"
#include "account.h"
#include "appchooser.h"
#include "background.h"
#include "clipboard.h"
#include "filechooser.h"
#include "globalshortcuts.h"
#include "lockdown.h"
#include "print.h"
#include "screenshot.h"
#include "screencast.h"
#include "remotedesktop.h"
#include "inputcapture.h"
#include "request.h"
#include "settings.h"
#include "wallpaper.h"
#include "dynamic-launcher.h"
#include "notification.h"
#include "usb.h"

static GMainLoop *loop = NULL;
static GHashTable *outstanding_handles = NULL;

static gboolean opt_verbose;
static gboolean opt_replace;
static gboolean show_version;
static gboolean settings_only = FALSE;

static GOptionEntry entries[] = {
  { "verbose", 'v', 0, G_OPTION_ARG_NONE, &opt_verbose, "Print debug information during command processing", NULL },
  { "replace", 'r', 0, G_OPTION_ARG_NONE, &opt_replace, "Replace a running instance", NULL },

  { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
  { NULL }
};

static void
message_handler (const gchar *log_domain,
                 GLogLevelFlags log_level,
                 const gchar *message,
                 gpointer user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    printf ("XDP: %s\n", message);
  else
    printf ("%s: %s\n", g_get_prgname (), message);
}

static void
printerr_handler (const gchar *string)
{
  int is_tty = isatty (1);
  const char *prefix = "";
  const char *suffix = "";
  if (is_tty)
    {
      prefix = "\x1b[31m\x1b[1m"; /* red, bold */
      suffix = "\x1b[22m\x1b[0m"; /* bold off, color reset */
    }
  fprintf (stderr, "%serror: %s%s\n", prefix, suffix, string);
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  if (!settings_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (settings_only)
    return;

  if (!file_chooser_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!access_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!account_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!app_chooser_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!background_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!lockdown_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!print_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!screenshot_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!screen_cast_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!remote_desktop_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!clipboard_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!input_capture_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!wallpaper_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!dynamic_launcher_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!notification_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!usb_init (connection, &error))
    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }

  if (!global_shortcuts_init (connection, &error))

    {
      g_warning ("error: %s\n", error->message);
      g_clear_error (&error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("org.freedesktop.impl.portal.desktop.gnome acquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("name lost");
  g_main_loop_quit (loop);
}

static gboolean
signal_handler_cb (gpointer user_data)
{
  g_main_loop_quit (loop);
  g_debug ("Terminated with signal.");
  return G_SOURCE_REMOVE;
}

int
main (int argc, char *argv[])
{
  guint owner_id;
  g_autoptr(GError) error = NULL;
  GDBusConnection  *session_bus;
  g_autoptr(GSource) signal_handler_source = NULL;
  g_autoptr(GOptionContext) context = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL)
    {
      g_printerr ("No session bus: %s\n", error->message);
      return 2;
    }

  context = g_option_context_new ("- portal backends");
  g_option_context_set_summary (context,
      "A backend implementation for xdg-desktop-portal.");
  g_option_context_set_description (context,
      "xdg-desktop-portal-gnome provides D-Bus interfaces that\n"
      "are used by xdg-desktop-portal to implement portals\n"
      "\n"
      "Documentation for the available D-Bus interfaces can be found at\n"
      "https://flatpak.github.io/xdg-desktop-portal/portal-docs.html\n"
      "\n"
      "Please report issues at https://gitlab.gnome.org/GNOME/xdg-desktop-portal-gnome/issues");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  if (!gxdp_init_gtk (GXDP_SERVICE_CLIENT_TYPE_PORTAL_BACKEND, &error))
    {
      g_debug ("Failed to initialize display server connection: %s",
               error->message);
      g_clear_error (&error);
      g_printerr ("Non-compatible display server, exposing settings only.\n");
      settings_only = TRUE;
    }

  g_set_printerr_handler (printerr_handler);

  if (opt_verbose)
    g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  g_set_prgname ("xdg-desktop-portal-gnome");

  loop = g_main_loop_new (NULL, FALSE);

  /* Setup a signal handler so that we can quit cleanly.
   * This is useful for triggering asan.
   */
  signal_handler_source = g_unix_signal_source_new (SIGHUP);
  g_source_set_callback (signal_handler_source, G_SOURCE_FUNC (signal_handler_cb), NULL, NULL);
  g_source_attach (signal_handler_source, g_main_loop_get_context (loop));

  outstanding_handles = g_hash_table_new (g_str_hash, g_str_equal);

  owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             "org.freedesktop.impl.portal.desktop.gnome",
                             G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | (opt_replace ? G_BUS_NAME_OWNER_FLAGS_REPLACE : 0),
                             on_bus_acquired,
                             on_name_acquired,
                             on_name_lost,
                             NULL,
                             NULL);

  if (!settings_only)
    adw_init ();

  g_main_loop_run (loop);

  g_bus_unown_name (owner_id);

  return 0;
}
