#pragma once
/******************************************************************************
*
* Biko
*
* DiffPreview.h
*   Inline diff preview using Scintilla indicators and annotations.
*   Manages preview mode: enter/exit, hunk navigation, toggle, apply.
*
******************************************************************************/

#include <wtypes.h>
#include "AIBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Indicator IDs (Scintilla supports 0-31; Notepad2e uses some)
// Reserve 24-27 for diff preview
//=============================================================================

#define INDIC_DIFF_DELETED    24
#define INDIC_DIFF_ADDED      25
#define INDIC_DIFF_MODIFIED   26
#define INDIC_DIFF_HUNK_SEL   27

//=============================================================================
// Margin for hunk markers
//=============================================================================

#define MARGIN_DIFF_MARKERS   4   // margin index

//=============================================================================
// Preview state
//=============================================================================

BOOL    DiffPreview_IsActive(void);
int     DiffPreview_GetCurrentHunk(void);
int     DiffPreview_GetHunkCount(void);

//=============================================================================
// Preview lifecycle
//=============================================================================

// Enter preview mode: show inline diff decorations for the given patches.
// The editor should be set to read-only during preview.
BOOL    DiffPreview_Enter(HWND hwndScintilla, AIPatch* pPatches, int iPatchCount);

// Exit preview mode: clear all decorations, restore editability.
void    DiffPreview_Exit(HWND hwndScintilla);

//=============================================================================
// Hunk navigation
//=============================================================================

// Move to the next/previous hunk and highlight it.
void    DiffPreview_NextHunk(HWND hwndScintilla);
void    DiffPreview_PrevHunk(HWND hwndScintilla);

// Toggle the current hunk's selection state (for partial apply).
void    DiffPreview_ToggleHunk(HWND hwndScintilla);

//=============================================================================
// Apply / Reject
//=============================================================================

// Apply all selected hunks to the buffer inside an undo group.
// Returns TRUE if any changes were made.
BOOL    DiffPreview_ApplySelected(HWND hwndScintilla);

// Apply all hunks.
BOOL    DiffPreview_ApplyAll(HWND hwndScintilla);

// Reject all â€” equivalent to DiffPreview_Exit().
void    DiffPreview_RejectAll(HWND hwndScintilla);

//=============================================================================
// Query
//=============================================================================

// Get a summary string like "+12 -5"
void    DiffPreview_GetSummary(WCHAR* wszBuf, int cchBuf);

#ifdef __cplusplus
}
#endif
