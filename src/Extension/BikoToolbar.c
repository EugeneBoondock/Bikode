/******************************************************************************
*
* Biko
*
* BikoToolbar.c
*   Compact top bar: branding, project, quick command, and status chips.
*
******************************************************************************/

#include "BikoToolbar.h"
#include "ui/theme/BikodeTheme.h"
#include "AICommands.h"
#include "GitUI.h"
#include "FileManager.h"
#include "DarkMode.h"
#include "PreviewTab.h"
#include "../resource.h"
#include <windowsx.h>
#include <stdio.h>
#include <string.h>

extern WCHAR szCurFile[MAX_PATH + 40];

static HWND s_hwnd = NULL;
static BOOL s_visible = TRUE;
static BOOL s_registered = FALSE;
static BOOL s_tracking = FALSE;
static BOOL s_hoverCommand = FALSE;
static RECT s_rcCommand = { 0 };
static BOOL s_hoverPreview = FALSE;
static RECT s_rcPreview = { 0 };
static BOOL s_hoverOpen = FALSE;
static RECT s_rcOpen = { 0 };
static BOOL s_hoverThemeSun = FALSE;
static BOOL s_hoverThemeMoon = FALSE;
static RECT s_rcThemeDock = { 0 };
static RECT s_rcThemeSun = { 0 };
static RECT s_rcThemeMoon = { 0 };
static HICON s_hBrandIcon = NULL;
static HFONT s_hBrandFont = NULL;
static HFONT s_hThemeGlyphFont = NULL;

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

static void EnsureBrandAssets(void)
{
    if (!s_hBrandFont) {
        s_hBrandFont = CreateFontW(-19, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH | FF_SWISS, L"Segoe UI Black");
        if (!s_hBrandFont)
            s_hBrandFont = BikodeTheme_GetFont(BKFONT_UI_BOLD);
    }

    if (!s_hBrandIcon) {
        s_hBrandIcon = (HICON)LoadImageW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDR_MAINWND),
            IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR);
    }

    if (!s_hThemeGlyphFont) {
        s_hThemeGlyphFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
    }
}

static int MeasureTextWidth(HDC hdc, HFONT hFont, LPCWSTR text)
{
    SIZE sz = { 0 };
    HFONT hOld;
    if (!text || !text[0])
        return 0;
    hOld = (HFONT)SelectObject(hdc, hFont);
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    SelectObject(hdc, hOld);
    return sz.cx;
}

static void BuildProjectLabel(WCHAR* wszBuf, int cchBuf)
{
    const WCHAR* root = FileManager_GetRootPath();
    const WCHAR* leaf = NULL;
    if (!wszBuf || cchBuf <= 0)
        return;
    wszBuf[0] = L'\0';

    if (root && root[0]) {
        leaf = wcsrchr(root, L'\\');
        leaf = (leaf && leaf[1]) ? leaf + 1 : root;
        lstrcpynW(wszBuf, leaf, cchBuf);
        return;
    }

    if (szCurFile[0]) {
        leaf = wcsrchr(szCurFile, L'\\');
        leaf = (leaf && leaf[1]) ? leaf + 1 : szCurFile;
        lstrcpynW(wszBuf, leaf, cchBuf);
        return;
    }

    lstrcpynW(wszBuf, L"Untitled", cchBuf);
}

static void BuildWorkspaceChip(WCHAR* wszBuf, int cchBuf)
{
    const WCHAR* root = FileManager_GetRootPath();
    const WCHAR* leaf = NULL;
    if (!wszBuf || cchBuf <= 0)
        return;
    wszBuf[0] = L'\0';
    if (!root || !root[0]) {
        lstrcpynW(wszBuf, L"WS Solo", cchBuf);
        return;
    }
    leaf = wcsrchr(root, L'\\');
    leaf = (leaf && leaf[1]) ? leaf + 1 : root;
    swprintf_s(wszBuf, cchBuf, L"WS %s", leaf);
}

