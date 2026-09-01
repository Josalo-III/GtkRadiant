# GtkRadiant macOS Port Plan

Status: implementation underway, 31 August 2026

## Purpose

The objective is not merely to make GtkRadiant compile or display a window on
macOS. The objective is to recover a trustworthy Quake III Arena mapping
environment on Apple Silicon, capable of loading, editing, saving, compiling,
and running large production maps without corrupting their source data.

The immediate development machine is an M1 Max MacBook Pro running arm64
macOS. The initial windowing target is X11 through XQuartz. Native macOS UI
integration may be considered later, but it must not delay recovery of a usable
editor.

## Background and motivation

The original Mac releases of Quake III Arena and its mapping tools arrived late
and were difficult to run reliably. Early mapping work was possible on a Mac,
but increasingly complex maps eventually required moving development to a
Windows 7 installation. That environment is currently on an offsite SSD, while
the map sources and assets are available in redundant local backups.

This history sets a higher bar than basic platform compatibility. The port must
be evaluated against the real projects that the editor is intended to recover.

## Guiding principles

1. Never modify the original map backup during port development or testing.
2. Never alter or destabilize the existing `ld-decode` development
   environment.
3. Prefer a coherent dependency stack over whichever packages happen to be
   installed locally.
4. Remove obsolete dependencies where the replacement has a bounded scope.
5. Keep file-format and compiler behavior separate from UI modernization.
6. Establish measurable correctness and performance baselines early.
7. Treat a successful launch as a diagnostic milestone, not completion.
8. Keep the Linux and Windows implementations buildable while modernizing the
   shared code.

## Upstream and cross-platform strategy

The Mac recovery work is being developed from the current upstream
`1.6-release` branch, which is also the upstream repository's default branch.
The aim is not to create a permanently divergent Mac fork. Shared GTK and
rendering modernization should be suitable for upstream use on Linux, BSD,
Windows, and macOS.

Changes will be separated by scope:

- shared GTK3 API migration, maintained widget helpers, and `GtkGLArea`
  integration belong in platform-neutral commits;
- XQuartz launch behavior, Apple application packaging, deployment metadata,
  and arm64-specific auditing belong in narrow `apple/` code or Darwin build
  branches; and
- recovery fixtures, local backup paths, and map-specific notes remain
  development infrastructure rather than product behavior.

Shared UI code should not acquire scattered `__APPLE__` branches. Where a
platform distinction is real, it should sit behind a small interface with the
common GTK behavior above it. Windows and Linux builds must remain visible as
the migration proceeds. Local Mac testing cannot prove their compatibility,
so upstream-ready work also requires CI or testing on those platforms before
it is presented as complete.

The historical upstream `gtk3` branch predates the current release branch by
many years and diverges across hundreds of files. It will be consulted at the
commit and function level only. Its unrelated features, assets, build files,
and compiler changes will not be imported with the GTK conversion.

## Platform and dependency decisions

### Retain

- **XQuartz** as the initial macOS display server. Version 2.8.6 is installed
  and its X11, OpenGL, and GLU libraries contain native arm64 slices.
- **GTK 3.24** as the target UI toolkit. It is a manageable migration from GTK2
  and supplies `GtkGLArea`.
- **OpenGL compatibility rendering** during the first port. Replacing the
  fixed-function renderer is a separate project.
- **SCons** as the build system for now. Updating the current build definitions
  is less risky than combining a platform port with a build-system rewrite.
- Maintained data and utility libraries such as GLib, libxml2, libpng,
  libjpeg-turbo, and zlib.

### Remove or avoid

- **GTK2**. It is end-of-life and would make the new port depend on a dead UI
  toolkit from its first working revision.
- **GtkGLExt**. Its usage is largely concentrated in the internal GL widget
  wrapper and can be replaced by GTK3's `GtkGLArea`.
- **A direct GLU dependency in Radiant**. The active editor already implements
  the GLU operations it uses (`Perspective`, `LookAt`, and error strings).
- **Homebrew's macOS GTK packages for the X11 build**. They use the Quartz GDK
  backend and are not compatible with the editor's existing GLX assumptions.
- **Mixed OpenGL client stacks**. Homebrew Mesa, XQuartz libraries, and
  MacPorts libraries must not be selected opportunistically by `pkg-config` in
  the same build.
