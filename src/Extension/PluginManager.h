#pragma once
/******************************************************************************
*
* Biko
*
* PluginManager.h
*   Lightweight plugin manager - loads DLLs from plugins/ folder,
*   routes commands, builds menu. Zero overhead if no plugins present.
*
******************************************************************************/

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Command ID Range
//=============================================================================

#define IDM_PLUGIN_SETTINGS     41100
#define IDM_PLUGIN_RELOAD       41101
#define IDM_PLUGIN_CMD_BASE     41200   // Base for plugin commands
#define IDM_PLUGIN_CMD_MAX      41999   // Max 800 plugin commands

//=============================================================================
// Initialization / Shutdown
//=============================================================================

// Initialize plugin manager (call from WM_CREATE).
// Does NOT load plugins - call PluginManager_LoadAll separately.
void PluginManager_Init(HWND hwndMain, HWND hwndEdit);

// Shutdown and unload all plugins (call from WM_DESTROY).
void PluginManager_Shutdown(void);

//=============================================================================
// Plugin Loading
//=============================================================================

// Load all enabled plugins from plugins/ folder.
// Called after main window is fully created.
void PluginManager_LoadAll(void);

// Reload all plugins (unload then load).
void PluginManager_ReloadAll(void);

// Get number of loaded plugins.
int PluginManager_GetPluginCount(void);

//=============================================================================
// Command Dispatch
//=============================================================================

// Handle a command in the plugin range (41200-41999).
// Returns TRUE if handled.
BOOL PluginManager_HandleCommand(HWND hwnd, UINT cmd);

//=============================================================================
// Menu
//=============================================================================

// Create the Plugins submenu and add to main menu.
void PluginManager_CreateMenu(HMENU hMainMenu);

// Update menu item states (enable/disable).
void PluginManager_UpdateMenu(HMENU hMainMenu);

//=============================================================================
// Notifications
//=============================================================================

// Notify plugins that the document changed.
void PluginManager_NotifyDocumentChanged(void);

#ifdef __cplusplus
}
#endif