static int DrawChipAuto(HDC hdc, int right, int top, LPCWSTR text, COLORREF accent, BOOL strong)
{
    RECT rcChip;
    int width = MeasureTextWidth(hdc,
        strong ? BikodeTheme_GetFont(BKFONT_UI_SMALL) : BikodeTheme_GetFont(BKFONT_MONO_SMALL),
        text) + 26;
    rcChip.right = right;
    rcChip.left = right - width;
    rcChip.top = top;
    rcChip.bottom = top + 22;
    BikodeTheme_DrawChip(hdc, &rcChip, text,
        BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        strong ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY) : BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
        strong ? BikodeTheme_GetFont(BKFONT_UI_SMALL) : BikodeTheme_GetFont(BKFONT_MONO_SMALL),
        TRUE, accent);
    return width;
}

static int DrawBrandWordmark(HDC hdc, int left, int top)
{
    SIZE szBi = { 0 };
    SIZE szKo = { 0 };
    SIZE szDe = { 0 };
    HFONT hOld;
    int iconSz = 20;
    int textX;
    int textY = top + 5;

    EnsureBrandAssets();
    hOld = (HFONT)SelectObject(hdc, s_hBrandFont);
    GetTextExtentPoint32W(hdc, L"BI", 2, &szBi);
    GetTextExtentPoint32W(hdc, L"KO", 2, &szKo);
    GetTextExtentPoint32W(hdc, L"DE", 2, &szDe);

    if (s_hBrandIcon)
        DrawIconEx(hdc, left, top + 6, s_hBrandIcon, iconSz, iconSz, 0, NULL, DI_NORMAL);

    textX = left + (s_hBrandIcon ? iconSz + 8 : 0);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    TextOutW(hdc, textX, textY, L"BI", 2);

    textX += szBi.cx;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));
    TextOutW(hdc, textX, textY, L"KO", 2);

    textX += szKo.cx;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    TextOutW(hdc, textX, textY, L"DE", 2);

    SelectObject(hdc, hOld);
    return (s_hBrandIcon ? iconSz + 8 : 0) + szBi.cx + szKo.cx + szDe.cx;
}

static void DrawThemeGlyph(HDC hdc, const RECT* rc, BOOL sun, COLORREF color)
{
    HFONT hOld;

    if (!rc)
        return;

    EnsureBrandAssets();
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    hOld = (HFONT)SelectObject(hdc, s_hThemeGlyphFont ? s_hThemeGlyphFont : BikodeTheme_GetFont(BKFONT_UI_BOLD));
    DrawTextW(hdc, sun ? L"\x2600" : L"\x263D", 1, (LPRECT)rc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, hOld);
}