- **The old `dylibbundler` packaging recipe as a prerequisite**. Packaging will
  be addressed only after an unbundled editor is stable.

### Dependency provider

The preferred initial provider is a single MacPorts GTK3/X11 client stack,
talking to the installed XQuartz server. All compiler and linker discovery must
use an explicit prefix. The build must report which GTK, GDK backend, X11,
OpenGL, and compiler it selected.

Build-only Python tooling such as SCons may be isolated in a virtual
environment rather than becoming part of the runtime bundle.

### Development-environment isolation

MacPorts is isolated on disk under `/opt/local`, but it must not become an
implicit dependency of other projects. In particular, the ambient shell
currently finds `/opt/local/bin/pkg-config` before Homebrew's implementation.
That is acceptable only if GtkRadiant makes its selection explicit; it must not
be relied upon as global configuration.

GtkRadiant development will therefore use a repository-local environment
entry point with explicit tool and library paths. It will set a constrained
`PATH`, `PKG_CONFIG_LIBDIR`, and any required runtime variables for the duration
of one command or subshell. The ordinary login-shell environment will not be
edited for this project.

The port will not:

- edit `.zshrc`, `.zprofile`, or other global shell startup files;
- add global `PKG_CONFIG_PATH`, `CPATH`, or `LIBRARY_PATH` values;
- replace system compiler or linker symlinks;
- install, remove, upgrade, relink, or deactivate Homebrew or Nix packages;
- modify or build inside the `ld-decode` workspace; or
- reuse `ld-decode` build directories, caches, virtual environments, or
  generated files.

GtkRadiant build and test outputs will remain inside this repository, a
dedicated temporary directory, or another explicitly designated GtkRadiant
location. Any future system-package change must be reviewed separately rather
than being performed as a side effect of a build.

## Existing code considerations

The current editor contains approximately 7,115 GTK calls covering about 320
distinct GTK APIs. This makes the GTK2-to-GTK3 migration significant, but it is
still preferable to preserving GTK2 indefinitely.

GtkGLExt use is concentrated in `radiant/glwidget.cpp`, its public wrapper, and
a small number of initialization and call sites. The difficult part of its
replacement is behavioral rather than mechanical:

- `GtkGLArea` owns a framebuffer and renders through GTK's render lifecycle.
- The current editor expects to make contexts current, render immediately, and
  swap buffers manually.
- The camera, grid views, texture browser, and plugins require compatible or
  shared OpenGL resources.
- The existing renderer requires a legacy/compatibility OpenGL context.

The abandoned upstream `gtk3` branch contains useful prior art, including an
early `GtkGLArea` implementation and many GTK API conversions. It is more than
600 mainline commits behind the current release branch and must be used as a
reference or source of carefully reviewed changes, not merged wholesale.

## Recovery corpus

The original backup is located at:

`/Users/josephburns/Documents/Q3A Mapping/`

It currently contains about 1,850 files, including:

- 646 `.map` files with 551 unique contents;
- 385 `.bak` files;
- 93 compiled BSPs;
- PRT, SRF, AAS, LIN, shader, texture, sound, model, and packaging assets;
- approximately 3.1 GB of map-authoring history; and
- a packaged `outrage` project containing `themepark` and `outrage-Sul_Dov`.

### Primary stress-test map

`map authoring files/themepark-06-03-22-1.map`

Observed baseline:

- SHA-256: `b4c3fa01412dae7d882aaad6d07da020f07e97d21eb025c2dd390a7c85666c2f`
- Size: 4,573,273 bytes
- 745 entity comments
- 6,493 brush comments
- 467 `patchDef2` blocks
- 37,028 brush-plane face records
- 2,571 patch control-point rows

An earlier inventory described all 40,533 parenthesized geometry lines as face
definitions. That total also included 2,571 patch control-point rows and 934
patch matrix delimiter rows. The separated counts above are the structural
baselines used for round-trip checks.

The packaged `themepark.bsp` predates this source by approximately six days.
It is a known-working runtime reference, but not the exact compiled form of the
latest source.

### Additional test levels

- A deliberately small map for fast parser, UI, and compiler iteration.
- `outrage-Sul_Dov` as a medium-size production map.
- `themepark-06-03-22-1.map` as the full correctness and performance test.

