/******************************************************************************
*
* Biko
*
* GitUI.c
*   Git integration via git.exe subprocess.
*   Dark-mode-aware dialogs for status, diff, log, commit.
*
******************************************************************************/

#include "GitUI.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include "FileManager.h"
#include <string.h>
#include <stdio.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

//=============================================================================
// Internal state
//=============================================================================

static HWND     s_hwndMain = NULL;
static BOOL     s_bInRepo = FALSE;
static WCHAR    s_wszGitExe[MAX_PATH] = L"";
static WCHAR    s_wszRepoRoot[MAX_PATH] = L"";
static WCHAR    s_wszBranch[128] = L"";
static WCHAR    s_wszStatusSummary[64] = L"";
static BOOL     s_bPanelVisible = FALSE;

// Replace all uses of szCurFile or current directory for git commands:
// For each git operation, use FileManager_GetRootPath() as the working directory.

// Example for command execution:
// BOOL bOk = CreateProcessW(
//     NULL, wszCmd, NULL, NULL, TRUE,
//     CREATE_NO_WINDOW, NULL, FileManager_GetRootPath(),
//     &si, &pi);

extern WCHAR szCurFile[MAX_PATH + 40];

//=============================================================================
// Forward declarations
//=============================================================================

static BOOL FindGitExe(void);
static BOOL DetectRepoRoot(const WCHAR* pszFile);
static void UpdateBranch(void);
static void UpdateStatusSummary(void);

//=============================================================================
// Helpers: run a process and capture output
//=============================================================================

static BOOL RunProcess(const WCHAR* pszCmdLine, const WCHAR* pszWorkDir,
                       char** ppOutput, int* piExitCode)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return FALSE;

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    WCHAR wszCmd[4096];
    wcsncpy_s(wszCmd, _countof(wszCmd), pszCmdLine, _TRUNCATE);

    BOOL bOk = CreateProcessW(
        NULL, wszCmd, NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, pszWorkDir,
        &si, &pi);

    CloseHandle(hWritePipe);

    if (!bOk)
    {
        CloseHandle(hReadPipe);
        return FALSE;
    }

    char* buf = NULL;
    int bufSize = 0;
    int bufCap = 4096;
    buf = (char*)n2e_Alloc(bufCap);

    for (;;)
    {
        DWORD dwRead = 0;
        char tmp[4096];
        if (!ReadFile(hReadPipe, tmp, sizeof(tmp), &dwRead, NULL) || dwRead == 0)
            break;

        if (bufSize + (int)dwRead >= bufCap)
        {
            bufCap = (bufSize + dwRead) * 2;
            buf = (char*)n2e_Realloc(buf, bufCap);
        }
        memcpy(buf + bufSize, tmp, dwRead);
        bufSize += dwRead;
    }

    if (buf)
        buf[bufSize] = '\0';

    WaitForSingleObject(pi.hProcess, 5000);

    DWORD dwExit = 0;
    GetExitCodeProcess(pi.hProcess, &dwExit);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

    if (ppOutput)
        *ppOutput = buf;
    else
        n2e_Free(buf);

    if (piExitCode)
        *piExitCode = (int)dwExit;

    return TRUE;
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

BOOL GitUI_Init(HWND hwndMain)
{
    s_hwndMain = hwndMain;

    if (!FindGitExe())
        return FALSE;

    if (FileManager_GetRootPath()[0])
    {
        DetectRepoRoot(FileManager_GetRootPath());
        if (s_bInRepo)
        {
            UpdateBranch();
            UpdateStatusSummary();
        }
    }

    return TRUE;
}

void GitUI_Shutdown(void)
{
    s_hwndMain = NULL;
    s_bInRepo = FALSE;
    s_bPanelVisible = FALSE;
}

//=============================================================================
// Public: Repo state
//=============================================================================

BOOL GitUI_IsInRepo(void)
{
    return s_bInRepo;
}

const WCHAR* GitUI_GetBranch(void)
{
    return s_wszBranch;
}

const WCHAR* GitUI_GetStatusSummary(void)
{
    return s_wszStatusSummary;
}

void GitUI_Refresh(void)
{
    if (FileManager_GetRootPath()[0])
    {
        DetectRepoRoot(FileManager_GetRootPath());
        if (s_bInRepo)
        {
            UpdateBranch();
            UpdateStatusSummary();
        }
        else
        {
            s_wszBranch[0] = L'\0';
            s_wszStatusSummary[0] = L'\0';
        }
    }
}

