# GtkRadiant macOS Recovery Plan and Journal

Status: implementation underway, 1 September 2026

## How to read this document

The opening sections are the project charter: motivation, constraints,
cross-platform policy, dependency decisions, and the recovery corpus. They
change only when the scope or evidence changes.

The **prospective roadmap** contains unfinished work and current exit criteria.
The **implementation journal** records completed checkpoints, tactical
decisions, commands, and observed evidence. When a roadmap item is completed,
its durable details move into the journal instead of leaving a growing list of
future-tense tasks that have already happened.

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
Windows 7 installation. That SSD has now been recovered and mounted read-only;
the map sources and assets also remain available in redundant local backups.

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

### Windows dependency runtime

The current Windows process is automated but not reproducible from source in
this repository. `scons target=setup` downloads custom prebuilt GTK2,
GtkGLExt, JPEG, and libxml2 archives from the project's S3 storage, unpacks
them beside the checkout, and copies a hard-coded runtime subset into
`install/`. The archives have names and consumers here, but no checked-in build
recipes, checksums, complete manifests, or source-provenance record. Visual
Studio projects also hard-code their GTK2 include paths, library paths, and
import-library names.

Consequently, Windows GTK3 support is not merely a distribution build waiting
to be run. It requires selection of a reproducible Windows GTK3 toolchain,
updates to the Visual Studio dependency model, a GTK3 runtime-closure and data
packaging step, and validation of `GtkGLArea` on Windows OpenGL. The abandoned
2015 Windows GTK3 work reached compilation but failed around GL initialization,
which reinforces treating this as a real platform port. Windows therefore
keeps the explicit GTK2 compatibility path until that work has its own tested
runtime and CI evidence.

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

## Prospective roadmap

### Phase 5: Production-map correctness and performance

- Open each staged fixture without saving.
- Exercise camera movement, selection, clipping, filters, textures, entities,
  patches, undo/redo, and standard plugins.
- Measure load time, frame responsiveness, selection latency, and memory use on
  the full `themepark` map.
- Save only to a new path and compare the result structurally with its source.
- Reopen the saved copy, compile it, and test it in Quake III Arena.
- Compare compiled behavior with the packaged reference BSPs where meaningful.
- Review `flame1side` and `glowgem` substitutions interactively in their map
  context before saving any replacement.

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

### Phase 7: Cross-platform validation and distribution

- Add macOS compilation and unit-test coverage.
- Add fixture-based parser and save-round-trip tests that are legally and
  practically suitable for the repository.
- Exercise the shared GTK3 path in Linux and BSD CI.
- Keep the explicit Windows GTK2 compatibility build green while designing a
  reproducible Windows GTK3 SDK and runtime pipeline.
- Keep large personal assets outside the public repository.
- Document the supported macOS, architecture, XQuartz, and Q3 data versions.
- Produce reproducible development and release instructions.

Exit criterion: a clean machine can reproduce each supported build, and
regressions in shared or platform-specific paths are visible before release.

### Acceptance criteria for the first usable Mac release

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

### Deferred work

- A native Cocoa/Quartz UI backend.
- GTK4 migration.
- A modern shader-based renderer or replacement graphics API.
- Large-scale plugin API redesign.
- General map-format cleanup or compiler behavior changes.
- Universal arm64/x86_64 distribution unless a concrete need emerges.
- App Store distribution.

These may become worthwhile after the recovered editor is stable and the maps
are again actively maintainable.

### Immediate next checkpoint

Phase 4 is complete. Start Phase 5 by opening the staged medium recovery
fixture, recording its first-load time and normal edit responsiveness, then
performing the first deliberate save-round-trip to a named candidate path.
Profile first-load texture latency before altering the legacy texture path.

## Implementation journal

### Phase 0 — recovery corpus protected and characterized

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

### Phase 1 — native arm64 command-line tools established

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

Both substitutions were recorded for later interactive review because their
visual context cannot be judged by the command-line validation. They did not
block the native compiler checkpoint.

### Phase 2 — deterministic macOS dependency discovery established

The SCons configuration now persists and prints four explicit settings:

```sh
dependency_prefix=/opt/local
gtk_version=3
graphics_backend=x11
macosx_deployment_target=11.0
```