All tests must operate on copied fixtures in a temporary or ignored workspace
location. The original backup remains read-only.

### External support-data staging

Downloaded gamepacks, SDK examples, and other upstream reference material are
staged outside the repository at:

`/Users/josephburns/Documents/Q3A Mapping/Sources/`

This directory is an intake location, not a build output directory and not a
source of files to commit wholesale. Its contents remain user-managed and are
treated as read-only by port scripts. It contains the Q3A, Q2, and JK2
example-map archives, the Shaderlab archive, and this GtkRadiant binary
distribution:

- Filename: `GtkRadiant-1.6.7-20230820.zip`
- Size: 109,583,694 bytes
- SHA-256:
  `a35cdc561c52260cc19917c200451d1e48734455b307efabd8398167f3ee8779`
- ZIP integrity check: passed
- Q3 gamepack path:
  `GtkRadiant-1.6.7-20230820/installs/Q3Pack/`
- Q3 gamepack inventory: 287 files, 8,011,856 uncompressed bytes

The archive is a Windows binary distribution, but the Q3 gamepack is
platform-neutral support data. Extract only its `installs/Q3Pack` subtree into
a dedicated ignored or temporary GtkRadiant staging directory; do not import
the bundled Windows executables, DLLs, debug files, or unrelated gamepacks.
Preserve the `game/` and `install/` subtrees because they have different
installation destinations.

Runtime Quake III data in `/Applications/Quake 3 Arena/baseq3/` remains
separate from editor gamepack data. In particular, installing the gamepack's
`install/baseq3` support files must be an explicit, reviewed staging operation,
not an overwrite of the existing game directory.

## Phased implementation

### Phase 0: Protect and characterize the recovery data

- Establish and verify the repository-local GtkRadiant environment entry point.
- Confirm that it does not modify the parent shell or resolve tools and
  libraries from unintended prefixes.
- Record hashes and structural counts for selected source maps.
- Identify the corresponding packaged assets and known-working BSP/AAS files.
- Identify and fingerprint the staged Q3 gamepack independently of the Quake
  III runtime PK3 files.
- Create a script or documented procedure that stages test copies without
  modifying the backup.
- Define structural comparisons for editor save round trips.

Exit criterion: repeatable fixture staging and baseline reports for the small,
medium, and stress-test maps.

#### Phase 0 implementation status

The isolated command launcher is `apple/macos-env.sh`. It executes a child
process with an allowlisted `PATH`, an explicit MacPorts-only
`PKG_CONFIG_LIBDIR`, the X11 GDK backend, and no inherited compiler or linker
search paths. Run its preflight check with:

```sh
apple/macos-env.sh --check
```

The preflight currently passes with an arm64 Apple compiler and resolves GTK3,
GDK X11, OpenGL, GLX, X11, GLib, libxml2, libpng, and zlib exclusively from
`/opt/local`. GDK Quartz is not visible. Tests also confirm that executing the
launcher does not modify the parent `PATH`, and attempting to source it is
rejected without changing zsh or bash options.

`apple/stage-recovery.py` stages three tiers of map fixtures, extracts only the
Q3Pack subtree from the external Radiant ZIP, and writes a JSON manifest. Its
destination is restricted to this repository or a system temporary directory.
The current ignored workspace is `.macos-work/phase0/`:

```sh
apple/macos-env.sh /usr/bin/python3 apple/stage-recovery.py stage \
  --backup-root "/Users/josephburns/Documents/Q3A Mapping" \
  --gamepack-archive "/Users/josephburns/Documents/Q3A Mapping/Sources/GtkRadiant-1.6.7-20230820.zip" \
  --destination "$PWD/.macos-work/phase0" \
  --fixture "small=/Users/josephburns/Documents/Q3A Mapping/map authoring files/2019/6-04-19-1.map" \
  --fixture "medium=/Users/josephburns/Documents/Q3A Mapping/map authoring files/Sul-Dov_05-27-22-01.map" \
  --fixture "stress=/Users/josephburns/Documents/Q3A Mapping/map authoring files/themepark-06-03-22-1.map"
```

