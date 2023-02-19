#!/bin/sh

# SPDX-FileCopyrightText: 2023 Jan Zickermann <jan.zickermann@gmail.com>
# SPDX-License-Identifier: LGPL-2.1-or-later

journalctl --user -fu xdg-desktop-portal-gnome -o cat
