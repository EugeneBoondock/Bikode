/******************************************************************************
*
* Biko
*
* MarkdownPreview.c
*   Styled Markdown preview using a read-only Scintilla control.
*   Parses basic Markdown and applies rich styles (headings, bold, italic,
*   code blocks, lists, blockquotes, links, horizontal rules).
*
******************************************************************************/

#include "MarkdownPreview.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include "SciCall.h"
#include "Scintilla.h"
#include <string.h>
#include <stdio.h>

//=============================================================================
// Markdown styles
//=============================================================================

#define MD_STYLE_DEFAULT    0
#define MD_STYLE_H1         1
#define MD_STYLE_H2         2
#define MD_STYLE_H3         3
#define MD_STYLE_BOLD       4
#define MD_STYLE_ITALIC     5
#define MD_STYLE_CODE       6
#define MD_STYLE_CODEBLOCK  7
#define MD_STYLE_LINK       8
#define MD_STYLE_LIST       9
#define MD_STYLE_QUOTE      10
#define MD_STYLE_HR         11

//=============================================================================
// Internal state
//=============================================================================

#define MDPREVIEW_WIDTH_RATIO 40   // percentage of parent width

static HWND     s_hwndMain = NULL;
static HWND     s_hwndPanel = NULL;
static HWND     s_hwndView = NULL;
static BOOL     s_bVisible = FALSE;

static const WCHAR* MDPREVIEW_CLASS = L"BikoMarkdownPreview";
static BOOL s_bClassRegistered = FALSE;

//=============================================================================
// Forward declarations
//=============================================================================

static LRESULT CALLBACK MdPanelWndProc(HWND, UINT, WPARAM, LPARAM);
static void SetupPreviewStyles(HWND hwndView);
static void RenderMarkdown(HWND hwndView, const char* pszText, int len);

// Simple markdown span types
typedef struct MdSpan {
    int     start;
    int     length;
    int     style;
} MdSpan;

//=============================================================================
// Panel class
//=============================================================================

static void RegisterMdClass(HINSTANCE hInst)
{
    if (s_bClassRegistered) return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MdPanelWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = MDPREVIEW_CLASS;

    RegisterClassExW(&wc);
    s_bClassRegistered = TRUE;
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

BOOL MarkdownPreview_Init(HWND hwndMain)
{
    s_hwndMain = hwndMain;
    return TRUE;
}

void MarkdownPreview_Shutdown(void)
{
    if (s_hwndPanel)
    {
        DestroyWindow(s_hwndPanel);
        s_hwndPanel = NULL;
        s_hwndView = NULL;
    }
    s_bVisible = FALSE;
}

//=============================================================================
// Public: Visibility
//=============================================================================

void MarkdownPreview_Toggle(HWND hwndParent)
{
    if (s_bVisible)
        MarkdownPreview_Hide();
    else
        MarkdownPreview_Show(hwndParent);
}

void MarkdownPreview_Show(HWND hwndParent)
{
    if (!s_hwndPanel)
    {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);
        RegisterMdClass(hInst);

        s_hwndPanel = CreateWindowExW(
            0, MDPREVIEW_CLASS, L"",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0, hwndParent,
            (HMENU)(UINT_PTR)IDC_MDPREVIEW_PANEL,
            hInst, NULL);

        if (s_hwndPanel)
        {
            s_hwndView = CreateWindowExW(
                0, L"Scintilla", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                0, 0, 0, 0, s_hwndPanel,
                (HMENU)(UINT_PTR)IDC_MDPREVIEW_VIEW,
                hInst, NULL);

            if (s_hwndView)
            {
                SetupPreviewStyles(s_hwndView);
                SendMessage(s_hwndView, SCI_SETREADONLY, TRUE, 0);
                SendMessage(s_hwndView, SCI_SETWRAPMODE, SC_WRAP_WORD, 0);
            }
        }
    }

    if (s_hwndPanel)
    {
        ShowWindow(s_hwndPanel, SW_SHOW);
        s_bVisible = TRUE;

        RECT rc;
        GetClientRect(hwndParent, &rc);
        SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                    MAKELPARAM(rc.right, rc.bottom));

        // Initial render
        HWND hwndCurEdit = n2e_GetActiveEdit();
        if (hwndCurEdit)
            MarkdownPreview_Refresh(hwndCurEdit);
    }
}

void MarkdownPreview_Hide(void)
{
    if (s_hwndPanel)
    {
        ShowWindow(s_hwndPanel, SW_HIDE);
        s_bVisible = FALSE;

        HWND hwndParent = GetParent(s_hwndPanel);
        if (hwndParent)
        {
            RECT rc;
            GetClientRect(hwndParent, &rc);
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rc.right, rc.bottom));
        }
    }
}

