/******************************************************************************
*
* Biko
*
* BikoToolbar.c
*   Custom owner-drawn toolbar with COMIC BOOK style.
*   Uses GDI vector-drawn icons for guaranteed rendering on any Windows.
*
******************************************************************************/

#include "BikoToolbar.h"
#include "DarkMode.h"
#include <windowsx.h>
#include <commctrl.h>

//=============================================================================
// Design constants
//=============================================================================

#define BTB_HEIGHT          38
#define BTB_BTN_WIDTH       34
#define BTB_SEP_WIDTH       12
#define BTB_ICON_SZ         16

// COMIC BOOK PALETTE
#define BTB_DK_BG           RGB(14,  14,  16)    // near-black ink panel
#define BTB_DK_HOVER        RGB(255, 215, 0)     // POW! yellow
#define BTB_DK_PRESS        RGB(210, 20,  20)    // BOOM! red
#define BTB_DK_CHECK        RGB(30,  100, 210)   // vivid blue
#define BTB_DK_ICON         RGB(235, 235, 235)   // white on idle
#define BTB_DK_DISABLED     RGB(60,  60,  65)    // very dim
#define BTB_DK_SEP          RGB(80,  75,  20)    // muted yellow
#define BTB_DK_ACCENT       RGB(255, 215, 0)     // Biko AI always yellow

// Light palette (unused in dark/comic mode)
#define BTB_LT_BG           RGB(245, 245, 245)
#define BTB_LT_HOVER        RGB(225, 225, 230)
#define BTB_LT_PRESS        RGB(210, 210, 215)
#define BTB_LT_CHECK        RGB(218, 218, 225)
#define BTB_LT_ICON         RGB(50,  50,  55)
#define BTB_LT_DISABLED     RGB(180, 180, 185)
#define BTB_LT_SEP          RGB(210, 210, 215)
#define BTB_LT_ACCENT       RGB(79,  70,  229)

//=============================================================================
// Icon types
//=============================================================================

typedef enum {
    ICON_NONE = 0,
    ICON_NEW, ICON_OPEN, ICON_SAVE,
    ICON_UNDO, ICON_REDO,
    ICON_CUT, ICON_COPY, ICON_PASTE,
    ICON_FIND, ICON_REPLACE,
    ICON_PRINT, ICON_WRAP,
    ICON_ZOOMIN, ICON_ZOOMOUT,
    ICON_BIKO
} IconType;

//=============================================================================
// Button definition
//=============================================================================

typedef struct {
    int      cmdId;
    IconType icon;
    LPCWSTR  tooltip;
    BOOL     enabled;
    BOOL     checked;
} TBButton;

#define CMD_FILE_NEW        40700
#define CMD_FILE_OPEN       40701
#define CMD_FILE_SAVE       40703
#define CMD_EDIT_UNDO       40704
#define CMD_EDIT_REDO       40705
#define CMD_EDIT_CUT        40706
#define CMD_EDIT_COPY       40707
#define CMD_EDIT_PASTE      40708
#define CMD_EDIT_FIND       40709
#define CMD_EDIT_REPLACE    40710
#define CMD_FILE_PRINT      40720
#define CMD_VIEW_WRAP       40711
#define CMD_VIEW_ZOOMIN     40712
#define CMD_VIEW_ZOOMOUT    40713
#define CMD_BIKO_AI         41032

