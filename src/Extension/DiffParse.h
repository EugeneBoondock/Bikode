#pragma once
/******************************************************************************
*
* Biko
*
* DiffParse.h
*   Unified diff parser. Parses standard unified diff format into
*   structured hunk lists for preview and application.
*
******************************************************************************/

#include <wtypes.h>
#include "AIBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Diff parsing
//=============================================================================

// Parse a unified diff string into an AIPatch structure.
// Allocates hunks array inside the patch. Caller must free with DiffParse_FreePatch().
BOOL    DiffParse_Parse(const char* pszDiff, int iDiffLen, AIPatch* pPatch);

// Parse multiple diffs (multi-file patch).
// Returns array of AIPatch, count in *piPatchCount.
// Caller must free with DiffParse_FreePatches().
AIPatch* DiffParse_ParseMulti(const char* pszDiff, int iDiffLen, int* piPatchCount);

// Free a single patch's internal allocations.
void    DiffParse_FreePatch(AIPatch* pPatch);

// Free an array of patches.
void    DiffParse_FreePatches(AIPatch* pPatches, int iPatchCount);

//=============================================================================
// Diff validation
//=============================================================================

// Validate that a diff string is well-formed unified diff.
// Returns TRUE if parseable, FALSE with error message.
BOOL    DiffParse_Validate(const char* pszDiff, int iDiffLen,
                           char* pszError, int iErrorLen);

// Validate hunks against actual buffer content.
// Sets bFailed on hunks where context doesn't match.
// Returns number of valid hunks.
int     DiffParse_ValidateHunks(const AIPatch* pPatch,
                                const char* pszBuffer, int iBufLen);

//=============================================================================
// Diff generation (for undo/debug)
//=============================================================================

// Generate a unified diff between two buffers.
// Caller must free returned string with n2e_Free().
char*   DiffParse_Generate(const char* pszOld, int iOldLen,
                           const char* pszNew, int iNewLen,
                           const char* pszFileName);

//=============================================================================
// Hunk utilities
//=============================================================================

// Get the line range in the original file that a hunk affects.
void    DiffParse_GetHunkRange(const AIPatchHunk* pHunk,
                               int* pStartLine, int* pEndLine);

// Get the text that would replace the affected range.
// Caller must free with n2e_Free().
char*   DiffParse_GetHunkNewText(const AIPatchHunk* pHunk);

// Count additions and deletions across all hunks.
void    DiffParse_CountChanges(const AIPatch* pPatch,
                               int* pAdditions, int* pDeletions);

#ifdef __cplusplus
}
#endif
