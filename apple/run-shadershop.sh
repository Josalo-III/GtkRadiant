#!/bin/sh
# Build ShaderShop, remove any pre-rename artifact that would shadow it, and
# launch GtkRadiant under the deterministic macOS dev environment.
#
#   apple/run-shadershop.sh
#
# The plugin used to be named "shaderpreview"; contrib/shadershop replaced
# contrib/shaderpreview outright, but scons does not clean products of a
# source tree that no longer exists, and install/ is gitignored, so a
# pre-rename checkout can carry install/modules/shaderpreview.so forever.
# Radiant loads every module it finds, so a stale copy beside the current one
# registers a second "ShaderShop..." command and opens a second preview window
# competing for its own GL context. This script removes it as a matter of
# course rather than relying on a one-time cleanup.
#
# Pass extra scons arguments after --, e.g.:
#   apple/run-shadershop.sh -- -j1
#
# Set GTKRADIANT_DEPS_PREFIX to override the default /opt/local dependency
# prefix; see apple/macos-env.sh.

set -eu

here=$( cd "$( dirname "$0" )" && pwd )
root=$( cd "$here/.." && pwd )
cd "$root"

scons_extra=""
if [ "${1-}" = "--" ]; then
	shift
	scons_extra="$*"
fi

echo "== removing pre-rename artifacts =="
for stale in \
	install/modules/shaderpreview.so \
	build/release/shobjs/modules/libshaderpreview.dylib \
	build/release/shobjs/contrib/shaderpreview
do
	if [ -e "$stale" ]; then
		echo "  rm -rf $stale"
		rm -rf "$stale"
	fi
done

echo "== building ShaderShop =="
# shellcheck disable=SC2086
apple/macos-env.sh scons -j10 $scons_extra target=radiant \
	cc=/usr/bin/clang cxx=/usr/bin/clang++ \
	install/modules/shadershop.so

if [ -e install/modules/shaderpreview.so ]; then
	echo "error: install/modules/shaderpreview.so was recreated by the build" >&2
	echo "       something in the tree still references the old plugin name" >&2
	exit 1
fi

echo "== launching GtkRadiant =="
echo "   Plugins -> ShaderShop... opens the preview window."
echo "   First launch on a machine with no prior preferences shows the game"
echo "   selection dialog; point it at an installed Quake 3 data directory."
exec apple/macos-env.sh install/radiant.bin
