#pragma once
/******************************************************************************
*
* Biko
*
* DarkMode.h
*   Windows 10/11 dark mode support.
*   - Dark title bar via DwmSetWindowAttribute or SetPreferredAppMode
*   - Custom colors for toolbar, status bar, tab bar
*   - Scintilla dark theme switching
*   - Dialog/control theming via uxtheme
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Lifecycle
//=============================================================================

// Initialize dark mode support. Call early (before window creation ideally).
void    DarkMode_Init(HWND hwndMain);

// Cleanup.
void    DarkMode_Shutdown(void);

//=============================================================================
// State
//=============================================================================

// Toggle dark mode on/off.
void    DarkMode_Toggle(void);

// Returns TRUE if dark mode is currently active.
BOOL    DarkMode_IsEnabled(void);

// Returns TRUE if the host OS supports dark mode (Win10 1809+).
BOOL    DarkMode_IsSupported(void);

// Force a specific mode.
void    DarkMode_SetEnabled(BOOL bEnable);

//=============================================================================
// Theme application
//=============================================================================

// Apply dark/light theme to the main window chrome (title bar, frame).
void    DarkMode_ApplyToWindow(HWND hwnd);

// Apply dark/light theme to a Scintilla editor control.
void    DarkMode_ApplyToEditor(HWND hwndEdit);

// Apply dark/light theme to toolbar.
void    DarkMode_ApplyToToolbar(HWND hwndToolbar);

// Apply dark/light theme to status bar.
void    DarkMode_ApplyToStatusBar(HWND hwndStatus);

// Apply dark/light theme to a dialog (call from WM_INITDIALOG).
void    DarkMode_ApplyToDialog(HWND hwndDlg);

// Apply dark/light theme to all known app windows (editor, toolbar, status bar, rebar).
// Call after toggling dark mode to refresh the entire UI.
void    DarkMode_ApplyAll(HWND hwndMain, HWND hwndEdit, HWND hwndToolbar,
                         HWND hwndStatus, HWND hwndReBar);

// Handle WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC / WM_CTLCOLORBTN for dark mode.
// Returns the brush to use, or NULL if not in dark mode.
HBRUSH  DarkMode_HandleCtlColor(HDC hdc);

//=============================================================================
// Drawing helpers
//=============================================================================

// Get the current background color.
COLORREF DarkMode_GetBackgroundColor(void);

// Get the current text color.
COLORREF DarkMode_GetTextColor(void);

// Get the current accent/highlight color.
COLORREF DarkMode_GetAccentColor(void);

// Get a custom dark-mode brush for owner-draw.
HBRUSH  DarkMode_GetBackgroundBrush(void);

//=============================================================================
// Color scheme values
//=============================================================================

typedef struct DarkModeColors {
    COLORREF clrBackground;
    COLORREF clrSurface;
    COLORREF clrText;
    COLORREF clrTextDim;
    COLORREF clrBorder;
    COLORREF clrAccent;
    COLORREF clrSelection;
    COLORREF clrCaretLine;
    COLORREF clrLineNumber;
    COLORREF clrIndentGuide;
    COLORREF clrComment;
    COLORREF clrKeyword;
    COLORREF clrString;
    COLORREF clrNumber;
    COLORREF clrOperator;
    COLORREF clrType;
    COLORREF clrFunction;
    COLORREF clrPreprocessor;
} DarkModeColors;

// Returns the current dark mode color scheme.
const DarkModeColors* DarkMode_GetColors(void);

#ifdef __cplusplus
}
#endif