These are the Darwin arm64 defaults, but they remain command-line settings so
the build record is self-describing. GTK3 is also the default on Linux and the
BSDs: if GTK2 and GTK3 development files coexist, an ordinary build must take
the maintained GTK3 path instead of silently preserving the aging stack. A
missing GTK3 dependency is an actionable configuration failure, not a reason
to downgrade automatically. GTK2 remains available through an explicit
`gtk_version=2` compatibility build. Windows keeps that compatibility default
because its externally hosted prebuilt SDK, Visual Studio projects, and
packaging are GTK2-specific; changing it belongs with a reproducible, tested
Windows GTK3 runtime update.

Configurations saved by the older build scripts are upgraded when loaded. The
arm64 deployment target rejects values older than macOS 11.0, and X11 is the
only accepted graphics backend until a second backend is deliberately
implemented.

SCons no longer invokes a bare `pkg-config`. When `dependency_prefix` is set,
it uses that prefix's executable with a private `PKG_CONFIG_LIBDIR` containing
only the prefix's `lib/pkgconfig` and `share/pkgconfig`; inherited
`PKG_CONFIG_PATH` is removed. Every package must report a prefix within the
selected dependency tree. On Darwin, its explicitly linked libraries are also
checked for the host architecture before build rules are emitted.

The selected GUI stack is audited even during a command-line-only build. The
current audit covers libxml2, GLib, zlib, libjpeg, libpng, GTK 3, GDK X11, X11,
OpenGL, and GLX. GTK 2 and GtkGLExt remain selectable only as a compatibility
configuration; they are not selected merely because both GTK generations are
installed. JPEG and PNG are now discovered through the same deterministic
MacPorts path rather than bare linker names.

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

### Phase 3 — GTK3 source migration reached the compile boundary

#### Phase 3 initial compiler survey

The first `target=radiant` GTK 3.24 build was run with `-k -j10` so the survey
would reach the editor, standard modules, and contributed plugins rather than
stopping at the first source file. This was a diagnostic failure, as expected,
and established the first migration groups.

An obsolete Apple-only inclusion of `GL/glu.h` initially prevented nearly every
editor translation unit from reaching GTK code. It has been removed: Radiant
already provides the only GLU-compatible operations it uses, and its public
types come from the OpenGL header. This is shared cleanup rather than a Mac
replacement API.

The exposed GTK3 blockers were concentrated in these families:

- 43 direct accesses to private `GtkWidget` allocation, window, or style fields
  across 11 files;
- the removed `GdkGC`, colormap, XOR-drawing, pixmap, and bitmap APIs;
- GtkGLExt headers and initialization in `main.cpp` and `glwidget.cpp`;
- direct access to the old color-selection-dialog internals;
- the removed `GtkNotebookPage` callback type; and
- secondary type errors caused by the common `CamWnd` and UI headers failing
  before their consumers are compiled.

Deprecated but still compilable GTK3 APIs, including the old box and table
constructors, generated substantial warning noise but were not first-order
blockers. They were left for later mechanical groups once the shared headers
compiled. The first accessor conversion used `gtk_widget_get_allocation`,
which is supported by the existing GTK 2.24 baseline as well as GTK3, so it did
not need a platform branch or immediately break non-Mac builds.

The historical branch's early `GtkGLArea` wrapper confirmed the intended widget
choice but did not provide a production-ready context-sharing implementation.
It was not copied wholesale; sharing remained a Phase 4 design and validation
problem.

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

#### Phase 3 GtkGLArea compile checkpoint

The bounded GTK3 integration now compiles and links the complete native arm64
editor and its standard modules. GTK3 builds use `GtkGLArea`, request a desktop
OpenGL 2.1 context with alpha and the caller-selected depth buffer, and route
the existing expose paths through GTK3's `render` signal. GTK2 builds retain
the GtkGLExt implementation behind version guards.

`GtkGLArea` owns its context and framebuffer, so the legacy explicit context
create, destroy, and buffer-swap entry points are compatibility no-ops on the
GTK3 path. PangoFT2 is now an explicit editor dependency rather than an
accidental transitive one. The resulting `install/radiant.bin` passes the
product audit as arm64, targeting macOS 11.0, with all 19 direct non-system
runtime libraries resolving inside `/opt/local`.

This satisfied Phase 3's compile exit criterion, but not Phase 4. At this
checkpoint, context creation under XQuartz, context sharing, repeated
realize/unrealize behavior, viewport resizing, and rendering correctness were
all still unproven. The requested share widget was retained as relationship
metadata for that work; GTK3 does not expose GtkGLExt's arbitrary share-context
constructor on `GtkGLArea`.