//=============================================================================
// Dark-mode dialog colours
//=============================================================================

#define GIT_DLG_DK_BG     RGB(30, 30, 30)
#define GIT_DLG_DK_TEXT   RGB(212, 212, 212)
#define GIT_DLG_DK_EDIT   RGB(37, 37, 37)

#define GIT_DLG_LT_BG     RGB(248, 248, 248)
#define GIT_DLG_LT_TEXT   RGB(30, 30, 30)
#define GIT_DLG_LT_EDIT   RGB(255, 255, 255)

/* dialog control IDs */
#define IDC_GIT_EDIT       1001
#define IDC_GIT_COMMIT_MSG 1002

//=============================================================================
// Shared data structs
//=============================================================================

typedef struct {
    const WCHAR *title;
    WCHAR       *text;
} GitDlgData;

typedef struct {
    WCHAR   commitMsg[2048];
    BOOL    committed;
    WCHAR  *statusText;
} GitCommitData;

//=============================================================================
// Dark mode helpers
//=============================================================================

static HBRUSH s_dlgBgBrush  = NULL;
static HBRUSH s_dlgEditBrush = NULL;

static void InitDlgBrushes(void)
{
    BOOL dk = DarkMode_IsEnabled();
    if (s_dlgBgBrush)   DeleteObject(s_dlgBgBrush);
    if (s_dlgEditBrush) DeleteObject(s_dlgEditBrush);
    s_dlgBgBrush   = CreateSolidBrush(dk ? GIT_DLG_DK_BG   : GIT_DLG_LT_BG);
    s_dlgEditBrush = CreateSolidBrush(dk ? GIT_DLG_DK_EDIT  : GIT_DLG_LT_EDIT);
}

static void FreeDlgBrushes(void)
{
    if (s_dlgBgBrush)   { DeleteObject(s_dlgBgBrush);   s_dlgBgBrush = NULL; }
    if (s_dlgEditBrush) { DeleteObject(s_dlgEditBrush);  s_dlgEditBrush = NULL; }
}

static void SetDarkTitleBar(HWND hwnd)
{
    if (!DarkMode_IsEnabled()) return;
    BOOL val = TRUE;
    typedef HRESULT (WINAPI *PFN)(HWND,DWORD,LPCVOID,DWORD);
    HMODULE hDwm = GetModuleHandleW(L"dwmapi.dll");
    if (!hDwm) hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        PFN pfn = (PFN)GetProcAddress(hDwm, "DwmSetWindowAttribute");
        if (pfn) pfn(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &val, sizeof(val));
    }
}

