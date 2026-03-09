/******************************************************************************
 *
 * Biko - Comic Theme System
 *
 * ComicTheme.c
 *   Implements a bold, fun, halftone-dot comic-book aesthetic across the
 *   entire Biko IDE. Uses pure Win32 GDI (no external dependencies).
 *   
 *   Design principles:
 *   - Paper/ink contrast: cream background, jet-black text
 *   - Bold 3px ink outlines on all panels
 *   - Halftone dot patterns for backgrounds
 *   - Vibrant POW colors: yellow accent, red/blue secondary
 *   - Comic Sans/Impact-style bold labeling (uses system fonts)
 *
 ******************************************************************************/

#include "ComicTheme.h"
#include "Scintilla.h"
#include "SciLexer.h"
#include <windows.h>
#include <windowsx.h>
#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

// =============================================================================
// Internal state
// =============================================================================

static BOOL   s_bInit         = FALSE;
static HBRUSH s_hPanelBrush   = NULL;   // sidebar/panel dark background
static HBRUSH s_hPaperBrush   = NULL;   // editor cream paper background
static HBRUSH s_hStatusBrush  = NULL;   // status bar black background
static HBRUSH s_hSelBrush     = NULL;   // yellow selection brush
static HBITMAP s_hDotPattern  = NULL;   // halftone dot pattern bitmap
static HBRUSH  s_hDotBrush    = NULL;   // halftone pattern brush
static HFONT  s_hBoldFont     = NULL;   // bold UI font
static HFONT  s_hStatusFont   = NULL;   // bold monospace status font

// =============================================================================
// Font creation helpers
// =============================================================================

static HFONT CreateComicFont(int height, BOOL bBold, BOOL bMono)
{
    return CreateFontW(
        height,            // height
        0,                 // width (auto)
        0, 0,              // escapement, orientation
        bBold ? FW_BLACK : FW_BOLD,
        FALSE, FALSE, FALSE,   // italic, underline, strikethrough
        ANSI_CHARSET,
        OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        (bMono ? FIXED_PITCH : VARIABLE_PITCH) | FF_SWISS,
        bMono ? L"Consolas" : L"Impact"
    );
}

// =============================================================================
// Halftone dot pattern
// Create a 6x6 checkerboard-dot bitmap for background texture
// =============================================================================

static void CreateHalftonePattern(void)
{
    WORD bits[6] = {
        0b101010,  // . X . X . X
        0b010101,  // X . X . X .
        0b101010,
        0b010101,
        0b101010,
        0b010101
    };
    s_hDotPattern = CreateBitmap(6, 6, 1, 1, bits);
    if (s_hDotPattern)
        s_hDotBrush = CreatePatternBrush(s_hDotPattern);
}

// =============================================================================
// Public: Init / Shutdown
// =============================================================================

void ComicTheme_Init(void)
{
    if (s_bInit) return;

    s_hPanelBrush  = CreateSolidBrush(COMIC_PANEL_BG);
    s_hPaperBrush  = CreateSolidBrush(COMIC_CODE_BG);
    s_hStatusBrush = CreateSolidBrush(COMIC_STATUS_BG);
    s_hSelBrush    = CreateSolidBrush(COMIC_YELLOW);
    s_hBoldFont    = CreateComicFont(-14, TRUE, FALSE);
    s_hStatusFont  = CreateComicFont(-12, TRUE, TRUE);

    CreateHalftonePattern();

    s_bInit = TRUE;
}

void ComicTheme_Shutdown(void)
{
    if (s_hPanelBrush)  { DeleteObject(s_hPanelBrush);  s_hPanelBrush = NULL; }
    if (s_hPaperBrush)  { DeleteObject(s_hPaperBrush);  s_hPaperBrush = NULL; }
    if (s_hStatusBrush) { DeleteObject(s_hStatusBrush); s_hStatusBrush = NULL; }
    if (s_hSelBrush)    { DeleteObject(s_hSelBrush);    s_hSelBrush = NULL; }
    if (s_hBoldFont)    { DeleteObject(s_hBoldFont);    s_hBoldFont = NULL; }
    if (s_hStatusFont)  { DeleteObject(s_hStatusFont);  s_hStatusFont = NULL; }
    if (s_hDotBrush)    { DeleteObject(s_hDotBrush);    s_hDotBrush = NULL; }
    if (s_hDotPattern)  { DeleteObject(s_hDotPattern);  s_hDotPattern = NULL; }
    s_bInit = FALSE;
}

