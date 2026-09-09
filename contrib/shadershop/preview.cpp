/*
   ShaderShop plugin for GtkRadiant

   The preview currently parses the selected shader's stage stack directly from
   its shader file. Each stage owns its map, blend and animation state so stage
   ordering remains explicit as preview support grows.
 */

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

#include "gtkutil.h"
#include "shadershop.h"

static GtkWidget* g_pPreviewWindow = NULL;
static GtkWidget* g_pPreviewWidget = NULL;
static GtkWidget* g_pSelectionLabel = NULL;
static GtkWidget* g_stageLabel = NULL;
static GtkWidget* g_pEditorWindow = NULL;
static GtkWidget* g_editorStageStack = NULL;
static GtkWidget* g_editorStatusLabel = NULL;
static bool g_editorRebuilding = false;
static bool g_editorDirty = false;
static GtkWidget* g_editorDropMarker = NULL;
static int g_editorDragIndex = -1;
static int g_editorDropIndex = -1;
static bool g_editorDropAfter = false;

static void rebuild_editor_stage_stack();
static GtkWidget* g_animationButton = NULL;
static GtkWidget* g_3dInspectButton = NULL;
static GtkWidget* g_backdropButton = NULL;
static GtkWidget* g_lightmapScale = NULL;

// The normal view remains an exact 2D material swatch.  Inspection mode uses
// the same renderer under an orbitable 3D camera so vertex effects can be
// judged from more than the one front-on view.
static bool g_3dInspect = false;
static float g_inspectYaw = 28.0f;
static float g_inspectPitch = -24.0f;
static float g_inspectPanX = 0.0f;
static float g_inspectPanY = 0.0f;
static float g_inspectZoom = 1.0f;
static double g_pointerX = 0.0;
static double g_pointerY = 0.0;

// Preview geometry is defined in centred material space: the quad is centred on
// the origin and its longer side is one unit.  Viewport extents belong to the
// projection, not to the vertices, so anything orientation- or origin-relative
// (orbit rotation, reflection texgen) pivots on the material rather than on a
// corner of the window.
//
// The material is also held away from the eye plane.  A surface sitting at eye
// z = 0 makes the eye vector lie in the surface plane, which degenerates
// reflection-based texture generation; a fixed offset keeps it well defined in
// both projections.
static const float PREVIEW_EYE_DISTANCE = 2.0f;
static const float PREVIEW_FILL = 0.78f;

// Backdrop for a stack that treats the destination as data.  Uniform is the
// point: what corrupts such a material is the checkerboard's pattern, not its
// brightness.  Two fields are needed because the two ways of reading the
// destination want opposite ones — a multiplicative stage over black yields
// black, and an additive stage over a light field saturates to white.
//
// The light field is not a separate constant: a stage that multiplies its
// destination is asking what light falls on the surface, which is the same
// question `map $lightmap` asks.  Both are driven by the one lightmap level so
// there is a single control for "how lit is this", rather than a slider that
// appears to do nothing because a fixed field is standing in front of it.
static const float PREVIEW_BACKDROP_DARK = 0.06f;

enum PreviewBackdrop
{
	BACKDROP_CHECKER,
	BACKDROP_LIGHTMAP,
	BACKDROP_DARK,
	BACKDROP_IMAGE
};

// What the stack is composited over.  AUTO derives it from how the stack reads
// its destination; the rest are the user's explicit choice.  A shader authored
// as an overlay — a modulator laid over other geometry — can only be judged
// against the thing it modulates, so an image backdrop is a preview-level
// control rather than an editor feature.
enum BackdropMode
{
	BACKDROP_MODE_AUTO,
	BACKDROP_MODE_CHECKER,
	BACKDROP_MODE_LIGHTMAP,
	BACKDROP_MODE_DARK,
	BACKDROP_MODE_IMAGE
};

static BackdropMode g_backdropMode = BACKDROP_MODE_AUTO;


// How the selected stack uses its destination.  Set once per selection.
static PreviewBackdrop g_previewBackdrop = BACKDROP_CHECKER;

// Stand-in lightmap level for `map $lightmap`, which has no meaning outside a
// compiled map.  It is a fabricated input, so it is a visible control rather
// than a constant hidden in the renderer.
//
// The default is identity, which is what reproduces the engine.  A `filter`
// lightmap stage at 1.0 is a no-op, so the material shows its own colours --
// the right neutral for a material swatch.  It also matches observation: Quake
// doubles the lightmap contribution through overbright bits, so a normally lit
// surface arrives on screen at roughly identity, and comparing a preview
// against the game only agrees here.  Lowering the control is how a shadowed
// condition is inspected.
static const float PREVIEW_LIGHTMAP_DEFAULT = 1.0f;
static float g_lightmapLevel = PREVIEW_LIGHTMAP_DEFAULT;

// View half-extents from the last render, in world units.  Pointer panning
// converts pixels to world units through these.
static float g_viewHalfX = 1.0f;
static float g_viewHalfY = 1.0f;

// Dimensions of the GL area's own framebuffer, reported by GtkGLArea's "resize"
// signal.  The widget allocation is not the same thing: it is unset until the
// first size-allocate and does not account for the window scale factor, so
// sizing the viewport from it means the first frame is drawn against a size the
// buffer does not have -- or, when the allocation is still zero, not drawn at
// all.
static int g_previewBufferWidth = 0;
static int g_previewBufferHeight = 0;

// Bounded retries for the case where a render arrives before the surface is
// usable. Re-queueing unconditionally would spin if the surface never became
// drawable, and a render loop that burns a core is worse than a blank frame.
static const int PREVIEW_SIZE_RETRY_LIMIT = 8;
static int g_previewSizeRetries = 0;

// Every first-frame recovery path we have -- realize, map, configure, resize,
// window creation -- ends in gtk_gl_area_queue_render(), and none of them can
// tell whether the frame it asked for was ever delivered. When GTK drops the
// request (the toplevel is not yet mapped, or the area has no allocation), a
// static shader has no animation timer to ask again, so the area keeps its
// undefined contents until unrelated damage -- a menu bar hover -- forces a
// repaint. That is the white window on first load.
//
// So watch for the frame instead of assuming it. The watchdog re-queues on a
// timer until draw_preview() reports a frame that reached a usable surface,
// then stops. The budget bounds it: if the surface is never drawable, this
// gives up rather than spinning.
static const guint PREVIEW_FIRST_FRAME_INTERVAL_MS = 100;
static const int PREVIEW_FIRST_FRAME_LIMIT = 30;
static bool g_previewFrameDrawn = false;
static int g_previewFrameWaits = 0;
static guint g_previewFrameTimer = 0;

// GL names of superseded textures. Deletion needs the preview context current,
// which is only guaranteed inside the render callback, so retirement is
// deferred to the start of the next frame rather than done from whichever UI
// callback dropped the texture.
static std::vector<GLuint> g_retiredTextures;

static void retire_texture( GLuint& texture ){
	if ( texture != 0 ) {
		g_retiredTextures.push_back( texture );
		texture = 0;
	}
}

static void delete_retired_textures(){
	if ( g_retiredTextures.empty() ) {
		return;
	}
	g_QglTable.m_pfn_qglDeleteTextures( static_cast<GLsizei>( g_retiredTextures.size() ), &g_retiredTextures[0] );
	g_retiredTextures.clear();
}

static GLuint g_checkerboardTexture = 0;
static GLuint g_selectedTexture = 0;
static unsigned char* g_selectedPixels = NULL;
static int g_selectedWidth = 0;
static int g_selectedHeight = 0;
static bool g_selectedTextureUploaded = false;

struct AnimationFrame
{
	std::string name;
	unsigned char* pixels;
	int width;
	int height;
	GLuint texture;
	bool uploaded;

	AnimationFrame() : pixels( NULL ), width( 0 ), height( 0 ), texture( 0 ), uploaded( false ) {}
};

static bool keyword_equals( const std::string& token, const char* keyword );

// The five waveforms the shader language defines (manual 2.4.8). Evaluated to
// match the engine's generated tables rather than to a tidier definition: the
// triangle peaks at a quarter period and square swings between -1 and 1, so a
// stage's base and amplitude mean what its author intended.
enum WaveForm
{
	WAVE_SIN,
	WAVE_TRIANGLE,
	WAVE_SQUARE,
	WAVE_SAWTOOTH,
	WAVE_INVERSE_SAWTOOTH
};

static float wave_value( WaveForm form, float t ){
	t -= floorf( t );
	switch ( form ) {
	case WAVE_TRIANGLE:
		if ( t < 0.25f ) return 4.0f * t;
		if ( t < 0.75f ) return 2.0f - 4.0f * t;
		return ( t - 0.75f ) * 4.0f - 1.0f;
	case WAVE_SQUARE:           return t < 0.5f ? 1.0f : -1.0f;
	case WAVE_SAWTOOTH:         return t;
	case WAVE_INVERSE_SAWTOOTH: return 1.0f - t;
	case WAVE_SIN:
		break;
	}
	return sinf( 6.28318530718f * t );
}

static bool parse_wave_form( const std::string& token, WaveForm& form ){
	if ( keyword_equals( token, "sin" ) )             { form = WAVE_SIN;              return true; }
	if ( keyword_equals( token, "triangle" ) )        { form = WAVE_TRIANGLE;         return true; }
	if ( keyword_equals( token, "square" ) )          { form = WAVE_SQUARE;           return true; }
	if ( keyword_equals( token, "sawtooth" ) )        { form = WAVE_SAWTOOTH;         return true; }
	if ( keyword_equals( token, "inversesawtooth" ) ) { form = WAVE_INVERSE_SAWTOOTH; return true; }
	return false;
}

// The engine stores a fixed number of animation frames per stage.
static const size_t ANIMATION_FRAME_LIMIT = 8;

// Keep tcMod directives as the shader wrote them. This ordered representation
// is the source of truth for the fixed-function swatch today and the later
// per-vertex evaluator.
enum TcModKind
{
	TCMOD_SCROLL,
	TCMOD_ROTATE,
	TCMOD_SCALE,
	TCMOD_TRANSFORM,
	TCMOD_STRETCH,
	TCMOD_TURB
};

struct TcModOperation
{
	TcModKind kind;
	WaveForm waveForm;
	float values[6];
	int sourceLine;

	TcModOperation( TcModKind operationKind, int line ) : kind( operationKind ), waveForm( WAVE_SIN ), sourceLine( line ){
		for ( int i = 0; i < 6; ++i ) values[i] = 0.0f;
	}
};

struct PreviewStage
{
	std::string mapName;
	std::string blendSrc;
	std::string blendDst;
	bool clamp;
	bool generatedLightmap;
	unsigned char* pixels;
	int width;
	int height;
	GLuint texture;
	bool uploaded;
	float animationFps;
	bool rgbWave;
	WaveForm rgbWaveForm;
	bool environmentTexGen;
	GLenum alphaFunction;
	float alphaReference;
	float alpha;
	float rgbBase, rgbAmplitude, rgbPhase, rgbFrequency;
	std::vector<TcModOperation> tcModOperations;
	std::vector<std::string> animationNames;
	std::vector<AnimationFrame> animationFrames;

	PreviewStage() :
		// A stage without blendFunc is an opaque replace in the Quake renderer.
		blendSrc( "GL_ONE" ),
		blendDst( "GL_ZERO" ),
		clamp( false ),
		generatedLightmap( false ),
		pixels( NULL ),
		width( 0 ),
		height( 0 ),
		texture( 0 ),
		uploaded( false ),
		animationFps( 0.0f ), rgbWave( false ), rgbWaveForm( WAVE_SIN ), environmentTexGen( false ), alphaFunction( GL_ALWAYS ), alphaReference( 0.0f ), alpha( 1.0f ),
		rgbBase( 1.0f ), rgbAmplitude( 0.0f ), rgbPhase( 0.0f ), rgbFrequency( 0.0f )
	{}
};

static bool stage_has_animated_tcmod( const PreviewStage& stage ){
	for ( std::vector<TcModOperation>::const_iterator operation = stage.tcModOperations.begin(); operation != stage.tcModOperations.end(); ++operation ) {
		if ( operation->kind == TCMOD_SCROLL || operation->kind == TCMOD_ROTATE || operation->kind == TCMOD_STRETCH ||
			 ( operation->kind == TCMOD_TURB && operation->values[1] != 0.0f && operation->values[3] != 0.0f ) ) return true;
	}
	return false;
}

static bool stage_requires_tessellated_preview( const PreviewStage& stage ){
	for ( std::vector<TcModOperation>::const_iterator operation = stage.tcModOperations.begin(); operation != stage.tcModOperations.end(); ++operation ) {
		if ( operation->kind == TCMOD_TURB ) return true;
	}
	return false;
}

static std::vector<PreviewStage> g_stages;

// deformVertexes belongs to the material rather than to an individual stage.
// The preview keeps its wave declarations separately so every rendered stage
// shares the same displaced inspection surface.
struct PreviewDeformWave
{
	float spread;
	WaveForm waveForm;
	float base;
	float amplitude;
	float phase;
	float frequency;
	int sourceLine;

	PreviewDeformWave() : spread( 0.0f ), waveForm( WAVE_SIN ), base( 0.0f ), amplitude( 0.0f ), phase( 0.0f ), frequency( 0.0f ), sourceLine( 0 ){}
};

static std::vector<PreviewDeformWave> g_deformWaves;

struct PreviewDeformMove
{
	float direction[3];
	WaveForm waveForm;
	float base;
	float amplitude;
	float phase;
	float frequency;
	int sourceLine;

	PreviewDeformMove() : waveForm( WAVE_SIN ), base( 0.0f ), amplitude( 0.0f ), phase( 0.0f ), frequency( 0.0f ), sourceLine( 0 ){
		direction[0] = direction[1] = direction[2] = 0.0f;
	}
};

struct PreviewDeformNormal
{
	float amplitude;
	float frequency;
	int sourceLine;

	PreviewDeformNormal() : amplitude( 0.0f ), frequency( 0.0f ), sourceLine( 0 ){}
};

struct PreviewDeformBulge
{
	float width;
	float height;
	float speed;
	int sourceLine;

	PreviewDeformBulge() : width( 0.0f ), height( 0.0f ), speed( 0.0f ), sourceLine( 0 ){}
};

static std::vector<PreviewDeformMove> g_deformMoves;
static std::vector<PreviewDeformNormal> g_deformNormals;
static std::vector<PreviewDeformBulge> g_deformBulges;

// A backdrop may be a plain image or a whole shader. Most of the interesting
// ones are shaders: an overlay is authored against another material, and that
// material is rarely a bare .tga.
static std::vector<PreviewStage> g_backdropStages;
static std::string g_backdropName;
static std::string g_editorImageName;

// A selected texture name and a shader definition are separate things in the
// Quake 3 asset model.  Keep that distinction through parsing so the UI does
// not describe a perfectly usable raw image as a failed shader parse.
enum ShaderSourceState
{
	SHADER_SOURCE_RAW_IMAGE,
	SHADER_SOURCE_PARSED,
	SHADER_SOURCE_UNAVAILABLE,
	SHADER_SOURCE_MALFORMED
};

static ShaderSourceState g_shaderSourceState = SHADER_SOURCE_RAW_IMAGE;

// Parse and load diagnostics for the current selection. Malformed or
// unsupported source is tolerated rather than fatal, but it is never silent:
// unreported tolerance is indistinguishable from a preview that is simply
// wrong.
static std::vector<std::string> g_diagnostics;

static const size_t DIAGNOSTIC_PRINT_LIMIT = 20;

static void shadershop_warn( const char* format, ... ){
	char text[512];
	va_list args;
	va_start( args, format );
	vsnprintf( text, sizeof( text ), format, args );
	va_end( args );

	g_diagnostics.push_back( text );
	if ( g_diagnostics.size() <= DIAGNOSTIC_PRINT_LIMIT ) {
		g_FuncTable.m_pfnSysFPrintf( SYS_WRN, "ShaderShop: %s\n", text );
	}
	else if ( g_diagnostics.size() == DIAGNOSTIC_PRINT_LIMIT + 1 ) {
		g_FuncTable.m_pfnSysFPrintf( SYS_WRN, "ShaderShop: further diagnostics suppressed\n" );
	}
}

// Preview time is real elapsed time, not a count of repaints. A tick counter
// divided by a shared clock rate made every stage's timing a function of
// whatever other stage happened to demand the fastest rate: two animMap stages
// at different frequencies beat against each other, and a waveform's period
// depended on its neighbours. Stages are independent, so their time must be.
//
// The timer's only job is now to ask for repaints; it is not the time source.
static const guint PREVIEW_REPAINT_INTERVAL_MS = 16;

static guint g_animationTimer = 0;
static bool g_animationActive = false;
static bool g_animationPaused = false;
static gint64 g_animationResumed = 0;   // monotonic microseconds, 0 when paused
static double g_animationElapsed = 0.0; // seconds accumulated before this run

static double preview_seconds(){
	double seconds = g_animationElapsed;
	if ( g_animationResumed != 0 ) {
		seconds += static_cast<double>( g_get_monotonic_time() - g_animationResumed ) / 1000000.0;
	}
	return seconds;
}

static void preview_clock_reset(){
	g_animationElapsed = 0.0;
	// Start only after the selected material has reached the GL context.  Asset
	// decoding and the first texture uploads are not animation time; counting
	// them made fast tcMod rotate materials visibly begin at a late phase.
	g_animationResumed = 0;
}

static void preview_clock_pause(){
	if ( g_animationResumed != 0 ) {
		g_animationElapsed += static_cast<double>( g_get_monotonic_time() - g_animationResumed ) / 1000000.0;
		g_animationResumed = 0;
	}
}

static void preview_clock_resume(){
	if ( g_animationResumed == 0 ) {
		g_animationResumed = g_get_monotonic_time();
	}
}

static void clear_stage_list( std::vector<PreviewStage>& stages ){
	for ( std::vector<PreviewStage>::iterator stage = stages.begin(); stage != stages.end(); ++stage ) {
		if ( stage->pixels != NULL ) {
			g_free( stage->pixels );
			stage->pixels = NULL;
		}
		retire_texture( stage->texture );
		stage->uploaded = false;
		for ( std::vector<AnimationFrame>::iterator frame = stage->animationFrames.begin(); frame != stage->animationFrames.end(); ++frame ) {
			if ( frame->pixels != NULL ) {
				g_free( frame->pixels );
				frame->pixels = NULL;
			}
			retire_texture( frame->texture );
			frame->uploaded = false;
		}
		stage->animationFrames.clear();
	}
	stages.clear();
}

static void clear_stage_images( PreviewStage& stage ){
	if ( stage.pixels != NULL ) {
		g_free( stage.pixels );
		stage.pixels = NULL;
	}
	retire_texture( stage.texture );
	stage.uploaded = false;
	for ( std::vector<AnimationFrame>::iterator frame = stage.animationFrames.begin(); frame != stage.animationFrames.end(); ++frame ) {
		if ( frame->pixels != NULL ) {
			g_free( frame->pixels );
			frame->pixels = NULL;
		}
		retire_texture( frame->texture );
		frame->uploaded = false;
	}
	stage.animationFrames.clear();
}

