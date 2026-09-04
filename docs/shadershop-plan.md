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
- use `UnGetToken` only as the single-token pushback mechanism it actually is.

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

On GTK 3, request the first render after realization and again from an idle
callback after mapping. A pre-realize render request can be discarded. Any idle
callback must consult the current live preview widget rather than retain a
destroyed widget pointer.

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

Two consequences remain important:

- **A failed animation frame is retained as an empty placeholder.** Dropping it
  would renumber every later frame, changing animation semantics after parsing.
- **A default-initialised semantic value is a decision.** If ShaderShop cannot
  resolve a legal Radiant token sequence into a preview state, it should report
  that state as unsupported rather than silently substitute a plausible one.

The stage summary must also stop collapsing unrelated failures into
`Parsed stages: 0 (shader API/file unavailable)`. Source acquisition and parsing
should distinguish at least:

```text
no current shader
shader lookup unavailable
shader object not found
shader has no source filename
VFS load failed
selected definition not found in loaded source
selected definition parsed with zero stages
```

That distinction is especially important while the current zero-stage problem
is being diagnosed.

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
    later: tcGen / ordered tcMod
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

**Status:** complete for the current preview. ShaderShop requests ScriptLib and
uses it to parse the VFS-loaded selected definition, preserving stage order,
`map`/`clampmap`, `animMap` frame lists, and stage-local blend state. The
shader API requirement is a normal Synapse wildcard requirement; registering
`SYN_REQUIRE_ANY` directly leaves the table unpopulated and produces the
misleading zero-stage state.

The previous native parity result — 259,236 identical tokens across 268 files —
is evidence that the old tokenizer learned the right lessons, not a permanent
architecture. The milestone is reopened until that tokenizer is replaced by
Radiant's exported parser.

Still required before this milestone is complete:

- add `_QERScripLibTable` to ShaderShop and request `SCRIPLIB_MAJOR` through
  Synapse;
- convert the selected-shader/stage interpreter to consume ScriptLib tokens;
- replace raw line scanning with `ScriptLine()` / `UnGetToken()` handling;
- remove private lexical helpers and tokenizer-extraction verification;
- retain `map` versus `clampmap` as distinct map kinds rather than collapsing
  them to one filename;
- recognise special map tokens explicitly;
- retain unsupported directives instead of dropping them;
- split the zero-stage status into the concrete source/lookup/parse states
  listed in the fault-tolerance section.

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
order-dependent. Note that the corpus spells the keyword `tcmod` 1,800 times
against `tcMod` 4,004, so case folding is a prerequisite here too.

When `rgbGen` arrives, its default is not constant: section 6.3 selects
`identityLighting` for additive and blended stages and `identity` for filter
stages. The default therefore depends on the stage's resolved blend mode, which
is a further reason blend correctness comes first.

**Acceptance:** known static shaders using additive, filter, alpha, and
unblended stages display in the expected source order. Known animated shaders
advance, pause, and resume deterministically. Two animated stages with different
frequencies remain independent. Re-measuring the corpus shows no `blendFunc`
directive resolving to an unintended factor.

**Status:** ordered stage compositing and stage-owned animation are working in
the shipped corpus. Explicit blend-factor case folding plus `add`, `filter`,
and `blend` shorthand are implemented. `$lightmap` uses neutral generated
white in the bare preview, while `clampmap` uses clamp-to-edge so animated
jumppad rings do not tile into the frame. `rgbGen wave`, `tcMod stretch`, and
`tcMod scroll` provide the first time-based jumppad and sky effects. Remaining
parity work includes exact square-wave evaluation, ordered general `tcMod`, alpha functions,
generated sources, and a real surface lightmap path.

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

Old CPU image buffers can be released immediately, but old GL texture names must
be retired and deleted while the preview context is current. Do not solve this
by casually making the preview context current inside arbitrary UI callbacks. A
deferred-retirement list consumed at the start of the next render is sufficient;
there is no need for a general resource manager yet.

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
- A renamed module leaves its old shared library behind in the install tree.

## Near-term sequence

Keep the next work narrow. Parser deference and basic stage playback are now
established; extend renderer semantics without weakening that boundary.

1. Make preview time truly elapsed-time based, preserving independent stage
   frequencies rather than deriving time from a shared tick rate.
2. Complete wave semantics, including a true square waveform and alpha waves.
3. Retain and apply ordered `tcMod` operations: scroll, scale, rotate, then
   stretch alongside one another.
4. Add `alphaFunc` and the remaining blend-factor families, reporting unknown
   values rather than selecting a plausible fallback.
5. Generate `$whiteimage`; retain the neutral `$lightmap` fallback until a
   model/BSP preview supplies genuine baked data.
6. Add deferred deletion of superseded preview GL textures and restore the GTK
   2 build by guarding GTK 3-only layout calls.
7. Move native verification fully up to the ScriptLib-backed stage interpreter.

The old private-parser corpus work is not discarded: it identified the semantic
boundaries that the ScriptLib-driven interpreter must preserve. What changes is
ownership. ShaderShop no longer proves that it can imitate Radiant's parser; it
uses Radiant's parser.

The larger lossless `ShaderDocument` refactor remains the correct destination,
but it is not the next patch.
