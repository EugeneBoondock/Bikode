/******************************************************************************
*
* Biko
*
* BikoSideRail.c
*   Slim comic-tech utility belt for core navigation.
*
******************************************************************************/

#include "BikoSideRail.h"
#include "../theme/BikodeTheme.h"
#include "../../AICommands.h"
#include "../../FileManager.h"
#include "../../ChatPanel.h"
#include "../../MissionControl.h"
#include "../../GitUI.h"
#include "../../PluginManager.h"
#include "../../DarkMode.h"
#include "../../../resource.h"
#include <commctrl.h>
#include <windowsx.h>

#define RAIL_BTN_COUNT 8
#define RAIL_MISSION_INDEX 4

typedef struct RailButton {
    UINT        cmdId;
    BikodeGlyph glyph;
    LPCWSTR     label;
    RECT        rc;
} RailButton;

static HWND s_hwnd = NULL;
static HWND s_hwndMain = NULL;
static HWND s_hwndTip = NULL;
static BOOL s_registered = FALSE;
static BOOL s_tracking = FALSE;
static int  s_hover = -1;
static UINT s_activeCmd = IDM_FILEMGR_TOGGLE;
static RailButton s_buttons[RAIL_BTN_COUNT] = {
    { IDM_FILEMGR_TOGGLE,  BKGLYPH_EXPLORER, L"Explorer", {0} },
    { IDM_EDIT_FIND,       BKGLYPH_SEARCH,   L"Search",   {0} },
    { IDM_VIEW_SHOWOUTLINE,BKGLYPH_SYMBOLS,  L"Symbols",  {0} },
    { IDM_GIT_STATUS,      BKGLYPH_GIT,      L"Git",      {0} },
    { IDM_AI_MISSION_CONTROL, BKGLYPH_COMMAND, L"Command Center", {0} },
    { IDM_AI_TOGGLE_CHAT,  BKGLYPH_AGENT,    L"Quick Chat",   {0} },
    { IDM_PLUGIN_SETTINGS, BKGLYPH_PLUGIN,   L"Plugins",  {0} },
    { IDM_AI_SETTINGS,     BKGLYPH_SETTINGS, L"Settings", {0} }
};

static LRESULT CALLBACK SideRailProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void LayoutButtons(int cx, int cy)
{
    int top = 8;
    int size = 32;
    int gap = 6;
    int x = (cx - size) / 2;
    for (int i = 0; i < RAIL_BTN_COUNT; i++)
    {
        s_buttons[i].rc.left = x;
        s_buttons[i].rc.top = top + i * (size + gap);
        s_buttons[i].rc.right = x + size;
        s_buttons[i].rc.bottom = s_buttons[i].rc.top + size;
    }
    if (cy > 160)
    {
        int bottomTop = cy - 40;
        s_buttons[RAIL_BTN_COUNT - 1].rc.top = bottomTop;
        s_buttons[RAIL_BTN_COUNT - 1].rc.bottom = bottomTop + size;
    }
}

static int HitTest(int x, int y)
{
    POINT pt = { x, y };
    for (int i = 0; i < RAIL_BTN_COUNT; i++)
    {
        if (PtInRect(&s_buttons[i].rc, pt))
            return i;
    }
    return -1;
}

static UINT GetActiveCommand(void)
{
    if (FileManager_IsVisible()) return IDM_FILEMGR_TOGGLE;
    if (MissionControl_IsVisible()) return IDM_AI_MISSION_CONTROL;
    if (ChatPanel_IsVisible()) return IDM_AI_TOGGLE_CHAT;
    if (GitUI_IsPanelVisible()) return IDM_GIT_STATUS;
    return s_activeCmd;
}