// =============================================================================
// Getters
// =============================================================================

HBRUSH ComicTheme_GetPanelBrush(void) { return s_hPanelBrush; }
HBRUSH ComicTheme_GetPaperBrush(void) { return s_hPaperBrush; }
HFONT  ComicTheme_GetBoldFont(void)   { return s_hBoldFont; }
HFONT  ComicTheme_GetStatusFont(void) { return s_hStatusFont; }

// =============================================================================
// Drawing primitives
// =============================================================================

void ComicTheme_FillBackground(HDC hdc, RECT* prc, COLORREF clr)
{
    HBRUSH hBr = CreateSolidBrush(clr);
    FillRect(hdc, prc, hBr);
    DeleteObject(hBr);
}

void ComicTheme_DrawBoldBorder(HDC hdc, RECT* prc, int thickness, COLORREF clr)
{
    HPEN hPen = CreatePen(PS_SOLID, thickness, clr);
    HPEN hOld = SelectObject(hdc, hPen);
    HBRUSH hOldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Rectangle(hdc, prc->left, prc->top, prc->right, prc->bottom);

    SelectObject(hdc, hOld);
    SelectObject(hdc, hOldBr);
    DeleteObject(hPen);
}

void ComicTheme_DrawHalftoneDots(HDC hdc, RECT* prc, COLORREF dotColor, COLORREF bgColor)
{
    if (!s_hDotBrush) return;

    // Set brush colors and origin
    SetBrushOrgEx(hdc, prc->left % 6, prc->top % 6, NULL);
    SetTextColor(hdc, dotColor);
    SetBkColor(hdc, bgColor);

    HBRUSH hOld = SelectObject(hdc, s_hDotBrush);
    PatBlt(hdc, prc->left, prc->top,
           prc->right - prc->left, prc->bottom - prc->top,
           PATCOPY);
    SelectObject(hdc, hOld);
}

void ComicTheme_DrawPanel(HDC hdc, RECT* prc, COLORREF bg)
{
    // Fill background
    ComicTheme_FillBackground(hdc, prc, bg);
    // Comic ink border — 3px black
    ComicTheme_DrawBoldBorder(hdc, prc, 3, COMIC_BLACK);
}

void ComicTheme_DrawButton(HDC hdc, RECT* prc, LPCWSTR text, BOOL bHot, BOOL bPressed)
{
    COLORREF bg = bPressed ? COMIC_RED : bHot ? COMIC_YELLOW : COMIC_PANEL_BG;
    COLORREF fg = (bPressed || bHot) ? COMIC_BLACK : COMIC_WHITE;

    ComicTheme_FillBackground(hdc, prc, bg);
    ComicTheme_DrawBoldBorder(hdc, prc, 2, COMIC_BLACK);

    if (text && *text)
    {
        SetTextColor(hdc, fg);
        SetBkMode(hdc, TRANSPARENT);
        HFONT hOldFont = SelectObject(hdc, s_hBoldFont ? s_hBoldFont : GetStockObject(SYSTEM_FONT));
        DrawTextW(hdc, text, -1, prc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, hOldFont);
    }
}

void ComicTheme_DrawStatusSegment(HDC hdc, RECT* prc, LPCWSTR text)
{
    // Status segments: black bg, yellow text, white right-border separator
    ComicTheme_FillBackground(hdc, prc, COMIC_STATUS_BG);

    // Right edge separator line in yellow
    HPEN hPen = CreatePen(PS_SOLID, 1, COMIC_YELLOW);
    HPEN hOld = SelectObject(hdc, hPen);
    MoveToEx(hdc, prc->right - 1, prc->top + 2, NULL);
    LineTo  (hdc, prc->right - 1, prc->bottom - 2);
    SelectObject(hdc, hOld);
    DeleteObject(hPen);

    if (text && *text)
    {
        SetTextColor(hdc, COMIC_YELLOW);
        SetBkMode(hdc, TRANSPARENT);
        HFONT hOldFont = SelectObject(hdc, s_hStatusFont ? s_hStatusFont : GetStockObject(ANSI_FIXED_FONT));
        RECT rText = *prc;
        InflateRect(&rText, -6, 0);
        DrawTextW(hdc, text, -1, &rText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, hOldFont);
    }
}

// =============================================================================
// Apply to Scintilla editor
// =============================================================================

