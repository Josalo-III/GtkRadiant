# Makefile for GtkRadiant.app, requires http://macdylibbundler.sourceforge.net/

BUILD = ../build
INSTALL = ../install
TARGET = target
RESOURCES = $(TARGET)/GtkRadiant.app/Contents/Resources
BINDIR = $(RESOURCES)/install
CONFDIR = $(RESOURCES)/etc
DATADIR = $(RESOURCES)/share
LIBDIR = $(RESOURCES)/lib
VERSION = 1.6.7
# Preview releases are distinguished in the artifact name. The release notes
# publish a SHA-256 per preview, so two builds must never share a filename:
# a fixed name silently replaces the artifact an earlier note still refers to.
PREVIEW = 2
DMG = $(TARGET)/GtkRadiant-$(VERSION)-preview.$(PREVIEW).dmg
DMGROOT = $(TARGET)/dmg-root
VOLUME_NAME = "GtkRadiant $(VERSION)"

# We must rewrite some Gtk configuration files, which vary from environment to
# environment depending on where you've installed MacPorts. Fun!

MACPORTS_PREFIX := $(shell which port | sed 's:/bin/port::')
PREFIX_SED_EXPR = "s:$(MACPORTS_PREFIX):@executable_path/..:g"

all: install bundle

-pre-install:
	install -d $(TARGET)
	cp -r GtkRadiant.app $(TARGET)
	find $(TARGET) -name .turd -delete
	
-gtk-runtime-gdk-pixbuf-2.0:
	cp -r $(MACPORTS_PREFIX)/lib/gdk-pixbuf-2.0 $(LIBDIR)
	find $(LIBDIR)/gdk-pixbuf-2.0 -type f ! -name "*.so" -delete
	
	install -d $(CONFDIR)/gtk-3.0
	gdk-pixbuf-query-loaders | sed $(PREFIX_SED_EXPR) > \
		$(CONFDIR)/gtk-3.0/gdk-pixbuf.loaders
	
-gtk-runtime: -gtk-runtime-gdk-pixbuf-2.0
	cp -r $(MACPORTS_PREFIX)/lib/gtk-3.0 $(LIBDIR)
	find $(LIBDIR)/gtk-3.0 -type f ! -name "*.so" -delete
	
	rm -rf $(LIBDIR)/gtk-3.0/{includes,modules}
	rm -rf $(LIBDIR)/gtk-3.0/*/printbackends
	
	cp -r $(MACPORTS_PREFIX)/share/themes/Default $(RESOURCES)/share

install: -pre-install -gtk-runtime
	test -d $(INSTALL)/installs/Q3Pack
	cp -r $(INSTALL) $(RESOURCES)
	rm -rf `find $(RESOURCES)/install -type d -name .svn`
	# Game paths are per-user state.  The bundled Q3Pack drives the first-run
	# configuration dialog; never publish a developer's generated q3.game.
	rm -f $(RESOURCES)/install/games/q3.game

bundle:
	
	# The Radiant plugins (modules) are a little funky
	# Some of them are actually linked against the build directory
	
	test -L build || ln -s $(BUILD)
	
	dylibbundler -b \
		-x $(BINDIR)/radiant.bin \
		-x $(BINDIR)/q3map2 \
		-x $(BINDIR)/q3map2_urt \
		-x $(BINDIR)/bspc \
		-x $(BINDIR)/q3data \
		`find $(BINDIR)/modules -name "*.so" | xargs -I {} echo -x {}` \
		`find $(LIBDIR) -name "*.so" | xargs -I {} echo -x {}` \
	-d $(LIBDIR) -of -p @executable_path/../lib
	
	rm -f build
	# Seal the completed application after dylibbundler has rewritten and signed
	# its individual Mach-O files.  This is ad-hoc signing; public notarization
	# remains a separate Apple Developer ID release step.
	codesign --force --deep --sign - $(TARGET)/GtkRadiant.app

image:
	rm -rf $(DMGROOT)
	install -d $(DMGROOT)
	cp -R $(TARGET)/GtkRadiant.app $(DMGROOT)
	ln -s /Applications $(DMGROOT)/Applications
	find $(DMGROOT) -name .DS_Store -delete
	hdiutil create -ov $(DMG) -srcfolder $(DMGROOT) -volname $(VOLUME_NAME)
	rm -rf $(DMGROOT)

# Clean the bundle and staging directory but keep built disk images: they are
# the release artifacts, they take a long time to produce, and an earlier
# preview's image must survive the next preview's build.
clean:
	rm -rf $(TARGET)/GtkRadiant.app $(DMGROOT)

distclean:
	rm -rf $(TARGET)/*
