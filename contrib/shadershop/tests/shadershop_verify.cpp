/*
   ShaderShop parser verification

   Exercises the stage-model walk over Radiant's own ScriptLib table and
   reports a current-semantics census of the shader corpus. ShaderShop no
   longer has a lexical tokenizer to compare: production uses this same parser
   API.

   Modes:
     --stages   print the stage model per shader definition, as a reference
     --census   emit tab-separated corpus counters for aggregation

   Note on Radiant's tokenizer, learned the hard way while writing this:
   TokenAvailable() does not skip comments, and GetToken( false ) only warns
   when it crosses a line rather than refusing to. Scoping arguments to a line
   therefore requires comparing scriptline across the call and ungetting.
 */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define MAXTOKEN 1024
typedef int qboolean;
static const int SYS_WRN = 2;
static void Sys_FPrintf( int, const char*, ... ){}
static void Error( const char* message, ... ){
	fprintf( stderr, "radiant tokenizer error: %s\n", message );
	exit( 3 );
}

#include "radiant_parse.inc"
#include "iscriplib.h"

// =============================================================================

static char* script_token(){ return token; }
static int script_line(){ return scriptline; }

// The plugin receives this table through Synapse.  The harness binds the real
// parser entry points into the same table shape, so line-scoped reads exercise
// the contract that production uses rather than a private token interface.
static _QERScripLibTable g_scriptLib = {
	1.0f, sizeof( _QERScripLibTable ),
	GetToken, NULL, UngetToken, script_token, StartTokenParsing, script_line,
	NULL, NULL, NULL
};

struct Stage
{
	std::string map, src, dst;
	float fps;
	int frames;
	Stage() : src( "GL_ONE" ), dst( "GL_ZERO" ), fps( 0.0f ), frames( 0 ) {}
};

static bool ieq( const std::string& a, const char* b ){
	size_t i = 0;
	for ( ; i < a.size() && b[i]; ++i ) {
		if ( tolower( (unsigned char)a[i] ) != tolower( (unsigned char)b[i] ) ) return false;
	}
	return i == a.size() && !b[i];
}

static const char* KNOWN_FACTORS[] = {
	"GL_ZERO", "GL_ONE", "GL_SRC_COLOR", "GL_ONE_MINUS_SRC_COLOR",
	"GL_DST_COLOR", "GL_ONE_MINUS_DST_COLOR", "GL_SRC_ALPHA",
	"GL_ONE_MINUS_SRC_ALPHA", "GL_DST_ALPHA", "GL_ONE_MINUS_DST_ALPHA", 0
};
static bool known_factor( const std::string& t ){
	for ( int i = 0; KNOWN_FACTORS[i]; ++i ) if ( ieq( t, KNOWN_FACTORS[i] ) ) return true;
	return false;
}

static long c_defs, c_stages, c_explicitBlend, c_shorthandBlend, c_unknownBlend, c_unknownFactor;
static long c_lightmap, c_whiteimage, c_clampmap, c_animmap, c_tcmod;

static char* slurp( const char* path, size_t& size ){
	FILE* f = fopen( path, "rb" );
	if ( !f ) return 0;
	fseek( f, 0, SEEK_END ); long n = ftell( f ); fseek( f, 0, SEEK_SET );
	char* buf = (char*)malloc( n + 1 );
	size = fread( buf, 1, n, f );
	buf[size] = 0;
	fclose( f );
	return buf;
}

static bool next_token_on_line( int line, std::string& value ){
	if ( !g_scriptLib.m_pfnGetToken( true ) ) return false;
	value = g_scriptLib.m_pfnToken();
	if ( g_scriptLib.m_pfnScriptLine() != line || value == "{" || value == "}" ) {
		g_scriptLib.m_pfnUnGetToken();
		return false;
	}
	return true;
}

