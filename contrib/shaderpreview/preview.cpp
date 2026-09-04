/*
   Shader Preview plugin for GtkRadiant

   Shader parsing belongs to a later milestone. The first image-backed preview
   loads the currently selected texture through Radiant's image API and keeps
   the checkerboard as a useful fallback when a shader has no directly-loadable
   image.
*/

#include <algorithm>
#include <string>
#include <vector>
#include <sstream>

#include "gtkutil.h"
#include "shaderpreview.h"

static GtkWidget* g_pPreviewWindow = NULL;
static GtkWidget* g_pPreviewWidget = NULL;
static GtkWidget* g_pSelectionLabel = NULL;
static GLuint g_checkerboardTexture = 0;
static GLuint g_selectedTexture = 0;
static unsigned char* g_selectedPixels = NULL;
static int g_selectedWidth = 0;
static int g_selectedHeight = 0;
static bool g_selectedTextureUploaded = false;
static std::vector<std::string> g_stageMaps;
struct PreviewStage {
	std::string name;
	unsigned char* pixels;
	int width;
	int height;
	GLuint texture;
	bool uploaded;
	std::string blendSrc, blendDst;
	PreviewStage() : pixels( NULL ), width( 0 ), height( 0 ), texture( 0 ), uploaded( false ), blendSrc( "GL_SRC_ALPHA" ), blendDst( "GL_ONE_MINUS_SRC_ALPHA" ) {}
};
static std::vector<PreviewStage> g_stages;
static GtkWidget* g_stageLabel = NULL;
static int g_animatedStageCount = 0;
struct AnimationFrame { unsigned char* pixels; int width, height; GLuint texture; bool uploaded; AnimationFrame() : pixels( NULL ), width( 0 ), height( 0 ), texture( 0 ), uploaded( false ) {} };
static std::vector<std::string> g_animationNames;
static std::vector<AnimationFrame> g_animationFrames;
static float g_animationFps = 0.0f;
static guint g_animationTimer = 0;
static unsigned int g_animationTick = 0;
static bool g_animationPaused = false;
static GtkWidget* g_animationButton = NULL;

