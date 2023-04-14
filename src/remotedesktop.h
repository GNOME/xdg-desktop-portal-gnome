/*
 * Copyright © 2018 Red Hat, Inc
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

#include "screencast.h"
#include "session.h"
#include "shell-dbus.h"

typedef struct _RemoteDesktopSession RemoteDesktopSession;

typedef enum _RemoteDesktopDeviceType
{
  REMOTE_DESKTOP_DEVICE_TYPE_NONE = 0,
  REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD = 1 << 0,
  REMOTE_DESKTOP_DEVICE_TYPE_POINTER = 1 << 1,
  REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN = 1 << 2,

  REMOTE_DESKTOP_DEVICE_TYPE_ALL = (REMOTE_DESKTOP_DEVICE_TYPE_POINTER |
                                    REMOTE_DESKTOP_DEVICE_TYPE_KEYBOARD |
                                    REMOTE_DESKTOP_DEVICE_TYPE_TOUCHSCREEN)
} RemoteDesktopDeviceType;

typedef enum _RemoteDesktopPersistMode
{
  REMOTE_DESKTOP_PERSIST_MODE_NONE = 0,
  REMOTE_DESKTOP_PERSIST_MODE_TRANSIENT = 1,
  REMOTE_DESKTOP_PERSIST_MODE_PERSISTENT = 2,
} RemoteDesktopPersistMode;

gboolean is_remote_desktop_session (Session *session);

void remote_desktop_session_sources_selected (RemoteDesktopSession *session,
                                              ScreenCastSelection *select);

void remote_desktop_session_request_clipboard (RemoteDesktopSession *session);

gboolean remote_desktop_session_is_clipboard_enabled (RemoteDesktopSession *session);

OrgGnomeMutterRemoteDesktopSession *
remote_desktop_session_mutter_session_proxy (RemoteDesktopSession *session);

gboolean remote_desktop_init (GDBusConnection *connection,
                              GError **error);
