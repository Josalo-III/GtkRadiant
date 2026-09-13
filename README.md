ShaderShop for GtkRadiant
==========================

ShaderShop is a GtkRadiant branch for visual Quake III shader work. Its native
plugin previews selected shader stages and supports authoring ordered draft
shaders directly in the editor, while retaining GtkRadiant's level editor, map
compilers, and data-authoring tools.

The preview and draft-authoring workflow is ready for use. Editing an existing
shader definition is intentionally not presented as a lossless round-trip yet:
comments, whitespace, unknown directives, and original spelling must remain
preserved before that capability is claimed. See the
[ShaderShop plan](docs/shadershop-plan.md) for the compatibility contract and
remaining scope.

Downloads
---------

Apple-silicon macOS preview builds are published on this repository's
[Releases page](https://github.com/Josalo-III/GtkRadiant/releases). They bundle
the ShaderShop plugin and the official GtkRadiant 1.6.7 gamepacks.

Useful links
------------

- [ShaderShop plan and compatibility notes](docs/shadershop-plan.md)
- [GtkRadiant documentation](https://icculus.org/gtkradiant/documentation.html)
- [Upstream GtkRadiant](https://github.com/TTimo/GtkRadiant)

Supported games
---------------

GtkRadiant provides level editing support for [Quake](https://en.wikipedia.org/wiki/Quake_(video_game)), [Quake2](https://en.wikipedia.org/wiki/Quake_II), [Quake2 Re-Release](https://en.wikipedia.org/wiki/Quake_II#Enhanced_version_and_Call_of_the_Machine), [Quake III Arena](https://ioquake3.org), [QuakeLive](https://www.quakelive.com), [Quetoo](http://quetoo.org), [Return to Castle Wolfenstein](https://en.wikipedia.org/wiki/Return_to_Castle_Wolfenstein), [Star Trek Voyager: Elite Force](https://en.wikipedia.org/wiki/Star_Trek:_Voyager_–_Elite_Force), [Star Wars Jedi Knight: Jedi Academy](https://en.wikipedia.org/wiki/Star_Wars_Jedi_Knight:_Jedi_Academy), [Unvanquished](https://www.unvanquished.net), [Urban Terror](http://urbanterror.info), [Wolfenstein: Enemy Territory](http://www.splashdamage.com/content/wolfenstein-enemy-territory-barracks).

How to build
------------

The maintained Apple-silicon macOS path, including the packaged application,
is documented [here](apple/README.md). It builds ShaderShop and uses the
official gamepacks rather than the historical live-SVN development packs.

For other platforms, install the usual GtkRadiant build dependencies, stage the
official `GtkRadiant-1.6.7-20230820.zip` gamepacks once, then build without
contacting the retired pack-fetch service:

The Linux version is developed and distributed via Flatpak. See [GtkRadiant on Flathub](https://flathub.org/apps/io.github.TTimo.GtkRadiant).

Building locally on Arch is the only setup that's likely to work as it's my daily driver. Ubuntu, Fedora etc. you are on your own. See the Flatpak SDK section below.

```sh
# ArchLinux
pacman -S git scons libxml2 gtk2 freeglut gtkglext subversion libjpeg-turbo
```

```sh
# get ShaderShop
git clone "https://github.com/Josalo-III/GtkRadiant.git"

# enter the source tree
cd GtkRadiant

# stage the verified official gamepacks (archive is not committed)
apple/stage-official-gamepacks.sh /absolute/path/GtkRadiant-1.6.7-20230820.zip

# build everything without fetching development packs
scons --no-packs
```

You can build a specific part like this:

```sh
# only build the GtkRadiant level editor
scons --no-packs target="radiant"

# only build the q3map2 map compiler and the q3data tool
scons --no-packs target="q3map2,q3data"
```

Level editor binary (`radiant`) and tools (like `q3map2`) will be found in `install/` directory. 
Use `scons --no-packs` after staging the official gamepacks. The default SCons
pack fetcher targets historical development sources and is not part of the
ShaderShop build path.

Building on Linux with the Flatpak SDK
--------------------------------------

```sh
# get the flatpak manifest
git clone "https://github.com/flathub/io.github.TTimo.GtkRadiant.git"

# use flatpak-builder
cd io.github.TTimo.GtkRadiant
flatpak-builder --force-clean --user --install gtkradiant io.github.TTimo.GtkRadiant.json
```

You can also checkout the GtkRadiant tree locally and [modify the manifest to point to it](https://gist.github.com/TTimo/fd683b368048dc8802e9aaf353ef68ec).

Here is what you can do for debugging:

```sh
flatpak run --command=sh --devel io.github.TTimo.GtkRadiant
gdb /app/gtkradiant/radiant.bin
```

Getting in touch
----------------

The [#radiant channel at QuakeNet](https://webchat.quakenet.org/?channels=radiant) is the official GtkRadiant IRC channel. Come and chat about level design, development or bugs, you're welcome. Bugs can be submitted on the [GitHub issue tracker](https://github.com/TTimo/GtkRadiant/issues).

Legal
-----

GtkRadiant source code is copyrighted by [id Software, Inc](http://idsoftware.com/) and various contributors and protected by the [General Public License v2](GPL).
