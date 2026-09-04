/*
   ShaderShop parser verification

   Exercises the stage-model walk over Radiant's own ScriptLib tokenizer and
   reports a census of the shader corpus.  ShaderShop no longer has a lexical
   tokenizer to compare: production uses this same parser API.

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

// =============================================================================

struct Stage
{
	std::string map, src, dst;
	float fps;
	int frames;
	Stage() : src( "GL_SRC_ALPHA" ), dst( "GL_ONE_MINUS_SRC_ALPHA" ), fps( 0.0f ), frames( 0 ) {}
};

static bool ieq( const std::string& a, const char* b ){
	size_t i = 0;
	for ( ; i < a.size() && b[i]; ++i ) {
		if ( tolower( (unsigned char)a[i] ) != tolower( (unsigned char)b[i] ) ) return false;
	}
	return i == a.size() && !b[i];
}

static const char* KNOWN_FACTORS[] = {
	"GL_ONE", "GL_ZERO", "GL_DST_COLOR", "GL_ONE_MINUS_DST_COLOR",
	"GL_SRC_ALPHA", "GL_ONE_MINUS_SRC_ALPHA", "GL_DST_ALPHA", "GL_ONE_MINUS_DST_ALPHA",
	"one", "zero", "dst_color", "one_minus_dst_color",
	"src_alpha", "one_minus_src_alpha", "dst_alpha", "one_minus_dst_alpha", 0
};
static bool known_factor( const std::string& t ){
	for ( int i = 0; KNOWN_FACTORS[i]; ++i ) if ( t == KNOWN_FACTORS[i] ) return true;
	return false;
}

static long c_defs, c_blend, c_kw, c_short, c_factor, c_wrong, c_srccolor;
static long c_lightmap, c_whiteimage, c_clampmap, c_animmap;

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
	if ( !GetToken( true ) ) return false;
	value = token;
	if ( scriptline != line || value == "{" || value == "}" ) {
		UngetToken();
		return false;
	}
	return true;
}

static void walk_stages( char* buf, bool census ){
	StartTokenParsing( buf );
	int depth = 0;
	std::string name;
	std::vector<Stage> stages;
	Stage* cur = 0;

	while ( GetToken( true ) ) {
		std::string t = token;

		if ( t == "{" ) {
			++depth;
			if ( depth == 2 ) { stages.push_back( Stage() ); cur = &stages.back(); }
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
		const int directiveLine = scriptline;
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
			++c_blend;
			bool bad = false;
			if ( t != "blendFunc" ) { ++c_kw; bad = true; }
			if ( args.size() < 2 ) { ++c_short; bad = true; }
			else {
				if ( !known_factor( args[0] ) || !known_factor( args[1] ) ) {
					++c_factor; bad = true;
					std::string j = args[0] + args[1];
					for ( size_t i = 0; i < j.size(); ++i ) j[i] = toupper( (unsigned char)j[i] );
					if ( j.find( "SRC_COLOR" ) != std::string::npos ) ++c_srccolor;
				}
				cur->src = args[0]; cur->dst = args[1];
			}
			if ( bad ) ++c_wrong;
		}
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
			c_defs, c_blend, c_kw, c_short, c_factor, c_wrong, c_srccolor,
			c_lightmap, c_whiteimage, c_clampmap, c_animmap );
	}
	else { fprintf( stderr, "unknown mode %s\n", mode ); rc = 2; }

	free( buf );
	return rc;
}