| Tier | Source | Bytes | Entities | Brushes | Patches | Brush faces |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| small | `2019/6-04-19-1.map` | 744 | 1 | 1 | 0 | 6 |
| medium | `Sul-Dov_05-27-22-01.map` | 3,189,012 | 525 | 4,606 | 294 | 26,581 |
| stress | `themepark-06-03-22-1.map` | 4,573,273 | 745 | 6,493 | 467 | 37,028 |

Each tier has a read-only `fixtures/baseline` file and an initially identical,
writable `fixtures/work` file. The manifest records source paths, sizes,
SHA-256 digests, and structural counts. A saved candidate can be checked with:

```sh
apple/macos-env.sh /usr/bin/python3 apple/stage-recovery.py compare \
  --baseline .macos-work/phase0/fixtures/baseline/stress.map \
  --candidate .macos-work/phase0/fixtures/work/stress.map
```

Structural equality is the first round-trip gate, not proof of semantic
identity. Later gates must reopen the saved file, compare parsed content, run
the compiler, and exercise the result in the game.

Known compiled references in `PK3/outrage/maps/` are:

- `outrage-Sul_Dov.bsp`: 6,717,364 bytes, SHA-256
  `55ca745625e6866d3fb4d0229a5230b413b39bab6ce5b5ee6e9b1bc8fb97a458`
- `outrage-Sul_Dov.aas`: 3,094,692 bytes, SHA-256
  `417cdc7abdd810a63482ab39b241b8a09b1fbd8fa59ea7467de1e4867e875940`
- `themepark.bsp`: 10,608,248 bytes, SHA-256
  `fdce449e49689e391ba8069073d4ec2dd9c095d2055712d88e0d0dd7ba162b36`
- `themepark.aas`: 4,817,192 bytes, SHA-256
  `f5c10ae91001e785ba14a328d898730745e9aa7a7c2882d349721519e562c546`

The backup `PK3/outrage.zip` and installed `baseq3/outrage.pk3` both pass ZIP
integrity checks but are different artifacts. Their SHA-256 digests are
`4d9570095223c0d0d74f810af3eac77e9f5ce6f79161145a13601ee4ce2f0045`
and `93d4079d7a622b7311a152a2d1585bb6859d233a38eb88a949a213d0ebac2df3`,
respectively.

### Phase 1: Native arm64 command-line tools

- Make Darwin and arm64 explicit build targets.
- Build `q3map2`, `q3map2_urt`, and `q3data` without requiring the editor UI.
- Remove obsolete deployment-target and hardcoded-prefix assumptions.
- Run parser and compile smoke tests on copied fixtures.
- Capture command lines, logs, output hashes, timings, and peak memory use.

Exit criterion: native-arm64 command-line tools can parse and compile selected
map copies reproducibly without touching the originals.

#### Phase 1 implementation status

The command-line-only build now succeeds with:

```sh
apple/macos-env.sh scons -j10 --no-packs \
  target=q3map2,q3data cc=/usr/bin/clang cxx=/usr/bin/clang++
```

All three installed tools are native arm64 Mach-O executables with a recorded
minimum macOS version of 11.0:

- `install/q3map2`
- `install/q3map2_urt`
- `install/q3data`

Their non-system dynamic libraries resolve under `/opt/local`; no Homebrew or
XQuartz client libraries appear in their linkage. The build required these
bounded compatibility fixes:

- ignore SCons options when parsing GtkRadiant `name=value` configuration;
- default Darwin builds to Apple clang and use macOS 11.0 for arm64;
- make zlib discovery independent of PNG and request it for all targets that
  compile the shared ZIP reader;
- make the Urban Terror PicoModel callback const-correct;
- declare the Urban Terror `ClearLightMap` cross-file function;
- pass `FILE *`, rather than a Windows-style integer descriptor, to q3data's
  internal `Q_filelength` function; and
- fix the Unix home-path insertion loop so multiple `-fs_basepath` arguments
  are preserved instead of all becoming duplicates of the first path.

The native q3map2 parsed the staged small fixture, wrote a 9,720-byte BSP, and
read that BSP back successfully with `-info`. The `.map` remained byte-identical
to its baseline. It also read the known Sul-Dov and themepark BSP references,
reporting 4,310 and 6,010 compiled brushes respectively.

Repeated small-map compiles have identical BSP structure but different raw
SHA-256 digests because q3map2 deliberately embeds the wall-clock compile time
in its BSP marker string. Two consecutive outputs differed by only the two
changing timestamp bytes. Reproducibility checks must either parse BSP lumps or
normalize this marker; raw BSP hashes alone are not a valid equality test.

