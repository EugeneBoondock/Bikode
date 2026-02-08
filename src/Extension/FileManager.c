/******************************************************************************
*
* Biko — File Manager Panel
*
* FileManager.c
*
*   Minimalist tree-view file explorer, left-docked sidebar.
*   Dark-mode aware, repository-friendly.
*
*   Architecture:
*   =============
*   Panel   — "BikoFilePanel" child of main window. Header bar + splitter.
*   Tree    — Win32 TreeView control child of panel.
*             Double-click on file -> FileLoad().
*             Expand folder -> lazy-populate children.
*
******************************************************************************/

#include "FileManager.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include <uxtheme.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <shlobj.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

/* External: szCurFile from Notepad2 */
extern WCHAR szCurFile[MAX_PATH + 40];

/* External: FileLoad from Notepad2 */
extern BOOL FileLoad(BOOL, BOOL, BOOL, BOOL, LPCWSTR);

/* ═══════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════ */
#define PANEL_WIDTH_DEFAULT   240
#define PANEL_WIDTH_MIN       120
#define PANEL_WIDTH_MAX       600
#define SPLITTER_W            3
#define HEADER_H              28

/* Dark colours */
#define C_DKBG      RGB(25, 25, 28)
#define C_DKHDR     RGB(30, 30, 34)
#define C_DKSPLIT   RGB(45, 45, 50)
#define C_DKTXT     RGB(204, 204, 204)
#define C_DKDIM     RGB(120, 120, 128)
#define C_DKTREE    RGB(30, 30, 34)
#define C_DKBORD    RGB(50, 50, 55)

/* Light colours */
#define C_LTBG      RGB(250, 250, 250)
#define C_LTHDR     RGB(243, 244, 246)
#define C_LTSPLIT   RGB(229, 231, 235)
#define C_LTTXT     RGB(17, 24, 39)
#define C_LTDIM     RGB(107, 114, 128)
#define C_LTTREE    RGB(255, 255, 255)
#define C_LTBORD    RGB(209, 213, 219)

static const WCHAR CLS_PANEL[] = L"BikoFilePanel";

/* ═══════════════════════════════════════════════════════════════════
 * State
 * ═══════════════════════════════════════════════════════════════════ */
static HWND  s_hwndPanel   = NULL;
static HWND  s_hwndTree    = NULL;
static HWND  s_hwndMain    = NULL;
static BOOL  s_visible     = FALSE;
static BOOL  s_regClass    = FALSE;
static int   s_panelW      = PANEL_WIDTH_DEFAULT;
static WCHAR s_rootPath[MAX_PATH] = {0};
static HFONT s_fontHdr     = NULL;
static HFONT s_fontTree    = NULL;
static BOOL  s_dragging    = FALSE;
static int   s_dragStartX  = 0;
static int   s_dragStartW  = 0;

/* Forward declarations */
static LRESULT CALLBACK FilePanelProc(HWND, UINT, WPARAM, LPARAM);
static void    EnsureClass(HINSTANCE);
static void    EnsureFonts(void);
static void    PopulateRoot(void);
static void    PopulateChildren(HTREEITEM hParent);
static void    RemoveChildren(HTREEITEM hParent);
static void    OnItemExpanding(NMTREEVIEWW *pnm);
static void    OnItemActivate(NMHDR *pnm);
static void    DrawHeader(HDC hdc, RECT *prc);
static void    DrawSplitter(HDC hdc, RECT *prc);
static BOOL    GetItemPath(HTREEITEM hItem, WCHAR *pszPath, int cch);
static BOOL    HasSubFolders(const WCHAR *pszDir);

/* ── Helpers to store full path in tree item lParam ── */
static WCHAR* AllocPathStr(const WCHAR *s) {
    int len = (int)wcslen(s) + 1;
    WCHAR *p = (WCHAR*)n2e_Alloc(len * sizeof(WCHAR));
    if (p) wcscpy_s(p, len, s);
    return p;
}