static void parse_selected_stages( const char* shaderName ){
	g_stageMaps.clear();
	g_animatedStageCount = 0;
	g_animationNames.clear();
	g_animationFps = 0.0f;
	for ( std::vector<AnimationFrame>::iterator i = g_animationFrames.begin(); i != g_animationFrames.end(); ++i ) g_free( i->pixels );
	g_animationFrames.clear();
	for ( std::vector<PreviewStage>::iterator i = g_stages.begin(); i != g_stages.end(); ++i ) g_free( i->pixels );
	g_stages.clear();
	if ( shaderName == NULL || g_ShadersTable.m_pfnShader_ForName_NoLoad == NULL || g_FuncTable.m_pfnLoadFile == NULL ) return;
	// Do not use Try_Shader_ForName here: it may load/bind Radiant's texture
	// from the UI callback, where the preview GL context is not current.
	IShader* shader = g_ShadersTable.m_pfnShader_ForName_NoLoad( shaderName );
	if ( shader == NULL || shader->getShaderFileName() == NULL || shader->getShaderFileName()[0] == '\0' ) return;
	void* buffer = NULL;
	const int size = g_FuncTable.m_pfnLoadFile( shader->getShaderFileName(), &buffer );
	if ( size <= 0 || buffer == NULL ) return;
	const char* text = static_cast<const char*>( buffer );
	const char* end = text + size;
	int shaderDepth = 0;
	int stageDepth = 0;
	int currentStage = -1;
	bool inShader = false;
	for ( const char* p = text; p < end; ) {
		while ( p < end && ( *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ) ) ++p;
		const char* token = p;
		while ( p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '{' && *p != '}' ) ++p;
		if ( p == token ) { if ( p < end ) { if ( *p == '{' ) { ++shaderDepth; if ( inShader && shaderDepth == 2 ) { stageDepth = 1; ++currentStage; g_stageMaps.push_back( std::string() ); } } else if ( *p == '}' ) { if ( stageDepth > 0 ) stageDepth--; if ( shaderDepth > 0 ) shaderDepth--; } ++p; } continue; }
		std::string word( token, p - token );
		if ( !inShader && word == shaderName ) inShader = true;
		if ( inShader && stageDepth == 1 && ( word == "map" || word == "clampmap" || word == "animMap" ) ) {
			while ( p < end && ( *p == ' ' || *p == '\t' ) ) ++p;
			if ( word == "animMap" ) {
				// animMap begins with a frame rate; the following token is frame zero.
				// Retaining the rate/frame list is the next parser increment.
				const char* rate = p;
				while ( p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '}' ) ++p;
				g_animationFps = static_cast<float>( atof( std::string( rate, p - rate ).c_str() ) );
				const char* framesEnd = p;
				while ( framesEnd < end && *framesEnd != '\n' && *framesEnd != '\r' && *framesEnd != '}' ) ++framesEnd;
				std::istringstream frames( std::string( p, framesEnd - p ) );
				std::string frameName;
				while ( frames >> frameName ) g_animationNames.push_back( frameName );
				while ( p < end && ( *p == ' ' || *p == '\t' ) ) ++p;
				++g_animatedStageCount;
			}
			const char* source = p;
			while ( p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '}' ) ++p;
			if ( p > source && currentStage >= 0 ) g_stageMaps[currentStage] = std::string( source, p - source );
		}
		else if ( inShader && stageDepth == 1 && word == "blendFunc" && currentStage >= 0 ) {
			while ( p < end && ( *p == ' ' || *p == '\t' ) ) ++p;
			const char* source = p;
			while ( p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '}' ) ++p;
			const std::string src( source, p - source );
			while ( p < end && ( *p == ' ' || *p == '\t' ) ) ++p;
			const char* destination = p;
			while ( p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '}' ) ++p;
			if ( !src.empty() && p > destination ) {
				// Store the factors temporarily in the stage-map side channel.
				g_stageMaps[currentStage] += "\n" + src + " " + std::string( destination, p - destination );
			}
		}
	}
	for ( std::vector<std::string>::const_iterator name = g_animationNames.begin(); name != g_animationNames.end(); ++name ) {
		AnimationFrame frame; g_FuncTable.m_pfnLoadImage( name->c_str(), &frame.pixels, &frame.width, &frame.height );
		if ( frame.pixels != NULL ) g_animationFrames.push_back( frame );
	}
	g_free( buffer );
	for ( std::vector<std::string>::const_iterator i = g_stageMaps.begin(); i != g_stageMaps.end(); ++i ) {
		PreviewStage stage;
		const std::string::size_type spec = i->find( '\n' );
		stage.name = spec == std::string::npos ? *i : i->substr( 0, spec );
		if ( spec != std::string::npos ) {
			std::istringstream blend( i->substr( spec + 1 ) );
			blend >> stage.blendSrc >> stage.blendDst;
		}
		g_FuncTable.m_pfnLoadImage( stage.name.c_str(), &stage.pixels, &stage.width, &stage.height );
		if ( stage.pixels != NULL && stage.width > 0 && stage.height > 0 ) g_stages.push_back( stage );
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
}

static gboolean animation_tick( gpointer ){
	if ( g_pPreviewWidget == NULL || g_animationFrames.empty() ) return FALSE;
	++g_animationTick;
#if GTK_CHECK_VERSION( 3, 0, 0 )
	gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
#else
	gtk_widget_queue_draw( g_pPreviewWidget );
#endif
	return TRUE;
}

static void start_animation_timer(){
	if ( g_animationTimer == 0 && !g_animationPaused && g_animationFrames.size() > 1 && g_animationFps > 0.0f ) {
		g_animationTimer = g_timeout_add( static_cast<guint>( 1000.0f / g_animationFps ), animation_tick, NULL );
	}
}

static void animation_clicked( GtkButton*, gpointer ){
	if ( g_animationTimer != 0 ) {
		g_source_remove( g_animationTimer ); g_animationTimer = 0; g_animationPaused = true;
		gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Play" );
	}
	else {
		g_animationPaused = false; start_animation_timer();
		gtk_button_set_label( GTK_BUTTON( g_animationButton ), "Pause" );
	}
}

void ShaderPreview_RefreshSelection(){
	if ( g_pSelectionLabel == NULL ) {
		return;
	}

	const char* selected = g_FuncTable.m_pfnGetCurrentTexture();
	if ( selected != NULL && selected[0] != '\0' ) {
		char text[512];
		snprintf( text, sizeof( text ), "Selected shader: %s", selected );
		gtk_label_set_text( GTK_LABEL( g_pSelectionLabel ), text );
		parse_selected_stages( selected );
		if ( g_animationTimer != 0 ) { g_source_remove( g_animationTimer ); g_animationTimer = 0; }
		g_animationTick = 0;
		g_animationPaused = false;
		start_animation_timer();
		if ( g_animationButton != NULL ) gtk_widget_set_sensitive( g_animationButton, g_animationFrames.size() > 1 && g_animationFps > 0.0f );
		if ( g_stageLabel != NULL ) {
			char stages[128];
			if ( !g_stageMaps.empty() ) {
				snprintf( stages, sizeof( stages ), "Parsed stages: %d, animMap: %d (%s)", static_cast<int>( g_stageMaps.size() ), g_animatedStageCount, g_stageMaps.front().c_str() );
			}
			else {
				snprintf( stages, sizeof( stages ), "Parsed stages: 0 (shader API/file unavailable)" );
			}
			gtk_label_set_text( GTK_LABEL( g_stageLabel ), stages );
		}
		clear_selected_image();
		// A shader name is not necessarily an image filename. Ask the shader
		// system for its representative texture first, then decode that image
		// into this plugin's own (portable) GL context.
		const char* imageName = selected;
		if ( g_ShadersTable.m_pfnShader_ForName_NoLoad != NULL ) {
			IShader* shader = g_ShadersTable.m_pfnShader_ForName_NoLoad( selected );
			if ( shader != NULL && shader->getTexture() != NULL ) {
				imageName = shader->getTexture()->name;
			}
		}
		g_FuncTable.m_pfnLoadImage( imageName, &g_selectedPixels, &g_selectedWidth, &g_selectedHeight );
		if ( g_selectedPixels == NULL && imageName != selected ) {
			g_FuncTable.m_pfnLoadImage( selected, &g_selectedPixels, &g_selectedWidth, &g_selectedHeight );
		}
	}
	else {
		gtk_label_set_text( GTK_LABEL( g_pSelectionLabel ), "No current shader selected — showing checkerboard" );
		clear_selected_image();
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
	if ( factor == "GL_ONE" || factor == "one" ) return GL_ONE;
	if ( factor == "GL_ZERO" || factor == "zero" ) return GL_ZERO;
	if ( factor == "GL_DST_COLOR" || factor == "dst_color" ) return GL_DST_COLOR;
	if ( factor == "GL_ONE_MINUS_DST_COLOR" || factor == "one_minus_dst_color" ) return GL_ONE_MINUS_DST_COLOR;
	if ( factor == "GL_SRC_ALPHA" || factor == "src_alpha" ) return GL_SRC_ALPHA;
	if ( factor == "GL_ONE_MINUS_SRC_ALPHA" || factor == "one_minus_src_alpha" ) return GL_ONE_MINUS_SRC_ALPHA;
	if ( factor == "GL_DST_ALPHA" || factor == "dst_alpha" ) return GL_DST_ALPHA;
	if ( factor == "GL_ONE_MINUS_DST_ALPHA" || factor == "one_minus_dst_alpha" ) return GL_ONE_MINUS_DST_ALPHA;
	return GL_SRC_ALPHA;
}

static void draw_preview(){
	const int width = gtkutil_widget_get_width( g_pPreviewWidget );
	const int height = gtkutil_widget_get_height( g_pPreviewWidget );
	if ( width <= 0 || height <= 0 ) {
		return;
	}

	ensure_checkerboard_texture();
	for ( std::vector<PreviewStage>::iterator i = g_stages.begin(); i != g_stages.end(); ++i ) {
		if ( i->pixels != NULL && !i->uploaded ) {
			g_QglTable.m_pfn_qglGenTextures( 1, &i->texture );
			g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, i->texture );
			g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
			g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
			g_QglTable.m_pfn_qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, i->width, i->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, i->pixels );
			i->uploaded = true;
		}
	}
	for ( std::vector<AnimationFrame>::iterator i = g_animationFrames.begin(); i != g_animationFrames.end(); ++i ) {
		if ( i->pixels != NULL && !i->uploaded ) { g_QglTable.m_pfn_qglGenTextures( 1, &i->texture ); g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, i->texture ); g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR ); g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR ); g_QglTable.m_pfn_qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, i->width, i->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, i->pixels ); i->uploaded = true; }
	}
	if ( g_selectedPixels != NULL && !g_selectedTextureUploaded ) {
		g_QglTable.m_pfn_qglGenTextures( 1, &g_selectedTexture );
		g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, g_selectedTexture );
		g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );
		g_QglTable.m_pfn_qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );
		g_QglTable.m_pfn_qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, g_selectedWidth, g_selectedHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_selectedPixels );
		g_selectedTextureUploaded = true;
	}

	g_QglTable.m_pfn_qglViewport( 0, 0, width, height );
	g_QglTable.m_pfn_qglClearColor( 0.08f, 0.09f, 0.11f, 1.0f );
	g_QglTable.m_pfn_qglClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	g_QglTable.m_pfn_qglDisable( GL_DEPTH_TEST );
	g_QglTable.m_pfn_qglEnable( GL_BLEND );
	g_QglTable.m_pfn_qglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

	g_QglTable.m_pfn_qglMatrixMode( GL_PROJECTION );
	g_QglTable.m_pfn_qglLoadIdentity();
	g_QglTable.m_pfn_qglOrtho( 0, width, 0, height, -1, 1 );
	g_QglTable.m_pfn_qglMatrixMode( GL_MODELVIEW );
	g_QglTable.m_pfn_qglLoadIdentity();

	const float imageWidth = !g_stages.empty() ? static_cast<float>( g_stages.front().width ) : ( g_selectedTextureUploaded ? static_cast<float>( g_selectedWidth ) : 1.0f );
	const float imageHeight = !g_stages.empty() ? static_cast<float>( g_stages.front().height ) : ( g_selectedTextureUploaded ? static_cast<float>( g_selectedHeight ) : 1.0f );
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
	// Always show the checkerboard underneath the image so transparent pixels
	// are visible. The selected stage is then composited over it with alpha.
	g_QglTable.m_pfn_qglDisable( GL_BLEND );
	g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, g_checkerboardTexture );
	g_QglTable.m_pfn_qglBegin( GL_QUADS );
	g_QglTable.m_pfn_qglTexCoord2f( 0, 1 ); g_QglTable.m_pfn_qglVertex2f( left, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( 1, 1 ); g_QglTable.m_pfn_qglVertex2f( right, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( 1, 0 ); g_QglTable.m_pfn_qglVertex2f( right, top );
	g_QglTable.m_pfn_qglTexCoord2f( 0, 0 ); g_QglTable.m_pfn_qglVertex2f( left, top );
	g_QglTable.m_pfn_qglEnd();
	for ( std::vector<PreviewStage>::const_iterator stage = g_stages.begin(); stage != g_stages.end(); ++stage ) {
		if ( !stage->uploaded ) continue;
		g_QglTable.m_pfn_qglEnable( GL_BLEND );
		g_QglTable.m_pfn_qglBlendFunc( blend_factor( stage->blendSrc ), blend_factor( stage->blendDst ) );
		g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, stage->texture );
		g_QglTable.m_pfn_qglBegin( GL_QUADS );
		// Image loaders use top-left origin; OpenGL's texture origin is bottom-left.
		g_QglTable.m_pfn_qglTexCoord2f( 0, 1 ); g_QglTable.m_pfn_qglVertex2f( left, bottom );
		g_QglTable.m_pfn_qglTexCoord2f( 1, 1 ); g_QglTable.m_pfn_qglVertex2f( right, bottom );
		g_QglTable.m_pfn_qglTexCoord2f( 1, 0 ); g_QglTable.m_pfn_qglVertex2f( right, top );
		g_QglTable.m_pfn_qglTexCoord2f( 0, 0 ); g_QglTable.m_pfn_qglVertex2f( left, top );
		g_QglTable.m_pfn_qglEnd();
	}
	if ( !g_animationFrames.empty() ) {
		const AnimationFrame& frame = g_animationFrames[g_animationTick % g_animationFrames.size()];
		g_QglTable.m_pfn_qglEnable( GL_BLEND ); g_QglTable.m_pfn_qglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA ); g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, frame.texture );
		g_QglTable.m_pfn_qglBegin( GL_QUADS );
		g_QglTable.m_pfn_qglTexCoord2f( 0, 1 ); g_QglTable.m_pfn_qglVertex2f( left, bottom ); g_QglTable.m_pfn_qglTexCoord2f( 1, 1 ); g_QglTable.m_pfn_qglVertex2f( right, bottom ); g_QglTable.m_pfn_qglTexCoord2f( 1, 0 ); g_QglTable.m_pfn_qglVertex2f( right, top ); g_QglTable.m_pfn_qglTexCoord2f( 0, 0 ); g_QglTable.m_pfn_qglVertex2f( left, top ); g_QglTable.m_pfn_qglEnd();
	}
	if ( g_stages.empty() && g_selectedTextureUploaded ) {
		g_QglTable.m_pfn_qglEnable( GL_BLEND );
		g_QglTable.m_pfn_qglBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
		g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, g_selectedTexture );
		g_QglTable.m_pfn_qglBegin( GL_QUADS );
		g_QglTable.m_pfn_qglTexCoord2f( 0, 1 ); g_QglTable.m_pfn_qglVertex2f( left, bottom );
		g_QglTable.m_pfn_qglTexCoord2f( 1, 1 ); g_QglTable.m_pfn_qglVertex2f( right, bottom );
		g_QglTable.m_pfn_qglTexCoord2f( 1, 0 ); g_QglTable.m_pfn_qglVertex2f( right, top );
		g_QglTable.m_pfn_qglTexCoord2f( 0, 0 ); g_QglTable.m_pfn_qglVertex2f( left, top );
		g_QglTable.m_pfn_qglEnd();
	}
	g_QglTable.m_pfn_qglDisable( GL_TEXTURE_2D );
}