The small recovery fixture is intentionally a one-brush fragment and fails as
real geometry once shader content flags are resolved. It remains useful for
fast source-parser checks, but is not the compiler smoke fixture.

The test-only VFS overlay is prepared with:

```sh
apple/macos-env.sh /usr/bin/python3 apple/stage-recovery.py prepare-vfs \
  --workspace "$PWD/.macos-work/phase0" \
  --quake-base "/Applications/Quake 3 Arena"
```

It generates a 40-entry `shaderlist.txt` by combining Q3Pack defaults with
shader files discovered in the installed PK3s. It does not copy or alter those
PK3s. q3map2 receives the overlay, staged Q3Pack, and installed Quake directory
as three separate read-only base paths. The operation also stages read-only
and writable copies of Q3Pack's `q3dm1sample.map` as the compiler fixture.

Current native BSP-only results are:

| Fixture | Source brushes | Output bytes | Wall time | Result |
| --- | ---: | ---: | ---: | --- |
| `q3dm1sample` | 1,100 | 1,157,248 | 0.53 s | compiled and read back |
| Sul-Dov | 4,606 | 4,182,440 | 1.71 s | compiled and read back |
| themepark | 6,493 | 6,516,072 | 2.86 s | compiled and read back |

All three source maps remained byte-identical to their baselines. The
production passes intentionally stop before VIS, lighting, and AAS generation.
Sul-Dov reports one patch-clipping diagnostic; themepark reports legacy
areaportals inside entities, two bogus patch planes, one volume diagnostic,
patch clipping, and six flipped triangles. Neither pass failed.

These geometry diagnostics are consistent with the maps' editing history.
Themepark was the first map and used Radiant's subtract operation extensively
to break up rockwork; that operation is known to leave difficult geometry.
Cleanup is deliberately deferred until the editor renders the affected areas
and new map revisions can be saved safely.

The merged VFS also reports two understood retired or legacy image references:

- `textures/sfx/flame1side` is an original Quake III reference that is normally
  missing; the adjacent flame TGA is the intended substitution.
- Sul-Dov's `textures/outrage-surface/glowgem` was retired. `myglass` is the
  likely replacement; the one-megabyte `gem.tga` is a direct ancestor.

Make both substitutions interactively once the editor is running so their
visual context can be checked. They do not block command-line port validation.

### Phase 2: Deterministic macOS dependency discovery

- Add explicit configuration for dependency prefix, GTK version, and graphics
  backend.
- Ensure `pkg-config` cannot silently mix Homebrew, XQuartz, MacPorts, and
  system libraries.
- Print a concise build configuration summary.
- Add checks for architecture mismatches and missing runtime libraries.
- Update the minimum macOS target to a value valid for arm64.

Exit criterion: two clean builds select the same headers and libraries and all
linked non-system libraries have the intended architecture and prefix.

#### Phase 2 implementation status

The SCons configuration now persists and prints four explicit settings:

```sh
dependency_prefix=/opt/local
gtk_version=3
graphics_backend=x11
macosx_deployment_target=11.0
```

These are the Darwin arm64 defaults, but they remain command-line settings so
the build record is self-describing. Configurations saved by the older build
scripts are upgraded when loaded. The arm64 deployment target rejects values
older than macOS 11.0, and X11 is the only accepted graphics backend until a
second backend is deliberately implemented.

SCons no longer invokes a bare `pkg-config`. When `dependency_prefix` is set,
it uses that prefix's executable with a private `PKG_CONFIG_LIBDIR` containing
only the prefix's `lib/pkgconfig` and `share/pkgconfig`; inherited
`PKG_CONFIG_PATH` is removed. Every package must report a prefix within the
selected dependency tree. On Darwin, its explicitly linked libraries are also
checked for the host architecture before build rules are emitted.

The selected GUI stack is audited even during a command-line-only build. The
current audit covers libxml2, GLib, zlib, libjpeg, libpng, GTK 3, GDK X11, X11,
OpenGL, and GLX. GTK 2 and GtkGLExt remain selectable only as a compatibility
configuration for other platforms; they are not the Mac target. JPEG and PNG
are now discovered through the same deterministic MacPorts path rather than
bare linker names.