static void PaintThemeToggle(HDC hdc, const RECT* rcDock)
{
    RECT rcSun;
    RECT rcMoon;
    RECT rcGlyph;
    BOOL darkEnabled = DarkMode_IsEnabled();
    COLORREF dockFill = BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_APP_BG), 228);
    COLORREF dockInner = BikodeTheme_GetColor(BKCLR_STROKE_SOFT);
    COLORREF sunAccent = BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);
    COLORREF moonAccent = BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
    COLORREF fill;
    COLORREF border;
    COLORREF glyph;

    if (!rcDock)
        return;

    s_rcThemeDock = *rcDock;
    BikodeTheme_DrawRoundedPanel(hdc, rcDock, dockFill,
        BikodeTheme_GetColor(BKCLR_STROKE_DARK), dockInner,
        BikodeTheme_GetMetric(BKMETRIC_RADIUS_BUTTON), FALSE);

    rcSun = *rcDock;
    rcMoon = *rcDock;
    InflateRect(&rcSun, -3, -3);
    InflateRect(&rcMoon, -3, -3);
    rcSun.right = rcDock->left + ((rcDock->right - rcDock->left) / 2) - 1;
    rcMoon.left = rcSun.right + 2;

    s_rcThemeSun = rcSun;
    s_rcThemeMoon = rcMoon;

    fill = (!darkEnabled)
        ? BikodeTheme_Mix(sunAccent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 96)
        : (s_hoverThemeSun
            ? BikodeTheme_Mix(sunAccent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 34)
            : dockFill);
    border = (!darkEnabled) ? sunAccent
        : (s_hoverThemeSun ? BikodeTheme_Mix(sunAccent, BikodeTheme_GetColor(BKCLR_STROKE_SOFT), 120) : dockFill);
    glyph = (!darkEnabled)
        ? BikodeTheme_GetColor(BKCLR_STROKE_DARK)
        : (s_hoverThemeSun ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY) : BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    if (!darkEnabled || s_hoverThemeSun) {
        BikodeTheme_DrawRoundedPanel(hdc, &rcSun, fill, fill, border,
            BikodeTheme_GetMetric(BKMETRIC_RADIUS_BUTTON) - 2, FALSE);
    }
    rcGlyph = rcSun;
    DrawThemeGlyph(hdc, &rcGlyph, TRUE, glyph);

    fill = darkEnabled
        ? BikodeTheme_Mix(moonAccent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 76)
        : (s_hoverThemeMoon
            ? BikodeTheme_Mix(moonAccent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 30)
            : dockFill);
    border = darkEnabled ? moonAccent
        : (s_hoverThemeMoon ? BikodeTheme_Mix(moonAccent, BikodeTheme_GetColor(BKCLR_STROKE_SOFT), 116) : dockFill);
    glyph = darkEnabled
        ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY)
        : (s_hoverThemeMoon ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY) : BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    if (darkEnabled || s_hoverThemeMoon) {
        BikodeTheme_DrawRoundedPanel(hdc, &rcMoon, fill, fill, border,
            BikodeTheme_GetMetric(BKMETRIC_RADIUS_BUTTON) - 2, FALSE);
    }
    rcGlyph = rcMoon;
    DrawThemeGlyph(hdc, &rcGlyph, FALSE, glyph);
}

