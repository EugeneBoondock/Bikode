/******************************************************************************
*
* Biko
*
* DiffPreview.c
*   Inline diff preview using Scintilla indicators and annotations.
*
******************************************************************************/

#include "DiffPreview.h"
#include "DiffParse.h"
#include "CommonUtils.h"
#include "SciCall.h"
#include "Scintilla.h"
#include <string.h>
#include <stdio.h>

//=============================================================================
// Internal state
//=============================================================================

static BOOL         s_bActive = FALSE;
static AIPatch*     s_pPatches = NULL;
static int          s_iPatchCount = 0;
static int          s_iCurrentHunk = 0;
static int          s_iTotalHunks = 0;
static HWND         s_hwndSci = NULL;
static BOOL         s_bWasReadOnly = FALSE;

//=============================================================================
// Internal: Scintilla indicator/annotation setup
//=============================================================================

static void SetupIndicators(HWND hwnd)
{
    // Deleted lines: red-tinted box
    SendMessage(hwnd, SCI_INDICSETSTYLE, INDIC_DIFF_DELETED, INDIC_STRAIGHTBOX);
    SendMessage(hwnd, SCI_INDICSETFORE, INDIC_DIFF_DELETED, RGB(255, 100, 100));
    SendMessage(hwnd, SCI_INDICSETALPHA, INDIC_DIFF_DELETED, 60);
    SendMessage(hwnd, SCI_INDICSETOUTLINEALPHA, INDIC_DIFF_DELETED, 120);
    SendMessage(hwnd, SCI_INDICSETUNDER, INDIC_DIFF_DELETED, TRUE);

    // Added lines highlight (used for modified-in-place)
    SendMessage(hwnd, SCI_INDICSETSTYLE, INDIC_DIFF_ADDED, INDIC_STRAIGHTBOX);
    SendMessage(hwnd, SCI_INDICSETFORE, INDIC_DIFF_ADDED, RGB(100, 200, 100));
    SendMessage(hwnd, SCI_INDICSETALPHA, INDIC_DIFF_ADDED, 60);
    SendMessage(hwnd, SCI_INDICSETOUTLINEALPHA, INDIC_DIFF_ADDED, 120);
    SendMessage(hwnd, SCI_INDICSETUNDER, INDIC_DIFF_ADDED, TRUE);

    // Hunk selection highlight
    SendMessage(hwnd, SCI_INDICSETSTYLE, INDIC_DIFF_HUNK_SEL, INDIC_ROUNDBOX);
    SendMessage(hwnd, SCI_INDICSETFORE, INDIC_DIFF_HUNK_SEL, RGB(100, 150, 255));
    SendMessage(hwnd, SCI_INDICSETALPHA, INDIC_DIFF_HUNK_SEL, 40);
    SendMessage(hwnd, SCI_INDICSETUNDER, INDIC_DIFF_HUNK_SEL, TRUE);

    // Enable annotations
    SendMessage(hwnd, SCI_ANNOTATIONSETVISIBLE, 2 /* ANNOTATION_BOXED */, 0);
}

static void ClearIndicators(HWND hwnd)
{
    int docLen = (int)SendMessage(hwnd, SCI_GETLENGTH, 0, 0);

    SendMessage(hwnd, SCI_SETINDICATORCURRENT, INDIC_DIFF_DELETED, 0);
    SendMessage(hwnd, SCI_INDICATORCLEARRANGE, 0, docLen);

    SendMessage(hwnd, SCI_SETINDICATORCURRENT, INDIC_DIFF_ADDED, 0);
    SendMessage(hwnd, SCI_INDICATORCLEARRANGE, 0, docLen);

    SendMessage(hwnd, SCI_SETINDICATORCURRENT, INDIC_DIFF_HUNK_SEL, 0);
    SendMessage(hwnd, SCI_INDICATORCLEARRANGE, 0, docLen);

    // Clear all annotations
    SendMessage(hwnd, SCI_ANNOTATIONCLEARALL, 0, 0);
}

