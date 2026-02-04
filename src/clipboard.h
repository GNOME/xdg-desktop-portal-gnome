/*
 * Copyright 2022 Google LLC
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

#include <gio/gio.h>
#include <glib.h>

#include "shell-dbus.h"

#include "session.h"

G_DECLARE_INTERFACE (ClipboardSession, clipboard_session,
                     CLIPBOARD, SESSION, GObject)

struct _ClipboardSessionInterface
{
  GTypeInterface parent_iface;

  void (* request_clipboard) (ClipboardSession *clipboard_session);
  OrgGnomeMutterClipboard * (* get_clipboard_proxy) (ClipboardSession *clipboard_session);
};

gboolean is_clipboard_session (Session *session);

gboolean clipboard_init (GDBusConnection *connection, GError **error);

gboolean clipboard_add_session (ClipboardSession *clipboard_session);

void clipboard_remove_session (ClipboardSession *clipboard_session);