void ComicTheme_ApplyToEditor(HWND hwndEdit)
{
    if (!hwndEdit) return;

    // Base styles — calmer editor plane than the surrounding chrome
    SendMessage(hwndEdit, SCI_STYLESETBACK, STYLE_DEFAULT, COMIC_CODE_BG);
    SendMessage(hwndEdit, SCI_STYLESETFORE, STYLE_DEFAULT, COMIC_CODE_TEXT);
    SendMessage(hwndEdit, SCI_STYLECLEARALL, 0, 0);

    // Syntax highlighting
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_COMMENT,        COMIC_CODE_COMMENT);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_COMMENTLINE,    COMIC_CODE_COMMENT);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_COMMENTDOC,     COMIC_CODE_COMMENT);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_WORD,           COMIC_CODE_KEYWORD);
    SendMessage(hwndEdit, SCI_STYLESETBOLD, SCE_C_WORD,           TRUE);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_WORD2,          COMIC_CODE_TYPE);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_STRING,         COMIC_CODE_STRING);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_NUMBER,         COMIC_CODE_NUMBER);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_PREPROCESSOR,   COMIC_CODE_PREPROC);
    SendMessage(hwndEdit, SCI_STYLESETFORE, SCE_C_IDENTIFIER,     COMIC_CODE_TEXT);

    // Selection: controlled cyan wash
    SendMessage(hwndEdit, SCI_SETSELBACK, TRUE, COMIC_CODE_SEL);
    SendMessage(hwndEdit, SCI_SETSELFORE, TRUE, COMIC_WHITE);
    SendMessage(hwndEdit, SCI_SETSELALPHA, 96, 0);
    SendMessage(hwndEdit, SCI_SETSELEOLFILLED, TRUE, 0);

    // Caret
    SendMessage(hwndEdit, SCI_SETCARETFORE, COMIC_YELLOW, 0);
    SendMessage(hwndEdit, SCI_SETCARETWIDTH, 2, 0);

    // Caret line: subtle chamber glow
    SendMessage(hwndEdit, SCI_SETCARETLINEVISIBLE, TRUE, 0);
    SendMessage(hwndEdit, SCI_SETCARETLINEBACK, COMIC_CODE_CARET, 0);
    SendMessage(hwndEdit, SCI_SETCARETLINEBACKALPHA, 28, 0);

    // Gutter / line numbers
    SendMessage(hwndEdit, SCI_STYLESETBACK, STYLE_LINENUMBER, COMIC_CODE_GUTTER);
    SendMessage(hwndEdit, SCI_STYLESETFORE, STYLE_LINENUMBER, COMIC_CODE_LINENUM);
    SendMessage(hwndEdit, SCI_STYLESETBOLD, STYLE_LINENUMBER, FALSE);
    SendMessage(hwndEdit, SCI_STYLESETBACK, STYLE_INDENTGUIDE, COMIC_CODE_BG);
    SendMessage(hwndEdit, SCI_STYLESETFORE, STYLE_INDENTGUIDE, RGB(34, 40, 50));

    // Fold margin
    SendMessage(hwndEdit, SCI_SETFOLDMARGINCOLOUR, TRUE, COMIC_CODE_GUTTER);
    SendMessage(hwndEdit, SCI_SETFOLDMARGINHICOLOUR, TRUE, COMIC_CODE_BG);

    // Brace matches and find markers
    SendMessage(hwndEdit, SCI_STYLESETFORE, STYLE_BRACELIGHT, COMIC_BLACK);
    SendMessage(hwndEdit, SCI_STYLESETBACK, STYLE_BRACELIGHT, COMIC_YELLOW);
    SendMessage(hwndEdit, SCI_STYLESETFORE, STYLE_BRACEBAD, COMIC_WHITE);
    SendMessage(hwndEdit, SCI_STYLESETBACK, STYLE_BRACEBAD, COMIC_RED);
    SendMessage(hwndEdit, SCI_STYLESETFORE, STYLE_LINEINDICATOR, COMIC_BLACK);
    SendMessage(hwndEdit, SCI_STYLESETBACK, STYLE_LINEINDICATOR, RGB(80, 54, 12));
    SendMessage(hwndEdit, SCI_STYLESETFORE, STYLE_LINEINDICATOR_FIRST_LAST, COMIC_WHITE);
    SendMessage(hwndEdit, SCI_STYLESETBACK, STYLE_LINEINDICATOR_FIRST_LAST, RGB(92, 65, 18));

    // Edge / right column hint
    SendMessage(hwndEdit, SCI_SETEDGECOLOUR, RGB(34, 40, 50), 0);
    SendMessage(hwndEdit, SCI_SETWHITESPACEFORE, TRUE, RGB(28, 34, 42));
    SendMessage(hwndEdit, SCI_SETWHITESPACEBACK, TRUE, COMIC_CODE_BG);
    SendMessage(hwndEdit, SCI_SETHSCROLLBAR, FALSE, 0);
    SendMessage(hwndEdit, SCI_SETVSCROLLBAR, FALSE, 0);
    ShowScrollBar(hwndEdit, SB_BOTH, FALSE);

    // Indicators: diagnostics, diffs, and AI callouts
    SendMessage(hwndEdit, SCI_INDICSETSTYLE, 0, INDIC_SQUIGGLE);
    SendMessage(hwndEdit, SCI_INDICSETFORE, 0, COMIC_RED);
    SendMessage(hwndEdit, SCI_INDICSETALPHA, 0, 150);
    SendMessage(hwndEdit, SCI_INDICSETSTYLE, 28, INDIC_ROUNDBOX);
    SendMessage(hwndEdit, SCI_INDICSETFORE, 28, COMIC_CYAN);
    SendMessage(hwndEdit, SCI_INDICSETALPHA, 28, 55);
    SendMessage(hwndEdit, SCI_INDICSETOUTLINEALPHA, 28, 180);
    SendMessage(hwndEdit, SCI_INDICSETSTYLE, 29, INDIC_STRAIGHTBOX);
    SendMessage(hwndEdit, SCI_INDICSETFORE, 29, COMIC_YELLOW);
    SendMessage(hwndEdit, SCI_INDICSETALPHA, 29, 42);
    SendMessage(hwndEdit, SCI_INDICSETSTYLE, 30, INDIC_STRAIGHTBOX);
    SendMessage(hwndEdit, SCI_INDICSETFORE, 30, COMIC_PURPLE);
    SendMessage(hwndEdit, SCI_INDICSETALPHA, 30, 42);
    SendMessage(hwndEdit, SCI_INDICSETSTYLE, 31, INDIC_SQUIGGLE);
    SendMessage(hwndEdit, SCI_INDICSETFORE, 31, COMIC_RED);

    // Style 240 is reserved for boxed AI annotations.
    SendMessage(hwndEdit, SCI_STYLESETFORE, 240, COMIC_WHITE);
    SendMessage(hwndEdit, SCI_STYLESETBACK, 240, RGB(23, 28, 36));
    SendMessage(hwndEdit, SCI_STYLESETBOLD, 240, TRUE);
}

