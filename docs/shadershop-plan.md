# ShaderShop preview and authoring plan

## Purpose

Add a native GtkRadiant contributed module named **ShaderShop** that makes
Quake III shader work visual and incremental. Preview is the current
functionality and the first interface, not the identity or final scope of the
module. The editor is entered from that preview once the source-derived
rendering workflow is useful and trustworthy.

ShaderShop is not a reimplementation of Q3ASE. It is intended to provide a
small, portable preview and authoring workflow backed by normal `.shader`
files, integrated with GtkRadiant's own VFS, image-loading, plugin, GTK, and
OpenGL facilities.

The target language reference is the original Quake III shader language.
ShaderShop should prefer faithful representation over silently approximating
unsupported semantics.

## Product shape

The `Plugins -> ShaderShop` command opens a modeless ShaderShop window. The
window begins as a shader preview and will ultimately contain an OpenGL
preview, an ordered stage stack, transport controls, source/editor access, and
structured controls where those controls can edit the source without destroying
information they do not understand.

The intended delivery order is:

1. Stable preview surface and selected-shader image.
2. Read-only parsing of one selected shader into an ordered, stage-owned model.
3. Correct stage image loading, compositing, animation, and first fixed-function
   shader operations.
4. Visible stage stack and controlled editing entry point.
5. Lossless shader-document/source editing and shader-file creation.

## Current implementation shape

ShaderShop remains deliberately small at this stage. The current contribution
uses:

```text
contrib/shadershop/
    plugin.cpp
    preview.cpp
    shadershop.h
    shadershop.def
    shadershop.vcxproj
```

The build registration in `config.py` has also been renamed to match
`shadershop`.

Do not split this into a larger parser/document/renderer directory hierarchy
merely for architectural neatness. The next split should happen because a
specific responsibility has become substantial enough to need its own contract,
not because the current preview has to resemble the final editor immediately.

The plugin-facing identity is now **ShaderShop**:

- user-facing name: `ShaderShop`
- Synapse/plugin minor name: `shadershop`
- public functions: `ShaderShop_Show()` and `ShaderShop_RefreshSelection()`
- module/project/definition-file identity: `shadershop`

`preview.cpp` remains an appropriate filename because preview is what that
translation unit currently implements.

## Technical architecture

```text
GTK 2 or GTK 3 window adapter
             |
             v
       Preview controller <---- ordered PreviewStage model
             |
             v
  OpenGL 2.1 renderer <------ preview-owned texture objects
             |
             v
   Radiant VFS and image loader
```

The current implementation may keep these responsibilities in `preview.cpp`
while they are still compact. The architectural boundaries still matter even
before they become file boundaries.

### Plugin boundary

ShaderShop is a regular Synapse plugin and uses established Radiant APIs:

- `PLUGIN_MAJOR` for the menu command.
- `RADIANT_MAJOR` for image loading, source-file loading, logging, dialogs, and
  related editor services.
- `UIGTK_MAJOR` for a Radiant-owned GL widget.
- `QGL_MAJOR` for Radiant's portable OpenGL dispatch table.
- the shader API for non-loading shader lookup and source-file identity.

The plugin front door should remain boring: API-table declarations, plugin
identity, command dispatch, and Synapse registration. Shader interpretation and
rendering should not migrate into `plugin.cpp`.

### Cross-platform GL contract

The renderer is platform-neutral C++ and stays within desktop OpenGL 2.1
compatibility functionality. It must never issue GLX, WGL, CGL, X11, Cocoa, or
Win32-context calls.

The GTK adapter creates the surface through
`g_UIGtkTable.m_pfn_glwidget_new`.

- GTK 2 draws from `expose-event` and explicitly swaps the buffer.
- GTK 3 draws from `GtkGLArea`'s `render` signal; timers and UI callbacks queue a
  render rather than performing GL work themselves.

All preview GL objects belong to the ShaderShop preview context. Preview
textures are not borrowed from or shared with Radiant's main context. CPU image
decoding may occur outside the GL callback, but texture creation, upload, and
deletion must occur while the preview context is current.

On GTK 3, request the first render after realization and again from an idle
callback after mapping. A pre-realize render request can be discarded. Any idle
callback must consult the current live preview widget rather than retain a
destroyed widget pointer.

