#pragma once
/******************************************************************************
*
* Biko Plugin API
*
* BikoPluginAPI.h
*   Stable C API for Biko plugins. Plugins are DLLs that export the
*   required functions defined below.
*
* API Version: 1
* License: BSD 3-Clause (same as Biko)
*
******************************************************************************/

#ifndef BIKO_PLUGIN_API_H
#define BIKO_PLUGIN_API_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// API Version
//=============================================================================

#define BIKO_PLUGIN_API_VERSION  1

//=============================================================================
// Plugin Info Structure
//=============================================================================

typedef struct BikoPluginInfo {
    UINT        apiVersion;      // Must be BIKO_PLUGIN_API_VERSION
    LPCWSTR     name;            // Plugin display name
    LPCWSTR     author;          // Author name
    LPCWSTR     version;         // Version string (e.g., "1.0.0")
    LPCWSTR     description;     // Brief description
} BikoPluginInfo;

//=============================================================================
// Host Services (provided by Biko to plugins)
//=============================================================================

typedef struct BikoHostServices {
    UINT        apiVersion;      // Host API version
    HWND        hwndMain;        // Main window handle
    HWND        hwndEdit;        // Scintilla editor handle
    
    // Text access - returns length written, 0 on failure
    int  (*GetText)(LPWSTR buffer, int maxLen);
    int  (*GetSelection)(LPWSTR buffer, int maxLen);
    BOOL (*SetText)(LPCWSTR text);
    BOOL (*ReplaceSelection)(LPCWSTR text);
    BOOL (*InsertText)(int pos, LPCWSTR text);
    
    // Cursor and selection
    int  (*GetCursorPos)(void);
    void (*SetCursorPos)(int pos);
    void (*GetSelectionRange)(int* start, int* end);
    void (*SetSelectionRange)(int start, int end);
    
    // Document info
    int  (*GetTextLength)(void);
    int  (*GetLineCount)(void);
    int  (*GetCurrentLine)(void);
    
    // UI feedback
    void (*SetStatusText)(LPCWSTR text);
    int  (*ShowMessageBox)(LPCWSTR text, LPCWSTR title, UINT type);
    
    // Undo grouping
    void (*BeginUndoAction)(void);
    void (*EndUndoAction)(void);
    
} BikoHostServices;

//=============================================================================
// Menu Item Structure
//=============================================================================

typedef struct BikoMenuItem {
    UINT        cmdId;           // Unique command ID (assigned by host)
    WCHAR       name[64];        // Menu item text
    WCHAR       shortcut[32];    // Optional hotkey text (display only)
    BOOL        separator;       // TRUE to insert separator before this item
} BikoMenuItem;

//=============================================================================
// Required Plugin Exports
//=============================================================================

#ifdef BIKO_PLUGIN_EXPORTS
#define BIKO_PLUGIN_API __declspec(dllexport)
#else
#define BIKO_PLUGIN_API __declspec(dllimport)
#endif

// Get plugin information. Called before Init.
// Return pointer to static BikoPluginInfo structure.
typedef BikoPluginInfo* (*PFN_BikoPlugin_GetInfo)(void);

// Initialize the plugin. Called once when loaded.
// pHost: pointer to host services (valid for plugin lifetime)
// Returns TRUE on success, FALSE to abort loading.
typedef BOOL (*PFN_BikoPlugin_Init)(const BikoHostServices* pHost);

// Shutdown the plugin. Called before unloading.
typedef void (*PFN_BikoPlugin_Shutdown)(void);

//=============================================================================
// Optional Plugin Exports
//=============================================================================

// Get number of menu items the plugin wants to add.
typedef int (*PFN_BikoPlugin_GetMenuItemCount)(void);

// Get menu item at index. Returns TRUE on success.
// cmdId in BikoMenuItem is assigned by the host after this call.
typedef BOOL (*PFN_BikoPlugin_GetMenuItem)(int index, BikoMenuItem* pItem);

// Handle a command. cmdId matches what was returned in GetMenuItem.
// Returns TRUE if handled.
typedef BOOL (*PFN_BikoPlugin_OnCommand)(UINT cmdId);

// Called when document changes (optional notification).
typedef void (*PFN_BikoPlugin_OnDocumentChanged)(void);

//=============================================================================
// Export names (for GetProcAddress)
//=============================================================================

#define BIKO_EXPORT_GETINFO         "BikoPlugin_GetInfo"
#define BIKO_EXPORT_INIT            "BikoPlugin_Init"
#define BIKO_EXPORT_SHUTDOWN        "BikoPlugin_Shutdown"
#define BIKO_EXPORT_GETMENUCOUNT    "BikoPlugin_GetMenuItemCount"
#define BIKO_EXPORT_GETMENUITEM     "BikoPlugin_GetMenuItem"
#define BIKO_EXPORT_ONCOMMAND       "BikoPlugin_OnCommand"
#define BIKO_EXPORT_ONDOCCHANGED    "BikoPlugin_OnDocumentChanged"

#ifdef __cplusplus
}
#endif

#endif // BIKO_PLUGIN_API_H