static void walk_stages( char* buf, bool census ){
	g_scriptLib.m_pfnStartTokenParsing( buf );
	int depth = 0;
	std::string name;
	std::vector<Stage> stages;
	Stage* cur = 0;

	while ( g_scriptLib.m_pfnGetToken( true ) ) {
		std::string t = g_scriptLib.m_pfnToken();

		if ( t == "{" ) {
			++depth;
			if ( depth == 2 ) { stages.push_back( Stage() ); cur = &stages.back(); ++c_stages; }
			continue;
		}
		if ( t == "}" ) {
			--depth;
			if ( depth == 1 ) cur = 0;
			else if ( depth == 0 && !name.empty() ) {
				++c_defs;
				if ( !census ) {
					printf( "%s\t%d", name.c_str(), (int)stages.size() );
					for ( size_t i = 0; i < stages.size(); ++i )
						printf( "|%s;%s;%s;%.3f;%d", stages[i].map.c_str(), stages[i].src.c_str(),
							stages[i].dst.c_str(), stages[i].fps, stages[i].frames );
					printf( "\n" );
				}
				name.clear(); stages.clear();
			}
			continue;
		}
		if ( depth == 0 ) { name = t; stages.clear(); continue; }
		if ( depth != 2 || !cur ) continue;

		std::vector<std::string> args;
		const int directiveLine = g_scriptLib.m_pfnScriptLine();
		for (;;) {
			std::string argument;
			if ( !next_token_on_line( directiveLine, argument ) ) break;
			args.push_back( argument );
		}

		if ( ieq( t, "map" ) || ieq( t, "clampmap" ) ) {
			if ( ieq( t, "clampmap" ) ) ++c_clampmap;
			if ( !args.empty() ) {
				cur->map = args[0];
				if ( ieq( args[0], "$lightmap" ) ) ++c_lightmap;
				else if ( ieq( args[0], "$whiteimage" ) ) ++c_whiteimage;
			}
		}
		else if ( ieq( t, "animMap" ) ) {
			++c_animmap;
			if ( !args.empty() ) { cur->fps = (float)atof( args[0].c_str() ); cur->frames = (int)args.size() - 1; }
		}
		else if ( ieq( t, "blendFunc" ) ) {
			if ( args.size() == 1 ) {
				if ( ieq( args[0], "add" ) ) { cur->src = "GL_ONE"; cur->dst = "GL_ONE"; ++c_shorthandBlend; }
				else if ( ieq( args[0], "filter" ) ) { cur->src = "GL_DST_COLOR"; cur->dst = "GL_ZERO"; ++c_shorthandBlend; }
				else if ( ieq( args[0], "blend" ) ) { cur->src = "GL_SRC_ALPHA"; cur->dst = "GL_ONE_MINUS_SRC_ALPHA"; ++c_shorthandBlend; }
				else ++c_unknownBlend;
			}
			else if ( args.size() >= 2 ) {
				++c_explicitBlend;
				if ( !known_factor( args[0] ) || !known_factor( args[1] ) ) ++c_unknownFactor;
				cur->src = args[0]; cur->dst = args[1];
			}
			else ++c_unknownBlend;
		}
		else if ( ieq( t, "tcMod" ) ) ++c_tcmod;
	}
}

int main( int argc, char** argv ){
	if ( argc < 3 ) { fprintf( stderr, "usage: shadershop_verify <mode> <file.shader>\n" ); return 2; }
	const char* mode = argv[1];
	size_t size = 0;
	char* buf = slurp( argv[2], size );
	if ( !buf ) { fprintf( stderr, "cannot open %s\n", argv[2] ); return 1; }

	int rc = 0;
	if ( !strcmp( mode, "--stages" ) ) walk_stages( buf, false );
	else if ( !strcmp( mode, "--census" ) ) {
		walk_stages( buf, true );
		printf( "%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\n",
			c_defs, c_stages, c_explicitBlend, c_shorthandBlend, c_unknownBlend, c_unknownFactor,
			c_lightmap, c_whiteimage, c_clampmap, c_animmap, c_tcmod );
	}
	else { fprintf( stderr, "unknown mode %s\n", mode ); rc = 2; }

	free( buf );
	return rc;
}