static TBButton s_btns[] = {
    { CMD_FILE_NEW,     ICON_NEW,     L"New (Ctrl+N)",            TRUE, FALSE },
    { CMD_FILE_OPEN,    ICON_OPEN,    L"Open (Ctrl+O)",           TRUE, FALSE },
    { CMD_FILE_SAVE,    ICON_SAVE,    L"Save (Ctrl+S)",           TRUE, FALSE },
    { 0, ICON_NONE, NULL, FALSE, FALSE },
    { CMD_EDIT_UNDO,    ICON_UNDO,    L"Undo (Ctrl+Z)",           TRUE, FALSE },
    { CMD_EDIT_REDO,    ICON_REDO,    L"Redo (Ctrl+Y)",           TRUE, FALSE },
    { 0, ICON_NONE, NULL, FALSE, FALSE },
    { CMD_EDIT_CUT,     ICON_CUT,     L"Cut (Ctrl+X)",            TRUE, FALSE },
    { CMD_EDIT_COPY,    ICON_COPY,    L"Copy (Ctrl+C)",           TRUE, FALSE },
    { CMD_EDIT_PASTE,   ICON_PASTE,   L"Paste (Ctrl+V)",          TRUE, FALSE },
    { 0, ICON_NONE, NULL, FALSE, FALSE },
    { CMD_EDIT_FIND,    ICON_FIND,    L"Find (Ctrl+F)",           TRUE, FALSE },
    { CMD_EDIT_REPLACE, ICON_REPLACE, L"Replace (Ctrl+H)",        TRUE, FALSE },
    { 0, ICON_NONE, NULL, FALSE, FALSE },
    { CMD_VIEW_WRAP,    ICON_WRAP,    L"Word Wrap",               TRUE, FALSE },
    { CMD_VIEW_ZOOMIN,  ICON_ZOOMIN,  L"Zoom In (Ctrl++)",        TRUE, FALSE },
    { CMD_VIEW_ZOOMOUT, ICON_ZOOMOUT, L"Zoom Out (Ctrl+-)",       TRUE, FALSE },
    { 0, ICON_NONE, NULL, FALSE, FALSE },
    { CMD_BIKO_AI,      ICON_BIKO,    L"Bikode AI (Ctrl+Shift+C)", TRUE, FALSE },
};

#define BTN_COUNT (sizeof(s_btns) / sizeof(s_btns[0]))

//=============================================================================
// State
//=============================================================================

static HWND  s_hwnd      = NULL;
static HWND  s_hwndTip   = NULL;
static int   s_hover     = -1;
static int   s_press     = -1;
static BOOL  s_visible   = TRUE;
static BOOL  s_tracking  = FALSE;
static BOOL  s_registered = FALSE;

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

//=============================================================================
// Geometry
//=============================================================================

static RECT BtnRect(int i)
{
    RECT r = { 0, 2, 0, BTB_HEIGHT - 2 };
    int x = 4;
    for (int j = 0; j < i; j++)
        x += (s_btns[j].cmdId == 0) ? BTB_SEP_WIDTH : BTB_BTN_WIDTH;
    r.left  = x;
    r.right = x + ((s_btns[i].cmdId == 0) ? BTB_SEP_WIDTH : BTB_BTN_WIDTH);
    return r;
}