static void Paint(HWND hwnd, HDC hdc)
{
    RECT rc;
    RECT rcBrand;
    RECT rcProject;
    RECT rcCommandIcon;
    WCHAR wszProject[128];
    WCHAR wszWorkspace[128];
    WCHAR wszAI[128];
    const WCHAR* branch = GitUI_GetBranch();
    int height = BikodeTheme_GetMetric(BKMETRIC_TITLE_HEIGHT);
    int chipRight;
    int commandW;
    int themeW = 74;

    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0)
        return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP obm = (HBITMAP)SelectObject(mem, bm);

    BikodeTheme_FillHalftone(mem, &rc, BikodeTheme_GetColor(BKCLR_APP_BG));

    {
        HPEN hTop = CreatePen(PS_SOLID, 1, BikodeTheme_GetColor(BKCLR_STROKE_DARK));
        HPEN hSoft = CreatePen(PS_SOLID, 1, BikodeTheme_GetColor(BKCLR_STROKE_SOFT));
        HPEN hOld = (HPEN)SelectObject(mem, hTop);
        MoveToEx(mem, rc.left, rc.bottom - 2, NULL);
        LineTo(mem, rc.right, rc.bottom - 2);
        SelectObject(mem, hSoft);
        MoveToEx(mem, rc.left, rc.bottom - 1, NULL);
        LineTo(mem, rc.right, rc.bottom - 1);
        SelectObject(mem, hOld);
        DeleteObject(hTop);
        DeleteObject(hSoft);
    }

    BuildProjectLabel(wszProject, ARRAYSIZE(wszProject));
    BuildWorkspaceChip(wszWorkspace, ARRAYSIZE(wszWorkspace));
    AICommands_GetStatusText(wszAI, ARRAYSIZE(wszAI));

    rcBrand.left = 10;
    rcBrand.top = 0;
    rcBrand.right = rcBrand.left + DrawBrandWordmark(mem, rcBrand.left, rcBrand.top);
    rcBrand.bottom = height;

    rcProject.left = rcBrand.right + 14;
    rcProject.top = 0;
    rcProject.right = rcProject.left + 220;
    rcProject.bottom = height;
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    SelectObject(mem, BikodeTheme_GetFont(BKFONT_UI_BOLD));
    DrawTextW(mem, wszProject, -1, &rcProject, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    s_rcThemeDock.right = rc.right - 10;
    s_rcThemeDock.left = s_rcThemeDock.right - themeW;
    s_rcThemeDock.top = 4;
    s_rcThemeDock.bottom = height - 4;
    PaintThemeToggle(mem, &s_rcThemeDock);

    chipRight = s_rcThemeDock.left - 10;
    chipRight -= DrawChipAuto(mem, chipRight, 5, wszAI, BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), TRUE) + 8;
    chipRight -= DrawChipAuto(mem, chipRight, 5, (branch && *branch) ? branch : L"No Branch", BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), FALSE) + 8;
    chipRight -= DrawChipAuto(mem, chipRight, 5, wszWorkspace, BikodeTheme_GetColor(BKCLR_HOT_MAGENTA), FALSE) + 8;

    commandW = min(340, max(240, rc.right / 3));
    int previewW = 96;
    
    s_rcCommand.left = max(rcProject.right + 20 + previewW + 10, (rc.right - commandW) / 2);
    s_rcCommand.right = min(chipRight - 10, s_rcCommand.left + commandW);
    
    // Position Preview Button left of Quick Command
    s_rcPreview.right = s_rcCommand.left - 10;
    s_rcPreview.left = s_rcPreview.right - previewW;
    s_rcPreview.top = 4;
    s_rcPreview.bottom = height - 4;

    // Position Open Button left of Preview Button
    s_rcOpen.right = s_rcPreview.left - 10;
    s_rcOpen.left = s_rcOpen.right - previewW;
    s_rcOpen.top = 4;
    s_rcOpen.bottom = height - 4;

    if (s_rcPreview.left > rcProject.right + 10) {
        BikodeTheme_DrawButton(mem, &s_rcPreview, L"Preview", BKGLYPH_SEARCH, s_hoverPreview, FALSE, FALSE, FALSE);
    } else {
        SetRectEmpty(&s_rcPreview);
    }
    if (s_rcOpen.left > rcProject.right + 10) {
        BikodeTheme_DrawButton(mem, &s_rcOpen, L"Open", BKGLYPH_OPEN, s_hoverOpen, FALSE, FALSE, FALSE);
    } else {
        SetRectEmpty(&s_rcOpen);
    }
    s_rcCommand.top = 4;
    s_rcCommand.bottom = height - 4;
    if (s_rcCommand.right < s_rcCommand.left + 180) {
        SetRectEmpty(&s_rcCommand);
    } else {
        BikodeTheme_DrawRoundedPanel(mem, &s_rcCommand,
            s_hoverCommand
                ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 22)
                : BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            s_hoverCommand ? BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            BikodeTheme_GetMetric(BKMETRIC_RADIUS_BUTTON), FALSE);

        rcCommandIcon = s_rcCommand;
        rcCommandIcon.left += 8;
        rcCommandIcon.top += 4;
        rcCommandIcon.right = rcCommandIcon.left + 16;
        rcCommandIcon.bottom = rcCommandIcon.top + 16;
        BikodeTheme_DrawGlyph(mem, BKGLYPH_COMMAND, &rcCommandIcon,
            BikodeTheme_GetColor(BKCLR_TEXT_MUTED), 2);

        rcCommandIcon.left = rcCommandIcon.right + 6;
        rcCommandIcon.right = s_rcCommand.right - 10;
        SetTextColor(mem, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
        SelectObject(mem, BikodeTheme_GetFont(BKFONT_UI_SMALL));
        DrawTextW(mem, L"Quick Command", -1, &rcCommandIcon,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, obm);
    DeleteObject(bm);
    DeleteDC(mem);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        BOOL hoverCommand = PtInRect(&s_rcCommand, pt);
        BOOL hoverPreview = PtInRect(&s_rcPreview, pt);
        BOOL hoverOpen = PtInRect(&s_rcOpen, pt);
        BOOL hoverThemeSun = PtInRect(&s_rcThemeSun, pt);
        BOOL hoverThemeMoon = PtInRect(&s_rcThemeMoon, pt);
        if (!s_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            s_tracking = TRUE;
        }
        if (hoverCommand != s_hoverCommand || hoverPreview != s_hoverPreview ||
            hoverOpen != s_hoverOpen || hoverThemeSun != s_hoverThemeSun ||
            hoverThemeMoon != s_hoverThemeMoon) {
            s_hoverCommand = hoverCommand;
            s_hoverPreview = hoverPreview;
            s_hoverOpen = hoverOpen;
            s_hoverThemeSun = hoverThemeSun;
            s_hoverThemeMoon = hoverThemeMoon;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        s_tracking = FALSE;
        s_hoverCommand = FALSE;
        s_hoverPreview = FALSE;
        s_hoverOpen = FALSE;
        s_hoverThemeSun = FALSE;
        s_hoverThemeMoon = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT &&
            (s_hoverCommand || s_hoverPreview || s_hoverOpen || s_hoverThemeSun || s_hoverThemeMoon)) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&s_rcCommand, pt)) {
            PostMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDM_BIKO_COMMAND_PALETTE, 0), 0);
            return 0;
        }
        if (PtInRect(&s_rcPreview, pt)) {
            PreviewTab_Toggle();
            return 0;
        }
        if (PtInRect(&s_rcOpen, pt)) {
            FileManager_BrowseForFolder(GetParent(hwnd));
            return 0;
        }
        if (PtInRect(&s_rcThemeSun, pt)) {
            if (DarkMode_IsEnabled()) {
                PostMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDM_VIEW_DARKMODE, 0), 0);
            }
            return 0;
        }
        if (PtInRect(&s_rcThemeMoon, pt)) {
            if (!DarkMode_IsEnabled()) {
                PostMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDM_VIEW_DARKMODE, 0), 0);
            }
            return 0;
        }
        break;
    }

    case WM_SIZE:
    case WM_DPICHANGED:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

