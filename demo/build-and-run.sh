#!/bin/bash

# SPDX-FileCopyrightText: 2023 Jan Zickermann <jan.zickermann@gmail.com>
# SPDX-License-Identifier: LGPL-2.1-or-later

set -e

ROOT_DIR=$(realpath $(dirname ${BASH_SOURCE[0]})/..)
BUILD_DIR="$ROOT_DIR/build"
UNIT_OVERRIDE_FILE=$HOME/.config/systemd/user/xdg-desktop-portal-gnome.service.d/override.conf

function clear_override {
	rm -f "$UNIT_OVERRIDE_FILE"
	rmdir $(dirname "$UNIT_OVERRIDE_FILE") || true
	echo "-] Removed unit override file to '$UNIT_OVERRIDE_FILE'."
	systemctl --user daemon-reload
	systemctl --user restart xdg-desktop-portal-gnome
}

if [ ! -d "$BUILD_DIR" ]
then
	echo "Configuring build directory: '$BUILD_DIR'..."
	mkdir -p "$BUILD_DIR"
	pushd "$BUILD_DIR"
	meson setup "$ROOT_DIR"
	popd
fi

if [ ! -f "$UNIT_OVERRIDE_FILE" ]
then
	trap clear_override EXIT
	echo "-] Adding unit override file to '$UNIT_OVERRIDE_FILE' which points to '$BUILD_DIR/src/xdg-desktop-portal-gnome'..."
	mkdir -p $(dirname "$UNIT_OVERRIDE_FILE") || true
	cat <<EOF > "$UNIT_OVERRIDE_FILE"
[Service]
ExecStart=
ExecStart="$BUILD_DIR/src/xdg-desktop-portal-gnome" --verbose
EOF
	systemctl --user daemon-reload
else
	echo "Using custom unit override:"
	echo "-->>-- $UNIT_OVERRIDE_FILE"
	cat "$UNIT_OVERRIDE_FILE"
	echo "--<<-- $UNIT_OVERRIDE_FILE"
fi

ninja -C "$BUILD_DIR"
systemctl --user restart xdg-desktop-portal-gnome
$@