static int HitTest(int x, int y)
{
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        if (s_btns[i].cmdId == 0) continue;
        RECT r = BtnRect(i);
        POINT p = { x, y };
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

//=============================================================================
// GDI icon drawing  (16x16 viewport at ox,oy)
//=============================================================================

static void L2(HDC h, int x1, int y1, int x2, int y2)
{
    MoveToEx(h, x1, y1, NULL); LineTo(h, x2, y2);
}

static void BTB_DrawIcon(HDC hdc, IconType icon, int ox, int oy, COLORREF clr)
{
    HPEN pen = CreatePen(PS_SOLID, 2, clr);  // thicker 2px pen for comic boldness
    HPEN op  = (HPEN)SelectObject(hdc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));

    switch (icon)
    {
    case ICON_NEW:
        L2(hdc, ox+3, oy+0, ox+10, oy+0);
        L2(hdc, ox+10,oy+0, ox+13, oy+3);
        L2(hdc, ox+13,oy+3, ox+13,oy+15);
        L2(hdc, ox+13,oy+15,ox+3, oy+15);
        L2(hdc, ox+3, oy+15,ox+3, oy+0);
        L2(hdc, ox+10,oy+0, ox+10,oy+3);
        L2(hdc, ox+10,oy+3, ox+13,oy+3);
        break;

    case ICON_OPEN:
        L2(hdc, ox+1, oy+4, ox+6, oy+4);
        L2(hdc, ox+6, oy+4, ox+7, oy+6);
        L2(hdc, ox+7, oy+6, ox+13,oy+6);
        L2(hdc, ox+1, oy+4, ox+1, oy+13);
        L2(hdc, ox+1, oy+13,ox+13,oy+13);
        L2(hdc, ox+13,oy+13,ox+14,oy+7);
        L2(hdc, ox+14,oy+7, ox+5, oy+7);
        L2(hdc, ox+5, oy+7, ox+1, oy+13);
        break;

    case ICON_SAVE:
        Rectangle(hdc, ox+2, oy+1, ox+14, oy+15);
        L2(hdc, ox+4, oy+1, ox+4, oy+6);
        L2(hdc, ox+4, oy+6, ox+11,oy+6);
        L2(hdc, ox+11,oy+6, ox+11,oy+1);
        Rectangle(hdc, ox+5, oy+9, ox+11,oy+14);
        break;

    case ICON_UNDO:
        L2(hdc, ox+2, oy+6, ox+6, oy+1);
        L2(hdc, ox+2, oy+6, ox+6, oy+10);
        L2(hdc, ox+2, oy+6, ox+10,oy+6);
        L2(hdc, ox+10,oy+6, ox+13,oy+9);
        L2(hdc, ox+13,oy+9, ox+13,oy+13);
        L2(hdc, ox+13,oy+13,ox+8, oy+13);
        break;

    case ICON_REDO:
        L2(hdc, ox+13,oy+6, ox+9, oy+1);
        L2(hdc, ox+13,oy+6, ox+9, oy+10);
        L2(hdc, ox+13,oy+6, ox+6, oy+6);
        L2(hdc, ox+6, oy+6, ox+2, oy+9);
        L2(hdc, ox+2, oy+9, ox+2, oy+13);
        L2(hdc, ox+2, oy+13,ox+8, oy+13);
        break;

    case ICON_CUT:
        L2(hdc, ox+5, oy+0, ox+10,oy+8);
        L2(hdc, ox+11,oy+0, ox+6, oy+8);
        Ellipse(hdc, ox+1, oy+9, ox+7, oy+15);
        Ellipse(hdc, ox+8, oy+9, ox+14,oy+15);
        break;

    case ICON_COPY:
        Rectangle(hdc, ox+0, oy+4, ox+10,oy+15);
        L2(hdc, ox+5, oy+0, ox+15,oy+0);
        L2(hdc, ox+15,oy+0, ox+15,oy+11);
        L2(hdc, ox+10,oy+11,ox+15,oy+11);
        L2(hdc, ox+5, oy+0, ox+5, oy+4);
        break;

    case ICON_PASTE:
        Rectangle(hdc, ox+2, oy+4, ox+13,oy+15);
        Rectangle(hdc, ox+5, oy+0, ox+10,oy+5);
        L2(hdc, ox+5, oy+8, ox+11,oy+8);
        L2(hdc, ox+5, oy+10,ox+11,oy+10);
        L2(hdc, ox+5, oy+12,ox+9, oy+12);
        break;

    case ICON_FIND:
        Ellipse(hdc, ox+1, oy+1, ox+11,oy+11);
        {
            HPEN tp = CreatePen(PS_SOLID, 2, clr);
            SelectObject(hdc, tp);
            L2(hdc, ox+10,oy+10,ox+14,oy+14);
            SelectObject(hdc, pen);
            DeleteObject(tp);
        }
        break;

    case ICON_REPLACE:
        Ellipse(hdc, ox+1, oy+1, ox+10,oy+10);
        L2(hdc, ox+9, oy+9, ox+11,oy+11);
        L2(hdc, ox+8, oy+12,ox+14,oy+12);
        L2(hdc, ox+12,oy+10,ox+14,oy+12);
        L2(hdc, ox+14,oy+12,ox+12,oy+14);
        break;

    case ICON_PRINT:
        Rectangle(hdc, ox+1, oy+5, ox+14,oy+12);
        Rectangle(hdc, ox+3, oy+0, ox+12,oy+6);
        Rectangle(hdc, ox+3, oy+10,ox+12,oy+15);
        break;

    case ICON_WRAP:
        L2(hdc, ox+1, oy+3, ox+13,oy+3);
        L2(hdc, ox+13,oy+3, ox+13,oy+8);
        L2(hdc, ox+13,oy+8, ox+5, oy+8);
        L2(hdc, ox+5, oy+8, ox+7, oy+6);
        L2(hdc, ox+5, oy+8, ox+7, oy+10);
        L2(hdc, ox+1, oy+13,ox+13,oy+13);
        break;

    case ICON_ZOOMIN:
        Ellipse(hdc, ox+1, oy+1, ox+11,oy+11);
        L2(hdc, ox+5, oy+4, ox+5, oy+9);
        L2(hdc, ox+3, oy+6, ox+8, oy+6);
        {
            HPEN tp2 = CreatePen(PS_SOLID, 2, clr);
            SelectObject(hdc, tp2);
            L2(hdc, ox+10,oy+10,ox+14,oy+14);
            SelectObject(hdc, pen);
            DeleteObject(tp2);
        }
        break;

    case ICON_ZOOMOUT:
        Ellipse(hdc, ox+1, oy+1, ox+11,oy+11);
        L2(hdc, ox+3, oy+6, ox+8, oy+6);
        {
            HPEN tp3 = CreatePen(PS_SOLID, 2, clr);
            SelectObject(hdc, tp3);
            L2(hdc, ox+10,oy+10,ox+14,oy+14);
            SelectObject(hdc, pen);
            DeleteObject(tp3);
        }
        break;

    case ICON_BIKO: {
        POINT pts[] = {
            {ox+1, oy+3}, {ox+3, oy+0}, {ox+12,oy+0}, {ox+14,oy+3},
            {ox+14,oy+9}, {ox+12,oy+11},{ox+8, oy+11},
            {ox+6, oy+14},{ox+6, oy+11},{ox+3, oy+11},
            {ox+1, oy+9}, {ox+1, oy+3}
        };
        Polyline(hdc, pts, 12);
        HBRUSH db = CreateSolidBrush(clr);
        RECT d1={ox+4, oy+5, ox+6, oy+7};
        RECT d2={ox+7, oy+5, ox+9, oy+7};
        RECT d3={ox+10,oy+5, ox+12,oy+7};
        FillRect(hdc, &d1, db);
        FillRect(hdc, &d2, db);
        FillRect(hdc, &d3, db);
        DeleteObject(db);
        break;
    }
    default: break;
    }

    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

//=============================================================================
// Paint — COMIC BOOK style
//=============================================================================

static void Paint(HWND hwnd, HDC hdc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;

    BOOL dk = DarkMode_IsEnabled();
    COLORREF cBg  = dk ? BTB_DK_BG       : BTB_LT_BG;
    COLORREF cHov = dk ? BTB_DK_HOVER    : BTB_LT_HOVER;
    COLORREF cPrs = dk ? BTB_DK_PRESS    : BTB_LT_PRESS;
    COLORREF cChk = dk ? BTB_DK_CHECK    : BTB_LT_CHECK;
    COLORREF cIco = dk ? BTB_DK_ICON     : BTB_LT_ICON;
    COLORREF cDis = dk ? BTB_DK_DISABLED : BTB_LT_DISABLED;
    COLORREF cAcc = dk ? BTB_DK_ACCENT   : BTB_LT_ACCENT;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm  = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP obm = (HBITMAP)SelectObject(mem, bm);

    // Ink-black background
    HBRUSH bg = CreateSolidBrush(cBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    // POW-yellow bottom rule — comic panel divider
    if (dk) {
        HPEN yPen = CreatePen(PS_SOLID, 2, RGB(255, 215, 0));
        HPEN oYP  = (HPEN)SelectObject(mem, yPen);
        MoveToEx(mem, 0, rc.bottom - 1, NULL);
        LineTo(mem, rc.right, rc.bottom - 1);
        SelectObject(mem, oYP);
        DeleteObject(yPen);
    }

    for (int i = 0; i < (int)BTN_COUNT; i++)
    {
        RECT br = BtnRect(i);

        if (s_btns[i].cmdId == 0) {
            // Dashed yellow separator
            int cx = (br.left + br.right) / 2;
            HPEN sp  = CreatePen(PS_DOT, 1, RGB(120, 110, 30));
            HPEN osp = (HPEN)SelectObject(mem, sp);
            MoveToEx(mem, cx, br.top  + 6, NULL);
            LineTo  (mem, cx, br.bottom - 6);
            SelectObject(mem, osp);
            DeleteObject(sp);
            continue;
        }

        BOOL bHot     = (i == s_hover);
        BOOL bPressed = (i == s_press);
        BOOL bChecked = s_btns[i].checked;

        COLORREF btnBg = cBg;
        if (bPressed)       btnBg = cPrs;
        else if (bHot)      btnBg = cHov;
        else if (bChecked)  btnBg = cChk;

        if (btnBg != cBg) {
            // Fill button with bold square shape
            HBRUSH hb = CreateSolidBrush(btnBg);
            // 2px comic-ink outline
            HPEN bp = CreatePen(PS_SOLID, 2, RGB(10, 10, 10));
            HBRUSH xb = (HBRUSH)SelectObject(mem, hb);
            HPEN   xp = (HPEN)SelectObject(mem, bp);
            Rectangle(mem, br.left + 2, br.top + 1, br.right - 2, br.bottom - 1);
            SelectObject(mem, xb);
            SelectObject(mem, xp);
            DeleteObject(hb);
            DeleteObject(bp);
        }

        // Icon color: yellow bg with black icon (HOT), white on idle, white on red (PRESS)
        COLORREF fg;
        if (bPressed)                fg = RGB(255, 255, 255);
        else if (bHot)               fg = RGB(10, 10, 10);
        else if (!s_btns[i].enabled) fg = cDis;
        else if (s_btns[i].icon == ICON_BIKO && s_btns[i].enabled)
            fg = cAcc;   // Biko AI always glows yellow
        else
            fg = cIco;

        int ix = (br.left + br.right)  / 2 - BTB_ICON_SZ / 2;
        int iy = (br.top  + br.bottom) / 2 - BTB_ICON_SZ / 2;
        BTB_DrawIcon(mem, s_btns[i].icon, ix, iy, fg);
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, obm);
    DeleteObject(bm);
    DeleteDC(mem);
}

//=============================================================================
// Tooltips
//=============================================================================

static void MakeTooltips(HWND hp, HINSTANCE hi)
{
    s_hwndTip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hp, NULL, hi, NULL);
    if (!s_hwndTip) return;

    for (int i = 0; i < (int)BTN_COUNT; i++) {
        if (s_btns[i].cmdId == 0) continue;
        RECT r = BtnRect(i);
        TOOLINFOW ti;
        ZeroMemory(&ti, sizeof(ti));
        ti.cbSize   = sizeof(TOOLINFOW);
        ti.uFlags   = TTF_SUBCLASS;
        ti.hwnd     = hp;
        ti.uId      = (UINT_PTR)i;
        ti.rect     = r;
        ti.lpszText = (LPWSTR)s_btns[i].tooltip;
        SendMessageW(s_hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

//=============================================================================
// WndProc
//=============================================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        int idx = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (!s_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            s_tracking = TRUE;
        }
        if (idx != s_hover) { s_hover = idx; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        s_tracking = FALSE;
        s_hover = s_press = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int idx = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (idx >= 0 && s_btns[idx].enabled) {
            s_press = idx;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (s_press >= 0) {
            int idx = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            int cmd = (idx == s_press && s_btns[idx].enabled) ? s_btns[idx].cmdId : 0;
            s_press = -1;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            if (cmd)
                SendMessage(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(cmd, 1), 0);
        }
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

//=============================================================================
// Public API
//=============================================================================

HWND BikoToolbar_Create(HWND hwndParent, HINSTANCE hInst)
{
    if (!s_registered) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"BikoToolbar";
        RegisterClassExW(&wc);
        s_registered = TRUE;
    }

    s_hwnd = CreateWindowExW(
        0, L"BikoToolbar", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 800, BTB_HEIGHT,
        hwndParent, (HMENU)(UINT_PTR)0xFB10, hInst, NULL);

    if (s_hwnd)
        MakeTooltips(s_hwnd, hInst);

    return s_hwnd;
}

void BikoToolbar_Destroy(void)
{
    if (s_hwndTip) { DestroyWindow(s_hwndTip); s_hwndTip = NULL; }
    if (s_hwnd)    { DestroyWindow(s_hwnd);    s_hwnd    = NULL; }
}

int BikoToolbar_GetHeight(void)
{
    return s_visible ? BTB_HEIGHT : 0;
}

void BikoToolbar_EnableButton(int cmdId, BOOL bEnable)
{
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        if (s_btns[i].cmdId == cmdId && s_btns[i].enabled != bEnable) {
            s_btns[i].enabled = bEnable;
            if (s_hwnd) InvalidateRect(s_hwnd, NULL, FALSE);
            return;
        }
    }
}

void BikoToolbar_CheckButton(int cmdId, BOOL bCheck)
{
    for (int i = 0; i < (int)BTN_COUNT; i++) {
        if (s_btns[i].cmdId == cmdId && s_btns[i].checked != bCheck) {
            s_btns[i].checked = bCheck;
            if (s_hwnd) InvalidateRect(s_hwnd, NULL, FALSE);
            return;
        }
    }
}

void BikoToolbar_ApplyDarkMode(void)
{
    if (s_hwnd) InvalidateRect(s_hwnd, NULL, TRUE);
}

HWND BikoToolbar_GetHwnd(void)
{
    return s_hwnd;
}

void BikoToolbar_Show(BOOL bShow)
{
    s_visible = bShow;
    if (s_hwnd) ShowWindow(s_hwnd, bShow ? SW_SHOW : SW_HIDE);
}

BOOL BikoToolbar_IsVisible(void)
{
    return s_visible;
}
