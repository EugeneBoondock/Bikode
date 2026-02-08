/******************************************************************************
*
* Biko
*
* BikoStatusBar.h
*   Custom owner-drawn status bar with full dark mode support.
*   Replaces the standard Win32 status bar which ignores dark theming.
*
******************************************************************************/

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Public API
//=============================================================================

// Create the custom status bar (call from CreateBars or MsgCreate).
HWND BikoStatusBar_Create(HWND hwndParent, HINSTANCE hInst);

// Destroy (call on WM_DESTROY).
void BikoStatusBar_Destroy(void);

// Returns the height in pixels.  0 if hidden.
int  BikoStatusBar_GetHeight(void);

// Show / hide.
void BikoStatusBar_Show(BOOL bShow);

// Query visibility.
BOOL BikoStatusBar_IsVisible(void);

// Get the underlying window handle.
HWND BikoStatusBar_GetHwnd(void);

// Refresh after dark-mode toggle.
void BikoStatusBar_ApplyDarkMode(void);

// Set the text for a given part index.
void BikoStatusBar_SetText(int iPart, LPCWSTR szText);

// Set part widths (array of right-edge positions, -1 for last = fill).
// nParts = number of parts, pWidths = array of right-edge coords.
void BikoStatusBar_SetParts(int nParts, const int *pWidths);

// Calculate the pixel width needed for a given text string (for sizing parts).
int  BikoStatusBar_CalcPaneWidth(LPCWSTR szText);

// Forward click/dblclick from parent WM_NOTIFY or WM_COMMAND.
// Returns the 0-based part index at (x,y) in screen coords, or -1.
int  BikoStatusBar_HitTest(int xScreen, int yScreen);

#ifdef __cplusplus
}
#endif