static void clear_stages(){
	clear_stage_list( g_stages );
	g_deformWaves.clear();
	g_deformMoves.clear();
	g_deformNormals.clear();
	g_deformBulges.clear();
	g_animationActive = false;
}

// The image manager reports failure by leaving the pixel pointer null, but a
// loader can also hand back a buffer with no usable dimensions. Treat both as
// failure, and never keep a buffer we would not draw.
static bool load_image( const char* name, unsigned char** pixels, int* width, int* height ){
	*pixels = NULL;
	*width = 0;
	*height = 0;

	g_FuncTable.m_pfnLoadImage( name, pixels, width, height );

	// Shader files commonly name a .tga while a mod ships the same asset as a
	// different supported format. With an explicit extension Radiant tries only
	// that loader; retrying the extensionless VFS path lets its image manager
	// search the installed formats without bypassing the VFS.
	if ( *pixels == NULL ) {
		std::string extensionless( name );
		const std::string::size_type dot = extensionless.find_last_of( '.' );
		const std::string::size_type slash = extensionless.find_last_of( "/\\" );
		if ( dot != std::string::npos && ( slash == std::string::npos || dot > slash ) ) {
			extensionless.erase( dot );
			g_FuncTable.m_pfnLoadImage( extensionless.c_str(), pixels, width, height );
		}
	}

	if ( *pixels != NULL && ( *width <= 0 || *height <= 0 ) ) {
		g_free( *pixels );
		*pixels = NULL;
	}
	if ( *pixels == NULL ) {
		*width = 0;
		*height = 0;
		return false;
	}
	return true;
}

// =============================================================================
// IMAGE SOURCE LOADER
//
// A named image resolved through Radiant's VFS and image manager, owning both
// its CPU buffer and its preview texture.  The backdrop uses this now; stage
// sources and the shader document loader are the reason it is a named seam
// rather than three more globals.

struct PreviewImageSource
{
	std::string name;
	unsigned char* pixels;
	int width;
	int height;
	GLuint texture;
	bool uploaded;

	PreviewImageSource() : pixels( NULL ), width( 0 ), height( 0 ), texture( 0 ), uploaded( false ) {}
};

static void preview_source_clear( PreviewImageSource& source ){
	if ( source.pixels != NULL ) {
		g_free( source.pixels );
		source.pixels = NULL;
	}
	source.name.clear();
	source.width = 0;
	source.height = 0;
	source.uploaded = false;
	retire_texture( source.texture );
}

static bool preview_source_load( PreviewImageSource& source, const char* name ){
	preview_source_clear( source );
	if ( name == NULL || name[0] == '\0' ) {
		return false;
	}
	source.name = name;
	return load_image( name, &source.pixels, &source.width, &source.height );
}

// Radiant's image manager resolves VFS-relative names. A chooser hands back an
// absolute path, so trim the game path when the file lives under it and report
// plainly when it does not rather than silently failing to load.
static bool preview_source_relative_name( const char* absolute, std::string& relative ){
	if ( absolute == NULL || g_FuncTable.m_pfnGetGamePath == NULL ) {
		return false;
	}
	const char* base = g_FuncTable.m_pfnGetGamePath();
	if ( base != NULL && base[0] != '\0' ) {
		const size_t length = strlen( base );
		if ( !strncmp( absolute, base, length ) ) {
			const char* tail = absolute + length;
			while ( *tail == '/' ) {
				++tail;
			}
			// Skip the mod directory (baseq3, missionpack, ...) that the VFS
			// itself supplies.
			const char* slash = strchr( tail, '/' );
			if ( slash != NULL ) {
				relative = slash + 1;
				return true;
			}
		}
	}
	return false;
}

static PreviewImageSource g_backdropImage;

static GLenum blend_factor( const std::string& factor );

// The checkerboard exists to reveal transparency, which is only a meaningful
// reading when the destination acts as background behind a translucent surface.
// A stage whose destination contribution is anything else is using the
// destination as data, and a patterned backdrop would then be multiplied,
// inverted or added into the material itself.
//
// Conventional transparency (dst scaled by GL_ONE_MINUS_SRC_ALPHA) and opaque
// replacement (dst scaled by GL_ZERO) are the two safe cases.
static bool stage_multiplies_destination( const PreviewStage& stage ){
	const GLenum src = blend_factor( stage.blendSrc );
	const GLenum dst = blend_factor( stage.blendDst );
	if ( src == GL_DST_COLOR || src == GL_ONE_MINUS_DST_COLOR ||
		 src == GL_DST_ALPHA || src == GL_ONE_MINUS_DST_ALPHA ) {
		return true;
	}
	return dst == GL_SRC_COLOR || dst == GL_ONE_MINUS_SRC_COLOR;
}

// The usual lightmap filter is neutral over white. Other destination-reading
// blends are more useful over the checkerboard by default: it is not a scene
// composite, but it keeps translucent water visible until a real backdrop is
// selected.
static bool stage_uses_lightmap_filter( const PreviewStage& stage ){
	return blend_factor( stage.blendSrc ) == GL_DST_COLOR && blend_factor( stage.blendDst ) == GL_ZERO;
}

static bool stage_adds_destination( const PreviewStage& stage ){
	const GLenum dst = blend_factor( stage.blendDst );
	return dst != GL_ZERO && dst != GL_ONE_MINUS_SRC_ALPHA;
}

// A generated lightmap placeholder is not drawn unless it is the ordinary
// filter stage, so it must not decide the backdrop either.
static bool stage_is_drawn( const PreviewStage& stage ){
	if ( !stage.generatedLightmap ) {
		return true;
	}
	return blend_factor( stage.blendSrc ) == GL_DST_COLOR && blend_factor( stage.blendDst ) == GL_ZERO;
}

static bool stage_dimensions( const PreviewStage& stage, int& width, int& height );

// A per-stage account of what the renderer resolved and what it will draw.
// Printed at standard level rather than as diagnostics: these are not problems,
// they are the answer to "why does it look like that", which is otherwise only
// recoverable by reading the source alongside the blend table.
static void report_stage_resolution( const char* shaderName ){
	if ( g_FuncTable.m_pfnSysFPrintf == NULL || g_stages.empty() ) {
		return;
	}

	g_FuncTable.m_pfnSysFPrintf( SYS_STD, "ShaderShop: %s resolved to %d stage(s)\n",
		shaderName, static_cast<int>( g_stages.size() ) );

	int number = 0;
	for ( std::vector<PreviewStage>::const_iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		++number;

		const char* source = stage->generatedLightmap ? "$lightmap (generated white)"
			: ( !stage->animationNames.empty() ? "animMap" : stage->mapName.c_str() );

		int width = 0;
		int height = 0;
		const bool haveImage = stage_dimensions( *stage, width, height ) || stage->generatedLightmap;

		g_FuncTable.m_pfnSysFPrintf(
			SYS_STD,
			"ShaderShop:   %d: %s%s  blend %s %s  %s%s%s%s\n",
			number,
			( source != NULL && source[0] != '\0' ) ? source : "(no image source)",
			haveImage ? "" : "  [IMAGE NOT LOADED]",
			stage->blendSrc.c_str(),
			stage->blendDst.c_str(),
			stage_is_drawn( *stage ) ? "drawn" : "skipped",
			stage->clamp ? " clamp" : "",
			stage->environmentTexGen ? " tcGen-env" : "",
			stage->alphaFunction != GL_ALWAYS ? " alphaFunc" : ""
		);
	}
}

static void classify_destination_use(){
	// Only the FIRST drawn stage can see the backdrop. Every later stage blends
	// against what the stages before it wrote, so its factors say nothing about
	// what the stack needs to stand on. Judging the whole stack was wrong and
	// visibly so: a trailing `$lightmap` filter stage — itself a preview
	// fabrication — forced a uniform light field under materials whose first
	// stage was an ordinary opaque or alpha-blended texture, so a dark material
	// read as a pale one.
	bool multiplies = false;
	bool lightmapFilter = false;
	bool adds = false;
	for ( std::vector<PreviewStage>::const_iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		if ( !stage_is_drawn( *stage ) ) {
			continue;
		}
		multiplies = stage_multiplies_destination( *stage );
		lightmapFilter = multiplies && stage_uses_lightmap_filter( *stage );
		adds = !multiplies && stage_adds_destination( *stage );
		break;
	}

	// An explicit choice is honoured as given; only AUTO derives the backdrop
	// from the stack. A user inspecting a shader over a chosen image has taken
	// responsibility for what the destination means.
	switch ( g_backdropMode ) {
	case BACKDROP_MODE_CHECKER: g_previewBackdrop = BACKDROP_CHECKER; return;
	case BACKDROP_MODE_LIGHTMAP: g_previewBackdrop = BACKDROP_LIGHTMAP; return;
	case BACKDROP_MODE_DARK:    g_previewBackdrop = BACKDROP_DARK;    return;
	case BACKDROP_MODE_IMAGE:
		g_previewBackdrop = BACKDROP_IMAGE;
		if ( g_backdropImage.pixels == NULL && g_backdropStages.empty() ) {
			shadershop_warn( "no backdrop loaded; showing the checkerboard instead" );
			g_previewBackdrop = BACKDROP_CHECKER;
		}
		return;
	case BACKDROP_MODE_AUTO:
		break;
	}

	// The filter lightmap needs white to remain neutral. Other destination-
	// reading stages, especially water, open over the checkerboard for a useful
	// default inspection view; users can select a scene-style backdrop when the
	// exact destination composite matters.
	if ( multiplies ) {
		if ( lightmapFilter ) {
			g_previewBackdrop = BACKDROP_LIGHTMAP;
			shadershop_warn( "stack multiplies its destination; shown over a uniform lightmap backdrop" );
		}
		else {
			g_previewBackdrop = BACKDROP_CHECKER;
			shadershop_warn( "stack multiplies its destination; shown over the checkerboard for visibility (select a backdrop for scene-composite fidelity)" );
		}
	}
	else if ( adds ) {
		g_previewBackdrop = BACKDROP_DARK;
	}
	else {
		g_previewBackdrop = BACKDROP_CHECKER;
		return;
	}

	if ( adds ) {
		shadershop_warn( "stack adds to its destination; shown over a uniform dark backdrop" );
	}
}


// `updateClock` is false for a backdrop stack: it may animate, but it must not
// redefine the preview clock the selected shader is being judged against.
static void load_stage_images( std::vector<PreviewStage>& stages, bool updateClock ){
	int stageNumber = 0;
	for ( std::vector<PreviewStage>::iterator stage = stages.begin(); stage != stages.end(); ++stage ) {
		++stageNumber;

		if ( !stage->animationNames.empty() ) {
			// A frame that fails to load is retained as an empty placeholder.
			// Dropping it would renumber every later frame and silently change
			// both the length and the content of the animation.
			size_t loaded = 0;
			for ( std::vector<std::string>::const_iterator name = stage->animationNames.begin(); name != stage->animationNames.end(); ++name ) {
				AnimationFrame frame;
				frame.name = *name;
				if ( load_image( name->c_str(), &frame.pixels, &frame.width, &frame.height ) ) {
					++loaded;
				}
				else {
					shadershop_warn(
						"stage %d: animation frame %d ('%s') could not be loaded",
						stageNumber, static_cast<int>( stage->animationFrames.size() ) + 1, name->c_str()
					);
				}
				stage->animationFrames.push_back( frame );
			}

			if ( loaded == 0 ) {
				shadershop_warn( "stage %d: no animation frame could be loaded", stageNumber );
			}
			if ( updateClock && stage->animationFrames.size() > 1 && stage->animationFps > 0.0f ) {
				g_animationActive = true;
			}
		}
		else if ( !stage->mapName.empty() ) {
			if ( !load_image( stage->mapName.c_str(), &stage->pixels, &stage->width, &stage->height ) ) {
				// Special sources are understood but not yet representable, and
				// are reported as such rather than as a missing file.
				if ( !strcasecmp( stage->mapName.c_str(), "$lightmap" ) ) {
					// A real lightmap is baked per surface and unavailable on this
					// standalone quad. White is the neutral input for the common
					// filter lightmap stage, preserving the material's base image.
					stage->pixels = static_cast<unsigned char*>( g_malloc( 4 ) );
					stage->pixels[0] = stage->pixels[1] = stage->pixels[2] = stage->pixels[3] = 255;
					stage->width = stage->height = 1;
					stage->generatedLightmap = true;
				}
				else if ( stage->mapName[0] == '$' ) {
					shadershop_warn(
						"stage %d: '%s' is not previewable in a bare material context",
						stageNumber, stage->mapName.c_str()
					);
				}
				else {
					shadershop_warn( "stage %d: image '%s' could not be loaded", stageNumber, stage->mapName.c_str() );
				}
			}
		}
		else {
			shadershop_warn( "stage %d: no image source", stageNumber );
		}
		if ( updateClock && ( stage->rgbWave || stage_has_animated_tcmod( *stage ) ) ) {
			g_animationActive = true;
		}
	}
}

// =============================================================================
// SCRIPT LIB TOKEN ADAPTATION
//
// Lexing is deliberately delegated to Radiant.  ScriptLib owns comments,
// quotes, delimiters and line accounting; ShaderShop only groups its tokens
// into the selected definition and its stages.

static bool next_script_token( std::string& result ){
	if ( !g_ScripLibTable.m_pfnGetToken( true ) ) {
		return false;
	}
	result = g_ScripLibTable.m_pfnToken();
	return true;
}

// GetToken(false) emits warnings after it has crossed a line instead of being a
// hard line boundary.  Observe ScriptLib's line counter and use its one-token
// pushback to keep the next directive available to the caller.
static bool next_script_token_on_line( int line, std::string& result ){
	if ( !next_script_token( result ) ) {
		return false;
	}
	if ( g_ScripLibTable.m_pfnScriptLine() != line || result == "{" || result == "}" ) {
		g_ScripLibTable.m_pfnUnGetToken();
		return false;
	}
	return true;
}

// Shader keywords are not case sensitive (shader manual 2.3). Texture paths
// are case sensitive on unix (2.2), so this is used for directive names only.
static bool keyword_equals( const std::string& token, const char* keyword ){
	std::string::size_type i = 0;
	for ( ; i < token.size() && keyword[i] != '\0'; ++i ) {
		if ( tolower( static_cast<unsigned char>( token[i] ) ) != tolower( static_cast<unsigned char>( keyword[i] ) ) ) {
			return false;
		}
	}
	return i == token.size() && keyword[i] == '\0';
}

// Radiant resolves shader names case-insensitively (Shader_ForName uses
// stricmp) while storing the spelling found in the file, so the selected name
// is compared the same way.
static bool name_equals( const std::string& token, const char* name ){
	return keyword_equals( token, name );
}

// =============================================================================
// SELECTED SHADER PARSING

