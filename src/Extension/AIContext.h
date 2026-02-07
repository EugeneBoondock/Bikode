#pragma once
/******************************************************************************
*
* Biko
*
* AIContext.h
*   Extract editor context (file, cursor, selection, viewport) for AI requests.
*
******************************************************************************/

#include <wtypes.h>
#include "AIBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Context extraction
//=============================================================================

// Fill an AIRequest with current editor context.
// Allocates memory for file content, selection, etc.
// The request must be freed with AIBridge_FreeRequest().
BOOL    AIContext_FillRequest(AIRequest* pReq, HWND hwndScintilla);

// Get the current file's language identifier string (e.g., "c", "cpp", "python")
// based on the active Scintilla lexer.
const char* AIContext_GetLanguageId(void);

// Get the project root directory (walks up from current file looking for
// .git, .hg, .svn, or a root marker file).
BOOL    AIContext_GetProjectRoot(LPCWSTR wszFilePath, WCHAR* wszRoot, int cchRoot);

// Get a UTF-8 copy of the current file content from Scintilla.
// Caller must free the returned pointer with n2e_Free().
char*   AIContext_GetFileContent(HWND hwndScintilla, int* pLength);

// Get a UTF-8 copy of the current selection from Scintilla.
// Returns NULL if no selection. Caller must free with n2e_Free().
char*   AIContext_GetSelection(HWND hwndScintilla, int* pLength);

// Get cursor position (0-based line and column).
void    AIContext_GetCursorPos(HWND hwndScintilla, int* pLine, int* pCol);

// Get selection range (0-based lines and columns).
// Returns FALSE if no selection.
BOOL    AIContext_GetSelectionRange(HWND hwndScintilla,
            int* pStartLine, int* pStartCol,
            int* pEndLine, int* pEndCol);

// Get visible line range.
void    AIContext_GetViewport(HWND hwndScintilla,
            int* pFirstLine, int* pLastLine);

#ifdef __cplusplus
}
#endif
