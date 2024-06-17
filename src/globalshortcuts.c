#define _GNU_SOURCE 1

#include "config.h"

#include <gio/gio.h>

#include <glib/gi18n.h>

#include "shell-dbus.h"

#include "globalshortcuts.h"
#include "request.h"
#include "utils.h"
#include "session.h"

static OrgGnomeShell *shell;
static OrgGnomeSettingsGlobalShortcutsProvider *settings;
static XdpImplGlobalShortcuts *global_shortcuts;
static OrgGnomeGlobalShortcutsRebind *global_shortcuts_rebind;


typedef struct _GlobalShortcutsSession
{
  Session parent;
  char *app_id;

  gboolean bound;
} GlobalShortcutsSession;

typedef struct _GlobalShortcutsSessionClass
{
  SessionClass parent_class;
} GlobalShortcutsSessionClass;

GType global_shortcuts_session_get_type ();
G_DEFINE_TYPE (GlobalShortcutsSession, global_shortcuts_session, session_get_type ())

static gboolean
is_global_shortcuts_session (Session *session)
{
  return G_TYPE_CHECK_INSTANCE_TYPE (session, global_shortcuts_session_get_type ());
}

static void
global_shortcuts_session_init (GlobalShortcutsSession *session)
{
}

static void
global_shortcuts_session_finalize (GObject *object)
{
  GlobalShortcutsSession *session = (GlobalShortcutsSession *) object;

  g_free (session->app_id);

  G_OBJECT_CLASS (global_shortcuts_session_parent_class)->finalize (object);
}

static void
shell_unbind_shortcuts_done (GObject *source,
                             GAsyncResult *result,
                             gpointer data)
{
  GError *error = NULL;

  if (!org_gnome_shell_call_unbind_shortcuts_finish (shell, result, &error))
    {
      g_debug ("Error from shell UnbindShortcuts: %s", error->message);
      g_error_free (error);
    }
}
static void
global_shortcuts_session_close (Session *session)
{
  GlobalShortcutsSession *shortcuts_session;

  /* When a session is closed, we need to unbind its shortcuts */
  g_debug ("Closing session %s", session_get_id (session));

  if (!is_global_shortcuts_session (session))
    {
      g_warning ("How did the wrong session get here?!");
      return;
    }

  shortcuts_session = (GlobalShortcutsSession *) session;

  if (!shortcuts_session->bound)
    return;

  g_debug ("Unbinding shortcuts of session %s", session_get_id (session));

  org_gnome_shell_call_unbind_shortcuts (shell, session_get_id (session),
                                         NULL,
                                         shell_unbind_shortcuts_done, NULL);
}

static void
global_shortcuts_session_class_init (GlobalShortcutsSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  SessionClass *session_class = XDP_SESSION_CLASS (klass);

  object_class->finalize = global_shortcuts_session_finalize;

  session_class->close = global_shortcuts_session_close;
}

static gboolean
handle_create_session (XdpImplGlobalShortcuts *object,
                       GDBusMethodInvocation  *invocation,
                       const char             *arg_handle,
                       const char             *arg_session_handle,
                       const char             *arg_app_id,
                       GVariant               *arg_options)
{
  const char *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GError) error = NULL;
  int response;
  Session *session;
  GVariantBuilder results_builder;

  session = g_object_new (global_shortcuts_session_get_type (),
                          "id", arg_session_handle,
                          "peer-name", sender,
                          NULL);
  ((GlobalShortcutsSession *) session)->app_id = g_strdup (arg_app_id);

  if (!session_export (session,
                       g_dbus_method_invocation_get_connection (invocation),
                       &error))
    {
      g_clear_object (&session);
      g_warning ("Failed to create global shortcuts session: %s", error->message);
      response = 2;
      goto out;
    }

  g_debug ("creating session %s", session_get_id (session));

  response = 0;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&results_builder, "{sv}",
                         "session_id", g_variant_new_string (session_get_id (session)));
  xdp_impl_global_shortcuts_complete_create_session (object,
                                                     invocation,
                                                     response,
                                                     g_variant_builder_end (&results_builder));

  return TRUE;
}


typedef struct {
  GDBusMethodInvocation *invocation;
  char *session_handle;
  Request *request;
  GVariant *shortcuts;
  gboolean canceled;
} BindShortcutsHandle;

