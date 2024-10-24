/*
 * Copyright © 2022 Red Hat, Inc
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

#include <gxdp.h>

#include "inputcapture.h"
#include "inputcapturedialog.h"
#include "session.h"
#include "gnomeinputcapture.h"
#include "request.h"
#include "utils.h"

typedef enum _InputCaptureCapabilities
{
  INPUT_CAPTURE_CAPABILITY_NONE = 0,
  INPUT_CAPTURE_CAPABILITY_KEYBOARD = 1,
  INPUT_CAPTURE_CAPABILITY_POINTER = 2,
  INPUT_CAPTURE_CAPABILITY_TOUCH = 4,
} InputCaptureCapabilities;

typedef struct _InputCaptureDialogHandle InputCaptureDialogHandle;

typedef struct
{
  unsigned int id;
  int x1;
  int y1;
  int x2;
  int y2;

  unsigned int gnome_barrier_id;
} BarrierInfo;

typedef struct _InputCaptureSession
{
  Session parent;

  GnomeInputCaptureSession *gnome_input_capture_session;
  gulong session_closed_handler_id;

  GList *barrier_infos;

  InputCaptureDialogHandle *dialog_handle;
} InputCaptureSession;

typedef struct _InputCaptureSessionClass
{
  SessionClass parent_class;
} InputCaptureSessionClass;

typedef struct _InputCaptureDialogHandle
{
  Request *request;
  char *session_handle;
  GDBusMethodInvocation *create_session_invocation;

  GtkWindow *dialog;
  GxdpExternalWindow *external_parent;

  unsigned int capabilities;

  int response;
} InputCaptureDialogHandle;

static GDBusConnection *impl_connection;
static GDBusInterfaceSkeleton *impl;

static GnomeInputCapture *gnome_input_capture;

GType input_capture_session_get_type (void);
G_DEFINE_TYPE (InputCaptureSession, input_capture_session, session_get_type ())

static void
input_capture_dialog_handle_free (InputCaptureDialogHandle *dialog_handle)
{
  g_clear_pointer (&dialog_handle->dialog, gtk_window_destroy);
  g_clear_object (&dialog_handle->external_parent);
  g_object_unref (dialog_handle->request);
  g_free (dialog_handle->session_handle);

  g_free (dialog_handle);
}

static void
input_capture_dialog_handle_close (InputCaptureDialogHandle *dialog_handle)
{
  input_capture_dialog_handle_free (dialog_handle);
}

static InputCaptureSession *
input_capture_session_new (const char *app_id,
                           const char *session_handle,
                           const char *peer_name)
{
  InputCaptureSession *input_capture_session;

  input_capture_session = g_object_new (input_capture_session_get_type (),
                                        "id", session_handle,
                                        "peer-name", peer_name,
                                        NULL);

  return input_capture_session;
}

static void
emit_session_signal (InputCaptureSession *input_capture_session,
                     const char          *signal_name,
                     const char          *argument_format,
                     ...)
{
  Session *session = (Session *)input_capture_session;
  va_list args;
  GVariant *argument_variant;
  const char *object_path;
  const char *interface_name;

  va_start (args, argument_format);
  argument_variant = g_variant_new_va (argument_format, NULL, &args);
  va_end (args);

  object_path = g_dbus_interface_skeleton_get_object_path (impl);
  interface_name = g_dbus_interface_skeleton_get_info (impl)->name;
  g_dbus_connection_emit_signal (impl_connection,
                                 session_get_peer_name (session),
                                 object_path,
                                 interface_name,
                                 signal_name,
                                 argument_variant,
                                 NULL);
}

static gboolean
on_request_handle_close_cb (XdpImplRequest           *object,
                            GDBusMethodInvocation    *invocation,
                            InputCaptureDialogHandle *dialog_handle)
{
  gtk_window_close (dialog_handle->dialog);
  return FALSE;
}

static void
on_session_zones_changed (GnomeInputCaptureSession *gnome_input_capture_session,
                          InputCaptureSession      *input_capture_session)
{
  Session *session = (Session *)input_capture_session;
  GVariantBuilder options_builder;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  emit_session_signal (input_capture_session,
                       "ZonesChanged",
                       "(oa{sv})",
                       session_get_id (session),
                       &options_builder);
}

static BarrierInfo *
find_barrier_info (InputCaptureSession *input_capture_session,
                   unsigned int         gnome_barrier_id)
{
  GList *l;

  for (l = input_capture_session->barrier_infos; l; l = l->next)
    {
      BarrierInfo *barrier_info = l->data;

      if (gnome_barrier_id == barrier_info->gnome_barrier_id)
        return barrier_info;
    }

  return NULL;
}

static void
on_session_activated (GnomeInputCaptureSession *gnome_input_capture_session,
                      unsigned int              activation_id,
                      double                    cursor_x,
                      double                    cursor_y,
                      unsigned int              gnome_barrier_id,
                      InputCaptureSession      *input_capture_session)
{
  Session *session = (Session *)input_capture_session;
  BarrierInfo *barrier_info;
  GVariantBuilder options_builder;

  barrier_info = find_barrier_info (input_capture_session, gnome_barrier_id);
  g_return_if_fail (barrier_info);

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options_builder, "{sv}",
                         "activation_id", g_variant_new_uint32 (activation_id));
  g_variant_builder_add (&options_builder, "{sv}",
                         "cursor_position", g_variant_new ("(dd)",
                                                           cursor_x, cursor_y));
  g_variant_builder_add (&options_builder, "{sv}",
                         "barrier_id", g_variant_new_uint32 (barrier_info->id));

  emit_session_signal (input_capture_session,
                       "Activated",
                       "(oa{sv})",
                       session_get_id (session),
                       &options_builder);
}

static void
on_session_deactivated (GnomeInputCaptureSession *gnome_input_capture_session,
                        unsigned int              activation_id,
                        InputCaptureSession      *input_capture_session)
{
  Session *session = (Session *)input_capture_session;
  GVariantBuilder options_builder;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options_builder, "{sv}",
                         "activation_id",
                         g_variant_new_uint32 (activation_id));
  emit_session_signal (input_capture_session,
                       "Deactivated",
                       "(oa{sv})",
                       session_get_id (session),
                       &options_builder);
}

static void
on_session_disabled (GnomeInputCaptureSession *gnome_input_capture_session,
                     InputCaptureSession      *input_capture_session)
{
  Session *session = (Session *)input_capture_session;
  GVariantBuilder options_builder;

  g_variant_builder_init (&options_builder, G_VARIANT_TYPE_VARDICT);
  emit_session_signal (input_capture_session,
                       "Disabled",
                       "(oa{sv})",
                       session_get_id (session),
                       &options_builder);
}

static void
on_session_closed (GnomeInputCaptureSession *gnome_input_capture_session,
                   InputCaptureSession      *input_capture_session)
{
  Session *session = (Session *)input_capture_session;

  session_emit_closed (session);
}

static void
on_input_capture_dialog_done_cb (GtkWidget                *widget,
                                 int                       dialog_response,
                                 InputCaptureDialogHandle *dialog_handle)
{
  GDBusMethodInvocation *invocation = dialog_handle->create_session_invocation;
  int response;
  GVariantBuilder results_builder;

  switch (dialog_response)
    {
    default:
      g_warning ("Unexpected response: %d", dialog_response);
      G_GNUC_FALLTHROUGH;
    case GTK_RESPONSE_DELETE_EVENT:
      response = 2;
      break;

    case GTK_RESPONSE_CANCEL:
      response = 1;
      break;

    case GTK_RESPONSE_OK:
      response = 0;
      break;
    }

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  if (response == 0)
    {
      g_autoptr(GError) error = NULL;
      InputCaptureCapabilities capabilities;
      GnomeInputCaptureSession *gnome_input_capture_session;
      Session *session;
      InputCaptureSession *input_capture_session;

      capabilities =
        dialog_handle->capabilities &
        gnome_input_capture_get_supported_capabilities (gnome_input_capture);
      gnome_input_capture_session =
        gnome_input_capture_create_session (gnome_input_capture,
                                            capabilities,
                                            &error);
      if (!gnome_input_capture_session)
        {
          g_warning ("Failed to create mutter input capture session: %s",
                     error->message);
          response = 2;
          goto out;
        }

      session =
        (Session *)input_capture_session_new (dialog_handle->request->app_id,
                                              dialog_handle->session_handle,
                                              dialog_handle->request->sender);

      if (!session_export (session,
                           g_dbus_method_invocation_get_connection (invocation),
                           &error))
        {
          g_clear_object (&gnome_input_capture_session);
          g_clear_object (&session);
          g_warning ("Failed to create input capture session: %s", error->message);
          response = 2;
          goto out;
        }

      input_capture_session = (InputCaptureSession *)session;
      input_capture_session->gnome_input_capture_session =
        gnome_input_capture_session;

      g_signal_connect (gnome_input_capture_session,
                        "activated",
                        G_CALLBACK (on_session_activated),
                        session);
      g_signal_connect (gnome_input_capture_session,
                        "deactivated",
                        G_CALLBACK (on_session_deactivated),
                        session);
      g_signal_connect (gnome_input_capture_session,
                        "zones-changed",
                        G_CALLBACK (on_session_zones_changed),
                        session);
      g_signal_connect (gnome_input_capture_session,
                        "disabled",
                        G_CALLBACK (on_session_disabled),
                        session);
      input_capture_session->session_closed_handler_id =
        g_signal_connect (gnome_input_capture_session,
                          "closed",
                          G_CALLBACK (on_session_closed),
                          session);

      g_variant_builder_add (&results_builder, "{sv}",
                             "capabilities",
                             g_variant_new_uint32 (capabilities));

      response = 0;
    }

out:

  if (dialog_handle->request->exported)
    request_unexport (dialog_handle->request);

  input_capture_dialog_handle_close (dialog_handle);

  xdp_impl_input_capture_complete_create_session ((XdpImplInputCapture *) impl,
                                                  invocation,
                                                  response,
                                                  g_variant_builder_end (&results_builder));
}

static void
create_input_capture_dialog (GDBusMethodInvocation *invocation,
                             Request               *request,
                             const char            *parent_window,
                             const char            *session_handle,
                             unsigned int           capabilities)
{
  g_autoptr(GtkWindowGroup) window_group = NULL;
  InputCaptureDialogHandle *dialog_handle;
  GxdpExternalWindow *external_parent;
  GdkSurface *surface;
  GtkWidget *fake_parent;
  GtkWindow *dialog;

  if (parent_window)
    {
      external_parent = gxdp_external_window_new_from_handle (parent_window);
      if (!external_parent)
        g_warning ("Failed to associate portal window with parent window %s",
                   parent_window);
    }
  else
    {
      external_parent = NULL;
    }

  fake_parent = g_object_new (GTK_TYPE_WINDOW, NULL);
  g_object_ref_sink (fake_parent);

  dialog = GTK_WINDOW (input_capture_dialog_new (request->app_id));
  gtk_window_set_transient_for (dialog, GTK_WINDOW (fake_parent));
  gtk_window_set_modal (dialog, TRUE);

  window_group = gtk_window_group_new ();
  gtk_window_group_add_window (window_group, GTK_WINDOW (dialog));

  dialog_handle = g_new0 (InputCaptureDialogHandle, 1);
  dialog_handle->request = g_object_ref (request);
  dialog_handle->external_parent = external_parent;
  dialog_handle->dialog = dialog;
  dialog_handle->session_handle = g_strdup (session_handle);
  dialog_handle->create_session_invocation = invocation;
  dialog_handle->capabilities = capabilities;

  g_signal_connect (request, "handle-close",
                    G_CALLBACK (on_request_handle_close_cb), dialog_handle);
  g_signal_connect (dialog, "done",
                    G_CALLBACK (on_input_capture_dialog_done_cb), dialog_handle);

  gtk_widget_realize (GTK_WIDGET (dialog));

  surface = gtk_native_get_surface (GTK_NATIVE (dialog));
  if (external_parent)
    gxdp_external_window_set_parent_of (external_parent, surface);

  gtk_window_present (dialog);
}

static gboolean
handle_create_session (XdpImplInputCapture   *object,
                       GDBusMethodInvocation *invocation,
                       const char            *arg_handle,
                       const char            *arg_session_handle,
                       const char            *arg_app_id,
                       const char            *arg_parent_window,
                       GVariant              *arg_options)
{
  const char *sender;
  g_autoptr(Request) request = NULL;
  unsigned int capabilities;

  g_variant_lookup (arg_options, "capabilities", "u", &capabilities);

  sender = g_dbus_method_invocation_get_sender (invocation);
  request = request_new (sender, arg_app_id, arg_handle);
  request_export (request,
                  g_dbus_method_invocation_get_connection (invocation));

  create_input_capture_dialog (invocation, request,
                               arg_parent_window,
                               arg_session_handle,
                               capabilities);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_disable (XdpImplInputCapture   *object,
                GDBusMethodInvocation *invocation,
                const char            *arg_session_handle,
                const char            *arg_app_id,
                GVariant              *arg_options)
{
  Session *session;
  InputCaptureSession *input_capture_session;
  GnomeInputCaptureSession *gnome_input_capture_session;
  int response;
  g_autoptr(GError) error = NULL;
  GVariantBuilder results_builder;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to disable on non-existing session %s", arg_session_handle);
      response = 2;
      goto out;
    }

  input_capture_session = (InputCaptureSession *)session;
  gnome_input_capture_session =
    input_capture_session->gnome_input_capture_session;

  if (!gnome_input_capture_session_disable (gnome_input_capture_session, &error))
    {
      g_warning ("Failed to disable session: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_input_capture_complete_disable (object, invocation, response,
                                           g_variant_builder_end (&results_builder));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_enable (XdpImplInputCapture   *object,
               GDBusMethodInvocation *invocation,
               const char            *arg_session_handle,
               const char            *arg_app_id,
               GVariant              *arg_options)
{
  Session *session;
  InputCaptureSession *input_capture_session;
  GnomeInputCaptureSession *gnome_input_capture_session;
  int response;
  g_autoptr(GError) error = NULL;
  GVariantBuilder results_builder;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to enable on non-existing session %s", arg_session_handle);
      response = 2;
      goto out;
    }

  input_capture_session = (InputCaptureSession *)session;
  gnome_input_capture_session =
    input_capture_session->gnome_input_capture_session;

  if (!gnome_input_capture_session_enable (gnome_input_capture_session, &error))
    {
      g_warning ("Failed to enable session: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

out:
  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  xdp_impl_input_capture_complete_enable (object, invocation, response,
                                          g_variant_builder_end (&results_builder));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_get_zones (XdpImplInputCapture   *object,
                  GDBusMethodInvocation *invocation,
                  const char            *arg_handle,
                  const char            *arg_session_handle,
                  const char            *arg_app_id,
                  GVariant              *arg_options)
{
  Session *session;
  InputCaptureSession *input_capture_session;
  GnomeInputCaptureSession *gnome_input_capture_session;
  int response;
  GVariantBuilder results_builder;
  GVariantBuilder zones_builder;
  g_autolist(InputCaptureZone) zones = NULL;
  g_autoptr(GError) error = NULL;
  uint32_t serial;
  GList *l;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to get zones on non-existing session %s", arg_session_handle);
      response = 2;
      goto out;
    }

  input_capture_session = (InputCaptureSession *)session;
  gnome_input_capture_session =
    input_capture_session->gnome_input_capture_session;

  zones =
    gnome_input_capture_session_get_zones (gnome_input_capture_session,
                                           &error);
  if (error)
    {
      g_warning ("Failed to get zones: %s", error->message);
      response = 2;
      goto out;
    }

  g_variant_builder_init (&zones_builder, G_VARIANT_TYPE ("a(uuii)"));
  for (l = zones; l; l = l->next)
    {
      InputCaptureZone *zone = l->data;

      g_variant_builder_add (&zones_builder, "(uuii)",
                             zone->width, zone->height,
                             zone->x, zone->y);
    }
  g_variant_builder_add (&results_builder, "{sv}",
                         "zones", g_variant_builder_end (&zones_builder));
  serial = gnome_input_capture_session_get_serial (gnome_input_capture_session);
  g_variant_builder_add (&results_builder, "{sv}",
                         "zone_set", g_variant_new_uint32 (serial));

  response = 0;

out:
  xdp_impl_input_capture_complete_get_zones (object, invocation, response,
                                             g_variant_builder_end (&results_builder));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static GVariant *
create_empty_vardict (void)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  return g_variant_builder_end (&builder);
}

static gboolean
handle_release (XdpImplInputCapture   *object,
                GDBusMethodInvocation *invocation,
                const char            *arg_session_handle,
                const char            *arg_app_id,
                GVariant              *arg_options)
{
  Session *session;
  InputCaptureSession *input_capture_session;
  GnomeInputCaptureSession *gnome_input_capture_session;
  double cursor_position_x;
  double cursor_position_y;
  gboolean has_cursor_position;
  int response;
  g_autoptr (GError) error = NULL;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to release on non-existing session %s", arg_session_handle);
      response = 2;
      goto out;
    }

  input_capture_session = (InputCaptureSession *)session;
  gnome_input_capture_session = input_capture_session->gnome_input_capture_session;

  has_cursor_position = g_variant_lookup (arg_options, "cursor_position", "(dd)",
                                          &cursor_position_x, &cursor_position_y);

  if (!gnome_input_capture_session_release (gnome_input_capture_session,
                                            has_cursor_position,
                                            cursor_position_x,
                                            cursor_position_y,
                                            &error))
    {
      g_warning ("Failed to release captured input: %s", error->message);
      response = 2;
      goto out;
    }

  response = 0;

out:
  xdp_impl_input_capture_complete_release (object,
                                           invocation,
                                           response,
                                           create_empty_vardict ());

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_set_pointer_barriers (XdpImplInputCapture   *object,
                             GDBusMethodInvocation *invocation,
                             const char            *arg_handle,
                             const char            *arg_session_handle,
                             const char            *arg_app_id,
                             GVariant              *arg_options,
                             GVariant              *arg_barriers,
                             unsigned int           arg_serial)
{
  Session *session;
  InputCaptureSession *input_capture_session;
  GnomeInputCaptureSession *gnome_input_capture_session;
  GVariantBuilder results_builder;
  GVariantBuilder failed_barriers_builder;
  GVariant *results;
  g_autofree BarrierInfo *barrier_infos = NULL;
  int n_barriers;
  int i;
  int response;
  g_autoptr (GError) error = NULL;

  g_variant_builder_init (&results_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init (&failed_barriers_builder, G_VARIANT_TYPE ("au"));

  n_barriers = g_variant_n_children (arg_barriers);
  barrier_infos = g_new0 (BarrierInfo, n_barriers);

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to set pointer barriers on non-existing session %s", arg_session_handle);
      response = 2;
      goto out;
    }
  input_capture_session = (InputCaptureSession *)session;
  gnome_input_capture_session = input_capture_session->gnome_input_capture_session;

  g_clear_list (&input_capture_session->barrier_infos, g_free);
  if (!gnome_input_capture_session_clear_barriers (gnome_input_capture_session,
                                                   &error))
    {
      g_warning ("Failed to clear existing barriers");
      response = 2;
      goto out;
    }

  for (i = 0; i < n_barriers; i++)
    {
      g_autoptr (GVariant) barrier_variant = NULL;
      BarrierInfo *barrier_info = &barrier_infos[i];

      barrier_variant = g_variant_get_child_value (arg_barriers, i);

      g_variant_lookup (barrier_variant, "barrier_id", "u",
                        &barrier_info->id);
      g_variant_lookup (barrier_variant, "position", "(iiii)",
                        &barrier_info->x1,
                        &barrier_info->y1,
                        &barrier_info->x2,
                        &barrier_info->y2);
    }

  for (i = 0; i < n_barriers; i++)
    {
      BarrierInfo *barrier_info = &barrier_infos[i];
      unsigned int gnome_barrier_id;
      g_autoptr (GError) error = NULL;

      gnome_barrier_id =
        gnome_input_capture_session_add_barrier (gnome_input_capture_session,
                                                 barrier_info->x1,
                                                 barrier_info->y1,
                                                 barrier_info->x2,
                                                 barrier_info->y2,
                                                 &error);
      if (!gnome_barrier_id)
        {
          g_warning ("Failed to add barrier %u: %s",
                     barrier_info->id, error->message);
          g_variant_builder_add (&failed_barriers_builder, "u", barrier_info->id);
          continue;
        }

      barrier_info->gnome_barrier_id = gnome_barrier_id;

      input_capture_session->barrier_infos =
        g_list_append (input_capture_session->barrier_infos,
                       g_memdup2 (barrier_info, sizeof *barrier_info));
    }

  response = 0;

out:
  g_variant_builder_add (&results_builder, "{sv}",
                         "failed_barriers", g_variant_builder_end (&failed_barriers_builder));
  results = g_variant_builder_end (&results_builder);
  xdp_impl_input_capture_complete_set_pointer_barriers (object, invocation,
                                                        response,
                                                        results);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_connect_to_eis (XdpImplInputCapture *object,
                       GDBusMethodInvocation *invocation,
                       GUnixFDList *fd_list,
                       const gchar *arg_session_handle,
                       const gchar *arg_app_id,
                       GVariant *arg_options)
{
  Session *session;
  InputCaptureSession *input_capture_session;
  GnomeInputCaptureSession *gnome_input_capture_session;
  g_autoptr(GError) error = NULL;
  g_autoptr (GUnixFDList) out_fd_list = NULL;
  GVariant *fd_variant;

  session = lookup_session (arg_session_handle);
  if (!session)
    {
      g_warning ("Tried to connect to EIS on non-existing session %s", arg_session_handle);
      return FALSE;
    }
  input_capture_session = (InputCaptureSession *)session;
  gnome_input_capture_session = input_capture_session->gnome_input_capture_session;

  if (!gnome_input_capture_connect_to_eis (gnome_input_capture_session,
                                           &out_fd_list,
                                           &fd_variant,
                                           &error))
    {
      g_warning ("Failed to connect to EIS: %s", error->message);
      return FALSE;
    }

  xdp_impl_input_capture_complete_connect_to_eis (object, invocation, out_fd_list, fd_variant);
  return TRUE;
}

static void
on_gnome_input_capture_enabled (GnomeInputCapture *gnome_input_capture)
{
  g_autoptr(GError) error = NULL;
  uint32_t supported_capabilities;

  impl = G_DBUS_INTERFACE_SKELETON (xdp_impl_input_capture_skeleton_new ());

  g_signal_connect (impl, "handle-create-session",
                    G_CALLBACK (handle_create_session), NULL);
  g_signal_connect (impl, "handle-disable",
                    G_CALLBACK (handle_disable), NULL);
  g_signal_connect (impl, "handle-enable",
                    G_CALLBACK (handle_enable), NULL);
  g_signal_connect (impl, "handle-release",
                    G_CALLBACK (handle_release), NULL);
  g_signal_connect (impl, "handle-get-zones",
                    G_CALLBACK (handle_get_zones), NULL);
  g_signal_connect (impl, "handle-set-pointer-barriers",
                    G_CALLBACK (handle_set_pointer_barriers), NULL);
  g_signal_connect (impl, "handle-connect-to-eis",
                    G_CALLBACK (handle_connect_to_eis), NULL);

  supported_capabilities =
    gnome_input_capture_get_supported_capabilities (gnome_input_capture);
  g_object_set (G_OBJECT (impl),
                "supported-capabilities", supported_capabilities,
                NULL);

  if (!g_dbus_interface_skeleton_export (impl,
                                         impl_connection,
                                         DESKTOP_PORTAL_OBJECT_PATH,
                                         &error))
    {
      g_warning ("Failed to export input capture portal implementation object: %s",
                 error->message);
      return;
    }

  g_debug ("providing %s", g_dbus_interface_skeleton_get_info (impl)->name);
}

static void
on_gnome_input_capture_disabled (GDBusConnection *connection,
                                 const char      *name,
                                 gpointer         user_data)
{
  if (impl)
    {
      g_debug ("unproviding %s", g_dbus_interface_skeleton_get_info (impl)->name);

      g_dbus_interface_skeleton_unexport (impl);
      g_clear_object (&impl);
    }
}

gboolean
input_capture_init (GDBusConnection  *connection,
                    GError          **error)
{
  impl_connection = connection;
  gnome_input_capture = gnome_input_capture_new (connection);

  g_signal_connect (gnome_input_capture, "enabled",
                    G_CALLBACK (on_gnome_input_capture_enabled), NULL);
  g_signal_connect (gnome_input_capture, "disabled",
                    G_CALLBACK (on_gnome_input_capture_disabled), NULL);

  return TRUE;
}

static void
input_capture_session_close (Session *session)
{
  InputCaptureSession *input_capture_session = (InputCaptureSession *)session;
  GnomeInputCaptureSession *gnome_input_capture_session;
  g_autoptr(GError) error = NULL;

  gnome_input_capture_session = input_capture_session->gnome_input_capture_session;
  if (gnome_input_capture_session)
    {
      g_clear_signal_handler (&input_capture_session->session_closed_handler_id,
                              gnome_input_capture_session);
      if (!gnome_input_capture_session_close (gnome_input_capture_session,
                                              &error))
        g_warning ("Failed to close GNOME input capture session: %s",
                   error->message);
      g_clear_object (&input_capture_session->gnome_input_capture_session);
    }
}

static void
input_capture_session_finalize (GObject *object)
{
  InputCaptureSession *input_capture_session = (InputCaptureSession *)object;

  g_clear_list (&input_capture_session->barrier_infos, g_free);
  g_clear_object (&input_capture_session->gnome_input_capture_session);

  G_OBJECT_CLASS (input_capture_session_parent_class)->finalize (object);
}

static void
input_capture_session_init (InputCaptureSession *input_capture_session)
{
}

static void
input_capture_session_class_init (InputCaptureSessionClass *klass)
{
  GObjectClass *gobject_class;
  SessionClass *session_class;

  gobject_class = (GObjectClass *)klass;
  gobject_class->finalize = input_capture_session_finalize;

  session_class = (SessionClass *)klass;
  session_class->close = input_capture_session_close;
}
