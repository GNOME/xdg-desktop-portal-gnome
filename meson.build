project(
  'xdg-desktop-portal-gnome',
  'c',
  version: '50.alpha',
  meson_version: '>= 0.59.0',
  license: 'LGPLv2.1+'
)

prefix = get_option('prefix')
datadir = get_option('datadir')
libdir = get_option('libdir')
libexecdir = get_option('libexecdir')

subproject('libgxdp',
  default_options: 'tests=false',
)

build_systemd_service = get_option('systemd')
systemduserunitdir = get_option('systemduserunitdir')
if systemduserunitdir == ''
  systemd = dependency('systemd', version: '>= 242', required: build_systemd_service)
  if build_systemd_service.allowed() and systemd.found()
    systemduserunitdir = systemd.get_variable(
      pkgconfig: 'systemduserunitdir',
      pkgconfig_define: ['prefix', prefix]
    )
  endif
endif

dbus_service_dir = get_option('dbus_service_dir')
if dbus_service_dir == ''
  dbus_service_dir = datadir / 'dbus-1' / 'services'
endif

subdir('data')
subdir('po')
subdir('src')

summary({
  'prefix': prefix,
  'datadir': datadir,
  'libdir': libdir,
  'libexecdir': libexecdir,
  'dbus_service_dir': dbus_service_dir,
})

summary({
  'systemd-service': build_systemd_service,
  'systemduserunitdir': systemduserunitdir,
}, section: 'systemd session service')
