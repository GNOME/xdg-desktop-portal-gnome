#define _GNU_SOURCE 1

#include "config.h"

#include <gio/gio.h>

#include <glib/gi18n.h>

#include "shell-dbus.h"

#include "globalshortcuts.h"
#include "request.h"
#include "utils.h"
#include "session.h"

#define SHELL_ACTION_MODE_NORMAL 1

#define META_KEY_BINDING_TRIGGER_RELEASE (1 << 7)

static OrgGnomeShell *shell;
static OrgGnomeSettingsGlobalShortcutsProvider *settings;
static XdpImplGlobalShortcuts *global_shortcuts;
static OrgGnomeGlobalShortcutsRebind *global_shortcuts_rebind;

typedef struct _Accelerator
{
  char *accelerator;
  guint accelerator_id; /* Shell identifier */
} Accelerator;

typedef struct _Shortcut
{
  gchar *shortcut_id; /* Client identifier */
  gchar *desc;
  GArray *accelerators;
} Shortcut;

typedef struct {
  GDBusMethodInvocation *invocation;
  Session *session;
  Request *request;
  GVariant *shortcuts;
  GPtrArray *mapped_accelerators;
  gboolean canceled;
} BindShortcutsHandle;

typedef struct _GlobalShortcutsSession
{
  Session parent;
  char *app_id;
  GArray *shortcuts;

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
clear_shortcut (Shortcut *shortcut)
{
  g_array_unref (shortcut->accelerators);
  g_free (shortcut->shortcut_id);
  g_free (shortcut->desc);
}

static void
clear_accelerator (Accelerator *accel)
{
  g_free (accel->accelerator);
}

static void
shortcuts_handle_free (BindShortcutsHandle *handle)
{
  g_object_unref (handle->invocation);
  if (handle->request)
    {
      if (handle->request->exported)
        request_unexport (handle->request);
      g_object_unref (handle->request);
    }
  g_clear_pointer (&handle->shortcuts, g_variant_unref);
  g_free (handle);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (BindShortcutsHandle, shortcuts_handle_free);

static void
settings_response_to_shortcuts (GlobalShortcutsSession *session,
                                GVariant               *response)
{
  GVariantIter iter;
  g_autofree char *shortcut_id = NULL;
  g_autoptr(GVariant) val = NULL;

  g_variant_iter_init (&iter, response);

  while (g_variant_iter_next (&iter, "(s@a{sv})", &shortcut_id, &val))
    {
      g_autoptr(GVariant) desc = NULL, shortcuts = NULL;
      Shortcut shortcut = { 0, };

      shortcut.shortcut_id = g_steal_pointer (&shortcut_id);
      shortcut.accelerators = g_array_new (FALSE, FALSE, sizeof (Accelerator));
      g_array_set_clear_func (shortcut.accelerators, (GDestroyNotify) clear_accelerator);

      desc = g_variant_lookup_value (val, "description", NULL);
      if (desc)
        shortcut.desc = g_variant_dup_string (desc, NULL);

      shortcuts = g_variant_lookup_value (val, "shortcuts", NULL);
      if (shortcuts)
        {
          GVariantIter iter2;
          g_autofree char *shortcut_str = NULL;

          g_variant_iter_init (&iter2, shortcuts);

          while (g_variant_iter_next (&iter2, "s", &shortcut_str))
            {
              Accelerator accel = { 0, };

              accel.accelerator = g_steal_pointer (&shortcut_str);
              g_array_append_val (shortcut.accelerators, accel);
            }

          g_array_append_val (session->shortcuts, shortcut);
        }
    }
}

static GVariant *
shortcuts_to_grab_accelerator_variant (GlobalShortcutsSession *shortcuts_session,
                                       BindShortcutsHandle    *handle)
{
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(suu)"));
  int i, j;

  for (i = 0; i < shortcuts_session->shortcuts->len; i++)
    {
      Shortcut *shortcut;

      shortcut = &g_array_index (shortcuts_session->shortcuts, Shortcut, i);

      for (j = 0; j < shortcut->accelerators->len; j++)
        {
          Accelerator *accel;

          accel = &g_array_index (shortcut->accelerators, Accelerator, j);
          g_ptr_array_add (handle->mapped_accelerators, accel);

          g_variant_builder_add (&builder, "(suu)",
                                 accel->accelerator,
                                 SHELL_ACTION_MODE_NORMAL,
                                 META_KEY_BINDING_TRIGGER_RELEASE);
        }
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static GVariant *
shortcuts_to_ungrab_accelerator_variant (GlobalShortcutsSession *shortcuts_session)
{
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("au"));
  int i, j;

  for (i = 0; i < shortcuts_session->shortcuts->len; i++)
    {
      Shortcut *shortcut;

      shortcut = &g_array_index (shortcuts_session->shortcuts, Shortcut, i);

      for (j = 0; j < shortcut->accelerators->len; j++)
        {
          Accelerator *accel;

          accel = &g_array_index (shortcut->accelerators, Accelerator, j);
          g_variant_builder_add (&builder, "u", accel->accelerator_id);
        }
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static void
global_shortcuts_session_init (GlobalShortcutsSession *session)
{
  session->shortcuts = g_array_new (FALSE, FALSE, sizeof (Shortcut));
  g_array_set_clear_func (session->shortcuts, (GDestroyNotify) clear_shortcut);
}

static void
global_shortcuts_session_finalize (GObject *object)
{
  GlobalShortcutsSession *session = (GlobalShortcutsSession *) object;

  g_clear_pointer (&session->shortcuts, g_array_unref);
  g_free (session->app_id);

  G_OBJECT_CLASS (global_shortcuts_session_parent_class)->finalize (object);
}

static void
shell_ungrab_accelerators_done (GObject *source,
                                GAsyncResult *result,
                                gpointer data)
{
  g_autoptr(GError) error = NULL;
  gboolean success;

  if (!org_gnome_shell_call_ungrab_accelerators_finish (shell, &success, result, &error))
    g_debug ("Error from shell UnbindShortcuts: %s", error->message);

  if (!success)
    g_debug ("Failed to ungrab accelerators");
}

static void
global_shortcuts_session_close (Session *session)
{
  GlobalShortcutsSession *shortcuts_session;
  g_autoptr(GVariant) variant = NULL;

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

  variant = shortcuts_to_ungrab_accelerator_variant (shortcuts_session);

  org_gnome_shell_call_ungrab_accelerators (shell,
                                            variant,
                                            NULL,
                                            shell_ungrab_accelerators_done,
                                            NULL);
}

static void
global_shortcuts_session_class_init (GlobalShortcutsSessionClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  SessionClass *session_class = (SessionClass *) klass;

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

static GVariant *
shortcuts_to_response_variant (GArray *shortcuts)
{
  GVariantBuilder builder;
  int i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));

  for (i = 0; i < shortcuts->len; i++)
    {
      GVariantBuilder accels_builder;
      Shortcut *shortcut;
      g_autofree char *str = NULL;

      shortcut = &g_array_index (shortcuts, Shortcut, i);
      g_variant_builder_init (&accels_builder, G_VARIANT_TYPE ("a{sv}"));

      if (shortcut->desc)
        {
          g_variant_builder_add (&accels_builder, "{sv}",
                                 "description",
                                 g_variant_new_string (shortcut->desc));
        }

      if (shortcut->accelerators->len == 1)
        {
          Accelerator *accel;

          accel = &g_array_index (shortcut->accelerators, Accelerator, 0);
          /* Translators: This string refers to a key combination */
          str = g_strdup_printf (_("Press %s"), accel->accelerator);
        }
      else if (shortcut->accelerators->len == 2)
        {
          Accelerator *accel1, *accel2;

          accel1 = &g_array_index (shortcut->accelerators, Accelerator, 0);
          accel2 = &g_array_index (shortcut->accelerators, Accelerator, 1);
          /* Translators: This string refers to 2 key combinations */
          str = g_strdup_printf (_("Press %s or %s"),
                                 accel1->accelerator,
                                 accel2->accelerator);
        }

      if (str)
        {
          g_variant_builder_add (&accels_builder, "{sv}",
                                 "trigger_description",
                                 g_variant_new_string (str));
        }

      g_variant_builder_add (&builder, "(s@a{sv})",
                             shortcut->shortcut_id,
                             g_variant_builder_end (&accels_builder));
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
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
  g_auto(GStrv) pieces = NULL;
  struct {
    const char *from;
    const char *to;
  } map[] = {
    { "CTRL", "<ctrl>" },
    { "SHIFT", "<shift>" },
    { "ALT", "<alt>" },
    { "NUM", "<mod2>" },
    { "LOGO", "<super>" },
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

  return g_string_free_and_steal (result);
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
  g_autofree char *shortcut_id = NULL;
  g_autoptr(GVariant) options = NULL;
  g_auto(GVariantBuilder) builder =
    G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("a(sa{sv})"));

  g_variant_iter_init (&iter, input);
  while (g_variant_iter_next (&iter, "(s@a{sv})", &shortcut_id, &options))
    {
      g_autoptr(GVariant) v = g_steal_pointer (&options);
      g_autofree char *id = g_steal_pointer (&shortcut_id);
      g_autofree char *description = NULL;
      g_autofree char *preferred_trigger = NULL;
      g_autofree char *settings_trigger = NULL;

      g_variant_lookup (v, "preferred_trigger", "s", &preferred_trigger);

      if (!g_variant_lookup (v, "description", "s", &description))
        description = g_strdup (_("Undocumented shortcut"));

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
    }

  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

static void
shell_grab_accelerators_done (GObject      *source,
                              GAsyncResult *result,
                              gpointer      data)
{
  OrgGnomeShell *proxy = (OrgGnomeShell *) source;
  GlobalShortcutsSession *shortcuts_session;
  g_autoptr(BindShortcutsHandle) handle = data;
  g_autoptr(GVariant) results = NULL, shortcuts = NULL;
  int response;
  GVariantBuilder results_builder;
  GVariantIter iter;
  g_autoptr(GError) error = NULL;
  guint accel_id;
  int i = 0;

  if (!org_gnome_shell_call_grab_accelerators_finish (proxy,
                                                      &results,
                                                      result,
                                                      &error))
    {
      g_warning ("Error from shell GrabAccelerators: %s", error->message);
      response = 2;
      goto out;
    }

  g_variant_iter_init (&iter, results);

  while (g_variant_iter_next (&iter, "u", &accel_id))
    {
      Accelerator *accel;

      accel = g_ptr_array_index (handle->mapped_accelerators, i);
      accel->accelerator_id = accel_id;
      i++;
    }

  shortcuts_session = (GlobalShortcutsSession *) handle->session;
  shortcuts = shortcuts_to_response_variant (shortcuts_session->shortcuts);

  if (shortcuts)
    {
      xdp_impl_global_shortcuts_emit_shortcuts_changed (global_shortcuts,
                                                        session_get_id (handle->session),
                                                        g_variant_ref (shortcuts));
    }

  response = 0;

 out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (shortcuts)
    g_variant_builder_add (&results_builder, "{sv}", "shortcuts", shortcuts);

  xdp_impl_global_shortcuts_complete_bind_shortcuts (global_shortcuts,
                                                     handle->invocation,
                                                     response,
                                                     g_variant_builder_end (&results_builder));
}

static void
settings_bind_shortcuts_done (GObject      *source,
                              GAsyncResult *result,
                              gpointer      data)
{
  OrgGnomeSettingsGlobalShortcutsProvider *proxy = (OrgGnomeSettingsGlobalShortcutsProvider *) source;
  g_autoptr(BindShortcutsHandle) handle = data;
  GlobalShortcutsSession *session = (GlobalShortcutsSession *) handle->session;
  g_autoptr(GVariant) response = NULL;
  GVariantBuilder results_builder;
  g_autoptr(GVariant) variant = NULL;
  g_autoptr(GError) error = NULL;

  if (!org_gnome_settings_global_shortcuts_provider_call_bind_shortcuts_finish (proxy,
                                                                                &response,
                                                                                result,
                                                                                &error))
    {
      g_debug ("Error from GlobalShortcutsProvider: %s", error->message);
      g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
      xdp_impl_global_shortcuts_complete_bind_shortcuts (global_shortcuts,
                                                         handle->invocation,
                                                         2,
                                                         g_variant_builder_end (&results_builder));
      return;
    }

  if (handle->canceled)
    {
      g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
      xdp_impl_global_shortcuts_complete_bind_shortcuts (global_shortcuts,
                                                         handle->invocation,
                                                         1,
                                                         g_variant_builder_end (&results_builder));
      return;
    }

  settings_response_to_shortcuts (session, response);
  variant = shortcuts_to_grab_accelerator_variant (session, handle);

  org_gnome_shell_call_grab_accelerators (shell,
                                          variant,
                                          NULL,
                                          shell_grab_accelerators_done,
                                          g_steal_pointer (&handle));
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
  g_autoptr(GVariant) settings_shortcuts = NULL;
  g_autoptr(BindShortcutsHandle) handle = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to bind shortcuts on non-existing %s", arg_session_handle);
      goto out;
    }

  if (!is_global_shortcuts_session (session))
    {
      g_warning ("Tried to bind shortcuts on the wrong session type");
      goto out;
    }

  shortcuts_session = (GlobalShortcutsSession *) session;

  if (shortcuts_session->bound)
    {
      g_warning ("Session already has bound shortcuts");
      goto out;
    }

  request = request_new (sender, shortcuts_session->app_id, arg_handle);
  request_export (request,
                  g_dbus_method_invocation_get_connection (invocation));

  shortcuts_session->bound = TRUE;

  g_assert (object == global_shortcuts);
  handle = g_new0 (BindShortcutsHandle, 1);
  handle->session = session;
  handle->invocation = g_object_ref (invocation);
  handle->request = g_object_ref (request);
  handle->shortcuts = g_variant_ref (arg_shortcuts);
  handle->mapped_accelerators = g_ptr_array_new ();

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (on_request_handle_close_cb), handle);

  settings_shortcuts = portal_shortcuts_to_settings (arg_shortcuts);

  org_gnome_settings_global_shortcuts_provider_call_bind_shortcuts (settings,
                                                                    shortcuts_session->app_id,
                                                                    arg_parent_window,
                                                                    settings_shortcuts,
                                                                    NULL,
                                                                    settings_bind_shortcuts_done,
                                                                    g_steal_pointer (&handle));
  return TRUE;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_global_shortcuts_complete_bind_shortcuts (object,
                                                     invocation,
                                                     2,
                                                     g_variant_builder_end (&results_builder));
  return TRUE;
}

static gboolean
handle_list_shortcuts (XdpImplGlobalShortcuts *object,
                       GDBusMethodInvocation  *invocation,
                       const char             *arg_handle,
                       const char             *arg_session_handle)
{
  Session *session;
  GlobalShortcutsSession *shortcuts_session;
  GVariantBuilder results_builder;
  g_autoptr(GVariant) shortcuts = NULL;
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

  response = 0;
  shortcuts_session = (GlobalShortcutsSession *) session;
  shortcuts = shortcuts_to_response_variant (shortcuts_session->shortcuts);

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  if (shortcuts)
    g_variant_builder_add (&results_builder, "{sv}", "shortcuts", shortcuts);
  xdp_impl_global_shortcuts_complete_list_shortcuts (global_shortcuts,
                                                     invocation,
                                                     response,
                                                     g_variant_builder_end (&results_builder));
  return TRUE;
}

static void
shell_grab_accelerators_rebind_done (GObject      *object,
                                     GAsyncResult *result,
                                     gpointer      data)
{
  GlobalShortcutsSession *shortcuts_session;
  BindShortcutsHandle *handle = data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) response = NULL, shortcuts = NULL;
  GVariantIter iter;
  guint accel_id;
  guint i = 0;

  if (!org_gnome_shell_call_grab_accelerators_finish (shell, &response, result, &error))
    g_debug ("Error during RebindShortcuts: %s", error->message);

  shortcuts_session = (GlobalShortcutsSession *) handle->session;

  g_variant_iter_init (&iter, response);

  while (g_variant_iter_next (&iter, "u", &accel_id))
    {
      Accelerator *accel;

      accel = g_ptr_array_index (handle->mapped_accelerators, i);
      accel->accelerator_id = accel_id;
      i++;
    }

  shortcuts = shortcuts_to_response_variant (shortcuts_session->shortcuts);

  if (shortcuts)
    {
      xdp_impl_global_shortcuts_emit_shortcuts_changed (global_shortcuts,
                                                        session_get_id (handle->session),
                                                        shortcuts);
    }

  org_gnome_global_shortcuts_rebind_complete_rebind_shortcuts (global_shortcuts_rebind,
                                                               handle->invocation);
}

static void
shell_ungrab_accelerators_rebind_done (GObject      *source,
                                       GAsyncResult *result,
                                       gpointer      data)
{
  BindShortcutsHandle *handle = data;
  GlobalShortcutsSession *session = (GlobalShortcutsSession *) handle->session;
  g_autoptr(GVariant) variant = NULL;
  g_autoptr(GError) error = NULL;
  gboolean success;

  if (!org_gnome_shell_call_ungrab_accelerators_finish (shell, &success, result, &error))
    g_debug ("Error during RebindShortcuts: %s", error->message);

  if (!success)
    g_debug ("Failure ungrabbing shortcuts");

  variant = shortcuts_to_grab_accelerator_variant (session, handle);

  org_gnome_shell_call_grab_accelerators (shell,
                                          variant,
                                          NULL,
                                          shell_grab_accelerators_rebind_done,
                                          handle);
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
  g_autoptr(GVariant) variant = NULL;
  g_autoptr(BindShortcutsHandle) handle = NULL;

  session = find_session (match_app_id, arg_app_id);

  if (!session)
    {
      g_debug ("Session for app %s not found in RebindShortcuts", arg_app_id);
      org_gnome_global_shortcuts_rebind_complete_rebind_shortcuts (object, invocation);
      return TRUE;
    }

  shortcuts_session = (GlobalShortcutsSession *) session;

  g_debug ("Re-binding shortcuts of session %s", session_get_id (session));

  handle = g_new0 (BindShortcutsHandle, 1);
  handle->session = session;
  handle->invocation = g_object_ref (invocation);
  handle->shortcuts = g_variant_ref (arg_shortcuts);
  handle->mapped_accelerators = g_ptr_array_new ();

  /* Reset shortcuts */
  variant = shortcuts_to_ungrab_accelerator_variant (shortcuts_session);
  g_array_remove_range (shortcuts_session->shortcuts, 0,
                        shortcuts_session->shortcuts->len);
  settings_response_to_shortcuts (shortcuts_session, arg_shortcuts);

  org_gnome_shell_call_ungrab_accelerators (shell,
                                            variant,
                                            NULL,
                                            shell_ungrab_accelerators_rebind_done,
                                            g_steal_pointer (&handle));

  return TRUE;
}

static gboolean
match_accelerator_id (Session       *session,
                      gconstpointer  data)
{
  const guint *id = data;
  GlobalShortcutsSession *shortcuts_session;
  int i, j;

  if (!is_global_shortcuts_session (session))
    return FALSE;

  shortcuts_session = (GlobalShortcutsSession *) session;

  for (i = 0; i < shortcuts_session->shortcuts->len; i++)
    {
      Shortcut *shortcut;

      shortcut = &g_array_index (shortcuts_session->shortcuts, Shortcut, i);

      for (j = 0; j < shortcut->accelerators->len; j++)
        {
          Accelerator *accel;

          accel = &g_array_index (shortcut->accelerators, Accelerator, j);
          if (accel->accelerator_id == *id)
            return TRUE;
        }
    }

  return FALSE;
}

static GlobalShortcutsSession *
find_session_and_shortcut_id (guint        action,
                              const char **shortcut_out)
{
  GlobalShortcutsSession *shortcuts_session;
  const char *shortcut_id = NULL;
  Session *session;
  int i, j;

  session = find_session (match_accelerator_id, &action);

  if (!session)
    {
      g_debug ("Session for accelerator %u not found", action);
      return NULL;
    }

  shortcuts_session = (GlobalShortcutsSession *) session;

  for (i = 0; i < shortcuts_session->shortcuts->len; i++)
    {
      Shortcut *shortcut;

      shortcut = &g_array_index (shortcuts_session->shortcuts, Shortcut, i);

      for (j = 0; j < shortcut->accelerators->len; j++)
        {
          Accelerator *accel;

          accel = &g_array_index (shortcut->accelerators, Accelerator, j);

          if (accel->accelerator_id == action)
            shortcut_id = shortcut->shortcut_id;
        }
    }

  if (!shortcut_id)
    {
      g_debug ("Shortcut ID not found in session %s", shortcuts_session->app_id);
      return NULL;
    }

  *shortcut_out = shortcut_id;

  return shortcuts_session;
}

static void
on_shell_activated (OrgGnomeShell *shell,
                    guint          action,
                    GVariant      *parameters,
                    gpointer       data)
{
  GlobalShortcutsSession *shortcuts_session;
  const char *shortcut_id = NULL;
  Session *session;
  GVariantBuilder options_builder;
  g_autoptr(GVariant) val = NULL;
  uint32_t timestamp = 0;

  shortcuts_session = find_session_and_shortcut_id (action, &shortcut_id);
  if (!shortcuts_session)
    return;

  session = (Session *) shortcuts_session;

  val = g_variant_lookup_value (parameters, "timestamp", G_VARIANT_TYPE_UINT32);
  if (val)
    timestamp = g_variant_get_uint32 (val);

  g_debug ("Propagating Activated for shortcut %s in group %s at %u",
           shortcut_id, shortcuts_session->app_id, timestamp);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_global_shortcuts_emit_activated (global_shortcuts,
                                            session_get_id (session),
                                            shortcut_id,
                                            timestamp,
                                            g_variant_builder_end (&options_builder));
}

static void
on_shell_deactivated (OrgGnomeShell *shell,
                      guint          action,
                      GVariant      *parameters,
                      gpointer       data)
{
  GlobalShortcutsSession *shortcuts_session;
  const char *shortcut_id = NULL;
  Session *session;
  GVariantBuilder options_builder;
  g_autoptr(GVariant) val = NULL;
  uint32_t timestamp = 0;

  shortcuts_session = find_session_and_shortcut_id (action, &shortcut_id);
  if (!shortcuts_session)
    return;

  session = (Session *) shortcuts_session;

  val = g_variant_lookup_value (parameters, "timestamp", G_VARIANT_TYPE_UINT32);
  if (val)
    timestamp = g_variant_get_uint32 (val);

  g_debug ("Propagating Deactivated for shortcut %s in group %s at %u",
           shortcut_id, shortcuts_session->app_id, timestamp);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_global_shortcuts_emit_deactivated (global_shortcuts,
                                              session_get_id (session),
                                              shortcut_id,
                                              timestamp,
                                              g_variant_builder_end (&options_builder));
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

  g_signal_connect (shell, "accelerator-activated", G_CALLBACK (on_shell_activated), global_shortcuts);
  g_signal_connect (shell, "accelerator-deactivated", G_CALLBACK (on_shell_deactivated), global_shortcuts);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (shell), G_MAXINT);

  settings = org_gnome_settings_global_shortcuts_provider_proxy_new_sync (bus,
                                                                          G_DBUS_PROXY_FLAGS_NONE,
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
