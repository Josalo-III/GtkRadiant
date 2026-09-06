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

ShaderShop has two standing obligations that outrank feature count:
**defer to Radiant for language**, and **keep the preview honest about what it
is showing**. The first is covered by the authority hierarchy below. The second
is covered by the compositing-isolation rules, which are new in this revision
and currently the module's largest source of visibly wrong output.

The target language is the original Quake III shader language, but ShaderShop
does not define that language for itself. **Radiant's own parser and shader
consumers are the executable specification for this module.** ShaderShop should
prefer exact deference to those code paths over a cleaner, friendlier, or more
manual-faithful interpretation.

## Authority hierarchy and parser deference

ShaderShop has one primary compatibility target: **the GtkRadiant code that
already reads, displays, and hands shader material into the build workflow.**

The authority order is therefore:

1. **Radiant's parser implementation and exported ScriptLib interface.**
   `radiant/parse.cpp` and `parse.h`, reached by plugins through
   `_QERScripLibTable`, define the lexical token stream ShaderShop must consume.
   ShaderShop must not maintain a parallel tokenizer whose behavior merely
   resembles Radiant's.
2. **Radiant's shader consumers and the build-side consumers used by this
   GtkRadiant tree.** These define how tokens are interpreted in the contexts
   ShaderShop is trying to preview or author.
3. **Observed runtime behavior and the shipped shader corpus.** These are useful
   for testing semantic interpretation and for finding places where a preview
   feature is incomplete.
4. **The Q3Radiant Shader Manual.** The manual remains valuable documentation
   for intent, terminology, and feature coverage, but it is commentary rather
   than an executable specification. If the manual and the code disagree, the
   code wins.

This reverses an earlier assumption in this plan. The manual is no longer
described as authoritative for syntax. The parser is.

### Direct ScriptLib use

`parse.h` explicitly exports the parser for plugins through
`_QERScripLibTable`, and `include/iscriplib.h` exposes the relevant API under
`SCRIPLIB_MAJOR` (`"scriptlib"`):

```text
StartTokenParsing
GetToken
GetTokenExtra
UnGetToken
Token
ScriptLine
TokenAvailable
```

ShaderShop should request `SCRIPLIB_MAJOR` through Synapse, keep a
`_QERScripLibTable` alongside its other Radiant API tables, and feed the
VFS-loaded shader source directly to that parser.

The resulting pipeline is:

```text
selected shader
      |
      v
shader source filename
      |
      v
Radiant VFS load
      |
      v
Radiant ScriptLib / parse.cpp
      |
      v
ShaderShop stage interpretation
      |
      v
preview renderer
```

ShaderShop therefore owns **stage interpretation**, not lexical parsing.

The private tokenizer currently in `preview.cpp` is transitional code. Its
useful discoveries — selected-definition boundaries, quoted-name handling,
commented braces, doubled slashes in paths, and line-scoped argument hazards —
remain valuable regression cases, but the tokenizer itself should be removed
once ScriptLib is wired in.

### Parser behavior is not to be "improved"

Deference means accepting Radiant's real behavior, including behavior that
would be easy to redesign.

For example, `GetToken()` recognizes `//` comments at token boundaries and does
not implement `/* ... */` block comments. ShaderShop must not add block-comment
syntax merely because another parser, manual, or modern expectation would find
it reasonable. If another Radiant shader consumer performs additional handling,
ShaderShop should copy or call that consumer behavior explicitly; it should not
silently insert it at the lexical layer.

Likewise, quoting, whitespace, line tracking, delimiter handling, and token
pushback come from ScriptLib rather than ShaderShop utility functions.

This is especially important for malformed source. A preview that is more
forgiving than Radiant can be more misleading than a preview that fails in the
same place Radiant does.

### Stateful parser call discipline

Radiant's parser is stateful. `token`, `script_p`, `scriptline`, and the unget
state are process-global parser state. ShaderShop must therefore use ScriptLib
synchronously and non-reentrantly:

- load the complete source buffer first;
- call `StartTokenParsing` once for the ShaderShop parse operation;
- complete the parse before returning to GTK or invoking code that may itself
  use ScriptLib;
- do not parse two shader sources concurrently;
- keep the source buffer alive for the whole parse;
- use `UnGetToken` only as the single-token pushback mechanism it actually is;
- **do not leave ScriptLib's cursor pointing into a buffer you are about to
  free.** `parse_selected_stages` calls `StartTokenParsing( buffer )` and then
  `g_free( buffer )`; `script_p` in `radiant/parse.cpp` is a file-scope global
  that still references it. Nothing reads it before the next
  `StartTokenParsing`, so this is not biting today, but ScriptLib is a
  process-wide singleton and leaving a dangling cursor in shared state is a
  landmine for every other consumer. Re-point it at a static empty string
  before releasing the buffer.

Where ShaderShop needs a line-scoped argument list, do not scan the source text
independently. Use ScriptLib's line state. In particular, `GetToken( false )`
does not provide a hard "stay on this line" contract, and `TokenAvailable()` has
its own historical behavior. A safe ShaderShop helper should observe
`ScriptLine()` before and after token acquisition and `UnGetToken()` a token
that belongs to the next line. This keeps even line-boundary decisions inside
Radiant's parser state.

### Manual notes still worth retaining

The original Q3Radiant Shader Manual remains in the Q3 gamepack at:

```text
install/installs/Q3Pack/install/docs/Q3AShader_Manual/
```

It should still be cited when it explains intended semantics or terminology,
but a manual claim is never sufficient reason to make ShaderShop parse source
differently from Radiant.

One already-recorded example remains useful as a documentation warning:
`animMap <frequency>` is described ambiguously in the manual, while the working
software selects frames as a time-rate operation. That observation belongs in
the semantic/evaluation layer, not in the lexical parser.

## Verification corpus

The twelve gamepacks supplied with GtkRadiant 1.6.7 provide 268 `.shader`
files totalling roughly 165,000 lines, holding 9,654 shader definitions under
9,584 distinct names. This remains the measurement surface for preview and
stage-interpretation claims.

The role of the corpus changes under parser deference.

Previously, ShaderShop's private tokenizer was compared against
`radiant/parse.cpp`, reaching:

```text
files 268   tokens 259236   identical 268   differing 0
definitions 9654   agree 9654   differ 0
```

That was useful evidence that the private implementation had converged on
Radiant for the shipped corpus, and it exposed real bugs in the earlier parser.
It is **not** a reason to keep two tokenizers.

After ScriptLib integration, token-by-token parity ceases to be a product
acceptance criterion because ShaderShop and Radiant will literally use the same
tokenizer. Verification should move one layer upward:

- feed the corpus through Radiant ScriptLib;
- exercise ShaderShop's selected-definition and stage interpreter on those
  tokens;
- compare stage counts, source order, map kinds, animation frame lists, and
  interpreted stage state against Radiant/build behavior where an equivalent
  consumer exists;
- keep adversarial cases only where they describe behavior Radiant itself
  accepts or produces.

Any corpus statistics that depended on ShaderShop's private tokenizer should be
re-measured after the ScriptLib conversion rather than copied forward.

### Launch recipe

`apple/run-shadershop.sh` clears pre-rename build artifacts, builds only the
`shadershop` module target, verifies the old artifact did not reappear, and
launches the editor through `apple/macos-env.sh`:

```sh
apple/run-shadershop.sh
```

Pass extra scons arguments after `--`, e.g. `apple/run-shadershop.sh -- -j1`.
`Plugins -> ShaderShop...` opens the preview window once the editor is up. A
machine with no prior GtkRadiant preferences shows the game-selection dialog
first, as it does for any other launch.

