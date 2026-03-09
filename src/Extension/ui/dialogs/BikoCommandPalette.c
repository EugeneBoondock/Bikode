/******************************************************************************
*
* Biko
*
* BikoCommandPalette.c
*   Compact owner-drawn command palette overlay.
*
******************************************************************************/

#include "BikoCommandPalette.h"
#include "../theme/BikodeTheme.h"
#include "../../AICommands.h"
#include "../../PluginManager.h"
#include "../../../resource.h"
#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>

#define IDC_CP_SEARCH  0xF301
#define IDC_CP_LIST    0xF302
#define CP_WIDTH       680
#define CP_HEIGHT      420

typedef struct PaletteItem {
    UINT cmdId;
    LPCWSTR category;
    LPCWSTR title;
    LPCWSTR shortcut;
} PaletteItem;

static const PaletteItem s_items[] = {
    { IDM_AI_EXPLAIN, L"Agents", L"Explain selection", L"AI" },
    { IDM_AI_FIX, L"Agents", L"Fix error", L"AI" },
    { IDM_AI_REFACTOR, L"Agents", L"Refactor symbol", L"AI" },
    { IDM_AI_CHAT, L"Agents", L"Open mission control", L"Ctrl+Shift+C" },
    { IDM_AI_SETTINGS, L"Settings", L"Open AI settings", L"" },
    { IDM_FILEMGR_TOGGLE, L"Workspace", L"Toggle explorer", L"" },
    { IDM_EDIT_FIND, L"Search", L"Find in file", L"Ctrl+F" },
    { IDM_VIEW_SHOWOUTLINE, L"Symbols", L"Show outline", L"" },
    { IDM_GIT_STATUS, L"Git", L"Show git status", L"" },
    { IDM_GIT_DIFF, L"Git", L"Show git diff", L"" },
    { IDM_TERMINAL_TOGGLE, L"Run", L"Toggle terminal", L"" },
    { IDM_TERMINAL_NEW, L"Run", L"New terminal", L"" },
    { IDM_PLUGIN_SETTINGS, L"Plugins", L"Plugin marketplace", L"" },
    { IDM_MARKDOWN_PREVIEW, L"View", L"Toggle markdown preview", L"" }
};

static HWND s_hwnd = NULL;
static HWND s_hwndParent = NULL;
static BOOL s_registered = FALSE;

static void PopulateList(HWND hwnd)
{
    HWND hEdit = GetDlgItem(hwnd, IDC_CP_SEARCH);
    HWND hList = GetDlgItem(hwnd, IDC_CP_LIST);
    WCHAR query[128];
    ListBox_ResetContent(hList);
    GetWindowTextW(hEdit, query, ARRAYSIZE(query));

    for (int i = 0; i < ARRAYSIZE(s_items); i++)
    {
        if (query[0])
        {
            WCHAR hay[256];
            wsprintfW(hay, L"%s %s", s_items[i].category, s_items[i].title);
            if (!StrStrIW(hay, query))
                continue;
        }
        int row = (int)ListBox_AddString(hList, s_items[i].title);
        ListBox_SetItemData(hList, row, i);
    }
    if (ListBox_GetCount(hList) > 0)
        ListBox_SetCurSel(hList, 0);
}

static void ExecuteSelected(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_CP_LIST);
    int sel = ListBox_GetCurSel(hList);
    if (sel != LB_ERR)
    {
        int idx = (int)ListBox_GetItemData(hList, sel);
        if (idx >= 0 && idx < ARRAYSIZE(s_items))
            PostMessage(s_hwndParent, WM_COMMAND, MAKEWPARAM(s_items[idx].cmdId, 0), 0);
    }
    DestroyWindow(hwnd);
}

static void LayoutChildren(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    MoveWindow(GetDlgItem(hwnd, IDC_CP_SEARCH), 18, 56, rc.right - 36, 32, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_CP_LIST), 18, 100, rc.right - 36, rc.bottom - 118, TRUE);
}

