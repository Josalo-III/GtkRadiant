/*
   ShaderShop plugin for GtkRadiant
 */

#ifndef _SHADERSHOP_H_
#define _SHADERSHOP_H_

#include <gtk/gtk.h>

#include "synapse.h"
#include "iplugin.h"
#include "qerplugin.h"
#include "ishaders.h"
#include "igl.h"
#include "iui_gtk.h"

#define SHADERSHOP_MINOR "shadershop"

extern _QERFuncTable_1 g_FuncTable;
extern _QERShadersTable g_ShadersTable;
extern _QERQglTable g_QglTable;
extern _QERUIGtkTable g_UIGtkTable;
extern GtkWidget* g_pMainWidget;

void ShaderShop_Show();
void ShaderShop_RefreshSelection();

class CSynapseClientShaderShop : public CSynapseClient
{
public:
	bool RequestAPI( APIDescriptor_t* pAPI );
	const char* GetInfo();
	const char* GetName();
};

#endif