Build products can be audited independently with:

```sh
apple/macos-env.sh /usr/bin/python3 apple/audit-macos-build.py \
  install/q3map2 install/q3map2_urt install/q3data
```

The audit verifies the product architecture and exact deployment target, then
checks every direct non-system runtime dependency for existence, `/opt/local`
containment, and arm64 support. At the first Phase 2 checkpoint, all three CLI
tools pass and resolve six distinct non-system runtime libraries.

Two successive clean `-j10` builds completed successfully. Their combined
`pkg-config` flags and direct `otool -L` linkage signatures were identical, and
the product/runtime audit passed after each build. Phase 2's exit criterion is
therefore met for the current CLI products and selected GUI dependency stack.

### Phase 3: GTK3 source migration

- Inventory the current GTK3 compiler failures and group them by API family
  before making broad source edits.
- Introduce or update small shared widget helpers where they remove repeated
  GTK-version and platform conditionals.
- Port the editor core in reviewable mechanical groups: object/signal access,
  widget allocation and windows, container/layout APIs, menus and toolbars,
  dialogs, input devices, and drawing callbacks.
- Bring `glwidget` only to a compilable GTK3 boundary in this phase; context
  lifecycle, sharing, rendering correctness, and removal of manual swaps are
  Phase 4 work.
- Port standard modules and contributed plugins after the editor core, keeping
  each family separately reviewable.
- Use the historical `gtk3` branch only as reviewed function-level reference.
  Do not merge or cherry-pick its broad mixed-purpose history.
- Preserve plugin interfaces where practical; version interfaces when an ABI
  change is unavoidable.
- Keep Windows and Linux compilation visible during the migration, adding CI
  or obtaining platform testing before calling the shared port upstream-ready.
- Avoid unrelated map-format, compiler, renderer-feature, asset, and UI-feature
  changes.

Exit criterion: the editor and its standard modules compile against GTK 3.24.

#### Phase 3 initial compiler survey

The first `target=radiant` GTK 3.24 build was run with `-k -j10` so the survey
would reach the editor, standard modules, and contributed plugins rather than
stopping at the first source file. This is a diagnostic failure, as expected,
but it establishes the first migration groups.

An obsolete Apple-only inclusion of `GL/glu.h` initially prevented nearly every
editor translation unit from reaching GTK code. It has been removed: Radiant
already provides the only GLU-compatible operations it uses, and its public
types come from the OpenGL header. This is shared cleanup rather than a Mac
replacement API.

The exposed GTK3 blockers are currently concentrated in these families:

- 43 direct accesses to private `GtkWidget` allocation, window, or style fields
  across 11 files;
- the removed `GdkGC`, colormap, XOR-drawing, pixmap, and bitmap APIs;
- GtkGLExt headers and initialization in `main.cpp` and `glwidget.cpp`;
- direct access to the old color-selection-dialog internals;
- the removed `GtkNotebookPage` callback type; and
- secondary type errors caused by the common `CamWnd` and UI headers failing
  before their consumers are compiled.

Deprecated but still compilable GTK3 APIs, including the old box and table
constructors, generate substantial warning noise but are not first-order
blockers. They will be migrated in mechanical groups after the shared headers
compile. The first accessor conversion uses `gtk_widget_get_allocation`, which
is supported by the existing GTK 2.24 baseline as well as GTK3, so it does not
need a platform branch or immediately break non-Mac builds.

The historical branch's early `GtkGLArea` wrapper confirms the intended widget
choice but does not provide a production-ready context-sharing implementation.
It will not be copied wholesale; Phase 4 must design and verify that behavior.

#### Phase 3 widget-access checkpoint

The first shared widget group is complete. Direct GTK2 struct access has been
removed from live editor and plugin code. Allocation reads use a small helper
built on `gtk_widget_get_allocation`, preserving compatibility with both GTK
2.24 and GTK3. Paned limits and bin children now use public properties or
accessors, while GTK3 accelerator labels use their supported setter.

The removed `GdkGC`/colormap XOR rectangle shared by camera and orthographic
views now uses a fresh Cairo context with the difference operator. This keeps
erase-by-redraw behavior without depending on X11 drawing primitives and is
available on both supported GTK generations.

