#!/bin/sh
# Stage the platform-neutral gamepacks from the official GtkRadiant 1.6.7
# distribution.  This deliberately avoids SCons' historical SVN fetch path:
# those live development trees are neither reproducible nor required to build
# the macOS editor.

set -eu

expected_sha256='a35cdc561c52260cc19917c200451d1e48734455b307efabd8398167f3ee8779'
archive_root='GtkRadiant-1.6.7-20230820'

usage(){
	printf '%s\n' "usage: $0 [--check] /absolute/path/GtkRadiant-1.6.7-20230820.zip" >&2
	exit 2
}

check_only=false
if [ "${1-}" = '--check' ]; then
	check_only=true
	shift
fi
[ "$#" -eq 1 ] || usage

archive=$1
[ -f "$archive" ] || { printf '%s\n' "error: archive is not a file: $archive" >&2; exit 1; }

actual_sha256=$( shasum -a 256 "$archive" | awk '{print $1}' )
if [ "$actual_sha256" != "$expected_sha256" ]; then
	printf '%s\n' "error: unexpected archive SHA-256: $actual_sha256" >&2
	exit 1
fi

here=$( CDPATH= cd -- "$( dirname -- "$0" )" && pwd )
root=$( CDPATH= cd -- "$here/.." && pwd )
target="$root/install/installs"
stage=$( mktemp -d "${TMPDIR:-/private/tmp}/gtkradiant-gamepacks.XXXXXX" )

cleanup(){
	rm -rf "$stage"
}
trap cleanup EXIT HUP INT TERM

unzip -tqq "$archive"
unzip -q "$archive" "$archive_root/installs/*" -d "$stage"
source="$stage/$archive_root/installs"
[ -d "$source/Q3Pack/install" ] || { printf '%s\n' 'error: official Q3Pack is missing from archive' >&2; exit 1; }

if [ "$check_only" = true ]; then
	if [ ! -d "$target" ]; then
		printf '%s\n' "gamepacks are absent; run $0 '$archive' to stage them"
		exit 1
	fi
	if diff -qr -x .DS_Store "$target" "$source"; then
		printf '%s\n' 'installed gamepacks match the official GtkRadiant 1.6.7 archive'
		exit 0
	fi
	printf '%s\n' 'installed gamepacks differ from the official archive; rerun without --check to replace them' >&2
	exit 1
fi

mkdir -p "$root/install"
backup="$stage/previous-installs"
if [ -e "$target" ]; then
	mv "$target" "$backup"
fi

if ! mv "$source" "$target"; then
	if [ -e "$backup" ]; then mv "$backup" "$target"; fi
	printf '%s\n' 'error: could not install official gamepacks; restored the previous gamepacks' >&2
	exit 1
fi

find "$target" -name .DS_Store -delete
rm -rf "$backup"
printf '%s\n' 'staged all official GtkRadiant 1.6.7 gamepacks under install/installs'
