
/******************************************************************************
*
* Biko
*
* WelcomeScreen.c
*   Premium welcome screen with logo, tagline, and action buttons.
*   Fully owner-drawn for pixel-perfect rendering in dark and light modes.
*
******************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "WelcomeScreen.h"
#include "ui/theme/BikodeTheme.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include "Terminal.h"
#include "ChatPanel.h"
#include "resource.h"

// External references
extern HWND hwndMain;

//=============================================================================
// Color palette
//=============================================================================

// Dark mode
#define DK_BG           RGB(24, 24, 24)
#define DK_SURFACE      RGB(36, 36, 36)
#define DK_SURFACE_HOV  RGB(48, 50, 54)
#define DK_BORDER       RGB(55, 55, 55)
#define DK_BORDER_HOV   RGB(80, 120, 200)
#define DK_TEXT1         RGB(230, 230, 230)
#define DK_TEXT2         RGB(150, 150, 150)
#define DK_MUTED        RGB(80, 80, 80)
#define DK_ACCENT       RGB(75, 139, 245)
#define DK_ACCENT_HOV   RGB(100, 160, 255)
#define DK_BADGE_BG     RGB(42, 42, 42)
#define DK_BADGE_BD     RGB(60, 60, 60)
#define DK_DIVIDER      RGB(50, 50, 50)

// Light mode
#define LT_BG           RGB(248, 244, 236)
#define LT_SURFACE      RGB(240, 234, 223)
#define LT_SURFACE_HOV  RGB(233, 227, 215)
#define LT_BORDER       RGB(201, 191, 176)
#define LT_BORDER_HOV   RGB(70, 120, 210)
#define LT_TEXT1        RGB(54, 46, 36)
#define LT_TEXT2        RGB(118, 104, 89)
#define LT_MUTED        RGB(159, 148, 135)
#define LT_ACCENT       RGB(50, 110, 215)
#define LT_ACCENT_HOV   RGB(30, 90, 195)
#define LT_BADGE_BG     RGB(233, 227, 215)
#define LT_BADGE_BD     RGB(201, 191, 176)
#define LT_DIVIDER      RGB(214, 205, 191)

//=============================================================================
// Button definitions
//=============================================================================

#define IDC_BTN_NEW         2001
#define IDC_BTN_OPEN        2002
#define IDC_BTN_TERMINAL    2003
#define IDC_BTN_CHAT        2004
#define NUM_BUTTONS         4

typedef struct {
    int         id;
    const WCHAR* label;
    const WCHAR* description;
    const WCHAR* shortcut;
    RECT        rcBtn;
    BOOL        bHover;
} WelcomeButton;

static WelcomeButton s_buttons[NUM_BUTTONS] = {
    { IDC_BTN_NEW,      L"New File",           L"Create a new empty document",       L"Ctrl+N"  },
    { IDC_BTN_OPEN,     L"Open File",          L"Open an existing file from disk",   L"Ctrl+O"  },
    { IDC_BTN_TERMINAL, L"Terminal",            L"Open the integrated terminal",      L"Ctrl+`"  },
    { IDC_BTN_CHAT,     L"Bikode AI",          L"Open the AI-first coding assistant",L""        },
};

#define WELCOME_HERO_TEXT       L"I write what I like. Build, debug, and ship inside an AI-first IDE with a native comic-noir shell."
#define WELCOME_HERO_TOP        170
#define WELCOME_HERO_MIN_HEIGHT 48
#define WELCOME_HERO_PAD_X      18
#define WELCOME_HERO_PAD_Y      10

//=============================================================================
// State
//=============================================================================

static HWND     s_hwndWelcome = NULL;
static HICON    s_hLogo = NULL;
static HFONT    s_hFontTitle = NULL;
static HFONT    s_hFontWordmark = NULL;
static HFONT    s_hFontTagline = NULL;
static HFONT    s_hFontBtnLabel = NULL;
static HFONT    s_hFontBtnDesc = NULL;
static HFONT    s_hFontShortcut = NULL;
static HFONT    s_hFontFooter = NULL;
static int      s_iHoverBtn = -1;
static BOOL     s_bClassRegistered = FALSE;

//=============================================================================
// Color helpers
//=============================================================================

#define C(dk, lt) (DarkMode_IsEnabled() ? (dk) : (lt))

//=============================================================================
// Fonts
//=============================================================================

static void EnsureFonts(void)
{
    if (s_hFontTitle) return;
    s_hFontTitle = BikodeTheme_GetFont(BKFONT_DISPLAY);
    s_hFontWordmark = CreateFontW(-72, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH | FF_ROMAN, L"Georgia");
    if (!s_hFontWordmark)
        s_hFontWordmark = BikodeTheme_GetFont(BKFONT_DISPLAY);
    s_hFontTagline = BikodeTheme_GetFont(BKFONT_MONO);
    s_hFontBtnLabel = BikodeTheme_GetFont(BKFONT_UI_BOLD);
    s_hFontBtnDesc = BikodeTheme_GetFont(BKFONT_UI_SMALL);
    s_hFontShortcut = BikodeTheme_GetFont(BKFONT_MONO_SMALL);
    s_hFontFooter = BikodeTheme_GetFont(BKFONT_UI_SMALL);
}

static void DestroyFonts(void)
{
    s_hFontTitle = NULL;
    if (s_hFontWordmark && s_hFontWordmark != BikodeTheme_GetFont(BKFONT_DISPLAY))
        DeleteObject(s_hFontWordmark);
    s_hFontWordmark = NULL;
    s_hFontTagline = NULL;
    s_hFontBtnLabel = NULL;
    s_hFontBtnDesc = NULL;
    s_hFontShortcut = NULL;
    s_hFontFooter = NULL;
}

static int MeasureHeroCardHeight(int cardWidth)
{
    RECT rcText;
    HWND hwndDC = s_hwndWelcome ? s_hwndWelcome : hwndMain;
    HDC hdc = GetDC(hwndDC);
    HFONT hOld = NULL;
    int textWidth;
    int textHeight;

    EnsureFonts();
    textWidth = max(140, cardWidth - (WELCOME_HERO_PAD_X * 2));
    rcText.left = 0;
    rcText.top = 0;
    rcText.right = textWidth;
    rcText.bottom = 0;

    if (hdc) {
        hOld = (HFONT)SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_BOLD));
        DrawTextW(hdc, WELCOME_HERO_TEXT, -1, &rcText,
                  DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        if (hOld)
            SelectObject(hdc, hOld);
        ReleaseDC(hwndDC, hdc);
    } else {
        rcText.bottom = 28;
    }

    textHeight = rcText.bottom - rcText.top;
    return max(WELCOME_HERO_MIN_HEIGHT, textHeight + (WELCOME_HERO_PAD_Y * 2));
}

//=============================================================================
// Drawing helpers
//=============================================================================

static void FillRoundRect(HDC hdc, const RECT* rc, int r, COLORREF fill, COLORREF border)
{
    HBRUSH hBr = CreateSolidBrush(fill);
    HPEN   hPn = CreatePen(PS_SOLID, 1, border);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hBr);
    HPEN   hOldPn = (HPEN)SelectObject(hdc, hPn);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, r, r);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPn);
    DeleteObject(hBr);
    DeleteObject(hPn);
}

static void DrawWebsiteWordmark(HDC hdc, int centerX, int top)
{
    SIZE szBi = { 0 };
    SIZE szKo = { 0 };
    SIZE szDe = { 0 };
    HFONT hOld;
    int x;
    int iconSize = 46;
    int iconGap = 12;
    int wordmarkW;
    int totalW;

    EnsureFonts();
    hOld = (HFONT)SelectObject(hdc, s_hFontWordmark);
    GetTextExtentPoint32W(hdc, L"Bi", 2, &szBi);
    GetTextExtentPoint32W(hdc, L"ko", 2, &szKo);
    GetTextExtentPoint32W(hdc, L"de.", 3, &szDe);

    wordmarkW = szBi.cx + szKo.cx + szDe.cx;
    totalW = wordmarkW + ((s_hLogo != NULL) ? (iconSize + iconGap) : 0);
    x = centerX - (totalW / 2);
    SetBkMode(hdc, TRANSPARENT);

    if (s_hLogo) {
        DrawIconEx(hdc, x, top + 8, s_hLogo, iconSize, iconSize, 0, NULL, DI_NORMAL);
        x += iconSize + iconGap;
    }

    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    TextOutW(hdc, x, top, L"Bi", 2);

    x += szBi.cx;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));
    TextOutW(hdc, x, top, L"ko", 2);

    x += szKo.cx;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    TextOutW(hdc, x, top, L"de.", 3);

    SelectObject(hdc, hOld);
}

static void DrawButtonCard(HDC hdc, WelcomeButton* btn)
{
    BOOL h = btn->bHover;
    COLORREF fill = h
        ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_SURFACE_ELEVATED), 24)
        : BikodeTheme_GetColor(BKCLR_SURFACE_RAISED);
    COLORREF accent = (btn->id == IDC_BTN_CHAT)
        ? BikodeTheme_GetColor(BKCLR_HOT_MAGENTA)
        : BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);

    BikodeTheme_DrawRoundedPanel(hdc, &btn->rcBtn, fill,
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        h ? BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        12, FALSE);

    RECT rcStripe = { btn->rcBtn.left + 1, btn->rcBtn.top + 10,
                      btn->rcBtn.left + 6, btn->rcBtn.bottom - 10 };
    HBRUSH hAcc = CreateSolidBrush(accent);
    FillRect(hdc, &rcStripe, hAcc);
    DeleteObject(hAcc);

    int pad = 22;

    // Label
    HFONT hOld = (HFONT)SelectObject(hdc, s_hFontBtnLabel);
    SetTextColor(hdc, h ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY) : BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    RECT rcL = { btn->rcBtn.left + pad, btn->rcBtn.top + 10,
                 btn->rcBtn.right - 90, btn->rcBtn.top + 28 };
    DrawTextW(hdc, btn->label, -1, &rcL, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Description
    SelectObject(hdc, s_hFontBtnDesc);
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    RECT rcD = { btn->rcBtn.left + pad, btn->rcBtn.top + 30,
                 btn->rcBtn.right - 90, btn->rcBtn.bottom - 6 };
    DrawTextW(hdc, btn->description, -1, &rcD, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    // Shortcut badge
    if (btn->shortcut[0])
    {
        SelectObject(hdc, s_hFontShortcut);
        SIZE sz;
        GetTextExtentPoint32W(hdc, btn->shortcut, (int)wcslen(btn->shortcut), &sz);
        int bw = sz.cx + 14;
        int bh = sz.cy + 6;
        int bx = btn->rcBtn.right - bw - 14;
        int by = btn->rcBtn.top + (btn->rcBtn.bottom - btn->rcBtn.top - bh) / 2;
        RECT rcBadge = { bx, by, bx + bw, by + bh };
        BikodeTheme_DrawChip(hdc, &rcBadge, btn->shortcut,
            BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            BikodeTheme_GetColor(BKCLR_TEXT_MUTED),
            s_hFontShortcut, FALSE, 0);
    }

    SelectObject(hdc, hOld);
}

//=============================================================================
// Layout
//=============================================================================

static void CalcLayout(int cx, int cy)
{
    int btnW = 430;
    int btnH = 62;
    int btnGap = 10;
    int heroH;
    int logoH;
    if (btnW > cx - 60) btnW = cx - 60;

    heroH = MeasureHeroCardHeight(btnW);
    logoH = WELCOME_HERO_TOP + heroH + 10;
    int cardsH = NUM_BUTTONS * (btnH + btnGap) - btnGap;
    int gapBetween = 28;
    int totalH = logoH + gapBetween + cardsH;

    int startY = (cy - totalH) / 2;
    if (startY < 30) startY = 30;

    int cardsTop = startY + logoH + gapBetween;
    int bx = (cx - btnW) / 2;

    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        s_buttons[i].rcBtn.left   = bx;
        s_buttons[i].rcBtn.top    = cardsTop + i * (btnH + btnGap);
        s_buttons[i].rcBtn.right  = bx + btnW;
        s_buttons[i].rcBtn.bottom = s_buttons[i].rcBtn.top + btnH;
    }
}

//=============================================================================
// Window procedure
//=============================================================================

static LRESULT CALLBACK WelcomeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cx = rc.right, cy = rc.bottom;

        // Double-buffer
        HDC hm = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc, cx, cy);
        HBITMAP hOld = (HBITMAP)SelectObject(hm, hBmp);

        EnsureFonts();

        BikodeTheme_FillHalftone(hm, &rc, BikodeTheme_GetColor(BKCLR_APP_BG));

        SetBkMode(hm, TRANSPARENT);

        // Layout metrics
        int btnW = 430;
        int heroH;
        int logoH;
        int heroW;
        if (btnW > cx - 60) btnW = cx - 60;
        heroW = btnW;
        heroH = MeasureHeroCardHeight(heroW);
        logoH = WELCOME_HERO_TOP + heroH + 10;
        int cardsH = NUM_BUTTONS * (62 + 10) - 10;
        int totalH = logoH + 34 + cardsH;
        int startY = (cy - totalH) / 2;
        if (startY < 30) startY = 30;
        int centerX = cx / 2;

        // Chapter chip
        {
            RECT rcChip = { centerX - 120, startY + 4, centerX + 120, startY + 30 };
            BikodeTheme_DrawChip(hm, &rcChip, L"ISSUE 01  BIKODE CONTROL ROOM",
                BikodeTheme_GetColor(BKCLR_SURFACE_ELEVATED),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
                BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE, BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
        }

        // --- Title ---
        DrawWebsiteWordmark(hm, centerX, startY + 52);

        // --- Tagline ---
        SelectObject(hm, s_hFontTagline);
        SetTextColor(hm, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
        RECT rcQ = { centerX - 270, startY + 124, centerX + 270, startY + 156 };
        DrawTextW(hm, L"AN AI IDE THAT CODES WHAT IT LIKES", -1, &rcQ,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        // --- Hero card ---
        {
            RECT rcHero = {
                centerX - (heroW / 2),
                startY + WELCOME_HERO_TOP,
                centerX + (heroW / 2),
                startY + WELCOME_HERO_TOP + heroH
            };
            RECT rcHeroText = rcHero;
            rcHeroText.left += WELCOME_HERO_PAD_X;
            rcHeroText.right -= WELCOME_HERO_PAD_X;
            rcHeroText.top += WELCOME_HERO_PAD_Y;
            rcHeroText.bottom -= WELCOME_HERO_PAD_Y;
            BikodeTheme_DrawCutCornerPanel(hm, &rcHero,
                BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
                BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                12, TRUE);
            SelectObject(hm, BikodeTheme_GetFont(BKFONT_UI_BOLD));
            SetTextColor(hm, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
            DrawTextW(hm, WELCOME_HERO_TEXT,
                -1, &rcHeroText, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
        }

        // --- Button cards ---
        for (int i = 0; i < NUM_BUTTONS; i++)
            DrawButtonCard(hm, &s_buttons[i]);

        // --- Footer ---
        SelectObject(hm, s_hFontFooter);
        SetTextColor(hm, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
        RECT rcF = { 0, cy - 32, cx, cy - 12 };
        DrawTextW(hm, L"Bikode  •  AI-first IDE  •  bikode.co.za",
                  -1, &rcF, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        {
            RECT rcFooter = { centerX - 190, cy - 42, centerX + 190, cy - 8 };
            if (rcFooter.left < 16) rcFooter.left = 16;
            if (rcFooter.right > cx - 16) rcFooter.right = cx - 16;
            BikodeTheme_DrawChip(hm, &rcFooter, L"BIKODE | AI-FIRST IDE | bikode.co.za",
                BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
                BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
                BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));
        }

        // Blit
        BitBlt(hdc, 0, 0, cx, cy, hm, 0, 0, SRCCOPY);
        SelectObject(hm, hOld);
        DeleteObject(hBmp);
        DeleteDC(hm);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
    {
        CalcLayout(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int hit = -1;
        for (int i = 0; i < NUM_BUTTONS; i++)
        {
            if (PtInRect(&s_buttons[i].rcBtn, pt)) { hit = i; break; }
        }

        if (hit != s_iHoverBtn)
        {
            if (s_iHoverBtn >= 0)
            {
                s_buttons[s_iHoverBtn].bHover = FALSE;
                InvalidateRect(hwnd, &s_buttons[s_iHoverBtn].rcBtn, FALSE);
            }
            s_iHoverBtn = hit;
            if (s_iHoverBtn >= 0)
            {
                s_buttons[s_iHoverBtn].bHover = TRUE;
                InvalidateRect(hwnd, &s_buttons[s_iHoverBtn].rcBtn, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (s_iHoverBtn >= 0)
        {
            s_buttons[s_iHoverBtn].bHover = FALSE;
            InvalidateRect(hwnd, &s_buttons[s_iHoverBtn].rcBtn, FALSE);
            s_iHoverBtn = -1;
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(LoadCursor(NULL, s_iHoverBtn >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        for (int i = 0; i < NUM_BUTTONS; i++)
        {
            if (!PtInRect(&s_buttons[i].rcBtn, pt)) continue;
            switch (s_buttons[i].id)
            {
            case IDC_BTN_NEW:
                WelcomeScreen_Hide();
                PostMessage(hwndMain, WM_COMMAND, IDM_FILE_NEW, 0);
                break;
            case IDC_BTN_OPEN:
                WelcomeScreen_Hide();
                PostMessage(hwndMain, WM_COMMAND, IDM_FILE_OPEN, 0);
                break;
            case IDC_BTN_TERMINAL:
                WelcomeScreen_Hide();
                Terminal_Toggle(hwndMain);
                break;
            case IDC_BTN_CHAT:
                WelcomeScreen_Hide();
                ChatPanel_Show(hwndMain);
                break;
            }
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        DestroyFonts();
        if (s_hLogo) { DestroyIcon(s_hLogo); s_hLogo = NULL; }
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

//=============================================================================
// Public API
//=============================================================================

BOOL WelcomeScreen_Init(HWND hwndParent)
{
    (void)hwndParent;
    if (s_bClassRegistered) return TRUE;

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WelcomeWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"BikoWelcomeScreen";
    RegisterClassExW(&wc);
    s_bClassRegistered = TRUE;
    return TRUE;
}

void WelcomeScreen_Show(HWND hwndParent)
{
    if (s_hwndWelcome)
    {
        ShowWindow(s_hwndWelcome, SW_SHOW);
        SetWindowPos(s_hwndWelcome, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        InvalidateRect(s_hwndWelcome, NULL, FALSE);
        return;
    }

    // Load logo icon (white for dark mode, black for light mode)
    HINSTANCE hInst = GetModuleHandle(NULL);
    int logoRes = DarkMode_IsEnabled() ? IDR_MAINWND : IDI_BIKO_LIGHT;
    s_hLogo = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(logoRes),
                                IMAGE_ICON, 128, 128, LR_DEFAULTCOLOR);

    RECT rc;
    GetClientRect(hwndParent, &rc);

    s_hwndWelcome = CreateWindowExW(
        0, L"BikoWelcomeScreen", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, rc.right, rc.bottom,
        hwndParent, NULL, hInst, NULL);

    if (s_hwndWelcome)
    {
        CalcLayout(rc.right, rc.bottom);
        SetWindowPos(s_hwndWelcome, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

void WelcomeScreen_Hide(void)
{
    if (s_hwndWelcome)
        ShowWindow(s_hwndWelcome, SW_HIDE);
}

BOOL WelcomeScreen_IsVisible(void)
{
    return (s_hwndWelcome && IsWindowVisible(s_hwndWelcome));
}

void WelcomeScreen_OnResize(HWND hwndParent, int cx, int cy)
{
    (void)hwndParent;
    if (s_hwndWelcome && IsWindowVisible(s_hwndWelcome))
        SetWindowPos(s_hwndWelcome, NULL, 0, 0, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
}

void WelcomeScreen_Shutdown(void)
{
    if (s_hwndWelcome)
    {
        DestroyWindow(s_hwndWelcome);
        s_hwndWelcome = NULL;
    }
    DestroyFonts();
    if (s_hLogo) { DestroyIcon(s_hLogo); s_hLogo = NULL; }
}