static void CenterOnParent(HWND hwnd)
{
    RECT rcP, rcD;
    GetWindowRect(GetParent(hwnd), &rcP);
    GetWindowRect(hwnd, &rcD);
    int dw = rcD.right - rcD.left, dh = rcD.bottom - rcD.top;
    int x = rcP.left + ((rcP.right - rcP.left) - dw) / 2;
    int y = rcP.top  + ((rcP.bottom - rcP.top) - dh) / 2;
    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

/* Fix bare \n -> \r\n for edit controls */
static WCHAR* FixLineEndings(const WCHAR *src)
{
    if (!src) return NULL;
    int len = (int)wcslen(src);
    int extra = 0;
    int i;
    for (i = 0; src[i]; i++)
        if (src[i] == L'\n' && (i == 0 || src[i-1] != L'\r'))
            extra++;
    if (extra == 0)
    {
        WCHAR *dup = (WCHAR*)n2e_Alloc((len + 1) * sizeof(WCHAR));
        if (dup) wcscpy_s(dup, len + 1, src);
        return dup;
    }
    {
        WCHAR *out = (WCHAR*)n2e_Alloc((len + extra + 1) * sizeof(WCHAR));
        int j = 0;
        if (!out) return NULL;
        for (i = 0; src[i]; i++) {
            if (src[i] == L'\n' && (i == 0 || src[i-1] != L'\r'))
                out[j++] = L'\r';
            out[j++] = src[i];
        }
        out[j] = 0;
        return out;
    }
}

//=============================================================================
// Output dialog — resizable, scrollable, monospace, dark-mode
//=============================================================================

static INT_PTR CALLBACK GitOutputDlgProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG: {
        GitDlgData *d = (GitDlgData*)lParam;
        HWND hEdit, hOK;
        HFONT hFont;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        SetWindowTextW(hwnd, d->title);

        hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        if (!hFont) hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)hFont);

        hEdit = GetDlgItem(hwnd, IDC_GIT_EDIT);
        if (hEdit && d->text) {
            SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetWindowTextW(hEdit, d->text);
        }

        InitDlgBrushes();
        SetDarkTitleBar(hwnd);
        CenterOnParent(hwnd);

        hOK = GetDlgItem(hwnd, IDOK);
        if (hOK) {
            BOOL dk = DarkMode_IsEnabled();
            if (dk) {
                SetWindowTextW(hOK, L"Close");
            }
        }

        return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC: {
        BOOL dk = DarkMode_IsEnabled();
        SetTextColor((HDC)wParam, dk ? GIT_DLG_DK_TEXT : GIT_DLG_LT_TEXT);
        SetBkColor  ((HDC)wParam, dk ? GIT_DLG_DK_BG   : GIT_DLG_LT_BG);
        return (INT_PTR)s_dlgBgBrush;
    }

    case WM_CTLCOLOREDIT: {
        BOOL dk = DarkMode_IsEnabled();
        SetTextColor((HDC)wParam, dk ? GIT_DLG_DK_TEXT : GIT_DLG_LT_TEXT);
        SetBkColor  ((HDC)wParam, dk ? GIT_DLG_DK_EDIT : GIT_DLG_LT_EDIT);
        return (INT_PTR)s_dlgEditBrush;
    }

    case WM_SIZE: {
        int cx = LOWORD(lParam), cy = HIWORD(lParam);
        HWND hEdit = GetDlgItem(hwnd, IDC_GIT_EDIT);
        HWND hOK   = GetDlgItem(hwnd, IDOK);
        if (hEdit) MoveWindow(hEdit, 8, 8, cx - 16, cy - 48, TRUE);
        if (hOK)   MoveWindow(hOK, cx - 88, cy - 34, 80, 26, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            HFONT hf = (HFONT)GetWindowLongPtr(hwnd, DWLP_USER);
            if (hf) DeleteObject(hf);
            FreeDlgBrushes();
            EndDialog(hwnd, LOWORD(wParam));
            return TRUE;
        }
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 400;
        mmi->ptMinTrackSize.y = 300;
        return 0;
    }
    }
    return FALSE;
}

//=============================================================================
// Build in-memory dialog template for output viewer
//=============================================================================

static int BuildOutputDlgTemplate(BYTE *buf, int bufSize)
{
    int off = 0;
    DLGTEMPLATE *dt;
    DLGITEMTEMPLATE *it;
    const WCHAR *fn;
    const WCHAR *t;
    size_t fl, tl;

    ZeroMemory(buf, bufSize);

    dt = (DLGTEMPLATE*)(buf + off);
    dt->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                WS_VISIBLE | DS_MODALFRAME | DS_SETFONT;
    dt->cdit = 2;
    dt->x = 0; dt->y = 0; dt->cx = 420; dt->cy = 300;
    off += sizeof(DLGTEMPLATE);

    *(WORD*)(buf + off) = 0; off += 2;          /* menu */
    *(WORD*)(buf + off) = 0; off += 2;          /* class */
    *(WCHAR*)(buf + off) = 0; off += 2;         /* title */
    *(WORD*)(buf + off) = 9; off += 2;          /* font size */
    fn = L"Segoe UI"; fl = wcslen(fn) + 1;
    memcpy(buf + off, fn, fl * sizeof(WCHAR)); off += (int)(fl * sizeof(WCHAR));

    /* Edit control */
    off = (off + 3) & ~3;
    it = (DLGITEMTEMPLATE*)(buf + off);
    it->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_LEFT;
    it->x = 4; it->y = 4; it->cx = 412; it->cy = 272;
    it->id = IDC_GIT_EDIT;
    off += sizeof(DLGITEMTEMPLATE);
    *(WORD*)(buf + off) = 0xFFFF; off += 2;
    *(WORD*)(buf + off) = 0x0081; off += 2;
    *(WCHAR*)(buf + off) = 0; off += 2;
    *(WORD*)(buf + off) = 0; off += 2;

    /* OK button */
    off = (off + 3) & ~3;
    it = (DLGITEMTEMPLATE*)(buf + off);
    it->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
    it->x = 356; it->y = 280; it->cx = 56; it->cy = 14;
    it->id = IDOK;
    off += sizeof(DLGITEMTEMPLATE);
    *(WORD*)(buf + off) = 0xFFFF; off += 2;
    *(WORD*)(buf + off) = 0x0080; off += 2;
    t = L"Close"; tl = wcslen(t) + 1;
    memcpy(buf + off, t, tl * sizeof(WCHAR)); off += (int)(tl * sizeof(WCHAR));
    *(WORD*)(buf + off) = 0; off += 2;

    return off;
}

