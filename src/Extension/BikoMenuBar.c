/******************************************************************************
*
* Biko
*
* BikoMenuBar.c
*   Custom owner-drawn menu bar with full dark mode support.
*   Renders top-level menu items as an owner-drawn child window, opens
*   standard Win32 popup menus via TrackPopupMenuEx.
*
******************************************************************************/

#include "BikoMenuBar.h"
#include "ui/theme/BikodeTheme.h"
#include "AICommands.h"
#include "GitUI.h"
#include "FileManager.h"
#include "DarkMode.h"
#include <windowsx.h>
#include <commctrl.h>
#include <string.h>

//=============================================================================
// Design constants
//=============================================================================

#define BMB_HEIGHT          36
#define BMB_PAD_X           10
#define BMB_MENU_START_X    10

// Maximum number of top-level menu items we support
#define BMB_MAX_ITEMS       16

//=============================================================================
// Menu item metadata
//=============================================================================

typedef struct {
  WCHAR   szLabel[64];    // display text (with & mnemonic stripped for width calc)
  WCHAR   szRaw[64];      // raw text (with & for mnemonic underline)
  HMENU   hPopup;         // popup submenu handle
  int     x;              // left edge in client coords
  int     width;          // total width (text + padding)
  WCHAR   chMnemonic;     // mnemonic character (lowercase), 0 if none
} MBItem;

//=============================================================================
// State
//=============================================================================

static HWND       s_hwnd       = NULL;
static HWND       s_hwndParent = NULL;
static HMENU      s_hMenu      = NULL;
static HINSTANCE  s_hInst      = NULL;
static HFONT      s_hFont      = NULL;
static BOOL       s_visible    = TRUE;
static BOOL       s_registered = FALSE;

static MBItem     s_items[BMB_MAX_ITEMS];
static int        s_itemCount  = 0;

static int        s_hover      = -1;   // mouse hover index
static int        s_open       = -1;   // currently open popup index
static BOOL       s_tracking   = FALSE;
static BOOL       s_altMode    = FALSE; // keyboard alt-navigation active
static int        s_altFocus   = -1;    // keyboard-focused item in alt mode
static BOOL       s_popupLoop  = FALSE; // TRUE while a popup is open
static RECT       s_rcCommandBox = { 0 };

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

//=============================================================================
// Font
//=============================================================================

static void CreateMenuFont(void)
{
  if (s_hFont) DeleteObject(s_hFont);

  NONCLIENTMETRICSW ncm;
  ncm.cbSize = sizeof(ncm);
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);

  ncm.lfMenuFont.lfWeight = FW_SEMIBOLD;
  ncm.lfMenuFont.lfHeight = -13;
  s_hFont = CreateFontIndirectW(&ncm.lfMenuFont);
  if (!s_hFont)
    s_hFont = BikodeTheme_GetFont(BKFONT_UI_BOLD);
}

//=============================================================================
// Build item list from HMENU
//=============================================================================

