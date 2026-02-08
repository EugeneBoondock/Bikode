/******************************************************************************
*
* Biko
*
* BikoMenuBar.h
*   Custom owner-drawn menu bar with full dark mode support.
*   Replaces the standard Win32 menu bar which ignores dark theming.
*
*   Architecture:
*   - A child window (class "BikoMenuBar") is created at the top of the
*     main window, just above the toolbar.
*   - The real Win32 HMENU is kept but removed from the window frame
*     (SetMenu(hwnd, NULL)).  The menu bar child reads the HMENU structure
*     to discover top-level items and their popup submenus.
*   - Clicking a menu item opens the popup via TrackPopupMenuEx.
*   - Keyboard navigation (Alt, F10, arrow keys, mnemonics) is handled
*     through a message hook so accelerators keep working.
*   - MsgInitMenu / WM_COMMAND from the popups are forwarded to the parent,
*     so existing command handling is untouched.
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

// Create the custom menu bar.  Pass the HMENU currently attached to the
// main window (GetMenu(hwnd)).  The function will detach it from the
// window and take ownership of rendering.  Call from MsgCreate after
// AICommands_CreateMenu so all dynamic menus are present.
HWND BikoMenuBar_Create(HWND hwndParent, HINSTANCE hInst, HMENU hMenu);

// Destroy (call on WM_DESTROY, before DestroyMenu if applicable).
void BikoMenuBar_Destroy(void);

// Returns the height in pixels.  0 if hidden.
int  BikoMenuBar_GetHeight(void);

// Show / hide.
void BikoMenuBar_Show(BOOL bShow);

// Query visibility.
BOOL BikoMenuBar_IsVisible(void);

// Get the underlying window handle.
HWND BikoMenuBar_GetHwnd(void);

// Refresh after dark-mode toggle.
void BikoMenuBar_ApplyDarkMode(void);

// Call when the HMENU has been modified at runtime (e.g. AICommands_CreateMenu
// appends a popup).  Re-reads all top-level items.
void BikoMenuBar_RefreshMenu(void);

// Forward WM_INITMENU / WM_INITMENUPOPUP to allow MsgInitMenu to
// enable/disable items before popup display.
void BikoMenuBar_ForwardInitMenu(HWND hwndParent);

// Returns TRUE if the menu bar currently has keyboard focus (alt-mode).
BOOL BikoMenuBar_HasFocus(void);

// Call from the parent's WndProc for messages that the menu bar may
// want to intercept (WM_SYSKEYDOWN/UP, WM_KEYDOWN for arrows/enter/esc).
// Returns TRUE if the message was consumed.
BOOL BikoMenuBar_HandleParentMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam, LRESULT *pResult);

#ifdef __cplusplus
}
#endif