static void
shortcuts_handle_free (BindShortcutsHandle *handle)
{
  g_object_unref (handle->invocation);
  g_free (handle->session_handle);
  if (handle->request)
    {
      if (handle->request->exported)
        request_unexport (handle->request);
      g_object_unref (handle->request);
    }
  g_clear_pointer (&handle->shortcuts, g_variant_unref);
  g_free (handle);
}

static gboolean
on_request_handle_close_cb (XdpImplRequest         *object,
                            GDBusMethodInvocation  *invocation,
                            BindShortcutsHandle    *handle)
{
  g_debug ("Received Close() from the frontend");
  handle->canceled = TRUE;
  return FALSE;
}

static char *
portal_trigger_to_settings (const char *trigger)
{
  char **pieces;
  struct {
    const char *from;
    const char *to;
  } map[] = {
    { "CTRL", "<ctrl>" },
    { "SHIFT", "<shift>" },
    { "ALT", "<alt>" },
    { "NUM", "<num>" },
    { "LOGO", "<logo>" },
  };

  GString *result = g_string_new ("");

  pieces = g_strsplit (trigger, "+", 0);

  for (int i = 0; pieces[i]; i++)
    {
      int j;

      for (j = 0; j < G_N_ELEMENTS (map); j++)
        {
          if (strcmp (pieces[i], map[j].from) == 0)
            {
              g_string_append (result, map[j].to);
              break;
            }
        }
      if (j == G_N_ELEMENTS (map))
        g_string_append (result, pieces[i]);
    }

  g_strfreev (pieces);

  return g_string_free (result, FALSE);
}

