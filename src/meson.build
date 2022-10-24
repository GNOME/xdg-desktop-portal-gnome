gnome = import('gnome')
pkg = import('pkgconfig')

xdg_desktop_portal_dep = dependency('xdg-desktop-portal', version: '>= 1.15.0')

# Desktop Portal D-Bus interfaces
desktop_portal_interfaces_dir = xdg_desktop_portal_dep.get_pkgconfig_variable('interfaces_dir')
desktop_portal_dbus_interfaces = [
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Account.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.Print.xml',
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
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.FileChooser.xml',
  desktop_portal_interfaces_dir / 'org.freedesktop.impl.portal.DynamicLauncher.xml',
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
  top_srcdir / 'data' / 'org.freedesktop.Accounts.User.xml',
  top_srcdir / 'data' / 'org.freedesktop.Accounts.xml',
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
config_h.set_quoted('PACKAGE_NAME', meson.project_name())
config_h.set_quoted('PACKAGE_STRING', '@0@ @1@'.format(meson.project_name(), meson.project_version()))
built_sources += configure_file(output: 'config.h', configuration: config_h)

# Sources
libadwaita_dep = dependency(
  'libadwaita-1',
  fallback: ['libadwaita', 'libadwaita_dep'],
  default_options: ['examples=false', 'introspection=disabled', 'tests=false', 'vapi=false'],
)
deps = [
  declare_dependency(sources: built_sources),
  meson.get_compiler('c').find_library('m'),
  dependency('fontconfig'),
  dependency('glib-2.0', version: '>= 2.62'),
  dependency('gio-unix-2.0'),
  dependency('gtk4', version: '>= 4.0'),
  dependency('gtk4-unix-print', version: '>= 4.0'),
  dependency('gsettings-desktop-schemas'),
  dependency('gnome-desktop-4'),
  dependency('gnome-bg-4'),
  libadwaita_dep,
  xdg_desktop_portal_dep,
]

sources = built_sources + files(
  'account.c',
  'accountdialog.c',
  'appchooser.c',
  'appchooserrow.c',
  'appchooserdialog.c',
  'background.c',
  'displaystatetracker.c',
  'dynamic-launcher.c',
  'externalwindow.c',
  'fc-monitor.c',
  'filechooser.c',
  'fdonotification.c',
  'gnomescreencast.c',
  'lockdown.c',
  'print.c',
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

gtk_x11_dep = dependency('gtk4-x11', required: false)
if gtk_x11_dep.found()
  sources += files('externalwindow-x11.c')
  deps += [dependency('x11')]
  common_flags += ['-DHAVE_GTK_X11']
endif

gtk_wayland_dep = dependency('gtk4-wayland', required: false)
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

install_data (
  'xdg-desktop-portal-gnome.gschema.xml',
  install_dir : datadir / 'glib-2.0' / 'schemas',
)

gnome.post_install(glib_compile_schemas: true)
