#pragma once
/******************************************************************************
*
* Biko
*
* PatchUndo.h
*   Multi-file undo journal for AI patch operations.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PATCH_FILES     32
#define MAX_UNDO_GROUPS     64

typedef struct TPatchUndoGroup
{
    UINT    uGroupId;
    UINT    uRequestId;     // AI request that generated this patch
    WCHAR   wszFiles[MAX_PATCH_FILES][MAX_PATH];
    int     iFileCount;
    int     iUndoSavePoints[MAX_PATCH_FILES];  // Scintilla save points
    WCHAR   wszDescription[256];
    FILETIME ftTimestamp;
} PatchUndoGroup;

// Initialize the undo journal.
void    PatchUndo_Init(void);

// Begin a new undo group. Returns group ID.
UINT    PatchUndo_Begin(UINT uRequestId, LPCWSTR wszDescription);

// Add a file to the current undo group with its save point.
BOOL    PatchUndo_AddFile(UINT uGroupId, LPCWSTR wszFilePath, int iSavePoint);

// Commit the undo group (marks it as complete).
void    PatchUndo_Commit(UINT uGroupId);

// Undo a specific group by ID. Returns TRUE if successful.
// Caller must handle re-loading files and calling SCI_UNDO.
BOOL    PatchUndo_Undo(UINT uGroupId);

// Get the most recent undo group.
const PatchUndoGroup* PatchUndo_GetLatest(void);

// Get an undo group by ID.
const PatchUndoGroup* PatchUndo_GetById(UINT uGroupId);

// Clear all undo groups.
void    PatchUndo_Clear(void);

// Get the count of active undo groups.
int     PatchUndo_GetCount(void);

#ifdef __cplusplus
}
#endif
