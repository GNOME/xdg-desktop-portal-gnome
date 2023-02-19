#!/bin/bash

# SPDX-FileCopyrightText: 2023 Jan Zickermann <jan.zickermann@gmail.com>
# SPDX-License-Identifier: LGPL-2.1-or-later

cd $(dirname ${BASH_SOURCE[0]})

function name-override {
	echo "-] Override displaystatetracker.c: \"$1\" -> \"$2\""
	sed -i 's/"org.gnome.Mutter.DisplayConfig'$1'"/"org.gnome.Mutter.DisplayConfig'$2'"/g' ../src/displaystatetracker.c
}
./display-config-mock.py & PID=$! || ( echo "Failed to start display-config-mock.py!" && exit 1 )
trap 'name-override Mock ""' EXIT
name-override "" Mock

./build-and-run.sh "$@"
kill -s TERM $PID
