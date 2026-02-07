/******************************************************************************
*
* Biko
*
* GitUI.c
*   Git integration via git.exe subprocess.
*
******************************************************************************/

#include "GitUI.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include <string.h>
#include <stdio.h>
#include <commctrl.h>

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

// External: szCurFile from Notepad2
extern WCHAR szCurFile[MAX_PATH + 40];

//=============================================================================
// Forward declarations
//=============================================================================

static BOOL FindGitExe(void);
static BOOL DetectRepoRoot(const WCHAR* pszFile);
static BOOL RunGitRaw(const WCHAR* pszArgs, char** ppOutput, int* piExit);
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

    // CreateProcess needs a writable command line buffer
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

    // Read all output
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

    if (szCurFile[0])
    {
        DetectRepoRoot(szCurFile);
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
    if (szCurFile[0])
    {
        DetectRepoRoot(szCurFile);
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
// Public: UI dialogs
//=============================================================================

void GitUI_ShowStatus(HWND hwndParent)
{
    if (!s_bInRepo)
    {
        MessageBoxW(hwndParent, L"Not in a git repository.", L"Biko â€” Git", MB_OK | MB_ICONINFORMATION);
        return;
    }

    char* pszOutput = NULL;
    int exitCode = 0;
    GitUI_RunCommand(L"status", &pszOutput, &exitCode);

    if (pszOutput)
    {
        // Convert to wide for display
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pszOutput, -1, NULL, 0);
        WCHAR* wszOutput = (WCHAR*)n2e_Alloc(wlen * sizeof(WCHAR));
        if (wszOutput)
        {
            MultiByteToWideChar(CP_UTF8, 0, pszOutput, -1, wszOutput, wlen);
            MessageBoxW(hwndParent, wszOutput, L"Biko â€” Git Status", MB_OK);
            n2e_Free(wszOutput);
        }
        n2e_Free(pszOutput);
    }
}

void GitUI_ShowDiff(HWND hwndParent)
{
    if (!s_bInRepo)
    {
        MessageBoxW(hwndParent, L"Not in a git repository.", L"Biko â€” Git", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Diff the current file
    WCHAR wszArgs[MAX_PATH + 32];
    _snwprintf_s(wszArgs, _countof(wszArgs), _TRUNCATE, L"diff -- \"%s\"", szCurFile);

    char* pszOutput = NULL;
    int exitCode = 0;
    GitUI_RunCommand(wszArgs, &pszOutput, &exitCode);

    if (pszOutput && pszOutput[0])
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pszOutput, -1, NULL, 0);
        WCHAR* wszOutput = (WCHAR*)n2e_Alloc(wlen * sizeof(WCHAR));
        if (wszOutput)
        {
            MultiByteToWideChar(CP_UTF8, 0, pszOutput, -1, wszOutput, wlen);
            MessageBoxW(hwndParent, wszOutput, L"Biko â€” Git Diff", MB_OK);
            n2e_Free(wszOutput);
        }
    }
    else
    {
        MessageBoxW(hwndParent, L"No changes detected.", L"Biko â€” Git Diff", MB_OK | MB_ICONINFORMATION);
    }

    if (pszOutput) n2e_Free(pszOutput);
}

void GitUI_ShowCommitDialog(HWND hwndParent)
{
    if (!s_bInRepo)
    {
        MessageBoxW(hwndParent, L"Not in a git repository.", L"Biko â€” Git", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Simple input dialog for commit message
    // We'll build a dialog from a template in memory

    // For now, use a simple InputBox-style approach
    WCHAR wszMsg[1024] = L"";

    // Build a minimal dialog template
    #pragma pack(push, 4)
    struct {
        DLGTEMPLATE dt;
        WORD menu, cls, title;
        // Static label
        WORD align1;
        DLGITEMTEMPLATE ditLabel;
        WORD classLabel[2];
        WCHAR textLabel[16];
        WORD extra1;
        // Edit control
        WORD align2;
        DLGITEMTEMPLATE ditEdit;
        WORD classEdit[2];
        WCHAR textEdit[1];
        WORD extra2;
        // OK button
        WORD align3;
        DLGITEMTEMPLATE ditOK;
        WORD classOK[2];
        WCHAR textOK[7];
        WORD extra3;
        // Cancel button
        WORD align4;
        DLGITEMTEMPLATE ditCancel;
        WORD classCancel[2];
        WCHAR textCancel[7];
        WORD extra4;
    } dlg;
    #pragma pack(pop)

    ZeroMemory(&dlg, sizeof(dlg));

    dlg.dt.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT;
    dlg.dt.cdit = 4;
    dlg.dt.cx = 300;
    dlg.dt.cy = 100;

    // Label
    dlg.ditLabel.style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    dlg.ditLabel.x = 8; dlg.ditLabel.y = 8;
    dlg.ditLabel.cx = 280; dlg.ditLabel.cy = 12;
    dlg.ditLabel.id = 0xFFFF;
    dlg.classLabel[0] = 0xFFFF; dlg.classLabel[1] = 0x0082; // static
    wcscpy_s(dlg.textLabel, _countof(dlg.textLabel), L"Commit message:");

    // Edit
    dlg.ditEdit.style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    dlg.ditEdit.x = 8; dlg.ditEdit.y = 24;
    dlg.ditEdit.cx = 280; dlg.ditEdit.cy = 40;
    dlg.ditEdit.id = 100;
    dlg.classEdit[0] = 0xFFFF; dlg.classEdit[1] = 0x0081; // edit

    // OK
    dlg.ditOK.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON;
    dlg.ditOK.x = 168; dlg.ditOK.y = 74;
    dlg.ditOK.cx = 56; dlg.ditOK.cy = 16;
    dlg.ditOK.id = IDOK;
    dlg.classOK[0] = 0xFFFF; dlg.classOK[1] = 0x0080; // button
    wcscpy_s(dlg.textOK, _countof(dlg.textOK), L"Commit");

    // Cancel
    dlg.ditCancel.style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    dlg.ditCancel.x = 232; dlg.ditCancel.y = 74;
    dlg.ditCancel.cx = 56; dlg.ditCancel.cy = 16;
    dlg.ditCancel.id = IDCANCEL;
    dlg.classCancel[0] = 0xFFFF; dlg.classCancel[1] = 0x0080;
    wcscpy_s(dlg.textCancel, _countof(dlg.textCancel), L"Cancel");

    // For simplicity, just use a basic message box to collect input
    // (A proper implementation would use the template above)
    // TODO: Replace with proper dialog
    int result = MessageBoxW(hwndParent,
        L"Stage all changes and commit?\n\n(Full commit dialog coming soon...)",
        L"Biko â€” Git Commit", MB_OKCANCEL | MB_ICONQUESTION);

    if (result == IDOK)
    {
        // Auto-generate commit message or use a placeholder
        GitUI_Commit("Auto-commit from Biko");
        GitUI_Refresh();

        MessageBoxW(hwndParent, L"Committed.", L"Biko â€” Git", MB_OK | MB_ICONINFORMATION);
    }
}

void GitUI_ShowLog(HWND hwndParent)
{
    if (!s_bInRepo)
    {
        MessageBoxW(hwndParent, L"Not in a git repository.", L"Biko â€” Git", MB_OK | MB_ICONINFORMATION);
        return;
    }

    char* pszOutput = NULL;
    int exitCode = 0;
    GitUI_RunCommand(L"log --oneline -20", &pszOutput, &exitCode);

    if (pszOutput)
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, pszOutput, -1, NULL, 0);
        WCHAR* wszOutput = (WCHAR*)n2e_Alloc(wlen * sizeof(WCHAR));
        if (wszOutput)
        {
            MultiByteToWideChar(CP_UTF8, 0, pszOutput, -1, wszOutput, wlen);
            MessageBoxW(hwndParent, wszOutput, L"Biko â€” Git Log (last 20)", MB_OK);
            n2e_Free(wszOutput);
        }
        n2e_Free(pszOutput);
    }
}

//=============================================================================
// Public: Panel
//=============================================================================

void GitUI_TogglePanel(HWND hwndParent)
{
    // TODO: Implement a proper dockable git status panel
    // For now, show/hide the status dialog
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

    WCHAR wszText[256];
    _snwprintf_s(wszText, _countof(wszText), _TRUNCATE,
        L"\xE0A0 %s  %s",  // git branch icon, branch name, status
        s_wszBranch,
        s_wszStatusSummary);

    SendMessageW(hwndStatus, SB_SETTEXTW, iPart, (LPARAM)wszText);
}

//=============================================================================
// Public: Operations
//=============================================================================

BOOL GitUI_StageFile(const WCHAR* pszFile)
{
    if (!s_bInRepo || !pszFile) return FALSE;

    WCHAR wszArgs[MAX_PATH + 32];
    _snwprintf_s(wszArgs, _countof(wszArgs), _TRUNCATE, L"add \"%s\"", pszFile);

    int exitCode = 0;
    return GitUI_RunCommand(wszArgs, NULL, &exitCode) && exitCode == 0;
}

BOOL GitUI_UnstageFile(const WCHAR* pszFile)
{
    if (!s_bInRepo || !pszFile) return FALSE;

    WCHAR wszArgs[MAX_PATH + 32];
    _snwprintf_s(wszArgs, _countof(wszArgs), _TRUNCATE, L"reset HEAD \"%s\"", pszFile);

    int exitCode = 0;
    return GitUI_RunCommand(wszArgs, NULL, &exitCode) && exitCode == 0;
}

BOOL GitUI_Commit(const char* pszMessage)
{
    if (!s_bInRepo || !pszMessage) return FALSE;

    // Stage all first
    int exitCode = 0;
    GitUI_RunCommand(L"add -A", NULL, &exitCode);

    // Then commit
    WCHAR wszMsg[1024];
    MultiByteToWideChar(CP_UTF8, 0, pszMessage, -1, wszMsg, _countof(wszMsg));

    WCHAR wszArgs[1200];
    _snwprintf_s(wszArgs, _countof(wszArgs), _TRUNCATE, L"commit -m \"%s\"", wszMsg);

    return GitUI_RunCommand(wszArgs, NULL, &exitCode) && exitCode == 0;
}

BOOL GitUI_RunCommand(const WCHAR* pszArgs, char** ppszOutput, int* piExitCode)
{
    if (s_wszGitExe[0] == L'\0') return FALSE;

    WCHAR wszCmd[4096];
    _snwprintf_s(wszCmd, _countof(wszCmd), _TRUNCATE, L"\"%s\" %s", s_wszGitExe, pszArgs);

    return RunProcess(wszCmd, s_wszRepoRoot[0] ? s_wszRepoRoot : NULL,
                      ppszOutput, piExitCode);
}

//=============================================================================
// Internal: Find git.exe
//=============================================================================

static BOOL FindGitExe(void)
{
    // Try PATH first
    WCHAR wszPath[MAX_PATH];
    DWORD dwLen = SearchPathW(NULL, L"git.exe", NULL, MAX_PATH, wszPath, NULL);
    if (dwLen > 0)
    {
        wcscpy_s(s_wszGitExe, _countof(s_wszGitExe), wszPath);
        return TRUE;
    }

    // Try common installation paths
    static const WCHAR* candidates[] = {
        L"C:\\Program Files\\Git\\bin\\git.exe",
        L"C:\\Program Files (x86)\\Git\\bin\\git.exe",
        L"C:\\Program Files\\Git\\cmd\\git.exe",
    };

    for (int i = 0; i < _countof(candidates); i++)
    {
        if (GetFileAttributesW(candidates[i]) != INVALID_FILE_ATTRIBUTES)
        {
            wcscpy_s(s_wszGitExe, _countof(s_wszGitExe), candidates[i]);
            return TRUE;
        }
    }

    return FALSE;
}

//=============================================================================
// Internal: Detect repo root
//=============================================================================

static BOOL DetectRepoRoot(const WCHAR* pszFile)
{
    s_bInRepo = FALSE;
    s_wszRepoRoot[0] = L'\0';

    if (!pszFile || !pszFile[0]) return FALSE;

    // Walk up from the file's directory looking for .git
    WCHAR wszDir[MAX_PATH];
    wcscpy_s(wszDir, _countof(wszDir), pszFile);

    // Strip filename
    WCHAR* pSlash = wcsrchr(wszDir, L'\\');
    if (!pSlash) pSlash = wcsrchr(wszDir, L'/');
    if (pSlash) *pSlash = L'\0';
    else return FALSE;

    while (wszDir[0])
    {
        WCHAR wszGitDir[MAX_PATH];
        _snwprintf_s(wszGitDir, _countof(wszGitDir), _TRUNCATE, L"%s\\.git", wszDir);

        if (GetFileAttributesW(wszGitDir) != INVALID_FILE_ATTRIBUTES)
        {
            wcscpy_s(s_wszRepoRoot, _countof(s_wszRepoRoot), wszDir);
            s_bInRepo = TRUE;
            return TRUE;
        }

        // Go up one level
        pSlash = wcsrchr(wszDir, L'\\');
        if (!pSlash) pSlash = wcsrchr(wszDir, L'/');
        if (!pSlash || pSlash == wszDir) break;

        // Don't go above drive root (e.g. "C:")
        if (pSlash == wszDir + 2 && wszDir[1] == L':')
            break;

        *pSlash = L'\0';
    }

    return FALSE;
}

//=============================================================================
// Internal: Update branch
//=============================================================================

static void UpdateBranch(void)
{
    s_wszBranch[0] = L'\0';
    if (!s_bInRepo) return;

    char* pszOutput = NULL;
    int exitCode = 0;

    if (GitUI_RunCommand(L"rev-parse --abbrev-ref HEAD", &pszOutput, &exitCode) && exitCode == 0)
    {
        if (pszOutput)
        {
            // Strip trailing newline
            int len = (int)strlen(pszOutput);
            while (len > 0 && (pszOutput[len-1] == '\n' || pszOutput[len-1] == '\r'))
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
    s_wszStatusSummary[0] = L'\0';
    if (!s_bInRepo) return;

    char* pszOutput = NULL;
    int exitCode = 0;

    if (GitUI_RunCommand(L"status --porcelain", &pszOutput, &exitCode) && exitCode == 0)
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

                // Skip to next line
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
            }

            _snwprintf_s(s_wszStatusSummary, _countof(s_wszStatusSummary), _TRUNCATE,
                L"+%d ~%d -%d", added, modified, deleted);
        }
    }

    if (pszOutput) n2e_Free(pszOutput);
}