### Image and file boundary

Use Radiant's `m_pfnLoadImage` image-manager API rather than introducing a
second image decoder. It selects the installed image module and returns RGBA
pixels. ShaderShop uploads those pixels into its own GL texture and releases the
CPU buffer with `g_free`.

Use the shader API's **non-loading** lookup when resolving the current shader.
A lookup that causes Radiant to load or bind a texture from an ordinary GTK
callback can operate under the wrong GL context.

Shader source is read through Radiant's VFS-aware file API.

A packaged asset may be previewed, but ShaderShop never modifies a PK3 in
place. Later authoring writes loose `scripts/*.shader` files. Editing a shader
whose source exists only in a PK3 therefore becomes an explicit override/save
operation.

## Stage ownership rule

The preview stage is now the unit of shader rendering state.

A `PreviewStage` owns the state needed to render that stage, including its
static map or animation source, blend state, decoded image data, animation
frames, and preview GL objects. Animation is not a separate global pass layered
over the completed shader.

This rule is important because Quake III stages are ordered render passes. A
stage-local `animMap` must occupy the same position and use the same stage state
that a static `map` would use.

The implementation should continue moving toward a shape conceptually like:

```text
PreviewStage
    source
        static map
        clamp map
        animMap(frequency, frames...)
        special/generated source
    blend state
    later: rgbGen / alphaGen
    later: tcGen / ordered tcMod
    later: depth/alpha state
```

This does **not** yet require a large class hierarchy. Plain structures are
appropriate.

## Parser boundary rule

The parser must locate exactly the requested shader definition and stop when
that shader's outer closing brace is reached. Stages from later shader
definitions in the same `.shader` file must never leak into the selected
preview.

The current stage-oriented parsing work establishes that outer boundary and
keeps each discovered stage separate.

Before parser fidelity is considered adequate, the tokenizer must also handle
comments explicitly:

- `// ...`
- `/* ... */`

Braces, shader names, or directive-like words inside comments must not
participate in shader or stage parsing.

This is the next parser-hardening step and is intentionally smaller than
building the eventual editor-grade document model.

## Milestones

### 1. Preview shell

Deliver a menu command and modeless window containing an OpenGL 2.1 surface.
Render an internally generated RGBA checkerboard on one textured quad. Capture
the current Radiant shader/texture name and show it in the window.

**Acceptance:** on the macOS GTK 3 build, first open, resize, close, and reopen
all render correctly without GL-context errors. The module also retains the GTK
2 code path.

**Status:** implemented and exercised on macOS GTK 3. The selected shader's
representative image can be shown at its native aspect ratio with source alpha
over a checkerboard. The checkerboard is a preview background, not shader
semantics.

### 2. Read-only selected-shader model

Parse exactly one selected shader's source block into an ordered stage model.

The immediate model remains `PreviewStage` rather than introducing a full
`ShaderDocument` prematurely. Each stage owns its own source and rendering
state.

Recognize and preserve the distinction among:

- `map`
- `clampmap`
- `animMap` frequency plus its complete frame list
- special maps such as `$lightmap` and `$whiteimage`
- stages with no ordinary image source

Also retain stage-local `blendFunc` state and later stage-local render
directives.

**Acceptance:** representative shader files produce the correct stage count,
correct source order, and no stages from neighboring shader definitions.
Multiple `animMap` stages remain independent.

**Status:** partially implemented. Stage ownership and ordered animation
representation are now in place, and the selected shader parser stops at the
selected shader's closing brace.

Still required before this milestone is complete:

- skip line and block comments correctly;
- retain `map` versus `clampmap` as distinct map kinds rather than collapsing
  them to one filename;
- recognize special map tokens explicitly;
- retain unsupported directives instead of simply dropping them;
- make parser keyword recognition compatible with the shader language's
  case-insensitive keywords.

### 3. Playback and compositing

Load stage images into preview-owned GL textures and render the stages once,
in source order. For an animated stage, choose the current animation frame at
that stage's position rather than drawing animation as an extra global pass.

A GTK main-loop timer advances preview time and queues a render. It performs no
OpenGL work.