### Verify natively

`contrib/shadershop/tests/verify.sh` should remain the native corpus harness,
but its responsibility changes.

The old harness extracted ShaderShop's tokenizer from `preview.cpp` and compared
it with `radiant/parse.cpp`. Once ShaderShop uses ScriptLib directly, retire
that extraction step. The harness should instead compile or exercise the actual
ShaderShop stage interpreter against the actual `_QERScripLibTable` token
stream.

The rule remains: **when in doubt, go native.** The stronger form is now
possible — do not merely compare against Radiant's parser; call it.

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

The rename left a stale `install/modules/shaderpreview.so` in the install tree,
now removed from this checkout. `install/` is gitignored — a build product, not
tracked source — so this is not a one-time fix: it recurs on every checkout or
machine that still has a pre-rename build lying around. Radiant loads every
module it finds, so the obsolete binary registers a second menu command and a
second preview window competing for its own GL context if it is not deleted
before the renamed module is exercised. `apple/run-shadershop.sh` removes it as
a matter of course; see the launch recipe below.

## Technical architecture

```text
GTK 2 or GTK 3 window adapter
             |
             v
       Preview controller <---- ordered PreviewStage model
             ^
             |
Radiant ScriptLib / parse.cpp <---- Radiant VFS shader source
             |
             v
      stage interpretation
             |
             v
  OpenGL 2.1 renderer <------ preview-owned texture objects
             |
             v
        Radiant image loader
```

The current implementation may keep most responsibilities in `preview.cpp`
while they are still compact. The parser boundary is the exception: once
ScriptLib is connected, the private tokenizer should disappear rather than be
promoted into a permanent ShaderShop parser subsystem.

### Plugin boundary

ShaderShop is a regular Synapse plugin and uses established Radiant APIs:

- `PLUGIN_MAJOR` for the menu command.
- `RADIANT_MAJOR` for image loading, source-file loading, logging, dialogs, and
  related editor services.
- `UIGTK_MAJOR` for a Radiant-owned GL widget.
- `QGL_MAJOR` for Radiant's portable OpenGL dispatch table.
- the shader API for non-loading shader lookup and source-file identity.
- `SCRIPLIB_MAJOR` for Radiant's canonical token parser.

The plugin front door should remain boring: API-table declarations, plugin
identity, command dispatch, Synapse registration, and the requested API tables.
Shader interpretation and rendering should not migrate into `plugin.cpp`.

The ScriptLib table should be treated like the other Radiant-owned service
tables, not replaced by directly linking another copy of `parse.cpp` into the
plugin. One parser implementation and one parser state are the point.

### Parser ownership boundary

ShaderShop obtains shader source through the VFS and gives that source to
Radiant's ScriptLib. It does not own:

- whitespace recognition;
- comment recognition;
- quoted-token rules;
- token delimiters;
- line accounting;
- token pushback.

ShaderShop begins at the next layer: recognizing the selected shader definition
and interpreting its ordered statements and stages from the token stream
Radiant supplies.

Any helper added for ShaderShop parsing should therefore operate on ScriptLib
tokens and parser state. It must not walk the raw source pointer to make lexical
decisions that ScriptLib could make differently.

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

On GTK 3, the first frame is driven by `GtkGLArea`'s **`resize`** signal, which
is emitted when the area creates or resizes its own framebuffer and carries the
real buffer dimensions. That is the earliest point at which the surface is
genuinely drawable, which makes it the correct trigger — realize and map both
happen too early to be relied on, and a render request issued before the buffer
exists is discarded.

**Do not size the viewport from the widget allocation.** `gtk_widget_get_allocation`
is not the GL buffer: it is unset until the first size-allocate and takes no
account of the window scale factor. Sizing from it meant an early render found
zero dimensions, returned without drawing, and left the buffer undefined — and
because `GtkGLArea` does not repaint on its own, a render that draws nothing and
queues nothing leaves the window blank until unrelated damage arrives. The
symptom was a preview that opened as a featureless rectangle until the window
was resized by its corner.

Two rules follow, and they generalise beyond this widget:

- **Never return from a render callback without clearing.** An undrawn buffer is
  undefined, not empty.
- **A frame that could not draw must queue another**, under a bounded retry
  count. Unbounded re-queueing spins a core when the surface never becomes
  drawable, which is worse than the blank frame it was meant to fix.

Any idle callback must consult the current live preview widget rather than
retain a destroyed widget pointer.

**GTK 2 must actually compile.** The compatibility policy below keeps GTK 2 as
the Windows path, but the current preview calls `gtk_widget_set_hexpand` and
`gtk_widget_set_vexpand` unguarded. Both are GTK 3.0 API and are used nowhere
else in the tree. Every GTK 3 entry point in this module belongs inside the
existing `GTK_CHECK_VERSION` guards, and the GTK 2 path is not "retained" until
it has been compiled.

### Image and file boundary

Use Radiant's `m_pfnLoadImage` image-manager API rather than introducing a
second image decoder. It selects the installed image module and returns RGBA
pixels. ShaderShop uploads those pixels into its own GL texture and releases the
CPU buffer with `g_free`.

Use the shader API's **non-loading** lookup when resolving the current shader.
A lookup that causes Radiant to load or bind a texture from an ordinary GTK
callback can operate under the wrong GL context.

Shader source is read through Radiant's VFS-aware file API. `m_pfnLoadFile`
resolves to `vfsLoadFile`, which allocates with `g_malloc` and appends a
terminating null, so `g_free` is the correct release and the buffer may be
treated as null-terminated.

That null-terminated VFS buffer is passed to
`g_ScripLibTable.m_pfnStartTokenParsing`; ShaderShop should not make a second
lexical copy merely to parse it.

A packaged asset may be previewed, but ShaderShop never modifies a PK3 in
place. Opening any shader creates a private, mutable in-memory document copied
from the VFS source; preview and editing always operate on that copy, never on
the source file in situ. Later authoring writes loose `scripts/*.shader` files.
Editing a shader whose source exists only in a PK3 therefore becomes an
explicit override/save operation.

Saving an existing shader is deliberately a two-step replacement operation.
`Save As` initially proposes its existing loose shader filename (or the
corresponding loose `scripts/*.shader` override name for a packaged source),
but does not silently replace it. The user must retain or re-enter that target
name and explicitly accept the normal overwrite warning before ShaderShop
replaces an existing `.shader` file. Until then, all changes remain only in the
private in-memory document.

## Fault tolerance policy

ShaderShop should be robust, but **robustness must not change the language
Radiant sees**.

The previous policy said "tolerate, follow the engine, and report." With parser
deference, that becomes:

**defer, interpret conservatively, and report preview limitations.**

- *Defer.* Lexical behavior and lexical failure behavior come from Radiant
  ScriptLib. ShaderShop does not add comment forms, quote recovery, brace
  recovery, or token defaults that cause malformed source to mean something
  different from what Radiant sees.
- *Interpret conservatively.* Once Radiant has produced tokens, ShaderShop may
  continue past statements it does not yet preview, provided doing so does not
  alter stage boundaries or invent semantic defaults.
- *Report preview limitations.* Unsupported directives, failed image loads,
  unresolved stage operations, and source-acquisition failures should be
  visible. A successful token parse is not the same thing as a faithful
  preview.

Diagnostics are collected per selection and cleared with it. Console output is
capped so that one shader cannot flood the log; the full count remains visible
in the window.

