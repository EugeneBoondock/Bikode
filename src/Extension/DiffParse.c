/******************************************************************************
*
* Biko
*
* DiffParse.c
*   Unified diff parser implementation.
*
******************************************************************************/

#include "DiffParse.h"
#include "CommonUtils.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

//=============================================================================
// Internal helpers
//=============================================================================

static char* dp_strdup(const char* s)
{
    if (!s) return NULL;
    int len = (int)strlen(s);
    char* p = (char*)n2e_Alloc(len + 1);
    if (p) memcpy(p, s, len + 1);
    return p;
}

static char* dp_strndup(const char* s, int len)
{
    if (!s || len <= 0) return NULL;
    char* p = (char*)n2e_Alloc(len + 1);
    if (p)
    {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

// Find next line in buffer, returns pointer to start of next line (or NULL)
// Sets *pLineLen to length of current line (excluding newline)
static const char* dp_nextLine(const char* p, const char* end, int* pLineLen)
{
    const char* start = p;
    while (p < end && *p != '\n' && *p != '\r')
        p++;
    *pLineLen = (int)(p - start);

    if (p < end)
    {
        if (*p == '\r' && p + 1 < end && *(p + 1) == '\n')
            return p + 2;
        return p + 1;
    }
    return NULL;
}

// Parse @@ -L,N +L,N @@ header
static BOOL dp_parseHunkHeader(const char* line, int lineLen,
                               int* pOldStart, int* pOldCount,
                               int* pNewStart, int* pNewCount)
{
    if (lineLen < 4 || line[0] != '@' || line[1] != '@') return FALSE;

    const char* p = line + 2;
    const char* end = line + lineLen;

    // Skip whitespace
    while (p < end && *p == ' ') p++;
    if (p >= end || *p != '-') return FALSE;
    p++;

    // Old start
    *pOldStart = (int)strtol(p, (char**)&p, 10);
    if (p < end && *p == ',')
    {
        p++;
        *pOldCount = (int)strtol(p, (char**)&p, 10);
    }
    else
    {
        *pOldCount = 1;
    }

    // Skip whitespace
    while (p < end && *p == ' ') p++;
    if (p >= end || *p != '+') return FALSE;
    p++;

    // New start
    *pNewStart = (int)strtol(p, (char**)&p, 10);
    if (p < end && *p == ',')
    {
        p++;
        *pNewCount = (int)strtol(p, (char**)&p, 10);
    }
    else
    {
        *pNewCount = 1;
    }

    return TRUE;
}

// Append text to a dynamic string buffer
typedef struct
{
    char* pBuf;
    int   iLen;
    int   iCap;
} DynStr;

static void ds_init(DynStr* ds)
{
    ds->pBuf = NULL;
    ds->iLen = 0;
    ds->iCap = 0;
}

static void ds_append(DynStr* ds, const char* text, int len)
{
    if (len <= 0) return;
    if (ds->iLen + len + 1 > ds->iCap)
    {
        int newCap = (ds->iCap == 0) ? 256 : ds->iCap * 2;
        while (newCap < ds->iLen + len + 1) newCap *= 2;
        char* pNew = (char*)n2e_Realloc(ds->pBuf, newCap);
        if (!pNew) return;
        ds->pBuf = pNew;
        ds->iCap = newCap;
    }
    memcpy(ds->pBuf + ds->iLen, text, len);
    ds->iLen += len;
    ds->pBuf[ds->iLen] = '\0';
}

static void ds_appendLine(DynStr* ds, const char* line, int lineLen)
{
    ds_append(ds, line, lineLen);
    ds_append(ds, "\n", 1);
}

static char* ds_detach(DynStr* ds)
{
    char* p = ds->pBuf;
    ds->pBuf = NULL;
    ds->iLen = 0;
    ds->iCap = 0;
    return p;
}

static void ds_free(DynStr* ds)
{
    if (ds->pBuf) n2e_Free(ds->pBuf);
    ds->pBuf = NULL;
    ds->iLen = 0;
    ds->iCap = 0;
}

//=============================================================================
// Public functions
//=============================================================================

BOOL DiffParse_Parse(const char* pszDiff, int iDiffLen, AIPatch* pPatch)
{
    if (!pszDiff || iDiffLen <= 0 || !pPatch) return FALSE;

    ZeroMemory(pPatch, sizeof(AIPatch));
    pPatch->pszRawDiff = dp_strndup(pszDiff, iDiffLen);
    pPatch->bSelected = TRUE;

    // Temporary hunk storage (max 256 hunks per file)
    #define MAX_HUNKS_PER_FILE 256
    AIPatchHunk tempHunks[MAX_HUNKS_PER_FILE];
    int hunkCount = 0;
    ZeroMemory(tempHunks, sizeof(tempHunks));

    const char* p = pszDiff;
    const char* end = pszDiff + iDiffLen;
    const char* nextLine;
    int lineLen;

    // Parse file headers
    while (p && p < end)
    {
        nextLine = dp_nextLine(p, end, &lineLen);

        if (lineLen >= 4 && strncmp(p, "--- ", 4) == 0)
        {
            // Skip --- line (old file)
        }
        else if (lineLen >= 4 && strncmp(p, "+++ ", 4) == 0)
        {
            // Parse file path from +++ b/path or +++ path
            const char* pathStart = p + 4;
            int pathLen = lineLen - 4;
            if (pathLen >= 2 && pathStart[0] == 'b' && pathStart[1] == '/')
            {
                pathStart += 2;
                pathLen -= 2;
            }
            // Remove leading "a/" too
            if (pathLen >= 2 && pathStart[0] == 'a' && pathStart[1] == '/')
            {
                pathStart += 2;
                pathLen -= 2;
            }
            pPatch->pszFilePath = dp_strndup(pathStart, pathLen);
        }
        else if (lineLen >= 2 && p[0] == '@' && p[1] == '@')
        {
            // Parse hunk header
            if (hunkCount >= MAX_HUNKS_PER_FILE) break;

            AIPatchHunk* hunk = &tempHunks[hunkCount];
            if (!dp_parseHunkHeader(p, lineLen,
                    &hunk->iOldStart, &hunk->iOldCount,
                    &hunk->iNewStart, &hunk->iNewCount))
            {
                p = nextLine;
                continue;
            }
            hunk->bSelected = TRUE;

            DynStr oldLines, newLines;
            ds_init(&oldLines);
            ds_init(&newLines);

            // Parse hunk body
            p = nextLine;
            while (p && p < end)
            {
                nextLine = dp_nextLine(p, end, &lineLen);

                if (lineLen >= 2 && p[0] == '@' && p[1] == '@')
                    break; // next hunk
                if (lineLen >= 4 && strncmp(p, "--- ", 4) == 0)
                    break; // next file
                if (lineLen >= 4 && strncmp(p, "+++ ", 4) == 0)
                    break;
                if (lineLen >= 4 && strncmp(p, "diff ", 4) == 0)
                    break;

                if (lineLen > 0)
                {
                    if (p[0] == '-')
                    {
                        ds_appendLine(&oldLines, p + 1, lineLen - 1);
                    }
                    else if (p[0] == '+')
                    {
                        ds_appendLine(&newLines, p + 1, lineLen - 1);
                    }
                    else if (p[0] == ' ')
                    {
                        // Context line â€” appears in both
                        ds_appendLine(&oldLines, p + 1, lineLen - 1);
                        ds_appendLine(&newLines, p + 1, lineLen - 1);
                    }
                    else if (p[0] == '\\')
                    {
                        // "\ No newline at end of file" â€” ignore
                    }
                }

                p = nextLine;
                continue;
            }

            hunk->pszOldLines = ds_detach(&oldLines);
            hunk->pszNewLines = ds_detach(&newLines);
            hunkCount++;

            continue; // don't advance p again
        }

        p = nextLine;
    }

    // Copy hunks to patch
    if (hunkCount > 0)
    {
        pPatch->iHunkCount = hunkCount;
        pPatch->pHunks = (AIPatchHunk*)n2e_Alloc(sizeof(AIPatchHunk) * hunkCount);
        if (pPatch->pHunks)
        {
            memcpy(pPatch->pHunks, tempHunks, sizeof(AIPatchHunk) * hunkCount);
        }
    }

    return hunkCount > 0;
}

AIPatch* DiffParse_ParseMulti(const char* pszDiff, int iDiffLen, int* piPatchCount)
{
    if (!pszDiff || iDiffLen <= 0 || !piPatchCount) return NULL;

    *piPatchCount = 0;

    // Split on "diff --git" or "--- " at file boundaries
    // Simple approach: find all "diff " or "--- a/" boundaries
    #define MAX_FILES 32
    AIPatch* patches = (AIPatch*)n2e_Alloc(sizeof(AIPatch) * MAX_FILES);
    if (!patches) return NULL;
    ZeroMemory(patches, sizeof(AIPatch) * MAX_FILES);

    const char* p = pszDiff;
    const char* end = pszDiff + iDiffLen;
    const char* fileStart = NULL;
    int fileCount = 0;

    // Find file boundaries
    const char* nextLine;
    int lineLen;

    // If there's only one file (no "diff --git" lines), parse as single
    const char* diffGitSearch = strstr(pszDiff, "diff --git");
    if (!diffGitSearch)
    {
        // Single file diff
        if (DiffParse_Parse(pszDiff, iDiffLen, &patches[0]))
        {
            *piPatchCount = 1;
            return patches;
        }
        n2e_Free(patches);
        return NULL;
    }

    // Multi-file: split on "diff --git" or "diff " lines
    p = pszDiff;
    fileStart = p;

    while (p && p < end)
    {
        nextLine = dp_nextLine(p, end, &lineLen);

        BOOL isNewFile = FALSE;
        if (p != pszDiff) // not the first line
        {
            if (lineLen >= 10 && strncmp(p, "diff --git", 10) == 0)
                isNewFile = TRUE;
            else if (lineLen >= 5 && strncmp(p, "diff ", 5) == 0)
                isNewFile = TRUE;
        }

        if (isNewFile && fileStart && fileCount < MAX_FILES)
        {
            int chunkLen = (int)(p - fileStart);
            if (chunkLen > 0)
            {
                DiffParse_Parse(fileStart, chunkLen, &patches[fileCount]);
                fileCount++;
            }
            fileStart = p;
        }

        p = nextLine;
    }

    // Last file
    if (fileStart && fileCount < MAX_FILES)
    {
        int chunkLen = (int)(end - fileStart);
        if (chunkLen > 0)
        {
            DiffParse_Parse(fileStart, chunkLen, &patches[fileCount]);
            fileCount++;
        }
    }

    *piPatchCount = fileCount;
    return patches;
}

void DiffParse_FreePatch(AIPatch* pPatch)
{
    if (!pPatch) return;
    if (pPatch->pszFilePath) n2e_Free(pPatch->pszFilePath);
    if (pPatch->pszRawDiff) n2e_Free(pPatch->pszRawDiff);
    if (pPatch->pszDescription) n2e_Free(pPatch->pszDescription);
    if (pPatch->pszProofSummary) n2e_Free(pPatch->pszProofSummary);
    if (pPatch->pszTouchedSymbols) n2e_Free(pPatch->pszTouchedSymbols);
    if (pPatch->pszAssumptions) n2e_Free(pPatch->pszAssumptions);
    if (pPatch->pszValidations) n2e_Free(pPatch->pszValidations);
    if (pPatch->pszReviewerVotes) n2e_Free(pPatch->pszReviewerVotes);
    if (pPatch->pszResidualRisk) n2e_Free(pPatch->pszResidualRisk);
    if (pPatch->pszRollbackFingerprint) n2e_Free(pPatch->pszRollbackFingerprint);
    if (pPatch->pszBaseBufferHash) n2e_Free(pPatch->pszBaseBufferHash);
    if (pPatch->pHunks)
    {
        for (int i = 0; i < pPatch->iHunkCount; i++)
        {
            if (pPatch->pHunks[i].pszOldLines) n2e_Free(pPatch->pHunks[i].pszOldLines);
            if (pPatch->pHunks[i].pszNewLines) n2e_Free(pPatch->pHunks[i].pszNewLines);
        }
        n2e_Free(pPatch->pHunks);
    }
    ZeroMemory(pPatch, sizeof(AIPatch));
}

void DiffParse_FreePatches(AIPatch* pPatches, int iPatchCount)
{
    if (!pPatches) return;
    for (int i = 0; i < iPatchCount; i++)
        DiffParse_FreePatch(&pPatches[i]);
    n2e_Free(pPatches);
}

BOOL DiffParse_Validate(const char* pszDiff, int iDiffLen,
                        char* pszError, int iErrorLen)
{
    if (!pszDiff || iDiffLen <= 0)
    {
        if (pszError) strncpy(pszError, "Empty diff", iErrorLen);
        return FALSE;
    }

    // Check for basic structure: must have at least one @@ header
    BOOL hasHunkHeader = FALSE;
    const char* p = pszDiff;
    const char* end = pszDiff + iDiffLen;
    int lineLen;

    while (p && p < end)
    {
        const char* next = dp_nextLine(p, end, &lineLen);
        if (lineLen >= 2 && p[0] == '@' && p[1] == '@')
        {
            int oStart, oCount, nStart, nCount;
            if (dp_parseHunkHeader(p, lineLen, &oStart, &oCount, &nStart, &nCount))
            {
                hasHunkHeader = TRUE;
                break;
            }
        }
        p = next;
    }

    if (!hasHunkHeader)
    {
        if (pszError) strncpy(pszError, "No valid @@ hunk headers found", iErrorLen);
        return FALSE;
    }

    return TRUE;
}

int DiffParse_ValidateHunks(const AIPatch* pPatch,
                            const char* pszBuffer, int iBufLen)
{
    if (!pPatch || !pszBuffer) return 0;

    int validCount = 0;

    // Build a simple line index of the buffer
    // (for quick line lookup)
    const char* p = pszBuffer;
    const char* end = pszBuffer + iBufLen;

    for (int i = 0; i < pPatch->iHunkCount; i++)
    {
        AIPatchHunk* hunk = &((AIPatch*)pPatch)->pHunks[i];

        // Basic validation: check line ranges are within buffer
        int totalLines = 0;
        p = pszBuffer;
        while (p < end)
        {
            if (*p == '\n') totalLines++;
            p++;
        }
        if (pszBuffer[iBufLen - 1] != '\n') totalLines++;

        if (hunk->iOldStart <= 0 || hunk->iOldStart > totalLines + 1)
        {
            hunk->bFailed = TRUE;
        }
        else
        {
            hunk->bFailed = FALSE;
            validCount++;
        }
    }

    return validCount;
}

char* DiffParse_Generate(const char* pszOld, int iOldLen,
                         const char* pszNew, int iNewLen,
                         const char* pszFileName)
{
    // Simple line-by-line diff generation (not a full Myers diff,
    // but sufficient for generating undo diffs)
    // For a production implementation, use a proper diff algorithm.

    DynStr out;
    ds_init(&out);

    char header[512];
    sprintf(header, "--- a/%s\n+++ b/%s\n", pszFileName ? pszFileName : "file",
            pszFileName ? pszFileName : "file");
    ds_append(&out, header, (int)strlen(header));

    // Trivial: show entire old as removed, entire new as added
    sprintf(header, "@@ -1,%d +1,%d @@\n", iOldLen > 0 ? 1 : 0, iNewLen > 0 ? 1 : 0);
    ds_append(&out, header, (int)strlen(header));

    // Old lines with -
    if (pszOld && iOldLen > 0)
    {
        const char* p = pszOld;
        const char* end = pszOld + iOldLen;
        int lineLen;
        while (p && p < end)
        {
            const char* next = dp_nextLine(p, end, &lineLen);
            ds_append(&out, "-", 1);
            ds_append(&out, p, lineLen);
            ds_append(&out, "\n", 1);
            p = next;
        }
    }

    // New lines with +
    if (pszNew && iNewLen > 0)
    {
        const char* p = pszNew;
        const char* end = pszNew + iNewLen;
        int lineLen;
        while (p && p < end)
        {
            const char* next = dp_nextLine(p, end, &lineLen);
            ds_append(&out, "+", 1);
            ds_append(&out, p, lineLen);
            ds_append(&out, "\n", 1);
            p = next;
        }
    }

    return ds_detach(&out);
}

void DiffParse_GetHunkRange(const AIPatchHunk* pHunk, int* pStartLine, int* pEndLine)
{
    if (!pHunk) return;
    *pStartLine = pHunk->iOldStart;
    *pEndLine = pHunk->iOldStart + pHunk->iOldCount - 1;
    if (*pEndLine < *pStartLine) *pEndLine = *pStartLine;
}

char* DiffParse_GetHunkNewText(const AIPatchHunk* pHunk)
{
    if (!pHunk || !pHunk->pszNewLines) return dp_strdup("");
    return dp_strdup(pHunk->pszNewLines);
}

void DiffParse_CountChanges(const AIPatch* pPatch, int* pAdditions, int* pDeletions)
{
    *pAdditions = 0;
    *pDeletions = 0;

    if (!pPatch) return;

    for (int i = 0; i < pPatch->iHunkCount; i++)
    {
        const AIPatchHunk* h = &pPatch->pHunks[i];
        // Count lines in old (deletions) and new (additions)
        if (h->pszOldLines)
        {
            const char* p = h->pszOldLines;
            while (*p)
            {
                if (*p == '\n') (*pDeletions)++;
                p++;
            }
        }
        if (h->pszNewLines)
        {
            const char* p = h->pszNewLines;
            while (*p)
            {
                if (*p == '\n') (*pAdditions)++;
                p++;
            }
        }
    }
}