// =============================================================================
// Apply to Status Bar
// =============================================================================

void ComicTheme_ApplyToStatusBar(HWND hwndStatus)
{
    if (!hwndStatus) return;
    // Remove windows theme so our custom draw kicks in fully
    SetWindowTheme(hwndStatus, L"", L"");
    InvalidateRect(hwndStatus, NULL, TRUE);
}

// =============================================================================  
// Apply to Toolbar
// =============================================================================

void ComicTheme_ApplyToToolbar(HWND hwndToolbar)
{
    if (!hwndToolbar) return;
    SetWindowTheme(hwndToolbar, L"", L"");
    InvalidateRect(hwndToolbar, NULL, TRUE);
}

// =============================================================================
// Apply to a generic sidebar panel window (fill + border)
// =============================================================================

void ComicTheme_ApplyToSidebar(HWND hwnd)
{
    if (!hwnd) return;
    SetWindowTheme(hwnd, L"", L"");
    SetClassLongPtr(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)s_hPanelBrush);
    InvalidateRect(hwnd, NULL, TRUE);
}

// =============================================================================
// Apply all at once
// =============================================================================

void ComicTheme_ApplyAll(HWND hwndMain, HWND hwndEdit, HWND hwndTB, HWND hwndStatus)
{
    if (hwndEdit)   ComicTheme_ApplyToEditor(hwndEdit);
    if (hwndTB)     ComicTheme_ApplyToToolbar(hwndTB);
    if (hwndStatus) ComicTheme_ApplyToStatusBar(hwndStatus);

    UNREFERENCED_PARAMETER(hwndMain);
}
