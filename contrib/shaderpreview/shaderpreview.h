/*
   Shader Preview plugin for GtkRadiant
*/

#ifndef _SHADERPREVIEW_H_
#define _SHADERPREVIEW_H_

#include <gtk/gtk.h>

#include "synapse.h"
#include "iplugin.h"
#include "qerplugin.h"
#include "ishaders.h"
#include "igl.h"
#include "iui_gtk.h"

#define SHADERPREVIEW_MINOR "shaderpreview"

extern _QERFuncTable_1 g_FuncTable;
extern _QERShadersTable g_ShadersTable;
extern _QERQglTable g_QglTable;
extern _QERUIGtkTable g_UIGtkTable;
extern GtkWidget* g_pMainWidget;

void ShaderPreview_Show();
void ShaderPreview_RefreshSelection();

class CSynapseClientShaderPreview : public CSynapseClient
{
public:
	bool RequestAPI( APIDescriptor_t* pAPI );
	const char* GetInfo();
	const char* GetName();
};

#endif
