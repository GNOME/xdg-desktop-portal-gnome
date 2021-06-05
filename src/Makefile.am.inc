dbus_built_sources = src/xdg-desktop-portal-dbus.c src/xdg-desktop-portal-dbus.h
BUILT_SOURCES += $(dbus_built_sources)
CLEANFILES += $(dbus_built_sources)

$(dbus_built_sources): src/Makefile.am.inc
	$(AM_V_GEN) $(GDBUS_CODEGEN)			\
	--interface-prefix org.freedesktop.impl.portal.	\
	--c-namespace XdpImpl				\
	--generate-c-code src/xdg-desktop-portal-dbus	\
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.Request.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.Session.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.AppChooser.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.Screenshot.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.ScreenCast.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.RemoteDesktop.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.Lockdown.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.Background.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.Settings.xml \
	 $(DESKTOP_PORTAL_INTERFACES_DIR)/org.freedesktop.impl.portal.Wallpaper.xml \
	$(NULL)

shell_built_sources = src/shell-dbus.c src/shell-dbus.h
BUILT_SOURCES += $(shell_built_sources)
CLEANFILES += $(shell_built_sources)

$(shell_built_sources): src/Makefile.am.inc
	$(AM_V_GEN) $(GDBUS_CODEGEN) \
	--generate-c-code src/shell-dbus \
	$(top_srcdir)/data/org.gnome.Mutter.DisplayConfig.xml \
	$(top_srcdir)/data/org.gnome.Mutter.ScreenCast.xml \
	$(top_srcdir)/data/org.gnome.Mutter.RemoteDesktop.xml \
	$(top_srcdir)/data/org.gnome.Shell.Screenshot.xml \
	$(top_srcdir)/data/org.gnome.Shell.Introspect.xml \
	$(NULL)

resource_files = $(shell $(GLIB_COMPILE_RESOURCES) --sourcedir=$(top_srcdir)/src --generate-dependencies $(top_srcdir)/src/xdg-desktop-portal-gnome.gresource.xml)

src/resources.c: src/xdg-desktop-portal-gnome.gresource.xml $(resource_files)
	$(AM_V_GEN) $(GLIB_COMPILE_RESOURCES) $< \
		--target=$@ --sourcedir=$(top_srcdir)/src --c-name _xdg_desktop --generate-source
CLEANFILES += src/resources.c

EXTRA_DIST += \
        src/appchooserdialog.ui                         \
        src/appchooserrow.ui                            \
        src/remotedesktopdialog.ui                      \
        src/screenshotdialog.ui                         \
        src/screenshotdialog.css                        \
        src/screencastwidget.ui                         \
        src/screencastdialog.ui                         \
        src/wallpaperdialog.ui                          \
        src/wallpaperpreview.ui                         \
        src/wallpaperpreview.css                        \
	src/xdg-desktop-portal-gnome.gresource.xml	\
	$(NULL)

libexec_PROGRAMS = \
	xdg-desktop-portal-gnome \
	$(NULL)

xdg_desktop_portal_gnome_SOURCES = \
	src/appchooser.h			\
	src/appchooser.c			\
	src/appchooserrow.h			\
	src/appchooserrow.c			\
	src/appchooserdialog.h		        \
	src/appchooserdialog.c		        \
        src/background.c                        \
        src/background.h                        \
	src/displaystatetracker.c		\
	src/displaystatetracker.h		\
	src/externalwindow.h			\
	src/externalwindow.c			\
        src/fc-monitor.c                        \
        src/fc-monitor.h                        \
	src/fdonotification.h			\
	src/fdonotification.c			\
	src/gnomescreencast.c			\
	src/gnomescreencast.h			\
	src/lockdown.c				\
	src/lockdown.h				\
	src/remotedesktop.c			\
	src/remotedesktop.h			\
	src/remotedesktopdialog.c		\
	src/remotedesktopdialog.h		\
	src/request.h			        \
	src/request.c			        \
	src/screenshot.h			\
	src/screenshot.c			\
	src/screenshotdialog.h		        \
	src/screenshotdialog.c		        \
	src/screencast.c			\
	src/screencast.h			\
	src/screencastwidget.c			\
	src/screencastwidget.h			\
	src/screencastdialog.c			\
	src/screencastdialog.h			\
	src/session.c   			\
	src/session.h	        		\
        src/settings.c                          \
        src/settings.h                          \
	src/shellintrospect.c			\
	src/shellintrospect.h			\
	src/utils.h				\
	src/utils.c				\
	src/wallpaperdialog.c			\
	src/wallpaperdialog.h			\
	src/wallpaperpreview.c			\
	src/wallpaperpreview.h			\
	src/wallpaper.c				\
	src/wallpaper.h				\
	src/xdg-desktop-portal-gnome.c	        \
	$(NULL)

nodist_xdg_desktop_portal_gnome_SOURCES = \
	src/resources.c				\
	$(dbus_built_sources)			\
	$(shell_built_sources)			\
	$(NULL)

if HAVE_GTK_X11
xdg_desktop_portal_gnome_SOURCES += \
	src/externalwindow-x11.h		\
	src/externalwindow-x11.c		\
	$(NULL)
endif

if HAVE_GTK_WAYLAND
xdg_desktop_portal_gnome_SOURCES += \
	src/externalwindow-wayland.h		\
	src/externalwindow-wayland.c		\
	$(NULL)
endif

xdg_desktop_portal_gnome_LDADD = $(BASE_LIBS) $(GTK_LIBS) $(GTK_X11_LIBS)
xdg_desktop_portal_gnome_CFLAGS = $(BASE_CFLAGS) $(GTK_CFLAGS) $(GTK_X11_CFLAGS)
xdg_desktop_portal_gnome_CPPFLAGS = \
	-DGETTEXT_PACKAGE=\"$(GETTEXT_PACKAGE)\"        \
	-DLOCALEDIR=\"$(localedir)\"                    \
	-I$(top_srcdir)/src				\
	-I$(top_builddir)/src				\
	$(NULL)

noinst_PROGRAMS = \
        testappchooser

testappchooser_LDADD = $(GTK_LIBS) $(GTK_X11_LIBS)
testappchooser_CFLAGS = $(GTK_CFLAGS) $(GTK_X11_CFLAGS)
testappchooser_CPPFLAGS = \
	-DGETTEXT_PACKAGE=\"$(GETTEXT_PACKAGE)\"        \
	-DLOCALEDIR=\"$(localedir)\"                    \
	-I$(top_srcdir)/src				\
	-I$(top_builddir)/src				\
	$(NULL)

testappchooser_SOURCES = \
        src/testappchooser.c                    \
	src/appchooserrow.h			\
	src/appchooserrow.c			\
	src/appchooserdialog.h		        \
	src/appchooserdialog.c		        \
        $(NULL)

nodist_testappchooser_SOURCES = \
	src/resources.c				\
	$(NULL)
