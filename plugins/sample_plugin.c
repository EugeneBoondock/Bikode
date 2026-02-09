/******************************************************************************
*
* Biko Sample Plugin
*
* sample_plugin.c
*   Example plugin demonstrating the Biko Plugin API.
*   Build with: cl /LD /DBIKO_PLUGIN_EXPORTS sample_plugin.c
*
******************************************************************************/

#define BIKO_PLUGIN_EXPORTS
#include "BikoPluginAPI.h"
#include <stdio.h>

//=============================================================================
// Plugin State
//=============================================================================

static const BikoHostServices* g_pHost = NULL;
static UINT g_cmdUppercase = 0;
static UINT g_cmdLowercase = 0;
static UINT g_cmdLineCount = 0;

//=============================================================================
// Plugin Info
//=============================================================================

static BikoPluginInfo s_pluginInfo = {
    BIKO_PLUGIN_API_VERSION,
    L"Sample Plugin",
    L"Biko Team",
    L"1.0.0",
    L"Demonstrates plugin API with text case commands"
};

//=============================================================================
// Required Exports
//=============================================================================

BIKO_PLUGIN_API BikoPluginInfo* BikoPlugin_GetInfo(void)
{
    return &s_pluginInfo;
}

BIKO_PLUGIN_API BOOL BikoPlugin_Init(const BikoHostServices* pHost)
{
    g_pHost = pHost;
    return TRUE;
}

BIKO_PLUGIN_API void BikoPlugin_Shutdown(void)
{
    g_pHost = NULL;
}

//=============================================================================
// Optional Exports - Menu Items
//=============================================================================

BIKO_PLUGIN_API int BikoPlugin_GetMenuItemCount(void)
{
    return 3;
}

BIKO_PLUGIN_API BOOL BikoPlugin_GetMenuItem(int index, BikoMenuItem* pItem)
{
    if (!pItem) return FALSE;
    
    switch (index)
    {
    case 0:
        wcscpy_s(pItem->name, 64, L"&Uppercase Selection");
        wcscpy_s(pItem->shortcut, 32, L"");
        pItem->separator = FALSE;
        g_cmdUppercase = pItem->cmdId = 0;  // Will be assigned by host
        return TRUE;
        
    case 1:
        wcscpy_s(pItem->name, 64, L"&Lowercase Selection");
        pItem->shortcut[0] = 0;
        pItem->separator = FALSE;
        g_cmdLowercase = pItem->cmdId = 0;
        return TRUE;
        
    case 2:
        wcscpy_s(pItem->name, 64, L"Show &Line Count");
        pItem->shortcut[0] = 0;
        pItem->separator = TRUE;  // Separator before this item
        g_cmdLineCount = pItem->cmdId = 0;
        return TRUE;
        
    default:
        return FALSE;
    }
}

BIKO_PLUGIN_API BOOL BikoPlugin_OnCommand(UINT cmdId)
{
    if (!g_pHost) return FALSE;
    
    // Note: We need to check relative offset since cmdId was assigned by host
    // The host assigns sequential IDs starting from menuCmdBase
    
    // Simple approach: just check which command based on order
    WCHAR buffer[4096];
    
    // Get the relative command index (0, 1, or 2)
    // This works because we know our commands are sequential
    if (cmdId == g_cmdUppercase || (g_cmdUppercase == 0 && cmdId % 3 == 0))
    {
        int len = g_pHost->GetSelection(buffer, 4096);
        if (len > 0)
        {
            CharUpperW(buffer);
            g_pHost->BeginUndoAction();
            g_pHost->ReplaceSelection(buffer);
            g_pHost->EndUndoAction();
            g_pHost->SetStatusText(L"Converted to uppercase");
        }
        return TRUE;
    }
    
    if (cmdId == g_cmdLowercase || (g_cmdLowercase == 0 && cmdId % 3 == 1))
    {
        int len = g_pHost->GetSelection(buffer, 4096);
        if (len > 0)
        {
            CharLowerW(buffer);
            g_pHost->BeginUndoAction();
            g_pHost->ReplaceSelection(buffer);
            g_pHost->EndUndoAction();
            g_pHost->SetStatusText(L"Converted to lowercase");
        }
        return TRUE;
    }
    
    if (cmdId == g_cmdLineCount || (g_cmdLineCount == 0 && cmdId % 3 == 2))
    {
        int lines = g_pHost->GetLineCount();
        WCHAR msg[128];
        wsprintfW(msg, L"Document has %d lines", lines);
        g_pHost->ShowMessageBox(msg, L"Line Count", MB_OK | MB_ICONINFORMATION);
        return TRUE;
    }
    
    return FALSE;
}