The tactical choices in this checkpoint are intended to keep the patch useful
outside macOS and easy to review upstream:

- GTK-generation branches use `GTK_CHECK_VERSION`, not `__APPLE__`, so Linux
  and BSD GTK3 builds receive the same widget and rendering upgrade while GTK2
  compatibility remains buildable.
- The editor requests desktop OpenGL 2.1 because its renderer uses the legacy
  fixed-function API. Requesting a modern core profile would require a renderer
  rewrite and would obscure the narrower platform recovery work.
- GTK3 context create, destroy, and swap entry points are no-ops because
  `GtkGLArea` owns that lifecycle and presents after `render`. Reimplementing
  GtkGLExt ownership on top of it would fight GTK's framebuffer model.
- PangoFT2 is named explicitly because Radiant calls that API directly. Relying
  on GTK to expose it transitively makes successful linking depend on packaging
  accidents.
- Context-sharing intent is recorded without claiming it works. The historical
  GTK3 branch did not solve sharing, and GTK3 offers no direct equivalent to
  GtkGLExt's arbitrary shared-context constructor; Phase 4 therefore treats
  sharing as a runtime design and validation problem.

### Phase 4 — XQuartz runtime integration

#### Initial game-selection probe

The first GTK3 binary was launched through `apple/macos-env.sh` with a
repository-local disposable home and generated game description. The game
description points at the staged Q3 pack metadata and reads the installed paks
from `/Applications/Quake 3 Arena`; it does not modify either location or the
production user preferences. This isolation is tactical: early startup crashes
and stale PID/preferences files must not contaminate an existing Radiant setup
or the map-recovery sources.

XQuartz reports a mapped, viewable 320 by 253 `Select a game` window owned by
`radiant.bin`. Startup emitted only the expected missing-first-run-preferences
warning and no loader or OpenGL-context error. A macOS screenshot did not
composite the rootless X11 surface even though the X11 window tree reported it
as viewable, so X11 window attributes—not the screenshot—are the evidence for
this checkpoint.

This proves GTK initialization, game discovery, and first-run dialog creation;
it does not prove `GtkGLArea` context creation because the editor view was not
entered. The probe was terminated after identifying that boundary; the next
checkpoint below crossed it with explicit context, version, sharing, and
rendering evidence before loading a recovery map.

#### First editor entry and rendered views

An isolated Mac translation of the recovered Windows `user2.proj` now starts
the main editor. Its base, map, texture, entity, autosave, and compiler paths
all point into `.macos-work/runtime/game-root`; the verified retail paks remain
read-only symlinks to `/Applications/Quake 3 Arena/baseq3`. Its q3map2 menu
entries invoke the native tool in `install/`. The original backup, installed
game data, Windows SSD, and normal user preferences remain untouched.

The first editor screenshot established that the GTK shell, menus, toolbars,
splitters, status bar, shader-directory list, and project loading all work.
The initial unsynchronized GL run showed white panes and one
`GL_INVALID_OPERATION`, so that image was treated as entry evidence rather
than a rendering success.

The Mac launcher now sets `GDK_GL=legacy` inside its child environment. This is
not an ambient shell setting and does not affect other applications. It is
required because Radiant's renderer still uses the fixed-function API; without
it, GDK attempts a modern core context that cannot execute that renderer.
Runtime diagnostics confirm four GtkGLAreas using desktop OpenGL 2.1, not ES:

- vendor: `Apple`;
- renderer: `Apple M1 Max`;
- version: `2.1 Metal - 89.4`;
- legacy context: yes; and
- one common GDK shared-context group for the camera, orthographic, texture,
  and auxiliary views.

GTK warns when Radiant requests 2.1 because its normal version-selection API
expects at least 3.2 on macOS. That warning describes the request path, not the
result: the explicit legacy mode subsequently supplies and reports the needed
2.1 context.

The first ordinary run later failed in XQuartz with `GLXBadContext` after
`xp_attach_gl_context returned: 2`. A synchronous X11 debugger run exposed the
cause and, importantly, rendered the orthographic grid, origin/entity markers,
and texture-browser `notex` tile without the earlier OpenGL error. This proved
that the fixed-function renderer and GtkGLArea framebuffer can work together;
the failure was in context transitions rather than map drawing or a required
core-profile rewrite.

