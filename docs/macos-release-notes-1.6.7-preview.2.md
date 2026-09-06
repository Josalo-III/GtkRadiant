# GtkRadiant 1.6.7 macOS arm64 Preview 2

This preview continues the Apple-silicon GTK3/XQuartz restoration begun in
Preview 1. It is a correctness release: the editor's OpenGL views now draw what
they are asked to draw. No new features are included.

## Fixed since Preview 1

- **Views no longer wash out toward white.** The GTK3 port asked GtkGLArea for
  an alpha channel, which it uses to blend each view's output with the widget
  background. GtkGLExt under GLX never did this, so code that legitimately
  ignores framebuffer alpha suddenly leaked the theme background through every
  clear and every blend. The texture browser, which clears with alpha 0, showed
  the theme's white and took its white texture labels with it; textures and
  shader stages ending on an alpha-blended pass were composited toward white
  regardless of the colour they computed. Radiant's GL views are opaque
  viewports and no longer request the alpha channel.
- **Intermittent blank views on startup.** `gtk_gl_area_make_current()` binds
  the GL context but not the widget's framebuffer object. GTK attaches it
  before emitting the render signal, so drawing from inside that signal was
  correct, but realize handlers, timers and refreshes were left drawing into
  framebuffer 0 while the view kept whatever undefined contents it had. With
  several GL areas alive, whether this bit depended on which context happened to
  be current, which is why it appeared at random and cleared on resize.
- **A short colour-defaults table.** `vDefaultColours` supplied fifteen
  initialisers for a sixteen-element array, leaving `COLOR_DETAIL` to
  zero-fill. The resulting value is unchanged, but the omission is no longer
  waiting for the next colour that gets added.

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

- Editor builds, launches and runs under XQuartz on Apple silicon from this
  tree.
- Texture browser renders on its configured background with legible labels,
  where Preview 1 showed white text on a white field.
- A known dark texture that Preview 1 displayed as pale now reads correctly;
  materials ending on an alpha-blended pass no longer wash toward white.

## Not established by this build

- **The intermittent blank view is not confirmed fixed.**
  `gtk_gl_area_attach_buffers()` was called nowhere in the port, which is a
  real defect and produces exactly this class of symptom, but the fault
  appeared at random and has not been reproduced enough times since to call it
  settled. If a view still opens blank, please report it along with any console
  output.
- Preview 1's compiler gates (Q3Map2 VIS/light, BSPC AAS generation) were not
  re-run. Nothing in this release touches the compilers.
- The bundle has not been tested on a clean Mac without the build-time MacPorts
  installation.

Please report packaging and XQuartz failures with the macOS crash report and
`radiant.log` when available.

SHA-256 (`GtkRadiant-1.6.7-preview.2.dmg`):

`248a795a7729a3cda0b68a1cd5d12b0ad5b6354eb29ac02d3332c18cc63651d4`
