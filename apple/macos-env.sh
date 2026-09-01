#!/bin/sh

# Run one GtkRadiant command with a deterministic MacPorts/X11 environment.
# Execute this file; do not source it. The caller's shell is never modified.

case "${ZSH_EVAL_CONTEXT-}" in
  *:file)
    echo "error: execute apple/macos-env.sh; do not source it" >&2
    return 2
    ;;
esac

if [ -n "${BASH_SOURCE-}" ] && [ "${BASH_SOURCE}" != "$0" ]; then
  echo "error: execute apple/macos-env.sh; do not source it" >&2
  return 2
fi

set -eu

deps_prefix=${GTKRADIANT_DEPS_PREFIX:-/opt/local}
pkg_config="$deps_prefix/bin/pkg-config"
isolated_path="$deps_prefix/bin:$deps_prefix/sbin:/usr/bin:/bin:/usr/sbin:/sbin"
pkg_config_libdir="$deps_prefix/lib/pkgconfig:$deps_prefix/share/pkgconfig"

usage() {
  echo "usage: apple/macos-env.sh --check" >&2
  echo "       apple/macos-env.sh COMMAND [ARG ...]" >&2
}

run_isolated() {
  /usr/bin/env -i \
    HOME="${HOME-}" \
    USER="${USER-}" \
    LOGNAME="${LOGNAME-${USER-}}" \
    TMPDIR="${TMPDIR-/tmp}" \
    LANG="${LANG-en_US.UTF-8}" \
    PATH="$isolated_path" \
    PKG_CONFIG="$pkg_config" \
    PKG_CONFIG_LIBDIR="$pkg_config_libdir" \
    PYTHONNOUSERSITE=1 \
    GDK_BACKEND=x11 \
    DISPLAY="${DISPLAY-}" \
    XAUTHORITY="${XAUTHORITY-}" \
    DEVELOPER_DIR="${DEVELOPER_DIR-/Applications/Xcode.app/Contents/Developer}" \
    "$@"
}

check_environment() {
  status=0

  if [ "$(/usr/bin/uname -m)" != "arm64" ]; then
    echo "error: host architecture is not arm64" >&2
    status=1
  fi

  for tool in "$pkg_config" /usr/bin/clang /usr/bin/clang++; do
    if [ ! -x "$tool" ]; then
      echo "error: required tool is not executable: $tool" >&2
      status=1
    fi
  done

  if [ "$status" -ne 0 ]; then
    return "$status"
  fi

  echo "dependency prefix: $deps_prefix"
  echo "PATH: $isolated_path"
  echo "PKG_CONFIG_LIBDIR: $pkg_config_libdir"
  echo "compiler target: $(run_isolated /usr/bin/clang -dumpmachine)"

  for package in gtk+-3.0 gdk-x11-3.0 gl glx x11 pangoft2 libxml-2.0 glib-2.0 libpng zlib; do
    if ! version=$(run_isolated "$pkg_config" --modversion "$package" 2>/dev/null); then
      echo "error: pkg-config package is unavailable: $package" >&2
      status=1
      continue
    fi

    prefix=$(run_isolated "$pkg_config" --variable=prefix "$package")
    case "$prefix" in
      "$deps_prefix"|"$deps_prefix"/*)
        echo "$package $version ($prefix)"
        ;;
      *)
        echo "error: $package resolved outside $deps_prefix: $prefix" >&2
        status=1
        ;;
    esac
  done

  if run_isolated "$pkg_config" --exists gdk-quartz-3.0; then
    echo "error: gdk-quartz-3.0 is visible in the X11 environment" >&2
    status=1
  else
    echo "GDK backend: X11 only"
  fi

  if [ -n "${DISPLAY-}" ]; then
    echo "DISPLAY: ${DISPLAY}"
  else
    echo "DISPLAY: not set (builds work; GUI launch requires XQuartz)"
  fi

  return "$status"
}

if [ "$#" -eq 0 ]; then
  usage
  exit 2
fi

if [ "$1" = "--check" ]; then
  if [ "$#" -ne 1 ]; then
    usage
    exit 2
  fi
  check_environment
  exit $?
fi

case "$1" in
  -h|--help)
    usage
    exit 0
    ;;
esac

exec /usr/bin/env -i \
  HOME="${HOME-}" \
  USER="${USER-}" \
  LOGNAME="${LOGNAME-${USER-}}" \
  TMPDIR="${TMPDIR-/tmp}" \
  LANG="${LANG-en_US.UTF-8}" \
  PATH="$isolated_path" \
  PKG_CONFIG="$pkg_config" \
  PKG_CONFIG_LIBDIR="$pkg_config_libdir" \
  PYTHONNOUSERSITE=1 \
  GDK_BACKEND=x11 \
  DISPLAY="${DISPLAY-}" \
  XAUTHORITY="${XAUTHORITY-}" \
  DEVELOPER_DIR="${DEVELOPER_DIR-/Applications/Xcode.app/Contents/Developer}" \
  "$@"