The transition storm came from preserving a GtkGLExt-era invariant too
literally. GTK has already made a GtkGLArea context current before emitting
`render`, but each legacy `OnExpose()` immediately called `MakeCurrent()`
again. Under XQuartz's GLX-to-CGL bridge, four views repeatedly detached and
reattached contexts and drawables. The GTK3 wrapper now compares the requested
context with GDK's current context and skips only that redundant transition.
It still makes a context current for realize callbacks, timers, plugin calls,
and any other caller that genuinely needs a change. This is a shared GTK3
lifecycle correction rather than an Apple-only rendering branch.

After rebuilding, a normal non-debugger launch created all four Apple 2.1
contexts, mapped the `unnamed.map` editor window, stayed alive beyond the
original failure interval, and emitted neither `GLXBadContext` nor a new
OpenGL error. The generated 40-entry Phase 1 shader list has also been copied
into the ignored runtime game root for the next restart; the prior `notex`
display was partly expected because that runtime file had not yet been staged.

This is the first working-editor milestone, not the Phase 4 exit criterion.
Repeated startup/shutdown, resize, redraw, real map loading, texture resource
sharing, and the GtkGenSurf and TexTool plugin GL views were identified as
remaining work.
TexTool already receives GTK3 `render` callbacks through Radiant's shared
`IWindow` wrapper, so it needs runtime validation rather than a duplicate
widget conversion. GtkGenSurf owns a separate legacy preview implementation
and is addressed in the later plugin-lifecycle checkpoint.

#### Copied-fixture interaction and runtime-data follow-up

The isolated runtime VFS initially used the installed game directory as its
loose-file root. The translated project's `basepath` was corrected to the
parent game root, and the generated game description's `enginepath` was then
pointed at that same disposable root. The root contains symlinks to the
verified retail paks, so this adds writable staged data without copying or
altering the installed Quake III directory. The VFS now reads the generated
40-entry shader list and the staged entity definitions. On the small copied
fixture, `textures/common/nodraw` and `textures/outrage/myglass` resolve where
they previously fell back to `shadernotex`.

The command-line map argument also uncovered a general project-loading bug.
Radiant treated a project marked `user_project=1` as if it had to match an
installed `default_project.proj` template. That is wrong for user-authored
projects, particularly recovered ones that intentionally live outside the
game installation. The shared project loader now bypasses only the template
comparison for explicit user projects; it continues to validate ordinary
template-derived projects. The isolated preference records project version 2,
so opening a `.map` at launch reuses the recovered project without a file
chooser.

The small fixture loaded as one brush and zero entities in 0.04 seconds. The
editor rendered and accepted repeated scrolling, while its copied source stayed
byte-identical to the protected baseline. A canvas click then initialized
BobToolz and reported missing `install/modules/bt/bt-el1.txt` and `bt-el2.txt`.
These are shipped BobToolz exclusion-list defaults, not user map data. They
were previously copied only by the old setup path, which the deliberately
minimal `--no-packs target=radiant` development build does not run. The normal
`radiant` target now installs all seven BobToolz `bt/` support files beside the
module on every platform, and the macOS build has verified their presence.

The first click after that correction still froze the editor. Sampling showed
both Radiant and XQuartz idle rather than consuming CPU, which made a renderer
loop unlikely but did not prove the source of the stalled interaction. The
remaining shared GTK2 input path called the deprecated global
`gdk_pointer_grab()`/`gdk_pointer_ungrab()` pair for every canvas click. That
is unnecessary for an ordinary in-widget GTK3 drag and is a poor fit for
XQuartz's event routing. GTK3 therefore leaves that legacy global grab out of
the normal press/release path; the GTK2 behavior is retained unchanged. This
is deliberately a narrow stabilization step: the distinct camera free-move
capture path still needs its own modern review before it can be declared
complete.

With the change and the BobToolz data installed, a fresh launch accepted a
canvas interaction, remained responsive, and saved a map to
`/Users/josephburns/gtk3.map`. The saved file is syntactically well-formed. It
contains the original one brush plus one newly created six-face brush using
`curry/curry_lightmap`; its structural counts are consequently two brushes and
twelve faces rather than the source fixture's one and six. The different count
is the expected result of the interactive edit, not evidence of an incomplete
save. Radiant also normalizes entity-key and face order when it writes the map,
so byte equality is intentionally not required for an edited candidate.

