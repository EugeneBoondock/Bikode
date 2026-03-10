/******************************************************************************
*
* Biko
*
* BikodeTheme.c
*   Central token source, font fallback, and chrome drawing helpers.
*
******************************************************************************/

#include "BikodeTheme.h"
#include <string.h>

typedef struct ThemeFontEntry {
    HFONT hFont;
    int   height;
    int   weight;
    BOOL  italic;
    BOOL  mono;
} ThemeFontEntry;

static ThemeFontEntry s_fonts[7];
static HBITMAP s_hHalftoneBmp = NULL;
static HBRUSH  s_hHalftoneBrush = NULL;
static HFONT   s_hIconFontSm = NULL;
static HFONT   s_hIconFontMd = NULL;
static HFONT   s_hIconFontLg = NULL;
static BOOL    s_bInit = FALSE;

static HFONT TryCreateFontWithFallback(const WCHAR* const* families, int height,
                                       int weight, BOOL italic, BOOL mono)
{
    HDC hdc = GetDC(NULL);
    for (int i = 0; families[i]; i++)
    {
        HFONT hFont = CreateFontW(height, 0, 0, 0, weight, italic, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            (mono ? FIXED_PITCH : DEFAULT_PITCH) | FF_DONTCARE, families[i]);
        if (hFont && hdc)
        {
            HFONT hOld = (HFONT)SelectObject(hdc, hFont);
            WCHAR actual[64] = L"";
            GetTextFaceW(hdc, ARRAYSIZE(actual), actual);
            SelectObject(hdc, hOld);
            if (_wcsicmp(actual, families[i]) == 0)
            {
                ReleaseDC(NULL, hdc);
                return hFont;
            }
        }
        if (hFont)
            DeleteObject(hFont);
    }
    if (hdc)
        ReleaseDC(NULL, hdc);
    return NULL;
}