BOOL MarkdownPreview_IsVisible(void)
{
    return s_bVisible;
}

//=============================================================================
// Public: Layout
//=============================================================================

int MarkdownPreview_Layout(HWND hwndParent, int parentWidth, int parentHeight)
{
    UNREFERENCED_PARAMETER(hwndParent);

    if (!s_bVisible || !s_hwndPanel) return 0;

    int previewWidth = (parentWidth * MDPREVIEW_WIDTH_RATIO) / 100;
    if (previewWidth < 200) previewWidth = 200;
    if (previewWidth > parentWidth - 200) previewWidth = parentWidth - 200;

    int x = parentWidth - previewWidth;

    MoveWindow(s_hwndPanel, x, 0, previewWidth, parentHeight, TRUE);

    if (s_hwndView)
        MoveWindow(s_hwndView, 0, 0, previewWidth, parentHeight, TRUE);

    return previewWidth;
}

//=============================================================================
// Public: Refresh
//=============================================================================

void MarkdownPreview_Refresh(HWND hwndSrc)
{
    if (!s_hwndView || !s_bVisible || !hwndSrc) return;

    int len = (int)SendMessage(hwndSrc, SCI_GETLENGTH, 0, 0);
    if (len <= 0)
    {
        SendMessage(s_hwndView, SCI_SETREADONLY, FALSE, 0);
        SendMessage(s_hwndView, SCI_CLEARALL, 0, 0);
        SendMessage(s_hwndView, SCI_SETREADONLY, TRUE, 0);
        return;
    }

    char* pszText = (char*)n2e_Alloc(len + 1);
    if (!pszText) return;

    struct Sci_TextRange tr;
    tr.chrg.cpMin = 0;
    tr.chrg.cpMax = len;
    tr.lpstrText = pszText;
    SendMessage(hwndSrc, SCI_GETTEXTRANGE, 0, (LPARAM)&tr);

    RenderMarkdown(s_hwndView, pszText, len);

    n2e_Free(pszText);
}

BOOL MarkdownPreview_IsMarkdownFile(const WCHAR* pszFile)
{
    if (!pszFile) return FALSE;

    const WCHAR* pDot = wcsrchr(pszFile, L'.');
    if (!pDot) return FALSE;

    return (_wcsicmp(pDot, L".md") == 0 ||
            _wcsicmp(pDot, L".markdown") == 0 ||
            _wcsicmp(pDot, L".mdown") == 0 ||
            _wcsicmp(pDot, L".mkd") == 0);
}

//=============================================================================
// Internal: Preview styles
//=============================================================================

