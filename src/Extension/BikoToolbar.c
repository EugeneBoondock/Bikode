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
#include <windowsx.h>
#include <string.h>

extern WCHAR szCurFile[MAX_PATH + 40];

static HWND s_hwnd = NULL;
static BOOL s_visible = TRUE;
static BOOL s_registered = FALSE;
static BOOL s_tracking = FALSE;
static BOOL s_hoverCommand = FALSE;
static RECT s_rcCommand = { 0 };

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

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
    _snwprintf_s(wszBuf, cchBuf, _TRUNCATE, L"WS %s", leaf);
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

    rcBrand.left = 8;
    rcBrand.top = 5;
    rcBrand.right = 84;
    rcBrand.bottom = height - 5;
    BikodeTheme_DrawChip(mem, &rcBrand, L"BIKODE",
        BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 48),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
        BikodeTheme_GetFont(BKFONT_TITLE), TRUE,
        BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));

    rcProject.left = rcBrand.right + 10;
    rcProject.top = 0;
    rcProject.right = rcProject.left + 220;
    rcProject.bottom = height;
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    SelectObject(mem, BikodeTheme_GetFont(BKFONT_UI_BOLD));
    DrawTextW(mem, wszProject, -1, &rcProject, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    chipRight = rc.right - 10;
    chipRight -= DrawChipAuto(mem, chipRight, 5, wszAI, BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), TRUE) + 8;
    chipRight -= DrawChipAuto(mem, chipRight, 5, (branch && *branch) ? branch : L"No Branch", BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), FALSE) + 8;
    chipRight -= DrawChipAuto(mem, chipRight, 5, wszWorkspace, BikodeTheme_GetColor(BKCLR_HOT_MAGENTA), FALSE) + 8;

    commandW = min(340, max(240, rc.right / 3));
    s_rcCommand.left = max(rcProject.right + 20, (rc.right - commandW) / 2);
    s_rcCommand.right = min(chipRight - 10, s_rcCommand.left + commandW);
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
        BOOL hover = PtInRect(&s_rcCommand, pt);
        if (!s_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            s_tracking = TRUE;
        }
        if (hover != s_hoverCommand) {
            s_hoverCommand = hover;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        s_tracking = FALSE;
        s_hoverCommand = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && s_hoverCommand) {
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
