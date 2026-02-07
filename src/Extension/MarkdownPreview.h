#pragma once
/******************************************************************************
*
* Biko
*
* MarkdownPreview.h
*   Side-by-side Markdown preview using a secondary Scintilla control.
*   Renders basic Markdown formatting (headings, bold, italic, code, lists)
*   using Scintilla styles and annotations.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IDC_MDPREVIEW_PANEL 0xFB40
#define IDC_MDPREVIEW_VIEW  0xFB41

//=============================================================================
// Lifecycle
//=============================================================================

BOOL    MarkdownPreview_Init(HWND hwndMain);
void    MarkdownPreview_Shutdown(void);

//=============================================================================
// Visibility
//=============================================================================

// Toggle markdown preview panel.
void    MarkdownPreview_Toggle(HWND hwndParent);
void    MarkdownPreview_Show(HWND hwndParent);
void    MarkdownPreview_Hide(void);
BOOL    MarkdownPreview_IsVisible(void);

//=============================================================================
// Layout
//=============================================================================

// Returns the width consumed by the preview panel on the right (0 if hidden).
int     MarkdownPreview_Layout(HWND hwndParent, int parentWidth, int parentHeight);

//=============================================================================
// Content
//=============================================================================

// Refresh the preview from the current editor content.
void    MarkdownPreview_Refresh(HWND hwndSrc);

// Check if the current file is a Markdown file.
BOOL    MarkdownPreview_IsMarkdownFile(const WCHAR* pszFile);

#ifdef __cplusplus
}
#endif
