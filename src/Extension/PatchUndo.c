/******************************************************************************
*
* Biko
*
* PatchUndo.c
*   Multi-file undo journal implementation.
*
******************************************************************************/

#include "PatchUndo.h"
#include "CommonUtils.h"
#include <string.h>

//=============================================================================
// Internal state
//=============================================================================

static PatchUndoGroup  s_groups[MAX_UNDO_GROUPS];
static int             s_iGroupCount = 0;
static UINT            s_uNextGroupId = 1;
static CRITICAL_SECTION s_cs;
static BOOL            s_bInitialized = FALSE;

//=============================================================================
// Public functions
//=============================================================================

void PatchUndo_Init(void)
{
    if (s_bInitialized) return;
    InitializeCriticalSection(&s_cs);
    ZeroMemory(s_groups, sizeof(s_groups));
    s_iGroupCount = 0;
    s_uNextGroupId = 1;
    s_bInitialized = TRUE;
}

UINT PatchUndo_Begin(UINT uRequestId, LPCWSTR wszDescription)
{
    if (!s_bInitialized) PatchUndo_Init();

    EnterCriticalSection(&s_cs);

    UINT groupId = s_uNextGroupId++;

    // Find a slot (ring buffer behavior)
    int slot = s_iGroupCount % MAX_UNDO_GROUPS;

    ZeroMemory(&s_groups[slot], sizeof(PatchUndoGroup));
    s_groups[slot].uGroupId = groupId;
    s_groups[slot].uRequestId = uRequestId;
    if (wszDescription)
        lstrcpynW(s_groups[slot].wszDescription, wszDescription, 256);

    SYSTEMTIME st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &s_groups[slot].ftTimestamp);

    if (s_iGroupCount < MAX_UNDO_GROUPS)
        s_iGroupCount++;

    LeaveCriticalSection(&s_cs);
    return groupId;
}

BOOL PatchUndo_AddFile(UINT uGroupId, LPCWSTR wszFilePath, int iSavePoint)
{
    if (!s_bInitialized) return FALSE;

    EnterCriticalSection(&s_cs);

    for (int i = 0; i < s_iGroupCount; i++)
    {
        if (s_groups[i].uGroupId == uGroupId)
        {
            int idx = s_groups[i].iFileCount;
            if (idx >= MAX_PATCH_FILES)
            {
                LeaveCriticalSection(&s_cs);
                return FALSE;
            }
            lstrcpynW(s_groups[i].wszFiles[idx], wszFilePath, MAX_PATH);
            s_groups[i].iUndoSavePoints[idx] = iSavePoint;
            s_groups[i].iFileCount++;

            LeaveCriticalSection(&s_cs);
            return TRUE;
        }
    }

    LeaveCriticalSection(&s_cs);
    return FALSE;
}

void PatchUndo_Commit(UINT uGroupId)
{
    // Currently no-op; group is usable after Begin.
    // Could add a "committed" flag if needed.
    UNREFERENCED_PARAMETER(uGroupId);
}

BOOL PatchUndo_Undo(UINT uGroupId)
{
    if (!s_bInitialized) return FALSE;

    EnterCriticalSection(&s_cs);

    for (int i = 0; i < s_iGroupCount; i++)
    {
        if (s_groups[i].uGroupId == uGroupId)
        {
            // Mark as undone by clearing group ID
            // The actual undo operation (SCI_UNDO calls) is handled by the caller
            s_groups[i].uGroupId = 0;
            LeaveCriticalSection(&s_cs);
            return TRUE;
        }
    }

    LeaveCriticalSection(&s_cs);
    return FALSE;
}

const PatchUndoGroup* PatchUndo_GetLatest(void)
{
    if (!s_bInitialized || s_iGroupCount == 0) return NULL;

    EnterCriticalSection(&s_cs);

    // Find most recent active group
    for (int i = s_iGroupCount - 1; i >= 0; i--)
    {
        int slot = i % MAX_UNDO_GROUPS;
        if (s_groups[slot].uGroupId != 0)
        {
            LeaveCriticalSection(&s_cs);
            return &s_groups[slot];
        }
    }

    LeaveCriticalSection(&s_cs);
    return NULL;
}

const PatchUndoGroup* PatchUndo_GetById(UINT uGroupId)
{
    if (!s_bInitialized) return NULL;

    EnterCriticalSection(&s_cs);

    for (int i = 0; i < s_iGroupCount; i++)
    {
        if (s_groups[i].uGroupId == uGroupId)
        {
            LeaveCriticalSection(&s_cs);
            return &s_groups[i];
        }
    }

    LeaveCriticalSection(&s_cs);
    return NULL;
}

void PatchUndo_Clear(void)
{
    if (!s_bInitialized) return;

    EnterCriticalSection(&s_cs);
    ZeroMemory(s_groups, sizeof(s_groups));
    s_iGroupCount = 0;
    LeaveCriticalSection(&s_cs);
}

int PatchUndo_GetCount(void)
{
    if (!s_bInitialized) return 0;
    return s_iGroupCount;
}
