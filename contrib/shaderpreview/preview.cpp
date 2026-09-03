/*
   Shader Preview plugin for GtkRadiant

   Shader parsing belongs to a later milestone. The first image-backed preview
   loads the currently selected texture through Radiant's image API and keeps
   the checkerboard as a useful fallback when a shader has no directly-loadable
   image.
*/

#include <algorithm>

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

static void clear_selected_image(){
	if ( g_selectedPixels != NULL ) {
		g_free( g_selectedPixels );
		g_selectedPixels = NULL;
	}
	g_selectedWidth = 0;
	g_selectedHeight = 0;
	g_selectedTextureUploaded = false;
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
		clear_selected_image();
		// A shader name is not necessarily an image filename. Ask the shader
		// system for its representative texture first, then decode that image
		// into this plugin's own (portable) GL context.
		const char* imageName = selected;
		if ( g_ShadersTable.m_pfnTry_Shader_ForName != NULL ) {
			IShader* shader = g_ShadersTable.m_pfnTry_Shader_ForName( selected );
			if ( shader != NULL && shader->getTexture() != NULL ) {
				imageName = shader->getTexture()->name;
			}
		}
		g_FuncTable.m_pfnLoadImage( imageName, &g_selectedPixels, &g_selectedWidth, &g_selectedHeight );
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

static void draw_preview(){
	const int width = gtkutil_widget_get_width( g_pPreviewWidget );
	const int height = gtkutil_widget_get_height( g_pPreviewWidget );
	if ( width <= 0 || height <= 0 ) {
		return;
	}

	ensure_checkerboard_texture();
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

	const float imageWidth = g_selectedTextureUploaded ? static_cast<float>( g_selectedWidth ) : 1.0f;
	const float imageHeight = g_selectedTextureUploaded ? static_cast<float>( g_selectedHeight ) : 1.0f;
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
	g_QglTable.m_pfn_qglBindTexture( GL_TEXTURE_2D, g_selectedTextureUploaded ? g_selectedTexture : g_checkerboardTexture );
	g_QglTable.m_pfn_qglColor4f( 1, 1, 1, 1 );
	g_QglTable.m_pfn_qglBegin( GL_QUADS );
	// Image loaders use top-left origin; OpenGL's texture origin is bottom-left.
	g_QglTable.m_pfn_qglTexCoord2f( 0, 1 ); g_QglTable.m_pfn_qglVertex2f( left, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( 1, 1 ); g_QglTable.m_pfn_qglVertex2f( right, bottom );
	g_QglTable.m_pfn_qglTexCoord2f( 1, 0 ); g_QglTable.m_pfn_qglVertex2f( right, top );
	g_QglTable.m_pfn_qglTexCoord2f( 0, 0 ); g_QglTable.m_pfn_qglVertex2f( left, top );
	g_QglTable.m_pfn_qglEnd();
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
	gtk_gl_area_queue_render( GTK_GL_AREA( data ) );
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
	g_selectedTexture = 0;
	clear_selected_image();
	g_pPreviewWidget = NULL;
	g_pSelectionLabel = NULL;
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

	GtkWidget* refresh = gtk_button_new_with_label( "Use Current Shader" );
	g_signal_connect( G_OBJECT( refresh ), "clicked", G_CALLBACK( refresh_selection_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), refresh, FALSE, FALSE, 0 );
	gtk_widget_show( refresh );

	GtkWidget* edit = gtk_button_new_with_label( "Edit Shader..." );
	g_signal_connect( G_OBJECT( edit ), "clicked", G_CALLBACK( edit_shader_clicked ), NULL );
	gtk_box_pack_end( GTK_BOX( controls ), edit, FALSE, FALSE, 0 );
	gtk_widget_show( edit );
	ShaderPreview_RefreshSelection();

	gtk_widget_show( g_pPreviewWindow );
#if GTK_CHECK_VERSION( 3, 0, 0 )
	gtk_gl_area_queue_render( GTK_GL_AREA( g_pPreviewWidget ) );
	// Ensure the first frame is requested after map/realize processing. Without
	// this, GTK can defer the render until another widget causes damage.
	g_idle_add( preview_idle_render, g_pPreviewWidget );
#else
	gtk_widget_queue_draw( g_pPreviewWidget );
#endif
}