/* Convert UTF-8 to wide, fix line endings, show in output dialog */
static void ShowGitOutput(HWND hwndParent, const WCHAR *title,
                           const char *utf8)
{
    WCHAR *wstr = NULL;
    BYTE buf[2048];
    GitDlgData data;

    if (utf8 && utf8[0]) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
        WCHAR *raw = (WCHAR*)n2e_Alloc(wlen * sizeof(WCHAR));
        if (raw) {
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, raw, wlen);
            wstr = FixLineEndings(raw);
            n2e_Free(raw);
        }
    }

    BuildOutputDlgTemplate(buf, sizeof(buf));

    data.title = title;
    data.text = wstr ? wstr : L"(no output)";

    DialogBoxIndirectParamW(
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE),
        (DLGTEMPLATE*)buf, hwndParent,
        GitOutputDlgProc, (LPARAM)&data);

    if (wstr) n2e_Free(wstr);
}

//=============================================================================
// Commit dialog proc
//=============================================================================

static INT_PTR CALLBACK GitCommitDlgProc(HWND hwnd, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG: {
        GitCommitData *d = (GitCommitData*)lParam;
        HWND hStatus, hMsg;
        HFONT hUI, hMono;

        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        SetWindowTextW(hwnd, L"Bikode \u2014 Git Commit");

        hUI = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        hMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        if (!hMono) hMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)hUI);

        hStatus = GetDlgItem(hwnd, IDC_GIT_EDIT);
        hMsg    = GetDlgItem(hwnd, IDC_GIT_COMMIT_MSG);
        if (hStatus) {
            SendMessage(hStatus, WM_SETFONT, (WPARAM)hMono, TRUE);
            if (d->statusText) SetWindowTextW(hStatus, d->statusText);
        }
        if (hMsg) SendMessage(hMsg, WM_SETFONT, (WPARAM)hUI, TRUE);

        InitDlgBrushes();
        SetDarkTitleBar(hwnd);
        CenterOnParent(hwnd);
        if (hMsg) SetFocus(hMsg);
        return FALSE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC: {
        BOOL dk = DarkMode_IsEnabled();
        SetTextColor((HDC)wParam, dk ? GIT_DLG_DK_TEXT : GIT_DLG_LT_TEXT);
        SetBkColor  ((HDC)wParam, dk ? GIT_DLG_DK_BG   : GIT_DLG_LT_BG);
        return (INT_PTR)s_dlgBgBrush;
    }

    case WM_CTLCOLOREDIT: {
        BOOL dk = DarkMode_IsEnabled();
        SetTextColor((HDC)wParam, dk ? GIT_DLG_DK_TEXT : GIT_DLG_LT_TEXT);
        SetBkColor  ((HDC)wParam, dk ? GIT_DLG_DK_EDIT : GIT_DLG_LT_EDIT);
        return (INT_PTR)s_dlgEditBrush;
    }

    case WM_SIZE: {
        int cx = LOWORD(lParam), cy = HIWORD(lParam);
        HWND hLabel1 = GetDlgItem(hwnd, 1010);
        HWND hMsg    = GetDlgItem(hwnd, IDC_GIT_COMMIT_MSG);
        HWND hLabel2 = GetDlgItem(hwnd, 1011);
        HWND hStatus = GetDlgItem(hwnd, IDC_GIT_EDIT);
        HWND hOK     = GetDlgItem(hwnd, IDOK);
        HWND hCancel = GetDlgItem(hwnd, IDCANCEL);
        int y = 8;
        int statusH;

        if (hLabel1) { MoveWindow(hLabel1, 8, y, cx - 16, 16, TRUE); y += 18; }
        if (hMsg)    { MoveWindow(hMsg, 8, y, cx - 16, 60, TRUE); y += 66; }
        if (hLabel2) { MoveWindow(hLabel2, 8, y, cx - 16, 16, TRUE); y += 18; }
        statusH = cy - y - 40;
        if (statusH < 40) statusH = 40;
        if (hStatus) { MoveWindow(hStatus, 8, y, cx - 16, statusH, TRUE); y += statusH + 6; }
        if (hCancel) MoveWindow(hCancel, cx - 88,  y, 80, 26, TRUE);
        if (hOK)     MoveWindow(hOK,     cx - 176, y, 80, 26, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            GitCommitData *d = (GitCommitData*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            HWND hMsg = GetDlgItem(hwnd, IDC_GIT_COMMIT_MSG);
            HFONT h;
            if (hMsg && d) {
                GetWindowTextW(hMsg, d->commitMsg, _countof(d->commitMsg));
                if (d->commitMsg[0]) d->committed = TRUE;
            }
            h = (HFONT)GetWindowLongPtr(hwnd, DWLP_USER);
            if (h) DeleteObject(h);
            FreeDlgBrushes();
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            HFONT h = (HFONT)GetWindowLongPtr(hwnd, DWLP_USER);
            if (h) DeleteObject(h);
            FreeDlgBrushes();
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 400;
        mmi->ptMinTrackSize.y = 350;
        return 0;
    }
    }
    return FALSE;
}

