/******************************************************************************
*
* Biko
*
* BikoToolbar.h
*   Custom owner-drawn toolbar for dark mode support.
*   Replaces the classic Win32 rebar+toolbar that refuses dark theming.
*
******************************************************************************/

#ifndef BIKO_TOOLBAR_H
#define BIKO_TOOLBAR_H

#include <windows.h>

//=============================================================================
// Public API
//=============================================================================

// Initialize the custom toolbar (call once from MsgCreate)
HWND    BikoToolbar_Create(HWND hwndParent, HINSTANCE hInst);

// Destroy (call on WM_DESTROY)
void    BikoToolbar_Destroy(void);

// Layout: returns the toolbar height. Call from MsgSize.
int     BikoToolbar_GetHeight(void);

// Enable/disable a toolbar button by command ID
void    BikoToolbar_EnableButton(int cmdId, BOOL bEnable);

// Check/uncheck a toolbar button by command ID (toggle state)
void    BikoToolbar_CheckButton(int cmdId, BOOL bCheck);

// Refresh dark mode colors
void    BikoToolbar_ApplyDarkMode(void);

// Get the toolbar window handle
HWND    BikoToolbar_GetHwnd(void);

// Show/hide
void    BikoToolbar_Show(BOOL bShow);

// Called when toolbar visibility toggled
BOOL    BikoToolbar_IsVisible(void);

#endif // BIKO_TOOLBAR_H
