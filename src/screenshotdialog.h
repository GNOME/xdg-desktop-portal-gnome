#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>
#include "shell-dbus.h"

G_BEGIN_DECLS

#define SCREENSHOT_TYPE_DIALOG (screenshot_dialog_get_type ())
G_DECLARE_FINAL_TYPE (ScreenshotDialog, screenshot_dialog, SCREENSHOT, DIALOG, AdwWindow)

ScreenshotDialog * screenshot_dialog_new (const char *app_id,
                                          gboolean permission_store_checked,
                                          OrgGnomeShellScreenshot *shell);

G_END_DECLS