HWND BikoToolbar_Create(HWND hwndParent, HINSTANCE hInst)
{
    if (!s_registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"BikoToolbar";
        RegisterClassExW(&wc);
        s_registered = TRUE;
    }

    s_hwnd = CreateWindowExW(
        0, L"BikoToolbar", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 800, BikodeTheme_GetMetric(BKMETRIC_TITLE_HEIGHT),
        hwndParent, (HMENU)(UINT_PTR)0xFB10, hInst, NULL);

    return s_hwnd;
}

void BikoToolbar_Destroy(void)
{
    if (s_hwnd) {
        DestroyWindow(s_hwnd);
        s_hwnd = NULL;
    }
    if (s_hBrandIcon) {
        DestroyIcon(s_hBrandIcon);
        s_hBrandIcon = NULL;
    }
    if (s_hBrandFont && s_hBrandFont != BikodeTheme_GetFont(BKFONT_UI_BOLD)) {
        DeleteObject(s_hBrandFont);
    }
    s_hBrandFont = NULL;
    if (s_hThemeGlyphFont) {
        DeleteObject(s_hThemeGlyphFont);
        s_hThemeGlyphFont = NULL;
    }
}

int BikoToolbar_GetHeight(void)
{
    return s_visible ? BikodeTheme_GetMetric(BKMETRIC_TITLE_HEIGHT) : 0;
}

void BikoToolbar_EnableButton(int cmdId, BOOL bEnable)
{
    UNREFERENCED_PARAMETER(cmdId);
    UNREFERENCED_PARAMETER(bEnable);
}

void BikoToolbar_CheckButton(int cmdId, BOOL bCheck)
{
    UNREFERENCED_PARAMETER(cmdId);
    UNREFERENCED_PARAMETER(bCheck);
}

void BikoToolbar_ApplyDarkMode(void)
{
    if (s_hwnd)
        InvalidateRect(s_hwnd, NULL, TRUE);
}

HWND BikoToolbar_GetHwnd(void)
{
    return s_hwnd;
}

void BikoToolbar_Show(BOOL bShow)
{
    s_visible = bShow;
    if (s_hwnd)
        ShowWindow(s_hwnd, bShow ? SW_SHOW : SW_HIDE);
}

BOOL BikoToolbar_IsVisible(void)
{
    return s_visible;
}