The next real editor-map probe opened `q3dm1sample.map` successfully. Its
textures were visible and camera navigation worked. This is stronger evidence
than the tiny one-brush fixture because it crosses ordinary map loading,
shader/texture lookup, viewport drawing, and navigation together. It is still
not a production-map round-trip: selection, resize, repeated lifecycle tests,
the GL-preview plugins, and the staged medium and stress maps remain Phase 4
and Phase 5 work.

The full Themepark source also opens in the editor. This is the recovery map's
6,493-brush, 467-patch load path, so it is a major entry milestone; it does
not authorize an in-place save or mean that performance and editing behavior
are complete. The current GTK3 layout's pane dividers were difficult or
impossible to resize interactively. The layout is built from normal
`GtkPaned` widgets and already persists their positions. Each GTK3 pane now
receives GTK's `wide-handle` affordance, while GTK2 is unchanged. This makes
the hit targets visible and larger without changing pane ownership or the
existing saved-position format. After restart the pane dividers drag-resized
correctly, validating that targeted usability correction.

Initial texture display remains noticeably slow. This is not Rosetta: the
editor is a native arm64 Mach-O executable and the live contexts report the
Apple M1 Max renderer. The legacy texture path decodes images on the UI thread,
constructs every mip level in CPU code, and synchronously uploads each level
with `glTexImage2D`; under XQuartz each upload also crosses the X11/Apple
compatibility boundary. Those facts make texture upload a plausible bottleneck,
but no optimization has been made on that inference. A sampled profile and
repeatable first-versus-warm load measurement are required before changing
texture filtering, mip generation, caching, or upload behavior.

The first measurement is now available for `q3dm1sample.map`: 11.49 seconds
for the initial load and 1.81 seconds for the immediate reload. The 9.68-second
reduction (a 6.3× cold-to-warm ratio) shows that most of the reported delay is
one-time resource initialization or caching rather than a persistent emulation
penalty. The still-visible warm-load cost includes map parsing and rendering as
well as any texture work, so it must not yet be attributed to a single layer.
The next profile must partition the cold interval among VFS reads/image decode,
CPU mip generation, and OpenGL upload.

A subsequent `q3dm1sample` load measured 10.58 seconds. On that run, selecting
an existing brush, moving it, and undoing the move worked; camera free-look was
nominal; wireframe, texture, and orthographic views navigated correctly; and
assigning a visible texture to a selected face worked. GtkGenSurf opened and
rendered its preview after the queued-render conversion, which validates that
plugin path. The actual plugin-menu label is lower-case `textool`, not the
source module's `Q3 Texture Tools` display name.

#### Remaining interaction and preview paths

The regular editor panes, a sample map, and Themepark have already exercised
the main shared `GtkGLArea` views. The remaining normal-input test is a
selected existing brush dragged inside its viewport, followed by a clean
restart/quit cycle. Camera free-move is deliberately separate: it uses a real
pointer capture to hide and confine the cursor, unlike the ordinary in-widget
drag path that was stabilized by removing a global legacy grab. It must be
entered and exited normally, including a focus change, before its capture code
is changed.

GtkGenSurf is different: it creates its own GL widget and still connected the
GTK2 `expose-event` signal. GTK3 has now been given a bounded conversion: it
connects to `render`, queues preview updates rather than drawing from arbitrary
callbacks, and redraws its live coordinate readout through that lifecycle.
GTK2 retains the original expose, scissor, and buffer-swap behavior. The native
arm64 module builds successfully, and opening its preview now renders under
GTK3. The remaining non-destructive checks are changing a control, resizing the
preview, and closing it without generating map geometry.

TexTool was investigated only long enough to classify it. Its live window is
supplied through Radiant's `IWindow` interface. GTK3 needed a `GtkGLArea`
`render` callback, child-before-show construction, the area-specific render
queue, and a per-paint viewport refresh; after those changes a selected
Themepark patch rendered its 3-by-3 green control-point grid. This verified
the wrapper's basic lifecycle. The texture background was flat white, however,
and GTK3 exposes no public equivalent to GtkGLExt's arbitrary per-widget
shared-context constructor.

Git history makes this an intentionally deferred legacy-tool issue rather
than a Mac recovery blocker. `origin/main` last changed TexTool substantively
in 2015 (`2f403e16`); its only later touch was the 2017 `abs` to `fabs` warning
fix (`5f7efeec`). The historical `origin/gtk3` branch likewise retained the
old GtkGLExt sharing assumption. Since TexTool is not part of the intended
mapping workflow, it is removed from the Phase 4 gate. Revisit texture sharing
only if a real TexTool use case emerges, at which point it warrants a scoped
design rather than another compatibility patch.

