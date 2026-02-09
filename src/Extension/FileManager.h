#pragma once
/******************************************************************************
*
* Biko
*
* FileManager.h
*   Minimalist file explorer / tree-view sidebar panel.
*   Left-docked, dark-mode aware, repository-friendly.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDC_FILEMGR_PANEL   0xFB40
#define IDC_FILEMGR_TREE    0xFB41

//=============================================================================
// Lifecycle
//=============================================================================

BOOL    FileManager_Init(HWND hwndMain);
void    FileManager_Shutdown(void);

//=============================================================================
// Visibility
//=============================================================================

void    FileManager_Toggle(HWND hwndParent);
void    FileManager_Show(HWND hwndParent);
void    FileManager_Hide(void);
BOOL    FileManager_IsVisible(void);

//=============================================================================
// Layout
//=============================================================================

// Called from MsgSize. Consumes width from the left side.
// Modifies *px and *pcx in-place. Returns width consumed.
int     FileManager_Layout(HWND hwndParent, int *px, int *pcx, int y, int cy);

//=============================================================================
// Operations
//=============================================================================

// Open a folder in the file tree.
void    FileManager_OpenFolder(LPCWSTR pszPath);

// Show a browse-for-folder dialog, then open that folder.
void    FileManager_BrowseForFolder(HWND hwndParent);

// Refresh the tree (e.g. after file save).
void    FileManager_Refresh(void);

// Get the panel HWND (may be NULL).
HWND    FileManager_GetPanelHwnd(void);

// Get the current root directory (repo/folder).
const WCHAR* FileManager_GetRootPath(void);

//=============================================================================
// Theme
//=============================================================================

void    FileManager_ApplyDarkMode(void);

#ifdef __cplusplus
}
#endif