//=============================================================================
// Commit dialog template builder + launcher
//=============================================================================

/* helper macro for building dialog item templates */
#define GIT_ADD_ITEM(buf, poff, id_, style_, x_, y_, cx_, cy_, classHi_, title_) \
do { \
    const WCHAR *_t; size_t _tl; DLGITEMTEMPLATE *_it; \
    *(poff) = (*(poff) + 3) & ~3; \
    _it = (DLGITEMTEMPLATE*)((buf) + *(poff)); \
    _it->style = (style_); _it->dwExtendedStyle = 0; \
    _it->x = (x_); _it->y = (y_); _it->cx = (cx_); _it->cy = (cy_); \
    _it->id = (id_); \
    *(poff) += sizeof(DLGITEMTEMPLATE); \
    *(WORD*)((buf) + *(poff)) = 0xFFFF; *(poff) += 2; \
    *(WORD*)((buf) + *(poff)) = (classHi_); *(poff) += 2; \
    _t = (title_); _tl = wcslen(_t) + 1; \
    memcpy((buf) + *(poff), _t, _tl * sizeof(WCHAR)); *(poff) += (int)(_tl * sizeof(WCHAR)); \
    *(WORD*)((buf) + *(poff)) = 0; *(poff) += 2; \
} while(0)

static void ShowCommitDialogImpl(HWND hwndParent)
{
    char *pszStatus = NULL;
    int exitCode = 0;
    WCHAR *wszStatus = NULL;
    BYTE buf[4096];
    int off = 0;
    DLGTEMPLATE *dt;
    const WCHAR *fn;
    size_t fl;
    GitCommitData cdata;
    INT_PTR result;

    GitUI_RunCommand(L"status --short", &pszStatus, &exitCode);

    if (pszStatus && pszStatus[0]) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pszStatus, -1, NULL, 0);
        WCHAR *raw = (WCHAR*)n2e_Alloc(wlen * sizeof(WCHAR));
        if (raw) {
            MultiByteToWideChar(CP_UTF8, 0, pszStatus, -1, raw, wlen);
            wszStatus = FixLineEndings(raw);
            n2e_Free(raw);
        }
    }
    if (pszStatus) n2e_Free(pszStatus);

    ZeroMemory(buf, sizeof(buf));

    dt = (DLGTEMPLATE*)(buf + off);
    dt->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
                WS_VISIBLE | DS_MODALFRAME | DS_SETFONT;
    dt->cdit = 6;
    dt->x = 0; dt->y = 0; dt->cx = 400; dt->cy = 320;
    off += sizeof(DLGTEMPLATE);

    *(WORD*)(buf + off) = 0; off += 2;
    *(WORD*)(buf + off) = 0; off += 2;
    *(WCHAR*)(buf + off) = 0; off += 2;
    *(WORD*)(buf + off) = 9; off += 2;
    fn = L"Segoe UI"; fl = wcslen(fn) + 1;
    memcpy(buf + off, fn, fl * sizeof(WCHAR)); off += (int)(fl * sizeof(WCHAR));

    GIT_ADD_ITEM(buf, &off, 1010,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        8, 8, 380, 12, 0x0082, L"Commit message:");

    GIT_ADD_ITEM(buf, &off, IDC_GIT_COMMIT_MSG,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | WS_VSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        8, 22, 380, 50, 0x0081, L"");

    GIT_ADD_ITEM(buf, &off, 1011,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        8, 78, 380, 12, 0x0082, L"Changed files:");

    GIT_ADD_ITEM(buf, &off, IDC_GIT_EDIT,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        8, 92, 380, 188, 0x0081, L"");

    GIT_ADD_ITEM(buf, &off, IDOK,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        268, 290, 56, 14, 0x0080, L"Commit");

    GIT_ADD_ITEM(buf, &off, IDCANCEL,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        332, 290, 56, 14, 0x0080, L"Cancel");

    ZeroMemory(&cdata, sizeof(cdata));
    cdata.statusText = wszStatus;

    result = DialogBoxIndirectParamW(
        (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE),
        (DLGTEMPLATE*)buf, hwndParent,
        GitCommitDlgProc, (LPARAM)&cdata);

    if (wszStatus) n2e_Free(wszStatus);

    if (result == IDOK && cdata.committed && cdata.commitMsg[0]) {
        char msg8[4096];
        int ex2 = 0;
        char *pOut = NULL;
        WideCharToMultiByte(CP_UTF8, 0, cdata.commitMsg, -1,
                            msg8, sizeof(msg8), NULL, NULL);
        GitUI_Commit(msg8);
        GitUI_Refresh();

        GitUI_RunCommand(L"log --oneline -1", &pOut, &ex2);
        if (pOut && pOut[0]) {
            int wl = MultiByteToWideChar(CP_UTF8, 0, pOut, -1, NULL, 0);
            WCHAR *ws = (WCHAR*)n2e_Alloc(wl * sizeof(WCHAR));
            if (ws) {
                WCHAR finalMsg[512];
                MultiByteToWideChar(CP_UTF8, 0, pOut, -1, ws, wl);
                _snwprintf_s(finalMsg, _countof(finalMsg), _TRUNCATE,
                    L"Committed successfully:\r\n\r\n%s", ws);
                MessageBoxW(hwndParent, finalMsg,
                    L"Bikode \u2014 Git Commit", MB_OK | MB_ICONINFORMATION);
                n2e_Free(ws);
            }
        }
        if (pOut) n2e_Free(pOut);
    }
}

