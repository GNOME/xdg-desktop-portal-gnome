/*
 * Copyright © 2016 Red Hat, Inc
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
 *       Matthias Clasen <mclasen@redhat.com>
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include "utils.h"

#if defined(GDK_WINDOWING_X11) && HAVE_X11
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#endif

static const GDBusErrorEntry xdg_desktop_portal_error_entries[] = {
  { XDG_DESKTOP_PORTAL_ERROR_FAILED,           "org.freedesktop.portal.Error.Failed" },
  { XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT, "org.freedesktop.portal.Error.InvalidArgument" },
  { XDG_DESKTOP_PORTAL_ERROR_NOT_FOUND,        "org.freedesktop.portal.Error.NotFound" },
  { XDG_DESKTOP_PORTAL_ERROR_EXISTS,           "org.freedesktop.portal.Error.Exists" },
  { XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,      "org.freedesktop.portal.Error.NotAllowed" },
  { XDG_DESKTOP_PORTAL_ERROR_CANCELLED,        "org.freedesktop.portal.Error.Cancelled" },
  { XDG_DESKTOP_PORTAL_ERROR_WINDOW_DESTROYED, "org.freedesktop.portal.Error.WindowDestroyed" }
};

GQuark
xdg_desktop_portal_error_quark (void)
{
  static volatile gsize quark_volatile = 0;

  g_dbus_error_register_error_domain ("xdg-desktop-portal-error-quark",
                                      &quark_volatile,
                                      xdg_desktop_portal_error_entries,
                                      G_N_ELEMENTS (xdg_desktop_portal_error_entries));
  return (GQuark) quark_volatile;
}

glong
str_distance (const char *a,
              const char *b)
{
  g_autofree gint *v0 = NULL;
  g_autofree gint *v1 = NULL;
  const gchar *s;
  const gchar *t;
  gunichar sc;
  gunichar tc;
  glong b_char_len;
  glong cost;
  glong i;
  glong j;

  /*
  * Handle degenerate cases.
  */
  if (g_strcmp0 (a, b) == 0)
    return 0;
  else if (!*a)
    return g_utf8_strlen (a, -1);
  else if (!*b)
    return g_utf8_strlen (a, -1);

  b_char_len = g_utf8_strlen (b, -1);

  /*
   * Create two vectors to hold our states.
   */

  v0 = g_new0 (gint, b_char_len + 1);
  v1 = g_new0 (gint, b_char_len + 1);

  /*
   * initialize v0 (the previous row of distances).
   * this row is A[0][i]: edit distance for an empty a.
   * the distance is just the number of characters to delete from b.
   */
  for (i = 0; i < b_char_len + 1; i++)
    v0[i] = i;

  for (i = 0, s = a; s && *s; i++, s = g_utf8_next_char(s))
    {
      /*
       * Calculate v1 (current row distances) from the previous row v0.
       */

      sc = g_utf8_get_char(s);

      /*
       * first element of v1 is A[i+1][0]
       *
       * edit distance is delete (i+1) chars from a to match empty
       * b.
       */
      v1[0] = i + 1;

      /*
       * use formula to fill in the rest of the row.
       */
      for (j = 0, t = b; t && *t; j++, t = g_utf8_next_char(t))
        {
          tc = g_utf8_get_char(t);
          cost = (sc == tc) ? 0 : 1;
          v1[j+1] = MIN (v1[j] + 1, MIN (v0[j+1] + 1, v0[j] + cost));
        }

      /*
       * copy v1 (current row) to v0 (previous row) for next iteration.
       */
      memcpy (v0, v1, sizeof(gint) * b_char_len);
    }

  return v1[b_char_len];
}

/*
 * Checks whether a parent window handle can be used to make a portal dialog
 * transient for it.
 *
 * Applications sometimes pass a handle to an offscreen or otherwise not
 * viewable window. Using such a window as the dialog parent makes the window
 * manager keep the dialog hidden, which ends up in the request timing out,
 * and the dialog being destroyed together with the offscreen parent. If the
 * handle is unusable, callers should show the dialog without a parent.
 *
 * Handles that can not be inspected here (Wayland handles, or X11 handles on
 * a Wayland session) are considered usable and left to libgxdp to resolve.
 */
gboolean
parent_window_is_usable (const char *handle)
{
  if (handle == NULL || *handle == '\0')
    return FALSE;

#if defined(GDK_WINDOWING_X11) && HAVE_X11
  {
    const char prefix[] = "x11:";
    GdkDisplay *display;
    Display *xdisplay;
    XWindowAttributes attrs;
    unsigned long xid;
    const char *xid_str;
    char *end = NULL;
    Status status;

    if (!g_str_has_prefix (handle, prefix))
      return TRUE;

    display = gdk_display_get_default ();
    if (display == NULL || !GDK_IS_X11_DISPLAY (display))
      return TRUE;

    xid_str = handle + strlen (prefix);
    if (*xid_str == '\0')
      return FALSE;

    for (const char *p = xid_str; *p != '\0'; p++)
      {
        if (!g_ascii_isxdigit (*p))
          return FALSE;
      }

    errno = 0;
    xid = strtoul (xid_str, &end, 16);
    if (errno != 0 || end == xid_str || *end != '\0' ||
        xid == 0 || xid > G_MAXUINT32)
      return FALSE;

    xdisplay = gdk_x11_display_get_xdisplay (display);

    gdk_x11_display_error_trap_push (display);
    status = XGetWindowAttributes (xdisplay, (Window) xid, &attrs);
    if (gdk_x11_display_error_trap_pop (display) != 0)
      return FALSE;

    if (status == 0)
      return FALSE;

    if ((Window) xid == attrs.root)
      return FALSE;

    /* Input-only and override-redirect (menus, tooltips, ...) windows can
     * not be used as dialog parents. An unmapped window is typically an
     * offscreen helper window.
     */
    if (attrs.class == InputOnly || attrs.depth == 0)
      return FALSE;

    if (attrs.override_redirect)
      return FALSE;

    if (attrs.map_state != IsViewable)
      return FALSE;

    if (attrs.width <= 0 || attrs.height <= 0)
      return FALSE;

    return TRUE;
  }
#endif

  return TRUE;
}
