gnome = import('gnome')
pkg = import('pkgconfig')

xdg_desktop_portal_dep = dependency('xdg-desktop-portal', version: '>= 1.5')

# Desktop Portal D-Bus interfaces
desktop_portal_interfaces_dir = xdg_desktop_portal_dep.get_pkgconfig_variable('interfaces_dir')
desktop_portal_dbus_interfaces = [
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Request.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Session.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.AppChooser.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Screenshot.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.ScreenCast.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.RemoteDesktop.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Lockdown.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Background.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Settings.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Wallpaper.xml',
]

built_sources = gnome.gdbus_codegen(
  'xdg-desktop-portal-dbus',
  sources: desktop_portal_dbus_interfaces,
  interface_prefix: 'org.freedesktop.impl.portal.',
  namespace: 'XdpImpl',
)

# Mutter / GNOME Shell D-Bus interfaces
top_srcdir = meson.source_root()
shell_dbus_interfaces = files(
	top_srcdir / 'data' / 'org.gnome.Mutter.DisplayConfig.xml',
	top_srcdir / 'data' / 'org.gnome.Mutter.RemoteDesktop.xml',
	top_srcdir / 'data' / 'org.gnome.Mutter.ScreenCast.xml',
	top_srcdir / 'data' / 'org.gnome.Shell.Introspect.xml',
	top_srcdir / 'data' / 'org.gnome.Shell.Screenshot.xml',
)

built_sources += gnome.gdbus_codegen(
  'shell-dbus',
  sources: shell_dbus_interfaces,
  interface_prefix: 'org.freedesktop.impl.portal.',
)

# GResources
built_sources += gnome.compile_resources(
  'xdg-desktop-portal-gnome-resources',
  'xdg-desktop-portal-gnome.gresource.xml',
  c_name: '_xdg_desktop',
)

# config.h
config_h = configuration_data()
config_h.set_quoted('GETTEXT_PACKAGE', meson.project_name())
config_h.set_quoted('LOCALEDIR', prefix / get_option('localedir'))
config_h.set_quoted('PACKAGE_STRING', '@0@ @1@'.format(meson.project_name(), meson.project_version()))
built_sources += configure_file(output: 'config.h', configuration: config_h)

# Sources
deps = [
  declare_dependency(sources: built_sources),
  dependency('fontconfig'),
  dependency('glib-2.0', version: '>= 2.44'),
  dependency('gio-unix-2.0'),
  dependency('gtk+-3.0', version: '>= 3.14'),
  dependency('gnome-desktop-3.0'),
  dependency('gsettings-desktop-schemas'),
  xdg_desktop_portal_dep,
]

sources = built_sources + files(
  'appchooser.c',
  'appchooserrow.c',
  'appchooserdialog.c',
  'background.c',
  'displaystatetracker.c',
  'externalwindow.c',
  'fc-monitor.c',
  'fdonotification.c',
  'gnomescreencast.c',
  'lockdown.c',
  'remotedesktop.c',
  'remotedesktopdialog.c',
  'request.c',
  'screenshot.c',
  'screenshotdialog.c',
  'screencast.c',
  'screencastwidget.c',
  'screencastdialog.c',
  'session.c',
  'settings.c',
  'shellintrospect.c',
  'utils.c',
  'wallpaperdialog.c',
  'wallpaperpreview.c',
  'wallpaper.c',
  'xdg-desktop-portal-gnome.c',
)

common_flags = []

gtk_x11_dep = dependency('gtk+-x11-3.0', required: false)
if gtk_x11_dep.found()
  sources += files('externalwindow-x11.c')
  common_flags += ['-DHAVE_GTK_X11']
endif

gtk_wayland_dep = dependency('gtk+-wayland-3.0', version: '>= 3.21.5', required: false)
if gtk_wayland_dep.found()
  sources += files('externalwindow-wayland.c')
  common_flags += ['-DHAVE_GTK_WAYLAND']
endif

add_project_arguments(common_flags, language: 'c')

executable(
  'xdg-desktop-portal-gnome',
  sources,
  dependencies: deps,
  install: true,
  install_dir: libexecdir,
)