static GVariant *
portal_shortcuts_to_settings (GVariant *input)
{
  /* The portal input looks like this:
   *
   * [
   *   ('id1', {'description':       <'the first shortcut'>,
   *            'preferred_trigger': <'CTRL+a'>}),
   *   ('id2', {'description':       <'the second shortcut'>,
   *            'preferred_trigger': <'SHIFT+ALT+Return'>})
   * ]
   *
   * The settings provider input looks like this:
   *
   * [
   *   ('id1', {'description':       <'the first shortcut'>,
   *            'preferred_trigger': <'<ctrl>a'>}),
   *   ('id2', {'description':       <'the second shortcut'>,
   *            'preferred_trigger': <'<shift><alt>Return'>})
   * ]
   */

  GVariantIter iter;
  char *id;
  GVariant *v;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));

  g_variant_iter_init (&iter, input);
  while (g_variant_iter_next (&iter, "(s@a{sv})", &id, &v))
    {
      g_autofree char *description = NULL;
      g_autofree char *preferred_trigger = NULL;
      g_autofree char *settings_trigger = NULL;

      g_variant_lookup (v, "preferred_trigger", "s", &preferred_trigger);

      if (!g_variant_lookup (v, "description", "s", &description))
        {
          description = g_strdup (_("Undocumented shortcut"));
        }

      g_variant_builder_open (&builder, G_VARIANT_TYPE ("(sa{sv})"));
      g_variant_builder_add (&builder, "s", id);
      g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add (&builder, "{sv}", "description",
                             g_variant_new_string (description));
      if (preferred_trigger)
        {
          settings_trigger = portal_trigger_to_settings (preferred_trigger);
          g_variant_builder_add (&builder, "{sv}", "preferred_trigger",
                                 g_variant_new_string (settings_trigger));
        }

      g_variant_builder_close (&builder);
      g_variant_builder_close (&builder);

      g_variant_unref (v);
      g_free (id);
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static char *
find_description (GVariant   *shortcuts,
                  const char *id_in)
{
  GVariantIter iter;
  char *id;
  GVariant *v;

  g_variant_iter_init (&iter, shortcuts);
  while (g_variant_iter_next (&iter, "(s@a{sv})", &id, &v))
    {
      if (strcmp (id, id_in) == 0)
        {
          char *description;

          if (g_variant_lookup (v, "description", "s", &description))
            {
              g_free (id);
              g_variant_unref (v);
              return description;
            }
        }

      g_free (id);
      g_variant_unref (v);
    }

  return NULL;
}

static GVariant *
settings_shortcuts_to_portal (GVariant *input, GVariant *shortcuts)
{
  /* The shell response looks like this:
   *
   * [
   *   ('id1', {'description':          <'the first shortcut'>,
   *            'trigger_description':  <[ 'press control and a', 'press conontrol and b' ]>,}),
   *   ('id2', {'description':          <'the second shortcut'>,
   *            'trigger_description':  <[ 'press control and c' ]>})
   * ]
   *
   * The portal response looks like this:
   *
   * [
   *   ('id1', {'description':         <'the first shortcut'>,
   *            'trigger_description': <'press Control+a or Control+b'>}),
   *   ('id2', {'description':         <'the second shortcut'>,
   *            'shortcuts':           <'press Control+c'>})
   * ]
   */

  GVariantIter iter;
  char *id;
  GVariant *v;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));

  g_variant_iter_init (&iter, input);
  while (g_variant_iter_next (&iter, "(s@a{sv})", &id, &v))
    {
      g_autofree char *description = NULL;
      g_auto(GStrv) trigger_desc = NULL;
      g_autofree char *settings_trigger = NULL;

      if (!g_variant_lookup (v, "trigger_description", "^as", &trigger_desc))
        {
          g_debug ("Ignoring shortcut with id %s, since it does not have a trigger description", id);
          g_variant_unref (v);
          g_free (id);
        }

      if (!g_variant_lookup (v, "description", "s", &description))
        {
          description = find_description (shortcuts, id);
          if (!description)
            description = g_strdup (_("Undocumented shortcut"));
        }

      settings_trigger = g_strjoinv (" or ", (char **)trigger_desc);

      g_variant_builder_open (&builder, G_VARIANT_TYPE ("(sa{sv})"));
      g_variant_builder_add (&builder, "s", id);
      g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));
      if (description)
        {
          g_variant_builder_add (&builder, "{sv}", "description",
                                 g_variant_new_string (description));
        }
      if (trigger_desc)
        {
          g_variant_builder_add (&builder, "{sv}", "trigger_description",
                                 g_variant_new_string (settings_trigger));
        }
      g_variant_builder_close (&builder);
      g_variant_builder_close (&builder);

      g_variant_unref (v);
      g_free (id);
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static void
shell_bind_shortcuts_done (GObject      *source,
                           GAsyncResult *result,
                           gpointer      data)
{
  OrgGnomeShell *proxy = (OrgGnomeShell *) source;
  BindShortcutsHandle *handle = data;
  g_autoptr(GVariant) results = NULL;
  guint changed = FALSE;
  int response;
  GVariantBuilder results_builder;
  g_autoptr(GVariant) shortcuts = NULL;
  g_autoptr(GVariant) portal_shortcuts = NULL;
  g_autoptr(GError) error = NULL;

  if (!org_gnome_shell_call_bind_shortcuts_finish (proxy,
                                                   &results,
                                                   &changed,
                                                   result,
                                                   &error))
    {
      g_debug ("Error from shell BindShortcuts: %s", error->message);
      response = 2;
      goto out;
    }

  shortcuts = g_variant_lookup_value (results, "shortcuts", NULL);
  if (shortcuts == NULL)
    {
      g_debug ("shell BindShortcuts response did not contain shortcuts");
      response = 2;
      goto out;
    }

  response = 0;

  char *s = g_variant_print (shortcuts, FALSE);
  g_debug ("Received from shell BindShortcuts: %s", s);
  g_free (s);

  portal_shortcuts = settings_shortcuts_to_portal (shortcuts, handle->shortcuts);

  if (changed)
    {
      xdp_impl_global_shortcuts_emit_shortcuts_changed (global_shortcuts,
                                                        handle->session_handle,
                                                        portal_shortcuts);
    }

out:
  if (strcmp (g_dbus_method_invocation_get_method_name (handle->invocation),
              "RebindShortcuts") == 0)
    {
      org_gnome_global_shortcuts_rebind_complete_rebind_shortcuts (global_shortcuts_rebind,
                                                                   handle->invocation);
    }
  else
    {
      g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
      if (portal_shortcuts)
        {
          g_variant_builder_add (&results_builder, "{sv}",
                                 "shortcuts", portal_shortcuts);
        }
      xdp_impl_global_shortcuts_complete_bind_shortcuts (global_shortcuts,
                                                         handle->invocation,
                                                         response,
                                                         g_variant_builder_end (&results_builder));
    }

  shortcuts_handle_free (handle);
}