static void Paint(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    BikodeTheme_DrawRoundedPanel(hdc, &rc,
        BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetMetric(BKMETRIC_RADIUS_PANEL),
        TRUE);

    UINT active = GetActiveCommand();
    for (int i = 0; i < RAIL_BTN_COUNT; i++)
    {
        BOOL hot = (i == s_hover);
        BOOL activeBtn = (s_buttons[i].cmdId == active);
        if (i == RAIL_MISSION_INDEX)
        {
            RECT glow = s_buttons[i].rc;
            COLORREF glowFill = activeBtn
                ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), 48)
                : BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), hot ? 40 : 18);
            InflateRect(&glow, 2, 2);
            BikodeTheme_DrawRoundedPanel(hdc, &glow, glowFill,
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                activeBtn ? BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW) : BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN),
                BikodeTheme_GetMetric(BKMETRIC_RADIUS_BUTTON) + 2, FALSE);
            {
                RECT dot = { glow.right - 7, glow.top + 3, glow.right - 3, glow.top + 7 };
                HBRUSH hAccent = CreateSolidBrush(activeBtn ? BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW) : BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));
                FillRect(hdc, &dot, hAccent);
                DeleteObject(hAccent);
            }
        }
        BikodeTheme_DrawButton(hdc, &s_buttons[i].rc, L"", s_buttons[i].glyph, hot, FALSE, FALSE, activeBtn);
        if (activeBtn)
        {
            RECT strip = { 2, s_buttons[i].rc.top + 4, 5, s_buttons[i].rc.bottom - 4 };
            HBRUSH hStrip = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
            FillRect(hdc, &strip, hStrip);
            DeleteObject(hStrip);
        }
    }

}

static LRESULT CALLBACK SideRailProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
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

    case WM_SIZE:
        LayoutButtons(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE:
    {
        int hover = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (!s_tracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            s_tracking = TRUE;
        }
        if (hover != s_hover)
        {
            s_hover = hover;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        s_tracking = FALSE;
        s_hover = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONUP:
    {
        int idx = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (idx >= 0)
        {
            s_activeCmd = s_buttons[idx].cmdId;
            // Avoid re-entering the parent window proc from inside the custom rail callback.
            PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(s_buttons[idx].cmdId, 0), 0);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL BikoSideRail_Init(HWND hwndMain)
{
    HINSTANCE hInst;
    BikodeTheme_Init();
    s_hwndMain = hwndMain;
    hInst = (HINSTANCE)GetWindowLongPtr(hwndMain, GWLP_HINSTANCE);

    if (!s_registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SideRailProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"BikoSideRail";
        RegisterClassExW(&wc);
        s_registered = TRUE;
    }

    if (!s_hwnd)
    {
        s_hwnd = CreateWindowExW(0, L"BikoSideRail", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, BikodeTheme_GetMetric(BKMETRIC_RAIL_WIDTH), 100,
            hwndMain, (HMENU)(UINT_PTR)0xFB70, hInst, NULL);
        s_hwndTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, s_hwnd, NULL, hInst, NULL);
        if (s_hwndTip)
        {
            for (int i = 0; i < RAIL_BTN_COUNT; i++)
            {
                TOOLINFOW ti = { sizeof(ti) };
                ti.uFlags = TTF_SUBCLASS;
                ti.hwnd = s_hwnd;
                ti.uId = i + 1;
                ti.rect = s_buttons[i].rc;
                ti.lpszText = (LPWSTR)s_buttons[i].label;
                SendMessageW(s_hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            }
        }
    }

    return s_hwnd != NULL;
}

void BikoSideRail_Shutdown(void)
{
    if (s_hwnd)
    {
        DestroyWindow(s_hwnd);
        s_hwnd = NULL;
    }
    if (s_hwndTip)
    {
        DestroyWindow(s_hwndTip);
        s_hwndTip = NULL;
    }
}

int BikoSideRail_Layout(HWND hwndParent, int* px, int* pcx, int y, int cy)
{
    int w;
    if (!s_hwnd) return 0;
    w = BikodeTheme_GetMetric(BKMETRIC_RAIL_WIDTH);
    MoveWindow(s_hwnd, *px, y, w, cy, TRUE);
    if (s_hwndTip)
    {
        for (int i = 0; i < RAIL_BTN_COUNT; i++)
        {
            TOOLINFOW ti = { sizeof(ti) };
            ti.hwnd = s_hwnd;
            ti.uId = i + 1;
            ti.rect = s_buttons[i].rc;
            SendMessageW(s_hwndTip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
    *px += w + 6;
    *pcx -= w + 6;
    InvalidateRect(s_hwnd, NULL, FALSE);
    UNREFERENCED_PARAMETER(hwndParent);
    return w;
}

void BikoSideRail_ApplyTheme(void)
{
    if (s_hwnd)
        InvalidateRect(s_hwnd, NULL, TRUE);
}

HWND BikoSideRail_GetHwnd(void)
{
    return s_hwnd;
}