static HFONT CreateFontWithFallback(const WCHAR* const* families, int height,
                                    int weight, BOOL italic, BOOL mono)
{
    HFONT hFont = TryCreateFontWithFallback(families, height, weight, italic, mono);
    if (hFont)
        return hFont;
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void CreateFonts(void)
{
    static const WCHAR* displayFamilies[] = {
        L"Bangers", L"Impact", L"Arial Black", L"Segoe UI Semibold", NULL
    };
    static const WCHAR* uiFamilies[] = {
        L"Inter", L"Segoe UI Variable Text", L"Segoe UI", L"Arial", NULL
    };
    static const WCHAR* monoFamilies[] = {
        L"JetBrains Mono", L"Cascadia Mono", L"Consolas", NULL
    };
    static const WCHAR* iconFamilies[] = {
        L"Segoe Fluent Icons", L"Segoe MDL2 Assets", NULL
    };

    if (s_fonts[BKFONT_DISPLAY].hFont) return;

    s_fonts[BKFONT_DISPLAY].hFont = CreateFontWithFallback(displayFamilies, -18, FW_HEAVY, FALSE, FALSE);
    s_fonts[BKFONT_TITLE].hFont = CreateFontWithFallback(displayFamilies, -14, FW_HEAVY, FALSE, FALSE);
    s_fonts[BKFONT_UI].hFont = CreateFontWithFallback(uiFamilies, -13, FW_NORMAL, FALSE, FALSE);
    s_fonts[BKFONT_UI_BOLD].hFont = CreateFontWithFallback(uiFamilies, -13, FW_SEMIBOLD, FALSE, FALSE);
    s_fonts[BKFONT_UI_SMALL].hFont = CreateFontWithFallback(uiFamilies, -11, FW_NORMAL, FALSE, FALSE);
    s_fonts[BKFONT_MONO].hFont = CreateFontWithFallback(monoFamilies, -12, FW_NORMAL, FALSE, TRUE);
    s_fonts[BKFONT_MONO_SMALL].hFont = CreateFontWithFallback(monoFamilies, -10, FW_NORMAL, FALSE, TRUE);
    s_hIconFontSm = TryCreateFontWithFallback(iconFamilies, -12, FW_NORMAL, FALSE, FALSE);
    s_hIconFontMd = TryCreateFontWithFallback(iconFamilies, -16, FW_NORMAL, FALSE, FALSE);
    s_hIconFontLg = TryCreateFontWithFallback(iconFamilies, -18, FW_NORMAL, FALSE, FALSE);
}

static void CreateHalftoneBrush(void)
{
    /* 32x32 pseudo-random noise bitmap for film-grain texture.
       Matches the fractalNoise grain overlay from BikodeWebsite. */
    WORD bits[64]; /* 32 rows * 2 WORDs per row */
    unsigned int seed = 0xB1C0DE;
    int i;
    for (i = 0; i < 64; i++) {
        seed = seed * 1103515245 + 12345;
        bits[i] = (WORD)((seed >> 16) & 0xFFFF);
    }
    if (s_hHalftoneBrush) return;
    s_hHalftoneBmp = CreateBitmap(32, 32, 1, 1, bits);
    if (s_hHalftoneBmp)
        s_hHalftoneBrush = CreatePatternBrush(s_hHalftoneBmp);
}

void BikodeTheme_Init(void)
{
    if (s_bInit) return;
    CreateFonts();
    CreateHalftoneBrush();
    s_bInit = TRUE;
}

void BikodeTheme_Shutdown(void)
{
    for (int i = 0; i < ARRAYSIZE(s_fonts); i++)
    {
        if (s_fonts[i].hFont && s_fonts[i].hFont != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(s_fonts[i].hFont);
        s_fonts[i].hFont = NULL;
    }
    if (s_hHalftoneBrush) {
        DeleteObject(s_hHalftoneBrush);
        s_hHalftoneBrush = NULL;
    }
    if (s_hHalftoneBmp) {
        DeleteObject(s_hHalftoneBmp);
        s_hHalftoneBmp = NULL;
    }
    if (s_hIconFontSm) {
        DeleteObject(s_hIconFontSm);
        s_hIconFontSm = NULL;
    }
    if (s_hIconFontMd) {
        DeleteObject(s_hIconFontMd);
        s_hIconFontMd = NULL;
    }
    if (s_hIconFontLg) {
        DeleteObject(s_hIconFontLg);
        s_hIconFontLg = NULL;
    }
    s_bInit = FALSE;
}

COLORREF BikodeTheme_GetColor(BikodeColorToken token)
{
    switch (token)
    {
    case BKCLR_APP_BG:            return RGB(24, 24, 24);      // biko-bg #181818
    case BKCLR_SURFACE_MAIN:      return RGB(36, 36, 36);      // biko-surface #242424
    case BKCLR_SURFACE_RAISED:    return RGB(48, 50, 58);      // biko-hover #30323a
    case BKCLR_SURFACE_ELEVATED:  return RGB(55, 55, 55);      // biko-border #373737
    case BKCLR_EDITOR_BG:         return RGB(24, 24, 24);      // biko-bg
    case BKCLR_EDITOR_GUTTER:     return RGB(36, 36, 36);      // biko-surface
    case BKCLR_EDITOR_ACTIVE_LINE:return RGB(30, 30, 30);      // subtle lift
    case BKCLR_EDITOR_SELECTION:  return RGB(30, 55, 97);      // accent at ~20%
    case BKCLR_EDITOR_FIND:       return RGB(62, 40, 12);
    case BKCLR_EDITOR_BRACE:      return RGB(255, 212, 0);
    case BKCLR_STROKE_DARK:       return RGB(17, 17, 17);      // #111111
    case BKCLR_STROKE_SOFT:       return RGB(50, 50, 50);      // biko-divider #323232
    case BKCLR_TEXT_PRIMARY:      return RGB(230, 230, 230);    // biko-text1 #E6E6E6
    case BKCLR_TEXT_SECONDARY:    return RGB(150, 150, 150);    // biko-text2 #969696
    case BKCLR_TEXT_MUTED:        return RGB(80, 80, 80);       // biko-muted #505050
    case BKCLR_SIGNAL_YELLOW:     return RGB(255, 212, 0);
    case BKCLR_ELECTRIC_CYAN:     return RGB(75, 139, 245);     // biko-accent #4B8BF5
    case BKCLR_HOT_MAGENTA:       return RGB(255, 77, 166);
    case BKCLR_SUCCESS_GREEN:     return RGB(31, 227, 138);
    case BKCLR_WARNING_ORANGE:    return RGB(255, 155, 48);
    case BKCLR_DANGER_RED:        return RGB(255, 91, 91);
    case BKCLR_TEXTURE_DOT:       return RGB(32, 32, 32);       // subtle grain dot
    default:                      return RGB(255, 255, 255);
    }
}

int BikodeTheme_GetMetric(BikodeMetricToken token)
{
    switch (token)
    {
    case BKMETRIC_TITLE_HEIGHT:  return 32;
    case BKMETRIC_TOOLBAR_HEIGHT:return 36;
    case BKMETRIC_STATUS_HEIGHT: return 24;
    case BKMETRIC_RAIL_WIDTH:    return 48;
    case BKMETRIC_RADIUS_PANEL:  return 10;
    case BKMETRIC_RADIUS_BUTTON: return 8;
    case BKMETRIC_RADIUS_DIALOG: return 14;
    case BKMETRIC_RADIUS_CHIP:   return 999;
    case BKMETRIC_GAP_XS:        return 4;
    case BKMETRIC_GAP_SM:        return 8;
    case BKMETRIC_GAP_MD:        return 12;
    case BKMETRIC_GAP_LG:        return 16;
    case BKMETRIC_ICON_SM:       return 12;
    case BKMETRIC_ICON_MD:       return 16;
    case BKMETRIC_ICON_LG:       return 18;
    default:                     return 0;
    }
}

HFONT BikodeTheme_GetFont(BikodeFontRole role)
{
    BikodeTheme_Init();
    return s_fonts[role].hFont ? s_fonts[role].hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

COLORREF BikodeTheme_Mix(COLORREF a, COLORREF b, BYTE mixA)
{
    BYTE mixB = (BYTE)(255 - mixA);
    return RGB(
        (GetRValue(a) * mixA + GetRValue(b) * mixB) / 255,
        (GetGValue(a) * mixA + GetGValue(b) * mixB) / 255,
        (GetBValue(a) * mixA + GetBValue(b) * mixB) / 255);
}

void BikodeTheme_FillHalftone(HDC hdc, const RECT* rc, COLORREF bgColor)
{
    BikodeTheme_Init();
    if (!s_hHalftoneBrush || !rc) return;

    HBRUSH hBg = CreateSolidBrush(bgColor);
    FillRect(hdc, rc, hBg);
    DeleteObject(hBg);

    SetBkColor(hdc, bgColor);
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXTURE_DOT));
    SetBrushOrgEx(hdc, rc->left % 32, rc->top % 32, NULL);
    HBRUSH hOld = (HBRUSH)SelectObject(hdc, s_hHalftoneBrush);
    PatBlt(hdc, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, PATCOPY);
    SelectObject(hdc, hOld);
}

static void DrawRoundedShape(HDC hdc, const RECT* rc, COLORREF fill, COLORREF stroke, int radius)
{
    HBRUSH hFill = CreateSolidBrush(fill);
    HPEN hPen = CreatePen(PS_SOLID, 1, stroke);
    HGDIOBJ oldBrush = SelectObject(hdc, hFill);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hFill);
    DeleteObject(hPen);
}

