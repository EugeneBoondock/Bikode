#pragma once
/******************************************************************************
*
* Biko
*
* AICommands.h
*   Command handlers for AI menu items and hotkeys.
*   Wires up context extraction, AI requests, and response handling.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Command IDs (reserved range 41000-41099)
//=============================================================================

#define IDM_AI_TRANSFORM        41000
#define IDM_AI_REFACTOR         41001
#define IDM_AI_EXPLAIN          41002
#define IDM_AI_FIX              41003
#define IDM_AI_COMPLETE         41004
#define IDM_AI_CHAT             41005

#define IDM_AI_APPLY_PATCH      41010
#define IDM_AI_REJECT_PATCH     41011
#define IDM_AI_NEXT_HUNK        41012
#define IDM_AI_PREV_HUNK        41013
#define IDM_AI_TOGGLE_HUNK      41014
#define IDM_AI_REFINE_PATCH     41015

#define IDM_AI_CANCEL           41020
#define IDM_AI_SETTINGS         41030
#define IDM_AI_RESTART_ENGINE   41031
#define IDM_AI_TOGGLE_CHAT      41032
#define IDM_AI_TOGGLE_TERMINAL  41033
#define IDM_AI_TOGGLE_MARKDOWN  41034
#define IDM_AI_TOGGLE_PROOF     41035
#define IDM_BIKO_COMMAND_PALETTE 41036
#define IDM_AI_MISSION_CONTROL  41037
#define IDM_AI_DUPLICATE_ORG    41038
#define IDM_AI_RUN_ACTIVE_ORG   41039

// Dark mode
#define IDM_VIEW_DARKMODE       41040

// Git
#define IDM_GIT_STATUS          41050
#define IDM_GIT_DIFF            41051
#define IDM_GIT_COMMIT          41052
#define IDM_GIT_LOG             41053
#define IDM_GIT_TOGGLE_PANEL    41054
#define IDM_GIT_PULL            41055
#define IDM_GIT_PUSH            41056
#define IDM_GIT_BLAME           41057
#define IDM_GIT_BRANCHES        41058
#define IDM_GIT_STASH           41059

// Terminal
#define IDM_TERMINAL_TOGGLE     41060
#define IDM_TERMINAL_NEW        41061

// Markdown
#define IDM_MARKDOWN_PREVIEW    41070

// File Manager
#define IDM_FILEMGR_TOGGLE      41080
#define IDM_FILEMGR_OPENFOLDER  41081

//=============================================================================
// Initialization
//=============================================================================

// Initialize AI commands (call from WM_CREATE).
void    AICommands_Init(HWND hwndMain, HWND hwndEdit);

// Shutdown AI commands (call from WM_DESTROY).
void    AICommands_Shutdown(void);

//=============================================================================
// Command dispatch (call from MsgCommand)
//=============================================================================

// Returns TRUE if the command was handled.
BOOL    AICommands_HandleCommand(HWND hwndMain, UINT cmd);

//=============================================================================
// Message handlers (call from MainWndProc)
//=============================================================================

// Handle WM_AI_* messages. Returns TRUE if handled.
BOOL    AICommands_HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

//=============================================================================
// Menu management
//=============================================================================

// Add AI submenu to the main menu bar.
void    AICommands_CreateMenu(HMENU hMainMenu);

// Update AI menu item states (enable/disable based on connection status).
void    AICommands_UpdateMenu(HMENU hMainMenu);

//=============================================================================
// Status bar
//=============================================================================

// Get the AI status text for the status bar.
void    AICommands_GetStatusText(WCHAR* wszBuf, int cchBuf);

//=============================================================================
// Transform dialog
//=============================================================================

// Show the "Transform..." instruction input dialog.
// Returns TRUE if user confirmed, with instruction in wszInstruction.
BOOL    AICommands_ShowTransformDialog(HWND hwndParent,
            WCHAR* wszInstruction, int cchInstruction);

#ifdef __cplusplus
}
#endif
