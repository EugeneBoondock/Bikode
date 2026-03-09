#pragma once
/******************************************************************************
*
* Biko
*
* BikodeTheme.h
*   Central design tokens and shared drawing helpers for the Bikode shell.
*
******************************************************************************/

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BikodeColorToken {
    BKCLR_APP_BG = 0,
    BKCLR_SURFACE_MAIN,
    BKCLR_SURFACE_RAISED,
    BKCLR_SURFACE_ELEVATED,
    BKCLR_EDITOR_BG,
    BKCLR_EDITOR_GUTTER,
    BKCLR_EDITOR_ACTIVE_LINE,
    BKCLR_EDITOR_SELECTION,
    BKCLR_EDITOR_FIND,
    BKCLR_EDITOR_BRACE,
    BKCLR_STROKE_DARK,
    BKCLR_STROKE_SOFT,
    BKCLR_TEXT_PRIMARY,
    BKCLR_TEXT_SECONDARY,
    BKCLR_TEXT_MUTED,
    BKCLR_SIGNAL_YELLOW,
    BKCLR_ELECTRIC_CYAN,
    BKCLR_HOT_MAGENTA,
    BKCLR_SUCCESS_GREEN,
    BKCLR_WARNING_ORANGE,
    BKCLR_DANGER_RED,
    BKCLR_TEXTURE_DOT
} BikodeColorToken;

typedef enum BikodeMetricToken {
    BKMETRIC_TITLE_HEIGHT = 0,
    BKMETRIC_TOOLBAR_HEIGHT,
    BKMETRIC_STATUS_HEIGHT,
    BKMETRIC_RAIL_WIDTH,
    BKMETRIC_RADIUS_PANEL,
    BKMETRIC_RADIUS_BUTTON,
    BKMETRIC_RADIUS_DIALOG,
    BKMETRIC_RADIUS_CHIP,
    BKMETRIC_GAP_XS,
    BKMETRIC_GAP_SM,
    BKMETRIC_GAP_MD,
    BKMETRIC_GAP_LG,
    BKMETRIC_ICON_SM,
    BKMETRIC_ICON_MD,
    BKMETRIC_ICON_LG
} BikodeMetricToken;

typedef enum BikodeFontRole {
    BKFONT_DISPLAY = 0,
    BKFONT_TITLE,
    BKFONT_UI,
    BKFONT_UI_BOLD,
    BKFONT_UI_SMALL,
    BKFONT_MONO,
    BKFONT_MONO_SMALL
} BikodeFontRole;

typedef enum BikodeGlyph {
    BKGLYPH_NONE = 0,
    BKGLYPH_NEW,
    BKGLYPH_OPEN,
    BKGLYPH_SAVE,
    BKGLYPH_UNDO,
    BKGLYPH_REDO,
    BKGLYPH_CUT,
    BKGLYPH_COPY,
    BKGLYPH_PASTE,
    BKGLYPH_FIND,
    BKGLYPH_REPLACE,
    BKGLYPH_WRAP,
    BKGLYPH_ZOOM_IN,
    BKGLYPH_ZOOM_OUT,
    BKGLYPH_AGENT,
    BKGLYPH_EXPLORER,
    BKGLYPH_SEARCH,
    BKGLYPH_SYMBOLS,
    BKGLYPH_GIT,
    BKGLYPH_PLUGIN,
    BKGLYPH_SETTINGS,
    BKGLYPH_TERMINAL,
    BKGLYPH_COMMAND
} BikodeGlyph;

void     BikodeTheme_Init(void);
void     BikodeTheme_Shutdown(void);
COLORREF BikodeTheme_GetColor(BikodeColorToken token);
int      BikodeTheme_GetMetric(BikodeMetricToken token);
HFONT    BikodeTheme_GetFont(BikodeFontRole role);

void BikodeTheme_DrawRoundedPanel(HDC hdc, const RECT* rc, COLORREF fill,
                                  COLORREF outerStroke, COLORREF innerStroke,
                                  int radius, BOOL drawTexture);
void BikodeTheme_DrawCutCornerPanel(HDC hdc, const RECT* rc, COLORREF fill,
                                    COLORREF outerStroke, COLORREF innerStroke,
                                    int cutSize, BOOL drawTexture);
void BikodeTheme_DrawChip(HDC hdc, const RECT* rc, LPCWSTR text,
                          COLORREF fill, COLORREF border, COLORREF textColor,
                          HFONT hFont, BOOL addAccentTab, COLORREF accent);
void BikodeTheme_DrawButton(HDC hdc, const RECT* rc, LPCWSTR text,
                            BikodeGlyph glyph, BOOL hot, BOOL pressed,
                            BOOL primary, BOOL active);
void BikodeTheme_DrawGlyph(HDC hdc, BikodeGlyph glyph, const RECT* rc,
                           COLORREF color, int strokeWidth);
void BikodeTheme_FillHalftone(HDC hdc, const RECT* rc, COLORREF bgColor);
COLORREF BikodeTheme_Mix(COLORREF a, COLORREF b, BYTE mixA);

#ifdef __cplusplus
}
#endif
