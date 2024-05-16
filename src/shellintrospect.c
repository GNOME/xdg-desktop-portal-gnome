/*
 * Copyright © 2019 Alberto Fanjul <albfan@gnome.org>
 * Copyright © 2019 Red Hat, Inc
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

#include "shell-dbus.h"
#include "shellintrospect.h"

struct _ShellWindow
{
  GObject parent;

  uint64_t id;
  char *title;
  char *app_id;
};

struct _ShellIntrospect
{
  GObject parent;

  guint shell_introspect_watch_name_id;
  GCancellable *cancellable;

  OrgGnomeShellIntrospect *proxy;

  unsigned int version;


  int num_listeners;
  GListStore *windows;
  gboolean initialized;

  gboolean animations_enabled;
  gboolean animations_enabled_valid;
};

G_DEFINE_TYPE (ShellIntrospect, shell_introspect, G_TYPE_OBJECT)

enum
{
  WINDOWS_CHANGED,
  ANIMATIONS_ENABLED_CHANGED,

  N_SIGNALS
};

static guint signals[N_SIGNALS];

static ShellIntrospect *_shell_introspect;


struct _WindowClass
{
  GObjectClass parent;
};

G_DEFINE_TYPE (ShellWindow, shell_window, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_ID,
  PROP_TITLE,
  PROP_APP_ID,
  N_PROPS
};

GParamSpec *properties[N_PROPS] = {NULL, };

static void
shell_window_init (ShellWindow *obj)
{
}

static void
shell_window_dispose (GObject *obj)
{
  ShellWindow *self = SHELL_WINDOW (obj);

  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->app_id, g_free);
}

static void
shell_window_set_title (ShellWindow *window,
                        const char  *title)
{
  g_clear_pointer (&window->title, g_free);

  if (title == NULL)
    window->title = g_strdup ("");
  else
    window->title = g_markup_escape_text (title, -1);

  g_object_notify (G_OBJECT (window), "title");
}

static void
shell_window_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  ShellWindow *window = SHELL_WINDOW (object);

  switch (property_id)
    {
    case PROP_ID:
      g_value_set_uint64 (value, window->id);
      break;

    case PROP_TITLE:
      g_value_set_string (value, window->title);
      break;

    case PROP_APP_ID:
      g_value_set_string (value, window->app_id);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
shell_window_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  ShellWindow *window = SHELL_WINDOW (object);

  switch (property_id)
    {
    case PROP_ID:
      window->id = g_value_get_uint64 (value);
      break;

    case PROP_TITLE:
      shell_window_set_title (window, g_value_get_string (value));
      break;

    case PROP_APP_ID:
      window->app_id = g_strdup (g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}


static void
shell_window_class_init (ShellWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = shell_window_dispose;
  object_class->get_property = shell_window_get_property;
  object_class->set_property = shell_window_set_property;

  properties[PROP_ID] = g_param_spec_uint64 ("id", NULL, NULL, 0, G_MAXUINT64, 0,
                                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
  properties[PROP_TITLE] = g_param_spec_string ("title", NULL, NULL, "",
                                                G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);
  properties[PROP_APP_ID] = g_param_spec_string ("app_id", NULL, NULL, "",
                                                 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static ShellWindow *
shell_window_new (uint64_t  id,
                  char     *title,
                  char     *app_id)
{
  ShellWindow *new_window;

  new_window = g_object_new (SHELL_TYPE_WINDOW, "id", id,
                             "title", title,
                             "app_id", app_id, NULL);

  return new_window;
}

ShellWindow *
shell_window_dup (ShellWindow *window)
{
  return shell_window_new (window->id, window->title, window->app_id);
}

static ShellWindow *
find_window_from_id (ShellIntrospect *shell_introspect,
                     uint64_t         id,
                     unsigned int    *index)
{
  GListModel *window_list = G_LIST_MODEL (shell_introspect->windows);

  for (unsigned int i = 0; i < g_list_model_get_n_items (window_list); i++)
    {
      ShellWindow *window =
        SHELL_WINDOW (g_list_model_get_item (window_list, i));

      if (id == shell_window_get_id (window))
        {
          if (index)
            *index = i;
          return window;
        }
    }

  return NULL;
}

static void
update_window_list (ShellIntrospect *shell_introspect,
                    GVariant        *windows_variant)
{
  GListModel *window_list = G_LIST_MODEL (shell_introspect->windows);
  g_autoptr(GPtrArray) windows = NULL;
  g_autoptr(GError) error = NULL;
  GVariantIter iter;
  uint64_t id;
  GVariant *params = NULL;
  unsigned int i;
  g_autoptr(GHashTable) old_ids = NULL;
  gpointer key;
  GHashTableIter old_ids_iter;

  old_ids = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);
  for (i = 0; i < g_list_model_get_n_items (window_list); i++)
    {
      ShellWindow *window =
        SHELL_WINDOW (g_list_model_get_item (window_list, i));
      uint64_t id = shell_window_get_id (window);

      g_hash_table_add (old_ids, g_memdup2 (&id, sizeof (id)));
    }

  g_variant_iter_init (&iter, windows_variant);

  windows = g_ptr_array_new_full (g_variant_iter_n_children (&iter),
                                  (GDestroyNotify) g_object_unref);

  while (g_variant_iter_loop (&iter, "{t@a{sv}}", &id, &params))
    {
      g_autofree char *app_id = NULL;
      g_autofree char *title = NULL;
      unsigned int time_since_user_time = UINT_MAX;
      ShellWindow *window;

      g_variant_lookup (params, "app-id", "s", &app_id);
      g_variant_lookup (params, "title", "s", &title);
      g_variant_lookup (params, "time-since-user-time", "u", &time_since_user_time);

      window = find_window_from_id (shell_introspect, id, NULL);
      if (window)
        {
          g_hash_table_remove (old_ids, &id);
          shell_window_set_title (window, title);
        }
      else
        {
          window = shell_window_new (id, title, app_id);
          g_ptr_array_add (windows, window);
        }

      g_clear_pointer (&params, g_variant_unref);
    }

  g_hash_table_iter_init (&old_ids_iter, old_ids);
  while (g_hash_table_iter_next (&old_ids_iter, &key, NULL))
    {
      uint64_t old_id = *(uint64_t *) key;
      unsigned int index;

      if (find_window_from_id (shell_introspect, old_id, &index))
        g_list_store_remove (shell_introspect->windows, index);
    }

  g_list_store_splice (shell_introspect->windows, 0, 0, windows->pdata, windows->len);
  shell_introspect->initialized = TRUE;

  g_signal_emit (shell_introspect, signals[WINDOWS_CHANGED], 0);
}

static void
sync_state (ShellIntrospect *shell_introspect)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) windows_variant = NULL;

  g_cancellable_cancel (shell_introspect->cancellable);
  g_clear_object (&shell_introspect->cancellable);
  shell_introspect->cancellable = g_cancellable_new ();

  if (!org_gnome_shell_introspect_call_get_windows_sync (shell_introspect->proxy,
                                                         &windows_variant,
                                                         shell_introspect->cancellable,
                                                         &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("Failed to get window list: %s", error->message);
      return;
    }

  g_clear_object (&shell_introspect->cancellable);

  update_window_list (shell_introspect, windows_variant);
}

static void
on_windows_changed_cb (GDBusProxy      *proxy,
                       ShellIntrospect *shell_introspect)
{
  if (shell_introspect->num_listeners > 0)
    sync_state (shell_introspect);
}

static void
sync_animations_enabled (ShellIntrospect *shell_introspect)
{
  gboolean animations_enabled;

  animations_enabled =
    org_gnome_shell_introspect_get_animations_enabled (shell_introspect->proxy);
  if (shell_introspect->animations_enabled_valid &&
      animations_enabled == shell_introspect->animations_enabled)
    return;

  shell_introspect->animations_enabled_valid = TRUE;
  shell_introspect->animations_enabled = animations_enabled;
  g_signal_emit (shell_introspect, signals[ANIMATIONS_ENABLED_CHANGED], 0);
}

static void
on_animations_enabled_changed (GObject         *object,
                               GParamSpec      *pspec,
                               ShellIntrospect *shell_introspect)
{
  sync_animations_enabled (shell_introspect);
}

static void
on_shell_introspect_proxy_acquired (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  ShellIntrospect *shell_introspect = user_data;
  OrgGnomeShellIntrospect *proxy;
  g_autoptr(GError) error = NULL;

  proxy = org_gnome_shell_introspect_proxy_new_for_bus_finish (result,
                                                               &error);
  if (!proxy)
    {
      g_warning ("Failed to acquire org.gnome.Shell.Introspect proxy: %s",
                 error->message);
      return;
    }

  shell_introspect->proxy = proxy;

  g_signal_connect (proxy, "windows-changed",
                    G_CALLBACK (on_windows_changed_cb),
                    shell_introspect);

  if (shell_introspect->num_listeners > 0)
    sync_state (shell_introspect);

  shell_introspect->version =
    org_gnome_shell_introspect_get_version (shell_introspect->proxy);

  if (shell_introspect->version >= 2)
    {
      g_signal_connect (proxy, "notify::animations-enabled",
                        G_CALLBACK (on_animations_enabled_changed),
                        shell_introspect);
      sync_animations_enabled (shell_introspect);
    }
}

static void
on_shell_introspect_name_appeared (GDBusConnection *connection,
                                   const char *name,
                                   const char *name_owner,
                                   gpointer user_data)
{
  ShellIntrospect *shell_introspect = user_data;

  org_gnome_shell_introspect_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                "org.gnome.Shell.Introspect",
                                                "/org/gnome/Shell/Introspect",
                                                shell_introspect->cancellable,
                                                on_shell_introspect_proxy_acquired,
                                                shell_introspect);
}

static void
on_shell_introspect_name_vanished (GDBusConnection *connection,
                                   const char *name,
                                   gpointer user_data)
{
  ShellIntrospect *shell_introspect = user_data;

  if (shell_introspect->cancellable)
    {
      g_cancellable_cancel (shell_introspect->cancellable);
      g_clear_object (&shell_introspect->cancellable);
    }
}

static void
shell_introspect_class_init (ShellIntrospectClass *klass)
{
  signals[WINDOWS_CHANGED] = g_signal_new ("windows-changed",
                                           G_TYPE_FROM_CLASS (klass),
                                           G_SIGNAL_RUN_LAST,
                                           0,
                                           NULL, NULL, NULL,
                                           G_TYPE_NONE, 0);
  signals[ANIMATIONS_ENABLED_CHANGED] =
    g_signal_new ("animations-enabled-changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
shell_introspect_init (ShellIntrospect *shell_introspect)
{
  shell_introspect->windows = g_list_store_new (SHELL_TYPE_WINDOW);
}

ShellIntrospect *
shell_introspect_get (void)
{
  ShellIntrospect *shell_introspect;

  if (_shell_introspect)
    return _shell_introspect;

  shell_introspect = g_object_new (shell_introspect_get_type (), NULL);
  shell_introspect->shell_introspect_watch_name_id =
    g_bus_watch_name (G_BUS_TYPE_SESSION,
                      "org.gnome.Shell.Introspect",
                      G_BUS_NAME_WATCHER_FLAGS_NONE,
                      on_shell_introspect_name_appeared,
                      on_shell_introspect_name_vanished,
                      shell_introspect, NULL);
  _shell_introspect = shell_introspect;
  return shell_introspect;
}

GListModel *
shell_introspect_get_windows (ShellIntrospect *shell_introspect)
{
  return G_LIST_MODEL (shell_introspect->windows);
}

void
shell_introspect_ref_listeners (ShellIntrospect *shell_introspect)
{
  shell_introspect->num_listeners++;

  if (shell_introspect->proxy)
    sync_state (shell_introspect);
}

void
shell_introspect_unref_listeners (ShellIntrospect *shell_introspect)
{
  g_return_if_fail (shell_introspect->num_listeners > 0);

  shell_introspect->num_listeners--;
  if (shell_introspect->num_listeners == 0)
    g_list_store_remove_all (shell_introspect->windows);
}

const char *
shell_window_get_title (ShellWindow *window)
{
  return window->title;
}

const char *
shell_window_get_app_id (ShellWindow *window)
{
  return window->app_id;
}

const uint64_t
shell_window_get_id (ShellWindow *window)
{
  return window->id;
}

gboolean
shell_introspect_are_animations_enabled (ShellIntrospect *shell_introspect,
                                         gboolean        *out_animations_enabled)
{
  if (!shell_introspect->animations_enabled_valid)
    return FALSE;

  *out_animations_enabled = shell_introspect->animations_enabled;
  return TRUE;
}

void
shell_introspect_wait_for_windows (ShellIntrospect *shell_introspect)
{
  g_assert (shell_introspect->num_listeners > 0);

  sync_state (shell_introspect);

  while (!shell_introspect->initialized)
    g_main_context_iteration (NULL, TRUE);
}