Parser diagnostics produced by Radiant itself should not be duplicated with a
second ShaderShop lexical diagnostic system.

**Accepting a directive and then rendering something else is the worst
outcome available.** It is worse than rejecting the directive, because the
diagnostics stay silent and the preview looks authoritative. The current
`rgbGen wave` and `tcMod stretch` handling validates both `sin` and `square`,
stores neither, and evaluates everything through `sinf` — a `square` wave is
recognised, accepted, and quietly drawn as a sine. Either carry the waveform
through to evaluation or report it as unsupported; there is no third option
that keeps the preview honest.

Two consequences remain important:

- **A failed animation frame is retained as an empty placeholder.** Dropping it
  would renumber every later frame, changing animation semantics after parsing.
- **A default-initialised semantic value is a decision.** If ShaderShop cannot
  resolve a legal Radiant token sequence into a preview state, it should report
  that state as unsupported rather than silently substitute a plausible one.

The stage summary must not collapse unrelated failures into one message.
Source acquisition and parsing should distinguish at least:

```text
no current shader
shader lookup unavailable
shader object not found
shader has no source filename
VFS load failed
selected definition not found in loaded source
selected definition parsed with zero stages
```

**Status:** implemented as `ShaderSourceState`. The status line now separates a
raw image with no `.shader` definition from a definition that failed to parse,
from source that could not be acquired. This was the right split and should be
extended rather than flattened as new failure modes appear.

## Compositing isolation

A shader stage stack is a sequence of blend operations against a destination.
The preview therefore has to be careful about *what the destination is*, because
several `blendFunc` factors read it. If the preview's own scaffolding is sitting
in that destination, the scaffolding becomes an operand and the material is
composited against something that does not exist in the game.

Two rules follow. Both are currently violated, and together they are the reason
the preview washes out.

### The checkerboard is presentation, never an operand

The checkerboard exists so a human can see transparency. It has no shader
meaning. But the current renderer draws it into the same buffer the stages then
composite against, so every destination-reading factor multiplies it into the
result:

```text
GL_DST_COLOR   GL_ONE_MINUS_DST_COLOR   GL_DST_ALPHA   GL_ONE_MINUS_DST_ALPHA
```

`blendFunc filter` — `GL_DST_COLOR GL_ZERO` — is one of the most common
directives in the language. Measured over the corpus:

```text
shader definitions                                        9654
  containing a destination-reading blend factor           4055   42.0%
```

Two in five shipped shaders currently multiply a 176/72 grey checker pattern
into their own material. The checker squares are not subtle; at 0.69 and 0.28
they change both the brightness and the pattern of the result.

The fix is structural, not a tweak to the checker colours. No shader stage may
ever sample the checkerboard.

An offscreen composite is the clean general form, but it is not currently
reachable: `_QERQglTable` exposes no framebuffer-object, renderbuffer,
`glReadPixels`, or `glCopyTexImage2D` entry points, so there is nowhere to
composite to and no way to recover the accumulated alpha. Extending the QGL
table is a Radiant-wide change and is not justified by this alone.

The implemented form works within that constraint by observing when the
checkerboard is *provably* harmless. A stack can only be affected by what sits
behind it through its destination factor, so:

- destination scaled by `GL_ZERO` (opaque replacement) or by
  `GL_ONE_MINUS_SRC_ALPHA` (conventional transparency) — the checkerboard either
  cannot contribute or contributes exactly as a background seen through a
  translucent surface, which is the one reading the checkerboard is *for*;
- anything else — the destination is data, and a patterned field is multiplied,
  inverted, or added into the material.

In the second case the checkerboard is replaced by a defined uniform field and
the substitution is reported. Transparency indication is lost for those
shaders, which is the correct trade: a stack that reads its destination as data
has no transparency to indicate.

**Only the first drawn stage can see the backdrop.** A stage blends against the
destination, but for every stage after the first that destination is *what the
earlier stages wrote*, not the backdrop. Classifying a stack by whether **any**
stage reads its destination was therefore wrong, and wrong in a way that showed:
a trailing `map $lightmap` / `blendFunc filter` stage — itself a preview
fabrication — made an entire stack look multiplicative and forced a uniform
light field under materials whose first stage was an ordinary opaque or
alpha-blended texture. A dark material then read as a pale one.

`textures/outrage/obs` is the case that exposed it: three stages, the first an
opaque chrome environment map, the last a `$lightmap` filter. The old rule saw
the filter stage and put a light field underneath; the opaque first stage should
have made the backdrop irrelevant. `obs-plain` was worse — its single textured
stage is `blendFunc blend` over a `.tga` whose alpha averages 0.078, so 92% of
what was displayed was the fabricated light field rather than the material.

Classification now examines only the first drawn stage. Across the shipped
corpus plus a real authored set, this moves 2,117 of 9,661 definitions (21.9%),
almost all of them from a uniform field back to the checkerboard:

```text
backdrop      all-stages   first-only
checkerboard        3699         5750
uniform light       4211         2977
uniform dark        1751          934
```

`textures/sfx/fanfx` — the case the uniform backdrop was built for — is
unchanged, because its *first* stage is the one reading the destination.

**Two fields are required, not one.** The two ways of reading a destination want
opposite ones. A multiplicative stage over a dark field yields darkness and
loses everything; an additive stage over a light field saturates to white.
Multiplication decides when a stack does both, because washing out preserves
more signal than multiplying by nearly zero.

**The light field is not its own constant.** A stage that multiplies its
destination is asking what light falls on the surface — the same question
`map $lightmap` asks. Holding those as two separate values produced a
lightmap slider that appeared to do nothing, because a fixed light field stood
in front of it for exactly the shaders the slider was meant to affect. They are
one control: the field is named **Lightmap** and the slider drives both it and
the `$lightmap` stand-in. The dark field stays fixed, because an additive stage
is not asking about lighting.

```text
backdrop chosen across 9654 corpus definitions
  checkerboard (provably safe)          3656   37.9%
  uniform light (multiplies destination) 4301   44.6%
  uniform dark  (adds to destination)    1697   17.6%
```

**Status:** implemented, and now user-selectable. The automatic classification
is the default, and a preview-level **Backdrop** control offers Auto,
Checkerboard, Lightmap, Dark, and Image — where Image accepts a `.shader` source
as well as an image file. An explicit choice is honoured as given —
a user compositing a shader over a chosen image has taken responsibility for
what the destination means. Neither uniform level is pure black or white, so
neither can be mistaken for shader content, and the status line names the
backdrop in use.

### Why an image backdrop is a preview control, not an editor feature

How `fanfx` is *used* settles this. In `museum.map` it appears exactly once, and
the brush is a one-unit-thick plate whose other five faces are all
`common/nodraw` — a dedicated overlay surface contributing nothing but that one
face. The next brush is the same construction carrying `sfx/fan`, the opaque fan
image.

The shader is therefore authored as a layer over other geometry. Its destination
in the game is the fan and the room behind it. A uniform field makes its
arithmetic *legible*, but it cannot show what the shader is *for*; only
compositing it over the thing it modulates can. That is a preview capability, so
the backdrop control belongs beside the preview rather than waiting for the
editor.

A consequence that had to be designed in rather than deferred: the useful
backdrop for this case is another **shader**, not an image. `sfx/fan` is itself
a shader, and most candidate backdrops are — an overlay is authored against
another material, and that material is rarely a bare `.tga`. Worse for a plain
file chooser, most of them live inside PK3s and are reached through the VFS
rather than the filesystem.

