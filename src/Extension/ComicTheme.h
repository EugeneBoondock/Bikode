#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Comic Theme Color Palette
// Comic-book noir meets futuristic dev tool.
// =============================================================================

// Panel / chrome colors — matched to BikodeWebsite palette
#define COMIC_BLACK        RGB(17, 17, 17)       // #111111
#define COMIC_WHITE        RGB(230, 230, 230)    // biko-text1 #E6E6E6
#define COMIC_PAPER        RGB(36, 36, 36)       // biko-surface #242424
#define COMIC_PANEL_BG     RGB(36, 36, 36)       // biko-surface #242424
#define COMIC_BORDER       RGB(55, 55, 55)       // biko-border #373737
#define COMIC_SIDEBAR_BG   RGB(36, 36, 36)       // biko-surface #242424

// Accent pops
#define COMIC_YELLOW       RGB(255, 212, 0)
#define COMIC_RED          RGB(255, 91, 91)
#define COMIC_BLUE         RGB(75, 139, 245)     // biko-accent #4B8BF5
#define COMIC_CYAN         RGB(100, 160, 255)    // biko-accent2 #64A0FF
#define COMIC_GREEN        RGB(31, 227, 138)
#define COMIC_ORANGE       RGB(255, 155, 48)
#define COMIC_PURPLE       RGB(255, 77, 166)

// Editor / code colors — neutral gray base from website
#define COMIC_CODE_BG       RGB(24, 24, 24)      // biko-bg #181818
#define COMIC_CODE_TEXT     RGB(230, 230, 230)    // biko-text1 #E6E6E6
#define COMIC_CODE_COMMENT  RGB(150, 150, 150)   // biko-text2 #969696
#define COMIC_CODE_KEYWORD  RGB(75, 139, 245)    // biko-accent #4B8BF5
#define COMIC_CODE_STRING   RGB(255, 155, 48)
#define COMIC_CODE_NUMBER   RGB(31, 227, 138)
#define COMIC_CODE_TYPE     RGB(100, 160, 255)   // biko-accent2 #64A0FF
#define COMIC_CODE_FUNC     RGB(255, 212, 0)
#define COMIC_CODE_PREPROC  RGB(255, 91, 91)
#define COMIC_CODE_SEL      RGB(30, 55, 97)      // accent blue at ~20%
#define COMIC_CODE_CARET    RGB(30, 30, 30)      // subtle lift from bg
#define COMIC_CODE_LINENUM  RGB(80, 80, 80)      // biko-muted #505050
#define COMIC_CODE_GUTTER   RGB(36, 36, 36)      // biko-surface #242424

// Status bar
#define COMIC_STATUS_BG    COMIC_BLACK
#define COMIC_STATUS_TEXT  COMIC_BLUE

// Toolbar
#define COMIC_TB_BG        RGB(24, 24, 24)       // biko-bg #181818
#define COMIC_TB_HOT       COMIC_BLUE
#define COMIC_TB_PRESSED   COMIC_CYAN

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