/* ═══════════════════════════════════════════════════════════════════
 * Window class
 * ═══════════════════════════════════════════════════════════════════ */
static void EnsureClass(HINSTANCE hi) {
    if (s_regClass) return;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = FilePanelProc;
    wc.hInstance      = hi;
    wc.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName  = CLS_PANEL;
    RegisterClassExW(&wc);
    s_regClass = TRUE;
}

/* ═══════════════════════════════════════════════════════════════════
 * Fonts
 * ═══════════════════════════════════════════════════════════════════ */
static void EnsureFonts(void) {
    if (!s_fontHdr)
        s_fontHdr = CreateFontW(-12, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (!s_fontTree)
        s_fontTree = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

/* ═══════════════════════════════════════════════════════════════════
 * Init / Shutdown
 * ═══════════════════════════════════════════════════════════════════ */
BOOL FileManager_Init(HWND hwndMain) {
    s_hwndMain = hwndMain;
    return TRUE;
}

void FileManager_Shutdown(void) {
    if (s_hwndTree) {
        /* Free all lParam path strings */
        HTREEITEM hRoot = TreeView_GetRoot(s_hwndTree);
        /* We rely on WM_DESTROY + TVN_DELETEITEM to free these */
    }
    if (s_hwndPanel) {
        DestroyWindow(s_hwndPanel);
        s_hwndPanel = NULL;
        s_hwndTree  = NULL;
    }
    if (s_fontHdr)  { DeleteObject(s_fontHdr);  s_fontHdr  = NULL; }
    if (s_fontTree) { DeleteObject(s_fontTree); s_fontTree = NULL; }
    s_visible = FALSE;
}

/* ═══════════════════════════════════════════════════════════════════
 * Create panel + tree
 * ═══════════════════════════════════════════════════════════════════ */
static BOOL CreatePanel(HWND hwndParent) {
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);
    EnsureClass(hi);
    EnsureFonts();

    if (!s_hwndPanel) {
        s_hwndPanel = CreateWindowExW(0, CLS_PANEL, L"",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0, hwndParent,
            (HMENU)(UINT_PTR)IDC_FILEMGR_PANEL, hi, NULL);
        if (!s_hwndPanel) return FALSE;
    }

    if (!s_hwndTree) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                      TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                      TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP | TVS_TRACKSELECT;
        s_hwndTree = CreateWindowExW(0, WC_TREEVIEWW, L"",
            style,
            0, HEADER_H, 0, 0, s_hwndPanel,
            (HMENU)(UINT_PTR)IDC_FILEMGR_TREE, hi, NULL);
        if (!s_hwndTree) return FALSE;

        SendMessage(s_hwndTree, WM_SETFONT, (WPARAM)s_fontTree, TRUE);
        TreeView_SetItemHeight(s_hwndTree, 22);

        /* Apply dark mode explorer theme if available */
        if (DarkMode_IsEnabled()) {
            SetWindowTheme(s_hwndTree, L"DarkMode_Explorer", NULL);
            TreeView_SetBkColor(s_hwndTree, C_DKTREE);
            TreeView_SetTextColor(s_hwndTree, C_DKTXT);
            TreeView_SetLineColor(s_hwndTree, C_DKDIM);
        } else {
            SetWindowTheme(s_hwndTree, L"Explorer", NULL);
        }
    }

    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════
 * Show / Hide / Toggle
 * ═══════════════════════════════════════════════════════════════════ */
void FileManager_Toggle(HWND hwndP) {
    if (s_visible) FileManager_Hide();
    else FileManager_Show(hwndP);
}

void FileManager_Show(HWND hwndP) {
    if (!s_hwndPanel) {
        if (!CreatePanel(hwndP)) return;
    }

    /* If no root folder set, try to derive from current file */
    if (s_rootPath[0] == L'\0' && szCurFile[0] != L'\0') {
        WCHAR dir[MAX_PATH];
        wcscpy_s(dir, MAX_PATH, szCurFile);
        /* Walk up to find .git directory (repo root) */
        WCHAR *p = wcsrchr(dir, L'\\');
        while (p && p > dir) {
            *p = L'\0';
            WCHAR gitDir[MAX_PATH];
            swprintf(gitDir, MAX_PATH, L"%s\\.git", dir);
            if (PathIsDirectoryW(gitDir)) {
                wcscpy_s(s_rootPath, MAX_PATH, dir);
                break;
            }
            p = wcsrchr(dir, L'\\');
        }
        /* Fallback: use the file's directory */
        if (s_rootPath[0] == L'\0') {
            wcscpy_s(s_rootPath, MAX_PATH, szCurFile);
            PathRemoveFileSpecW(s_rootPath);
        }
    }

    /* If still no root, use current directory */
    if (s_rootPath[0] == L'\0') {
        GetCurrentDirectoryW(MAX_PATH, s_rootPath);
    }

    PopulateRoot();

    ShowWindow(s_hwndPanel, SW_SHOW);
    s_visible = TRUE;

    /* Trigger re-layout */
    RECT rc;
    GetClientRect(hwndP, &rc);
    SendMessage(hwndP, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
}

void FileManager_Hide(void) {
    if (s_hwndPanel) {
        ShowWindow(s_hwndPanel, SW_HIDE);
        s_visible = FALSE;
        HWND p = GetParent(s_hwndPanel);
        if (p) {
            RECT rc;
            GetClientRect(p, &rc);
            SendMessage(p, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        }
    }
}

BOOL FileManager_IsVisible(void) { return s_visible; }

/* ═══════════════════════════════════════════════════════════════════
 * Layout — called from MsgSize()
 * ═══════════════════════════════════════════════════════════════════ */
int FileManager_Layout(HWND hwndP, int *px, int *pcx, int y, int cy) {
    (void)hwndP;
    if (!s_visible || !s_hwndPanel) return 0;

    int w = s_panelW;
    if (w > *pcx - 200) w = *pcx - 200;  /* leave at least 200px for editor */
    if (w < PANEL_WIDTH_MIN) w = PANEL_WIDTH_MIN;

    MoveWindow(s_hwndPanel, *px, y, w, cy, TRUE);

    /* Position tree inside panel */
    if (s_hwndTree) {
        MoveWindow(s_hwndTree, 0, HEADER_H, w - SPLITTER_W, cy - HEADER_H, TRUE);
    }

    *px  += w;
    *pcx -= w;
    return w;
}

/* ═══════════════════════════════════════════════════════════════════
 * Open folder / Refresh
 * ═══════════════════════════════════════════════════════════════════ */
void FileManager_OpenFolder(LPCWSTR pszPath) {
    if (!pszPath || !*pszPath) return;
    wcscpy_s(s_rootPath, MAX_PATH, pszPath);
    if (s_hwndTree) {
        PopulateRoot();
    }
    if (!s_visible && s_hwndMain) {
        FileManager_Show(s_hwndMain);
    }
}

void FileManager_Refresh(void) {
    if (s_hwndTree && s_rootPath[0] != L'\0') {
        /* Remember expanded state is complex — just repopulate */
        PopulateRoot();
    }
}

HWND FileManager_GetPanelHwnd(void) { return s_hwndPanel; }

void FileManager_ApplyDarkMode(void) {
    if (s_hwndTree) {
        if (DarkMode_IsEnabled()) {
            SetWindowTheme(s_hwndTree, L"DarkMode_Explorer", NULL);
            TreeView_SetBkColor(s_hwndTree, C_DKTREE);
            TreeView_SetTextColor(s_hwndTree, C_DKTXT);
            TreeView_SetLineColor(s_hwndTree, C_DKDIM);
        } else {
            SetWindowTheme(s_hwndTree, L"Explorer", NULL);
            TreeView_SetBkColor(s_hwndTree, (COLORREF)-1);
            TreeView_SetTextColor(s_hwndTree, (COLORREF)-1);
            TreeView_SetLineColor(s_hwndTree, (COLORREF)-1);
        }
        InvalidateRect(s_hwndTree, NULL, TRUE);
    }
    if (s_hwndPanel) InvalidateRect(s_hwndPanel, NULL, TRUE);
}

/* ═══════════════════════════════════════════════════════════════════
 * Tree population helpers
 * ═══════════════════════════════════════════════════════════════════ */

/* Sorting: folders first, then alphabetical */
static int CALLBACK TreeCompare(LPARAM lp1, LPARAM lp2, LPARAM lpSort) {
    (void)lpSort;
    const WCHAR *a = (const WCHAR*)lp1;
    const WCHAR *b = (const WCHAR*)lp2;
    if (!a || !b) return 0;
    BOOL aDir = PathIsDirectoryW(a);
    BOOL bDir = PathIsDirectoryW(b);
    if (aDir && !bDir) return -1;
    if (!aDir && bDir) return 1;
    return _wcsicmp(PathFindFileNameW(a), PathFindFileNameW(b));
}

/* Add a dummy "loading" child so the + expander shows */
static void AddDummyChild(HTREEITEM hParent) {
    TVINSERTSTRUCTW tvi = {0};
    tvi.hParent      = hParent;
    tvi.hInsertAfter = TVI_LAST;
    tvi.item.mask    = TVIF_TEXT;
    tvi.item.pszText = L"";
    TreeView_InsertItem(s_hwndTree, &tvi);
}

/* Check if item is a dummy placeholder */
static BOOL IsDummyChild(HTREEITEM hItem) {
    TVITEMW ti = {0};
    ti.mask  = TVIF_PARAM;
    ti.hItem = hItem;
    TreeView_GetItem(s_hwndTree, &ti);
    return (ti.lParam == 0);
}

/* Should we skip this file/folder? (hidden files, .git, etc.) */
static BOOL ShouldSkip(const WCHAR *name, DWORD attrs) {
    if (name[0] == L'.' && name[1] == L'\0') return TRUE;
    if (name[0] == L'.' && name[1] == L'.' && name[2] == L'\0') return TRUE;
    /* Skip common clutter but show dotfiles */
    if (_wcsicmp(name, L".git") == 0) return TRUE;
    if (_wcsicmp(name, L".vs") == 0) return TRUE;
    if (_wcsicmp(name, L"node_modules") == 0) return TRUE;
    if (_wcsicmp(name, L"__pycache__") == 0) return TRUE;
    if (attrs & FILE_ATTRIBUTE_HIDDEN) return TRUE;
    if (attrs & FILE_ATTRIBUTE_SYSTEM) return TRUE;
    return FALSE;
}

static void PopulateFolder(HTREEITEM hParent, const WCHAR *dir) {
    WCHAR pattern[MAX_PATH + 4];
    swprintf(pattern, MAX_PATH + 4, L"%s\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (ShouldSkip(fd.cFileName, fd.dwFileAttributes)) continue;

        WCHAR fullPath[MAX_PATH];
        swprintf(fullPath, MAX_PATH, L"%s\\%s", dir, fd.cFileName);

        BOOL isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        TVINSERTSTRUCTW tvi = {0};
        tvi.hParent      = hParent;
        tvi.hInsertAfter = TVI_LAST;
        tvi.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
        tvi.item.pszText = fd.cFileName;
        tvi.item.lParam  = (LPARAM)AllocPathStr(fullPath);
        tvi.item.cChildren = isDir ? 1 : 0;  /* 1 = has expander */

        TreeView_InsertItem(s_hwndTree, &tvi);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);

    /* Sort: folders first, then alpha */
    TVSORTCB sort = {0};
    sort.hParent     = hParent;
    sort.lpfnCompare = TreeCompare;
    TreeView_SortChildrenCB(s_hwndTree, &sort, 0);
}

static void PopulateRoot(void) {
    if (!s_hwndTree || s_rootPath[0] == L'\0') return;

    /* Disable redraw during bulk insert */
    SendMessage(s_hwndTree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(s_hwndTree);

    /* Add the root folder as a top-level node, expanded */
    const WCHAR *rootName = PathFindFileNameW(s_rootPath);

    TVINSERTSTRUCTW tvi = {0};
    tvi.hParent      = TVI_ROOT;
    tvi.hInsertAfter = TVI_FIRST;
    tvi.item.mask    = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN | TVIF_STATE;
    tvi.item.pszText = (LPWSTR)rootName;
    tvi.item.lParam  = (LPARAM)AllocPathStr(s_rootPath);
    tvi.item.cChildren = 1;
    tvi.item.state     = TVIS_EXPANDED;
    tvi.item.stateMask = TVIS_EXPANDED;

    HTREEITEM hRoot = TreeView_InsertItem(s_hwndTree, &tvi);

    /* Populate first level */
    PopulateFolder(hRoot, s_rootPath);

    SendMessage(s_hwndTree, WM_SETREDRAW, TRUE, 0);
    TreeView_Expand(s_hwndTree, hRoot, TVE_EXPAND);
    InvalidateRect(s_hwndTree, NULL, TRUE);
}

/* Remove all children of an item (used before repopulating) */
static void RemoveChildren(HTREEITEM hParent) {
    HTREEITEM hChild = TreeView_GetChild(s_hwndTree, hParent);
    while (hChild) {
        HTREEITEM hNext = TreeView_GetNextSibling(s_hwndTree, hChild);
        TreeView_DeleteItem(s_hwndTree, hChild);
        hChild = hNext;
    }
}

/* Lazy expand: populate children when folder is first expanded */
static void OnItemExpanding(NMTREEVIEWW *pnm) {
    if (pnm->action != TVE_EXPAND) return;

    HTREEITEM hItem = pnm->itemNew.hItem;

    /* Check if the first child is a dummy (no lParam) or we need to repopulate */
    HTREEITEM hChild = TreeView_GetChild(s_hwndTree, hItem);
    if (!hChild) return;

    /* If already populated with real items, don't re-populate */
    TVITEMW ti = {0};
    ti.mask  = TVIF_PARAM;
    ti.hItem = hChild;
    TreeView_GetItem(s_hwndTree, &ti);

    /* We use cChildren=1 and populate on first expand.
       If first child has a valid lParam, it's already populated. */
    /* Actually, we populated folders immediately at first level, so need a different check.
       Use a sentinel: if item has exactly 0 children (after deleting dummy), refill. */

    /* Our approach: cChildren=1 signals "this is a folder". On first expand,
       children are NOT yet added (except for root level).
       So check: does this item have any children at all? If not, populate. */

    /* For non-root folders, the children haven't been added yet because we
       set cChildren=1 but didn't call PopulateFolder for them.
       Only the root's direct children are populated in PopulateRoot.
       So for sub-folders we need to populate on expand. */

    /* Build the full path for this item */
    WCHAR path[MAX_PATH] = {0};
    ti.mask  = TVIF_PARAM;
    ti.hItem = hItem;
    TreeView_GetItem(s_hwndTree, &ti);
    if (!ti.lParam) return;

    const WCHAR *itemPath = (const WCHAR*)ti.lParam;

    /* If children already exist, check if they're real */
    hChild = TreeView_GetChild(s_hwndTree, hItem);
    if (hChild) {
        TVITEMW ci = {0};
        ci.mask  = TVIF_PARAM;
        ci.hItem = hChild;
        TreeView_GetItem(s_hwndTree, &ci);
        if (ci.lParam != 0) return;  /* Already populated with real items */
    }

    /* Remove any dummy children and populate */
    SendMessage(s_hwndTree, WM_SETREDRAW, FALSE, 0);
    RemoveChildren(hItem);
    PopulateFolder(hItem, itemPath);
    SendMessage(s_hwndTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_hwndTree, NULL, TRUE);
}

/* Double-click on file -> open it */
static void OnItemActivate(NMHDR *pnm) {
    (void)pnm;
    HTREEITEM hSel = TreeView_GetSelection(s_hwndTree);
    if (!hSel) return;

    TVITEMW ti = {0};
    ti.mask  = TVIF_PARAM | TVIF_CHILDREN;
    ti.hItem = hSel;
    TreeView_GetItem(s_hwndTree, &ti);

    if (ti.cChildren) {
        /* It's a folder — toggle expand */
        TreeView_Expand(s_hwndTree, hSel, TVE_TOGGLE);
        return;
    }

    /* It's a file — open it */
    const WCHAR *filePath = (const WCHAR*)ti.lParam;
    if (filePath && filePath[0]) {
        FileLoad(FALSE, FALSE, FALSE, FALSE, filePath);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Path retrieval
 * ═══════════════════════════════════════════════════════════════════ */
static BOOL GetItemPath(HTREEITEM hItem, WCHAR *pszPath, int cch) {
    TVITEMW ti = {0};
    ti.mask  = TVIF_PARAM;
    ti.hItem = hItem;
    TreeView_GetItem(s_hwndTree, &ti);
    if (!ti.lParam) return FALSE;
    wcscpy_s(pszPath, cch, (const WCHAR*)ti.lParam);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════
 * Drawing
 * ═══════════════════════════════════════════════════════════════════ */
static void DrawHeader(HDC hdc, RECT *prc) {
    BOOL dk = DarkMode_IsEnabled();
    COLORREF bgClr  = dk ? C_DKHDR  : C_LTHDR;
    COLORREF txtClr = dk ? C_DKDIM  : C_LTDIM;
    COLORREF brdClr = dk ? C_DKBORD : C_LTBORD;

    HBRUSH hbr = CreateSolidBrush(bgClr);
    FillRect(hdc, prc, hbr);
    DeleteObject(hbr);

    /* Border at bottom */
    HPEN pen = CreatePen(PS_SOLID, 1, brdClr);
    HPEN old = SelectObject(hdc, pen);
    MoveToEx(hdc, prc->left, prc->bottom - 1, NULL);
    LineTo(hdc, prc->right, prc->bottom - 1);
    SelectObject(hdc, old);
    DeleteObject(pen);

    /* Title text */
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, txtClr);
    HFONT oldFont = SelectObject(hdc, s_fontHdr);
    RECT textRc = *prc;
    textRc.left += 10;
    textRc.right -= 4;

    /* Show "EXPLORER" label in caps, minimal style */
    DrawTextW(hdc, L"EXPLORER", -1, &textRc,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

static void DrawSplitter(HDC hdc, RECT *prc) {
    BOOL dk = DarkMode_IsEnabled();
    HBRUSH hbr = CreateSolidBrush(dk ? C_DKSPLIT : C_LTSPLIT);
    FillRect(hdc, prc, hbr);
    DeleteObject(hbr);
}

/* ═══════════════════════════════════════════════════════════════════
 * Splitter drag (resize panel)
 * ═══════════════════════════════════════════════════════════════════ */
static BOOL HitSplitter(int x, int panelW) {
    return (x >= panelW - SPLITTER_W && x <= panelW);
}

/* ═══════════════════════════════════════════════════════════════════
 * Panel WndProc
 * ═══════════════════════════════════════════════════════════════════ */
static LRESULT CALLBACK FilePanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        /* Header */
        RECT hdrRc = { 0, 0, rc.right, HEADER_H };
        DrawHeader(hdc, &hdrRc);

        /* Splitter on right edge */
        RECT splitRc = { rc.right - SPLITTER_W, HEADER_H, rc.right, rc.bottom };
        DrawSplitter(hdc, &splitRc);

        /* Background below header if tree doesn't cover it */
        BOOL dk = DarkMode_IsEnabled();
        RECT bgRc = { 0, HEADER_H, rc.right - SPLITTER_W, rc.bottom };
        HBRUSH hbr = CreateSolidBrush(dk ? C_DKTREE : C_LTTREE);
        FillRect(hdc, &bgRc, hbr);
        DeleteObject(hbr);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;  /* We paint everything */

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (HitSplitter(pt.x, rc.right)) {
            SetCursor(LoadCursor(NULL, IDC_SIZEWE));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lParam);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (HitSplitter(x, rc.right)) {
            s_dragging  = TRUE;
            s_dragStartX = x;
            s_dragStartW = s_panelW;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        if (s_dragging) {
            int x = (short)LOWORD(lParam);
            int delta = x - s_dragStartX;
            int newW = s_dragStartW + delta;
            if (newW < PANEL_WIDTH_MIN) newW = PANEL_WIDTH_MIN;
            if (newW > PANEL_WIDTH_MAX) newW = PANEL_WIDTH_MAX;
            s_panelW = newW;
            /* Trigger parent re-layout */
            HWND p = GetParent(hwnd);
            if (p) {
                RECT prc;
                GetClientRect(p, &prc);
                SendMessage(p, WM_SIZE, SIZE_RESTORED,
                            MAKELPARAM(prc.right, prc.bottom));
            }
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (s_dragging) {
            s_dragging = FALSE;
            ReleaseCapture();
            return 0;
        }
        break;
    }

    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR*)lParam;
        if (hdr->hwndFrom == s_hwndTree) {
            switch (hdr->code) {
            case TVN_ITEMEXPANDINGW:
                OnItemExpanding((NMTREEVIEWW*)lParam);
                break;
            case NM_DBLCLK:
                OnItemActivate(hdr);
                return 0;  /* Don't let default processing interfere */
            case TVN_DELETEITEMW: {
                /* Free the path string stored in lParam */
                NMTREEVIEWW *pnm = (NMTREEVIEWW*)lParam;
                if (pnm->itemOld.lParam) {
                    n2e_Free((void*)pnm->itemOld.lParam);
                }
                break;
            }
            case NM_RETURN:
                OnItemActivate(hdr);
                return 0;
            }
        }
        break;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        if (DarkMode_IsEnabled()) {
            HDC hdcCtl = (HDC)wParam;
            SetTextColor(hdcCtl, C_DKTXT);
            SetBkColor(hdcCtl, C_DKTREE);
            static HBRUSH s_treeBrush = NULL;
            if (!s_treeBrush) s_treeBrush = CreateSolidBrush(C_DKTREE);
            return (LRESULT)s_treeBrush;
        }
        break;
    }

    case WM_DESTROY:
        s_hwndTree  = NULL;
        s_hwndPanel = NULL;
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ═══════════════════════════════════════════════════════════════════
 * Browse for folder dialog
 * ═══════════════════════════════════════════════════════════════════ */
void FileManager_BrowseForFolder(HWND hwndParent) {
    WCHAR szFolder[MAX_PATH] = {0};
    BROWSEINFOW bi = {0};
    bi.hwndOwner      = hwndParent;
    bi.pszDisplayName  = szFolder;
    bi.lpszTitle       = L"Open Folder";
    bi.ulFlags         = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl  = SHBrowseForFolderW(&bi);
    if (pidl) {
        SHGetPathFromIDListW(pidl, szFolder);
        CoTaskMemFree(pidl);
        if (szFolder[0]) FileManager_OpenFolder(szFolder);
    }
}
