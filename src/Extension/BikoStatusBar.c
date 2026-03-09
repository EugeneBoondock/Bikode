/******************************************************************************
*
* Biko
*
* BikoStatusBar.c
*   Custom owner-drawn status bar with full dark mode support.
*   Renders status panes as a single owner-drawn child window.
*
******************************************************************************/

#include "BikoStatusBar.h"
#include "ui/theme/BikodeTheme.h"
#include "DarkMode.h"
#include <windowsx.h>
#include <commctrl.h>

//=============================================================================
// Design constants
//=============================================================================

#define BSB_HEIGHT          24
#define BSB_PAD_X           8
#define BSB_MAX_PARTS       8
#define BSB_MAX_TEXT       256

//=============================================================================
// State
//=============================================================================

static HWND       s_hwnd       = NULL;
static HWND       s_hwndParent = NULL;
static HFONT      s_hFont      = NULL;
static BOOL       s_visible    = TRUE;
static BOOL       s_registered = FALSE;
static BOOL       s_tracking   = FALSE;
static int        s_hover      = -1;   // hovered pane index

// Parts
static int        s_nParts     = 0;
static int        s_widths[BSB_MAX_PARTS];  // right-edge of each part
static WCHAR      s_text[BSB_MAX_PARTS][BSB_MAX_TEXT];

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

//=============================================================================
// Font
//=============================================================================

static void CreateStatusFont(void)
{
  s_hFont = BikodeTheme_GetFont(BKFONT_MONO_SMALL);
}

//=============================================================================
// Geometry
//=============================================================================

static RECT PaneRect(int i, const RECT *pClient)
{
  RECT r;
  r.top    = 0;
  r.bottom = pClient->bottom;
  r.left   = (i == 0) ? 0 : s_widths[i - 1];
  r.right  = (s_widths[i] == -1) ? pClient->right : s_widths[i];
  return r;
}

static int HitTest(int x, int y)
{
  RECT rc;
  GetClientRect(s_hwnd, &rc);
  if (y < 0 || y >= rc.bottom) return -1;
  for (int i = 0; i < s_nParts; i++) {
    RECT pr = PaneRect(i, &rc);
    if (x >= pr.left && x < pr.right)
      return i;
  }
  return -1;
}

//=============================================================================
// Painting
//=============================================================================

static void Paint(HWND hwnd, HDC hdc)
{
  RECT rc;
  GetClientRect(hwnd, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) return;

  // Double-buffer
  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
  HBITMAP obm = (HBITMAP)SelectObject(mem, bm);

  BikodeTheme_DrawRoundedPanel(mem, &rc,
      BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
      BikodeTheme_GetColor(BKCLR_STROKE_DARK),
      BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
      0, TRUE);

  HFONT of = (HFONT)SelectObject(mem, s_hFont);
  SetBkMode(mem, TRANSPARENT);

  for (int i = 0; i < s_nParts; i++) {
    RECT pr = PaneRect(i, &rc);
    InflateRect(&pr, -2, -3);
    BikodeTheme_DrawRoundedPanel(mem, &pr,
        (i == s_hover)
            ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 26)
            : BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        (i == s_hover) ? BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        8, FALSE);

    if (s_text[i][0]) {
      RECT tr = pr;
      tr.left  += BSB_PAD_X;
      tr.right -= BSB_PAD_X;
      SetTextColor(mem,
          (i == s_nParts - 1) ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY) : BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
      DrawTextW(mem, s_text[i], -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
  }

  SelectObject(mem, of);

  BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
  SelectObject(mem, obm);
  DeleteObject(bm);
  DeleteDC(mem);
}

//=============================================================================
// WndProc
//=============================================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam)
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
    if (idx != s_hover) {
      s_hover = idx;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
  }

  case WM_MOUSELEAVE:
    s_tracking = FALSE;
    s_hover = -1;
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;

  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK: {
    int idx = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    if (idx >= 0) {
      // Forward as NM_CLICK / NM_DBLCLK to parent via WM_COMMAND
      // Use the same NMMOUSE structure the real status bar would send
      NMMOUSE nmm;
      ZeroMemory(&nmm, sizeof(nmm));
      nmm.hdr.hwndFrom = hwnd;
      nmm.hdr.idFrom   = GetDlgCtrlID(hwnd);
      nmm.hdr.code     = (msg == WM_LBUTTONDBLCLK) ? NM_DBLCLK : NM_CLICK;
      nmm.dwItemSpec    = (DWORD_PTR)idx;
      nmm.pt.x          = GET_X_LPARAM(lParam);
      nmm.pt.y          = GET_Y_LPARAM(lParam);
      SendMessage(s_hwndParent, WM_NOTIFY, nmm.hdr.idFrom, (LPARAM)&nmm);
    }
    return 0;
  }

  case WM_SIZE:
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;

  case WM_DPICHANGED:
    CreateStatusFont();
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

//=============================================================================
// Public API
//=============================================================================

HWND BikoStatusBar_Create(HWND hwndParent, HINSTANCE hInst)
{
  s_hwndParent = hwndParent;
  s_nParts = 0;
  for (int i = 0; i < BSB_MAX_PARTS; i++)
    s_text[i][0] = 0;

  CreateStatusFont();

  if (!s_registered) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"BikoStatusBar";
    RegisterClassExW(&wc);
    s_registered = TRUE;
  }

  s_hwnd = CreateWindowExW(
      0, L"BikoStatusBar", L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
      0, 0, 800, BSB_HEIGHT,
      hwndParent, (HMENU)(UINT_PTR)0xFB30, hInst, NULL);

  return s_hwnd;
}