static void BuildItems(void)
{
  s_itemCount = 0;
  if (!s_hMenu) return;

  int count = GetMenuItemCount(s_hMenu);
  if (count > BMB_MAX_ITEMS) count = BMB_MAX_ITEMS;

  // Measure widths
  HDC hdc = GetDC(s_hwnd ? s_hwnd : s_hwndParent);
  HFONT of = (HFONT)SelectObject(hdc, s_hFont);

  int x = BMB_MENU_START_X;

  for (int i = 0; i < count; i++) {
    MENUITEMINFOW mii;
    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING | MIIM_SUBMENU;
    mii.dwTypeData = s_items[i].szRaw;
    mii.cch = 63;
    if (!GetMenuItemInfoW(s_hMenu, i, TRUE, &mii))
      continue;

    s_items[i].hPopup = mii.hSubMenu;

    // Copy raw => label, stripping '&' and extracting mnemonic
    s_items[i].chMnemonic = 0;
    int d = 0;
    for (int s = 0; s_items[i].szRaw[s]; s++) {
      if (s_items[i].szRaw[s] == L'&') {
        if (s_items[i].szRaw[s + 1]) {
          s_items[i].chMnemonic = (WCHAR)CharLowerW(
              (LPWSTR)(UINT_PTR)(WCHAR)s_items[i].szRaw[s + 1]);
        }
        continue; // skip the '&'
      }
      s_items[i].szLabel[d++] = s_items[i].szRaw[s];
    }
    s_items[i].szLabel[d] = 0;
    if (d == 0 && s_items[i].hPopup) {
      UINT firstCmd = GetMenuItemID(s_items[i].hPopup, 0);
      if (firstCmd == IDM_AI_TOGGLE_CHAT || firstCmd == IDM_BIKO_COMMAND_PALETTE ||
          firstCmd == IDM_AI_TRANSFORM) {
        lstrcpyW(s_items[i].szRaw, L"&Agents");
        lstrcpyW(s_items[i].szLabel, L"Agents");
        s_items[i].chMnemonic = L'a';
        d = 6;
      }
      else if (firstCmd == IDM_GIT_STATUS || firstCmd == IDM_GIT_DIFF) {
        lstrcpyW(s_items[i].szRaw, L"&Git");
        lstrcpyW(s_items[i].szLabel, L"Git");
        s_items[i].chMnemonic = L'g';
        d = 3;
      }
    }

    // Measure text width
    SIZE sz;
    GetTextExtentPoint32W(hdc, s_items[i].szLabel, d, &sz);
    s_items[i].width = sz.cx + BMB_PAD_X * 2 + 6;
    s_items[i].x = x;
    x += s_items[i].width;

    s_itemCount++;
  }

  SelectObject(hdc, of);
  ReleaseDC(s_hwnd ? s_hwnd : s_hwndParent, hdc);
}

//=============================================================================
// Hit testing
//=============================================================================

static RECT ItemRect(int i)
{
  RECT r;
  r.left   = s_items[i].x;
  r.top    = 0;
  r.right  = s_items[i].x + s_items[i].width;
  r.bottom = BMB_HEIGHT;
  return r;
}

static int HitTest(int x, int y)
{
  if (y < 0 || y >= BMB_HEIGHT) return -1;
  for (int i = 0; i < s_itemCount; i++) {
    if (x >= s_items[i].x && x < s_items[i].x + s_items[i].width)
      return i;
  }
  return -1;
}

//=============================================================================
// Painting
//=============================================================================

static void DrawMenuText(HDC hdc, const WCHAR *szRaw, RECT *prc,
                         COLORREF clrText, BOOL bHighlight)
{
  SetTextColor(hdc, clrText);
  SetBkMode(hdc, TRANSPARENT);

  // Use DrawTextW with DT_PREFIX to draw & mnemonics with underline
  // Only show underline when in alt mode
  UINT dtFlags = DT_CENTER | DT_VCENTER | DT_SINGLELINE;
  if (!s_altMode)
    dtFlags |= DT_HIDEPREFIX;

  DrawTextW(hdc, szRaw, -1, prc, dtFlags);
}

static void BuildWorkspaceLabel(WCHAR* wszBuf, int cchBuf)
{
  const WCHAR* root = FileManager_GetRootPath();
  const WCHAR* leaf = NULL;
  if (!wszBuf || cchBuf <= 0) return;
  wszBuf[0] = L'\0';
  if (!root || !*root) {
    lstrcpynW(wszBuf, L"No workspace", cchBuf);
    return;
  }
  leaf = wcsrchr(root, L'\\');
  if (leaf && leaf[1]) leaf++;
  else leaf = root;
  lstrcpynW(wszBuf, leaf, cchBuf);
}