static void
settings_bind_shortcuts_done (GObject      *source,
                              GAsyncResult *result,
                              gpointer      data)
{
  OrgGnomeSettingsGlobalShortcutsProvider *proxy = (OrgGnomeSettingsGlobalShortcutsProvider *) source;
  BindShortcutsHandle *handle = data;
  g_autoptr(GVariant) shortcuts = NULL;
  GVariantBuilder results_builder;
  int response;
  GError *error = NULL;

  if (!org_gnome_settings_global_shortcuts_provider_call_bind_shortcuts_finish (proxy,
                                                                                &shortcuts,
                                                                                result,
                                                                                &error))
    {
      g_debug ("Error from GlobalShortcutsProvider: %s", error->message);
      g_error_free (error);
      response = 2;
      g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
      xdp_impl_global_shortcuts_complete_bind_shortcuts (global_shortcuts,
                                                         handle->invocation,
                                                         response,
                                                         g_variant_builder_end (&results_builder));
      shortcuts_handle_free (handle);
      return;
    }

  if (handle->canceled)
    {
      response = 1;
      g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
      xdp_impl_global_shortcuts_complete_bind_shortcuts (global_shortcuts,
                                                         handle->invocation,
                                                         response,
                                                         g_variant_builder_end (&results_builder));
      shortcuts_handle_free (handle);
      return;
    }

  char *s = g_variant_print (shortcuts, FALSE);
  g_debug ("Received from GlobalShortcutsProvider: %s", s);
  g_free (s);

  org_gnome_shell_call_bind_shortcuts (shell,
                                       handle->session_handle,
                                       shortcuts,
                                       NULL,
                                       shell_bind_shortcuts_done,
                                       handle);
}

static gboolean
handle_bind_shortcuts (XdpImplGlobalShortcuts *object,
                       GDBusMethodInvocation  *invocation,
                       const char             *arg_handle,
                       const char             *arg_session_handle,
                       GVariant               *arg_shortcuts,
                       const char             *arg_parent_window,
                       GVariant               *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  Session *session;
  GlobalShortcutsSession *shortcuts_session;
  GVariantBuilder results_builder;
  int response;
  g_autoptr(GVariant) settings_shortcuts = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to bind shortcuts on non-existing %s", arg_session_handle);
      response = 2;
      goto out;
    }

  if (!is_global_shortcuts_session (session))
    {
      g_warning ("Tried to bind shortcuts on the wrong session type");
      response = 2;
      goto out;
    }

  shortcuts_session = (GlobalShortcutsSession *) session;

  if (shortcuts_session->bound)
    {
      g_warning ("Session already has bound shortcuts");
      response = 2;
      goto out;
    }

  request = request_new (sender, shortcuts_session->app_id, arg_handle);
  request_export (request,
                  g_dbus_method_invocation_get_connection (invocation));

  shortcuts_session->bound = TRUE;

  g_assert (object == global_shortcuts);
  BindShortcutsHandle *handle = g_new0 (BindShortcutsHandle, 1);
  handle->session_handle = g_strdup (arg_session_handle);
  handle->invocation = g_object_ref (invocation);
  handle->request = g_object_ref (request);
  handle->shortcuts = g_variant_ref (arg_shortcuts);

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (on_request_handle_close_cb), handle);

  settings_shortcuts = portal_shortcuts_to_settings (arg_shortcuts);

  char *s = g_variant_print (settings_shortcuts, FALSE);
  g_debug ("Sending to GlobalShortcutsProvider: %s", s);
  g_free (s);

  response = 0;

  org_gnome_settings_global_shortcuts_provider_call_bind_shortcuts (settings,
                                                                    shortcuts_session->app_id,
                                                                    arg_parent_window,
                                                                    settings_shortcuts,
                                                                    NULL,
                                                                    settings_bind_shortcuts_done,
                                                                    handle);

  return TRUE;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_global_shortcuts_complete_bind_shortcuts (object,
                                                     invocation,
                                                     response,
                                                     g_variant_builder_end (&results_builder));
  return TRUE;
}