// Get byte position of the start of a line (1-based line number)
static int GetLineStartPos(HWND hwnd, int line1Based)
{
    int line0 = line1Based - 1;
    if (line0 < 0) line0 = 0;
    return (int)SendMessage(hwnd, SCI_POSITIONFROMLINE, (WPARAM)line0, 0);
}

// Get byte position of the end of a line (1-based)
static int GetLineEndPos(HWND hwnd, int line1Based)
{
    int line0 = line1Based - 1;
    if (line0 < 0) line0 = 0;
    return (int)SendMessage(hwnd, SCI_GETLINEENDPOSITION, (WPARAM)line0, 0);
}

// Highlight deleted lines with red indicator
static void HighlightDeletedRange(HWND hwnd, int startLine1, int lineCount)
{
    if (lineCount <= 0) return;

    int startPos = GetLineStartPos(hwnd, startLine1);
    int endPos = GetLineEndPos(hwnd, startLine1 + lineCount - 1);

    if (endPos <= startPos) return;

    SendMessage(hwnd, SCI_SETINDICATORCURRENT, INDIC_DIFF_DELETED, 0);
    SendMessage(hwnd, SCI_INDICATORFILLRANGE, (WPARAM)startPos, (LPARAM)(endPos - startPos));
}

// Show added lines as annotation below the preceding line
static void ShowAddedAnnotation(HWND hwnd, int afterLine1, const char* newLines)
{
    if (!newLines || !newLines[0]) return;

    int line0 = afterLine1 - 1;
    if (line0 < 0) line0 = 0;

    // Build annotation text with "+" prefix for each line
    int len = (int)strlen(newLines);
    char* annotText = (char*)n2e_Alloc(len + 256);
    if (!annotText) return;

    int outPos = 0;
    const char* p = newLines;
    while (*p)
    {
        annotText[outPos++] = '+';
        annotText[outPos++] = ' ';
        while (*p && *p != '\n')
        {
            annotText[outPos++] = *p++;
        }
        if (*p == '\n')
        {
            annotText[outPos++] = '\n';
            p++;
        }
    }
    // Remove trailing newline
    if (outPos > 0 && annotText[outPos - 1] == '\n')
        outPos--;
    annotText[outPos] = '\0';

    SendMessage(hwnd, SCI_ANNOTATIONSETTEXT, (WPARAM)line0, (LPARAM)annotText);

    // Use a green-ish style
    // We need to define an annotation style
    SendMessage(hwnd, SCI_ANNOTATIONSETSTYLE, (WPARAM)line0, 0);

    n2e_Free(annotText);
}

// Highlight current hunk with selection indicator
static void HighlightCurrentHunk(HWND hwnd)
{
    // Clear previous hunk highlight
    int docLen = (int)SendMessage(hwnd, SCI_GETLENGTH, 0, 0);
    SendMessage(hwnd, SCI_SETINDICATORCURRENT, INDIC_DIFF_HUNK_SEL, 0);
    SendMessage(hwnd, SCI_INDICATORCLEARRANGE, 0, docLen);

    // Find the current hunk across all patches
    int hunkIdx = 0;
    for (int p = 0; p < s_iPatchCount; p++)
    {
        for (int h = 0; h < s_pPatches[p].iHunkCount; h++)
        {
            if (hunkIdx == s_iCurrentHunk)
            {
                AIPatchHunk* hunk = &s_pPatches[p].pHunks[h];
                int startPos = GetLineStartPos(hwnd, hunk->iOldStart);
                int endPos = GetLineEndPos(hwnd, hunk->iOldStart +
                    (hunk->iOldCount > 0 ? hunk->iOldCount - 1 : 0));

                if (endPos > startPos)
                {
                    SendMessage(hwnd, SCI_SETINDICATORCURRENT, INDIC_DIFF_HUNK_SEL, 0);
                    SendMessage(hwnd, SCI_INDICATORFILLRANGE, (WPARAM)startPos,
                                (LPARAM)(endPos - startPos));
                }

                // Scroll to hunk
                int line0 = hunk->iOldStart - 1;
                if (line0 < 0) line0 = 0;
                SendMessage(hwnd, SCI_ENSUREVISIBLE, (WPARAM)line0, 0);
                SendMessage(hwnd, SCI_GOTOLINE, (WPARAM)line0, 0);
                return;
            }
            hunkIdx++;
        }
    }
}

