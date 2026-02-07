#pragma once
/******************************************************************************
*
* Biko
*
* ChatPanel.h
*   Dockable chat panel using a Scintilla control for output display
*   and a standard edit control for user input.
*   Docks to the bottom of the editor window.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

// Chat panel control IDs
#define IDC_CHAT_PANEL      0xFB20
#define IDC_CHAT_OUTPUT     0xFB21
#define IDC_CHAT_INPUT      0xFB22
#define IDC_CHAT_SEND       0xFB23

//=============================================================================
// Lifecycle
//=============================================================================

// Create the chat panel (call from WM_CREATE or on first toggle).
BOOL    ChatPanel_Create(HWND hwndParent);

// Destroy the chat panel.
void    ChatPanel_Destroy(void);

//=============================================================================
// Visibility
//=============================================================================

// Toggle chat panel visibility.
void    ChatPanel_Toggle(HWND hwndParent);

// Show/hide explicitly.
void    ChatPanel_Show(HWND hwndParent);
void    ChatPanel_Hide(void);

BOOL    ChatPanel_IsVisible(void);

//=============================================================================
// Layout
//=============================================================================

// Recalculate chat panel position (call from WM_SIZE).
// Returns the width consumed by the chat panel (0 if hidden).
// parentRight = total client width, editorTop/editorHeight = editor area.
int     ChatPanel_Layout(HWND hwndParent, int parentRight, int editorTop, int editorHeight);

//=============================================================================
// Content
//=============================================================================

// Append a user message to the chat output.
void    ChatPanel_AppendUserMessage(const char* pszMessage);

// Append an AI response to the chat output.
void    ChatPanel_AppendResponse(const char* pszResponse);

// Append a system/status message.
void    ChatPanel_AppendSystem(const char* pszMessage);

// Clear all chat content.
void    ChatPanel_Clear(void);

//=============================================================================
// Input handling
//=============================================================================

// Process Enter key in the input field â€” sends the message.
void    ChatPanel_SendInput(void);

// Focus the input field.
void    ChatPanel_FocusInput(void);

//=============================================================================
// Window procedure (for subclassing)
//=============================================================================

// Handle WM_COMMAND for chat panel controls.
BOOL    ChatPanel_HandleCommand(WPARAM wParam, LPARAM lParam);

// Handle WM_NOTIFY for the chat Scintilla output.
BOOL    ChatPanel_HandleNotify(LPARAM lParam);

// Refresh dark/light mode styling on all chat panel controls.
void    ChatPanel_ApplyDarkMode(void);

#ifdef __cplusplus
}
#endif
