GtkRadiant for Apple OSX
========================

This directory provides packaging steps for GtkRadiant for OSX. This document describes compiling the application on OSX as well as generating distributable bundles using the framework provided in this directory.

Dependencies & Compilation
--------------------------

The maintained local path targets Apple silicon on macOS 11 or newer, GTK3,
and XQuartz. It uses the platform-neutral gamepacks from the official
GtkRadiant 1.6.7 distribution rather than updating historical development
gamepacks through SVN.

- Install [MacPorts](http://macports.org).
- Install [XQuartz](http://xquartz.macosforge.org/)

- Install dependencies with MacPorts:

```
sudo port install dylibbundler pkgconfig gtk3 scons
```

- Get the GtkRadiant code and compile:

```
git clone https://github.com/Josalo-III/GtkRadiant.git
cd GtkRadiant/

# Verify and stage all twelve official gamepacks once. The archive is not
# committed to the repository.
apple/stage-official-gamepacks.sh \
  "/absolute/path/GtkRadiant-1.6.7-20230820.zip"

# Build and run the editor without contacting the historical SVN gamepack
# service. On the ShaderShop branch this also builds the plugin.
apple/run-shadershop.sh
```

- Run the build:

(from the GtkRadiant/ directory)
```
apple/macos-env.sh ./install/radiant.bin
```

XQuartz note: on my configuration XQuartz doesn't automatically start for some reason. I have to open another terminal, and run the following command: `/Applications/Utilities/XQuartz.app/Contents/MacOS/X11.bin`, then start radiant. 
    
Building GtkRadiant.app
-----------------------

The `Makefile` in the 'apple/' directory produces a distributable .app bundle
using `dylibbundler`. It packages the staged official gamepacks and removes any
checkout-specific `q3.game` before the first launch:

```
make
make image
```

Getting help
------------

Get on irc: Quakenet #radiant, or ask on the mailing list, or post something on the issue tracker..