//=============================================================================
// Public: Preview state
//=============================================================================

BOOL DiffPreview_IsActive(void)
{
    return s_bActive;
}

int DiffPreview_GetCurrentHunk(void)
{
    return s_iCurrentHunk;
}

int DiffPreview_GetHunkCount(void)
{
    return s_iTotalHunks;
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

BOOL DiffPreview_Enter(HWND hwndScintilla, AIPatch* pPatches, int iPatchCount)
{
    if (s_bActive) DiffPreview_Exit(hwndScintilla);
    if (!pPatches || iPatchCount <= 0) return FALSE;

    s_hwndSci = hwndScintilla;
    s_pPatches = pPatches;
    s_iPatchCount = iPatchCount;
    s_iCurrentHunk = 0;

    // Count total hunks
    s_iTotalHunks = 0;
    for (int p = 0; p < iPatchCount; p++)
        s_iTotalHunks += pPatches[p].iHunkCount;

    if (s_iTotalHunks == 0)
    {
        s_pPatches = NULL;
        return FALSE;
    }

    // Save read-only state and set read-only
    s_bWasReadOnly = (BOOL)SendMessage(hwndScintilla, SCI_GETREADONLY, 0, 0);
    SendMessage(hwndScintilla, SCI_SETREADONLY, TRUE, 0);

    // Setup indicators
    SetupIndicators(hwndScintilla);

    // Apply visual decorations for all hunks in the current file
    for (int p = 0; p < iPatchCount; p++)
    {
        for (int h = 0; h < pPatches[p].iHunkCount; h++)
        {
            AIPatchHunk* hunk = &pPatches[p].pHunks[h];
            if (hunk->bFailed) continue;

            // Highlight deleted lines
            if (hunk->iOldCount > 0)
            {
                HighlightDeletedRange(hwndScintilla, hunk->iOldStart, hunk->iOldCount);
            }

            // Show added lines as annotations
            if (hunk->pszNewLines && hunk->pszNewLines[0])
            {
                int annotLine = hunk->iOldStart + hunk->iOldCount;
                if (annotLine < 1) annotLine = 1;
                ShowAddedAnnotation(hwndScintilla, annotLine, hunk->pszNewLines);
            }
        }
    }

    // Highlight first hunk
    HighlightCurrentHunk(hwndScintilla);

    s_bActive = TRUE;
    return TRUE;
}

void DiffPreview_Exit(HWND hwndScintilla)
{
    if (!s_bActive) return;

    ClearIndicators(hwndScintilla);

    // Restore read-only state
    SendMessage(hwndScintilla, SCI_SETREADONLY, s_bWasReadOnly, 0);

    s_bActive = FALSE;
    s_pPatches = NULL;
    s_iPatchCount = 0;
    s_iCurrentHunk = 0;
    s_iTotalHunks = 0;
    s_hwndSci = NULL;
}

//=============================================================================
// Public: Navigation
//=============================================================================

void DiffPreview_NextHunk(HWND hwndScintilla)
{
    if (!s_bActive || s_iTotalHunks <= 0) return;
    s_iCurrentHunk = (s_iCurrentHunk + 1) % s_iTotalHunks;
    HighlightCurrentHunk(hwndScintilla);
}

void DiffPreview_PrevHunk(HWND hwndScintilla)
{
    if (!s_bActive || s_iTotalHunks <= 0) return;
    s_iCurrentHunk = (s_iCurrentHunk - 1 + s_iTotalHunks) % s_iTotalHunks;
    HighlightCurrentHunk(hwndScintilla);
}

void DiffPreview_ToggleHunk(HWND hwndScintilla)
{
    if (!s_bActive) return;

    int hunkIdx = 0;
    for (int p = 0; p < s_iPatchCount; p++)
    {
        for (int h = 0; h < s_pPatches[p].iHunkCount; h++)
        {
            if (hunkIdx == s_iCurrentHunk)
            {
                s_pPatches[p].pHunks[h].bSelected = !s_pPatches[p].pHunks[h].bSelected;
                // Re-render (simple: just update hunk highlight color)
                HighlightCurrentHunk(hwndScintilla);
                return;
            }
            hunkIdx++;
        }
    }
}

//=============================================================================
// Public: Apply
//=============================================================================

BOOL DiffPreview_ApplySelected(HWND hwndScintilla)
{
    if (!s_bActive) return FALSE;

    // Exit preview mode first (clears decorations, restores editability)
    AIPatch* patches = s_pPatches;
    int patchCount = s_iPatchCount;

    ClearIndicators(hwndScintilla);
    SendMessage(hwndScintilla, SCI_SETREADONLY, s_bWasReadOnly, 0);

    // Begin undo group
    SendMessage(hwndScintilla, SCI_BEGINUNDOACTION, 0, 0);

    BOOL bChanged = FALSE;

    // Apply hunks in reverse order (to preserve line numbers)
    for (int p = patchCount - 1; p >= 0; p--)
    {
        for (int h = patches[p].iHunkCount - 1; h >= 0; h--)
        {
            AIPatchHunk* hunk = &patches[p].pHunks[h];
            if (!hunk->bSelected || hunk->bFailed || hunk->bApplied)
                continue;

            // Delete old lines
            if (hunk->iOldCount > 0)
            {
                int startPos = GetLineStartPos(hwndScintilla, hunk->iOldStart);
                int endLine1 = hunk->iOldStart + hunk->iOldCount;
                int endPos;

                // Get position of the line after the deleted range
                int totalLines = (int)SendMessage(hwndScintilla, SCI_GETLINECOUNT, 0, 0);
                if (endLine1 - 1 < totalLines)
                {
                    endPos = GetLineStartPos(hwndScintilla, endLine1);
                }
                else
                {
                    endPos = (int)SendMessage(hwndScintilla, SCI_GETLENGTH, 0, 0);
                }

                if (endPos > startPos)
                {
                    SendMessage(hwndScintilla, SCI_DELETERANGE,
                                (WPARAM)startPos, (LPARAM)(endPos - startPos));
                }
            }

            // Insert new lines
            if (hunk->pszNewLines && hunk->pszNewLines[0])
            {
                int insertPos = GetLineStartPos(hwndScintilla, hunk->iOldStart);
                SendMessage(hwndScintilla, SCI_INSERTTEXT,
                            (WPARAM)insertPos, (LPARAM)hunk->pszNewLines);
            }

            hunk->bApplied = TRUE;
            bChanged = TRUE;
        }
    }

    // End undo group
    SendMessage(hwndScintilla, SCI_ENDUNDOACTION, 0, 0);

    // Clean up preview state
    s_bActive = FALSE;
    s_pPatches = NULL;
    s_iPatchCount = 0;
    s_iCurrentHunk = 0;
    s_iTotalHunks = 0;

    return bChanged;
}

BOOL DiffPreview_ApplyAll(HWND hwndScintilla)
{
    if (!s_bActive) return FALSE;

    // Select all hunks
    for (int p = 0; p < s_iPatchCount; p++)
    {
        for (int h = 0; h < s_pPatches[p].iHunkCount; h++)
        {
            if (!s_pPatches[p].pHunks[h].bFailed)
                s_pPatches[p].pHunks[h].bSelected = TRUE;
        }
    }

    return DiffPreview_ApplySelected(hwndScintilla);
}

void DiffPreview_RejectAll(HWND hwndScintilla)
{
    DiffPreview_Exit(hwndScintilla);
}

//=============================================================================
// Public: Query
//=============================================================================

void DiffPreview_GetSummary(WCHAR* wszBuf, int cchBuf)
{
    if (!s_bActive || !s_pPatches)
    {
        lstrcpynW(wszBuf, L"", cchBuf);
        return;
    }

    int additions = 0, deletions = 0;
    for (int p = 0; p < s_iPatchCount; p++)
    {
        int a, d;
        DiffParse_CountChanges(&s_pPatches[p], &a, &d);
        additions += a;
        deletions += d;
    }

    swprintf(wszBuf, cchBuf, L"+%d -%d", additions, deletions);
}
