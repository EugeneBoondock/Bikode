#include "Subclassing.h"

#define PROPERTY_ORIGINAL_WINDOW_PROC L"OriginalWindowProc"

BOOL n2e_IsSubclassedWindow(const HWND hwnd)
{
  return (GetProp(hwnd, PROPERTY_ORIGINAL_WINDOW_PROC) != 0);
}

BOOL n2e_SubclassWindow(const HWND hwnd, const WNDPROC lpWndProc)
{
  if (!n2e_IsSubclassedWindow(hwnd))
  {
    WNDPROC oldProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)lpWndProc);
    if (oldProc)
    {
      SetProp(hwnd, PROPERTY_ORIGINAL_WINDOW_PROC, (HANDLE)oldProc);
      return TRUE;
    }
  }
  return FALSE;
}

BOOL n2e_ForceSubclassWindow(const HWND hwnd, const WNDPROC lpWndProc)
{
  // Force subclass even if already subclassed - useful for terminal/chat windows
  // that need to override the default Scintilla subclass
  WNDPROC oldProc = (WNDPROC)GetWindowLongPtr(hwnd, GWLP_WNDPROC);
  if (oldProc && oldProc != lpWndProc)
  {
    SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)lpWndProc);

    // If this window was already subclassed by us, keep the original proc
    // Otherwise, store the current proc as original
    if (!n2e_IsSubclassedWindow(hwnd))
    {
      SetProp(hwnd, PROPERTY_ORIGINAL_WINDOW_PROC, (HANDLE)oldProc);
    }
    return TRUE;
  }
  return n2e_SubclassWindow(hwnd, lpWndProc);
}

WNDPROC n2e_GetOriginalWindowProc(const HWND hwnd)
{
  return (WNDPROC)GetProp(hwnd, PROPERTY_ORIGINAL_WINDOW_PROC);
}

LRESULT n2e_CallOriginalWindowProc(const HWND hwnd, const UINT uMsg, const WPARAM wParam, const LPARAM lParam)
{
  return CallWindowProc(n2e_GetOriginalWindowProc(hwnd), hwnd, uMsg, wParam, lParam);
}
