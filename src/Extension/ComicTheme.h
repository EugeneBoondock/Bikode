#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Comic Theme Color Palette
// Comic-book noir meets futuristic dev tool.
// =============================================================================

// Panel / chrome colors
#define COMIC_BLACK        RGB(5, 6, 8)
#define COMIC_WHITE        RGB(243, 245, 247)
#define COMIC_PAPER        RGB(17, 21, 28)
#define COMIC_PANEL_BG     RGB(17, 21, 28)
#define COMIC_BORDER       RGB(5, 6, 8)
#define COMIC_SIDEBAR_BG   RGB(23, 28, 36)

// Accent pops
#define COMIC_YELLOW       RGB(255, 212, 0)
#define COMIC_RED          RGB(255, 91, 91)
#define COMIC_BLUE         RGB(53, 224, 255)
#define COMIC_CYAN         RGB(53, 224, 255)
#define COMIC_GREEN        RGB(31, 227, 138)
#define COMIC_ORANGE       RGB(255, 155, 48)
#define COMIC_PURPLE       RGB(255, 77, 166)

// Editor / code colors - calmer than the shell
#define COMIC_CODE_BG       RGB(11, 15, 20)
#define COMIC_CODE_TEXT     RGB(243, 245, 247)
#define COMIC_CODE_COMMENT  RGB(120, 160, 124)
#define COMIC_CODE_KEYWORD  RGB(53, 224, 255)
#define COMIC_CODE_STRING   RGB(255, 155, 48)
#define COMIC_CODE_NUMBER   RGB(31, 227, 138)
#define COMIC_CODE_TYPE     RGB(255, 77, 166)
#define COMIC_CODE_FUNC     RGB(255, 212, 0)
#define COMIC_CODE_PREPROC  RGB(255, 91, 91)
#define COMIC_CODE_SEL      RGB(22, 47, 63)
#define COMIC_CODE_CARET    RGB(16, 21, 28)
#define COMIC_CODE_LINENUM  RGB(110, 119, 133)
#define COMIC_CODE_GUTTER   RGB(17, 22, 29)

// Status bar
#define COMIC_STATUS_BG    COMIC_BLACK
#define COMIC_STATUS_TEXT  COMIC_YELLOW

// Toolbar
#define COMIC_TB_BG        RGB(18, 18, 20)
#define COMIC_TB_HOT       COMIC_YELLOW
#define COMIC_TB_PRESSED   COMIC_RED

// Halftone dot pattern size
#define COMIC_DOT_SIZE     6

// =============================================================================
// Public API
// =============================================================================

// Initialize Comic Theme (call once at startup)
void ComicTheme_Init(void);
void ComicTheme_Shutdown(void);

// Apply comic look to different pieces
void ComicTheme_ApplyToEditor(HWND hwndEdit);
void ComicTheme_ApplyToStatusBar(HWND hwndStatus);
void ComicTheme_ApplyToToolbar(HWND hwndToolbar);
void ComicTheme_ApplyToSidebar(HWND hwnd);
void ComicTheme_ApplyAll(HWND hwndMain, HWND hwndEdit, HWND hwndTB, HWND hwndStatus);

// Drawing primitives - use in WM_PAINT / NM_CUSTOMDRAW handlers
void ComicTheme_FillBackground(HDC hdc, RECT* prc, COLORREF clr);
void ComicTheme_DrawBoldBorder(HDC hdc, RECT* prc, int thickness, COLORREF clr);
void ComicTheme_DrawHalftoneDots(HDC hdc, RECT* prc, COLORREF dotColor, COLORREF bgColor);
void ComicTheme_DrawButton(HDC hdc, RECT* prc, LPCWSTR text, BOOL bHot, BOOL bPressed);
void ComicTheme_DrawPanel(HDC hdc, RECT* prc, COLORREF bg);
void ComicTheme_DrawStatusSegment(HDC hdc, RECT* prc, LPCWSTR text);

// Brush/color getters
HBRUSH ComicTheme_GetPanelBrush(void);
HBRUSH ComicTheme_GetPaperBrush(void);
HFONT  ComicTheme_GetBoldFont(void);
HFONT  ComicTheme_GetStatusFont(void);

#ifdef __cplusplus
}
#endif