static void Paint(HWND hwnd, HDC hdc)
{
  RECT rc;
  GetClientRect(hwnd, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) return;

  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
  HBITMAP obm = (HBITMAP)SelectObject(mem, bm);
  HFONT of = (HFONT)SelectObject(mem, s_hFont);

  SetBkMode(mem, TRANSPARENT);
  BikodeTheme_FillHalftone(mem, &rc, BikodeTheme_GetColor(BKCLR_SURFACE_MAIN));

  {
    HPEN hOuter = CreatePen(PS_SOLID, 1, BikodeTheme_GetColor(BKCLR_STROKE_DARK));
    HPEN hInner = CreatePen(PS_SOLID, 1, BikodeTheme_GetColor(BKCLR_STROKE_SOFT));
    HPEN hOld = (HPEN)SelectObject(mem, hOuter);
    MoveToEx(mem, rc.left, rc.bottom - 2, NULL);
    LineTo(mem, rc.right, rc.bottom - 2);
    SelectObject(mem, hInner);
    MoveToEx(mem, rc.left, rc.bottom - 1, NULL);
    LineTo(mem, rc.right, rc.bottom - 1);
    SelectObject(mem, hOld);
    DeleteObject(hOuter);
    DeleteObject(hInner);
  }

  SetRectEmpty(&s_rcCommandBox);

  for (int i = 0; i < s_itemCount; i++) {
    RECT ir = ItemRect(i);
    ir.top += 5;
    ir.bottom -= 5;
    if (i == s_open || i == s_hover || (s_altMode && i == s_altFocus)) {
      BikodeTheme_DrawRoundedPanel(mem, &ir,
          (i == s_open)
              ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 18)
              : BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 14),
          BikodeTheme_GetColor(BKCLR_STROKE_DARK),
          (i == s_open) ? BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
          8, FALSE);
    }
    DrawMenuText(mem, s_items[i].szRaw, &ir,
        (i == s_open) ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY) : BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
        (i == s_open));
  }

  SelectObject(mem, of);
  BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
  SelectObject(mem, obm);
  DeleteObject(bm);
  DeleteDC(mem);
}

//=============================================================================
// Popup management
//=============================================================================

