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
static GtkWidget* g_animationButton = NULL;

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

// The engine stores a fixed number of animation frames per stage.
static const size_t ANIMATION_FRAME_LIMIT = 8;

struct PreviewStage
{
	std::string mapName;
	std::string blendSrc;
	std::string blendDst;
	bool clamp;
	unsigned char* pixels;
	int width;
	int height;
	GLuint texture;
	bool uploaded;
	float animationFps;
	bool rgbWave;
	bool stretchWave;
	float rgbBase, rgbAmplitude, rgbPhase, rgbFrequency;
	float stretchBase, stretchAmplitude, stretchPhase, stretchFrequency;
	std::vector<std::string> animationNames;
	std::vector<AnimationFrame> animationFrames;

	PreviewStage() :
		blendSrc( "GL_SRC_ALPHA" ),
		blendDst( "GL_ONE_MINUS_SRC_ALPHA" ),
		clamp( false ),
		pixels( NULL ),
		width( 0 ),
		height( 0 ),
		texture( 0 ),
		uploaded( false ),
		animationFps( 0.0f ), rgbWave( false ), stretchWave( false ),
		rgbBase( 1.0f ), rgbAmplitude( 0.0f ), rgbPhase( 0.0f ), rgbFrequency( 0.0f ),
		stretchBase( 1.0f ), stretchAmplitude( 0.0f ), stretchPhase( 0.0f ), stretchFrequency( 0.0f )
	{}
};

static std::vector<PreviewStage> g_stages;

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

static guint g_animationTimer = 0;
static unsigned int g_animationTick = 0;
static float g_animationClockFps = 0.0f;
static bool g_animationPaused = false;