//=============================================================================
// Public: UI dialogs
//=============================================================================

void GitUI_ShowStatus(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        char *out = NULL; int ex = 0;
        GitUI_RunCommand(L"status", &out, &ex);
        ShowGitOutput(hwndParent, L"Bikode \u2014 Git Status", out);
        if (out) n2e_Free(out);
    }
}

void GitUI_ShowDiff(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        WCHAR args[MAX_PATH + 32];
        char *out = NULL; int ex = 0;
        _snwprintf_s(args, _countof(args), _TRUNCATE,
                     L"diff -- \"%s\"", szCurFile);
        GitUI_RunCommand(args, &out, &ex);
        if (out && out[0])
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Diff", out);
        else
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Diff",
                           "No changes detected.");
        if (out) n2e_Free(out);
    }
}

void GitUI_ShowCommitDialog(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShowCommitDialogImpl(hwndParent);
}

void GitUI_ShowLog(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        char *out = NULL; int ex = 0;
        GitUI_RunCommand(L"log --oneline -40 --decorate", &out, &ex);
        ShowGitOutput(hwndParent, L"Bikode \u2014 Git Log (last 40)", out);
        if (out) n2e_Free(out);
    }
}

//=============================================================================
// Public: Extra UI dialogs
//=============================================================================

void GitUI_ShowBlame(HWND hwndParent)
{
    if (!s_bInRepo || !szCurFile[0]) {
        MessageBoxW(hwndParent, L"Not in a git repository or no file open.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        WCHAR args[MAX_PATH + 32];
        char *out = NULL; int ex = 0;
        _snwprintf_s(args, _countof(args), _TRUNCATE,
                     L"blame \"%s\"", szCurFile);
        GitUI_RunCommand(args, &out, &ex);
        ShowGitOutput(hwndParent, L"Bikode \u2014 Git Blame", out);
        if (out) n2e_Free(out);
    }
}

void GitUI_ShowBranches(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        char *out = NULL; int ex = 0;
        GitUI_RunCommand(L"branch -a --no-color", &out, &ex);
        ShowGitOutput(hwndParent, L"Bikode \u2014 Git Branches", out);
        if (out) n2e_Free(out);
    }
}

void GitUI_ShowStash(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        char *out = NULL; int ex = 0;
        GitUI_RunCommand(L"stash list", &out, &ex);
        if (out && out[0])
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Stash", out);
        else
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Stash",
                           "No stashes found.");
        if (out) n2e_Free(out);
    }
}

void GitUI_PullWithUI(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        char *out = NULL; int ex = 0;
        GitUI_RunCommand(L"pull", &out, &ex);
        GitUI_Refresh();
        if (ex == 0)
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Pull",
                           out && out[0] ? out : "Already up to date.");
        else
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Pull (failed)", out);
        if (out) n2e_Free(out);
    }
}

