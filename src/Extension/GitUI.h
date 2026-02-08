#pragma once
/******************************************************************************
*
* Biko
*
* GitUI.h
*   Minimal git integration — status, diff, commit, log.
*   Shells out to git.exe for all operations.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Lifecycle
//=============================================================================

// Initialize git integration. Finds git.exe, detects repo root.
BOOL    GitUI_Init(HWND hwndMain);

// Cleanup.
void    GitUI_Shutdown(void);

//=============================================================================
// Repo state
//=============================================================================

// Returns TRUE if the current file is inside a git repository.
BOOL    GitUI_IsInRepo(void);

// Returns the current branch name (static buffer, do not free).
const WCHAR* GitUI_GetBranch(void);

// Returns a short status string (e.g. "+2 ~1 -0") for status bar.
const WCHAR* GitUI_GetStatusSummary(void);

// Refresh git status for the current file. Call after file save.
void    GitUI_Refresh(void);

//=============================================================================
// UI dialogs
//=============================================================================

// Show git status in a dialog.
void    GitUI_ShowStatus(HWND hwndParent);

// Show diff for current file.
void    GitUI_ShowDiff(HWND hwndParent);

// Show commit dialog with staged changes.
void    GitUI_ShowCommitDialog(HWND hwndParent);

// Show git log in a dialog.
void    GitUI_ShowLog(HWND hwndParent);

//=============================================================================
// Git panel
//=============================================================================

// Toggle the git status panel (side panel or bottom panel).
void    GitUI_TogglePanel(HWND hwndParent);

// Returns TRUE if the git panel is visible.
BOOL    GitUI_IsPanelVisible(void);

//=============================================================================
// Status bar integration
//=============================================================================

// Update the status bar with git branch and status info.
void    GitUI_UpdateStatusBar(HWND hwndStatus, int iPart);

//=============================================================================
// Extra UI dialogs
//=============================================================================

// Show git blame for the current file.
void    GitUI_ShowBlame(HWND hwndParent);

// Show all branches.
void    GitUI_ShowBranches(HWND hwndParent);

// Show stash list.
void    GitUI_ShowStash(HWND hwndParent);

// Pull with output dialog.
void    GitUI_PullWithUI(HWND hwndParent);

// Push with output dialog.
void    GitUI_PushWithUI(HWND hwndParent);

//=============================================================================
// Git operations (lower-level)
//=============================================================================

// Stage the current file.
BOOL    GitUI_StageFile(const WCHAR* pszFile);

// Unstage the current file.
BOOL    GitUI_UnstageFile(const WCHAR* pszFile);

// Commit with a message.
BOOL    GitUI_Commit(const char* pszMessage);

// Pull from remote.
BOOL    GitUI_Pull(void);

// Push to remote.
BOOL    GitUI_Push(void);

// Run arbitrary git command and capture output.
// Caller must free *ppszOutput via n2e_Free.
BOOL    GitUI_RunCommand(const WCHAR* pszArgs, char** ppszOutput, int* piExitCode);

#ifdef __cplusplus
}
#endif