Implement animation using document time:

```text
frame = floor(time * frequency) mod frameCount
```

The preview clock should eventually be time-based rather than depending on one
global tick count whose cadence happens to match one animated stage. This
matters when different `animMap` stages use different frequencies.

#### Blend correctness before more features

Do not add more stage features until basic `blendFunc` semantics are
unambiguous.

The preview must distinguish:

- no `blendFunc` specified;
- shorthand `blendFunc add`;
- shorthand `blendFunc filter`;
- shorthand `blendFunc blend`;
- explicit source/destination GL factors.

An unknown blend factor must not silently become `GL_SRC_ALPHA`.

The checkerboard is a presentation background. It must not force every shader
stage to behave as though it used conventional alpha blending.

After blend behavior is correct, add the next renderer operations in modest
increments:

1. `tcMod scroll`
2. `tcMod scale`
3. `tcMod rotate`
4. simple `rgbGen`
5. simple `alphaGen`

`tcMod` operations must remain ordered because their composition is
order-dependent.

**Acceptance:** known static shaders using additive, filter, alpha, and
unblended stages display in the expected source order. Known animated shaders
advance, pause, and resume deterministically. Two animated stages with different
frequencies remain independent.

**Status:** initial ordered stage compositing and stage-owned animation are
present. Blend semantics and time-base cleanup remain prerequisites to claiming
preview parity.

### 4. Stage stack and editing entry point

Add a visible read-only stage list first. Once the preview can explain the
source accurately, add controlled Add, Remove, Move Up, and Move Down actions.

`Edit Shader...` should not become a second, simplified material model detached
from the shader source. Structured controls are views and editors of the same
source-backed document.

**Acceptance:** moving two editable stages changes the preview order while
preserving unrecognized source material.

### 5. Lossless shader document and editor

This is the point where the current preview parser should be promoted into a
real source/document subsystem.

The source becomes authoritative. The editor should preserve:

- whitespace;
- comments;
- directive spelling and case;
- directive ordering;
- braces;
- stage ordering;
- unrecognized statements.

Known statements gain typed semantic interpretation, but unknown statements
remain first-class source nodes rather than being discarded.

A suitable eventual shape is:

```text
ShaderFile
    original source / tokens
    ShaderDefinition
        ordered global statements
        ordered Stage nodes
            ordered statements
```

Semantic objects should point back to preserved syntax/source ranges rather
than replacing the source with a newly formatted abstract representation.

#### Lossless editing invariants

1. Opening and saving an untouched shader should produce byte-identical source.
2. A structured edit should alter only the affected statement or stage where
   practical.
3. Unknown directives and comments survive structured edits.
4. Parsing support, semantic evaluation, and preview support are separate
   capabilities.
5. Completely new ShaderShop-authored shaders may use ShaderShop's own
   formatting convention.

A directive can therefore be:

```text
Parsed -> Evaluatable -> Previewable
```

without requiring every parsed directive to be previewable in the current
bare-material context.

Examples:

- `blendFunc`: parsed, evaluatable, previewable.
- `q3map_surfaceLight`: parsed/editable, but not a runtime material-preview
  operation.
- `$lightmap`: understood, but full visual meaning requires map/lightmap
  context.
- `deformVertexes`: can become previewable once the preview has appropriate
  tessellated geometry.

This avoids false visual fidelity while still allowing ShaderShop eventually to
understand the whole language.

**Acceptance:** create or edit a shader, save it to a loose shader file, reload
it in Radiant, assign it to geometry, and reopen it without losing comments or
unrecognized source text.

## Resource lifecycle work

The preview already respects the important context rule: GL work happens under
the preview context. One remaining maintenance item should be completed before
long-running refresh/edit workflows are treated as stable.

When selection or parsed stages are replaced, old CPU image buffers can be
released immediately, but old GL texture IDs must be retired and deleted while
the preview context is current. Do not solve this by casually making the
preview context current inside arbitrary UI callbacks.

A simple deferred-retirement list consumed at the start of the next render is
sufficient. There is no need for a general resource manager yet.

Window destruction must also stop timers and leave no idle/render callback
targeting the destroyed widget/context.