// Parse one named definition out of an already-loaded shader source buffer.
// The selection preview and the backdrop both need this, so it takes its target
// rather than writing to module state.  The buffer is handed to ScriptLib as-is
// and is not owned here.
static bool parse_definition_into( char* source, const char* shaderName, const char* sourceName,
								   std::vector<PreviewStage>& stages, std::string* editorImage,
								   std::vector<PreviewDeformWave>* deformWaves,
								   std::vector<PreviewDeformMove>* deformMoves,
								   std::vector<PreviewDeformNormal>* deformNormals,
								   std::vector<PreviewDeformBulge>* deformBulges ){
	g_ScripLibTable.m_pfnStartTokenParsing( source );
	int depth = 0;
	int currentStage = -1;
	bool inShader = false;
	bool shaderClosed = false;

	for (;;) {
		std::string token;
		if ( !next_script_token( token ) ) {
			break;
		}

		if ( token == "{" ) {
			++depth;
			if ( inShader && depth == 2 ) {
				stages.push_back( PreviewStage() );
				currentStage = static_cast<int>( stages.size() ) - 1;
			}
			continue;
		}

		if ( token == "}" ) {
			if ( depth > 0 ) {
				--depth;
			}
			if ( inShader && depth == 1 ) {
				currentStage = -1;
			}
			else if ( inShader && depth == 0 ) {
				// The selected shader's outer closing brace is a hard boundary.
				shaderClosed = true;
				break;
			}
			continue;
		}

		if ( !inShader ) {
			// A shader name is a definition header only at depth zero. Matching
			// it inside an earlier shader's body — where the same name often
			// appears as a map or qer_editorimage argument — would parse that
			// neighbour's stages instead.
			if ( depth == 0 && name_equals( token, shaderName ) ) {
				inShader = true;
			}
			continue;
		}

		if ( depth == 1 && keyword_equals( token, "qer_editorimage" ) ) {
			std::string imageName;
			if ( next_script_token_on_line( g_ScripLibTable.m_pfnScriptLine(), imageName ) ) {
				if ( editorImage != NULL ) *editorImage = imageName;
			}
			continue;
		}

		if ( depth == 1 && keyword_equals( token, "deformVertexes" ) ) {
			const int directiveLine = g_ScripLibTable.m_pfnScriptLine();
			std::string mode;
			if ( !next_script_token_on_line( directiveLine, mode ) ) {
				shadershop_warn( "deformVertexes has no mode" );
				continue;
			}
			if ( keyword_equals( mode, "wave" ) ) {
				std::string spread, wave, base, amplitude, phase, frequency;
				if ( !next_script_token_on_line( directiveLine, spread ) || !next_script_token_on_line( directiveLine, wave ) || !next_script_token_on_line( directiveLine, base ) || !next_script_token_on_line( directiveLine, amplitude ) || !next_script_token_on_line( directiveLine, phase ) || !next_script_token_on_line( directiveLine, frequency ) ) {
					shadershop_warn( "incomplete deformVertexes wave" );
					continue;
				}
				WaveForm form;
				if ( !parse_wave_form( wave, form ) ) {
					shadershop_warn( "deformVertexes waveform '%s' is not supported", wave.c_str() );
					continue;
				}
				if ( deformWaves != NULL ) {
					PreviewDeformWave deform;
					deform.spread = static_cast<float>( atof( spread.c_str() ) );
					deform.waveForm = form;
					deform.base = static_cast<float>( atof( base.c_str() ) );
					deform.amplitude = static_cast<float>( atof( amplitude.c_str() ) );
					deform.phase = static_cast<float>( atof( phase.c_str() ) );
					deform.frequency = static_cast<float>( atof( frequency.c_str() ) );
					deform.sourceLine = directiveLine;
					if ( deform.spread == 0.0f ) shadershop_warn( "deformVertexes wave has zero spread" );
					else deformWaves->push_back( deform );
				}
			}
			else if ( keyword_equals( mode, "move" ) ) {
				std::string values[8];
				bool complete = true;
				for ( int i = 0; i < 8; ++i ) complete = complete && next_script_token_on_line( directiveLine, values[i] );
				WaveForm form;
				if ( !complete ) shadershop_warn( "incomplete deformVertexes move" );
				else if ( !parse_wave_form( values[3], form ) ) shadershop_warn( "deformVertexes move waveform '%s' is not supported", values[3].c_str() );
				else if ( deformMoves != NULL ) {
					PreviewDeformMove deform;
					for ( int i = 0; i < 3; ++i ) deform.direction[i] = static_cast<float>( atof( values[i].c_str() ) );
					deform.waveForm = form;
					deform.base = static_cast<float>( atof( values[4].c_str() ) );
					deform.amplitude = static_cast<float>( atof( values[5].c_str() ) );
					deform.phase = static_cast<float>( atof( values[6].c_str() ) );
					deform.frequency = static_cast<float>( atof( values[7].c_str() ) );
					deform.sourceLine = directiveLine;
					deformMoves->push_back( deform );
				}
			}
			else if ( keyword_equals( mode, "normal" ) ) {
				std::string amplitude, frequency;
				if ( !next_script_token_on_line( directiveLine, amplitude ) || !next_script_token_on_line( directiveLine, frequency ) ) shadershop_warn( "incomplete deformVertexes normal" );
				else if ( deformNormals != NULL ) {
					PreviewDeformNormal deform;
					deform.amplitude = static_cast<float>( atof( amplitude.c_str() ) );
					deform.frequency = static_cast<float>( atof( frequency.c_str() ) );
					deform.sourceLine = directiveLine;
					deformNormals->push_back( deform );
				}
			}
			else if ( keyword_equals( mode, "bulge" ) ) {
				std::string width, height, speed;
				if ( !next_script_token_on_line( directiveLine, width ) || !next_script_token_on_line( directiveLine, height ) || !next_script_token_on_line( directiveLine, speed ) ) shadershop_warn( "incomplete deformVertexes bulge" );
				else if ( deformBulges != NULL ) {
					PreviewDeformBulge deform;
					deform.width = static_cast<float>( atof( width.c_str() ) );
					deform.height = static_cast<float>( atof( height.c_str() ) );
					deform.speed = static_cast<float>( atof( speed.c_str() ) );
					deform.sourceLine = directiveLine;
					deformBulges->push_back( deform );
				}
			}
			else {
				shadershop_warn( "deformVertexes %s is not previewable yet", mode.c_str() );
			}
			continue;
		}

		if ( depth != 2 || currentStage < 0 ) {
			continue;
		}

		PreviewStage& stage = stages[currentStage];

		const int stageNumber = currentStage + 1;
		const int directiveLine = g_ScripLibTable.m_pfnScriptLine();

		if ( keyword_equals( token, "map" ) || keyword_equals( token, "clampmap" ) ) {
			std::string name;
			if ( !next_script_token_on_line( directiveLine, name ) ) {
				shadershop_warn( "stage %d: '%s' has no argument", stageNumber, token.c_str() );
			}
			else {
				if ( !stage.mapName.empty() ) {
					shadershop_warn(
						"stage %d: image source replaced ('%s' after '%s')",
						stageNumber, name.c_str(), stage.mapName.c_str()
					);
				}
				stage.mapName = name;
				stage.clamp = keyword_equals( token, "clampmap" );
			}
		}
		else if ( keyword_equals( token, "animMap" ) ) {
			std::string rate;
			if ( !next_script_token_on_line( directiveLine, rate ) ) {
				shadershop_warn( "stage %d: animMap has no frequency", stageNumber );
			}
			stage.animationFps = static_cast<float>( atof( rate.c_str() ) );

			for (;;) {
				std::string frame;
				if ( !next_script_token_on_line( directiveLine, frame ) ) {
					break;
				}
				stage.animationNames.push_back( frame );
			}

			if ( stage.animationNames.size() < 2 ) {
				shadershop_warn(
					"stage %d: animMap lists %d frames; needs at least two to animate",
					stageNumber, static_cast<int>( stage.animationNames.size() )
				);
			}
			else if ( stage.animationNames.size() > ANIMATION_FRAME_LIMIT ) {
				// The engine stores a fixed eight frames per stage. Preview them
				// all rather than truncate, but the surplus will not play in game.
				shadershop_warn(
					"stage %d: animMap lists %d frames; the engine plays only the first %d",
					stageNumber, static_cast<int>( stage.animationNames.size() ),
					static_cast<int>( ANIMATION_FRAME_LIMIT )
				);
			}
			if ( stage.animationFps <= 0.0f ) {
				shadershop_warn( "stage %d: animMap frequency '%s' is not a positive rate", stageNumber, rate.c_str() );
			}
		}
		else if ( keyword_equals( token, "blendFunc" ) ) {
			std::string src;
			std::string dst;
			if ( !next_script_token_on_line( directiveLine, src ) ) {
				shadershop_warn( "stage %d: blendFunc has no arguments", stageNumber );
			}
			else if ( !next_script_token_on_line( directiveLine, dst ) ) {
				if ( keyword_equals( src, "add" ) ) {
					stage.blendSrc = "GL_ONE";
					stage.blendDst = "GL_ONE";
				}
				else if ( keyword_equals( src, "filter" ) ) {
					stage.blendSrc = "GL_DST_COLOR";
					stage.blendDst = "GL_ZERO";
				}
				else if ( keyword_equals( src, "blend" ) ) {
					stage.blendSrc = "GL_SRC_ALPHA";
					stage.blendDst = "GL_ONE_MINUS_SRC_ALPHA";
				}
				else {
					shadershop_warn( "stage %d: unknown blendFunc shorthand '%s'", stageNumber, src.c_str() );
				}
			}
			else {
				stage.blendSrc = src;
				stage.blendDst = dst;
			}
		}
		else if ( keyword_equals( token, "alphaFunc" ) ) {
			std::string mode;
			if ( !next_script_token_on_line( directiveLine, mode ) ) {
				shadershop_warn( "stage %d: alphaFunc has no argument", stageNumber );
			}
			else if ( keyword_equals( mode, "GT0" ) ) { stage.alphaFunction = GL_GREATER; stage.alphaReference = 0.0f; }
			else if ( keyword_equals( mode, "LT128" ) ) { stage.alphaFunction = GL_LESS; stage.alphaReference = 128.0f / 255.0f; }
			else if ( keyword_equals( mode, "GE128" ) ) { stage.alphaFunction = GL_GEQUAL; stage.alphaReference = 128.0f / 255.0f; }
			else shadershop_warn( "stage %d: alphaFunc '%s' is not supported", stageNumber, mode.c_str() );
		}
		else if ( keyword_equals( token, "alphaGen" ) ) {
			std::string mode;
			if ( next_script_token_on_line( directiveLine, mode ) && keyword_equals( mode, "portal" ) ) {
				std::string range;
				if ( !next_script_token_on_line( directiveLine, range ) ) shadershop_warn( "stage %d: alphaGen portal has no range", stageNumber );
				// Portal alpha is view-distance dependent. A standalone swatch has
				// no portal camera, so use a neutral midpoint rather than making the
				// overlay fully opaque or entirely discarding it.
				stage.alpha = 0.5f;
			}
		}
		else if ( keyword_equals( token, "tcGen" ) ) {
			std::string mode;
			if ( next_script_token_on_line( directiveLine, mode ) && keyword_equals( mode, "environment" ) ) {
				stage.environmentTexGen = true;
			}
		}
		else if ( keyword_equals( token, "tcMod" ) ) {
			std::string mode;
			if ( !next_script_token_on_line( directiveLine, mode ) ) continue;
			if ( keyword_equals( mode, "scroll" ) ) {
				std::string s, t;
				if ( !next_script_token_on_line( directiveLine, s ) || !next_script_token_on_line( directiveLine, t ) ) {
					shadershop_warn( "stage %d: incomplete tcMod scroll", stageNumber );
				}
				else {
					TcModOperation operation( TCMOD_SCROLL, directiveLine );
					operation.values[0] = static_cast<float>( atof( s.c_str() ) );
					operation.values[1] = static_cast<float>( atof( t.c_str() ) );
					stage.tcModOperations.push_back( operation );
				}
				continue;
			}
			if ( keyword_equals( mode, "rotate" ) ) {
				std::string rate;
				if ( !next_script_token_on_line( directiveLine, rate ) ) {
					shadershop_warn( "stage %d: incomplete tcMod rotate", stageNumber );
				}
				else {
					TcModOperation operation( TCMOD_ROTATE, directiveLine );
					operation.values[0] = static_cast<float>( atof( rate.c_str() ) );
					stage.tcModOperations.push_back( operation );
				}
				continue;
			}
			if ( keyword_equals( mode, "scale" ) ) {
				std::string s, t;
				if ( !next_script_token_on_line( directiveLine, s ) || !next_script_token_on_line( directiveLine, t ) ) {
					shadershop_warn( "stage %d: incomplete tcMod scale", stageNumber );
				}
				else {
					TcModOperation operation( TCMOD_SCALE, directiveLine );
					operation.values[0] = static_cast<float>( atof( s.c_str() ) );
					operation.values[1] = static_cast<float>( atof( t.c_str() ) );
					stage.tcModOperations.push_back( operation );
				}
				continue;
			}
			if ( keyword_equals( mode, "transform" ) ) {
				std::string values[6];
				bool complete = true;
				for ( int i = 0; i < 6; ++i ) complete = complete && next_script_token_on_line( directiveLine, values[i] );
				if ( !complete ) shadershop_warn( "stage %d: incomplete tcMod transform", stageNumber );
				else {
					TcModOperation operation( TCMOD_TRANSFORM, directiveLine );
					for ( int i = 0; i < 6; ++i ) operation.values[i] = static_cast<float>( atof( values[i].c_str() ) );
					stage.tcModOperations.push_back( operation );
				}
				continue;
			}
			if ( keyword_equals( mode, "turb" ) ) {
				std::string base, amplitude, phase, frequency;
				if ( !next_script_token_on_line( directiveLine, base ) || !next_script_token_on_line( directiveLine, amplitude ) || !next_script_token_on_line( directiveLine, phase ) || !next_script_token_on_line( directiveLine, frequency ) ) {
					shadershop_warn( "stage %d: incomplete tcMod turb", stageNumber );
				}
				else {
					TcModOperation operation( TCMOD_TURB, directiveLine );
					operation.values[0] = static_cast<float>( atof( base.c_str() ) );
					operation.values[1] = static_cast<float>( atof( amplitude.c_str() ) );
					operation.values[2] = static_cast<float>( atof( phase.c_str() ) );
					operation.values[3] = static_cast<float>( atof( frequency.c_str() ) );
					stage.tcModOperations.push_back( operation );
				}
				continue;
			}
			if ( !keyword_equals( mode, "stretch" ) ) continue;
			std::string wave, base, amplitude, phase, frequency;
			if ( !next_script_token_on_line( directiveLine, wave ) || !next_script_token_on_line( directiveLine, base ) || !next_script_token_on_line( directiveLine, amplitude ) || !next_script_token_on_line( directiveLine, phase ) || !next_script_token_on_line( directiveLine, frequency ) ) {
				shadershop_warn( "stage %d: incomplete %s %s", stageNumber, token.c_str(), mode.c_str() );
				continue;
			}
			WaveForm form;
			if ( !parse_wave_form( wave, form ) ) { shadershop_warn( "stage %d: waveform '%s' is not supported", stageNumber, wave.c_str() ); continue; }
			const float b = static_cast<float>( atof( base.c_str() ) ), a = static_cast<float>( atof( amplitude.c_str() ) ), p = static_cast<float>( atof( phase.c_str() ) ), f = static_cast<float>( atof( frequency.c_str() ) );
			TcModOperation operation( TCMOD_STRETCH, directiveLine );
			operation.waveForm = form;
			operation.values[0] = b; operation.values[1] = a; operation.values[2] = p; operation.values[3] = f;
			stage.tcModOperations.push_back( operation );
		}
		else if ( keyword_equals( token, "rgbGen" ) ) {
			std::string mode;
			if ( !next_script_token_on_line( directiveLine, mode ) || !keyword_equals( mode, "wave" ) ) continue;
			std::string wave, base, amplitude, phase, frequency;
			if ( !next_script_token_on_line( directiveLine, wave ) || !next_script_token_on_line( directiveLine, base ) || !next_script_token_on_line( directiveLine, amplitude ) || !next_script_token_on_line( directiveLine, phase ) || !next_script_token_on_line( directiveLine, frequency ) ) {
				shadershop_warn( "stage %d: incomplete rgbGen wave", stageNumber );
				continue;
			}
			WaveForm form;
			if ( !parse_wave_form( wave, form ) ) { shadershop_warn( "stage %d: waveform '%s' is not supported", stageNumber, wave.c_str() ); continue; }
			stage.rgbWave = true;
			stage.rgbWaveForm = form;
			stage.rgbBase = static_cast<float>( atof( base.c_str() ) );
			stage.rgbAmplitude = static_cast<float>( atof( amplitude.c_str() ) );
			stage.rgbPhase = static_cast<float>( atof( phase.c_str() ) );
			stage.rgbFrequency = static_cast<float>( atof( frequency.c_str() ) );
		}
	}

	if ( !inShader ) {
		shadershop_warn(
			"'%s' was not found at the top level of %s",
			shaderName, sourceName
		);
	}
	else if ( !shaderClosed ) {
		shadershop_warn(
			"'%s' reaches end of file without a closing brace",
			shaderName
		);
	}

	return inShader && shaderClosed;
}

// Every definition header in a shader source, in file order.  A .shader file
// commonly holds many, so choosing one of them is a real step rather than an
// implementation detail.
static void enumerate_definitions( char* source, std::vector<std::string>& names ){
	names.clear();
	if ( g_ScripLibTable.m_pfnStartTokenParsing == NULL || g_ScripLibTable.m_pfnGetToken == NULL ||
		 g_ScripLibTable.m_pfnToken == NULL ) {
		return;
	}

	g_ScripLibTable.m_pfnStartTokenParsing( source );
	int depth = 0;
	std::string pending;
	for (;;) {
		std::string token;
		if ( !next_script_token( token ) ) {
			break;
		}
		if ( token == "{" ) {
			if ( depth == 0 && !pending.empty() ) {
				names.push_back( pending );
				pending.clear();
			}
			++depth;
			continue;
		}
		if ( token == "}" ) {
			if ( depth > 0 ) {
				--depth;
			}
			continue;
		}
		if ( depth == 0 ) {
			pending = token;
		}
	}
}

static void parse_selected_stages( const char* shaderName ){
	clear_stages();
	g_diagnostics.clear();
	g_editorImageName.clear();
	g_previewBackdrop = BACKDROP_CHECKER;
	g_shaderSourceState = SHADER_SOURCE_RAW_IMAGE;

	if ( shaderName == NULL || g_ShadersTable.m_pfnShader_ForName_NoLoad == NULL || g_FuncTable.m_pfnLoadFile == NULL ||
		 g_ScripLibTable.m_pfnStartTokenParsing == NULL || g_ScripLibTable.m_pfnGetToken == NULL ||
		 g_ScripLibTable.m_pfnToken == NULL || g_ScripLibTable.m_pfnScriptLine == NULL ||
		 g_ScripLibTable.m_pfnUnGetToken == NULL ) {
		g_shaderSourceState = SHADER_SOURCE_UNAVAILABLE;
		return;
	}

	// Do not use Try_Shader_ForName here: it may load/bind Radiant's texture
	// from the UI callback, where the preview GL context is not current.
	IShader* shader = g_ShadersTable.m_pfnShader_ForName_NoLoad( shaderName );
	if ( shader == NULL || shader->getShaderFileName() == NULL || shader->getShaderFileName()[0] == '\0' ) {
		// No source file is the ordinary raw-image case, not an error.
		return;
	}
	g_shaderSourceState = SHADER_SOURCE_MALFORMED;

	void* buffer = NULL;
	const int size = g_FuncTable.m_pfnLoadFile( shader->getShaderFileName(), &buffer );
	if ( size <= 0 || buffer == NULL ) {
		g_shaderSourceState = SHADER_SOURCE_UNAVAILABLE;
		shadershop_warn( "could not read shader source '%s'", shader->getShaderFileName() );
		return;
	}

	const bool complete = parse_definition_into( static_cast<char*>( buffer ), shaderName, shader->getShaderFileName(), g_stages, &g_editorImageName, &g_deformWaves, &g_deformMoves, &g_deformNormals, &g_deformBulges );
	g_free( buffer );
	// ScriptLib's cursor is process-global; do not leave it pointing into the
	// buffer just released.
	if ( g_ScripLibTable.m_pfnStartTokenParsing != NULL ) {
		static char emptyScript[1] = { '\0' };
		g_ScripLibTable.m_pfnStartTokenParsing( emptyScript );
	}
	for ( std::vector<PreviewDeformWave>::const_iterator deform = g_deformWaves.begin(); deform != g_deformWaves.end(); ++deform ) {
		if ( deform->amplitude != 0.0f && deform->frequency != 0.0f ) {
			g_animationActive = true;
			break;
		}
	}
	for ( std::vector<PreviewDeformMove>::const_iterator deform = g_deformMoves.begin(); deform != g_deformMoves.end(); ++deform ) {
		if ( deform->amplitude != 0.0f && deform->frequency != 0.0f ) {
			g_animationActive = true;
			break;
		}
	}
	for ( std::vector<PreviewDeformNormal>::const_iterator deform = g_deformNormals.begin(); deform != g_deformNormals.end(); ++deform ) {
		if ( deform->amplitude != 0.0f && deform->frequency != 0.0f ) {
			g_animationActive = true;
			break;
		}
	}
	for ( std::vector<PreviewDeformBulge>::const_iterator deform = g_deformBulges.begin(); deform != g_deformBulges.end(); ++deform ) {
		if ( deform->height != 0.0f && deform->speed != 0.0f ) {
			g_animationActive = true;
			break;
		}
	}
	load_stage_images( g_stages, true );
	classify_destination_use();
	report_stage_resolution( shaderName );
	if ( complete ) {
		g_shaderSourceState = SHADER_SOURCE_PARSED;
	}
}

static void clear_selected_image(){
	if ( g_selectedPixels != NULL ) {
		g_free( g_selectedPixels );
		g_selectedPixels = NULL;
	}
	g_selectedWidth = 0;
	g_selectedHeight = 0;
	g_selectedTextureUploaded = false;
	retire_texture( g_selectedTexture );
}

static int animated_stage_count(){
	int count = 0;
	for ( std::vector<PreviewStage>::const_iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		if ( stage->animationFrames.size() > 1 && stage->animationFps > 0.0f ) {
			++count;
		}
	}
	return count;
}

static gboolean animation_tick( gpointer ){
	if ( g_pPreviewWidget == NULL || !g_animationActive ) {
		// Clear the id here too: returning FALSE destroys the source, and a
		// stale id makes the next g_source_remove raise a GLib critical and
		// start_animation_timer decline to restart.
		g_animationTimer = 0;
		return FALSE;
	}

#if GTK_CHECK_VERSION( 3, 0, 0 )
	gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
#else
	gtk_widget_queue_draw( g_pPreviewWidget );
#endif
	return TRUE;
}

static void queue_preview_render(){
	if ( g_pPreviewWidget == NULL ) {
		return;
	}
#if GTK_CHECK_VERSION( 3, 0, 0 )
	gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
	// queue_render only marks the widget as needing a repaint; actually
	// producing one is left to the toplevel's GdkFrameClock, which on the
	// quartz backend ticks on the display link of the window that owns it.
	// A newly-shown or not-yet-key window is not guaranteed to be driving
	// that link yet, so a queued render can sit unpainted indefinitely --
	// which is exactly what selecting the window (clicking its title bar)
	// was fixing by making it key. Ask GDK to run the paint cycle right
	// now instead of waiting for a frame-clock tick that may not come.
	if ( gtk_widget_get_realized( g_pPreviewWidget ) ) {
		GdkWindow* window = gtk_widget_get_window( g_pPreviewWidget );
		if ( window != NULL ) {
			gdk_window_process_updates( window, TRUE );
		}
	}
#else
	gtk_widget_queue_draw( g_pPreviewWidget );
#endif
}

