#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Comic Theme Color Palette
// Classic 4-color comic pop art: yellow, red, blue, black+white
// =============================================================================

// Panel / chrome colors
#define COMIC_BLACK        RGB(10, 10, 10)
#define COMIC_WHITE        RGB(255, 255, 250)
#define COMIC_PAPER        RGB(255, 252, 230)       // warm paper yellow
#define COMIC_PANEL_BG     RGB(20, 20, 22)          // night panel dark
#define COMIC_BORDER       RGB(10, 10, 10)          // thick ink outline
#define COMIC_SIDEBAR_BG   RGB(28, 28, 32)

// Accent pops
#define COMIC_YELLOW       RGB(255, 220, 0)         // POW! yellow
#define COMIC_RED          RGB(220, 30, 30)         // BOOM! red
#define COMIC_BLUE         RGB(30, 100, 220)        // classic blue
#define COMIC_CYAN         RGB(0, 210, 230)
#define COMIC_GREEN        RGB(0, 200, 100)
#define COMIC_ORANGE       RGB(255, 120, 20)
#define COMIC_PURPLE       RGB(160, 40, 220)

// Editor / code colors - on cream background
#define COMIC_CODE_BG       RGB(252, 248, 220)      // cream paper
#define COMIC_CODE_TEXT     RGB(10, 10, 10)         // ink black
#define COMIC_CODE_COMMENT  RGB(100, 140, 70)       // green felt pen
#define COMIC_CODE_KEYWORD  RGB(30, 80, 200)        // stark blue
#define COMIC_CODE_STRING   RGB(180, 30, 30)        // red ink
#define COMIC_CODE_NUMBER   RGB(0, 140, 60)
#define COMIC_CODE_TYPE     RGB(150, 0, 180)        // purple
#define COMIC_CODE_FUNC     RGB(200, 100, 0)        // orange
#define COMIC_CODE_PREPROC  RGB(80, 40, 200)
#define COMIC_CODE_SEL      RGB(255, 220, 0)        // yellow selection
#define COMIC_CODE_CARET    RGB(230, 245, 255)      // very light blue caret line
#define COMIC_CODE_LINENUM  RGB(140, 120, 60)       // muted tan
#define COMIC_CODE_GUTTER   RGB(240, 235, 200)      // slightly darker paper

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
