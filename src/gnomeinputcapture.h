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

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <stdint.h>

#include "screencast.h"

typedef struct _GnomeInputCapture GnomeInputCapture;
typedef struct _GnomeInputCaptureSession GnomeInputCaptureSession;

GnomeInputCapture *gnome_input_capture_new (GDBusConnection *connection);

uint32_t gnome_input_capture_get_supported_capabilities (GnomeInputCapture *gnome_input_capture);

gboolean gnome_input_capture_session_enable (GnomeInputCaptureSession  *gnome_input_capture_session,
                                             GError                   **error);

gboolean gnome_input_capture_session_disable (GnomeInputCaptureSession  *gnome_input_capture_session,
                                              GError                   **error);

GnomeInputCaptureSession *gnome_input_capture_create_session (GnomeInputCapture  *gnome_input_capture,
                                                              uint32_t            device_types,
                                                              GError            **error);

gboolean gnome_input_capture_session_close (GnomeInputCaptureSession  *gnome_input_capture_session,
                                            GError                   **error);

GList * gnome_input_capture_session_get_zones (GnomeInputCaptureSession  *gnome_input_capture_session,
                                               GError                   **error);

unsigned int gnome_input_capture_session_add_barrier (GnomeInputCaptureSession  *gnome_input_capture_session,
                                                      int                        x1,
                                                      int                        y1,
                                                      int                        x2,
                                                      int                        y2,
                                                      GError                   **error);

gboolean gnome_input_capture_session_clear_barriers (GnomeInputCaptureSession  *gnome_input_capture_session,
                                                     GError                   **error);

gboolean gnome_input_capture_session_release (GnomeInputCaptureSession  *gnome_input_capture_session,
                                              gboolean                   has_cursor_position,
                                              double                     cursor_position_x,
                                              double                     cursor_position_y,
                                              GError                   **error);

uint32_t gnome_input_capture_session_get_serial (GnomeInputCaptureSession *gnome_input_capture_session);

gboolean gnome_input_capture_connect_to_eis (GnomeInputCaptureSession  *gnome_input_capture_session,
                                             GUnixFDList              **fd_list,
                                             GVariant                 **fd_variant,
                                             GError                   **error);