static void
shell_list_shortcuts_done (GObject      *source,
                           GAsyncResult *result,
                           gpointer      data)
{
  OrgGnomeShell *proxy = (OrgGnomeShell *) source;
  BindShortcutsHandle *handle = data;
  g_autoptr(GVariant) results = NULL;
  int response = 2;
  GVariantBuilder results_builder;
  g_autoptr(GVariant) shortcuts = NULL;
  g_autoptr(GVariant) portal_shortcuts = NULL;
  GError *error = NULL;

  if (!org_gnome_shell_call_list_shortcuts_finish (proxy, &results, result, &error))
    {
      g_debug ("Error from shell ListShortcuts: %s", error->message);
      g_error_free (error);
      response = 2;
      goto out;
    }

  shortcuts = g_variant_lookup_value (results, "shortcuts", NULL);
  if (shortcuts == NULL)
    {
      g_debug ("shell BindShortcuts response did not contain shortcuts");
      response = 2;
      goto out;
    }

  char *s = g_variant_print (results, FALSE);
  g_debug ("Received from shell ListShortcuts: %s", s);
  g_free (s);

  response = 0;
  portal_shortcuts = settings_shortcuts_to_portal (shortcuts, NULL);

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  if (portal_shortcuts)
    {
      g_variant_builder_add (&results_builder, "{sv}",
                             "shortcuts", portal_shortcuts);
    }
  xdp_impl_global_shortcuts_complete_list_shortcuts (global_shortcuts,
                                                     handle->invocation,
                                                     response,
                                                     g_variant_builder_end (&results_builder));

  shortcuts_handle_free (handle);
}

static gboolean
handle_list_shortcuts (XdpImplGlobalShortcuts *object,
                       GDBusMethodInvocation  *invocation,
                       const char             *arg_handle,
                       const char             *arg_session_handle)
{
  Session *session;
  GVariantBuilder results_builder;
  int response;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to list shortcuts on non-existing %s", arg_session_handle);
      response = 2;
      goto out;
    }

  if (!is_global_shortcuts_session (session))
    {
      g_warning ("Tried to bind shortcuts on the wrong session type");
      response = 2;
      goto out;
    }

  BindShortcutsHandle *handle = g_new0 (BindShortcutsHandle, 1);
  handle->session_handle = g_strdup (arg_session_handle);
  handle->invocation = g_object_ref (invocation);

  org_gnome_shell_call_list_shortcuts (shell, handle->session_handle, NULL, shell_list_shortcuts_done, handle);

  return TRUE;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_global_shortcuts_complete_bind_shortcuts (object,
                                                     invocation,
                                                     response,
                                                     g_variant_builder_end (&results_builder));
  return TRUE;
}

static gboolean
match_app_id (Session       *session,
              gconstpointer  data)
{
  const char *app_id = data;
  GlobalShortcutsSession *shortcuts_session;

  if (!is_global_shortcuts_session (session))
    return FALSE;

  shortcuts_session = (GlobalShortcutsSession *) session;

  if (g_strcmp0 (shortcuts_session->app_id, app_id) != 0)
    return FALSE;

  return TRUE;
}

static gboolean
handle_rebind_shortcuts (OrgGnomeGlobalShortcutsRebind *object,
                         GDBusMethodInvocation         *invocation,
                         const char                    *arg_app_id,
                         GVariant                      *arg_shortcuts)
{
  Session *session;
  GlobalShortcutsSession *shortcuts_session;

  session = find_session (match_app_id, arg_app_id);

  if (!session)
    {
      g_warning ("Session for app %s not found in RebindShortcuts", arg_app_id);
      org_gnome_global_shortcuts_rebind_complete_rebind_shortcuts (object, invocation);
      return TRUE;
    }

  shortcuts_session = (GlobalShortcutsSession *) session;

  shortcuts_session->bound = TRUE;

  BindShortcutsHandle *handle = g_new0 (BindShortcutsHandle, 1);
  handle->session_handle = g_strdup (session_get_id (session));
  handle->invocation = g_object_ref (invocation);
  handle->shortcuts = g_variant_ref (arg_shortcuts);

  char *s = g_variant_print (arg_shortcuts, FALSE);
  g_debug ("Sending to shell: %s", s);
  g_free (s);

  org_gnome_shell_call_bind_shortcuts (shell, handle->session_handle, arg_shortcuts, NULL, shell_bind_shortcuts_done, handle);

  return TRUE;
}