static void render_preview(){
	if ( !g_UIGtkTable.m_pfn_glwidget_make_current( g_pPreviewWidget ) ) {
		g_FuncTable.m_pfnSysFPrintf( SYS_ERR, "Shader Preview: failed to activate OpenGL context\n" );
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
	// damage (such as button hover).
	gtk_gl_area_queue_render( GTK_GL_AREA( widget ) );
}

static gboolean preview_idle_render( gpointer data ){
	(void)data;
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
	// The GtkGLArea/GtkGLExt widget owns the GL context. Its destruction releases
	// the context and the checkerboard texture with it.
	g_checkerboardTexture = 0;
	if ( g_animationTimer != 0 ) { g_source_remove( g_animationTimer ); g_animationTimer = 0; }
	for ( std::vector<PreviewStage>::iterator i = g_stages.begin(); i != g_stages.end(); ++i ) g_free( i->pixels );
	g_stages.clear();
	g_selectedTexture = 0;
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
		"Shader editing follows the preview, stage-stack, animation, and compositing milestones.",
		"Shader Editor",
		MB_OK,
		NULL
	);
}

static void refresh_selection_clicked( GtkButton*, gpointer ){
	ShaderPreview_RefreshSelection();
}


void ShaderPreview_Show(){
	if ( g_pPreviewWindow != NULL ) {
		ShaderPreview_RefreshSelection();
		gtk_window_present( GTK_WINDOW( g_pPreviewWindow ) );
		return;
	}

	g_pPreviewWindow = gtk_window_new( GTK_WINDOW_TOPLEVEL );
	gtk_window_set_title( GTK_WINDOW( g_pPreviewWindow ), "Shader Preview" );
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
	ShaderPreview_RefreshSelection();

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