void BikodeTheme_DrawRoundedPanel(HDC hdc, const RECT* rc, COLORREF fill,
                                  COLORREF outerStroke, COLORREF innerStroke,
                                  int radius, BOOL drawTexture)
{
    RECT inner = *rc;
    if (drawTexture)
        BikodeTheme_FillHalftone(hdc, rc, fill);
    DrawRoundedShape(hdc, rc, fill, outerStroke, radius);
    InflateRect(&inner, -1, -1);
    DrawRoundedShape(hdc, &inner, fill, innerStroke, radius > 2 ? radius - 2 : radius);
}

void BikodeTheme_DrawCutCornerPanel(HDC hdc, const RECT* rc, COLORREF fill,
                                    COLORREF outerStroke, COLORREF innerStroke,
                                    int cutSize, BOOL drawTexture)
{
    POINT pts[6];
    HPEN hPen;
    HBRUSH hBrush;
    HGDIOBJ oldBrush, oldPen;

    if (!rc) return;
    if (drawTexture)
        BikodeTheme_FillHalftone(hdc, rc, fill);

    pts[0].x = rc->left; pts[0].y = rc->top + cutSize;
    pts[1].x = rc->left + cutSize; pts[1].y = rc->top;
    pts[2].x = rc->right - cutSize; pts[2].y = rc->top;
    pts[3].x = rc->right; pts[3].y = rc->top + cutSize;
    pts[4].x = rc->right; pts[4].y = rc->bottom;
    pts[5].x = rc->left; pts[5].y = rc->bottom;

    hBrush = CreateSolidBrush(fill);
    hPen = CreatePen(PS_SOLID, 1, outerStroke);
    oldBrush = SelectObject(hdc, hBrush);
    oldPen = SelectObject(hdc, hPen);
    Polygon(hdc, pts, ARRAYSIZE(pts));
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);

    pts[0].x += 1; pts[0].y += 1;
    pts[1].x += 1; pts[1].y += 1;
    pts[2].x -= 1; pts[2].y += 1;
    pts[3].x -= 1; pts[3].y += 1;
    pts[4].x -= 1; pts[4].y -= 1;
    pts[5].x += 1; pts[5].y -= 1;

    hBrush = CreateSolidBrush(fill);
    hPen = CreatePen(PS_SOLID, 1, innerStroke);
    oldBrush = SelectObject(hdc, hBrush);
    oldPen = SelectObject(hdc, hPen);
    Polygon(hdc, pts, ARRAYSIZE(pts));
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void BikodeTheme_DrawChip(HDC hdc, const RECT* rc, LPCWSTR text,
                          COLORREF fill, COLORREF border, COLORREF textColor,
                          HFONT hFont, BOOL addAccentTab, COLORREF accent)
{
    RECT textRc;
    DrawRoundedShape(hdc, rc, fill, border, 16);
    textRc = *rc;
    if (addAccentTab)
    {
        RECT tab = *rc;
        tab.right = min(rc->right, rc->left + 8);
        HBRUSH hAcc = CreateSolidBrush(accent);
        FillRect(hdc, &tab, hAcc);
        DeleteObject(hAcc);
        textRc.left += 10;
    }
    else
    {
        textRc.left += 10;
    }
    textRc.right -= 10;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textColor);
    SelectObject(hdc, hFont ? hFont : BikodeTheme_GetFont(BKFONT_UI_SMALL));
    DrawTextW(hdc, text, -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void L2(HDC hdc, int x1, int y1, int x2, int y2)
{
    MoveToEx(hdc, x1, y1, NULL);
    LineTo(hdc, x2, y2);
}

static HFONT GetIconFontForSize(int size)
{
    if (size <= 15)
        return s_hIconFontSm ? s_hIconFontSm : (s_hIconFontMd ? s_hIconFontMd : s_hIconFontLg);
    if (size <= 18)
        return s_hIconFontMd ? s_hIconFontMd : (s_hIconFontLg ? s_hIconFontLg : s_hIconFontSm);
    return s_hIconFontLg ? s_hIconFontLg : (s_hIconFontMd ? s_hIconFontMd : s_hIconFontSm);
}

static WCHAR GetIconGlyph(BikodeGlyph glyph)
{
    switch (glyph)
    {
    case BKGLYPH_NEW:       return 0xE710; // Add
    case BKGLYPH_OPEN:      return 0xE8E5; // OpenFile
    case BKGLYPH_SAVE:      return 0xE74E; // Save
    case BKGLYPH_UNDO:      return 0xE7A7; // Undo
    case BKGLYPH_REDO:      return 0xE7A6; // Redo
    case BKGLYPH_CUT:       return 0xE8C6; // Cut
    case BKGLYPH_COPY:      return 0xE8C8; // Copy
    case BKGLYPH_PASTE:     return 0xE77F; // Paste
    case BKGLYPH_FIND:      return 0xE721; // Search
    case BKGLYPH_REPLACE:   return 0xE72C; // Refresh
    case BKGLYPH_WRAP:      return 0xF7B7; // AddNewLine
    case BKGLYPH_ZOOM_IN:   return 0xE8A3; // ZoomIn
    case BKGLYPH_ZOOM_OUT:  return 0xE71F; // ZoomOut
    case BKGLYPH_AGENT:     return 0xE99A; // Robot
    case BKGLYPH_EXPLORER:  return 0xE838; // FolderOpen
    case BKGLYPH_SEARCH:    return 0xE721; // Search
    case BKGLYPH_SYMBOLS:   return 0xE943; // Code
    case BKGLYPH_GIT:       return 0xEA3C; // MergeCall
    case BKGLYPH_PLUGIN:    return 0xEA86; // Puzzle
    case BKGLYPH_SETTINGS:  return 0xE713; // Setting
    case BKGLYPH_TERMINAL:  return 0xE756; // CommandPrompt
    case BKGLYPH_COMMAND:   return 0xE773; // SearchAndApps
    default:                return 0;
    }
}

static BOOL DrawFontGlyph(HDC hdc, BikodeGlyph glyph, const RECT* rc, COLORREF color)
{
    WCHAR text[2] = { 0 };
    HFONT hFont;
    HFONT hOld;
    RECT rcGlyph;

    if (!rc)
        return FALSE;

    text[0] = GetIconGlyph(glyph);
    if (!text[0])
        return FALSE;

    hFont = GetIconFontForSize(min(rc->right - rc->left, rc->bottom - rc->top));
    if (!hFont)
        return FALSE;

    rcGlyph = *rc;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    hOld = (HFONT)SelectObject(hdc, hFont);
    DrawTextW(hdc, text, 1, &rcGlyph, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(hdc, hOld);
    return TRUE;
}

void BikodeTheme_DrawGlyph(HDC hdc, BikodeGlyph glyph, const RECT* rc, COLORREF color, int strokeWidth)
{
    int ox, oy, s;
    HPEN hPen;
    HGDIOBJ oldPen, oldBrush;
    if (!rc) return;
    ox = rc->left;
    oy = rc->top;
    s = min(rc->right - rc->left, rc->bottom - rc->top);

    BikodeTheme_Init();
    if (DrawFontGlyph(hdc, glyph, rc, color))
        return;

    hPen = CreatePen(PS_SOLID, strokeWidth, color);
    oldPen = SelectObject(hdc, hPen);
    oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    switch (glyph)
    {
    case BKGLYPH_EXPLORER:
        L2(hdc, ox + 1, oy + 5, ox + s / 2, oy + 5);
        L2(hdc, ox + s / 2, oy + 5, ox + s / 2 + 2, oy + 7);
        Rectangle(hdc, ox + 1, oy + 7, ox + s - 1, oy + s - 1);
        break;
    case BKGLYPH_SEARCH:
    case BKGLYPH_FIND:
        Ellipse(hdc, ox + 1, oy + 1, ox + s - 5, oy + s - 5);
        L2(hdc, ox + s - 6, oy + s - 6, ox + s - 1, oy + s - 1);
        break;
    case BKGLYPH_SYMBOLS:
        L2(hdc, ox + 2, oy + 3, ox + s - 2, oy + 3);
        L2(hdc, ox + 2, oy + s / 2, ox + s - 4, oy + s / 2);
        L2(hdc, ox + 2, oy + s - 3, ox + s - 6, oy + s - 3);
        Ellipse(hdc, ox + s - 5, oy + 1, ox + s - 1, oy + 5);
        break;
    case BKGLYPH_GIT:
        Ellipse(hdc, ox + 2, oy + 1, ox + 6, oy + 5);
        Ellipse(hdc, ox + s - 6, oy + s - 5, ox + s - 2, oy + s - 1);
        Ellipse(hdc, ox + 2, oy + s - 5, ox + 6, oy + s - 1);
        L2(hdc, ox + 4, oy + 5, ox + 4, oy + s - 5);
        L2(hdc, ox + 4, oy + s - 5, ox + s - 4, oy + s - 3);
        break;
    case BKGLYPH_AGENT:
        RoundRect(hdc, ox + 1, oy + 1, ox + s - 1, oy + s - 3, 6, 6);
        L2(hdc, ox + s / 2 - 1, oy + s - 3, ox + s / 2 - 5, oy + s - 1);
        L2(hdc, ox + s / 2 - 1, oy + s - 3, ox + s / 2 + 4, oy + s - 1);
        break;
    case BKGLYPH_PLUGIN:
        Rectangle(hdc, ox + 2, oy + 4, ox + s - 2, oy + s - 2);
        L2(hdc, ox + s / 2, oy + 1, ox + s / 2, oy + 4);
        L2(hdc, ox + 1, oy + s / 2, ox + 4, oy + s / 2);
        break;
    case BKGLYPH_SETTINGS:
        Ellipse(hdc, ox + 3, oy + 3, ox + s - 3, oy + s - 3);
        Ellipse(hdc, ox + s / 2 - 2, oy + s / 2 - 2, ox + s / 2 + 2, oy + s / 2 + 2);
        break;
    case BKGLYPH_TERMINAL:
        Rectangle(hdc, ox + 1, oy + 2, ox + s - 1, oy + s - 2);
        L2(hdc, ox + 4, oy + 6, ox + 7, oy + 9);
        L2(hdc, ox + 7, oy + 9, ox + 4, oy + 12);
        L2(hdc, ox + 9, oy + 12, ox + s - 4, oy + 12);
        break;
    case BKGLYPH_COMMAND:
        RoundRect(hdc, ox + 1, oy + 2, ox + s - 1, oy + s - 2, 6, 6);
        L2(hdc, ox + 4, oy + 6, ox + s - 4, oy + 6);
        L2(hdc, ox + 4, oy + 10, ox + s - 7, oy + 10);
        break;
    case BKGLYPH_NEW:
        Rectangle(hdc, ox + 3, oy + 2, ox + s - 3, oy + s - 2);
        L2(hdc, ox + s / 2, oy + 5, ox + s / 2, oy + s - 5);
        L2(hdc, ox + 5, oy + s / 2, ox + s - 5, oy + s / 2);
        break;
    case BKGLYPH_OPEN:
        L2(hdc, ox + 1, oy + 6, ox + 5, oy + 3);
        Rectangle(hdc, ox + 1, oy + 5, ox + s - 2, oy + s - 2);
        break;
    case BKGLYPH_SAVE:
        Rectangle(hdc, ox + 2, oy + 2, ox + s - 2, oy + s - 2);
        Rectangle(hdc, ox + 5, oy + 4, ox + s - 5, oy + s / 2);
        break;
    case BKGLYPH_UNDO:
        L2(hdc, ox + 3, oy + 7, ox + 7, oy + 3);
        L2(hdc, ox + 3, oy + 7, ox + 7, oy + 11);
        L2(hdc, ox + 3, oy + 7, ox + s - 4, oy + 7);
        break;
    case BKGLYPH_REDO:
        L2(hdc, ox + s - 3, oy + 7, ox + s - 7, oy + 3);
        L2(hdc, ox + s - 3, oy + 7, ox + s - 7, oy + 11);
        L2(hdc, ox + s - 3, oy + 7, ox + 4, oy + 7);
        break;
    case BKGLYPH_CUT:
        L2(hdc, ox + 4, oy + 3, ox + s - 4, oy + s - 3);
        L2(hdc, ox + s - 4, oy + 3, ox + 4, oy + s - 3);
        break;
    case BKGLYPH_COPY:
        Rectangle(hdc, ox + 2, oy + 4, ox + s - 4, oy + s - 2);
        Rectangle(hdc, ox + 5, oy + 2, ox + s - 1, oy + s - 5);
        break;
    case BKGLYPH_PASTE:
        Rectangle(hdc, ox + 3, oy + 4, ox + s - 3, oy + s - 2);
        Rectangle(hdc, ox + 5, oy + 1, ox + s - 5, oy + 5);
        break;
    case BKGLYPH_REPLACE:
        Ellipse(hdc, ox + 1, oy + 1, ox + s - 5, oy + s - 5);
        L2(hdc, ox + s - 6, oy + s - 6, ox + s - 2, oy + s - 2);
        L2(hdc, ox + s - 6, oy + 2, ox + s - 2, oy + 2);
        break;
    case BKGLYPH_WRAP:
        L2(hdc, ox + 2, oy + 4, ox + s - 3, oy + 4);
        L2(hdc, ox + s - 3, oy + 4, ox + s - 3, oy + 9);
        L2(hdc, ox + s - 3, oy + 9, ox + 6, oy + 9);
        L2(hdc, ox + 6, oy + 9, ox + 8, oy + 7);
        L2(hdc, ox + 6, oy + 9, ox + 8, oy + 11);
        break;
    case BKGLYPH_ZOOM_IN:
        Ellipse(hdc, ox + 1, oy + 1, ox + s - 5, oy + s - 5);
        L2(hdc, ox + s / 2 - 1, oy + 4, ox + s / 2 - 1, oy + s - 8);
        L2(hdc, ox + 4, oy + s / 2 - 1, ox + s - 8, oy + s / 2 - 1);
        L2(hdc, ox + s - 6, oy + s - 6, ox + s - 1, oy + s - 1);
        break;
    case BKGLYPH_ZOOM_OUT:
        Ellipse(hdc, ox + 1, oy + 1, ox + s - 5, oy + s - 5);
        L2(hdc, ox + 4, oy + s / 2 - 1, ox + s - 8, oy + s / 2 - 1);
        L2(hdc, ox + s - 6, oy + s - 6, ox + s - 1, oy + s - 1);
        break;
    default:
        break;
    }

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

void BikodeTheme_DrawButton(HDC hdc, const RECT* rc, LPCWSTR text,
                            BikodeGlyph glyph, BOOL hot, BOOL pressed,
                            BOOL primary, BOOL active)
{
    RECT iconRc = *rc;
    RECT textRc = *rc;
    COLORREF fill = BikodeTheme_GetColor(BKCLR_SURFACE_RAISED);
    COLORREF outer = BikodeTheme_GetColor(BKCLR_STROKE_DARK);
    COLORREF inner = BikodeTheme_GetColor(BKCLR_STROKE_SOFT);
    COLORREF fg = BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY);

    if (primary)
    {
        fill = BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW),
                               BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 60);
        fg = BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY);
    }
    if (active)
    {
        fill = BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_HOT_MAGENTA),
                               BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 40);
        fg = BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY);
    }
    if (hot)
    {
        fill = BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), fill, 38);
        fg = BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY);
    }
    if (pressed)
    {
        fill = BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), fill, 78);
        inner = BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);
        fg = BikodeTheme_GetColor(BKCLR_STROKE_DARK);
    }

    BikodeTheme_DrawRoundedPanel(hdc, rc, fill, outer, inner, BikodeTheme_GetMetric(BKMETRIC_RADIUS_BUTTON), FALSE);

    if (glyph != BKGLYPH_NONE)
    {
        int size = BikodeTheme_GetMetric(BKMETRIC_ICON_MD);
        iconRc.left = rc->left + 10;
        iconRc.top = rc->top + ((rc->bottom - rc->top) - size) / 2;
        iconRc.right = iconRc.left + size;
        iconRc.bottom = iconRc.top + size;
        BikodeTheme_DrawGlyph(hdc, glyph, &iconRc, fg, 2);
        textRc.left = iconRc.right + 8;
    }
    else
    {
        textRc.left += 10;
    }
    textRc.right -= 10;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_BOLD));
    DrawTextW(hdc, text ? text : L"", -1, &textRc, DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}