void GitUI_PushWithUI(HWND hwndParent)
{
    if (!s_bInRepo) {
        MessageBoxW(hwndParent, L"Not in a git repository.",
                     L"Bikode \u2014 Git", MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        char *out = NULL; int ex = 0;
        GitUI_RunCommand(L"push", &out, &ex);
        if (ex == 0)
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Push",
                           out && out[0] ? out : "Pushed successfully.");
        else
            ShowGitOutput(hwndParent, L"Bikode \u2014 Git Push (failed)", out);
        if (out) n2e_Free(out);
    }
}

//=============================================================================
// Public: Panel
//=============================================================================

void GitUI_TogglePanel(HWND hwndParent)
{
    s_bPanelVisible = !s_bPanelVisible;
    if (s_bPanelVisible)
        GitUI_ShowStatus(hwndParent);
    else
        s_bPanelVisible = FALSE;
}

BOOL GitUI_IsPanelVisible(void)
{
    return s_bPanelVisible;
}

//=============================================================================
// Public: Status bar
//=============================================================================

void GitUI_UpdateStatusBar(HWND hwndStatus, int iPart)
{
    if (!hwndStatus || !s_bInRepo) return;

    {
        WCHAR wszText[256];
        _snwprintf_s(wszText, _countof(wszText), _TRUNCATE,
            L"\xE0A0 %s  %s",
            s_wszBranch, s_wszStatusSummary);
        SendMessageW(hwndStatus, SB_SETTEXTW, iPart, (LPARAM)wszText);
    }
}

//=============================================================================
// Public: Operations
//=============================================================================

BOOL GitUI_StageFile(const WCHAR* pszFile)
{
    if (!s_bInRepo || !pszFile) return FALSE;
    {
        WCHAR args[MAX_PATH + 32];
        int ex = 0;
        BOOL ok;
        _snwprintf_s(args, _countof(args), _TRUNCATE, L"add \"%s\"", pszFile);
        ok = GitUI_RunCommand(args, NULL, &ex) && ex == 0;
        if (ok) GitUI_Refresh();
        return ok;
    }
}

BOOL GitUI_UnstageFile(const WCHAR* pszFile)
{
    if (!s_bInRepo || !pszFile) return FALSE;
    {
        WCHAR args[MAX_PATH + 32];
        int ex = 0;
        BOOL ok;
        _snwprintf_s(args, _countof(args), _TRUNCATE,
                     L"reset HEAD \"%s\"", pszFile);
        ok = GitUI_RunCommand(args, NULL, &ex) && ex == 0;
        if (ok) GitUI_Refresh();
        return ok;
    }
}

BOOL GitUI_Commit(const char* pszMessage)
{
    if (!s_bInRepo || !pszMessage) return FALSE;
    {
        int ex = 0;
        WCHAR wszMsg[1024];
        WCHAR args[1200];
        GitUI_RunCommand(L"add -A", NULL, &ex);
        MultiByteToWideChar(CP_UTF8, 0, pszMessage, -1,
                            wszMsg, _countof(wszMsg));
        _snwprintf_s(args, _countof(args), _TRUNCATE,
                     L"commit -m \"%s\"", wszMsg);
        return GitUI_RunCommand(args, NULL, &ex) && ex == 0;
    }
}

BOOL GitUI_Pull(void)
{
    if (!s_bInRepo) return FALSE;
    {
        int ex = 0;
        BOOL ok = GitUI_RunCommand(L"pull", NULL, &ex) && ex == 0;
        if (ok) GitUI_Refresh();
        return ok;
    }
}

BOOL GitUI_Push(void)
{
    if (!s_bInRepo) return FALSE;
    {
        int ex = 0;
        return GitUI_RunCommand(L"push", NULL, &ex) && ex == 0;
    }
}

BOOL GitUI_RunCommand(const WCHAR* pszArgs, char** ppszOutput, int* piExitCode)
{
    WCHAR wszCmd[4096];
    if (s_wszGitExe[0] == L'\0') return FALSE;

    _snwprintf_s(wszCmd, _countof(wszCmd), _TRUNCATE,
                 L"\"%s\" %s", s_wszGitExe, pszArgs);

    return RunProcess(wszCmd, FileManager_GetRootPath(),
                      ppszOutput, piExitCode);
}

//=============================================================================
// Internal: Find git.exe
//=============================================================================