static LRESULT CALLBACK PaletteProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HWND hEdit;
        HWND hList;
        CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_CP_SEARCH, NULL, NULL);
        CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_CP_LIST, NULL, NULL);
        hEdit = GetDlgItem(hwnd, IDC_CP_SEARCH);
        hList = GetDlgItem(hwnd, IDC_CP_LIST);
        SendMessage(hEdit, WM_SETFONT, (WPARAM)BikodeTheme_GetFont(BKFONT_UI), TRUE);
        SendMessage(hList, WM_SETFONT, (WPARAM)BikodeTheme_GetFont(BKFONT_UI), TRUE);
        SendMessage(hList, LB_SETITEMHEIGHT, 0, 48);
        LayoutChildren(hwnd);
        PopulateList(hwnd);
        SetFocus(hEdit);
        return 0;
    }

    case WM_SIZE:
        LayoutChildren(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc, title, subtitle;
        GetClientRect(hwnd, &rc);
        BikodeTheme_DrawCutCornerPanel(hdc, &rc,
            BikodeTheme_GetColor(BKCLR_SURFACE_ELEVATED),
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            18, TRUE);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
        SelectObject(hdc, BikodeTheme_GetFont(BKFONT_DISPLAY));
        title.left = 18; title.top = 14; title.right = rc.right - 18; title.bottom = 40;
        DrawTextW(hdc, L"COMMAND PALETTE", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL));
        SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
        subtitle.left = 20; subtitle.top = 36; subtitle.right = rc.right - 20; subtitle.bottom = 54;
        DrawTextW(hdc, L"Search commands, tools, and mission shortcuts", -1, &subtitle,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        static HBRUSH hBrush = NULL;
        if (!hBrush)
            hBrush = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SURFACE_RAISED));
        SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
        SetBkColor(hdc, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED));
        return (LRESULT)hBrush;
    }

    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        if (!dis || dis->CtlID != IDC_CP_LIST || dis->itemID == (UINT)-1)
            break;
        int idx = (int)ListBox_GetItemData(dis->hwndItem, dis->itemID);
        RECT card = dis->rcItem;
        BOOL selected = (dis->itemState & ODS_SELECTED) != 0;
        COLORREF fill = selected
            ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 32)
            : BikodeTheme_GetColor(BKCLR_SURFACE_RAISED);
        BikodeTheme_DrawRoundedPanel(dis->hDC, &card, fill,
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            selected ? BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            10, FALSE);

        RECT chip = { card.left + 10, card.top + 10, card.left + 100, card.top + 30 };
        RECT title = { card.left + 10, card.top + 18, card.right - 90, card.bottom - 8 };
        RECT shortcut = { card.right - 80, card.top + 12, card.right - 12, card.top + 30 };
        BikodeTheme_DrawChip(dis->hDC, &chip, s_items[idx].category,
            BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY), BikodeTheme_GetFont(BKFONT_UI_SMALL),
            TRUE, BikodeTheme_GetColor(BKCLR_HOT_MAGENTA));
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
        SelectObject(dis->hDC, BikodeTheme_GetFont(BKFONT_UI_BOLD));
        DrawTextW(dis->hDC, s_items[idx].title, -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (s_items[idx].shortcut && s_items[idx].shortcut[0])
        {
            BikodeTheme_DrawChip(dis->hDC, &shortcut, s_items[idx].shortcut,
                BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                BikodeTheme_GetColor(BKCLR_TEXT_MUTED), BikodeTheme_GetFont(BKFONT_MONO_SMALL),
                FALSE, 0);
        }
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CP_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            PopulateList(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_CP_LIST && HIWORD(wParam) == LBN_DBLCLK) {
            ExecuteSelected(hwnd);
            return 0;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_RETURN) {
            ExecuteSelected(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        s_hwnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void BikoCommandPalette_Show(HWND hwndParent)
{
    HINSTANCE hInst;
    RECT rcParent;
    int x, y;
    BikodeTheme_Init();
    s_hwndParent = hwndParent;
    hInst = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);
    if (!s_registered)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = PaletteProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"BikoCommandPalette";
        RegisterClassExW(&wc);
        s_registered = TRUE;
    }

    if (s_hwnd) {
        SetForegroundWindow(s_hwnd);
        SetFocus(GetDlgItem(s_hwnd, IDC_CP_SEARCH));
        return;
    }

    GetWindowRect(hwndParent, &rcParent);
    x = rcParent.left + ((rcParent.right - rcParent.left) - CP_WIDTH) / 2;
    y = rcParent.top + 76;
    s_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"BikoCommandPalette", L"",
        WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, CP_WIDTH, CP_HEIGHT, hwndParent, NULL, hInst, NULL);
}

void BikoCommandPalette_Hide(void)
{
    if (s_hwnd)
        DestroyWindow(s_hwnd);
}

void BikoCommandPalette_ApplyTheme(void)
{
    if (s_hwnd)
        InvalidateRect(s_hwnd, NULL, TRUE);
}

BOOL BikoCommandPalette_IsVisible(void)
{
    return s_hwnd != NULL;
}
