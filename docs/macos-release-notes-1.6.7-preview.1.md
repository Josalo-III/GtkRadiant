# GtkRadiant 1.6.7 macOS arm64 Preview 1

This preview restores a native Apple-silicon GtkRadiant workflow using GTK3
and XQuartz. It is intended for testing on arm64 Macs before a notarized,
clean-machine-qualified release.

## Included

- Native arm64 GtkRadiant 1.6.7 editor.
- Native arm64 Q3Map2, Q3Map2-URT, Q3Data, and BSPC tools.
- All twelve gamepacks supplied by the official GtkRadiant 1.6.7 archive.
- GTK3 and MacPorts runtime libraries bundled inside the application.
- An Applications alias in the disk image for drag installation.

## Requirements and first launch

- Apple-silicon Mac running macOS 11 or newer.
- XQuartz 2.8.6 or a compatible later release.
- Separately installed game data; retail PAK files are not included.

Drag `GtkRadiant.app` onto the Applications alias before launching it. On first
run, choose the game and its installation directory. Preferences and generated
game descriptions are stored under `~/.radiant/1.6.7`.

The app is ad-hoc signed but not Apple-notarized. macOS may therefore require
using **Open** from the Finder context menu on first launch.

## Validation completed

- Finder launch with XQuartz already running.
- Finder launch that starts XQuartz automatically.
- Four shared legacy OpenGL 2.1 contexts using the Apple M1 Max Metal renderer.
- Loading and navigating Quake III maps with visible textures.
- Brush selection, movement, undo, free-look, orthographic views, face
  retexturing, saving, Q3Map2 compile/VIS/light, and BSPC AAS generation.

The bundle has not yet been tested on a clean Mac without the build-time
MacPorts installation. Please report packaging and XQuartz failures with the
macOS crash report and `radiant.log` when available.

SHA-256 (`GtkRadiant-1.6.7-preview.1.dmg`, published as `GtkRadiant-1.6.7.dmg`):

`b71bf657105cc2689561ba709861722e539a666ef9a00651f8d21ddad9a9ce1a`