static void
on_shell_activated (OrgGnomeShell *shell,
                    const char    *group_id,
                    const char    *shortcut_id,
                    guint64        timestamp,
                    gpointer       data)
{
  GDBusInterfaceSkeleton *object = data;
  GDBusConnection *connection;
  const char *object_path;
  const char *interface_name;
  Session *session;
  GVariantBuilder options_builder;

  session = lookup_session (group_id);
  if (!session)
    {
      g_warning ("Tried to activate shortcut %s in unknown group %s", shortcut_id, group_id);
      return;
    }

  g_debug ("Propagating Activated for shortcut %s in group %s", shortcut_id, group_id);

  connection = g_dbus_interface_skeleton_get_connection (object);
  object_path = g_dbus_interface_skeleton_get_object_path (object);
  interface_name = g_dbus_interface_skeleton_get_info (object)->name;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_emit_signal (connection,
                                 session_get_peer_name (session),
                                 object_path,
                                 interface_name,
                                 "Activated",
                                 g_variant_new ("(ost@a{sv})",
                                                group_id, shortcut_id, timestamp, g_variant_builder_end (&options_builder)),
                                 NULL);
}

static void
on_shell_deactivated (OrgGnomeShell *shell,
                      const char    *group_id,
                      const char    *shortcut_id,
                      guint64        timestamp,
                      gpointer       data)
{
  GDBusInterfaceSkeleton *object = data;
  GDBusConnection *connection;
  const char *object_path;
  const char *interface_name;
  Session *session;
  GVariantBuilder options_builder;

  session = lookup_session (group_id);
  if (!session)
    {
      g_warning ("Tried to deactivate shortcut %s in unknown group %s", shortcut_id, group_id);
      return;
    }

  g_debug ("Propagating Deactivated for shortcut %s in group %s", shortcut_id, group_id);

  connection = g_dbus_interface_skeleton_get_connection (object);
  object_path = g_dbus_interface_skeleton_get_object_path (object);
  interface_name = g_dbus_interface_skeleton_get_info (object)->name;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  g_dbus_connection_emit_signal (connection,
                                 session_get_peer_name (session),
                                 object_path,
                                 interface_name,
                                 "Deactivated",
                                 g_variant_new ("(ost@a{sv})",
                                                group_id, shortcut_id, timestamp, g_variant_builder_end (&options_builder)),
                                 NULL);
}

gboolean
global_shortcuts_init (GDBusConnection *bus,
                       GError **error)
{
  global_shortcuts = xdp_impl_global_shortcuts_skeleton_new ();
  xdp_impl_global_shortcuts_set_version (global_shortcuts, 1);

  g_signal_connect (global_shortcuts, "handle-bind-shortcuts", G_CALLBACK (handle_bind_shortcuts), NULL);
  g_signal_connect (global_shortcuts, "handle-create-session", G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (global_shortcuts, "handle-list-shortcuts", G_CALLBACK (handle_list_shortcuts), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (global_shortcuts),
                                         bus,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         error))
    return FALSE;

  global_shortcuts_rebind = org_gnome_global_shortcuts_rebind_skeleton_new ();
  g_signal_connect (global_shortcuts_rebind, "handle-rebind-shortcuts", G_CALLBACK (handle_rebind_shortcuts), NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (global_shortcuts_rebind),
                                         bus,
                                         "/org/gnome/globalshortcuts",
                                         error))
    return FALSE;

  shell = org_gnome_shell_proxy_new_sync (bus,
                                          G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                          "org.gnome.Shell",
                                          "/org/gnome/Shell",
                                          NULL,
                                          error);
  if (shell == NULL)
    return FALSE;

  g_signal_connect (shell, "activated", G_CALLBACK (on_shell_activated), global_shortcuts);
  g_signal_connect (shell, "deactivated", G_CALLBACK (on_shell_deactivated), global_shortcuts);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (shell), G_MAXINT);

  settings = org_gnome_settings_global_shortcuts_provider_proxy_new_sync (bus,
                                                                         G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                                                                         "org.gnome.Settings.GlobalShortcutsProvider",
                                                                         "/org/gnome/Settings/GlobalShortcutsProvider",
                                                                         NULL,
                                                                         error);
  if (settings == NULL)
    return FALSE;

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (settings), G_MAXINT);

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (G_DBUS_INTERFACE_SKELETON (global_shortcuts))->name);

  return TRUE;
}