static gboolean preview_first_frame_watchdog( gpointer ){
	if ( g_pPreviewWidget == NULL || g_previewFrameDrawn || g_previewFrameWaits >= PREVIEW_FIRST_FRAME_LIMIT ) {
		g_previewFrameTimer = 0;
		return FALSE;
	}

	++g_previewFrameWaits;
	queue_preview_render();
	return TRUE;
}

// Called from every point that believes it has just asked for the first frame.
// Cheap to call repeatedly: it is a no-op once a frame has actually landed.
static void watch_for_first_frame(){
	if ( g_pPreviewWidget == NULL || g_previewFrameDrawn || g_previewFrameTimer != 0 ) {
		return;
	}
	g_previewFrameWaits = 0;
	g_previewFrameTimer = g_timeout_add( PREVIEW_FIRST_FRAME_INTERVAL_MS, preview_first_frame_watchdog, NULL );
}

static void start_animation_timer(){
	if ( g_animationTimer == 0 && !g_animationPaused && g_animationActive ) {
		g_animationTimer = g_timeout_add( PREVIEW_REPAINT_INTERVAL_MS, animation_tick, NULL );
	}
}

static void animation_clicked( GtkButton*, gpointer ){
	if ( g_animationTimer != 0 ) {
		g_source_remove( g_animationTimer );
		g_animationTimer = 0;
		g_animationPaused = true;
		preview_clock_pause();
		gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Play" );
	}
	else {
		g_animationPaused = false;
		preview_clock_resume();
		start_animation_timer();
		gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Pause" );
	}
}

static const char* normalise_shader_name( const char* selected, std::string& storage ){
	if ( selected != NULL && !strncmp( selected, "texture/", 8 ) ) {
		storage = "textures/";
		storage += selected + 8;
		return storage.c_str();
	}
	return selected;
}

static void set_stage_label( bool hasDirectImage, bool hasEditorImage ){
	if ( g_stageLabel == NULL ) {
		return;
	}

	char stages[256];
	switch ( g_shaderSourceState ) {
	case SHADER_SOURCE_RAW_IMAGE:
		snprintf( stages, sizeof( stages ), "Raw image — no .shader definition" );
		break;
	case SHADER_SOURCE_PARSED:
		if ( g_stages.empty() ) {
			snprintf( stages, sizeof( stages ), "Shader definition — no render stages" );
		}
		else {
			snprintf(
				stages,
				sizeof( stages ),
				"Shader definition: %d stages, %s, animated: %d",
				static_cast<int>( g_stages.size() ),
				hasDirectImage ? "direct thumbnail image" : ( hasEditorImage ? "editor thumbnail image" : "no thumbnail image" ),
				animated_stage_count()
			);
		}
		break;
	case SHADER_SOURCE_UNAVAILABLE:
		snprintf( stages, sizeof( stages ), "Shader source unavailable — direct image preview" );
		break;
	case SHADER_SOURCE_MALFORMED:
		snprintf( stages, sizeof( stages ), "Shader definition could not be parsed" );
		break;
	}

	if ( g_backdropButton != NULL ) {
		static const char* modeLabels[] = { "Auto", "Checkerboard", "Lightmap", "Dark" };
		gtk_button_set_label(
			GTK_BUTTON( g_backdropButton ),
			g_backdropMode == BACKDROP_MODE_IMAGE ? g_backdropName.c_str() : modeLabels[g_backdropMode]
		);
	}

	if ( g_previewBackdrop == BACKDROP_IMAGE ) {
		char backdrop[96];
		snprintf( backdrop, sizeof( backdrop ), " — over %s", g_backdropName.c_str() );
		strncat( stages, backdrop, sizeof( stages ) - strlen( stages ) - 1 );
	}
	else if ( g_previewBackdrop != BACKDROP_CHECKER ) {
		strncat(
			stages,
			g_previewBackdrop == BACKDROP_LIGHTMAP ? " — over lightmap backdrop" : " — over dark backdrop",
			sizeof( stages ) - strlen( stages ) - 1
		);
	}

	// Tolerated problems are reported here as well as on the console, so a
	// wrong-looking preview can be told apart from a correct one.
	if ( !g_diagnostics.empty() ) {
		char issues[64];
		snprintf(
			issues,
			sizeof( issues ),
			" — %d issue%s, see console",
			static_cast<int>( g_diagnostics.size() ),
			g_diagnostics.size() == 1 ? "" : "s"
		);
		strncat( stages, issues, sizeof( stages ) - strlen( stages ) - 1 );
	}
	gtk_label_set_text( GTK_LABEL( g_stageLabel ), stages );
}

void ShaderShop_RefreshSelection(){
	if ( g_pSelectionLabel == NULL ) {
		return;
	}

	const char* rawSelected = g_FuncTable.m_pfnGetCurrentTexture();
	std::string selectedStorage;
	const char* selected = normalise_shader_name( rawSelected, selectedStorage );
	if ( selected != NULL && selected[0] != '\0' ) {
		char text[512];
		snprintf( text, sizeof( text ), "Selected shader: %s", selected );
		gtk_label_set_text( GTK_LABEL( g_pSelectionLabel ), text );

		g_editorDirty = false;
		parse_selected_stages( selected );

		if ( g_animationTimer != 0 ) {
			g_source_remove( g_animationTimer );
			g_animationTimer = 0;
		}
		g_animationPaused = false;
		preview_clock_reset();
		start_animation_timer();

		if ( g_animationButton != NULL ) {
			gtk_widget_set_sensitive( g_animationButton, g_animationActive );
			gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Pause" );
		}

		clear_selected_image();
		const bool hasDirectImage = load_image( selected, &g_selectedPixels, &g_selectedWidth, &g_selectedHeight );
		bool hasEditorImage = false;
		if ( !hasDirectImage && !g_editorImageName.empty() ) {
			hasEditorImage = load_image( g_editorImageName.c_str(), &g_selectedPixels, &g_selectedWidth, &g_selectedHeight );
		}

		// A shader name is not necessarily an image filename. Ask the shader
		// system for its representative texture first, then decode that image
		// into this plugin's own GL context.
		const char* imageName = NULL;
		if ( !hasDirectImage && !hasEditorImage && g_ShadersTable.m_pfnShader_ForName_NoLoad != NULL ) {
			IShader* shader = g_ShadersTable.m_pfnShader_ForName_NoLoad( selected );
			if ( shader != NULL && shader->getTexture() != NULL ) {
				imageName = shader->getTexture()->name;
			}
		}

		if ( !hasDirectImage && !hasEditorImage && imageName != NULL && imageName[0] != '\0' ) {
			load_image( imageName, &g_selectedPixels, &g_selectedWidth, &g_selectedHeight );
		}
		set_stage_label( hasDirectImage, hasEditorImage );
	}
	else {
		gtk_label_set_text( GTK_LABEL( g_pSelectionLabel ), "No current shader selected — showing checkerboard" );
		clear_stages();
		clear_selected_image();
		g_diagnostics.clear();

		if ( g_animationTimer != 0 ) {
			g_source_remove( g_animationTimer );
			g_animationTimer = 0;
		}
		if ( g_animationButton != NULL ) {
			gtk_widget_set_sensitive( g_animationButton, FALSE );
		}
		if ( g_stageLabel != NULL ) {
			gtk_label_set_text( GTK_LABEL( g_stageLabel ), "Parsed stages: 0" );
		}
		g_editorDirty = false;
	}

	rebuild_editor_stage_stack();

	// g_previewFrameDrawn is a latch on "has a frame reached the screen since
	// the last thing that could invalidate it", not "since the window opened".
	// A shader switch is exactly such an invalidation: the queue_render() below
	// is a single request, as fragile as the very first one was, and a large or
	// complex shader is the case most likely to have it land at a moment GTK
	// drops it (mid-allocate, mid-realize). Re-arm the watchdog here so a
	// dropped request for THIS selection gets asked for again, instead of
	// relying on the previous selection's frame having already latched true.
	g_previewFrameDrawn = false;
	queue_preview_render();
	watch_for_first_frame();
}

static void ensure_checkerboard_texture(){
	if ( g_checkerboardTexture != 0 ) {
		return;
	}

	const int size = 64;
	unsigned char pixels[size * size * 4];
	for ( int y = 0; y < size; ++y ) {
		for ( int x = 0; x < size; ++x ) {
			const int offset = ( y * size + x ) * 4;
			const unsigned char value = ( ( x / 8 + y / 8 ) & 1 ) ? 72 : 176;
			pixels[offset + 0] = value;
			pixels[offset + 1] = value;
			pixels[offset + 2] = value;
			pixels[offset + 3] = 255;
		}
	}

	g_QglTable.m_pfn_qglGenTextures( 1, &g_checkerboardTexture );
	g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, g_checkerboardTexture );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
	g_QglTable.m_pfn_qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
}

static GLenum blend_factor( const std::string& factor ){
	std::string value = factor;
	for ( std::string::iterator i = value.begin(); i != value.end(); ++i ) *i = tolower( static_cast<unsigned char>( *i ) );
	if ( value.compare( 0, 3, "gl_" ) == 0 ) value.erase( 0, 3 );
	if ( value == "one" ) return GL_ONE;
	if ( value == "zero" ) return GL_ZERO;
	if ( value == "src_color" ) return GL_SRC_COLOR;
	if ( value == "one_minus_src_color" ) return GL_ONE_MINUS_SRC_COLOR;
	if ( value == "dst_color" ) return GL_DST_COLOR;
	if ( value == "one_minus_dst_color" ) return GL_ONE_MINUS_DST_COLOR;
	if ( value == "src_alpha" ) return GL_SRC_ALPHA;
	if ( value == "one_minus_src_alpha" ) return GL_ONE_MINUS_SRC_ALPHA;
	if ( value == "dst_alpha" ) return GL_DST_ALPHA;
	if ( value == "one_minus_dst_alpha" ) return GL_ONE_MINUS_DST_ALPHA;
	return GL_SRC_ALPHA;
}

static void upload_texture( GLuint& texture, bool& uploaded, unsigned char* pixels, int width, int height, bool clamp = false ){
	if ( pixels == NULL || uploaded ) {
		return;
	}

	g_QglTable.m_pfn_qglGenTextures( 1, &texture );
	g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, texture );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT );
	g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT );
	g_QglTable.m_pfn_qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	uploaded = true;
}

static bool stage_dimensions( const PreviewStage& stage, int& width, int& height ){
	if ( stage.generatedLightmap ) {
		// The white 1x1 is a compositing placeholder, not the material's
		// geometry or display extent.
		return false;
	}
	// Frames that failed to load are kept as placeholders to preserve frame
	// numbering, so the first frame is not necessarily the first with an image.
	for ( std::vector<AnimationFrame>::const_iterator frame = stage.animationFrames.begin(); frame != stage.animationFrames.end(); ++frame ) {
		if ( frame->pixels != NULL && frame->width > 0 && frame->height > 0 ) {
			width = frame->width;
			height = frame->height;
			return true;
		}
	}
	if ( stage.pixels != NULL && stage.width > 0 && stage.height > 0 ) {
		width = stage.width;
		height = stage.height;
		return true;
	}
	return false;
}

static GLuint stage_texture( const PreviewStage& stage ){
	if ( !stage.animationFrames.empty() ) {
		unsigned int frameIndex = 0;
		if ( stage.animationFrames.size() > 1 && stage.animationFps > 0.0f ) {
			// floor( time * frequency ) mod frameCount, per stage and in real
			// time, so stages with different frequencies stay independent.
			const double frame = floor( preview_seconds() * stage.animationFps );
			frameIndex = static_cast<unsigned int>(
				fmod( frame, static_cast<double>( stage.animationFrames.size() ) )
			);
		}
		return stage.animationFrames[frameIndex].uploaded ? stage.animationFrames[frameIndex].texture : 0;
	}
	return stage.uploaded ? stage.texture : 0;
}

static void draw_uniform_quad( float level, float left, float bottom, float right, float top ){
	g_QglTable.m_pfn_qglDisable( GL_TEXTURE_2D );
	g_QglTable.m_pfn_qglColor4f( level, level, level, 1.0f );
	g_QglTable.m_pfn_qglBegin( GL_QUADS );
	g_QglTable.m_pfn_qglVertex2f( left, bottom );
	g_QglTable.m_pfn_qglVertex2f( right, bottom );
	g_QglTable.m_pfn_qglVertex2f( right, top );
	g_QglTable.m_pfn_qglVertex2f( left, top );
	g_QglTable.m_pfn_qglEnd();
	g_QglTable.m_pfn_qglEnable( GL_TEXTURE_2D );
	g_QglTable.m_pfn_qglColor4f( 1, 1, 1, 1 );
}