static BOOL FindGitExe(void)
{
    WCHAR wszPath[MAX_PATH];
    DWORD dwLen = SearchPathW(NULL, L"git.exe", NULL, MAX_PATH, wszPath, NULL);
    if (dwLen > 0)
    {
        wcscpy_s(s_wszGitExe, _countof(s_wszGitExe), wszPath);
        return TRUE;
    }

    {
        static const WCHAR* candidates[] = {
            L"C:\\Program Files\\Git\\bin\\git.exe",
            L"C:\\Program Files (x86)\\Git\\bin\\git.exe",
            L"C:\\Program Files\\Git\\cmd\\git.exe",
        };
        int i;
        for (i = 0; i < _countof(candidates); i++)
        {
            if (GetFileAttributesW(candidates[i]) != INVALID_FILE_ATTRIBUTES)
            {
                wcscpy_s(s_wszGitExe, _countof(s_wszGitExe), candidates[i]);
                return TRUE;
            }
        }
    }

    return FALSE;
}

//=============================================================================
// Internal: Detect repo root
//=============================================================================

static BOOL DetectRepoRoot(const WCHAR* pszFile)
{
    WCHAR wszDir[MAX_PATH];
    WCHAR* pSlash;

    s_bInRepo = FALSE;
    s_wszRepoRoot[0] = L'\0';

    if (!pszFile || !pszFile[0]) return FALSE;

    wcscpy_s(wszDir, _countof(wszDir), pszFile);

    pSlash = wcsrchr(wszDir, L'\\');
    if (!pSlash) pSlash = wcsrchr(wszDir, L'/');
    if (pSlash) *pSlash = L'\0';
    else return FALSE;

    while (wszDir[0])
    {
        WCHAR wszGitDir[MAX_PATH];
        _snwprintf_s(wszGitDir, _countof(wszGitDir), _TRUNCATE,
                     L"%s\\.git", wszDir);

        if (GetFileAttributesW(wszGitDir) != INVALID_FILE_ATTRIBUTES)
        {
            wcscpy_s(s_wszRepoRoot, _countof(s_wszRepoRoot), wszDir);
            s_bInRepo = TRUE;
            return TRUE;
        }

        pSlash = wcsrchr(wszDir, L'\\');
        if (!pSlash) pSlash = wcsrchr(wszDir, L'/');
        if (!pSlash || pSlash == wszDir) break;
        if (pSlash == wszDir + 2 && wszDir[1] == L':') break;
        *pSlash = L'\0';
    }

    return FALSE;
}

//=============================================================================
// Internal: Update branch
//=============================================================================

static void UpdateBranch(void)
{
    char* pszOutput = NULL;
    int exitCode = 0;

    s_wszBranch[0] = L'\0';
    if (!s_bInRepo) return;

    if (GitUI_RunCommand(L"rev-parse --abbrev-ref HEAD",
                          &pszOutput, &exitCode) && exitCode == 0)
    {
        if (pszOutput)
        {
            int len = (int)strlen(pszOutput);
            while (len > 0 && (pszOutput[len-1] == '\n' ||
                               pszOutput[len-1] == '\r'))
                pszOutput[--len] = '\0';

            MultiByteToWideChar(CP_UTF8, 0, pszOutput, -1,
                s_wszBranch, _countof(s_wszBranch));
        }
    }

    if (pszOutput) n2e_Free(pszOutput);
}

//=============================================================================
// Internal: Update status summary
//=============================================================================

static void UpdateStatusSummary(void)
{
    char* pszOutput = NULL;
    int exitCode = 0;

    s_wszStatusSummary[0] = L'\0';
    if (!s_bInRepo) return;

    if (GitUI_RunCommand(L"status --porcelain",
                          &pszOutput, &exitCode) && exitCode == 0)
    {
        if (pszOutput)
        {
            int added = 0, modified = 0, deleted = 0;
            const char* p = pszOutput;
            while (*p)
            {
                if (p[0] == '?' && p[1] == '?') added++;
                else if (p[0] == 'A' || p[1] == 'A') added++;
                else if (p[0] == 'M' || p[1] == 'M') modified++;
                else if (p[0] == 'D' || p[1] == 'D') deleted++;

                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
            }

            _snwprintf_s(s_wszStatusSummary,
                _countof(s_wszStatusSummary), _TRUNCATE,
                L"+%d ~%d -%d", added, modified, deleted);
        }
    }

    if (pszOutput) n2e_Free(pszOutput);
}
