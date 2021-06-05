portaldir = $(datadir)/xdg-desktop-portal/portals
dist_portal_DATA = data/gnome.portal

%.service: data/%.service.in config.log
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

dbus_servicedir = $(DBUS_SERVICE_DIR)
dbus_service_in_files = data/org.freedesktop.impl.portal.desktop.gnome.service.in
dbus_service_DATA = org.freedesktop.impl.portal.desktop.gnome.service

CLEANFILES += $(dbus_service_DATA)

systemduserunit_in_files = data/xdg-desktop-portal-gnome.service.in
systemduserunit_DATA = xdg-desktop-portal-gnome.service

CLEANFILES += $(systemduserunit_DATA)

%.desktop.in: data/%.desktop.in.in
	$(AM_V_GEN) msgfmt --desktop -d po --template $< -o $@

%.desktop: %.desktop.in
	$(AM_V_GEN) $(SED) -e "s|\@libexecdir\@|$(libexecdir)|" $< > $@

desktopdir = $(datadir)/applications
desktop_in_in_files = data/xdg-desktop-portal-gnome.desktop.in.in
desktop_in_files = xdg-desktop-portal-gnome.desktop.in
desktop_DATA = xdg-desktop-portal-gnome.desktop

CLEANFILES += \
	$(desktop_in_files)	\
	$(desktop_DATA)		\
	$(NULL)

EXTRA_DIST += \
	$(dbus_service_in_files)                \
        $(systemduserunit_in_files)             \
        $(desktop_in_in_files)                  \
        data/org.gnome.Mutter.DisplayConfig.xml \
        data/org.gnome.Mutter.ScreenCast.xml    \
        data/org.gnome.Mutter.RemoteDesktop.xml \
        data/org.gnome.Shell.Introspect.xml     \
        data/org.gnome.Shell.Screenshot.xml     \
        $(NULL)
