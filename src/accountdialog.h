#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define ACCOUNT_TYPE_DIALOG (account_dialog_get_type())
G_DECLARE_FINAL_TYPE (AccountDialog, account_dialog, ACCOUNT, DIALOG, AdwWindow)

AccountDialog * account_dialog_new (const char *app_id,
                                    const char *user_name,
                                    const char *real_name,
                                    const char *icon_file,
                                    const char *reason);

const char *account_dialog_get_user_name (AccountDialog *dialog);
const char *account_dialog_get_real_name (AccountDialog *dialog);
const char *account_dialog_get_icon_file (AccountDialog *dialog);

G_END_DECLS