The right source for that list is **Radiant's own active shader list**, not the
filesystem. Every shader loaded alongside the current map is already in memory,
reachable through `m_pfnGetActiveShaderCount` and `m_pfnActiveShader_ForIndex`,
and `IShader::IsInUse()` marks the subset the map actually uses. Choosing from
that list means:

- no file picker for the common case — the backdrop control opens a menu;
- **PK3s stop being a problem entirely**, because a shader inside a pak is in
  the active list like any other. Reaching one through a file chooser was never
  going to work, and deferring PK3 support is no longer necessary for this
  feature;
- the names offered are the ones Radiant will actually resolve, so a chosen
  backdrop cannot fail to exist.

The menu is grouped by the directory part of the shader name, the same grouping
the texture browser uses, because the active list runs to thousands of entries
and one flat menu would be unusable. Shaders in use by the current map are
offered as their own group ahead of the full set.

Loading a `.shader` file directly is retained behind `From file...` for sources
that are not loaded — a shader being authored, or one from a set Radiant has not
been pointed at. That path still has to solve selection, because a shader file
holds many definitions (`sfx.shader` alone holds **129**):

- one definition in the file: applied directly, no dialog;
- many: the definitions are listed in file order and the user picks.

A chosen definition is parsed into its own stage list and composited as the
backdrop, over the lightmap field, before the selected stack is composited over
that. It is bounded by construction — a backdrop never gets a backdrop of its
own — and it does not touch the preview clock, so a backdrop that animates
cannot redefine the timebase the selected shader is being judged against.

The museum case needs neither: with the map open, `textures/sfx/fan` is in the
active list — and in the "used by this map" group — so previewing `fanfx` over
it is two clicks.

The worked case is `textures/sfx/fanfx`, which is a single stage of
`blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR` and carries `surfaceparm nolightmap`,
so it isolates this defect from the `$lightmap` one. A `GL_ZERO` source means
the result is `dst * (1 - src)` — output that is *entirely* a function of the
destination. Against the checkerboard the checkerboard was the picture, masked
by the fan; against the clear colour it was almost black. It now resolves to
`0.62 * (1 - src)`: a uniform field carrying the fan pattern and nothing else.

### A fabricated neutral input is a semantic lie

`$lightmap` currently resolves to a generated 1x1 pure white texture. White is
the identity for a filter stage, so this was chosen to be harmless. It is not
harmless: it asserts a fully lit surface.

```text
  containing map $lightmap                                3449   35.7%
  containing both                                         3438   35.6%
```

A real lightmap is usually much darker than white and is the main reason a
lightmapped material does not read at full brightness in game. Substituting
white makes roughly a third of the corpus preview systematically brighter and
flatter than the material will ever appear on a surface — and because 35.6% of
definitions hit both problems at once, the same shaders are also being
multiplied by the checkerboard.

This is the same category error the fault-tolerance policy already forbids
elsewhere: inventing a plausible value for something the preview cannot know,
and then not saying so. `$lightmap` in a bare material context is *not
previewable*. It should be represented as an explicit, visible unknown — in the
stage list, in the diagnostics, and in the composite — rather than silently
resolved to the one value that makes the arithmetic disappear.

**Status:** implemented as a preview-level control. The generated lightmap is a
white 1x1 texture modulated at draw time by a **Lightmap** slider, so the
fabricated value is visible, adjustable, and reported rather than hidden in the
renderer. Applying it as a colour multiplier rather than baking it into the
texture means moving the control needs no re-upload — which matters while
texture retirement is still outstanding.

The default is **identity**, which is what reproduces the engine. A `filter`
lightmap stage at 1.0 is a no-op, so the material shows its own colours — the
right neutral for a material swatch. Quake also doubles the lightmap through
overbright bits, so a normally lit surface reaches the screen at roughly
identity, and a preview only agrees with the game here. Lowering the control is
how a shadowed condition is inspected.

An earlier revision defaulted this to 0.5 on the argument that a white default
would be the old assertion with a control attached. That reasoning was wrong:
what made the original substitution a lie was that it was hidden and unreported,
not its value. Once the fabrication is a labelled control with a visible
position, the honest default is the one that matches the engine — and comparison
against a real map confirmed it.

The control generalises past `$lightmap`: inspecting a material under different
lighting is the same question `rgbGen identityLighting` and overbright raise, and
those are equally context-free in a bare swatch. It is named for the general
idea, not for the directive that forced it.

Genuine baked data remains the real answer, and waits on a model/BSP preview
mode.

`$whiteimage` is the opposite case and should not be confused with it. The
engine really does generate an internal white image, so a generated 1x1 white
texture is an exact representation rather than a stand-in — subject to
confirming the behaviour in the Radiant/build consumer.

### The host renderer can invalidate all of this

A preview that composites correctly can still be wrong after the fact. During
this work every material read too bright, and the cause was not in ShaderShop at
all: `radiant/glwidget.cpp` enabled `gtk_gl_area_set_has_alpha( area, TRUE )`
during the GTK3 port.

`GtkGLArea` uses that alpha channel to blend the GL output with the widget
background. GtkGLExt under GLX never did — the drawable was opaque and the alpha
channel inert — so fifteen years of code that ignored framebuffer alpha was
suddenly meaningful. Radiant's texture browser clears with `alpha 0`, so it went
fully transparent and rendered as the GTK theme's white, taking its white
texture labels with it. And any stack ending on an alpha-blended stage left a
low framebuffer alpha, so its material was composited toward white regardless of
the colour it had computed.

`textures/outrage/obs` was the case that exposed it. Tracing its alpha:

```text
clear                                            A = 1.00
stage 1  GL_ONE / GL_ZERO                        A = 1.00
stage 2  GL_ONE_MINUS_SRC_ALPHA / GL_SRC_ALPHA   A = 0.078*0.922 + 1.0*0.078 = 0.15
stage 3  GL_DST_COLOR / GL_ZERO                  A = 0.15
```

85% of what was displayed was the widget background. The colour ShaderShop
computed was correct throughout; it was being composited away afterwards.

Radiant's GL views are opaque viewports and are now created with
`has_alpha FALSE`.

The lesson is a boundary, not a bug: **ShaderShop can only be as correct as the
surface it draws on.** When output disagrees with the game across *every*
material rather than a class of them, suspect the host GL surface before the
stage interpreter. A symptom that uniform is not about blend semantics.

## Preview coordinate space

The preview quad is built in **pixel coordinates**, with the origin at the
viewport's bottom-left corner. That was fine while the only projection was a 2D
ortho over the same pixel range. It stops being fine as soon as anything is
*origin-relative*, because the origin is a corner of the screen rather than the
centre of the material.

Two features have already been built on top of that assumption and are wrong as
a result:

- **3D inspection orbits the corner, not the swatch.** The 3D path scales pixel
  coordinates into a normalised ortho volume without first re-centring, so the
  quad's centre lands away from the origin — at `(1.00, 0.78)` on a 640x500
  widget, and exactly on the top-right corner `(1.00, 1.00)` at 500x500.
  `qglRotatef` then rotates about the origin, so orbiting swings the material
  around a point off its own corner instead of turning it in place.
- **`tcGen environment` could not produce a reflection.** `GL_SPHERE_MAP`
  derives texture coordinates from the eye-space reflection vector. The quad sat
  at eye `z = 0`, which put the eye vector in the surface plane: with a constant
  normal `(0,0,1)`, `n·u` was zero and the reflection collapsed to
  `normalize(x, y, 0)` — a ring anchored on the *pixel-space origin*, determined
  by where the quad happened to sit in the viewport rather than by surface
  orientation. Note this was a consequence of the surface lying in the eye
  plane, not of the projection being orthographic; eye-space position varies
  across the quad under ortho too, once the surface is held off that plane.

