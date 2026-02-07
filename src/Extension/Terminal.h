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

// Create and show a new terminal instance.
BOOL    Terminal_New(HWND hwndParent);

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

//=============================================================================
// Run command
//=============================================================================

// Run a command in the terminal (creates a new terminal if none exists).
void    Terminal_RunCommand(HWND hwndParent, const char* pszCommand);

#ifdef __cplusplus
}
#endif
