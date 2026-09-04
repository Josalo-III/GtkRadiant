/*
   ShaderShop plugin for GtkRadiant
 */

#include "shadershop.h"

_QERFuncTable_1 g_FuncTable;
_QERShadersTable g_ShadersTable;
_QERScripLibTable g_ScripLibTable;
_QERQglTable g_QglTable;
_QERUIGtkTable g_UIGtkTable;
GtkWidget* g_pMainWidget = NULL;

static const char* PLUGIN_NAME = "ShaderShop";
static const char* PLUGIN_COMMANDS = "ShaderShop...,About";

extern "C" const char* QERPlug_Init( void*, void* pMainWidget ){
	g_pMainWidget = static_cast<GtkWidget*>( pMainWidget );
	return PLUGIN_NAME;
}

extern "C" const char* QERPlug_GetName(){
	return PLUGIN_NAME;
}

extern "C" const char* QERPlug_GetCommandList(){
	return PLUGIN_COMMANDS;
}

extern "C" void QERPlug_Dispatch( const char* command, vec3_t, vec3_t, bool ){
	if ( !strcmp( command, "ShaderShop..." ) ) {
		ShaderShop_Show();
	}
	else if ( !strcmp( command, "About" ) ) {
		g_FuncTable.m_pfnMessageBox(
			g_pMainWidget,
			"ShaderShop\n\nQuake III shader preview and editing plugin.",
			"About ShaderShop",
			MB_OK,
			NULL
		);
	}
}

// =============================================================================
// SYNAPSE

CSynapseServer* g_pSynapseServer = NULL;
CSynapseClientShaderShop g_SynapseClient;

#if __GNUC__ >= 4
#pragma GCC visibility push(default)
#endif
extern "C" CSynapseClient* SYNAPSE_DLL_EXPORT Synapse_EnumerateInterfaces( const char* version, CSynapseServer* server ){
#if __GNUC__ >= 4
#pragma GCC visibility pop
#endif
	if ( strcmp( version, SYNAPSE_VERSION ) ) {
		Syn_Printf( "ERROR: synapse API version mismatch: should be '" SYNAPSE_VERSION "', got '%s'\n", version );
		return NULL;
	}

	g_pSynapseServer = server;
	g_pSynapseServer->IncRef();
	Set_Syn_Printf( g_pSynapseServer->Get_Syn_Printf() );

	g_SynapseClient.AddAPI( PLUGIN_MAJOR, SHADERSHOP_MINOR, sizeof( _QERPluginTable ) );
	g_SynapseClient.AddAPI( RADIANT_MAJOR, NULL, sizeof( _QERFuncTable_1 ), SYN_REQUIRE, &g_FuncTable );
	// SYN_REQUIRE_ANY is an internal Synapse resolver type, not a valid client
	// registration type. Use the normal wildcard requirement, as the other
	// shader-consuming modules do, so the table is actually populated.
	g_SynapseClient.AddAPI( SHADERS_MAJOR, "*", sizeof( _QERShadersTable ), SYN_REQUIRE, &g_ShadersTable );
	g_SynapseClient.AddAPI( SCRIPLIB_MAJOR, NULL, sizeof( _QERScripLibTable ), SYN_REQUIRE, &g_ScripLibTable );
	g_SynapseClient.AddAPI( QGL_MAJOR, NULL, sizeof( _QERQglTable ), SYN_REQUIRE, &g_QglTable );
	g_SynapseClient.AddAPI( UIGTK_MAJOR, NULL, sizeof( _QERUIGtkTable ), SYN_REQUIRE, &g_UIGtkTable );

	return &g_SynapseClient;
}

bool CSynapseClientShaderShop::RequestAPI( APIDescriptor_t* pAPI ){
	if ( !strcmp( pAPI->major_name, PLUGIN_MAJOR ) ) {
		_QERPluginTable* table = static_cast<_QERPluginTable*>( pAPI->mpTable );
		table->m_pfnQERPlug_Init = QERPlug_Init;
		table->m_pfnQERPlug_GetName = QERPlug_GetName;
		table->m_pfnQERPlug_GetCommandList = QERPlug_GetCommandList;
		table->m_pfnQERPlug_Dispatch = QERPlug_Dispatch;
		return true;
	}

	Syn_Printf( "ERROR: RequestAPI( '%s' ) not found in '%s'\n", pAPI->major_name, GetInfo() );
	return false;
}

#include "version.h"

const char* CSynapseClientShaderShop::GetInfo(){
	return "ShaderShop built " __DATE__ " " RADIANT_VERSION;
}

const char* CSynapseClientShaderShop::GetName(){
	return "shadershop";
}
