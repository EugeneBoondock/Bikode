#pragma once
/******************************************************************************
*
* Biko
*
* Terminal.h
*   Embedded terminal panel using Windows ConPTY (Pseudo Console) API.
*   Falls back to CreateProcess + pipe for older Windows versions.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDC_TERMINAL_PANEL  0xFB30
#define IDC_TERMINAL_VIEW   0xFB31

//=============================================================================
// Lifecycle
//=============================================================================

// Initialize terminal subsystem.
BOOL    Terminal_Init(HWND hwndMain);

// Shutdown all terminal instances.
void    Terminal_Shutdown(void);

//=============================================================================
// Terminal management
//=============================================================================

// Create and show a new terminal instance (uses active shell).
BOOL    Terminal_New(HWND hwndParent);

// Create a new terminal with a specific shell type (0=PowerShell, 1=CMD, 2=Git Bash, 3=WSL).
BOOL    Terminal_NewShell(HWND hwndParent, int shellType);

// Toggle terminal panel visibility.
void    Terminal_Toggle(HWND hwndParent);

// Show/hide terminal.
void    Terminal_Show(HWND hwndParent);
void    Terminal_Hide(void);

// Returns TRUE if the terminal panel is visible.
BOOL    Terminal_IsVisible(void);

//=============================================================================
// Layout
//=============================================================================

// Recalculate terminal position (call from WM_SIZE).
// Returns the height consumed by the terminal panel (0 if hidden).
int     Terminal_Layout(HWND hwndParent, int parentWidth, int parentBottom);

//=============================================================================
// Input
//=============================================================================

// Write a string to the terminal's stdin.
void    Terminal_Write(const char* pszData, int len);

// Write a command and press Enter.
void    Terminal_SendCommand(const char* pszCommand);

// Focus the terminal view.
void    Terminal_Focus(void);

// Returns the terminal panel HWND (or NULL if not created).
HWND    Terminal_GetPanelHwnd(void);

// Re-apply dark/light mode styles to terminal Scintilla view and panel.
void    Terminal_ApplyDarkMode(void);

// Returns TRUE if the terminal believes it should hold keyboard focus.
// Used by the main window to prevent focus-stealing from the terminal.
BOOL    Terminal_WantsFocus(void);

// Notify terminal that focus was explicitly given to another component.
void    Terminal_RelinquishFocus(void);

//=============================================================================
// Run command
//=============================================================================

// Run a command in the terminal (creates a new terminal if none exists).
void    Terminal_RunCommand(HWND hwndParent, const char* pszCommand);

// Append transcript text to the active terminal session and auto-show the panel without stealing focus.
void    Terminal_AppendTranscript(HWND hwndParent, const char* pszText);

#ifdef __cplusplus
}
#endif