static void SetupPreviewStyles(HWND hwndView)
{
    if (!hwndView) return;

    BOOL bDark = DarkMode_IsEnabled();
    COLORREF bgColor = bDark ? RGB(30, 30, 30) : RGB(255, 255, 255);
    COLORREF fgColor = bDark ? RGB(212, 212, 212) : RGB(30, 30, 30);

    // Default
    SendMessage(hwndView, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Segoe UI");
    SendMessage(hwndView, SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    SendMessage(hwndView, SCI_STYLESETBACK, STYLE_DEFAULT, bgColor);
    SendMessage(hwndView, SCI_STYLESETFORE, STYLE_DEFAULT, fgColor);
    SendMessage(hwndView, SCI_STYLECLEARALL, 0, 0);

    // H1
    SendMessage(hwndView, SCI_STYLESETFONT, MD_STYLE_H1, (LPARAM)"Segoe UI");
    SendMessage(hwndView, SCI_STYLESETSIZE, MD_STYLE_H1, 22);
    SendMessage(hwndView, SCI_STYLESETBOLD, MD_STYLE_H1, TRUE);
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_H1, fgColor);

    // H2
    SendMessage(hwndView, SCI_STYLESETFONT, MD_STYLE_H2, (LPARAM)"Segoe UI");
    SendMessage(hwndView, SCI_STYLESETSIZE, MD_STYLE_H2, 18);
    SendMessage(hwndView, SCI_STYLESETBOLD, MD_STYLE_H2, TRUE);
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_H2, fgColor);

    // H3
    SendMessage(hwndView, SCI_STYLESETFONT, MD_STYLE_H3, (LPARAM)"Segoe UI");
    SendMessage(hwndView, SCI_STYLESETSIZE, MD_STYLE_H3, 14);
    SendMessage(hwndView, SCI_STYLESETBOLD, MD_STYLE_H3, TRUE);
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_H3, fgColor);

    // Bold
    SendMessage(hwndView, SCI_STYLESETBOLD, MD_STYLE_BOLD, TRUE);
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_BOLD, fgColor);

    // Italic
    SendMessage(hwndView, SCI_STYLESETITALIC, MD_STYLE_ITALIC, TRUE);
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_ITALIC, fgColor);

    // Inline code
    SendMessage(hwndView, SCI_STYLESETFONT, MD_STYLE_CODE, (LPARAM)"Consolas");
    SendMessage(hwndView, SCI_STYLESETSIZE, MD_STYLE_CODE, 10);
    SendMessage(hwndView, SCI_STYLESETBACK, MD_STYLE_CODE,
        bDark ? RGB(50, 50, 50) : RGB(240, 240, 240));

    // Code block
    SendMessage(hwndView, SCI_STYLESETFONT, MD_STYLE_CODEBLOCK, (LPARAM)"Consolas");
    SendMessage(hwndView, SCI_STYLESETSIZE, MD_STYLE_CODEBLOCK, 10);
    SendMessage(hwndView, SCI_STYLESETBACK, MD_STYLE_CODEBLOCK,
        bDark ? RGB(40, 40, 40) : RGB(245, 245, 245));
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_CODEBLOCK,
        bDark ? RGB(200, 200, 200) : RGB(60, 60, 60));

    // Link
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_LINK,
        bDark ? RGB(80, 160, 255) : RGB(0, 102, 204));
    SendMessage(hwndView, SCI_STYLESETUNDERLINE, MD_STYLE_LINK, TRUE);

    // List bullet
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_LIST,
        bDark ? RGB(180, 180, 180) : RGB(80, 80, 80));

    // Blockquote
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_QUOTE,
        bDark ? RGB(160, 160, 160) : RGB(100, 100, 100));
    SendMessage(hwndView, SCI_STYLESETITALIC, MD_STYLE_QUOTE, TRUE);

    // Horizontal rule
    SendMessage(hwndView, SCI_STYLESETFORE, MD_STYLE_HR,
        bDark ? RGB(80, 80, 80) : RGB(180, 180, 180));

    // No margins
    SendMessage(hwndView, SCI_SETMARGINWIDTHN, 0, 8); // small left margin
    SendMessage(hwndView, SCI_SETMARGINWIDTHN, 1, 0);

    // Caret
    SendMessage(hwndView, SCI_SETCARETFORE, fgColor, 0);
}

//=============================================================================
// Internal: Render Markdown
//=============================================================================

// Growable span array
typedef struct MdSpanList {
    MdSpan*     items;
    int         count;
    int         capacity;
} MdSpanList;

static void SpanList_Init(MdSpanList* pList)
{
    pList->items = NULL;
    pList->count = 0;
    pList->capacity = 0;
}

static void SpanList_Add(MdSpanList* pList, int start, int length, int style)
{
    if (pList->count >= pList->capacity)
    {
        pList->capacity = pList->capacity ? pList->capacity * 2 : 128;
        pList->items = (MdSpan*)n2e_Realloc(pList->items,
            pList->capacity * sizeof(MdSpan));
    }
    pList->items[pList->count].start = start;
    pList->items[pList->count].length = length;
    pList->items[pList->count].style = style;
    pList->count++;
}

static void SpanList_Free(MdSpanList* pList)
{
    if (pList->items) n2e_Free(pList->items);
    pList->items = NULL;
    pList->count = 0;
    pList->capacity = 0;
}