static void draw_textured_quad( GLuint texture, float left, float bottom, float right, float top ){
	g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, texture );
	g_QglTable.m_pfn_qglBegin( GL_QUADS );
	g_QglTable.m_pfn_qglTexCoord2f( 0, 1 ); g_QglTable.m_pfn_qglVertex2f( left, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( 1, 1 ); g_QglTable.m_pfn_qglVertex2f( right, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( 1, 0 ); g_QglTable.m_pfn_qglVertex2f( right, top );
	g_QglTable.m_pfn_qglTexCoord2f( 0, 0 ); g_QglTable.m_pfn_qglVertex2f( left, top );
	g_QglTable.m_pfn_qglEnd();
}

static float stage_wave( WaveForm form, float base, float amplitude, float phase, float frequency ){
	const float time = static_cast<float>( preview_seconds() );
	return base + amplitude * wave_value( form, phase + time * frequency );
}

static float preview_time(){
	return static_cast<float>( preview_seconds() );
}

struct PreviewTexCoord
{
	float s;
	float t;
};

// Q3 applies coordinate modifiers in declaration order. Keeping that rule in
// one evaluator means the current swatch and the later tessellated inspection
// mesh cannot quietly diverge.
static PreviewTexCoord evaluate_stage_texcoord( const PreviewStage& stage, float s, float t, float elapsedSeconds ){
	for ( std::vector<TcModOperation>::const_iterator operation = stage.tcModOperations.begin(); operation != stage.tcModOperations.end(); ++operation ) {
		switch ( operation->kind ) {
		case TCMOD_SCROLL:
			s += operation->values[0] * elapsedSeconds;
			t += operation->values[1] * elapsedSeconds;
			break;
		case TCMOD_ROTATE: {
			const float radians = operation->values[0] * elapsedSeconds * 0.01745329251994329577f;
			const float cosine = cosf( radians );
			const float sine = sinf( radians );
			const float centeredS = s - 0.5f;
			const float centeredT = t - 0.5f;
			s = 0.5f + cosine * centeredS - sine * centeredT;
			t = 0.5f + sine * centeredS + cosine * centeredT;
			break;
		}
		case TCMOD_SCALE:
			s *= operation->values[0];
			t *= operation->values[1];
			break;
		case TCMOD_TRANSFORM: {
			const float sourceS = s;
			s = operation->values[0] * sourceS + operation->values[1] * t + operation->values[4];
			t = operation->values[2] * sourceS + operation->values[3] * t + operation->values[5];
			break;
		}
		case TCMOD_STRETCH: {
			const float stretch = operation->values[0] + operation->values[1] * wave_value( operation->waveForm, operation->values[2] + elapsedSeconds * operation->values[3] );
			// The shader's waveform is the visual scale.  Texture coordinates
			// therefore use its reciprocal, as does the original renderer.
			// Leave a degenerate authored value stable instead of dividing by zero.
			if ( fabsf( stretch ) > 0.0001f ) {
				const float reciprocal = 1.0f / stretch;
				s = 0.5f + ( s - 0.5f ) * reciprocal;
				t = 0.5f + ( t - 0.5f ) * reciprocal;
			}
			break;
		}
		case TCMOD_TURB: {
			// tcMod turb offsets each coordinate from the other, producing the
			// characteristic circulating churn rather than a uniform translation.
			// The language leaves base undefined; retain it as a phase offset so
			// authored nonzero values still produce a distinct, stable result.
			const float sourceS = s;
			const float sourceT = t;
			const float timePhase = operation->values[0] + operation->values[2] + elapsedSeconds * operation->values[3];
			s = sourceS + sinf( 6.28318530718f * ( sourceT + timePhase ) ) * operation->values[1];
			t = sourceT + sinf( 6.28318530718f * ( sourceS + timePhase ) ) * operation->values[1];
			break;
		}
		}
	}
	PreviewTexCoord coordinate = { s, t };
	return coordinate;
}

static void draw_stage_textured_quad( GLuint texture, const PreviewStage& stage, float left, float bottom, float right, float top ){
	const float elapsedSeconds = preview_time();
	const PreviewTexCoord bottomLeft = evaluate_stage_texcoord( stage, 0.0f, 1.0f, elapsedSeconds );
	const PreviewTexCoord bottomRight = evaluate_stage_texcoord( stage, 1.0f, 1.0f, elapsedSeconds );
	const PreviewTexCoord topRight = evaluate_stage_texcoord( stage, 1.0f, 0.0f, elapsedSeconds );
	const PreviewTexCoord topLeft = evaluate_stage_texcoord( stage, 0.0f, 0.0f, elapsedSeconds );
	g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, texture );
	g_QglTable.m_pfn_qglBegin( GL_QUADS );
	g_QglTable.m_pfn_qglTexCoord2f( bottomLeft.s, bottomLeft.t ); g_QglTable.m_pfn_qglVertex2f( left, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( bottomRight.s, bottomRight.t ); g_QglTable.m_pfn_qglVertex2f( right, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( topRight.s, topRight.t ); g_QglTable.m_pfn_qglVertex2f( right, top );
	g_QglTable.m_pfn_qglTexCoord2f( topLeft.s, topLeft.t ); g_QglTable.m_pfn_qglVertex2f( left, top );
	g_QglTable.m_pfn_qglEnd();
}

// Keep this deliberately modest: it is dense enough for the first vertex
// deformation and turbulence work, but remains inexpensive on legacy GL and
// avoids requiring vertex buffers or programmable rendering.
static const int PREVIEW_MESH_SUBDIVISIONS = 24;

// Material space is normalised to a unit-long side while shader deformation
// values are authored in Quake world units. This conversion gives ordinary
// amplitudes (roughly 1--5 units) a visible but restrained inspection scale.
static const float PREVIEW_DEFORM_WORLD_UNITS = 64.0f;

static float preview_deform_height( float u, float v, float elapsedSeconds ){
	float height = 0.0f;
	for ( std::vector<PreviewDeformWave>::const_iterator deform = g_deformWaves.begin(); deform != g_deformWaves.end(); ++deform ) {
		// Quake's wave deformation spreads the waveform over vertex position.
		// The preview's u/v coordinates stand in for a 64-unit material side.
		const float spatialPhase = ( u + v ) * PREVIEW_DEFORM_WORLD_UNITS / deform->spread;
		const float wave = wave_value( deform->waveForm, deform->phase + elapsedSeconds * deform->frequency + spatialPhase );
		height += ( deform->base + deform->amplitude * wave ) / PREVIEW_DEFORM_WORLD_UNITS;
	}
	for ( std::vector<PreviewDeformBulge>::const_iterator deform = g_deformBulges.begin(); deform != g_deformBulges.end(); ++deform ) {
		// Quake's bulge travels along the surface S coordinate and pushes along
		// its normal. The preview plane's initial normal is +Z.
		height += sinf( 6.28318530718f * ( u * deform->width + elapsedSeconds * deform->speed ) ) * deform->height / PREVIEW_DEFORM_WORLD_UNITS;
	}
	return height;
}

static void preview_deform_move( float elapsedSeconds, float& x, float& y, float& z ){
	for ( std::vector<PreviewDeformMove>::const_iterator deform = g_deformMoves.begin(); deform != g_deformMoves.end(); ++deform ) {
		const float amount = deform->base + deform->amplitude * wave_value( deform->waveForm, deform->phase + elapsedSeconds * deform->frequency );
		x += deform->direction[0] * amount / PREVIEW_DEFORM_WORLD_UNITS;
		y += deform->direction[1] * amount / PREVIEW_DEFORM_WORLD_UNITS;
		z += deform->direction[2] * amount / PREVIEW_DEFORM_WORLD_UNITS;
	}
}

static float preview_normal_noise( float x, float y, float z, float time, float phase ){
	// The game uses a smooth four-dimensional noise field. This compact,
	// deterministic approximation supplies the same continuously moving normal
	// perturbation without importing another renderer's noise implementation.
	return sinf( x * 7.19f + y * 11.71f + z * 5.31f + time * 1.93f + phase ) *
		   sinf( x * 3.67f - y * 8.23f + z * 9.17f + time * 1.21f + phase * 0.37f );
}

static void preview_deform_normal( float u, float v, float z, float elapsedSeconds, float& normalX, float& normalY, float& normalZ ){
	for ( std::vector<PreviewDeformNormal>::const_iterator deform = g_deformNormals.begin(); deform != g_deformNormals.end(); ++deform ) {
		const float time = elapsedSeconds * deform->frequency;
		normalX += deform->amplitude * preview_normal_noise( u, v, z, time, 0.0f );
		normalY += deform->amplitude * preview_normal_noise( u, v, z, time, 100.0f );
		normalZ += deform->amplitude * preview_normal_noise( u, v, z, time, 200.0f );
	}
}

static bool preview_has_geometry_deforms(){
	return !g_deformWaves.empty() || !g_deformMoves.empty() || !g_deformNormals.empty() || !g_deformBulges.empty();
}

static void draw_stage_mesh_vertex( const PreviewStage& stage, float u, float v, float x, float y, float width, float height, float elapsedSeconds ){
	const float normalStep = 1.0f / PREVIEW_MESH_SUBDIVISIONS;
	float z = preview_deform_height( u, v, elapsedSeconds );
	const float dzdu = ( preview_deform_height( u + normalStep, v, elapsedSeconds ) - preview_deform_height( u - normalStep, v, elapsedSeconds ) ) / ( 2.0f * normalStep );
	const float dzdv = ( preview_deform_height( u, v + normalStep, elapsedSeconds ) - preview_deform_height( u, v - normalStep, elapsedSeconds ) ) / ( 2.0f * normalStep );
	float normalX = -dzdu / width;
	float normalY = -dzdv / height;
	float normalZ = 1.0f;
	preview_deform_normal( u, v, z, elapsedSeconds, normalX, normalY, normalZ );
	const float normalLength = sqrtf( normalX * normalX + normalY * normalY + normalZ * normalZ );
	if ( normalLength != 0.0f ) {
		normalX /= normalLength;
		normalY /= normalLength;
		normalZ /= normalLength;
	}

	const PreviewTexCoord coordinate = evaluate_stage_texcoord( stage, u, 1.0f - v, elapsedSeconds );
	preview_deform_move( elapsedSeconds, x, y, z );
	g_QglTable.m_pfn_qglNormal3f( normalX, normalY, normalZ );
	g_QglTable.m_pfn_qglTexCoord2f( coordinate.s, coordinate.t );
	g_QglTable.m_pfn_qglVertex3f( x, y, z );
}

static void draw_stage_textured_mesh( GLuint texture, const PreviewStage& stage, float left, float bottom, float right, float top ){
	const float elapsedSeconds = preview_time();
	const float width = right - left;
	const float height = top - bottom;
	g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, texture );
	for ( int row = 0; row < PREVIEW_MESH_SUBDIVISIONS; ++row ) {
		const float lowerV = static_cast<float>( row ) / PREVIEW_MESH_SUBDIVISIONS;
		const float upperV = static_cast<float>( row + 1 ) / PREVIEW_MESH_SUBDIVISIONS;
		g_QglTable.m_pfn_qglBegin( GL_QUAD_STRIP );
		for ( int column = 0; column <= PREVIEW_MESH_SUBDIVISIONS; ++column ) {
			const float u = static_cast<float>( column ) / PREVIEW_MESH_SUBDIVISIONS;
			const float x = left + width * u;
			draw_stage_mesh_vertex( stage, u, lowerV, x, bottom + height * lowerV, width, height, elapsedSeconds );
			draw_stage_mesh_vertex( stage, u, upperV, x, bottom + height * upperV, width, height, elapsedSeconds );
		}
		g_QglTable.m_pfn_qglEnd();
	}
}

static void draw_stage_textured_surface( GLuint texture, const PreviewStage& stage, float left, float bottom, float right, float top ){
	if ( g_3dInspect || preview_has_geometry_deforms() || stage_requires_tessellated_preview( stage ) ) {
		draw_stage_textured_mesh( texture, stage, left, bottom, right, top );
	}
	else {
		draw_stage_textured_quad( texture, stage, left, bottom, right, top );
	}
}

static void draw_preview(){
	// Prefer the size GtkGLArea reported for its own buffer; fall back to the
	// allocation only before the first resize has been seen.
	int width = g_previewBufferWidth;
	int height = g_previewBufferHeight;
	if ( width <= 0 || height <= 0 ) {
		width = gtkutil_widget_get_width( g_pPreviewWidget );
		height = gtkutil_widget_get_height( g_pPreviewWidget );
	}

	if ( width <= 0 || height <= 0 ) {
		// The surface is not usable yet.  Clear anyway rather than returning
		// into an undefined buffer, and ask for another frame: GtkGLArea will
		// not repaint on its own, so a render that draws nothing and queues
		// nothing leaves the window blank until unrelated damage arrives.
		g_QglTable.m_pfn_qglClearColor( 0.08f, 0.09f, 0.11f, 1.0f );
		g_QglTable.m_pfn_qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		if ( g_previewSizeRetries < PREVIEW_SIZE_RETRY_LIMIT ) {
			++g_previewSizeRetries;
			queue_preview_render();
		}
		return;
	}
	// A frame reached a usable surface, so the retry budget is spent on the
	// next stall, not carried over from this one.
	g_previewSizeRetries = 0;
	g_previewFrameDrawn = true;

	// The preview context is current here and nowhere else, so this is where
	// superseded textures are actually released.
	delete_retired_textures();

	ensure_checkerboard_texture();

	for ( std::vector<PreviewStage>::iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		if ( !stage->animationFrames.empty() ) {
			for ( std::vector<AnimationFrame>::iterator frame = stage->animationFrames.begin(); frame != stage->animationFrames.end(); ++frame ) {
				upload_texture( frame->texture, frame->uploaded, frame->pixels, frame->width, frame->height );
			}
		}
		else {
			upload_texture( stage->texture, stage->uploaded, stage->pixels, stage->width, stage->height, stage->clamp );
		}
	}

	upload_texture( g_selectedTexture, g_selectedTextureUploaded, g_selectedPixels, g_selectedWidth, g_selectedHeight );

	// The first usable GL frame is the visual zero of a newly selected material.
	// In particular, a rapid tcMod rotate must not accrue time while its texture
	// is still being decoded or uploaded.
	if ( g_animationActive && !g_animationPaused && g_animationResumed == 0 ) {
		preview_clock_resume();
	}

	g_QglTable.m_pfn_qglViewport( 0, 0, width, height );
	g_QglTable.m_pfn_qglClearColor( 0.08f, 0.09f, 0.11f, 1.0f );
	g_QglTable.m_pfn_qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	g_QglTable.m_pfn_qglDisable( GL_DEPTH_TEST );

	int imageWidth = 0;
	int imageHeight = 0;
	bool haveStageImage = false;
	for ( std::vector<PreviewStage>::const_iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		if ( stage_dimensions( *stage, imageWidth, imageHeight ) ) {
			haveStageImage = true;
			break;
		}
	}
	if ( !haveStageImage && g_selectedTextureUploaded ) {
		imageWidth = g_selectedWidth;
		imageHeight = g_selectedHeight;
	}
	if ( imageWidth <= 0 || imageHeight <= 0 ) {
		imageWidth = 1;
		imageHeight = 1;
	}

	// Material half-extents in centred space; the longer side spans one unit so
	// the material's own aspect ratio is the only thing these carry.
	const float materialAspect = static_cast<float>( imageWidth ) / static_cast<float>( imageHeight );
	const float halfX = materialAspect >= 1.0f ? 0.5f : 0.5f * materialAspect;
	const float halfY = materialAspect >= 1.0f ? 0.5f / materialAspect : 0.5f;

	const float left = -halfX;
	const float bottom = -halfY;
	const float right = halfX;
	const float top = halfY;

	// View extents: contain the material with margin, then widen whichever axis
	// the widget requires so the material is never stretched.
	const float viewAspect = static_cast<float>( width ) / static_cast<float>( height );
	const float zoom = g_3dInspect ? g_inspectZoom : 1.0f;
	float viewHalfY = std::max( halfY, halfX / viewAspect ) / PREVIEW_FILL / zoom;
	float viewHalfX = viewHalfY * viewAspect;
	g_viewHalfX = viewHalfX;
	g_viewHalfY = viewHalfY;

	g_QglTable.m_pfn_qglMatrixMode( GL_PROJECTION );
	g_QglTable.m_pfn_qglLoadIdentity();
	g_QglTable.m_pfn_qglOrtho( -viewHalfX, viewHalfX, -viewHalfY, viewHalfY, 0.1, 100.0 );

	g_QglTable.m_pfn_qglMatrixMode( GL_MODELVIEW );
	g_QglTable.m_pfn_qglLoadIdentity();
	if ( g_3dInspect ) {
		// Pan is a view-space offset; the rotations then act on geometry that is
		// already centred on the origin, so the material turns in place.
		g_QglTable.m_pfn_qglTranslatef( g_inspectPanX, g_inspectPanY, -PREVIEW_EYE_DISTANCE );
		g_QglTable.m_pfn_qglRotatef( g_inspectPitch, 1.0f, 0.0f, 0.0f );
		g_QglTable.m_pfn_qglRotatef( g_inspectYaw, 0.0f, 1.0f, 0.0f );
	}
	else {
		g_QglTable.m_pfn_qglTranslatef( 0.0f, 0.0f, -PREVIEW_EYE_DISTANCE );
	}

	g_QglTable.m_pfn_qglEnable( GL_TEXTURE_2D );
	g_QglTable.m_pfn_qglColor4f( 1, 1, 1, 1 );

	upload_texture( g_backdropImage.texture, g_backdropImage.uploaded, g_backdropImage.pixels, g_backdropImage.width, g_backdropImage.height );

	for ( std::vector<PreviewStage>::iterator stage = g_backdropStages.begin(); stage != g_backdropStages.end(); ++stage ) {
		if ( !stage->animationFrames.empty() ) {
			for ( std::vector<AnimationFrame>::iterator frame = stage->animationFrames.begin(); frame != stage->animationFrames.end(); ++frame ) {
				upload_texture( frame->texture, frame->uploaded, frame->pixels, frame->width, frame->height );
			}
		}
		else {
			upload_texture( stage->texture, stage->uploaded, stage->pixels, stage->width, stage->height, stage->clamp );
		}
	}

	g_QglTable.m_pfn_qglDisable( GL_BLEND );
	if ( g_previewBackdrop == BACKDROP_IMAGE && !g_backdropStages.empty() ) {
		// A backdrop shader is composited over the lightmap field, then the
		// selected stack is composited over that. Bounded by construction: a
		// backdrop never gets a backdrop of its own.
		draw_uniform_quad( g_lightmapLevel, left, bottom, right, top );
		for ( std::vector<PreviewStage>::const_iterator stage = g_backdropStages.begin(); stage != g_backdropStages.end(); ++stage ) {
			const GLuint texture = stage_texture( *stage );
			if ( texture == 0 || !stage_is_drawn( *stage ) ) {
				continue;
			}
			g_QglTable.m_pfn_qglEnable( GL_BLEND );
			g_QglTable.m_pfn_qglBlendFunc( blend_factor( stage->blendSrc ), blend_factor( stage->blendDst ) );
			draw_textured_quad( texture, left, bottom, right, top );
		}
		g_QglTable.m_pfn_qglDisable( GL_BLEND );
		g_QglTable.m_pfn_qglColor4f( 1, 1, 1, 1 );
	}
	else if ( g_previewBackdrop == BACKDROP_IMAGE && g_backdropImage.uploaded ) {
		draw_textured_quad( g_backdropImage.texture, left, bottom, right, top );
	}
	else if ( g_previewBackdrop != BACKDROP_CHECKER ) {
		// This stack reads the destination, so whatever sits behind it is an
		// operand. Give it a defined uniform field instead of the checkerboard;
		// the pattern, not the brightness, is what corrupts such a material.
		draw_uniform_quad( g_previewBackdrop == BACKDROP_LIGHTMAP ? g_lightmapLevel : PREVIEW_BACKDROP_DARK, left, bottom, right, top );
	}
	else {
		// BACKDROP_CHECKER: the stack provably cannot sample it.
		draw_textured_quad( g_checkerboardTexture, left, bottom, right, top );
	}

	// Stage coordinates are supplied directly by evaluate_stage_texcoord. Do not
	// inherit a texture matrix from another Radiant drawing path.
	g_QglTable.m_pfn_qglMatrixMode( GL_TEXTURE );
	g_QglTable.m_pfn_qglLoadIdentity();
	g_QglTable.m_pfn_qglMatrixMode( GL_MODELVIEW );

	bool drewStage = false;
	for ( std::vector<PreviewStage>::const_iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		// White stands in for a neutral lightmap only in the usual filter stage.
		// Under any other blend it would replace the material with a white
		// rectangle in this context-free preview.
		if ( !stage_is_drawn( *stage ) ) {
			continue;
		}
		const GLuint texture = stage_texture( *stage );
		if ( texture == 0 ) {
			continue;
		}

		g_QglTable.m_pfn_qglEnable( GL_BLEND );
		g_QglTable.m_pfn_qglBlendFunc( blend_factor( stage->blendSrc ), blend_factor( stage->blendDst ) );
		float brightness = stage->rgbWave ? stage_wave( stage->rgbWaveForm, stage->rgbBase, stage->rgbAmplitude, stage->rgbPhase, stage->rgbFrequency ) : 1.0f;
		// The generated lightmap is a white 1x1; the stand-in level is applied as
		// a colour multiplier so moving the control needs no texture rebuild.
		if ( stage->generatedLightmap ) {
			brightness *= g_lightmapLevel;
		}
		g_QglTable.m_pfn_qglColor4f( brightness, brightness, brightness, stage->alpha );
		if ( stage->alphaFunction != GL_ALWAYS ) {
			g_QglTable.m_pfn_qglEnable( GL_ALPHA_TEST );
			g_QglTable.m_pfn_qglAlphaFunc( stage->alphaFunction, stage->alphaReference );
		}
		if ( stage->environmentTexGen ) {
			// GL_SPHERE_MAP derives coordinates from the eye-space reflection
			// vector, so it needs both a surface normal and a surface that is
			// not in the eye plane.  The object normal is transformed by the
			// modelview, which is what makes the reflection track the material
			// as it is orbited in inspection mode.
			g_QglTable.m_pfn_qglNormal3f( 0.0f, 0.0f, 1.0f );
			g_QglTable.m_pfn_qglTexGenf( GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP );
			g_QglTable.m_pfn_qglTexGenf( GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP );
			g_QglTable.m_pfn_qglEnable( GL_TEXTURE_GEN_S );
			g_QglTable.m_pfn_qglEnable( GL_TEXTURE_GEN_T );
		}
		draw_stage_textured_surface( texture, *stage, left, bottom, right, top );
		if ( stage->environmentTexGen ) {
			g_QglTable.m_pfn_qglDisable( GL_TEXTURE_GEN_S );
			g_QglTable.m_pfn_qglDisable( GL_TEXTURE_GEN_T );
		}
		if ( stage->alphaFunction != GL_ALWAYS ) g_QglTable.m_pfn_qglDisable( GL_ALPHA_TEST );
		g_QglTable.m_pfn_qglColor4f( 1, 1, 1, 1 );
		drewStage = true;
	}

	if ( !drewStage && g_selectedTextureUploaded ) {
		g_QglTable.m_pfn_qglEnable( GL_BLEND );
		g_QglTable.m_pfn_qglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		draw_textured_quad( g_selectedTexture, left, bottom, right, top );
	}

	g_QglTable.m_pfn_qglDisable( GL_TEXTURE_2D );
}

static void render_preview(){
#if !GTK_CHECK_VERSION( 3, 0, 0 )
	if ( !g_UIGtkTable.m_pfn_glwidget_make_current( g_pPreviewWidget ) ) {
		g_FuncTable.m_pfnSysFPrintf( SYS_ERR, "ShaderShop: failed to activate OpenGL context\n" );
		return;
	}
#endif

	// GtkGLArea has already made its context current and attached its FBO before
	// it emits the GTK3 render signal. Re-entering the host helper here can
	// attach a different buffer during the callback; GTK2 still needs the
	// explicit GtkGLExt transition above.
	draw_preview();
	g_QglTable.m_pfn_QE_CheckOpenGLForErrors();
}

#if GTK_CHECK_VERSION( 3, 0, 0 )
static gboolean preview_idle_render( gpointer );

static gboolean preview_render( GtkGLArea*, GdkGLContext*, gpointer ){
	render_preview();
	return TRUE;
}

// GtkGLArea emits this when it creates or resizes its framebuffer, with the
// real buffer dimensions.  It is the earliest point at which the surface is
// genuinely drawable, which makes it the right trigger for the first frame.
static void preview_gl_resize( GtkGLArea*, gint width, gint height, gpointer ){
	g_previewBufferWidth = width;
	g_previewBufferHeight = height;
	g_previewSizeRetries = 0;
	queue_preview_render();
}

static void preview_realized( GtkWidget* widget, gpointer ){
	// GtkGLArea creates its context during realization. Queueing before that
	// point can be dropped, leaving the first frame waiting for unrelated UI
	// damage such as button hover.
	gtk_gl_area_queue_render( GTK_GL_AREA( widget ) );
	// Queue once more from the GTK idle queue: realization can happen before
	// allocation/map, in which case the first direct request is discarded.
	g_idle_add( preview_idle_render, NULL );
	watch_for_first_frame();
}

static gboolean preview_idle_render( gpointer ){
	if ( g_pPreviewWindow != NULL && g_pPreviewWidget != NULL ) {
		gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
	}
	return FALSE;
}

static gboolean preview_mapped_or_configured( GtkWidget*, GdkEvent*, gpointer ){
	// GtkGLArea can be realized before its first usable allocation.  An idle
	// request after map/configure is context-safe and avoids needing a resize or
	// pointer hover to damage the first frame.
	g_idle_add( preview_idle_render, NULL );
	watch_for_first_frame();
	return FALSE;
}
#else
static gboolean preview_mapped_or_configured( GtkWidget*, GdkEvent*, gpointer ){
	if ( g_pPreviewWidget != NULL ) gtk_widget_queue_draw( g_pPreviewWidget );
	return FALSE;
}

static gboolean preview_expose( GtkWidget*, GdkEventExpose* event, gpointer ){
	if ( event->count != 0 ) {
		return TRUE;
	}
	render_preview();
	g_UIGtkTable.m_pfn_glwidget_swap_buffers( g_pPreviewWidget );
	return TRUE;
}
#endif

static void clear_backdrop(){
	clear_stage_list( g_backdropStages );
	preview_source_clear( g_backdropImage );
	g_backdropName.clear();
}

// The control area carries two rows now, so the widgets are scaled down to keep
// the preview surface dominant.
static void preview_compact( GtkWidget* widget ){
#if GTK_CHECK_VERSION( 3, 0, 0 )
	static GtkCssProvider* provider = NULL;
	if ( provider == NULL ) {
		provider = gtk_css_provider_new();
		gtk_css_provider_load_from_data(
			provider,
			".shadershop-compact { font-size: 90%; }"
			".shadershop-compact button { padding: 1px 6px; min-height: 0; }"
			".shadershop-compact combobox button { padding: 1px 4px; }",
			-1, NULL
		);
	}
	gtk_style_context_add_provider(
		gtk_widget_get_style_context( widget ),
		GTK_STYLE_PROVIDER( provider ),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
	);
	gtk_style_context_add_class( gtk_widget_get_style_context( widget ), "shadershop-compact" );
#else
	(void)widget;
#endif
}

static void preview_destroyed( GtkWidget*, gpointer ){
	// The preview widget owns the GL context. Its destruction releases the GL
	// textures; CPU image data remains the plugin's responsibility.
	g_checkerboardTexture = 0;
	g_selectedTexture = 0;
	g_retiredTextures.clear();

	if ( g_animationTimer != 0 ) {
		g_source_remove( g_animationTimer );
		g_animationTimer = 0;
	}

	clear_stages();
	clear_selected_image();

	if ( g_previewFrameTimer != 0 ) {
		g_source_remove( g_previewFrameTimer );
		g_previewFrameTimer = 0;
	}

	g_previewBufferWidth = 0;
	g_previewBufferHeight = 0;
	g_previewSizeRetries = 0;
	g_previewFrameDrawn = false;
	g_previewFrameWaits = 0;

	g_pPreviewWidget = NULL;
	g_pSelectionLabel = NULL;
	g_stageLabel = NULL;
	g_animationButton = NULL;
	g_3dInspectButton = NULL;
	g_backdropButton = NULL;
	g_lightmapScale = NULL;
	clear_backdrop();
	g_pPreviewWindow = NULL;
}

// =============================================================================
// DRAFT STAGE EDITOR
//
// This deliberately edits the parsed preview representation only.  It gives
// authors an immediate, ordered compositing workbench without claiming that a
// lossy reconstruction is safe to write back to a .shader file.  Save arrives
// with the lossless ShaderDocument layer.

static void editor_set_status(){
	if ( g_editorStatusLabel == NULL ) return;
	char status[256];
	snprintf(
		status, sizeof( status ),
		"%d stage%s — %s draft; preview updates immediately, source files are untouched",
		static_cast<int>( g_stages.size() ), g_stages.size() == 1 ? "" : "s",
		g_editorDirty ? "modified" : "read-only"
	);
	gtk_label_set_text( GTK_LABEL( g_editorStatusLabel ), status );
}

static int editor_stage_index( GtkWidget* widget ){
	return GPOINTER_TO_INT( g_object_get_data( G_OBJECT( widget ), "shadershop-stage-index" ) );
}

static void editor_mark_dirty(){
	g_editorDirty = true;
	editor_set_status();
	queue_preview_render();
}

static GtkWidget* editor_blend_combo( const std::string& selected ){
	static const char* factors[] = {
		"GL_ZERO", "GL_ONE", "GL_SRC_COLOR", "GL_ONE_MINUS_SRC_COLOR",
		"GL_DST_COLOR", "GL_ONE_MINUS_DST_COLOR", "GL_SRC_ALPHA",
		"GL_ONE_MINUS_SRC_ALPHA", "GL_DST_ALPHA", "GL_ONE_MINUS_DST_ALPHA"
	};
	GtkWidget* combo = gtk_combo_box_text_new();
	int active = 0;
	for ( size_t i = 0; i < sizeof( factors ) / sizeof( factors[0] ); ++i ) {
		gtk_combo_box_text_append_text( GTK_COMBO_BOX_TEXT( combo ), factors[i] );
		if ( !strcasecmp( factors[i], selected.c_str() ) ) active = static_cast<int>( i );
	}
	gtk_combo_box_set_active( GTK_COMBO_BOX( combo ), active );
	return combo;
}

static const char* editor_blend_description( const PreviewStage& stage ){
	const GLenum source = blend_factor( stage.blendSrc );
	const GLenum destination = blend_factor( stage.blendDst );
	if ( source == GL_ONE && destination == GL_ZERO ) return "Compositing: Replace — this stage is the new destination";
	if ( source == GL_SRC_ALPHA && destination == GL_ONE_MINUS_SRC_ALPHA ) return "Compositing: Alpha blend — source alpha over the destination";
	if ( source == GL_ONE && destination == GL_ONE ) return "Compositing: Add — brightens by adding source and destination";
	if ( source == GL_SRC_ALPHA && destination == GL_ONE ) return "Compositing: Alpha add — adds the source through its alpha";
	if ( source == GL_DST_COLOR && destination == GL_ZERO ) return "Compositing: Multiply — source darkens or tints the destination";
	if ( ( source == GL_ONE && destination == GL_ONE_MINUS_SRC_COLOR ) ||
		 ( source == GL_ONE_MINUS_DST_COLOR && destination == GL_ONE ) ) return "Compositing: Screen — brightens while preserving highlights";
	if ( source == GL_ZERO && destination == GL_ONE_MINUS_SRC_COLOR ) return "Compositing: Inverse multiply — source darkens the destination by inverse color";
	if ( source == GL_ZERO && destination == GL_ONE ) return "Compositing: Destination only — this stage does not contribute visible color";
	return "Compositing: Custom OpenGL factors — inspect the source/destination pair";
}

static void editor_blend_changed( GtkComboBox* combo, gpointer destination ){
	if ( g_editorRebuilding ) return;
	const int index = editor_stage_index( GTK_WIDGET( combo ) );
	if ( index < 0 || index >= static_cast<int>( g_stages.size() ) ) return;
	gchar* value = gtk_combo_box_text_get_active_text( GTK_COMBO_BOX_TEXT( combo ) );
	if ( value == NULL ) return;
	if ( GPOINTER_TO_INT( destination ) == 0 ) g_stages[index].blendSrc = value;
	else g_stages[index].blendDst = value;
	g_free( value );
	GtkWidget* summary = static_cast<GtkWidget*>( g_object_get_data( G_OBJECT( combo ), "shadershop-blend-summary" ) );
	if ( summary != NULL ) gtk_label_set_text( GTK_LABEL( summary ), editor_blend_description( g_stages[index] ) );
	classify_destination_use();
	editor_mark_dirty();
}

static void editor_image_apply_clicked( GtkButton* button, gpointer ){
	const int index = editor_stage_index( GTK_WIDGET( button ) );
	GtkWidget* entry = static_cast<GtkWidget*>( g_object_get_data( G_OBJECT( button ), "shadershop-image-entry" ) );
	if ( entry == NULL || index < 0 || index >= static_cast<int>( g_stages.size() ) ) return;
	const char* name = gtk_entry_get_text( GTK_ENTRY( entry ) );
	PreviewStage& stage = g_stages[index];
	clear_stage_images( stage );
	stage.animationNames.clear();
	stage.animationFps = 0.0f;
	stage.mapName = name == NULL ? "" : name;
	if ( !stage.mapName.empty() && !load_image( stage.mapName.c_str(), &stage.pixels, &stage.width, &stage.height ) ) {
		shadershop_warn( "stage %d: image '%s' could not be loaded", index + 1, stage.mapName.c_str() );
	}
	editor_mark_dirty();
	rebuild_editor_stage_stack();
}

static void editor_animation_rate_changed( GtkSpinButton* spin, gpointer ){
	if ( g_editorRebuilding ) return;
	const int index = editor_stage_index( GTK_WIDGET( spin ) );
	if ( index < 0 || index >= static_cast<int>( g_stages.size() ) ) return;
	g_stages[index].animationFps = static_cast<float>( gtk_spin_button_get_value( spin ) );
	editor_mark_dirty();
}

static void editor_stage_remove_clicked( GtkButton* button, gpointer ){
	const int index = editor_stage_index( GTK_WIDGET( button ) );
	if ( index < 0 || index >= static_cast<int>( g_stages.size() ) ) return;
	clear_stage_images( g_stages[index] );
	g_stages.erase( g_stages.begin() + index );
	classify_destination_use();
	editor_mark_dirty();
	rebuild_editor_stage_stack();
}

static void editor_stage_add_clicked( GtkButton*, gpointer ){
	g_stages.push_back( PreviewStage() );
	classify_destination_use();
	editor_mark_dirty();
	rebuild_editor_stage_stack();
}

static GtkTargetEntry g_editorStageDragTargets[] = {
	{ const_cast<gchar*>( "application/x-shadershop-stage" ), GTK_TARGET_SAME_APP, 0 }
};

static void editor_clear_drop_marker(){
	if ( g_editorDropMarker != NULL ) gtk_widget_hide( g_editorDropMarker );
	g_editorDropMarker = NULL;
	g_editorDropIndex = -1;
	g_editorDropAfter = false;
}

static void editor_show_drop_marker( GtkWidget* slot, int index, bool after ){
	if ( g_editorDropMarker != NULL && g_editorDropIndex == index && g_editorDropAfter == after ) return;
	if ( g_editorDropMarker != NULL ) gtk_widget_hide( g_editorDropMarker );
	g_editorDropMarker = static_cast<GtkWidget*>( g_object_get_data( G_OBJECT( slot ), after ? "shadershop-drop-after" : "shadershop-drop-before" ) );
	g_editorDropIndex = index;
	g_editorDropAfter = after;
	if ( g_editorDropMarker != NULL ) gtk_widget_show( g_editorDropMarker );
}

static void editor_stage_drag_begin( GtkWidget* grip, GdkDragContext*, gpointer ){
	g_editorDragIndex = editor_stage_index( grip );
	editor_clear_drop_marker();
}

static void editor_stage_drag_end( GtkWidget*, GdkDragContext*, gpointer ){
	g_editorDragIndex = -1;
	editor_clear_drop_marker();
}

static void editor_stage_drag_data_get( GtkWidget* grip, GdkDragContext*, GtkSelectionData* selection, guint, guint, gpointer ){
	const int index = editor_stage_index( grip );
	const GdkAtom target = gdk_atom_intern( "application/x-shadershop-stage", FALSE );
	gtk_selection_data_set( selection, target, 8, reinterpret_cast<const guchar*>( &index ), sizeof( index ) );
}

static gboolean editor_stage_drag_motion( GtkWidget* slot, GdkDragContext* context, gint, gint y, guint time, gpointer ){
	GtkAllocation allocation;
	gtk_widget_get_allocation( slot, &allocation );
	editor_show_drop_marker( slot, editor_stage_index( slot ), y >= allocation.height / 2 );
	gdk_drag_status( context, GDK_ACTION_MOVE, time );
	return TRUE;
}

static void editor_stage_drag_leave( GtkWidget*, GdkDragContext*, guint, gpointer ){
	editor_clear_drop_marker();
}

static gboolean editor_stage_drag_drop( GtkWidget* slot, GdkDragContext* context, gint, gint y, guint time, gpointer ){
	GtkAllocation allocation;
	gtk_widget_get_allocation( slot, &allocation );
	editor_show_drop_marker( slot, editor_stage_index( slot ), y >= allocation.height / 2 );
	const GdkAtom target = gtk_drag_dest_find_target( slot, context, NULL );
	if ( target == GDK_NONE ) return FALSE;
	gtk_drag_get_data( slot, context, target, time );
	return TRUE;
}

static void editor_stage_drag_data_received( GtkWidget* slot, GdkDragContext* context, gint, gint, GtkSelectionData* selection, guint, guint time, gpointer ){
	int source = g_editorDragIndex;
	if ( gtk_selection_data_get_length( selection ) == static_cast<gint>( sizeof( source ) ) ) {
		memcpy( &source, gtk_selection_data_get_data( selection ), sizeof( source ) );
	}
	const int target = g_editorDropIndex >= 0 ? g_editorDropIndex : editor_stage_index( slot );
	int destination = target + ( g_editorDropAfter ? 1 : 0 );
	bool moved = false;
	if ( source >= 0 && source < static_cast<int>( g_stages.size() ) && destination >= 0 && destination <= static_cast<int>( g_stages.size() ) ) {
		if ( source < destination ) --destination;
		if ( destination != source ) {
			PreviewStage movedStage = g_stages[source];
			g_stages.erase( g_stages.begin() + source );
			g_stages.insert( g_stages.begin() + destination, movedStage );
			classify_destination_use();
			editor_mark_dirty();
			moved = true;
		}
	}
	gtk_drag_finish( context, moved, FALSE, time );
	editor_clear_drop_marker();
	if ( moved ) rebuild_editor_stage_stack();
}

static void editor_destroyed( GtkWidget*, gpointer ){
	g_pEditorWindow = NULL;
	g_editorStageStack = NULL;
	g_editorStatusLabel = NULL;
	editor_clear_drop_marker();
	g_editorDragIndex = -1;
}

static void editor_pack_row( GtkWidget* box, GtkWidget* child ){
	gtk_box_pack_start( GTK_BOX( box ), child, FALSE, FALSE, 0 );
	gtk_widget_show( child );
}

static void editor_append_stage( int index ){
	const PreviewStage& stage = g_stages[index];
	char title[128];
	snprintf( title, sizeof( title ), "Stage contents%s", stage_is_drawn( stage ) ? "" : " — no drawable image" );
	GtkWidget* slot = gtk_event_box_new();
	g_object_set_data( G_OBJECT( slot ), "shadershop-stage-index", GINT_TO_POINTER( index ) );
	gtk_drag_dest_set( slot, GTK_DEST_DEFAULT_ALL, g_editorStageDragTargets, 1, GDK_ACTION_MOVE );
	g_signal_connect( G_OBJECT( slot ), "drag-motion", G_CALLBACK( editor_stage_drag_motion ), NULL );
	g_signal_connect( G_OBJECT( slot ), "drag-leave", G_CALLBACK( editor_stage_drag_leave ), NULL );
	g_signal_connect( G_OBJECT( slot ), "drag-drop", G_CALLBACK( editor_stage_drag_drop ), NULL );
	g_signal_connect( G_OBJECT( slot ), "drag-data-received", G_CALLBACK( editor_stage_drag_data_received ), NULL );
	gtk_box_pack_start( GTK_BOX( g_editorStageStack ), slot, FALSE, FALSE, 0 );
	gtk_widget_show( slot );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	GtkWidget* slotRow = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 0 );
	GtkWidget* contentColumn = gtk_box_new( GTK_ORIENTATION_VERTICAL, 2 );
	GtkWidget* contentRow = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 4 );
	GtkWidget* body = gtk_box_new( GTK_ORIENTATION_VERTICAL, 4 );
	GtkWidget* imageRow = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 4 );
	GtkWidget* blendRow = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 4 );
	GtkWidget* animationRow = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 4 );
	GtkWidget* orderRow = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 4 );
