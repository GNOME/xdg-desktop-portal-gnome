#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2023 Jan Zickermann <jan.zickermann@gmail.com>
# SPDX-License-Identifier: LGPL-2.1-or-later

import gi
from gi.repository import GLib

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop

DBusGMainLoop(set_as_default=True)

OPATH = "/org/gnome/Mutter/DisplayConfig"
IFACE = "org.gnome.Mutter.DisplayConfig"
BUS_NAME = "org.gnome.Mutter.DisplayConfigMock"

DISPLAY_CONFIGS = [
    [
        ((1280, 1024), (0, 0), 1.0, False, True, True),
    ],
    [
        ((1920, 1080), (0, 0), 1.0, False, False, True),
        ((1024, 1365), (1920, 0), 2.0, True, False, False),
        ((2048, 1536), (1920+1365, 100), 0.5, False, False, False)
    ],
    [
        ((2560, 1440), (-1440, 1080 - 2560), 1.0, True, False, False),
        ((1440, 1080), (0, 0), 1.0, False, False, False),
        ((1680, 1050), (-840, 1080), 1.0, False, False, True),
        ((1680, 1050), (-1680//2, 1080+1050), 0.8, False, True, False)
    ],
    *[[
        ((800, 600), (x * 800, y * 600), 1.0, False, False, False) for x in range(n) for y in range (n)
    ] for n in range(12)],
    *[[
        ((800, 600), (0, i * 600), 1.0, False, False, False) for i in range(n*2)
    ] for n in range(6)],
]


def display_mock(num, wh, xy, scale=1.0, vertical=False, builtin=False, primary=False):
    monitor_id = (f'connector{num}', 'vendor0', 'product0', f'{num:03d}')
    monitor = (
        monitor_id,
        [
            ('default', wh[0], wh[1],  60.0,
             1.0, [], {'is-current': True})
        ],
        {'display-name':
            f'{"Primary " if primary else ""}{"Build-in " if builtin else ""}Test Display {num} ({wh[0]}x{wh[1]})', 'is-builtin': builtin}
    )
    logical_display = (xy[0], xy[1], scale,
                       1 if vertical else 0, primary, [monitor_id], {})
    return monitor, logical_display


class DisplayConfigMock(dbus.service.Object):
    def __init__(self):
        bus = dbus.SessionBus()
        bus.request_name(BUS_NAME)
        bus_name = dbus.service.BusName(BUS_NAME, bus=bus)
        dbus.service.Object.__init__(self, bus_name, OPATH)

        self.t = 0
        self.timeout_id = 0

        def cycleStates():
            if self.timeout_id != 0:
                GLib.source_remove(self.timeout_id)
                self.t += 1

            self.timeout_id = GLib.timeout_add(1000 if self.t % len(DISPLAY_CONFIGS) > 2 else 5000, cycleStates)
            self.MonitorsChanged()
        cycleStates()

    @dbus.service.method(dbus_interface=IFACE,
                         in_signature="",
                         out_signature="ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv}"
                         )
    def GetCurrentState(self):
        displays = DISPLAY_CONFIGS[self.t % len(DISPLAY_CONFIGS)]

        def transpose(l):
            return [list(ll) for ll in zip(*l)]
        monitors = []
        logical_monitors = []
        duplicated_monitors = []
        if len(displays):
            monitors, logical_monitors = transpose([
                display_mock(i, *display) for i, display in enumerate(displays)
            ])
            builtin_indices = [i for i, m in enumerate(
                monitors) if m[2]['is-builtin']]
            if builtin_indices:
                builtin_monitors = [monitors[i] for i in builtin_indices]
                logical_monitors_with_builtin = [
                    logical_monitors[i] for i in builtin_indices]
                # Mock a duplicated monitor for each builtin monitor
                duplicated_monitors, _ = transpose([display_mock(
                    i+len(monitors), m[1][0][1:3], (0, 0)) for i, m in enumerate(builtin_monitors)])
                for lm, dm in zip(logical_monitors_with_builtin, duplicated_monitors):
                    lm[-2].append(dm[0])

        return 100, monitors + duplicated_monitors, logical_monitors, {'layout-mode': 2}

    @dbus.service.signal(dbus_interface=IFACE)
    def MonitorsChanged(self):
        pass


if __name__ == "__main__":
    a = DisplayConfigMock()
    loop = GLib.MainLoop()
    loop.run()