#### Phase 4 completion

Phase 4 is complete as of 2026-09-02 for the recovered Quake III mapping
workflow. The primary GTK3 `GtkGLArea` views create native Apple M1 Max legacy
contexts, share the editor's texture resources, render repeatedly under
XQuartz, and survive map loads, resizing, ordinary editing, undo, free-look,
and restart cycles without a further context or framebuffer error. The sample
map and the 6,493-brush/467-patch Themepark map both open with visible
textures; a brush move and undo work; pane dividers resize; and a test map
saves successfully. GtkGenSurf's preview opens and renders through the GTK3
render lifecycle. Its advanced generator controls remain a Phase 5 use-case
test rather than a platform gate.

The closure deliberately excludes TexTool's unmaintained arbitrary texture
sharing and does not claim performance, map round-trip, compiler, or gameplay
correctness. Those are the defined work of Phase 5, not reasons to keep
reopening the GTK3 runtime port.

### Phase 5 — compiler recipe and first fast-pipeline probe

The native arm64 `install/q3map2` is built from GtkRadiant upstream's bundled
Q3Map2 source, not an unverified binary distribution or the separate 2016
source extraction. The active Mac `user2-macos.proj` now adds `-skyfix` to each
Quake III BSP recipe: the single BSP command plus the BSP leg of the fast-test,
full-test, and full-final pipelines. It is intentionally absent from VIS and
light passes, where it has no meaning. The ignored runtime default template
will be regenerated from this decision when the development launcher is
formalized for packaging.

The first updated fast-pipeline probe used the staged `q3dm1sample.map`.
The BSP pass printed `GL_CLAMP sky fix/hack/workaround enabled`, parsed 987
world brushes, 113 patches, and 161 entities, and wrote a 1,157,144-byte BSP
in 0.56 seconds. It also found `Entity 156, Brush 0: Entity leaked`; Q3Map2
therefore stopped after BSP and did not run VIS or light. The existing
`textures/sfx/flame1side` warning is the known original-Q3 asset gap, not a
compiler integration failure. This establishes native compiler invocation and
the skyfix recipe, while classifying `q3dm1sample` as an editing/rendering
fixture rather than the sealed full-pipeline test map. A future compile gate
needs either a sealed fixture or a deliberate leak repair in a recovery map.

That repair was immediately made in the staged `q3dm7sample` map. Its updated
recipe completed BSP, VIS, and light. Radiant did not retain the child
compiler's scrollback in `radiant.log`, so its elapsed wall time is unavailable,
but the generated 4,647,072-byte BSP verifies every output stage: Q3Map2's
read-only `-info` report lists 4,045 drawsurfaces, 29,990 draw vertices,
visibility data, 16 lightmaps, and a 32,200-entry lightgrid. This is the first
complete native-arm64 Q3Map2 pipeline result for the recovered workflow. The
next Phase 5 gate is to load that BSP in ioquake3 and inspect it, especially
the sky edges and the repaired leak area.

### Recovered Windows 7 environment audit

The original Windows 7 SSD was recovered and mounted at `/Volumes/Windows7`.
macOS reports the NTFS volume as read-only; the audit performed no writes to it.
This gives the recovery work a second kind of evidence: the local backup remains
the protected source corpus, while the SSD records the installation layout and
editor configuration that were actually used.

The relevant preserved locations are:

- `/Volumes/Windows7/Users/Joseph Burns/Desktop/outrage/` — working assets,
  archives, packaged output, and map-development history;
- `/Volumes/Windows7/Program Files (x86)/ioquake3/baseq3/` — the live writable
  game and authoring tree used by Radiant;
- `/Volumes/Windows7/Program Files/GtkRadiant-1.6.6-20180422/` — GtkRadiant,
  its Q3Pack, preferences, modules, and compiler tools; and
- `/Volumes/Windows7/Program Files (x86)/Quake Toolkit/` — an independent
  legacy Windows helper application.

#### Authoritative editor state

The active per-game preference file is
`GtkRadiant-1.6.6-20180422/installs/Q3Pack/game/local.pref`. It records
`user2.proj` as the last project and
`baseq3/maps/themepark-06-03-22-1.map` as the last map. Its recent-file list and
map timestamps place this state in June 2022.