#else
	GtkWidget* slotRow = gtk_hbox_new( FALSE, 0 );
	GtkWidget* contentColumn = gtk_vbox_new( FALSE, 2 );
	GtkWidget* contentRow = gtk_hbox_new( FALSE, 4 );
	GtkWidget* body = gtk_vbox_new( FALSE, 4 );
	GtkWidget* imageRow = gtk_hbox_new( FALSE, 4 );
	GtkWidget* blendRow = gtk_hbox_new( FALSE, 4 );
	GtkWidget* animationRow = gtk_hbox_new( FALSE, 4 );
	GtkWidget* orderRow = gtk_hbox_new( FALSE, 4 );
#endif
	gtk_container_add( GTK_CONTAINER( slot ), slotRow );
	gtk_widget_show( slotRow );
	GtkWidget* stageChrome = gtk_event_box_new();
	GdkColor darkChrome;
	darkChrome.red = 0x2020; darkChrome.green = 0x2424; darkChrome.blue = 0x2b2b;
	gtk_widget_modify_bg( stageChrome, GTK_STATE_NORMAL, &darkChrome );
	char number[16];
	snprintf( number, sizeof( number ), "%d", index + 1 );
	GtkWidget* numberLabel = gtk_label_new( number );
	GdkColor lightText;
	lightText.red = 0xd8d8; lightText.green = 0xd8d8; lightText.blue = 0xd8d8;
	gtk_widget_modify_fg( numberLabel, GTK_STATE_NORMAL, &lightText );
	gtk_widget_set_size_request( stageChrome, 36, -1 );
	gtk_container_add( GTK_CONTAINER( stageChrome ), numberLabel );
	gtk_box_pack_start( GTK_BOX( slotRow ), stageChrome, FALSE, FALSE, 0 );
	gtk_widget_show( numberLabel );
	gtk_widget_show( stageChrome );
	gtk_box_pack_start( GTK_BOX( slotRow ), contentColumn, TRUE, TRUE, 5 );
	gtk_widget_show( contentColumn );
	GtkWidget* beforeMarker = gtk_label_new( "Drop stage here" );
	gtk_widget_set_no_show_all( beforeMarker, TRUE );
	gtk_box_pack_start( GTK_BOX( contentColumn ), beforeMarker, FALSE, FALSE, 0 );
	g_object_set_data( G_OBJECT( slot ), "shadershop-drop-before", beforeMarker );
	gtk_box_pack_start( GTK_BOX( contentColumn ), contentRow, FALSE, FALSE, 0 );
	gtk_widget_show( contentRow );
	GtkWidget* grip = gtk_event_box_new();
	GtkWidget* gripLabel = gtk_label_new( "⋮\n⋮\n⋮" );
	gtk_container_add( GTK_CONTAINER( grip ), gripLabel );
	gtk_widget_set_tooltip_text( grip, "Drag this stage to the highlighted insertion point" );
	gtk_widget_set_size_request( grip, 24, -1 );
	g_object_set_data( G_OBJECT( grip ), "shadershop-stage-index", GINT_TO_POINTER( index ) );
	gtk_drag_source_set( grip, GDK_BUTTON1_MASK, g_editorStageDragTargets, 1, GDK_ACTION_MOVE );
	g_signal_connect( G_OBJECT( grip ), "drag-begin", G_CALLBACK( editor_stage_drag_begin ), NULL );
	g_signal_connect( G_OBJECT( grip ), "drag-end", G_CALLBACK( editor_stage_drag_end ), NULL );
	g_signal_connect( G_OBJECT( grip ), "drag-data-get", G_CALLBACK( editor_stage_drag_data_get ), NULL );
	gtk_box_pack_start( GTK_BOX( contentRow ), grip, FALSE, FALSE, 0 );
	gtk_widget_show( gripLabel );
	gtk_widget_show( grip );
	GtkWidget* frame = gtk_frame_new( title );
	gtk_frame_set_shadow_type( GTK_FRAME( frame ), GTK_SHADOW_ETCHED_IN );
	gtk_box_pack_start( GTK_BOX( contentRow ), frame, TRUE, TRUE, 0 );
	gtk_widget_show( frame );
	gtk_container_set_border_width( GTK_CONTAINER( body ), 5 );
	gtk_container_add( GTK_CONTAINER( frame ), body );
	gtk_widget_show( body );
	editor_pack_row( body, imageRow );
	editor_pack_row( body, blendRow );

	GtkWidget* imageLabel = gtk_label_new( "Image (VFS path):" );
	editor_pack_row( imageRow, imageLabel );
	GtkWidget* imageEntry = gtk_entry_new();
	gtk_entry_set_text( GTK_ENTRY( imageEntry ), stage.mapName.c_str() );
	gtk_widget_set_tooltip_text( imageEntry, "Use game-relative names such as textures/myset/image.tga; this does not write a file." );
	gtk_box_pack_start( GTK_BOX( imageRow ), imageEntry, TRUE, TRUE, 0 );
	gtk_widget_show( imageEntry );
	GtkWidget* load = gtk_button_new_with_label( "Load" );
	g_object_set_data( G_OBJECT( load ), "shadershop-stage-index", GINT_TO_POINTER( index ) );
	g_object_set_data( G_OBJECT( load ), "shadershop-image-entry", imageEntry );
	g_signal_connect( G_OBJECT( load ), "clicked", G_CALLBACK( editor_image_apply_clicked ), NULL );
	editor_pack_row( imageRow, load );

	editor_pack_row( blendRow, gtk_label_new( "Blend:" ) );
	GtkWidget* source = editor_blend_combo( stage.blendSrc );
	g_object_set_data( G_OBJECT( source ), "shadershop-stage-index", GINT_TO_POINTER( index ) );
	g_signal_connect( G_OBJECT( source ), "changed", G_CALLBACK( editor_blend_changed ), NULL );
	editor_pack_row( blendRow, source );
	editor_pack_row( blendRow, gtk_label_new( "→" ) );
	GtkWidget* destination = editor_blend_combo( stage.blendDst );
	g_object_set_data( G_OBJECT( destination ), "shadershop-stage-index", GINT_TO_POINTER( index ) );
	g_signal_connect( G_OBJECT( destination ), "changed", G_CALLBACK( editor_blend_changed ), GINT_TO_POINTER( 1 ) );
	editor_pack_row( blendRow, destination );
	GtkWidget* blendSummary = gtk_label_new( editor_blend_description( stage ) );
	gtk_label_set_line_wrap( GTK_LABEL( blendSummary ), TRUE );
	gtk_misc_set_alignment( GTK_MISC( blendSummary ), 0.0f, 0.5f );
	g_object_set_data( G_OBJECT( source ), "shadershop-blend-summary", blendSummary );
	g_object_set_data( G_OBJECT( destination ), "shadershop-blend-summary", blendSummary );
	editor_pack_row( body, blendSummary );
	editor_pack_row( body, animationRow );
	editor_pack_row( body, orderRow );

	char animationText[128];
	if ( stage.animationNames.size() > 1 ) snprintf( animationText, sizeof( animationText ), "Animation: %d frames at", static_cast<int>( stage.animationNames.size() ) );
	else snprintf( animationText, sizeof( animationText ), "Animation: static image" );
	editor_pack_row( animationRow, gtk_label_new( animationText ) );
	GtkWidget* rate = gtk_spin_button_new_with_range( 0.0, 60.0, 0.1 );
	gtk_spin_button_set_value( GTK_SPIN_BUTTON( rate ), stage.animationFps );
	gtk_widget_set_sensitive( rate, stage.animationNames.size() > 1 );
	gtk_widget_set_tooltip_text( rate, "animMap frames per second. Frame-list editing will be added with the source document." );
	g_object_set_data( G_OBJECT( rate ), "shadershop-stage-index", GINT_TO_POINTER( index ) );
	g_signal_connect( G_OBJECT( rate ), "value-changed", G_CALLBACK( editor_animation_rate_changed ), NULL );
	editor_pack_row( animationRow, rate );
	editor_pack_row( animationRow, gtk_label_new( "fps" ) );

	editor_pack_row( orderRow, gtk_label_new( "Reorder using the grip at left." ) );
	GtkWidget* remove = gtk_button_new_with_label( "Remove Stage" );
	g_object_set_data( G_OBJECT( remove ), "shadershop-stage-index", GINT_TO_POINTER( index ) );
	g_signal_connect( G_OBJECT( remove ), "clicked", G_CALLBACK( editor_stage_remove_clicked ), NULL );
	editor_pack_row( orderRow, remove );
	GtkWidget* afterMarker = gtk_label_new( "Drop stage here" );
	gtk_widget_set_no_show_all( afterMarker, TRUE );
	gtk_box_pack_start( GTK_BOX( contentColumn ), afterMarker, FALSE, FALSE, 0 );
	g_object_set_data( G_OBJECT( slot ), "shadershop-drop-after", afterMarker );
}

