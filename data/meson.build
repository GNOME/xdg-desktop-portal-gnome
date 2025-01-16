i18n = import('i18n')

libexecdir_conf = configuration_data()
libexecdir_conf.set('libexecdir', prefix / libexecdir)

# Portal
gnome_portal_interfaces = [
  'org.freedesktop.impl.portal.Access',
  'org.freedesktop.impl.portal.Account',
  'org.freedesktop.impl.portal.AppChooser',
  'org.freedesktop.impl.portal.Background',
  'org.freedesktop.impl.portal.Clipboard',
  'org.freedesktop.impl.portal.DynamicLauncher',
  'org.freedesktop.impl.portal.FileChooser',
  'org.freedesktop.impl.portal.GlobalShortcuts',
  'org.freedesktop.impl.portal.InputCapture',
  'org.freedesktop.impl.portal.Lockdown',
  'org.freedesktop.impl.portal.Notification',
  'org.freedesktop.impl.portal.Print',
  'org.freedesktop.impl.portal.RemoteDesktop',
  'org.freedesktop.impl.portal.ScreenCast',
  'org.freedesktop.impl.portal.Screenshot',
  'org.freedesktop.impl.portal.Settings',
  'org.freedesktop.impl.portal.Usb',
  'org.freedesktop.impl.portal.Wallpaper',
]
gnome_portal_conf = configuration_data()
gnome_portal_conf.set('PORTALS', ';'.join(gnome_portal_interfaces))

portal_dir = datadir / 'xdg-desktop-portal' / 'portals'
configure_file(
  input: 'gnome.portal.in',
  output: 'gnome.portal',
  configuration: gnome_portal_conf,
  install_dir: portal_dir,
)

# Service
configure_file(
  input: 'org.freedesktop.impl.portal.desktop.gnome.service.in',
  output: 'org.freedesktop.impl.portal.desktop.gnome.service',
  configuration: libexecdir_conf,
  install_dir: dbus_service_dir,
)

# systemd unit
if systemduserunitdir != ''
  configure_file(
    input: 'xdg-desktop-portal-gnome.service.in',
    output: 'xdg-desktop-portal-gnome.service',
    configuration: libexecdir_conf,
    install_dir: systemduserunitdir,
  )
endif

# Desktop file
desktop_in = configure_file(
  input: 'xdg-desktop-portal-gnome.desktop.in.in',
  output: 'xdg-desktop-portal-gnome.desktop.in',
  configuration: libexecdir_conf,
)

i18n.merge_file(
  type: 'desktop',
  input: desktop_in,
  output: 'xdg-desktop-portal-gnome.desktop',
  po_dir: meson.project_source_root() / 'po',
  install: true,
  install_dir: datadir / 'applications',
)