static void OpenPopup(int idx)
{
  if (idx < 0 || idx >= s_itemCount) return;
  if (!s_items[idx].hPopup) return;

  s_open = idx;
  s_popupLoop = TRUE;
  InvalidateRect(s_hwnd, NULL, FALSE);
  UpdateWindow(s_hwnd);

  // Send WM_INITMENU so MsgInitMenu can enable/disable items
  SendMessage(s_hwndParent, WM_INITMENU, (WPARAM)s_hMenu, 0);
  SendMessage(s_hwndParent, WM_INITMENUPOPUP,
              (WPARAM)s_items[idx].hPopup,
              MAKELPARAM(idx, FALSE));

  // Calculate popup position: below the item, aligned left
  RECT ir = ItemRect(idx);
  POINT pt;
  pt.x = ir.left;
  pt.y = ir.bottom;
  ClientToScreen(s_hwnd, &pt);

  UINT flags = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD |
               TPM_VERPOSANIMATION;

  int cmd = (int)TrackPopupMenuEx(
      s_items[idx].hPopup,
      flags,
      pt.x, pt.y,
      s_hwndParent,
      NULL);

  s_open = -1;
  s_popupLoop = FALSE;
  s_altMode = FALSE;
  s_altFocus = -1;
  InvalidateRect(s_hwnd, NULL, FALSE);

  if (cmd > 0) {
    PostMessage(s_hwndParent, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
  }
}

//=============================================================================
// Keyboard navigation
//=============================================================================

static int FindMnemonicItem(WCHAR ch)
{
  ch = (WCHAR)(UINT_PTR)CharLowerW((LPWSTR)(UINT_PTR)ch);
  for (int i = 0; i < s_itemCount; i++) {
    if (s_items[i].chMnemonic == ch)
      return i;
  }
  return -1;
}

static void ActivateAltMode(void)
{
  s_altMode = TRUE;
  s_altFocus = 0;
  InvalidateRect(s_hwnd, NULL, FALSE);
}

static void DeactivateAltMode(void)
{
  s_altMode = FALSE;
  s_altFocus = -1;
  InvalidateRect(s_hwnd, NULL, FALSE);
}

//=============================================================================
// Hook for menu-loop arrow-key navigation
//
// When a popup is open and the user presses Left/Right, Windows doesn't
// let us intercept that easily.  We install a WH_MSGFILTER hook to
// detect arrow keys inside TrackPopupMenuEx and switch popups.
//=============================================================================

static HHOOK s_hMsgHook = NULL;
static BOOL  s_switchPending = FALSE;
static int   s_switchDir = 0;  // -1 = left, +1 = right

static LRESULT CALLBACK MenuMsgFilter(int code, WPARAM wParam, LPARAM lParam)
{
  if (code == MSGF_MENU) {
    MSG *pMsg = (MSG *)lParam;
    if (pMsg->message == WM_KEYDOWN) {
      if (pMsg->wParam == VK_LEFT) {
        s_switchPending = TRUE;
        s_switchDir = -1;
        // End the current popup
        EndMenu();
        return 1;
      }
      else if (pMsg->wParam == VK_RIGHT) {
        s_switchPending = TRUE;
        s_switchDir = 1;
        EndMenu();
        return 1;
      }
    }
  }
  return CallNextHookEx(s_hMsgHook, code, wParam, lParam);
}

static void OpenPopupWithHook(int idx)
{
  // Install hook
  s_hMsgHook = SetWindowsHookExW(WH_MSGFILTER, MenuMsgFilter,
                                  NULL, GetCurrentThreadId());
  s_switchPending = FALSE;

  do {
    s_switchPending = FALSE;
    OpenPopup(idx);
    if (s_switchPending) {
      idx += s_switchDir;
      if (idx < 0) idx = s_itemCount - 1;
      if (idx >= s_itemCount) idx = 0;
    }
  } while (s_switchPending);

  // Remove hook
  if (s_hMsgHook) {
    UnhookWindowsHookEx(s_hMsgHook);
    s_hMsgHook = NULL;
  }
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
    if (s_open < 0) {
      s_hover = -1;
      InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;

  case WM_LBUTTONDOWN: {
    int idx = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    if (idx >= 0) {
      OpenPopupWithHook(idx);
    }
    return 0;
  }

  case WM_SIZE:
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;

  case WM_DPICHANGED:
    CreateMenuFont();
    BuildItems();
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;

  default:
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

//=============================================================================
// Parent message handler — called from MainWndProc
//=============================================================================

BOOL BikoMenuBar_HandleParentMessage(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam, LRESULT *pResult)
{
  (void)hwnd;
  (void)pResult;

  switch (msg)
  {
  case WM_SYSKEYDOWN:
    // Alt + letter => find mnemonic
    if (wParam == VK_MENU || wParam == VK_F10) {
      // Plain Alt or F10: toggle alt mode
      if (!s_altMode) {
        ActivateAltMode();
      } else {
        DeactivateAltMode();
      }
      if (pResult) *pResult = 0;
      return TRUE;
    }
    if (GetKeyState(VK_MENU) & 0x8000) {
      int idx = FindMnemonicItem((WCHAR)wParam);
      if (idx >= 0) {
        OpenPopupWithHook(idx);
        if (pResult) *pResult = 0;
        return TRUE;
      }
    }
    break;

  case WM_SYSKEYUP:
    if (wParam == VK_MENU || wParam == VK_F10) {
      // Swallow the key-up so Windows doesn't activate its own menu
      if (pResult) *pResult = 0;
      return TRUE;
    }
    break;

  case WM_KEYDOWN:
    if (s_altMode) {
      switch (wParam) {
      case VK_LEFT:
        if (s_altFocus > 0) s_altFocus--;
        else s_altFocus = s_itemCount - 1;
        InvalidateRect(s_hwnd, NULL, FALSE);
        if (pResult) *pResult = 0;
        return TRUE;

      case VK_RIGHT:
        if (s_altFocus < s_itemCount - 1) s_altFocus++;
        else s_altFocus = 0;
        InvalidateRect(s_hwnd, NULL, FALSE);
        if (pResult) *pResult = 0;
        return TRUE;

      case VK_RETURN:
      case VK_DOWN:
        if (s_altFocus >= 0 && s_altFocus < s_itemCount) {
          OpenPopupWithHook(s_altFocus);
        }
        if (pResult) *pResult = 0;
        return TRUE;

      case VK_ESCAPE:
        DeactivateAltMode();
        if (pResult) *pResult = 0;
        return TRUE;

      default: {
        // Check for mnemonic
        int idx = FindMnemonicItem((WCHAR)wParam);
        if (idx >= 0) {
          OpenPopupWithHook(idx);
          if (pResult) *pResult = 0;
          return TRUE;
        }
        // Not a menu key — deactivate alt mode and let the key through
        DeactivateAltMode();
        break;
      }
      }
    }
    break;

  case WM_SYSCOMMAND:
    // Block the system menu (SC_KEYMENU) when we handle it ourselves
    if ((wParam & 0xFFF0) == SC_KEYMENU && lParam != 0) {
      int idx = FindMnemonicItem((WCHAR)lParam);
      if (idx >= 0) {
        OpenPopupWithHook(idx);
        if (pResult) *pResult = 0;
        return TRUE;
      }
    }
    if ((wParam & 0xFFF0) == SC_KEYMENU && lParam == 0) {
      // Plain Alt press — toggle alt mode
      if (!s_altMode)
        ActivateAltMode();
      else
        DeactivateAltMode();
      if (pResult) *pResult = 0;
      return TRUE;
    }
    break;
  }

  return FALSE;
}

//=============================================================================
// Public API
//=============================================================================

HWND BikoMenuBar_Create(HWND hwndParent, HINSTANCE hInst, HMENU hMenu)
{
  s_hwndParent = hwndParent;
  s_hMenu = hMenu;
  s_hInst = hInst;

  CreateMenuFont();

  if (!s_registered) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"BikoMenuBar";
    RegisterClassExW(&wc);
    s_registered = TRUE;
  }

  s_hwnd = CreateWindowExW(
      0, L"BikoMenuBar", L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
      0, 0, 800, BMB_HEIGHT,
      hwndParent, (HMENU)(UINT_PTR)0xFB20, hInst, NULL);

  if (s_hwnd) {
    BuildItems();
    // Detach the native menu from the window frame so we render our own
    SetMenu(hwndParent, NULL);
  }

  return s_hwnd;
}

void BikoMenuBar_Destroy(void)
{
  if (s_hFont && s_hFont != GetStockObject(DEFAULT_GUI_FONT)) {
    DeleteObject(s_hFont);
    s_hFont = NULL;
  }
  if (s_hwnd) {
    DestroyWindow(s_hwnd);
    s_hwnd = NULL;
  }
}

int BikoMenuBar_GetHeight(void)
{
  return (s_visible && s_hwnd) ? BMB_HEIGHT : 0;
}

void BikoMenuBar_Show(BOOL bShow)
{
  s_visible = bShow;
  if (s_hwnd) ShowWindow(s_hwnd, bShow ? SW_SHOW : SW_HIDE);
}

BOOL BikoMenuBar_IsVisible(void)
{
  return s_visible;
}

HWND BikoMenuBar_GetHwnd(void)
{
  return s_hwnd;
}

void BikoMenuBar_ApplyDarkMode(void)
{
  if (s_hwnd) InvalidateRect(s_hwnd, NULL, TRUE);
}

void BikoMenuBar_RefreshMenu(void)
{
  if (s_hwnd) {
    BuildItems();
    InvalidateRect(s_hwnd, NULL, FALSE);
  }
}

void BikoMenuBar_ForwardInitMenu(HWND hwndParent)
{
  if (s_hMenu)
    SendMessage(hwndParent, WM_INITMENU, (WPARAM)s_hMenu, 0);
}

BOOL BikoMenuBar_HasFocus(void)
{
  return s_altMode;
}