static void rebuild_editor_stage_stack(){
	if ( g_editorStageStack == NULL ) return;
	g_editorRebuilding = true;
	GList* children = gtk_container_get_children( GTK_CONTAINER( g_editorStageStack ) );
	for ( GList* child = children; child != NULL; child = child->next ) gtk_widget_destroy( GTK_WIDGET( child->data ) );
	g_list_free( children );
	for ( int index = 0; index < static_cast<int>( g_stages.size() ); ++index ) editor_append_stage( index );
	GtkWidget* add = gtk_button_new_with_label( "Add Stage" );
	g_signal_connect( G_OBJECT( add ), "clicked", G_CALLBACK( editor_stage_add_clicked ), NULL );
	gtk_box_pack_start( GTK_BOX( g_editorStageStack ), add, FALSE, FALSE, 4 );
	gtk_widget_show( add );
	g_editorRebuilding = false;
	editor_set_status();
}

static void edit_shader_clicked( GtkButton*, gpointer ){
	if ( g_pEditorWindow != NULL ) {
		rebuild_editor_stage_stack();
		gtk_window_present( GTK_WINDOW( g_pEditorWindow ) );
		return;
	}
	g_pEditorWindow = gtk_window_new( GTK_WINDOW_TOPLEVEL );
	gtk_window_set_title( GTK_WINDOW( g_pEditorWindow ), "ShaderShop — Stage Editor" );
	gtk_window_set_default_size( GTK_WINDOW( g_pEditorWindow ), 560, 560 );
	if ( g_pPreviewWindow != NULL ) gtk_window_set_transient_for( GTK_WINDOW( g_pEditorWindow ), GTK_WINDOW( g_pPreviewWindow ) );
	g_signal_connect( G_OBJECT( g_pEditorWindow ), "destroy", G_CALLBACK( editor_destroyed ), NULL );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	GtkWidget* box = gtk_box_new( GTK_ORIENTATION_VERTICAL, 6 );
#else
	GtkWidget* box = gtk_vbox_new( FALSE, 6 );
#endif
	gtk_container_set_border_width( GTK_CONTAINER( box ), 6 );
	gtk_container_add( GTK_CONTAINER( g_pEditorWindow ), box );
	gtk_widget_show( box );
	GtkWidget* heading = gtk_label_new( "Ordered shader stages — the dark number gutter is a fixed position. Drag a stage by its grip to the highlighted insertion point." );
	gtk_label_set_line_wrap( GTK_LABEL( heading ), TRUE );
	gtk_misc_set_alignment( GTK_MISC( heading ), 0.0f, 0.5f );
	gtk_box_pack_start( GTK_BOX( box ), heading, FALSE, FALSE, 0 );
	gtk_widget_show( heading );
	GtkWidget* scroll = gtk_scrolled_window_new( NULL, NULL );
	gtk_scrolled_window_set_policy( GTK_SCROLLED_WINDOW( scroll ), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC );
	gtk_box_pack_start( GTK_BOX( box ), scroll, TRUE, TRUE, 0 );
	gtk_widget_show( scroll );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	g_editorStageStack = gtk_box_new( GTK_ORIENTATION_VERTICAL, 6 );
#else
	g_editorStageStack = gtk_vbox_new( FALSE, 6 );
#endif
	gtk_container_set_border_width( GTK_CONTAINER( g_editorStageStack ), 4 );
	gtk_scrolled_window_add_with_viewport( GTK_SCROLLED_WINDOW( scroll ), g_editorStageStack );
	gtk_widget_show( g_editorStageStack );
	g_editorStatusLabel = gtk_label_new( "" );
	gtk_label_set_line_wrap( GTK_LABEL( g_editorStatusLabel ), TRUE );
	gtk_misc_set_alignment( GTK_MISC( g_editorStatusLabel ), 0.0f, 0.5f );
	gtk_box_pack_start( GTK_BOX( box ), g_editorStatusLabel, FALSE, FALSE, 0 );
	gtk_widget_show( g_editorStatusLabel );
	rebuild_editor_stage_stack();
	gtk_widget_show( g_pEditorWindow );
	gtk_window_present( GTK_WINDOW( g_pEditorWindow ) );
}

static gboolean refresh_selection_idle( gpointer ){
	ShaderShop_RefreshSelection();
	return FALSE;
}

static gboolean activate_and_refresh_idle( gpointer eventTime ){
	if ( g_pPreviewWindow == NULL ) {
		return FALSE;
	}

	const guint32 timestamp = GPOINTER_TO_UINT( eventTime );
	// The initial present() happens before the X11 toplevel is mapped. Repeat
	// the user-initiated activation once GTK has returned to its event loop,
	// using the menu event's timestamp so the window manager accepts it as a
	// legitimate focus request rather than focus stealing.
	gtk_window_present_with_time( GTK_WINDOW( g_pPreviewWindow ), timestamp );
	GdkWindow* window = gtk_widget_get_window( g_pPreviewWindow );
	if ( window != NULL ) {
		gdk_window_focus( window, timestamp );
	}

	// Load only after activation; the active toplevel owns the frame clock that
	// will present the GtkGLArea buffer.
	ShaderShop_RefreshSelection();
	return FALSE;
}

static void refresh_selection_clicked( GtkButton*, gpointer ){
	// Let the texture window finish its focus/selection update first. This
	// avoids requiring a second click when ShaderShop already owns focus.
	g_idle_add( refresh_selection_idle, NULL );
}

static void inspect_3d_toggled( GtkToggleButton* button, gpointer ){
	g_3dInspect = gtk_toggle_button_get_active( button ) != FALSE;
	queue_preview_render();
}

// Present the definitions found in a shader file and return the chosen one.
// A single-definition file needs no dialog; a file with many is the common case
// and choosing from it is the point of the loader.
static bool choose_definition( const std::vector<std::string>& names, std::string& chosen ){
	if ( names.empty() ) {
		return false;
	}
	if ( names.size() == 1 ) {
		chosen = names[0];
		return true;
	}

	GtkWidget* dialog = gtk_dialog_new_with_buttons(
		"Choose shader", GTK_WINDOW( g_pPreviewWindow ), GTK_DIALOG_MODAL,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Use", GTK_RESPONSE_ACCEPT, (const char*)NULL
	);
	gtk_window_set_default_size( GTK_WINDOW( dialog ), 420, 380 );

	GtkWidget* scroll = gtk_scrolled_window_new( NULL, NULL );
	gtk_scrolled_window_set_policy( GTK_SCROLLED_WINDOW( scroll ), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC );

	GtkListStore* store = gtk_list_store_new( 1, G_TYPE_STRING );
	for ( std::vector<std::string>::const_iterator name = names.begin(); name != names.end(); ++name ) {
		GtkTreeIter iter;
		gtk_list_store_append( store, &iter );
		gtk_list_store_set( store, &iter, 0, name->c_str(), -1 );
	}
	GtkWidget* view = gtk_tree_view_new_with_model( GTK_TREE_MODEL( store ) );
	g_object_unref( store );
	gtk_tree_view_append_column(
		GTK_TREE_VIEW( view ),
		gtk_tree_view_column_new_with_attributes( "Shader", gtk_cell_renderer_text_new(), "text", 0, (const char*)NULL )
	);
	gtk_container_add( GTK_CONTAINER( scroll ), view );
	gtk_box_pack_start( GTK_BOX( gtk_dialog_get_content_area( GTK_DIALOG( dialog ) ) ), scroll, TRUE, TRUE, 0 );
	gtk_widget_show_all( scroll );

	bool picked = false;
	if ( gtk_dialog_run( GTK_DIALOG( dialog ) ) == GTK_RESPONSE_ACCEPT ) {
		GtkTreeModel* model = NULL;
		GtkTreeIter iter;
		if ( gtk_tree_selection_get_selected( gtk_tree_view_get_selection( GTK_TREE_VIEW( view ) ), &model, &iter ) ) {
			char* value = NULL;
			gtk_tree_model_get( model, &iter, 0, &value, -1 );
			if ( value != NULL ) {
				chosen = value;
				g_free( value );
				picked = true;
			}
		}
	}
	gtk_widget_destroy( dialog );
	return picked;
}

// Load a shader definition out of a .shader source and use its stack as the
// backdrop.  `vfsName` is the VFS-relative source path the image manager and
// LoadFile both understand.
static bool load_backdrop_shader( const char* vfsName ){
	void* buffer = NULL;
	const int size = g_FuncTable.m_pfnLoadFile( vfsName, &buffer );
	if ( size <= 0 || buffer == NULL ) {
		return false;
	}

	std::vector<std::string> names;
	enumerate_definitions( static_cast<char*>( buffer ), names );

	std::string chosen;
	bool ok = false;
	if ( choose_definition( names, chosen ) ) {
		clear_backdrop();
		parse_definition_into( static_cast<char*>( buffer ), chosen.c_str(), vfsName, g_backdropStages, NULL, NULL, NULL, NULL, NULL );
		load_stage_images( g_backdropStages, false );
		ok = !g_backdropStages.empty();
		if ( ok ) {
			g_backdropName = chosen;
		}
		else {
			g_FuncTable.m_pfnSysFPrintf( SYS_WRN, "ShaderShop: backdrop shader '%s' has no drawable stages\n", chosen.c_str() );
		}
	}

	g_free( buffer );
	if ( g_ScripLibTable.m_pfnStartTokenParsing != NULL ) {
		static char emptyScript[1] = { '\0' };
		g_ScripLibTable.m_pfnStartTokenParsing( emptyScript );
	}
	return ok;
}

// Radiant already holds every shader loaded alongside the current map, so a
// backdrop can be chosen from that list rather than by hunting for the .shader
// file it came from. This also sidesteps the PK3 problem entirely: a shader
// inside a pak is in the active list like any other.
static bool load_backdrop_from_active_shader( const char* shaderName ){
	if ( g_ShadersTable.m_pfnShader_ForName_NoLoad == NULL || g_FuncTable.m_pfnLoadFile == NULL ) {
		return false;
	}
	IShader* shader = g_ShadersTable.m_pfnShader_ForName_NoLoad( shaderName );
	if ( shader == NULL || shader->getShaderFileName() == NULL || shader->getShaderFileName()[0] == '\0' ) {
		// A shader with no source file is a plain image; load it as one.
		clear_backdrop();
		if ( preview_source_load( g_backdropImage, shaderName ) ) {
			g_backdropName = shaderName;
			return true;
		}
		return false;
	}

	void* buffer = NULL;
	const int size = g_FuncTable.m_pfnLoadFile( shader->getShaderFileName(), &buffer );
	if ( size <= 0 || buffer == NULL ) {
		return false;
	}

	clear_backdrop();
	parse_definition_into( static_cast<char*>( buffer ), shaderName, shader->getShaderFileName(), g_backdropStages, NULL, NULL, NULL, NULL, NULL );
	load_stage_images( g_backdropStages, false );
	const bool ok = !g_backdropStages.empty();
	if ( ok ) {
		g_backdropName = shaderName;
	}

	g_free( buffer );
	if ( g_ScripLibTable.m_pfnStartTokenParsing != NULL ) {
		static char emptyScript[1] = { '\0' };
		g_ScripLibTable.m_pfnStartTokenParsing( emptyScript );
	}
	return ok;
}

static void backdrop_shader_selected( GtkMenuItem* item, gpointer ){
	const char* name = static_cast<const char*>( g_object_get_data( G_OBJECT( item ), "shadershop-name" ) );
	if ( name == NULL ) {
		return;
	}
	if ( load_backdrop_from_active_shader( name ) ) {
		g_backdropMode = BACKDROP_MODE_IMAGE;
	}
	else {
		g_FuncTable.m_pfnSysFPrintf( SYS_WRN, "ShaderShop: could not use '%s' as a backdrop\n", name );
	}
	ShaderShop_RefreshSelection();
}

// Group by the directory part of the shader name.  The active list runs to
// thousands of entries, so one flat menu would be unusable; the grouping is the
// same one the texture browser presents.
static void append_shader_items( GtkWidget* menu, const std::vector<std::string>& names ){
	std::string currentGroup;
	GtkWidget* groupMenu = NULL;
	for ( std::vector<std::string>::const_iterator name = names.begin(); name != names.end(); ++name ) {
		const std::string::size_type slash = name->find_last_of( '/' );
		const std::string group = slash == std::string::npos ? std::string( "(top level)" ) : name->substr( 0, slash );
		const std::string leaf = slash == std::string::npos ? *name : name->substr( slash + 1 );

		if ( groupMenu == NULL || group != currentGroup ) {
			currentGroup = group;
			groupMenu = gtk_menu_new();
			GtkWidget* groupItem = gtk_menu_item_new_with_label( group.c_str() );
			gtk_menu_item_set_submenu( GTK_MENU_ITEM( groupItem ), groupMenu );
			gtk_menu_shell_append( GTK_MENU_SHELL( menu ), groupItem );
			gtk_widget_show( groupItem );
		}

		GtkWidget* item = gtk_menu_item_new_with_label( leaf.c_str() );
		g_object_set_data_full( G_OBJECT( item ), "shadershop-name", g_strdup( name->c_str() ), g_free );
		g_signal_connect( G_OBJECT( item ), "activate", G_CALLBACK( backdrop_shader_selected ), NULL );
		gtk_menu_shell_append( GTK_MENU_SHELL( groupMenu ), item );
		gtk_widget_show( item );
	}
}