The rule going forward: **preview geometry is defined in a centred, unit
material space, and the projection maps that space to the viewport.** Pixel
extents are a property of the projection, not of the vertices. Any feature that
depends on orientation, rotation, or an eye vector must be expressed in that
centred space.

A second requirement follows for any reflection-based generator: **the material
must be held off the eye plane.** A surface at eye `z = 0` degenerates the eye
vector into the surface plane regardless of projection. A fixed eye offset keeps
it well defined, and because `glNormal3f` is specified in object space and
transformed by the modelview, the reflection then tracks the material as it is
orbited.

A flat quad still only ever shows one surface orientation, so environment
mapping remains an approximation until there is curvature or a perspective
camera to vary the normal across the surface. What inspection mode buys is the
ability to vary that single orientation interactively and see the reflection
respond.

**Status:** implemented. Preview geometry is defined in centred material space
with the longer side spanning one unit; view extents, margin, and zoom belong to
the projection. Verified numerically across viewport and material aspect ratios:
the material centre lands on the origin exactly, the constraining axis fills
78% as before, material aspect is preserved, the rotated quad stays inside the
depth range, and the eye vector is non-degenerate at every corner. Orbit
rotation and pointer panning now operate on the material rather than on a window
corner, and panning converts pointer pixels through the live view extents so a
drag tracks the cursor at any zoom.

### Credible 3D transform inspection

The current 3D inspection control rotates a single material plane. That is
useful for checking the response of a flat `tcGen environment` stage, but it
does not yet make vertex-dependent effects credible: every vertex has the same
normal relationship and there are too few samples to show a curved or turbulent
surface. It must therefore remain a diagnostic view, not evidence that a shader
with 3D transforms has been faithfully previewed.

The next preview milestone is a tessellated material surface in inspection mode
with a shared, ordered coordinate evaluator. The work is deliberately ordered:

1. Preserve each `tcMod` directive in source order and evaluate that chain at
   every vertex, rather than retaining independent scroll/rotate/scale fields.
2. Render a sufficiently dense material grid in 3D inspection mode while
   keeping the current 2D swatch as the exact compositing reference.
3. Apply `deformVertexes wave` to that grid so orbit reveals genuine changing
   shape, normals, and perspective.
4. Add `tcMod turb` through the same per-vertex evaluator. It is a nonlinear
   UV displacement and cannot be represented by the existing texture-matrix
   shortcuts.

`textures/outrage/runninglava` is the first acceptance shader: at an oblique
orbit it must visibly ripple and scroll as one surface, without changing the
stage order or compositing established by the 2D preview. This sequence puts
the geometry needed to judge a 3D effect in place before expanding the language
support that depends on it.

## Stage ownership rule

The preview stage is the unit of shader rendering state.

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
    later: alphaFunc
    later: rgbGen / alphaGen
    tcGen
    tcMod operations (ordered source list)
    later: deformVertexes / surface geometry
    later: depth state