## Style and repository conventions

ShaderShop should look like a GtkRadiant contribution rather than a separate
application dropped into the tree.

Follow the existing Radiant/BobToolz-era source formatting where practical:

```cpp
void Function( int value ){
	if ( condition ) {
		...
	}
}
```

Use tabs for indentation and the repository's spacing/bracing conventions.

Keep normal source-file license/copyright headers.

Useful organizational conventions from established plugins:

- keep plugin/Synapse registration at the plugin front door;
- keep actual operations out of command dispatch;
- use section comments where they make a long implementation easier to scan;
- when the UI becomes large enough, moving GTK construction into a `dialogs/`
  area is reasonable.

Do **not** inherit historical compatibility baggage solely for stylistic
similarity: no new umbrella `StdAfx` layer, Win32 compatibility typedefs, GLX
code, or obsolete GTK abstractions are needed for ShaderShop.

## Compatibility and release policy

The module should retain both active GtkRadiant UI paths.

GTK 3 is the preferred path where a maintained GTK 3 toolchain/runtime is
available. GTK 2 remains the Windows compatibility path until the Windows
GtkRadiant build moves to GTK 3.

Windows GTK 3 enablement is a Radiant build/runtime-packaging project, not a
reason to fork ShaderShop's rendering architecture.

The same preview-shell smoke test should remain the minimum platform gate:

- plugin loads;
- ShaderShop window opens;
- first frame renders;
- resize works;
- selection refresh works;
- close/reopen works;
- no GL-context errors occur.

## Explicitly deferred

- BSP, MD3/model, sky, fog, RoQ, and other scene-context preview modes.
- Full preview fidelity for context-dependent renderer features.
- A complete parser for every engine or Q3Map2 extension before core Quake III
  language support is solid.
- Rewriting existing PK3 files.
- GPU-program/shader-language rendering beyond the portable OpenGL 2.1 path.
- Premature decomposition into many C++ subsystems before the current
  responsibilities justify it.

## Implementation pitfalls recorded so far

- A shader name is not necessarily an image filename. Radiant's representative
  texture can be an editor image and does not expose the stage stack.
- GtkRadiant's shader lookup must not accidentally cause texture loading or GL
  binding from a callback running under the wrong context.
- GTK may warn that legacy GL 2.1 is below its nominal GTK 3 requirement even
  when it successfully creates the required compatibility context. Treat the
  actual context report as the capability result.
- Apple's OpenGL compatibility implementation may make fixed-function
  processing relatively expensive. Cache parsing and uploads before changing
  the portable rendering contract.
- Image loaders do not necessarily share a row-origin convention. Preserve the
  verified texture-coordinate convention and validate it against multiple image
  formats before introducing per-format flips.
- The selected shader's outer closing brace is a hard parser boundary. Parsing
  beyond it can silently incorporate stages belonging to later shaders in the
  same source file.
- Comments are syntax to the parser even though they are not shader statements;
  a tokenizer that merely splits on braces and whitespace can be confused by
  comment contents.
- `animMap` belongs to its stage. A global animation pass destroys stage order
  and stage-local blend semantics.
- `blendFunc` is stage-local. A global Alpha/Additive/Multiply preview selector
  is misleading.
- No `blendFunc` and `blendFunc blend` are not synonymous.
- Unsupported syntax should remain visible and explicit rather than being
  mapped to a plausible-looking fallback.
- Source parsing and source rewriting are different milestones. The preview can
  use a small semantic parser now; destructive pretty-print serialization must
  wait for the lossless source model.

## Near-term sequence

Keep the next work narrow:

1. Build and smoke-test the ShaderShop rename on macOS after the `config.py`
   update.
2. Verify selected-shader parsing stops at the correct outer brace using a
   `.shader` file containing several definitions.
3. Add comment skipping to the current parser.
4. Retain `map` versus `clampmap` in `PreviewStage`.
5. Correct `blendFunc` defaults and shorthand forms.
6. Make animation selection time-based per stage.
7. Add deferred deletion of superseded preview GL textures.
8. Only then begin the first `tcMod` operation.

The larger lossless `ShaderDocument` refactor remains the correct destination,
but it is not the next patch.