void BikoStatusBar_Destroy(void)
{
  s_hFont = NULL;
  if (s_hwnd) {
    DestroyWindow(s_hwnd);
    s_hwnd = NULL;
  }
}

int BikoStatusBar_GetHeight(void)
{
  return (s_visible && s_hwnd) ? BSB_HEIGHT : 0;
}

void BikoStatusBar_Show(BOOL bShow)
{
  s_visible = bShow;
  if (s_hwnd) ShowWindow(s_hwnd, bShow ? SW_SHOW : SW_HIDE);
}

BOOL BikoStatusBar_IsVisible(void)
{
  return s_visible;
}

HWND BikoStatusBar_GetHwnd(void)
{
  return s_hwnd;
}

void BikoStatusBar_ApplyDarkMode(void)
{
  if (s_hwnd) InvalidateRect(s_hwnd, NULL, TRUE);
}

void BikoStatusBar_SetText(int iPart, LPCWSTR szText)
{
  if (iPart < 0 || iPart >= BSB_MAX_PARTS) return;
  if (szText)
    wcsncpy_s(s_text[iPart], BSB_MAX_TEXT, szText, _TRUNCATE);
  else
    s_text[iPart][0] = 0;
  if (s_hwnd) InvalidateRect(s_hwnd, NULL, FALSE);
}

void BikoStatusBar_SetParts(int nParts, const int *pWidths)
{
  if (nParts > BSB_MAX_PARTS) nParts = BSB_MAX_PARTS;
  s_nParts = nParts;
  for (int i = 0; i < nParts; i++)
    s_widths[i] = pWidths[i];
  if (s_hwnd) InvalidateRect(s_hwnd, NULL, FALSE);
}

int BikoStatusBar_CalcPaneWidth(LPCWSTR szText)
{
  if (!szText || !szText[0]) return 0;
  HDC hdc = GetDC(s_hwnd ? s_hwnd : s_hwndParent);
  HFONT of = (HFONT)SelectObject(hdc, s_hFont);
  SIZE sz;
  GetTextExtentPoint32W(hdc, szText, (int)wcslen(szText), &sz);
  SelectObject(hdc, of);
  ReleaseDC(s_hwnd ? s_hwnd : s_hwndParent, hdc);
  return sz.cx + BSB_PAD_X * 2 + 2;
}

int BikoStatusBar_HitTest(int xScreen, int yScreen)
{
  if (!s_hwnd) return -1;
  POINT pt = { xScreen, yScreen };
  ScreenToClient(s_hwnd, &pt);
  return HitTest(pt.x, pt.y);
}
