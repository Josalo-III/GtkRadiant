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

1. Preview surface.
2. Image-file stage loading and stage-stack reordering.
3. Frame rate and stage animation.
4. Blend/compositing modes.
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

The shader and VFS APIs are added when the module begins loading or writing
shader documents.

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

### 2. File loader and stack

Add a stage list with Add, Remove, Move Up, and Move Down actions. The initial
selection captured by the preview becomes the document being loaded. Each
stage holds a VFS image path and uploads an independent texture. Draw stages in
their listed order with an initially fixed opaque blend state.

**Acceptance:** two distinct images load; moving either stage immediately
changes the rendered order; failed paths leave the current preview intact and
produce a useful error.

### 3. Time and animation

Add a timer owned by the GTK main loop, Play/Pause, Reset, elapsed-time display,
and a configurable preview frame rate. The timer changes model time only and
queues a render. Add `animMap`-style frame sequences and basic scrolling or
rotating texture coordinates after the transport is stable.

**Acceptance:** animation pauses deterministically, resumes without a time
jump, and does not draw after the window is destroyed.

### 4. Compositing

Expose a small, named set of blend presets first, then the underlying source
and destination blend factors. Add alpha-test, depth-write, depth-function,
constant colour, and alpha controls as the renderer supports them. Map these
directly to the equivalent Quake 3 stage concepts.

**Acceptance:** test fixtures demonstrate opaque, additive, filter, and alpha
blend results in the expected stage order.

### 5. Shader documents and editor entry point

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

These can be revisited after the stage workflow has proven useful.