The core and plugin color dialogs now use `GtkColorChooserDialog` under GTK3
while retaining the existing GTK2 implementation behind a version guard. The
notebook callback no longer names the removed `GtkNotebookPage` type, and an
unused BobToolz `GdkPixmap` helper has been deleted.

After this group, a full keep-going build reports only two compiler failures:
the deliberate GtkGLExt includes in `main.cpp` and `glwidget.cpp`. All surveyed
non-OpenGL widget code in the editor, standard modules, and contributed plugins
compiles against GTK 3.24. The next unit is therefore the bounded `GtkGLArea`
compile integration, followed by the separate lifecycle and rendering work in
Phase 4.

### Phase 4: `GtkGLArea` and rendering integration

- Replace GtkGLExt initialization and widget management.
- Implement GTK render, realize, resize, and teardown lifecycles correctly.
- Request and verify a legacy-compatible OpenGL context.
- Establish correct context sharing for all editor views and plugins.
- Remove direct GLU linkage from the active editor target.
- Reduce or isolate direct GLX use so context ownership remains with GDK.
- Validate camera, orthographic, texture, text, patch, and plugin rendering.

Exit criterion: all primary editor views render correctly and repeatedly under
XQuartz without context, framebuffer, or resource-sharing errors.

### Phase 5: Production-map correctness and performance

- Open each staged fixture without saving.
- Exercise camera movement, selection, clipping, filters, textures, entities,
  patches, undo/redo, and standard plugins.
- Measure load time, frame responsiveness, selection latency, and memory use on
  the full `themepark` map.
- Save only to a new path and compare the result structurally with its source.
- Reopen the saved copy, compile it, and test it in Quake III Arena.
- Compare compiled behavior with the packaged reference BSPs where meaningful.

Exit criterion: the largest map is practically editable, round-trips safely,
and compiles into a playable BSP.

### Phase 6: macOS application packaging

- Replace the MacPorts-specific 2013-era bundle recipe.
- Construct a relocatable `.app` using explicit install names and rpaths.
- Bundle only required runtime libraries, modules, loaders, schemas, themes,
  gamepacks, and support data.
- Supply a launcher that starts or connects to XQuartz predictably.
- Add version metadata, an arm64 deployment target, signing readiness, and
  diagnostics for missing Q3 data.
- Test the bundle from a clean user account or clean machine environment.

Exit criterion: the application runs outside the source tree without relying
on ambient development prefixes.

### Phase 7: Continuous integration and distribution

- Add macOS compilation and unit-test coverage.
- Add fixture-based parser and save-round-trip tests that are legally and
  practically suitable for the repository.
- Keep large personal assets outside the public repository.
- Document the supported macOS, architecture, XQuartz, and Q3 data versions.
- Produce reproducible development and release instructions.

Exit criterion: a clean machine can reproduce the supported build and a
regression in the Mac path is visible before release.

## Acceptance criteria

The first usable release must satisfy all of the following:

- Runs natively on Apple Silicon; Rosetta is not required.
- Uses GTK3 and does not depend on GTK2 or GtkGLExt.
- Uses one coherent X11/OpenGL client stack.
- Loads the latest `themepark` source without parser or rendering failure.
- Provides responsive navigation and editing on that map.
- Resolves the project's custom shaders, textures, models, sounds, and music.
- Saves a copied map without semantic loss or corruption.
- Compiles the saved copy with native-arm64 tools.
- Produces a BSP that loads and behaves correctly in Quake III Arena.
- Can be rebuilt and launched using documented, repeatable steps.

## Deferred work

The following are explicitly outside the first recovery milestone:

- A native Cocoa/Quartz UI backend.
- GTK4 migration.
- A modern shader-based renderer or replacement graphics API.
- Large-scale plugin API redesign.
- General map-format cleanup or compiler behavior changes.
- Universal arm64/x86_64 distribution unless a concrete need emerges.
- App Store distribution.

These may become worthwhile after the recovered editor is stable and the maps
are again actively maintainable.

## Immediate next steps

1. Make the command-line tools compile natively on arm64.
2. Introduce deterministic Darwin dependency configuration into SCons.
3. Establish repeatable parser and compiler smoke tests on staged fixtures.
4. Begin the GTK3 migration in small, buildable changes.