static void clear_stages(){
	for ( std::vector<PreviewStage>::iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		if ( stage->pixels != NULL ) {
			g_free( stage->pixels );
			stage->pixels = NULL;
		}
		for ( std::vector<AnimationFrame>::iterator frame = stage->animationFrames.begin(); frame != stage->animationFrames.end(); ++frame ) {
			if ( frame->pixels != NULL ) {
				g_free( frame->pixels );
				frame->pixels = NULL;
			}
		}
		stage->animationFrames.clear();
	}
	g_stages.clear();
	g_animationClockFps = 0.0f;
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

static void load_stage_images(){
	g_animationClockFps = 0.0f;

	int stageNumber = 0;
	for ( std::vector<PreviewStage>::iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
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
			if ( stage->animationFrames.size() > 1 && stage->animationFps > g_animationClockFps ) {
				g_animationClockFps = stage->animationFps;
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
		if ( stage->rgbWave || stage->stretchWave ) g_animationClockFps = std::max( g_animationClockFps, 30.0f );
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

static void parse_selected_stages( const char* shaderName ){
	clear_stages();
	g_diagnostics.clear();

	if ( shaderName == NULL || g_ShadersTable.m_pfnShader_ForName_NoLoad == NULL || g_FuncTable.m_pfnLoadFile == NULL ||
		 g_ScripLibTable.m_pfnStartTokenParsing == NULL || g_ScripLibTable.m_pfnGetToken == NULL ||
		 g_ScripLibTable.m_pfnToken == NULL || g_ScripLibTable.m_pfnScriptLine == NULL ||
		 g_ScripLibTable.m_pfnUnGetToken == NULL ) {
		return;
	}

	// Do not use Try_Shader_ForName here: it may load/bind Radiant's texture
	// from the UI callback, where the preview GL context is not current.
	IShader* shader = g_ShadersTable.m_pfnShader_ForName_NoLoad( shaderName );
	if ( shader == NULL || shader->getShaderFileName() == NULL || shader->getShaderFileName()[0] == '\0' ) {
		return;
	}

	void* buffer = NULL;
	const int size = g_FuncTable.m_pfnLoadFile( shader->getShaderFileName(), &buffer );
	if ( size <= 0 || buffer == NULL ) {
		return;
	}

	g_ScripLibTable.m_pfnStartTokenParsing( static_cast<char*>( buffer ) );
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
				g_stages.push_back( PreviewStage() );
				currentStage = static_cast<int>( g_stages.size() ) - 1;
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

		if ( depth != 2 || currentStage < 0 ) {
			continue;
		}

		PreviewStage& stage = g_stages[currentStage];

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
		else if ( keyword_equals( token, "rgbGen" ) || keyword_equals( token, "tcMod" ) ) {
			const bool isRgb = keyword_equals( token, "rgbGen" );
			std::string mode;
			if ( !next_script_token_on_line( directiveLine, mode ) || !keyword_equals( mode, isRgb ? "wave" : "stretch" ) ) continue;
			std::string wave, base, amplitude, phase, frequency;
			if ( !next_script_token_on_line( directiveLine, wave ) || !next_script_token_on_line( directiveLine, base ) || !next_script_token_on_line( directiveLine, amplitude ) || !next_script_token_on_line( directiveLine, phase ) || !next_script_token_on_line( directiveLine, frequency ) ) {
				shadershop_warn( "stage %d: incomplete %s %s", stageNumber, token.c_str(), mode.c_str() );
				continue;
			}
			if ( !keyword_equals( wave, "sin" ) && !keyword_equals( wave, "square" ) ) { shadershop_warn( "stage %d: wave '%s' is not supported yet", stageNumber, wave.c_str() ); continue; }
			const float b = static_cast<float>( atof( base.c_str() ) ), a = static_cast<float>( atof( amplitude.c_str() ) ), p = static_cast<float>( atof( phase.c_str() ) ), f = static_cast<float>( atof( frequency.c_str() ) );
			if ( isRgb ) { stage.rgbWave = true; stage.rgbBase = b; stage.rgbAmplitude = a; stage.rgbPhase = p; stage.rgbFrequency = f; }
			else { stage.stretchWave = true; stage.stretchBase = b; stage.stretchAmplitude = a; stage.stretchPhase = p; stage.stretchFrequency = f; }
		}
	}

	if ( !inShader ) {
		shadershop_warn(
			"'%s' was not found at the top level of %s",
			shaderName, shader->getShaderFileName()
		);
	}
	else if ( !shaderClosed ) {
		shadershop_warn(
			"'%s' reaches end of file without a closing brace",
			shaderName
		);
	}

	g_free( buffer );
	load_stage_images();
}

static void clear_selected_image(){
	if ( g_selectedPixels != NULL ) {
		g_free( g_selectedPixels );
		g_selectedPixels = NULL;
	}
	g_selectedWidth = 0;
	g_selectedHeight = 0;
	g_selectedTextureUploaded = false;
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
	if ( g_pPreviewWidget == NULL || g_animationClockFps <= 0.0f ) {
		return FALSE;
	}

	++g_animationTick;
#if GTK_CHECK_VERSION( 3, 0, 0 )
	gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
#else
	gtk_widget_queue_draw( g_pPreviewWidget );
#endif
	return TRUE;
}

static void start_animation_timer(){
	if ( g_animationTimer == 0 && !g_animationPaused && g_animationClockFps > 0.0f ) {
		guint interval = static_cast<guint>( 1000.0f / g_animationClockFps );
		if ( interval == 0 ) {
			interval = 1;
		}
		g_animationTimer = g_timeout_add( interval, animation_tick, NULL );
	}
}

static void animation_clicked( GtkButton*, gpointer ){
	if ( g_animationTimer != 0 ) {
		g_source_remove( g_animationTimer );
		g_animationTimer = 0;
		g_animationPaused = true;
		gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Play" );
	}
	else {
		g_animationPaused = false;
		start_animation_timer();
		gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Pause" );
	}
}

void ShaderShop_RefreshSelection(){
	if ( g_pSelectionLabel == NULL ) {
		return;
	}

	const char* selected = g_FuncTable.m_pfnGetCurrentTexture();
	if ( selected != NULL && selected[0] != '\0' ) {
		char text[512];
		snprintf( text, sizeof( text ), "Selected shader: %s", selected );
		gtk_label_set_text( GTK_LABEL( g_pSelectionLabel ), text );

		parse_selected_stages( selected );

		if ( g_animationTimer != 0 ) {
			g_source_remove( g_animationTimer );
			g_animationTimer = 0;
		}
		g_animationTick = 0;
		g_animationPaused = false;
		start_animation_timer();

		if ( g_animationButton != NULL ) {
			gtk_widget_set_sensitive( g_animationButton, g_animationClockFps > 0.0f );
			gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Pause" );
		}

		if ( g_stageLabel != NULL ) {
			char stages[192];
			if ( !g_stages.empty() ) {
				snprintf(
					stages,
					sizeof( stages ),
					"Parsed stages: %d, animated: %d",
					static_cast<int>( g_stages.size() ),
					animated_stage_count()
				);
			}
			else {
				snprintf( stages, sizeof( stages ), "Parsed stages: 0 (shader API/file unavailable)" );
			}

			// Tolerated problems are reported here as well as on the console, so
			// a wrong-looking preview can be told apart from a correct one.
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

		clear_selected_image();

		// A shader name is not necessarily an image filename. Ask the shader
		// system for its representative texture first, then decode that image
		// into this plugin's own GL context.
		const char* imageName = selected;
		if ( g_ShadersTable.m_pfnShader_ForName_NoLoad != NULL ) {
			IShader* shader = g_ShadersTable.m_pfnShader_ForName_NoLoad( selected );
			if ( shader != NULL && shader->getTexture() != NULL ) {
				imageName = shader->getTexture()->name;
			}
		}

		if ( imageName == NULL || imageName[0] == '\0' ) {
			imageName = selected;
		}
		if ( !load_image( imageName, &g_selectedPixels, &g_selectedWidth, &g_selectedHeight ) && imageName != selected ) {
			load_image( selected, &g_selectedPixels, &g_selectedWidth, &g_selectedHeight );
		}
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
	}

	if ( g_pPreviewWidget != NULL ) {
#if GTK_CHECK_VERSION( 3, 0, 0 )
		gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
#else
		gtk_widget_queue_draw( g_pPreviewWidget );
#endif
	}
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
		if ( stage.animationFrames.size() > 1 && stage.animationFps > 0.0f && g_animationClockFps > 0.0f ) {
			frameIndex = static_cast<unsigned int>(
				static_cast<double>( g_animationTick ) * stage.animationFps / g_animationClockFps
			) % static_cast<unsigned int>( stage.animationFrames.size() );
		}
		return stage.animationFrames[frameIndex].uploaded ? stage.animationFrames[frameIndex].texture : 0;
	}
	return stage.uploaded ? stage.texture : 0;
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

static float stage_wave( float base, float amplitude, float phase, float frequency ){
	const float time = g_animationClockFps > 0.0f ? static_cast<float>( g_animationTick ) / g_animationClockFps : 0.0f;
	return base + amplitude * sinf( 6.28318530718f * ( phase + time * frequency ) );
}

static void draw_preview(){
	const int width = gtkutil_widget_get_width( g_pPreviewWidget );
	const int height = gtkutil_widget_get_height( g_pPreviewWidget );
	if ( width <= 0 || height <= 0 ) {
		return;
	}

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

	g_QglTable.m_pfn_qglViewport( 0, 0, width, height );
	g_QglTable.m_pfn_qglClearColor( 0.08f, 0.09f, 0.11f, 1.0f );
	g_QglTable.m_pfn_qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	g_QglTable.m_pfn_qglDisable( GL_DEPTH_TEST );

	g_QglTable.m_pfn_qglMatrixMode( GL_PROJECTION );
	g_QglTable.m_pfn_qglLoadIdentity();
	g_QglTable.m_pfn_qglOrtho( 0, width, 0, height, -1, 1 );
	g_QglTable.m_pfn_qglMatrixMode( GL_MODELVIEW );
	g_QglTable.m_pfn_qglLoadIdentity();

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

	const float maxWidth = static_cast<float>( width ) * 0.78f;
	const float maxHeight = static_cast<float>( height ) * 0.78f;
	const float scale = std::min( maxWidth / imageWidth, maxHeight / imageHeight );
	const float drawWidth = imageWidth * scale;
	const float drawHeight = imageHeight * scale;
	const float left = ( width - drawWidth ) * 0.5f;
	const float bottom = ( height - drawHeight ) * 0.5f;
	const float right = left + drawWidth;
	const float top = bottom + drawHeight;

	g_QglTable.m_pfn_qglEnable( GL_TEXTURE_2D );
	g_QglTable.m_pfn_qglColor4f( 1, 1, 1, 1 );

	// Always show the checkerboard underneath the stage stack so transparent
	// pixels remain visible.
	g_QglTable.m_pfn_qglDisable( GL_BLEND );
	draw_textured_quad( g_checkerboardTexture, left, bottom, right, top );

	bool drewStage = false;
	for ( std::vector<PreviewStage>::const_iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		const GLuint texture = stage_texture( *stage );
		if ( texture == 0 ) {
			continue;
		}

		g_QglTable.m_pfn_qglEnable( GL_BLEND );
		g_QglTable.m_pfn_qglBlendFunc( blend_factor( stage->blendSrc ), blend_factor( stage->blendDst ) );
		const float brightness = stage->rgbWave ? stage_wave( stage->rgbBase, stage->rgbAmplitude, stage->rgbPhase, stage->rgbFrequency ) : 1.0f;
		g_QglTable.m_pfn_qglColor4f( brightness, brightness, brightness, 1.0f );
		if ( stage->stretchWave ) {
			const float stretch = stage_wave( stage->stretchBase, stage->stretchAmplitude, stage->stretchPhase, stage->stretchFrequency );
			g_QglTable.m_pfn_qglMatrixMode( GL_TEXTURE );
			g_QglTable.m_pfn_qglLoadIdentity();
			g_QglTable.m_pfn_qglTranslatef( 0.5f, 0.5f, 0.0f );
			g_QglTable.m_pfn_qglScalef( stretch, stretch, 1.0f );
			g_QglTable.m_pfn_qglTranslatef( -0.5f, -0.5f, 0.0f );
			g_QglTable.m_pfn_qglMatrixMode( GL_MODELVIEW );
		}
		draw_textured_quad( texture, left, bottom, right, top );
		if ( stage->stretchWave ) { g_QglTable.m_pfn_qglMatrixMode( GL_TEXTURE ); g_QglTable.m_pfn_qglLoadIdentity(); g_QglTable.m_pfn_qglMatrixMode( GL_MODELVIEW ); }
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
	if ( !g_UIGtkTable.m_pfn_glwidget_make_current( g_pPreviewWidget ) ) {
		g_FuncTable.m_pfnSysFPrintf( SYS_ERR, "ShaderShop: failed to activate OpenGL context\n" );
		return;
	}

	draw_preview();
	g_QglTable.m_pfn_QE_CheckOpenGLForErrors();
}

#if GTK_CHECK_VERSION( 3, 0, 0 )
static gboolean preview_render( GtkGLArea*, GdkGLContext*, gpointer ){
	render_preview();
	return TRUE;
}

static void preview_realized( GtkWidget* widget, gpointer ){
	// GtkGLArea creates its context during realization. Queueing before that
	// point can be dropped, leaving the first frame waiting for unrelated UI
	// damage such as button hover.
	gtk_gl_area_queue_render( GTK_GL_AREA( widget ) );
}

static gboolean preview_idle_render( gpointer ){
	if ( g_pPreviewWindow != NULL && g_pPreviewWidget != NULL ) {
		gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
	}
	return FALSE;
}
#else
static gboolean preview_expose( GtkWidget*, GdkEventExpose* event, gpointer ){
	if ( event->count != 0 ) {
		return TRUE;
	}
	render_preview();
	g_UIGtkTable.m_pfn_glwidget_swap_buffers( g_pPreviewWidget );
	return TRUE;
}
#endif

static void preview_destroyed( GtkWidget*, gpointer ){
	// The preview widget owns the GL context. Its destruction releases the GL
	// textures; CPU image data remains the plugin's responsibility.
	g_checkerboardTexture = 0;
	g_selectedTexture = 0;

	if ( g_animationTimer != 0 ) {
		g_source_remove( g_animationTimer );
		g_animationTimer = 0;
	}

	clear_stages();
	clear_selected_image();

	g_pPreviewWidget = NULL;
	g_pSelectionLabel = NULL;
	g_stageLabel = NULL;
	g_animationButton = NULL;
	g_pPreviewWindow = NULL;
}

static void edit_shader_clicked( GtkButton*, gpointer ){
	g_FuncTable.m_pfnMessageBox(
		g_pPreviewWindow,
		"Shader editing is not implemented yet; this build currently provides the preview surface.",
		"ShaderShop",
		MB_OK,
		NULL
	);
}

static void refresh_selection_clicked( GtkButton*, gpointer ){
	ShaderShop_RefreshSelection();
}

void ShaderShop_Show(){
	if ( g_pPreviewWindow != NULL ) {
		ShaderShop_RefreshSelection();
		gtk_window_present( GTK_WINDOW( g_pPreviewWindow ) );
		return;
	}

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
	gtk_widget_set_events( g_pPreviewWidget, GDK_EXPOSURE_MASK );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "render", G_CALLBACK( preview_render ), NULL );
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "realize", G_CALLBACK( preview_realized ), NULL );
#else
	g_signal_connect( G_OBJECT( g_pPreviewWidget ), "expose-event", G_CALLBACK( preview_expose ), NULL );
#endif
	gtk_container_add( GTK_CONTAINER( frame ), g_pPreviewWidget );
	gtk_widget_set_hexpand( g_pPreviewWidget, TRUE );
	gtk_widget_set_vexpand( g_pPreviewWidget, TRUE );
	gtk_widget_show( g_pPreviewWidget );

#if GTK_CHECK_VERSION( 3, 0, 0 )
	GtkWidget* controls = gtk_box_new( GTK_ORIENTATION_HORIZONTAL, 6 );
#else
	GtkWidget* controls = gtk_hbox_new( FALSE, 6 );
#endif
	gtk_box_pack_start( GTK_BOX( box ), controls, FALSE, FALSE, 0 );
	gtk_widget_show( controls );

	g_pSelectionLabel = gtk_label_new( "No current shader selected — showing checkerboard" );
	gtk_box_pack_start( GTK_BOX( controls ), g_pSelectionLabel, TRUE, TRUE, 0 );
	gtk_widget_show( g_pSelectionLabel );

	g_stageLabel = gtk_label_new( "Parsed stages: 0" );
	gtk_box_pack_start( GTK_BOX( controls ), g_stageLabel, FALSE, FALSE, 0 );
	gtk_widget_show( g_stageLabel );

	GtkWidget* refresh = gtk_button_new_with_label( "Use Current Shader" );
	g_signal_connect( G_OBJECT( refresh ), "clicked", G_CALLBACK( refresh_selection_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), refresh, FALSE, FALSE, 0 );
	gtk_widget_show( refresh );

	g_animationButton = gtk_button_new_with_label( "Pause" );
	g_signal_connect( G_OBJECT( g_animationButton ), "clicked", G_CALLBACK( animation_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), g_animationButton, FALSE, FALSE, 0 );
	gtk_widget_show( g_animationButton );

	GtkWidget* edit = gtk_button_new_with_label( "Edit Shader..." );
	g_signal_connect( G_OBJECT( edit ), "clicked", G_CALLBACK( edit_shader_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), edit, FALSE, FALSE, 0 );
	gtk_widget_show( edit );

	ShaderShop_RefreshSelection();

	gtk_widget_show( g_pPreviewWindow );
	gtk_widget_queue_resize( g_pPreviewWidget );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
	// Ensure the first frame is requested after map/realize processing. Without
	// this, GTK can defer the render until another widget causes damage.
	g_idle_add( preview_idle_render, NULL );
#else
	gtk_widget_queue_draw( g_pPreviewWidget );
#endif
}
