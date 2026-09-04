# Shader preview and authoring plan

## Purpose

Add a native GtkRadiant contributed module that makes Quake 3 shader work
visual and incremental. Its first interface is a shader preview window; the
editor is entered from that window after the preview workflow is useful.

This is not a reimplementation of Q3ASE. The module will favour a small,
portable preview and an authoring workflow backed by normal `.shader` files.

## Product shape

The `Plugins -> Shader Preview` command opens a modeless preview window. The
window will ultimately contain an OpenGL preview, a stage stack, transport
controls, and an `Edit Shader...` button. The editor starts as a way to save
the constructed stack, then grows into a source-first shader editor.

The intended delivery order is:

1. Stable preview surface and selected-shader image.
2. Read-only shader-document parsing into an ordered stage model.
3. Correct stage image loading, compositing, and animation playback.
4. Visible/reorderable stage stack and controlled editing.
5. Shader-file creation and source editing.

## Technical architecture

```
GTK 2 or GTK 3 window adapter
             |
             v
       Preview controller <---- UI model (stages, time, camera)
             |
             v
  OpenGL 2.1 renderer <------ texture upload/cache
             |
             v
   Radiant VFS and image loader
```

### New module

Create `contrib/shaderpreview/` as a regular Synapse plugin and add it to the
SCons module list. It initially requires these established APIs:

- `PLUGIN_MAJOR` for the menu command.
- `RADIANT_MAJOR` for image loading, file dialogs, logging, and preferences.
- `UIGTK_MAJOR` for a Radiant-owned GL widget.
- `QGL_MAJOR` for Radiant's portable OpenGL dispatch table.

The optional shader API supplies selection metadata, including the source
`.shader` filename. The preview must use its non-loading lookup only: a lookup
that causes Radiant to load/bind a texture from a GTK callback can use the
wrong GL context. Shader source is read through Radiant's VFS-aware file API.

### Cross-platform GL contract

The renderer is platform-neutral C++ and stays within desktop OpenGL 2.1
compatibility functionality. It must never issue GLX, WGL, CGL, X11, or
Win32-context calls.

The GTK adapter creates the surface through
`g_UIGtkTable.m_pfn_glwidget_new`. It contains the only GTK-version branch:

- GTK 2 draws from `expose-event` and explicitly swaps the buffer.
- GTK 3 draws from `GtkGLArea`'s `render` signal; redraw requests queue a
  render instead of drawing from timers or input handlers.

All GL objects belong to this preview context. No preview texture is shared
with Radiant's main context. Create, upload, and delete those objects only
while this context is current.

On GTK 3, queue the first render after the widget is realized *and* from an
idle callback after it is mapped. A pre-realize request may be discarded and
leave the preview blank until unrelated UI damage, such as a button hover.
Idle callbacks must consult the live preview widget rather than retain a
destroyed widget pointer.

### Image and file boundary

Use Radiant's `m_pfnLoadImage` image-manager API rather than introducing a
new decoder. It selects the installed image module and returns RGBA pixels;
the preview uploads them to an owned GL texture and releases the source buffer
with `g_free`.

The first loader works with loose, VFS-resolvable game assets. A packaged image
can be previewed, but it is never modified. Later authoring saves only to a
loose `scripts/*.shader` file in the active game directory. Editing a shader
whose source exists only in a PK3 is therefore an explicit `Save as override`
operation.

## Milestones

### 1. Preview shell

Deliver a menu command and modeless window containing an OpenGL 2.1 surface.
Render an internally generated RGBA checkerboard on one textured quad. Capture
the current Radiant shader/texture name and show it in the window; the selected
document identity will later drive loading and editing. Include only
close/reopen and resize behaviour; no file loading or shader parsing.

**Acceptance:** on the macOS GTK 3 build, the first open, resize, close, and
reopen all render correctly without GL-context errors. The module also builds
through the GTK 2 code path.

**Status:** implemented and exercised on macOS GTK 3. The selected shader's
representative image is shown at its native aspect ratio, with source alpha
over a checkerboard. The checkerboard is intentionally the built-in
transparency background; an optional user image belongs in a later preview
appearance preference.

### 2. Read-only shader document