```

This does **not** yet require a large class hierarchy. Plain structures are
appropriate.

## Parser boundary rules

The parser boundary is now defined by **ownership**, not by ShaderShop's own
tokenization rules.

### Lexical boundary: Radiant owns it

All source tokenization comes from `_QERScripLibTable`. ShaderShop should delete
its private `next_token`, comment scanner, quoted-token reader, raw
line-scanner, and related lexical helpers once the ScriptLib path is working.

Do not preserve those helpers as a fallback. A fallback tokenizer recreates the
very ambiguity this decision removes.

### Definition boundary: ShaderShop still owns it

ScriptLib provides tokens, not the selected `PreviewShader`. ShaderShop must
still locate exactly the shader definition corresponding to the selected
Radiant shader and stop at that definition's outer closing boundary.

That logic should be expressed entirely in terms of Radiant tokens.

The important invariants remain:

- a selected name must not accidentally match an in-body map or editor-image
  token;
- stages from a neighboring definition must never leak into the selected
  preview;
- duplicate-name resolution should match Radiant's shader manager behavior;
- name comparison should match Radiant's own lookup behavior rather than impose
  a new case policy.

Where Radiant's existing shader consumer already contains the rule, prefer
calling or mechanically following that code over deriving the rule from the
manual.

### Stage line boundaries

Several shader directives use the rest of the current source line as an
argument list, notably `animMap`.

ShaderShop must not recover this by scanning the raw file pointer after
ScriptLib has tokenized the source. Use `ScriptLine()` to establish the line of
the directive, obtain tokens through ScriptLib, and `UnGetToken()` the first
token that belongs to the following line.

This is deliberately a thin adaptation around Radiant's parser state, not a
second parser.

### Case behavior

Case handling belongs to the semantic consumer layer, not to tokenization.

Where Radiant's shader manager performs case-insensitive shader-name lookup,
ShaderShop should do the same. Where image paths retain their spelling for the
VFS/image loader, ShaderShop should preserve them. Keyword and blend-factor
comparison should follow the actual Radiant/build consumer behavior.

The manual may explain why a distinction exists, but it does not override the
code.

## Loader seam

The backdrop image, the eventual stage-source browser, `Create Shader...`, and
the shader document all need the same thing: resolve a name the user chose into
something the preview can draw, through Radiant's VFS and image manager rather
than a private decoder.

That seam exists now as `PreviewImageSource` — a named image owning its CPU
buffer and its preview texture, loaded by `preview_source_load` through the same
`load_image` path the stages use, so VFS resolution and the extensionless retry
come free. `preview_source_relative_name` trims the game path from a chooser's
absolute filename so the image manager resolves it the way a shader reference
would, and reports plainly when the chosen file lies outside the VFS instead of
failing silently.

Alongside it, the parser was split so that parsing is not tied to the selection.
`parse_definition_into` takes its target stage list, source buffer, and name, so
the same code serves the selected shader and a backdrop shader;
`enumerate_definitions` lists every definition header in a source in file order;
and `clear_stage_list` / `load_stage_images` take the list they operate on. The
clock update is a parameter rather than an assumption, because only the selected
shader may define the preview timebase.

That split is what made a shader backdrop possible without a second parser, and
it is the same split the editor needs. What remains to be added on this seam:

- browsing and replacing a stage's `map` source from the stage list;
- reading a shader document for editing, which is the same VFS load the parser
  already performs;
- a PK3-aware file browser, should a source that Radiant has *not* loaded ever
  need reaching; the active-list menu removed the urgency.

`Create Shader...` exists in the control strip and reports honestly that it
needs the document layer. It is a placed track, not a stub pretending to work:
the button's presence fixes where the action lives, and its message states that
authoring writes a loose `scripts/*.shader` file rather than touching a PK3.

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

The GTK 2 half of the acceptance criterion is currently **not met**: the module
does not compile against GTK 2 because of the unguarded expand calls noted in
the GL contract section.

### 2. Read-only selected-shader model

Parse exactly one selected shader's source block into an ordered stage model,
**using Radiant ScriptLib as the only tokenizer**.

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

**Acceptance:**

- ShaderShop requests and receives `SCRIPLIB_MAJOR`.
- The VFS-loaded shader buffer is parsed through `_QERScripLibTable`.
- No ShaderShop lexical tokenizer remains in the production path.
- The selected definition is found without matching an in-body reference.
- Parsing stops at the selected shader's outer boundary.
- Representative shader files produce the correct stage count and source
  order.
- Multiple `animMap` stages remain independent.
- Corpus verification exercises the stage interpreter above ScriptLib rather
  than comparing two tokenizers.

**Status:** met. ShaderShop requests `SCRIPLIB_MAJOR` through Synapse and parses
the VFS-loaded selected definition through `_QERScripLibTable`. The private
tokenizer has been removed outright — no lexical helper, comment scanner,
quoted-token reader, or raw line scanner remains in the production path. Line
scoped arguments go through `ScriptLine()` plus `UnGetToken()`. Stage order,
`map` versus `clampmap`, `animMap` frame lists, special map tokens, and
stage-local blend state are preserved, and the zero-stage status has been split
into concrete source/lookup/parse states.

One registration note worth keeping: the shader API is a normal Synapse
wildcard requirement. Registering `SYN_REQUIRE_ANY` directly leaves the table
unpopulated, which presents as an unexplained zero-stage result.

The earlier native parity result — 259,236 identical tokens across 268 files —
was evidence that the old tokenizer had learned the right lessons. It is not an
architecture, and it is no longer maintained. Radiant's parser is now simply
the parser.

Remaining in this milestone:

- retain unsupported directives as first-class source rather than dropping
  them, which is a prerequisite for milestone 5 rather than for the preview.

The corpus cases that found the old opening-boundary, quoted-name, doubled-slash,
and commented-brace defects remain valuable tests. Their expected result is now
"ShaderShop behaves as Radiant does," not "ShaderShop's tokenizer agrees with
Radiant."

### 3. Playback and compositing

Load stage images into preview-owned GL textures and render the stages once,
in source order. For an animated stage, choose the current animation frame at
that stage's position rather than drawing animation as an extra global pass.

A GTK main-loop timer advances preview time and queues a render. It performs no
OpenGL work.

Implement animation using document time:

```text
frame = floor( time * frequency ) mod frameCount
```

`frequency` is treated as frames per second by the working software path recorded
above. The preview clock must be time-based rather than depending on one global
tick count whose cadence happens to match one animated stage. This matters as
soon as two `animMap` stages use different frequencies.

#### Blend correctness before more features

Do not add more stage features until `blendFunc` semantics are unambiguous.
Measured against the corpus, the current implementation resolves **3,341 of
12,559 `blendFunc` directives (26.6%) to the wrong blend state**, in three
overlapping ways:

- **1,793** carry a factor token the lookup does not recognise and which falls
  through to `GL_SRC_ALPHA`. Two causes: token case, since the lookup matches
  `GL_ONE` and `one` but not the very common `gl_one`; and two factors absent
  from the table altogether, `GL_SRC_COLOR` and `GL_ONE_MINUS_SRC_COLOR`. The
  latter are not obscure — the `SRC_COLOR` family accounts for 1,231 of these,
  and `GL_DST_COLOR GL_SRC_COLOR` is the documented detail-stage combination.
  Before locking the preview behavior, confirm the same interpretation in the
  Radiant/build consumer.
- **849** use a shorthand form. The parser demands two factor tokens, finds a
  newline where the second should be, and discards the directive, so the stage
  keeps its default. These are now reported rather than dropped silently, but
  they are still not resolved.
- **1,529** spelled the keyword in a case the parser did not match before the
  boundary work. They now parse, and mostly land in one of the categories above
  instead.

A related fidelity note found in the same authored set: shaders routinely name a
`.tga` for an asset that ships as `.jpg` (`ragechrome_env`, `ragegilt_env`). The
extensionless VFS retry in `load_image` is what resolves these, and without it
those stages silently would not draw. It is load-bearing, not a convenience.

The preview must distinguish the following states. The manual documents these
forms in section 6.2, but the resolved behavior must be checked against the
Radiant/build consumer before it is treated as final:

- no `blendFunc` specified — **no blending at all** (6.2.5), not alpha blending;
- `blendfunc add` — `GL_ONE GL_ONE`;
- `blendfunc filter` — `GL_DST_COLOR GL_ZERO`, equivalently `GL_ZERO
  GL_SRC_COLOR`;
- `blendfunc blend` — `GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA`;
- explicit source/destination GL factors, from the full valid sets in 6.2.3 and
  6.2.4, which are not symmetric between source and destination.

An unknown blend factor must not silently become `GL_SRC_ALPHA`. It should be
reported, in keeping with the rule that unsupported syntax stays visible.

Section 6.2.5 also notes the engine warns when any stage after the first omits
`blendFunc`. Surfacing that as a ShaderShop diagnostic is cheap and matches the
same rule.

The checkerboard is a presentation background. It must not force every shader
stage to behave as though it used conventional alpha blending.

#### Special map sources

`$whiteimage` is not an approximation problem if the Radiant/build consumer
confirms the documented behavior: section 6.1.1 describes an internally
generated white image, for which a generated 1x1 white texture is an exact
preview representation. Forty-two corpus stages would then become correct for
almost no work.

`$lightmap` cannot be correct without map and lightmap context, and it accounts
for 3,463 corpus stages. Those stages fail to load and draw nothing. They are
now *reported* as not previewable rather than vanishing without explanation, but
they still need a neutral placeholder so the stage remains visible in the
composite. Note also that 6.1.1 requires `rgbGen
identity` to accompany `$lightmap`, which matters once `rgbGen` lands.

#### Subsequent renderer operations

After blend behaviour is correct, add operations in modest increments:

1. `alphaFunc` — promoted ahead of `tcMod`. Section 6.10 defines only `GT0`,
   `LT128`, and `GE128`; all three map directly onto fixed-function
   `glAlphaFunc`, need no time base and no ordered composition, and are visually
   decisive. Grates, screens, and fences read as solid without it.
2. `tcMod scroll`
3. `tcMod scale`
4. `tcMod rotate`
5. simple `rgbGen`
6. simple `alphaGen`

`tcMod` operations must remain ordered; section 6.6 states coordinates are
modified in the order the directives appear, and composition is
order-dependent. The current implementation applies scroll, stretch, rotate,
scale, and transform in a fixed sequence written into the renderer, which is
only accidentally correct when a stage happens to list them that way. The stage
model needs an ordered list of `tcMod` operations, not a set of independent
flags, and both the 2D and tessellated 3D paths must use that same evaluator.
That is also the prerequisite for `tcMod turb`: turbulence is nonlinear,
time-dependent per-vertex UV displacement rather than another matrix entry.
Note that the corpus spells the keyword `tcmod` 1,800 times against `tcMod`
4,004, so case folding is a prerequisite here too.

When `rgbGen` arrives, its default is not constant: section 6.3 selects
`identityLighting` for additive and blended stages and `identity` for filter
stages. The default therefore depends on the stage's resolved blend mode, which
is a further reason blend correctness comes first.

**Acceptance:** known static shaders using additive, filter, alpha, and
unblended stages display in the expected source order. Known animated shaders
advance, pause, and resume deterministically. Two animated stages with different
frequencies remain independent. Re-measuring the corpus shows no `blendFunc`
directive resolving to an unintended factor.

**Status:** substantial progress, with one class of defect now blocking further
feature work.

Working: ordered stage compositing and stage-owned animation; blend-factor case
folding; `add` / `filter` / `blend` shorthand; the `SRC_COLOR` family in the
factor table; the corrected `GL_ONE`/`GL_ZERO` default for a stage with no
`blendFunc`; `clampmap` mapped to clamp-to-edge so jumppad rings do not tile;
`alphaFunc`; `rgbGen wave`; `tcMod scroll`, `scale`, `rotate`, `transform`, and
`stretch`; and the extensionless VFS retry that lets a mod ship a format other
than the one the shader names.

**Fixed since:** the checkerboard is no longer an operand. It is now used only
where it provably cannot affect the result, and a defined uniform backdrop is
substituted elsewhere, with the choice reported. Preview geometry is centred, so
orbit and reflection generators act on the material.

**Still blocking:** `$lightmap` fabricates white for 35.7% of corpus
definitions. Until that stops asserting a fully lit surface, brightness
comparisons against in-game appearance remain unreliable for roughly a third of
the corpus.

Also outstanding: `square` waveforms are accepted but evaluated as sine (see the
fault-tolerance policy); `tcMod` operations are applied in a fixed internal
order rather than source order; and preview time is still derived from a shared
tick counter rather than elapsed time.

### 4. Stage stack and editing entry point

Add a visible read-only stage list first. Once the preview can explain the
source accurately, add controlled Add, Remove, Move Up, and Move Down actions.

The stage list is also where unsupported and non-previewable material becomes
visible rather than merely absent: a `$lightmap` stage, an unrecognised
directive, or a stage whose image failed to load should each be legible in the
list.

`Edit Shader...` should not become a second, simplified material model detached
from the shader source. Structured controls are views and editors of the same
source-backed document.

**Acceptance:** moving two editable stages changes the preview order while
preserving unrecognized source material.

### 5. Lossless shader document and editor

This is the point where the current preview parser should be promoted into a
real source/document subsystem.

Opening makes an isolated, mutable `ShaderDocument` copy of the selected
definition's VFS source. The preview may re-render that document immediately as
edits are made, while the installed loose file remains untouched. Saving is a
separate operation with dirty-state tracking and an overwrite confirmation for
an existing target; it must never be an implicit consequence of changing a
control or closing the preview.

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

Preserving directive case is a real requirement, not a nicety: the corpus
contains `blendFunc`, `blendfunc`, `BlendFunc`, and `Blendfunc`, and a
round-trip that normalises them has rewritten the author's file.

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
- `alphaFunc`: parsed, evaluatable, previewable.
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

The preview respects the important context rule: GL work happens under the
preview context. The remaining lifecycle defects are not long-tail maintenance
and should be fixed alongside milestone 3.

**Textures leak on every selection change.** `clear_stages()` frees CPU pixel
buffers but discards stage and frame texture names without deleting them.
`clear_selected_image()` clears the uploaded flag without deleting
`g_selectedTexture`, so the next upload overwrites the name with a freshly
generated one. Selection refresh is the most frequent interaction in the window,
so this accumulates during exactly the workflow the module is for. Retained
placeholders for failed animation frames hold no GL name, so they do not add to
this.

`qglDeleteTextures` is present in the QGL dispatch table and is currently called
nowhere in the module — nothing ShaderShop allocates on the GPU is ever
released. An `animMap` stage leaks up to eight names per refresh.

Old CPU image buffers can be released immediately, but old GL texture names must
be retired and deleted while the preview context is current. Do not solve this
by casually making the preview context current inside arbitrary UI callbacks. A
deferred-retirement list consumed at the start of the next render is sufficient;
there is no need for a general resource manager yet.

**Status:** texture retirement is implemented. `qglDeleteTextures` was present
in the dispatch table and called nowhere, so nothing ShaderShop allocated on the
GPU was ever released and every selection change leaked its whole stage stack.
Dropping a texture now pushes its name onto a retirement list consumed at the
top of the next render, where the preview context is guaranteed current.

**The animation timer leaves a stale source id.** The tick callback returns
`FALSE` when the widget is gone without clearing `g_animationTimer`. GLib then
destroys the source while the variable still holds its id, so a later
`g_source_remove` raises a critical warning and `start_animation_timer` declines
to restart because the id looks live. Clear the id wherever the source can end.

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
GtkRadiant build moves to GTK 3. "Retained" means compiled, not merely
`#ifdef`-ed.

Windows GTK 3 enablement is a Radiant build/runtime-packaging project, not a
reason to fork ShaderShop's rendering architecture.

The same preview-shell smoke test remains the minimum platform gate:

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
- **Do not reimplement Radiant's lexer.** `_QERScripLibTable` exists for
  plugins; using a look-alike tokenizer only creates a second language.
- Radiant ScriptLib is stateful and non-reentrant. A ShaderShop parse must be a
  short synchronous operation that owns the parser state until it is finished.
- `GetToken()` recognizes Radiant's actual comment and quote behavior. Do not
  add `/* ... */` block comments or other lexical conveniences unless the
  Radiant consumer being mirrored explicitly does so above ScriptLib.
- `TokenAvailable()` and `GetToken( false )` have historical semantics that are
  not equivalent to a strict line reader. Use `ScriptLine()` plus
  `UnGetToken()` for ShaderShop's line-scoped argument collection.
- The selected shader's outer closing brace is still a semantic boundary.
  Parsing beyond it can incorporate stages belonging to later definitions.
- The selected shader's name is also a semantic boundary. Never let an in-body
  reference to the same string become a definition header.
- Duplicate shader definitions and case handling should follow Radiant's shader
  manager rather than a rule inferred from documentation.
- The manual is documentation, not the parser specification. A manual statement
  can guide feature interpretation but cannot justify a tokenization difference.
- GtkRadiant's `parse.cpp` behavior may look old or permissive in places. That
  is not license for ShaderShop to repair it locally; compatibility is the
  purpose of the dependency.
- GTK may warn that legacy GL 2.1 is below its nominal GTK 3 requirement even
  when it successfully creates the required compatibility context. Treat the
  actual context report as the capability result.
- Apple's OpenGL compatibility implementation may make fixed-function
  processing relatively expensive. Cache parsing and uploads before changing
  the portable rendering contract.
- Image loaders do not necessarily share a row-origin convention. Preserve the
  verified texture-coordinate convention and validate it against multiple image
  formats before introducing per-format flips.
- `animMap` belongs to its stage. A global animation pass destroys stage order
  and stage-local blend semantics.
- `blendFunc` is stage-local. A global Alpha/Additive/Multiply preview selector
  is misleading.
- No `blendFunc` and `blendFunc blend` are not synonymous. The former means no
  blending at all.
- A default-initialised blend state is a silent decision. `PreviewStage`
  defaulting to alpha blending made every dropped or unrecognised `blendFunc`
  invisible rather than diagnosable.
- Unsupported semantic syntax should remain visible and explicit rather than
  being mapped to a plausible-looking fallback.
- A stage that fails to load its image is reported, not skipped — including
  animation frames, which are retained as placeholders because omitting one
  shifts every later frame index.
- The image manager can return a buffer with unusable dimensions as well as no
  buffer at all. Both are failures; treat them alike and keep neither.
- Counting the corpus with regexes is error-prone. Count through Radiant's parser
  and the ShaderShop interpreter instead.
- Source parsing and source rewriting are different milestones. The preview can
  use a small semantic interpreter now; destructive pretty-print serialization
  must wait for the lossless source model.
- Presentation scaffolding must not be reachable by shader arithmetic. A
  background drawn into the destination buffer becomes an operand for every
  destination-reading blend factor, and roughly two in five shipped shaders use
  one.
- A neutral placeholder chosen because it is the identity for one blend mode is
  still a fabricated input. White `$lightmap` is the identity for a filter
  stage and a lie about every lit surface.
- Geometry defined in pixel coordinates puts the origin at a screen corner.
  Anything origin-relative — rotation, sphere-map texgen, an orbit camera —
  then pivots on that corner instead of on the material.
- An orthographic camera gives a flat quad one surface orientation and one eye
  vector. Reflection-based texture generation cannot vary across it, so
  environment mapping needs curvature or perspective before it means anything.
- Validating an enumerated argument and then discarding which value it was is
  worse than not parsing it. `square` accepted and drawn as `sin` is invisible
  to the diagnostics that exist precisely to catch it.
- ScriptLib's cursor is process-global. Freeing the buffer it points into
  leaves shared state dangling for every other consumer in the editor.
- A renamed module leaves its old shared library behind in the install tree.

## Near-term sequence

Compositing isolation comes first. It is not a feature; it is the precondition
for believing any of the features already built. Roughly 42% of the corpus is
currently composited against the preview's own background, so blend, `rgbGen`,
and `alphaGen` work done before this lands is being validated against the wrong
picture.

1. ~~**Isolate compositing.**~~ Done. The checkerboard is used only where it
   provably cannot contribute; every other stack gets a defined uniform light or
   dark field, chosen by how it reads its destination and named in the status
   line. An offscreen composite remains the better general answer if the QGL
   table ever gains framebuffer objects.
2. ~~**Stop fabricating lightmaps.**~~ Done. The stand-in level is a preview
   control defaulting to identity rather than a hidden white constant, applied
   as a draw-time multiplier so it needs no texture rebuild.
3. ~~**Fix the preview coordinate space.**~~ Done. Geometry is defined in
   centred unit material space, the projection owns viewport extents and zoom,
   and the material is held off the eye plane so reflection generators are well
   defined. Orbit and pan now pivot on the material.
4. ~~**Honour waveform type.**~~ Done, and wider than planned. All five
   waveforms the language defines are evaluated to match the engine's generated
   tables: `sin`, `triangle`, `square`, `sawtooth`, `inversesawtooth`. Anything
   else is reported rather than substituted.
5. ~~**Retire GL textures.**~~ Done. Deferred-deletion list consumed at the
   start of the next render, and the animation timer id is cleared wherever the
   source can end.
6. ~~**Make preview time elapsed-time based.**~~ Done. Time is monotonic
   elapsed seconds, paused and resumed with the transport; the timer only asks
   for repaints.
7. **Give `tcMod` a real ordered list and shared evaluator** rather than
   independent flags applied in a fixed renderer sequence.
8. **Make 3D inspection a tessellated material surface** and support
   `deformVertexes wave`; retain the 2D swatch as the compositing reference.
9. **Add `tcMod turb`** to that evaluator, with `runninglava` inspected at an
   oblique angle as the first acceptance case.
10. Generate `$whiteimage` once the Radiant/build consumer confirms the
   documented behaviour.
11. Restore the GTK 2 build by guarding GTK 3-only layout calls.
12. Move native verification fully up to the ScriptLib-backed stage interpreter.

### Implementation order: transform-capable preview

This is the work order for the next preview tranche. Each slice leaves a useful
preview running on the legacy OpenGL path used by GTK 2 and GTK 3; it must not
depend on shaders, framebuffer objects, or a platform-specific GL context.

1. ~~**Introduce the ordered stage model, without changing the picture.**~~
   Done. Every supported `tcMod` is now an operation record in source order,
   retaining its arguments and source location.
2. ~~**Add one coordinate evaluator.**~~ Done for the 2D swatch. Given a
   source UV, elapsed time, and an ordered operation list, it returns the
   transformed UV and supplies it directly to the fixed-function vertices.
   Scroll, scale, rotate, stretch, and transform no longer pass through a
   texture matrix or independent stage fields. Acceptance still needs native
   visual confirmation with deliberately reversed directive pairs: swapping
   `scroll` and `rotate` must change the result in script order.
3. **Introduce a reusable tessellated material mesh for 3D inspection.** Keep
   the 2D view as the low-cost reference view, but draw an evenly subdivided
   grid in inspection mode using the same fixed-function, cross-platform GL
   calls as the existing quad. Evaluate UVs at its vertices and preserve orbit,
   pan, zoom, aspect ratio, stage order, alpha testing, and blend state.
   Acceptance: an affine multi-stage shader looks equivalent in 2D and in a
   front-on 3D view, while an oblique orbit makes the mesh geometry evident.
4. **Parse and render `deformVertexes wave`.** Evaluate the existing waveform
   functions against each grid vertex, recompute a usable normal from nearby
   displaced vertices, and leave unsupported deform forms visibly reported.
   Acceptance: a wave-deformed fixture changes silhouette and lighting/texgen
   response as it animates under orbit; it is not merely a projected UV change.
5. **Add `tcMod turb`.** Implement Quake III's time-dependent turbulent UV
   displacement in the shared evaluator; it is evaluated per mesh vertex, not
   approximated by a texture matrix. Acceptance: `textures/outrage/runninglava`
   ripples and scrolls continuously at an oblique orbit while retaining its
   established composite in 2D.
6. **Broaden only from observed failures.** Next candidates are `tcGen vector`,
   further `deformVertexes` forms, and a curved inspection mesh for environment
   mapping. Each gets a named corpus shader and acceptance observation before it
   is implemented; unsupported directives stay explicit rather than silently
   approximated.
7. **Begin the editor foundation once this representation is stable.** Build an
   in-memory `ShaderDocument` from the selected VFS source, preserve a lossless
   editable copy, and make save an explicit loose-file Save As/replace action.
   That editor model must consume the same ordered stage representation, never
   mutate a packaged or VFS source in place.

Items 1 through 3 were all the same underlying discipline, and all three are now
done: the preview does not let its own scaffolding — background, placeholder, or
coordinate origin — participate in what it claims to be showing. Where a value
must be invented, it is a visible control rather than a constant. That
discipline is what separates a preview from a picture that merely resembles one.

### Waveforms and the preview clock

Time-varying stage state was two faults, and they had to be fixed together
because a waveform is a function of time and the time was wrong.

The clock was a repaint counter divided by a single rate, and that rate was the
maximum any stage demanded. Every stage's timing was therefore a function of its
neighbours: two `animMap` stages at different frequencies beat against each
other, and a waveform's period depended on what else was in the shader. Preview
time is now monotonic elapsed seconds, paused and resumed with the transport,
and the timer only asks for repaints. `animMap` selects
`floor( time * frequency ) mod frameCount` per stage, in real time.

On that base all five waveforms the language defines are evaluated to match the
engine's generated tables rather than a tidier definition — triangle peaks at a
quarter period and square swings between -1 and 1, so a stage's base and
amplitude mean what its author intended. Measured over the shipped corpus plus a
real authored set, `sin` is only 55% of wave directives:

```text
 1037  sin               was correct
  196  square            was accepted, then drawn as sin
  218  inversesawtooth   was rejected, no animation
  154  sawtooth          was rejected
  134  triangle          was rejected
  132  noise / random    not in the language; still reported
```

The 196 `square` directives are the ones that mattered most: accepted,
validated, and then quietly drawn as something else, which is the single
outcome the fault-tolerance policy exists to prevent.

The next work is `tcMod` ordering, which is the last place where a stage's
declared meaning is reinterpreted rather than followed.

The old private-parser corpus work is not discarded: it identified the semantic
boundaries that the ScriptLib-driven interpreter must preserve. What changed is
ownership. ShaderShop no longer proves that it can imitate Radiant's parser; it
uses Radiant's parser.

The larger lossless `ShaderDocument` refactor remains the correct destination,
but it is not the next patch.