Another file at `GtkRadiant-1.6.6-20180422/q3.game/local.pref` points to a
missing `user7.proj` and contains older window state. It is not the operative
configuration. This distinction follows the Windows non-network preference
path in the source: per-game preferences are stored in the installed game-tools
directory, which is the Q3Pack `game/` subtree in this installation. The old
`radiant.log` also names that Q3Pack preference location.

The log itself records a 1.6.6 process compiled on 23 April 2018 and started on
1 June 2019. That invocation exited after two seconds, before OpenGL became
ready, so it must not be treated as evidence of a successful editor session.
The 2022 preference state, named maps, compiled products, and packaging
timestamps are stronger evidence of the later working environment.

`user2.proj` preserves the authoring configuration that matters for recovery:

- the correct `baseq3` map, texture, autosave, entity-definition, and base
  paths;
- separate q3map2 BSP, VIS, and lighting passes;
- fast-test, full-test, and final compile recipes;
- final lighting with `-fast -patchshadows -samples 3 -bounce 8 -dirty
  -gamma 2 -compensate 4`;
- BSPC AAS generation; and
- ASE conversion using `-meta -patchmeta -subdivisions 4`.

These recipes are behavioral requirements, not portable configuration files.
The recovered project embeds Windows paths and executable names, so copying it
into the Mac preference directory would create a misleading and fragile setup.
The Mac recovery project should translate the known options onto isolated,
writable macOS paths and native tools. The Q3 BSP recipes intentionally add
`-skyfix`: Q3Map2 documents it as the workaround for the black GL_CLAMP border
that can appear at skybox edges on ATI and newer NVIDIA hardware. It belongs on
the BSP pass only; VIS and light consume the resulting BSP and do not accept
the switch. The historical `user6.proj` made this available only in an
experimental final preset, but the recovery workflow adopts it as the Quake
III baseline because compatibility across period hardware is a real release
property, not a cosmetic compile preference.

#### Data and gamepack comparisons

All nine official `pak0.pk3` through `pak8.pk3` files on the SSD are
byte-identical, by SHA-256, to those in
`/Applications/Quake 3 Arena/baseq3/`. The Windows installation therefore does
not supply a different set of required retail data, and the Mac paks should not
be replaced or duplicated.

The recovered GtkRadiant Q3Pack `game/` tree is identical to the already staged
1.6.7 Q3Pack except for its generated Windows `local.pref`. This independently
validates the platform-neutral gamepack selected during Phase 0. The generated
preference file remains user state and must not be imported as gamepack data.

The SSD's latest named Themepark source,
`baseq3/maps/themepark-06-03-22-1.map`, has SHA-256
`b4c3fa01412dae7d882aaad6d07da020f07e97d21eb025c2dd390a7c85666c2f`.
It is byte-identical to the primary stress fixture in the local backup. The
preserved `autosave.map` is eleven minutes older and slightly smaller, so it
does not contain later unnamed Themepark work.

The recovered `outrage` desktop tree contains 552 `.map` files, 299 `.bak`
files, 29 shader files, and 15 PK3s. Its packaged `outrage.pk3`, compiled
`themepark.bsp`, and `themepark.aas` are dated 3 June 2022. The live Windows
`baseq3/maps` directory contains 36 maps, 25 backups, 10 BSPs, 19 PRTs, and 22
SRF files. These are useful provenance and runtime references, but testing must
continue to use copied fixtures rather than edit either the SSD or the local
backup.

#### Historical tools versus recovery requirements

The recovered Radiant editor and its bundled q3map2 are 32-bit x86 Windows
executables; the installation also includes a separate 64-bit x86 q3map2, and
the active preferences request that x64 compiler. Its adjacent GTK2 and
GtkGLExt DLL closure corroborates the documented legacy Windows dependency
model, but does not provide a source-reproducible GTK3 runtime.

Quake Toolkit 1.57 is a 32-bit Windows-only helper with presets for old q3map
and q3map2 versions. `ROJAN.q3p` likewise names GtkRadiant 1.6.5 and an external
q3map2gui log path. Neither represents a dependency to port. They are historical
evidence only; the later `user2.proj` compile recipes are the authoritative
description of the desired workflow.

The audit therefore narrows the next editor step: construct an isolated,
writable Mac project from the semantics of `user2.proj`, reuse the verified Mac
retail paks and staged Q3Pack, and enter the main editor without importing
Windows-specific window state, ATI workarounds, paths, or GTK2 runtime files.