static bool name_has_extension( const std::string& name, const char* extension ){
	const std::string::size_type dot = name.find_last_of( '.' );
	return dot != std::string::npos && !strcasecmp( name.c_str() + dot, extension );
}

static void backdrop_choose_clicked( GtkButton*, gpointer ){
	GtkWidget* chooser = gtk_file_chooser_dialog_new(
		"Choose backdrop image", GTK_WINDOW( g_pPreviewWindow ), GTK_FILE_CHOOSER_ACTION_OPEN,
		"_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, (const char*)NULL
	);
	if ( g_FuncTable.m_pfnGetGamePath != NULL ) {
		const char* base = g_FuncTable.m_pfnGetGamePath();
		if ( base != NULL && base[0] != '\0' ) {
			gtk_file_chooser_set_current_folder( GTK_FILE_CHOOSER( chooser ), base );
		}
	}

	GtkFileFilter* any = gtk_file_filter_new();
	gtk_file_filter_set_name( any, "Shader scripts and images" );
	gtk_file_filter_add_pattern( any, "*.shader" );
	gtk_file_filter_add_pattern( any, "*.tga" );
	gtk_file_filter_add_pattern( any, "*.jpg" );
	gtk_file_filter_add_pattern( any, "*.png" );
	gtk_file_chooser_add_filter( GTK_FILE_CHOOSER( chooser ), any );

	GtkFileFilter* scripts = gtk_file_filter_new();
	gtk_file_filter_set_name( scripts, "Shader scripts (*.shader)" );
	gtk_file_filter_add_pattern( scripts, "*.shader" );
	gtk_file_chooser_add_filter( GTK_FILE_CHOOSER( chooser ), scripts );

	if ( gtk_dialog_run( GTK_DIALOG( chooser ) ) == GTK_RESPONSE_ACCEPT ) {
		char* filename = gtk_file_chooser_get_filename( GTK_FILE_CHOOSER( chooser ) );
		if ( filename != NULL ) {
			// Prefer the VFS-relative name so the image manager and LoadFile
			// resolve it the way a shader reference would; fall back to the
			// literal path and report if neither reaches anything.
			std::string relative;
			const bool haveRelative = preview_source_relative_name( filename, relative );
			const std::string target = haveRelative ? relative : std::string( filename );
			bool loaded = false;

			if ( name_has_extension( target, ".shader" ) ) {
				loaded = load_backdrop_shader( target.c_str() );
			}
			else {
				clear_backdrop();
				loaded = preview_source_load( g_backdropImage, target.c_str() );
				if ( loaded ) {
					g_backdropName = target;
				}
			}
			if ( loaded ) {
				g_backdropMode = BACKDROP_MODE_IMAGE;
			}
			else {
				g_FuncTable.m_pfnSysFPrintf( SYS_WRN, "ShaderShop: could not load backdrop '%s'\n", filename );
			}
			g_free( filename );
		}
	}
	gtk_widget_destroy( chooser );
	ShaderShop_RefreshSelection();
}

static void backdrop_mode_selected( GtkMenuItem* item, gpointer ){
	const gpointer mode = g_object_get_data( G_OBJECT( item ), "shadershop-mode" );
	g_backdropMode = static_cast<BackdropMode>( GPOINTER_TO_INT( mode ) );
	ShaderShop_RefreshSelection();
}

static void backdrop_menu_clicked( GtkButton* button, gpointer ){
	GtkWidget* menu = gtk_menu_new();

	static const char* modeLabels[] = { "Auto", "Checkerboard", "Lightmap", "Dark" };
	for ( int i = 0; i < 4; ++i ) {
		GtkWidget* item = gtk_menu_item_new_with_label( modeLabels[i] );
		g_object_set_data( G_OBJECT( item ), "shadershop-mode", GINT_TO_POINTER( i ) );
		g_signal_connect( G_OBJECT( item ), "activate", G_CALLBACK( backdrop_mode_selected ), NULL );
		gtk_menu_shell_append( GTK_MENU_SHELL( menu ), item );
		gtk_widget_show( item );
	}

	GtkWidget* separator = gtk_separator_menu_item_new();
	gtk_menu_shell_append( GTK_MENU_SHELL( menu ), separator );
	gtk_widget_show( separator );

	// Shaders already loaded with the current map. Split so the handful actually
	// used by the map is reachable without walking the whole texture set.
	std::vector<std::string> inUse;
	std::vector<std::string> loaded;
	if ( g_ShadersTable.m_pfnGetActiveShaderCount != NULL && g_ShadersTable.m_pfnActiveShader_ForIndex != NULL ) {
		const int count = g_ShadersTable.m_pfnGetActiveShaderCount();
		for ( int i = 0; i < count; ++i ) {
			IShader* shader = g_ShadersTable.m_pfnActiveShader_ForIndex( i );
			if ( shader == NULL || shader->getName() == NULL || shader->getName()[0] == '\0' ) {
				continue;
			}
			if ( shader->IsInUse() ) {
				inUse.push_back( shader->getName() );
			}
			loaded.push_back( shader->getName() );
		}
	}

	if ( !inUse.empty() ) {
		GtkWidget* item = gtk_menu_item_new_with_label( "Used by this map" );
		GtkWidget* sub = gtk_menu_new();
		gtk_menu_item_set_submenu( GTK_MENU_ITEM( item ), sub );
		append_shader_items( sub, inUse );
		gtk_menu_shell_append( GTK_MENU_SHELL( menu ), item );
		gtk_widget_show( item );
	}

	if ( !loaded.empty() ) {
		GtkWidget* item = gtk_menu_item_new_with_label( "All loaded shaders" );
		GtkWidget* sub = gtk_menu_new();
		gtk_menu_item_set_submenu( GTK_MENU_ITEM( item ), sub );
		append_shader_items( sub, loaded );
		gtk_menu_shell_append( GTK_MENU_SHELL( menu ), item );
		gtk_widget_show( item );
	}
	else {
		GtkWidget* item = gtk_menu_item_new_with_label( "No shaders loaded" );
		gtk_widget_set_sensitive( item, FALSE );
		gtk_menu_shell_append( GTK_MENU_SHELL( menu ), item );
		gtk_widget_show( item );
	}

	GtkWidget* separator2 = gtk_separator_menu_item_new();
	gtk_menu_shell_append( GTK_MENU_SHELL( menu ), separator2 );
	gtk_widget_show( separator2 );

	GtkWidget* fromFile = gtk_menu_item_new_with_label( "From file..." );
	g_signal_connect( G_OBJECT( fromFile ), "activate", G_CALLBACK( backdrop_choose_clicked ), NULL );
	gtk_menu_shell_append( GTK_MENU_SHELL( menu ), fromFile );
	gtk_widget_show( fromFile );

#if GTK_CHECK_VERSION( 3, 22, 0 )
	gtk_menu_popup_at_widget( GTK_MENU( menu ), GTK_WIDGET( button ), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL );
#else
	gtk_menu_popup( GTK_MENU( menu ), NULL, NULL, NULL, NULL, 0, gtk_get_current_event_time() );
#endif
}


static void lightmap_level_changed( GtkRange* range, gpointer ){
	g_lightmapLevel = static_cast<float>( gtk_range_get_value( range ) );
	queue_preview_render();
}

static void create_shader_clicked( GtkButton*, gpointer ){
	g_FuncTable.m_pfnMessageBox(
		g_pPreviewWindow,
		"Creating a new shader needs the source document layer, which is not built yet.\n\n"
		"It will author a loose scripts/*.shader file rather than modifying any PK3.",
		"ShaderShop",
		MB_OK,
		NULL
	);
}

static gboolean preview_button_press( GtkWidget*, GdkEventButton* event, gpointer ){
	if ( !g_3dInspect || ( event->button != 1 && event->button != 2 && event->button != 3 ) ) {
		return FALSE;
	}
	g_pointerX = event->x;
	g_pointerY = event->y;
	return TRUE;
}

static gboolean preview_motion( GtkWidget* widget, GdkEventMotion* event, gpointer ){
	if ( !g_3dInspect || ( event->state & ( GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK ) ) == 0 ) {
		return FALSE;
	}
	const double dx = event->x - g_pointerX;
	const double dy = event->y - g_pointerY;
	g_pointerX = event->x;
	g_pointerY = event->y;
	if ( event->state & GDK_BUTTON1_MASK ) {
		g_inspectYaw += static_cast<float>( dx ) * 0.5f;
		g_inspectPitch += static_cast<float>( dy ) * 0.5f;
		g_inspectPitch = std::max( -89.0f, std::min( 89.0f, g_inspectPitch ) );
	}
	else {
		// Convert pointer pixels to world units through the current view extents
		// so a drag tracks the cursor at any zoom.
		const int pixelWidth = std::max( 1, gtkutil_widget_get_width( widget ) );
		const int pixelHeight = std::max( 1, gtkutil_widget_get_height( widget ) );
		g_inspectPanX += static_cast<float>( dx ) * 2.0f * g_viewHalfX / pixelWidth;
		g_inspectPanY -= static_cast<float>( dy ) * 2.0f * g_viewHalfY / pixelHeight;
	}
	queue_preview_render();
	return TRUE;
}

static gboolean preview_scroll( GtkWidget*, GdkEventScroll* event, gpointer ){
	if ( !g_3dInspect ) {
		return FALSE;
	}
	if ( event->direction == GDK_SCROLL_UP ) g_inspectZoom *= 1.12f;
	else if ( event->direction == GDK_SCROLL_DOWN ) g_inspectZoom /= 1.12f;
	else return FALSE;
	g_inspectZoom = std::max( 0.15f, std::min( 5.0f, g_inspectZoom ) );
	queue_preview_render();
	return TRUE;
}

void ShaderShop_Show(){
	if ( g_pPreviewWindow != NULL ) {
		ShaderShop_RefreshSelection();
		gtk_window_present( GTK_WINDOW( g_pPreviewWindow ) );
		return;
	}

	// The preview is a 2D material swatch; inspection is something the user asks
	// for about a particular shader, not a mode the window should remember.
	g_3dInspect = false;
	g_inspectYaw = 28.0f;
	g_inspectPitch = -24.0f;
	g_inspectPanX = 0.0f;
	g_inspectPanY = 0.0f;
	g_inspectZoom = 1.0f;

	const guint32 activationTime = gtk_get_current_event_time();
	g_pPreviewWindow = gtk_window_new( GTK_WINDOW_TOPLEVEL );
	gtk_window_set_title( GTK_WINDOW( g_pPreviewWindow ), "ShaderShop" );
	gtk_window_set_default_size( GTK_WINDOW( g_pPreviewWindow ), 640, 500 );
	if ( g_pMainWidget != NULL ) {
		gtk_window_set_transient_for( GTK_WINDOW( g_pPreviewWindow ), GTK_WINDOW( g_pMainWidget ) );
	}
	g_signal_connect( G_OBJECT( g_pPreviewWindow ), "destroy", G_CALLBACK( preview_destroyed ), NULL );

#if GTK_CHECK_VERSION( 3, 0, 0 )
	GtkWidget* box = gtk_box_new( GTK_ORIENTATION_VERTICAL, 6 );
#else
	GtkWidget* box = gtk_vbox_new( FALSE, 6 );
#endif
	gtk_container_set_border_width( GTK_CONTAINER( box ), 6 );
	gtk_container_add( GTK_CONTAINER( g_pPreviewWindow ), box );
	gtk_widget_show( box );

	GtkWidget* frame = gtk_frame_new( NULL );
	gtk_frame_set_shadow_type( GTK_FRAME( frame ), GTK_SHADOW_IN );
	gtk_box_pack_start( GTK_BOX( box ), frame, TRUE, TRUE, 0 );
	gtk_widget_show( frame );

	g_pPreviewWidget = g_UIGtkTable.m_pfn_glwidget_new( FALSE, NULL );
	gtk_widget_set_events( g_pPreviewWidget, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "render", G_CALLBACK( preview_render ), NULL );
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "realize", G_CALLBACK( preview_realized ), NULL );
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "resize", G_CALLBACK( preview_gl_resize ), NULL );
#else
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "expose-event", G_CALLBACK( preview_expose ), NULL );
#endif
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "button-press-event", G_CALLBACK( preview_button_press ), NULL );
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "motion-notify-event", G_CALLBACK( preview_motion ), NULL );
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "scroll-event", G_CALLBACK( preview_scroll ), NULL );
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "map-event", G_CALLBACK( preview_mapped_or_configured ), NULL );
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "configure-event", G_CALLBACK( preview_mapped_or_configured ), NULL );
	gtk_container_add( GTK_CONTAINER( frame ), g_pPreviewWidget );
	gtk_widget_set_hexpand( g_pPreviewWidget, TRUE );
	gtk_widget_set_vexpand( g_pPreviewWidget, TRUE );
	gtk_widget_show( g_pPreviewWidget );

	// Row one reports what is being shown; row two is where it is controlled.
#if GTK_CHECK_VERSION( 3, 0, 0 )
	GtkWidget* statusRow = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
	GtkWidget* controls = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 4 );
#else
	GtkWidget* statusRow = gtk_hbox_new( FALSE, 6 );
	GtkWidget* controls = gtk_hbox_new( FALSE, 4 );
#endif
	gtk_box_pack_start( GTK_BOX( box ), statusRow, FALSE, FALSE, 0 );
	gtk_box_pack_start( GTK_BOX( box ), controls, FALSE, FALSE, 0 );
	preview_compact( statusRow );
	preview_compact( controls );
	gtk_widget_show( statusRow );
	gtk_widget_show( controls );

	g_pSelectionLabel = gtk_label_new( "No current shader selected" );
	gtk_box_pack_start( GTK_BOX( statusRow ), g_pSelectionLabel, TRUE, TRUE, 0 );
	gtk_widget_show( g_pSelectionLabel );

	g_stageLabel = gtk_label_new( "Parsed stages: 0" );
	gtk_box_pack_end( GTK_BOX( statusRow ), g_stageLabel, FALSE, FALSE, 0 );
	gtk_widget_show( g_stageLabel );

	// --- preview controls ---------------------------------------------------
	GtkWidget* backdropLabel = gtk_label_new( "Backdrop:" );
	gtk_box_pack_start( GTK_BOX( controls ), backdropLabel, FALSE, FALSE, 0 );
	gtk_widget_show( backdropLabel );

	g_backdropButton = gtk_button_new_with_label( "Auto" );
	gtk_widget_set_tooltip_text(
		g_backdropButton,
		"What the stage stack is composited over. Auto picks a uniform field when "
		"the stack reads its destination, so the checkerboard is never blended in. "
		"Any shader loaded with the current map can be used instead."
	);
	g_signal_connect( G_OBJECT( g_backdropButton ), "clicked", G_CALLBACK( backdrop_menu_clicked ), NULL );
	gtk_box_pack_start( GTK_BOX( controls ), g_backdropButton, FALSE, FALSE, 0 );
	gtk_widget_show( g_backdropButton );

	GtkWidget* lightmapLabel = gtk_label_new( "Lightmap:" );
	gtk_box_pack_start( GTK_BOX( controls ), lightmapLabel, FALSE, FALSE, 0 );
	gtk_widget_show( lightmapLabel );

#if GTK_CHECK_VERSION( 3, 0, 0 )
	g_lightmapScale = gtk_scale_new_with_range( GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01 );
#else
	g_lightmapScale = gtk_hscale_new_with_range( 0.0, 1.0, 0.01 );
#endif
	gtk_scale_set_draw_value( GTK_SCALE( g_lightmapScale ), FALSE );
	gtk_range_set_value( GTK_RANGE( g_lightmapScale ), g_lightmapLevel );
	gtk_widget_set_size_request( g_lightmapScale, 90, -1 );
	gtk_widget_set_tooltip_text(
		g_lightmapScale,
		"How lit the surface is taken to be: drives both the map $lightmap "
		"stand-in and the lightmap backdrop. Neither has a value outside a "
		"compiled map, so this is a preview control, not shader content."
	);
	g_signal_connect( G_OBJECT( g_lightmapScale ), "value-changed", G_CALLBACK( lightmap_level_changed ), NULL );
	gtk_box_pack_start( GTK_BOX( controls ), g_lightmapScale, FALSE, FALSE, 0 );
	gtk_widget_show( g_lightmapScale );

	g_animationButton = gtk_button_new_with_label( "Pause" );
	g_signal_connect( G_OBJECT( g_animationButton ), "clicked", G_CALLBACK( animation_clicked ), NULL );
	gtk_box_pack_start( GTK_BOX( controls ), g_animationButton, FALSE, FALSE, 0 );
	gtk_widget_show( g_animationButton );

	g_3dInspectButton = gtk_toggle_button_new_with_label( "3D" );
	gtk_widget_set_tooltip_text( g_3dInspectButton, "Left drag: orbit; right or middle drag: pan; wheel: zoom" );
	g_signal_connect( G_OBJECT( g_3dInspectButton ), "toggled", G_CALLBACK( inspect_3d_toggled ), NULL );
	gtk_box_pack_start( GTK_BOX( controls ), g_3dInspectButton, FALSE, FALSE, 0 );
	gtk_widget_show( g_3dInspectButton );

	// --- document actions ---------------------------------------------------
	GtkWidget* edit = gtk_button_new_with_label( "Edit Shader..." );
	g_signal_connect( G_OBJECT( edit ), "clicked", G_CALLBACK( edit_shader_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), edit, FALSE, FALSE, 0 );
	gtk_widget_show( edit );

	GtkWidget* create = gtk_button_new_with_label( "Create Shader..." );
	g_signal_connect( G_OBJECT( create ), "clicked", G_CALLBACK( create_shader_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), create, FALSE, FALSE, 0 );
	gtk_widget_show( create );

	GtkWidget* refresh = gtk_button_new_with_label( "Use Current Shader" );
	g_signal_connect( G_OBJECT( refresh ), "clicked", G_CALLBACK( refresh_selection_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), refresh, FALSE, FALSE, 0 );
	gtk_widget_show( refresh );

	gtk_widget_show( g_pPreviewWindow );
	// gtk_widget_show() does not guarantee the window becomes key on the
	// quartz backend when it is transient for another window; present() is
	// the same call the "already open" branch above uses to raise and focus
	// it, and a not-yet-key window's frame clock is the mechanism behind
	// the blank-until-clicked symptom this plugin has been chasing.
	gtk_window_present( GTK_WINDOW( g_pPreviewWindow ) );
	// Initial selection loading used to happen before show(), so its render
	// request targeted an unrealized, unmapped GtkGLArea. Activate the mapped
	// toplevel from idle first, then load and queue the actual shader frame.
	g_idle_add( activate_and_refresh_idle, GUINT_TO_POINTER( activationTime ) );
	gtk_widget_queue_resize( g_pPreviewWidget );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
	// Ensure the first frame is requested after map/realize processing. Without
	// this, GTK can defer the render until another widget causes damage.
	g_idle_add( preview_idle_render, NULL );
	watch_for_first_frame();
#else
	gtk_widget_queue_draw( g_pPreviewWidget );
#endif
}