Parse the selected shader's source block into an explicit document model. A
stage contains its map kind, image source(s), blend factors, colour/alpha
generation, texture-coordinate modifiers, and unsupported directives. The
parser must preserve stage order and distinguish:

- `map` and `clampmap` single-image stages;
- `animMap` frequency plus its full frame list (not merely its first token);
- special maps such as `$lightmap` and `$whiteimage`;
- stages with no ordinary image, which may rely on generated colour/alpha.

**Acceptance:** the preview displays a read-only stage count and source list
that matches representative static, animated, and text-oriented shaders.

**Status:** initial source-block lookup and static-map extraction are present.
The current parser is deliberately not yet sufficient for `animMap` frame
lists or special maps; it must be replaced by the explicit document model
before visual parity is claimed.

### 3. Playback and compositing

Load and cache all stage images into preview-owned GL textures. Render stages
in order over the checkerboard. Apply the parsed `blendFunc` per stage rather
than exposing a global preview blend selector. Add a GTK main-loop timer that
only advances document time and queues a render; it never performs GL work.

Implement `animMap` by choosing `floor(time * frequency) mod frameCount`.
Then add `tcMod scroll`, `scale`, and `rotate`, followed by simple `rgbGen` and
`alphaGen` cases. Keep unsupported directives visible in the UI/source rather
than silently treating them as ordinary alpha layers.

**Acceptance:** a known `animMap` shader visibly advances, pauses
deterministically, and resumes without a time jump. Static shaders using
additive, filter, and alpha blend display in the expected stage order. Closing
the window leaves no timer or render callback targeting its destroyed context.

### 4. Stage stack and editing entry point

Add a visible, reorderable stage list with Add, Remove, Move Up, and Move Down
actions. The preview remains the first interface; `Edit Shader...` opens the
authoring controls only once the read-only renderer accurately explains the
source document.

**Acceptance:** moving two distinct editable stages immediately changes the
previewed order while preserving unrecognised source directives.

### 5. Shader documents and editor

Serialize the stage model to a single shader block and write it to a loose
shader file. Reload the changed relative `scripts/...shader` file through
Radiant so the Texture Browser and map use the result. The preview window's
`Edit Shader...` button opens a source-first editor for that document.

The editor preserves unrecognised directives and comments. Structured controls
edit only directives they understand; the source remains authoritative.

**Acceptance:** create a shader, save it, reload it in Radiant, assign it to a
face, and reopen it without losing unrecognised source text.

## Compatibility and release policy

The module must compile on both active UI paths from its first commit. GTK 3 is
the preferred path wherever a maintained GTK 3 toolchain/runtime is available;
GTK 2 is the Windows compatibility fallback until Windows GTK 3 support is
delivered.

When a Windows GTK 3 build becomes viable, its migration is a Radiant build and
runtime-packaging change: prefer GTK 3 when available, otherwise retain GTK 2.
The shader-preview source should not require an architectural change. The GTK
3 Windows enablement gate is a successful build plus the same preview-shell
smoke test used on macOS and Linux.

## Explicitly deferred

- BSP, MD3, model, sky, fog, and RoQ preview modes.
- A complete parser for every engine and Q3Map2 extension.
- Rewriting existing PK3 files.
- GPU-program/shader-language rendering beyond the portable OpenGL 2.1 path.

## Implementation pitfalls recorded so far

- GTK may warn that legacy GL 2.1 is below its nominal GTK 3 requirement even
  when it successfully creates the required compatibility context. Treat the
  actual context report as the capability result.
- Apple's OpenGL-on-Metal layer can fall back to software fixed-function
  fragment processing. This is a performance concern, not a reason to add
  platform-specific GL code; cache parsing and uploads before changing the
  rendering contract.
- Image loaders do not necessarily share a row-origin convention. Preserve the
  verified texture-coordinate convention and validate it against more than one
  image format before introducing per-format flips.
- Do not rely on Radiant's representative texture as a shader preview. It can
  be an editor image and does not expose the shader's stage stack.
- A global Alpha/Additive/Multiply control is misleading in preview mode.
  Blending belongs to each parsed stage; user editing controls come after the
  source-derived renderer is trustworthy.

These can be revisited after the stage workflow has proven useful.