static void RenderMarkdown(HWND hwndView, const char* pszText, int len)
{
    if (!hwndView || !pszText) return;

    SendMessage(hwndView, SCI_SETREADONLY, FALSE, 0);
    SendMessage(hwndView, SCI_CLEARALL, 0, 0);

    // We'll build a "rendered" version: strip markdown syntax, build spans
    // For simplicity, we render line-by-line for block elements,
    // and do inline parsing for spans

    MdSpanList spans;
    SpanList_Init(&spans);

    // Simple approach: copy text verbatim, then apply styles
    // This preserves line structure for a "styled source" preview
    SendMessage(hwndView, SCI_ADDTEXT, len, (LPARAM)pszText);

    // Parse and style line by line
    const char* p = pszText;
    int pos = 0;
    BOOL inCodeBlock = FALSE;

    while (pos < len)
    {
        // Find end of line
        int lineStart = pos;
        while (pos < len && pszText[pos] != '\n')
            pos++;
        int lineEnd = pos;
        int lineLen = lineEnd - lineStart;
        if (pos < len) pos++; // skip \n

        const char* line = pszText + lineStart;

        // Code block fence
        if (lineLen >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`')
        {
            inCodeBlock = !inCodeBlock;
            SpanList_Add(&spans, lineStart, lineLen, MD_STYLE_CODEBLOCK);
            continue;
        }

        if (inCodeBlock)
        {
            SpanList_Add(&spans, lineStart, lineLen, MD_STYLE_CODEBLOCK);
            continue;
        }

        // Headings
        if (lineLen >= 2 && line[0] == '#' && line[1] == ' ')
        {
            SpanList_Add(&spans, lineStart, lineLen, MD_STYLE_H1);
            continue;
        }
        if (lineLen >= 3 && line[0] == '#' && line[1] == '#' && line[2] == ' ')
        {
            SpanList_Add(&spans, lineStart, lineLen, MD_STYLE_H2);
            continue;
        }
        if (lineLen >= 4 && line[0] == '#' && line[1] == '#' && line[2] == '#' && line[3] == ' ')
        {
            SpanList_Add(&spans, lineStart, lineLen, MD_STYLE_H3);
            continue;
        }

        // Horizontal rule
        if (lineLen >= 3 &&
            ((line[0] == '-' && line[1] == '-' && line[2] == '-') ||
             (line[0] == '*' && line[1] == '*' && line[2] == '*') ||
             (line[0] == '_' && line[1] == '_' && line[2] == '_')))
        {
            SpanList_Add(&spans, lineStart, lineLen, MD_STYLE_HR);
            continue;
        }

        // Blockquote
        if (lineLen >= 2 && line[0] == '>' && line[1] == ' ')
        {
            SpanList_Add(&spans, lineStart, lineLen, MD_STYLE_QUOTE);
            continue;
        }

        // List items
        if (lineLen >= 2 &&
            ((line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' '))
        {
            SpanList_Add(&spans, lineStart, 2, MD_STYLE_LIST);
            // Rest is default (with inline parsing below)
        }

        // Inline parsing for bold, italic, code, links
        for (int i = 0; i < lineLen; i++)
        {
            int absPos = lineStart + i;

            // Inline code: `...`
            if (line[i] == '`')
            {
                int j = i + 1;
                while (j < lineLen && line[j] != '`') j++;
                if (j < lineLen)
                {
                    SpanList_Add(&spans, absPos, j - i + 1, MD_STYLE_CODE);
                    i = j;
                    continue;
                }
            }

            // Bold: **...**
            if (i + 1 < lineLen && line[i] == '*' && line[i+1] == '*')
            {
                int j = i + 2;
                while (j + 1 < lineLen && !(line[j] == '*' && line[j+1] == '*')) j++;
                if (j + 1 < lineLen)
                {
                    SpanList_Add(&spans, absPos, j - i + 2, MD_STYLE_BOLD);
                    i = j + 1;
                    continue;
                }
            }

            // Italic: *...*
            if (line[i] == '*' && (i + 1 < lineLen) && line[i+1] != '*')
            {
                int j = i + 1;
                while (j < lineLen && line[j] != '*') j++;
                if (j < lineLen)
                {
                    SpanList_Add(&spans, absPos, j - i + 1, MD_STYLE_ITALIC);
                    i = j;
                    continue;
                }
            }

            // Link: [text](url)
            if (line[i] == '[')
            {
                int j = i + 1;
                while (j < lineLen && line[j] != ']') j++;
                if (j + 1 < lineLen && line[j+1] == '(')
                {
                    int k = j + 2;
                    while (k < lineLen && line[k] != ')') k++;
                    if (k < lineLen)
                    {
                        SpanList_Add(&spans, absPos, k - i + 1, MD_STYLE_LINK);
                        i = k;
                        continue;
                    }
                }
            }
        }
    }

    // Apply all collected style spans
    for (int i = 0; i < spans.count; i++)
    {
        SendMessage(hwndView, SCI_STARTSTYLING, spans.items[i].start, 0);
        SendMessage(hwndView, SCI_SETSTYLING, spans.items[i].length, spans.items[i].style);
    }

    SpanList_Free(&spans);

    SendMessage(hwndView, SCI_SETREADONLY, TRUE, 0);
    SendMessage(hwndView, SCI_GOTOPOS, 0, 0);
}

//=============================================================================
// Internal: Panel window proc
//=============================================================================

static LRESULT CALLBACK MdPanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        if (s_hwndView)
        {
            int cx = LOWORD(lParam);
            int cy = HIWORD(lParam);
            MoveWindow(s_hwndView, 0, 0, cx, cy, TRUE);
        }
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
