/******************************************************************************
*
* Biko
*
* Terminal.c
*   World-class embedded terminal with ConPTY and shell selector.
*   Supports PowerShell, CMD, Git Bash, and WSL Bash.
*   Uses a Scintilla control as the terminal viewport.
*
******************************************************************************/

#include "Terminal.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include <uxtheme.h>
#include <shlwapi.h>
#include "SciCall.h"
#include "Scintilla.h"
#include <string.h>
#include <stdio.h>

//=============================================================================
// ConPTY types (Windows 10 1809+)
//=============================================================================

typedef VOID* HPCON;

typedef HRESULT (WINAPI *pfnCreatePseudoConsole)(
    COORD size, HANDLE hInput, HANDLE hOutput, DWORD dwFlags, HPCON* phPC);
typedef HRESULT (WINAPI *pfnResizePseudoConsole)(HPCON hPC, COORD size);
typedef void    (WINAPI *pfnClosePseudoConsole)(HPCON hPC);

static pfnCreatePseudoConsole    s_pfnCreatePC = NULL;
static pfnResizePseudoConsole    s_pfnResizePC = NULL;
static pfnClosePseudoConsole     s_pfnClosePC = NULL;
static BOOL                      s_bConPTYAvailable = FALSE;

//=============================================================================
// Design constants
//=============================================================================

#define TERMINAL_HEIGHT         220
#define TERMINAL_SPLITTER       3
#define TERMINAL_HEADER_HEIGHT  32

// Dark mode colors (matching ChatPanel)
#define TRM_DK_SURFACE      RGB(18, 18, 18)
#define TRM_DK_HEADER       RGB(25, 25, 28)
#define TRM_DK_SPLITTER     RGB(45, 45, 50)
#define TRM_DK_TEXT         RGB(204, 204, 204)
#define TRM_DK_TEXT_DIM     RGB(120, 120, 128)
#define TRM_DK_ACCENT       RGB(99, 102, 241)
#define TRM_DK_DROPDOWN_BG  RGB(40, 40, 44)
#define TRM_DK_DROPDOWN_HL  RGB(55, 55, 62)
#define TRM_DK_BORDER       RGB(50, 50, 55)
#define TRM_DK_BTN_HOVER    RGB(50, 50, 56)
#define TRM_DK_TERM_BG      RGB(12, 12, 12)

// Light mode colors
#define TRM_LT_SURFACE      RGB(249, 250, 251)
#define TRM_LT_HEADER       RGB(243, 244, 246)
#define TRM_LT_SPLITTER     RGB(229, 231, 235)
#define TRM_LT_TEXT         RGB(17, 24, 39)
#define TRM_LT_TEXT_DIM     RGB(107, 114, 128)
#define TRM_LT_ACCENT       RGB(79, 70, 229)
#define TRM_LT_DROPDOWN_BG  RGB(255, 255, 255)
#define TRM_LT_DROPDOWN_HL  RGB(238, 242, 255)
#define TRM_LT_BORDER       RGB(209, 213, 219)
#define TRM_LT_BTN_HOVER    RGB(229, 231, 235)
#define TRM_LT_TERM_BG      RGB(255, 255, 255)

//=============================================================================
// Shell definitions
//=============================================================================

typedef enum {
    SHELL_POWERSHELL = 0,
    SHELL_CMD,
    SHELL_GIT_BASH,
    SHELL_WSL_BASH,
    SHELL_COUNT
} ShellType;

typedef struct {
    const WCHAR*    wszLabel;       // Display name
    const WCHAR*    wszIcon;        // Unicode icon
    WCHAR           wszPath[MAX_PATH]; // Resolved path (empty = not found)
    BOOL            bAvailable;
} ShellInfo;

static ShellInfo s_shells[SHELL_COUNT] = {
    { L"PowerShell", L"\x26A1", { 0 }, FALSE },
    { L"CMD",        L">_",     { 0 }, FALSE },
    { L"Git Bash",   L"\xE0B0", { 0 }, FALSE },
    { L"Bash (WSL)", L"\x00A7",  { 0 }, FALSE },
};

static ShellType s_activeShell = SHELL_POWERSHELL;

//=============================================================================
// Terminal state
//=============================================================================

typedef struct TerminalInstance {
    HPCON           hPseudoConsole;
    HANDLE          hProcess;
    HANDLE          hThread;
    HANDLE          hPipeIn;        // Write end -> shell stdin
    HANDLE          hPipeOut;       // Read end <- shell stdout
    HANDLE          hInputRead;
    HANDLE          hOutputWrite;
    HANDLE          hReadThread;
    HWND            hwndView;       // Scintilla control
    volatile BOOL   bAlive;
    int             currentStyle;
    ShellType       shellType;
} TerminalInstance;

static HWND                 s_hwndMain = NULL;
static HWND                 s_hwndPanel = NULL;
static TerminalInstance*    s_pTerminal = NULL;
static BOOL                 s_bVisible = FALSE;
static int                  s_iPanelHeight = TERMINAL_HEIGHT;
static WNDPROC              s_pfnOrigViewProc = NULL;
static HFONT                s_hFontHeader = NULL;
static HFONT                s_hFontDropdown = NULL;
static BOOL                 s_bCloseHover = FALSE;
static BOOL                 s_bNewHover = FALSE;

static const WCHAR* TERMINAL_PANEL_CLASS = L"BikoTerminalPanel";
static BOOL s_bClassRegistered = FALSE;

// Header button rects (calculated during paint)
static RECT s_rcDropdown = { 0 };
static RECT s_rcNewBtn = { 0 };
static RECT s_rcCloseBtn = { 0 };

//=============================================================================
// Forward declarations
//=============================================================================

static LRESULT CALLBACK TermPanelWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK TermViewSubclassProc(HWND, UINT, WPARAM, LPARAM);
static DWORD   WINAPI   TerminalReaderThread(LPVOID lpParam);
static BOOL    InitConPTY(void);
static BOOL    CreateTerminalProcess(TerminalInstance* pTerm, COORD size, ShellType shell);
static void    DestroyTerminalInstance(TerminalInstance* pTerm);
static void    SetupTerminalStyles(HWND hwndView);
static void    AppendTerminalOutput(HWND hwndView, const char* data, int len);
static void    DetectAvailableShells(void);
static void    ShowShellMenu(HWND hwnd);
static ShellType GetBestShell(void);

//=============================================================================
// Panel window class
//=============================================================================

static void RegisterTermPanelClass(HINSTANCE hInst)
{
    if (s_bClassRegistered) return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = TermPanelWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = TERMINAL_PANEL_CLASS;

    RegisterClassExW(&wc);
    s_bClassRegistered = TRUE;
}

static void CreateTermFonts(void)
{
    if (!s_hFontHeader)
    {
        s_hFontHeader = CreateFontW(
            -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    if (!s_hFontDropdown)
    {
        s_hFontDropdown = CreateFontW(
            -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
}

//=============================================================================
// Shell detection
//=============================================================================

static void DetectAvailableShells(void)
{
    // PowerShell (prefer pwsh.exe over powershell.exe)
    {
        WCHAR wszPath[MAX_PATH];
        // Try pwsh (PowerShell 7+) first
        if (SearchPathW(NULL, L"pwsh.exe", NULL, MAX_PATH, wszPath, NULL))
        {
            wcscpy_s(s_shells[SHELL_POWERSHELL].wszPath, MAX_PATH, wszPath);
            s_shells[SHELL_POWERSHELL].bAvailable = TRUE;
        }
        else if (SearchPathW(NULL, L"powershell.exe", NULL, MAX_PATH, wszPath, NULL))
        {
            wcscpy_s(s_shells[SHELL_POWERSHELL].wszPath, MAX_PATH, wszPath);
            s_shells[SHELL_POWERSHELL].bAvailable = TRUE;
        }
    }

    // CMD
    {
        WCHAR wszPath[MAX_PATH];
        DWORD dwLen = GetEnvironmentVariableW(L"COMSPEC", wszPath, MAX_PATH);
        if (dwLen > 0 && dwLen < MAX_PATH)
        {
            wcscpy_s(s_shells[SHELL_CMD].wszPath, MAX_PATH, wszPath);
            s_shells[SHELL_CMD].bAvailable = TRUE;
        }
        else if (SearchPathW(NULL, L"cmd.exe", NULL, MAX_PATH, wszPath, NULL))
        {
            wcscpy_s(s_shells[SHELL_CMD].wszPath, MAX_PATH, wszPath);
            s_shells[SHELL_CMD].bAvailable = TRUE;
        }
    }

    // Git Bash
    {
        WCHAR wszPath[MAX_PATH];
        // Check common locations
        const WCHAR* gitBashPaths[] = {
            L"C:\\Program Files\\Git\\bin\\bash.exe",
            L"C:\\Program Files (x86)\\Git\\bin\\bash.exe",
            NULL
        };

        BOOL found = FALSE;
        // Try PATH first
        if (SearchPathW(NULL, L"bash.exe", NULL, MAX_PATH, wszPath, NULL))
        {
            // Verify it's Git Bash (not WSL bash)
            if (wcsstr(wszPath, L"Git") != NULL)
            {
                wcscpy_s(s_shells[SHELL_GIT_BASH].wszPath, MAX_PATH, wszPath);
                s_shells[SHELL_GIT_BASH].bAvailable = TRUE;
                found = TRUE;
            }
        }

        if (!found)
        {
            for (int i = 0; gitBashPaths[i]; i++)
            {
                if (GetFileAttributesW(gitBashPaths[i]) != INVALID_FILE_ATTRIBUTES)
                {
                    wcscpy_s(s_shells[SHELL_GIT_BASH].wszPath, MAX_PATH, gitBashPaths[i]);
                    s_shells[SHELL_GIT_BASH].bAvailable = TRUE;
                    break;
                }
            }
        }
    }

    // WSL Bash
    {
        WCHAR wszPath[MAX_PATH];
        if (SearchPathW(NULL, L"wsl.exe", NULL, MAX_PATH, wszPath, NULL))
        {
            wcscpy_s(s_shells[SHELL_WSL_BASH].wszPath, MAX_PATH, wszPath);
            s_shells[SHELL_WSL_BASH].bAvailable = TRUE;
        }
    }
}

static ShellType GetBestShell(void)
{
    if (s_shells[SHELL_POWERSHELL].bAvailable) return SHELL_POWERSHELL;
    if (s_shells[SHELL_CMD].bAvailable) return SHELL_CMD;
    if (s_shells[SHELL_GIT_BASH].bAvailable) return SHELL_GIT_BASH;
    if (s_shells[SHELL_WSL_BASH].bAvailable) return SHELL_WSL_BASH;
    return SHELL_CMD; // Fallback
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

BOOL Terminal_Init(HWND hwndMain)
{
    s_hwndMain = hwndMain;
    s_bConPTYAvailable = InitConPTY();
    DetectAvailableShells();
    s_activeShell = GetBestShell();
    return TRUE;
}

void Terminal_Shutdown(void)
{
    if (s_pTerminal)
    {
        DestroyTerminalInstance(s_pTerminal);
        n2e_Free(s_pTerminal);
        s_pTerminal = NULL;
    }

    if (s_hwndPanel)
    {
        DestroyWindow(s_hwndPanel);
        s_hwndPanel = NULL;
    }

    if (s_hFontHeader) { DeleteObject(s_hFontHeader); s_hFontHeader = NULL; }
    if (s_hFontDropdown) { DeleteObject(s_hFontDropdown); s_hFontDropdown = NULL; }

    s_bVisible = FALSE;
}

//=============================================================================
// Internal: Spawn terminal with selected shell
//=============================================================================

static BOOL SpawnTerminal(HWND hwndParent, ShellType shell)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);

    if (!s_hwndPanel)
    {
        RegisterTermPanelClass(hInst);
        CreateTermFonts();

        s_hwndPanel = CreateWindowExW(
            0, TERMINAL_PANEL_CLASS, L"",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0, hwndParent,
            (HMENU)(UINT_PTR)IDC_TERMINAL_PANEL, hInst, NULL);

        if (!s_hwndPanel) return FALSE;
    }

    // Kill existing terminal
    if (s_pTerminal)
    {
        DestroyTerminalInstance(s_pTerminal);
        n2e_Free(s_pTerminal);
        s_pTerminal = NULL;
    }

    s_pTerminal = (TerminalInstance*)n2e_Alloc(sizeof(TerminalInstance));
    if (!s_pTerminal) return FALSE;
    ZeroMemory(s_pTerminal, sizeof(TerminalInstance));
    s_pTerminal->currentStyle = STYLE_DEFAULT;
    s_pTerminal->shellType = shell;

    // Create Scintilla view
    s_pTerminal->hwndView = CreateWindowExW(
        0, L"Scintilla", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 0, 0, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_TERMINAL_VIEW, hInst, NULL);

    if (!s_pTerminal->hwndView)
    {
        n2e_Free(s_pTerminal);
        s_pTerminal = NULL;
        return FALSE;
    }

    SetupTerminalStyles(s_pTerminal->hwndView);

    // Subclass for keyboard input
    s_pfnOrigViewProc = (WNDPROC)SetWindowLongPtrW(
        s_pTerminal->hwndView, GWLP_WNDPROC, (LONG_PTR)TermViewSubclassProc);

    // Create the shell process
    COORD size;
    size.X = 120;
    size.Y = 30;

    if (!CreateTerminalProcess(s_pTerminal, size, shell))
    {
        DestroyTerminalInstance(s_pTerminal);
        n2e_Free(s_pTerminal);
        s_pTerminal = NULL;
        return FALSE;
    }

    // Start reader thread
    s_pTerminal->bAlive = TRUE;
    s_pTerminal->hReadThread = CreateThread(NULL, 0,
        TerminalReaderThread, s_pTerminal, 0, NULL);

    s_activeShell = shell;
    InvalidateRect(s_hwndPanel, NULL, TRUE);
    return TRUE;
}

//=============================================================================
// Public: Terminal management
//=============================================================================

BOOL Terminal_New(HWND hwndParent)
{
    BOOL result = SpawnTerminal(hwndParent, s_activeShell);
    if (result)
        Terminal_Show(hwndParent);
    return result;
}

BOOL Terminal_NewShell(HWND hwndParent, int shellType)
{
    if (shellType < 0 || shellType >= SHELL_COUNT) return FALSE;
    if (!s_shells[shellType].bAvailable) return FALSE;
    BOOL result = SpawnTerminal(hwndParent, (ShellType)shellType);
    if (result)
        Terminal_Show(hwndParent);
    return result;
}

void Terminal_Toggle(HWND hwndParent)
{
    if (s_bVisible)
        Terminal_Hide();
    else
    {
        if (!s_pTerminal)
            Terminal_New(hwndParent);
        else
            Terminal_Show(hwndParent);
    }
}

void Terminal_Show(HWND hwndParent)
{
    if (!s_hwndPanel) Terminal_New(hwndParent);
    if (s_hwndPanel)
    {
        ShowWindow(s_hwndPanel, SW_SHOW);
        s_bVisible = TRUE;

        RECT rc;
        GetClientRect(hwndParent, &rc);
        SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                    MAKELPARAM(rc.right, rc.bottom));

        Terminal_Focus();
    }
}

void Terminal_Hide(void)
{
    if (s_hwndPanel)
    {
        ShowWindow(s_hwndPanel, SW_HIDE);
        s_bVisible = FALSE;

        HWND hwndParent = GetParent(s_hwndPanel);
        if (hwndParent)
        {
            RECT rc;
            GetClientRect(hwndParent, &rc);
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rc.right, rc.bottom));
        }
    }
}

BOOL Terminal_IsVisible(void)
{
    return s_bVisible;
}

//=============================================================================
// Public: Layout
//=============================================================================

int Terminal_Layout(HWND hwndParent, int parentWidth, int parentBottom)
{
    UNREFERENCED_PARAMETER(hwndParent);

    if (!s_bVisible || !s_hwndPanel) return 0;

    int panelTop = parentBottom - s_iPanelHeight;
    if (panelTop < 100) panelTop = 100;

    MoveWindow(s_hwndPanel, 0, panelTop, parentWidth, s_iPanelHeight, TRUE);

    int viewTop = TERMINAL_SPLITTER + TERMINAL_HEADER_HEIGHT;
    int viewH = s_iPanelHeight - viewTop;
    if (viewH < 40) viewH = 40;

    if (s_pTerminal && s_pTerminal->hwndView)
    {
        MoveWindow(s_pTerminal->hwndView,
                   0, viewTop,
                   parentWidth, viewH,
                   TRUE);
    }

    return s_iPanelHeight;
}

//=============================================================================
// Public: Input
//=============================================================================

void Terminal_Write(const char* pszData, int len)
{
    if (!s_pTerminal || !s_pTerminal->bAlive || !s_pTerminal->hPipeIn)
        return;

    DWORD dwWritten;
    WriteFile(s_pTerminal->hPipeIn, pszData, (DWORD)len, &dwWritten, NULL);
}

void Terminal_SendCommand(const char* pszCommand)
{
    if (!pszCommand) return;
    Terminal_Write(pszCommand, (int)strlen(pszCommand));
    Terminal_Write("\r\n", 2);
}

void Terminal_Focus(void)
{
    if (s_pTerminal && s_pTerminal->hwndView)
        SetFocus(s_pTerminal->hwndView);
}

HWND Terminal_GetPanelHwnd(void)
{
    return s_hwndPanel;
}

void Terminal_RunCommand(HWND hwndParent, const char* pszCommand)
{
    if (!s_pTerminal || !s_pTerminal->bAlive)
        Terminal_New(hwndParent);

    if (s_pTerminal && s_pTerminal->bAlive)
    {
        Terminal_Show(hwndParent);
        Terminal_SendCommand(pszCommand);
    }
}

//=============================================================================
// Internal: ConPTY initialization
//=============================================================================

static BOOL InitConPTY(void)
{
    HMODULE hKernel = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel) return FALSE;

    s_pfnCreatePC = (pfnCreatePseudoConsole)
        GetProcAddress(hKernel, "CreatePseudoConsole");
    s_pfnResizePC = (pfnResizePseudoConsole)
        GetProcAddress(hKernel, "ResizePseudoConsole");
    s_pfnClosePC = (pfnClosePseudoConsole)
        GetProcAddress(hKernel, "ClosePseudoConsole");

    return (s_pfnCreatePC && s_pfnResizePC && s_pfnClosePC);
}

//=============================================================================
// Internal: Create shell process
//=============================================================================

static BOOL CreateTerminalProcess(TerminalInstance* pTerm, COORD size, ShellType shell)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hPipeInRead = NULL, hPipeInWrite = NULL;
    HANDLE hPipeOutRead = NULL, hPipeOutWrite = NULL;

    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, &sa, 0))
        return FALSE;
    if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, &sa, 0))
    {
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeInWrite);
        return FALSE;
    }

    pTerm->hPipeIn = hPipeInWrite;
    pTerm->hPipeOut = hPipeOutRead;
    pTerm->hInputRead = hPipeInRead;
    pTerm->hOutputWrite = hPipeOutWrite;

    if (s_bConPTYAvailable)
    {
        HRESULT hr = s_pfnCreatePC(size, hPipeInRead, hPipeOutWrite, 0,
                                   &pTerm->hPseudoConsole);
        if (FAILED(hr))
            s_bConPTYAvailable = FALSE;
    }

    // Resolve shell command
    WCHAR wszCmd[MAX_PATH * 2];
    wszCmd[0] = L'\0';

    if (s_shells[shell].bAvailable && s_shells[shell].wszPath[0])
    {
        if (shell == SHELL_GIT_BASH)
        {
            // Git Bash needs --login -i for interactive mode
            swprintf_s(wszCmd, MAX_PATH * 2, L"\"%s\" --login -i",
                       s_shells[shell].wszPath);
        }
        else if (shell == SHELL_WSL_BASH)
        {
            wcscpy_s(wszCmd, MAX_PATH * 2, s_shells[shell].wszPath);
        }
        else
        {
            wcscpy_s(wszCmd, MAX_PATH * 2, s_shells[shell].wszPath);
        }
    }
    else
    {
        // Fallback to COMSPEC
        DWORD dwLen = GetEnvironmentVariableW(L"COMSPEC", wszCmd, MAX_PATH);
        if (dwLen == 0)
            wcscpy_s(wszCmd, MAX_PATH * 2, L"cmd.exe");
    }

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL bCreated = FALSE;

    if (s_bConPTYAvailable && pTerm->hPseudoConsole)
    {
        SIZE_T attrListSize = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)n2e_Alloc(attrListSize);
        if (si.lpAttributeList)
        {
            if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrListSize))
            {
                UpdateProcThreadAttribute(si.lpAttributeList, 0,
                    0x00020016 /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE */,
                    pTerm->hPseudoConsole, sizeof(HPCON), NULL, NULL);

                si.StartupInfo.dwFlags = 0;

                bCreated = CreateProcessW(NULL, wszCmd, NULL, NULL, FALSE,
                    EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                    &si.StartupInfo, &pi);
            }
        }
    }

    if (!bCreated)
    {
        // Fallback: plain pipes
        si.StartupInfo.cb = sizeof(STARTUPINFOW);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.StartupInfo.hStdInput = hPipeInRead;
        si.StartupInfo.hStdOutput = hPipeOutWrite;
        si.StartupInfo.hStdError = hPipeOutWrite;
        si.StartupInfo.wShowWindow = SW_HIDE;

        bCreated = CreateProcessW(NULL, wszCmd, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, NULL,
            &si.StartupInfo, &pi);
    }

    if (bCreated)
    {
        pTerm->hProcess = pi.hProcess;
        pTerm->hThread = pi.hThread;
    }

    // Cleanup handles
    if (s_bConPTYAvailable && pTerm->hPseudoConsole)
    {
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        pTerm->hInputRead = NULL;
        pTerm->hOutputWrite = NULL;

        if (si.lpAttributeList)
        {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            n2e_Free(si.lpAttributeList);
        }
    }
    else if (bCreated)
    {
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        pTerm->hInputRead = NULL;
        pTerm->hOutputWrite = NULL;
    }

    return bCreated;
}

//=============================================================================
// Internal: Destroy terminal instance
//=============================================================================

static void DestroyTerminalInstance(TerminalInstance* pTerm)
{
    if (!pTerm) return;

    pTerm->bAlive = FALSE;

    if (pTerm->hPseudoConsole && s_pfnClosePC)
    {
        s_pfnClosePC(pTerm->hPseudoConsole);
        pTerm->hPseudoConsole = NULL;
    }

    if (pTerm->hPipeIn) { CloseHandle(pTerm->hPipeIn); pTerm->hPipeIn = NULL; }
    if (pTerm->hPipeOut) { CloseHandle(pTerm->hPipeOut); pTerm->hPipeOut = NULL; }
    if (pTerm->hInputRead) { CloseHandle(pTerm->hInputRead); pTerm->hInputRead = NULL; }
    if (pTerm->hOutputWrite) { CloseHandle(pTerm->hOutputWrite); pTerm->hOutputWrite = NULL; }

    if (pTerm->hReadThread)
    {
        WaitForSingleObject(pTerm->hReadThread, 2000);
        CloseHandle(pTerm->hReadThread);
        pTerm->hReadThread = NULL;
    }

    if (pTerm->hProcess)
    {
        TerminateProcess(pTerm->hProcess, 0);
        CloseHandle(pTerm->hProcess);
        pTerm->hProcess = NULL;
    }
    if (pTerm->hThread)
    {
        CloseHandle(pTerm->hThread);
        pTerm->hThread = NULL;
    }

    if (pTerm->hwndView)
    {
        DestroyWindow(pTerm->hwndView);
        pTerm->hwndView = NULL;
    }
}

//=============================================================================
// Internal: Reader thread
//=============================================================================

static DWORD WINAPI TerminalReaderThread(LPVOID lpParam)
{
    TerminalInstance* pTerm = (TerminalInstance*)lpParam;
    char buf[4096];

    while (pTerm->bAlive && pTerm->hPipeOut)
    {
        DWORD dwRead = 0;
        BOOL bOk = ReadFile(pTerm->hPipeOut, buf, sizeof(buf) - 1, &dwRead, NULL);
        if (!bOk || dwRead == 0)
            break;

        buf[dwRead] = '\0';

        if (pTerm->hwndView)
        {
            char* pCopy = (char*)n2e_Alloc(dwRead + 1);
            if (pCopy)
            {
                memcpy(pCopy, buf, dwRead + 1);
                PostMessage(GetParent(pTerm->hwndView),
                    WM_USER + 200, (WPARAM)pCopy, (LPARAM)dwRead);
            }
        }
    }

    pTerm->bAlive = FALSE;
    return 0;
}

//=============================================================================
// Internal: Terminal styles
//=============================================================================

static void SetupTerminalStyles(HWND hwndView)
{
    if (!hwndView) return;

    BOOL bDark = DarkMode_IsEnabled();
    COLORREF bg = bDark ? TRM_DK_TERM_BG : TRM_LT_TERM_BG;
    COLORREF fg = bDark ? TRM_DK_TEXT : TRM_LT_TEXT;

    SendMessage(hwndView, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Cascadia Code");
    SendMessage(hwndView, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    SendMessage(hwndView, SCI_STYLESETBACK, STYLE_DEFAULT, bg);
    SendMessage(hwndView, SCI_STYLESETFORE, STYLE_DEFAULT, fg);
    SendMessage(hwndView, SCI_STYLECLEARALL, 0, 0);

    // ANSI Colors (Styles 40-47: Normal, 48-55: Bright)
    COLORREF colors[8] = {
        RGB(0, 0, 0),       RGB(197, 15, 31),   RGB(19, 161, 14),   RGB(193, 156, 0),
        RGB(0, 55, 218),    RGB(136, 23, 152),  RGB(58, 150, 221),  RGB(204, 204, 204)
    };
    COLORREF colorsBright[8] = {
        RGB(118, 118, 118), RGB(231, 72, 86),   RGB(22, 198, 12),   RGB(249, 241, 165),
        RGB(59, 120, 255),  RGB(180, 0, 158),   RGB(97, 214, 214),  RGB(242, 242, 242)
    };

    if (bDark) {
        colors[0] = RGB(128, 128, 128);
        colors[7] = RGB(242, 242, 242);
        colors[4] = RGB(59, 120, 255);
    }

    for (int i = 0; i < 8; i++) {
        int sNorm = 40 + i;
        SendMessage(hwndView, SCI_STYLESETFORE, sNorm, colors[i]);
        SendMessage(hwndView, SCI_STYLESETBACK, sNorm, bg);
        SendMessage(hwndView, SCI_STYLESETFONT, sNorm, (LPARAM)"Cascadia Code");
        SendMessage(hwndView, SCI_STYLESETSIZE, sNorm, 10);

        int sBright = 48 + i;
        SendMessage(hwndView, SCI_STYLESETFORE, sBright, colorsBright[i]);
        SendMessage(hwndView, SCI_STYLESETBACK, sBright, bg);
        SendMessage(hwndView, SCI_STYLESETFONT, sBright, (LPARAM)"Cascadia Code");
        SendMessage(hwndView, SCI_STYLESETSIZE, sBright, 10);
    }

    // No margins - terminal is edge-to-edge
    SendMessage(hwndView, SCI_SETMARGINWIDTHN, 0, 6);
    SendMessage(hwndView, SCI_SETMARGINWIDTHN, 1, 0);
    SendMessage(hwndView, SCI_SETWRAPMODE, SC_WRAP_CHAR, 0);
    SendMessage(hwndView, SCI_SETREADONLY, FALSE, 0);
    SendMessage(hwndView, SCI_SETCARETFORE, fg, 0);

    // Selection
    SendMessage(hwndView, SCI_SETSELBACK, TRUE,
        bDark ? RGB(55, 55, 70) : RGB(180, 215, 255));
    SendMessage(hwndView, SCI_SETSELFORE, TRUE, fg);

    // Line spacing
    SendMessage(hwndView, SCI_SETEXTRAASCENT, 1, 0);
    SendMessage(hwndView, SCI_SETEXTRADESCENT, 1, 0);

    // Scrollbar theme
    if (bDark)
        SetWindowTheme(hwndView, L"DarkMode_Explorer", NULL);
    else
        SetWindowTheme(hwndView, NULL, NULL);
}

//=============================================================================
// Internal: Append output (ANSI parsing)
//=============================================================================

static void AppendTerminalOutput(HWND hwndView, const char* data, int len)
{
    if (!hwndView || !data || len <= 0) return;

    const char* p = data;
    const char* end = data + len;
    const char* start = p;

    while (p < end)
    {
        if (*p == '\033')
        {
            // Flush text before escape
            if (p > start) {
                int L = (int)(p - start);
                int docLen = (int)SendMessage(hwndView, SCI_GETLENGTH, 0, 0);
                SendMessage(hwndView, SCI_APPENDTEXT, L, (LPARAM)start);
                SendMessage(hwndView, SCI_STARTSTYLING, docLen, 0);
                SendMessage(hwndView, SCI_SETSTYLING, L, s_pTerminal->currentStyle);
            }

            const char* escStart = p;
            p++;
            if (p < end && *p == '[')
            {
                p++;
                int params[8] = { 0 };
                int paramCount = 0;
                int val = 0;
                BOOL hasVal = FALSE;

                while (p < end) {
                    if (*p >= '0' && *p <= '9') {
                        val = val * 10 + (*p - '0');
                        hasVal = TRUE;
                    } else if (*p == ';') {
                        if (paramCount < 8) params[paramCount++] = val;
                        val = 0; hasVal = FALSE;
                    } else {
                        break;
                    }
                    p++;
                }
                if (hasVal && paramCount < 8) params[paramCount++] = val;

                if (p < end && *p == 'm') {
                    if (paramCount == 0) s_pTerminal->currentStyle = STYLE_DEFAULT;
                    for (int i = 0; i < paramCount; i++) {
                        int code = params[i];
                        if (code == 0) s_pTerminal->currentStyle = STYLE_DEFAULT;
                        else if (code >= 30 && code <= 37) s_pTerminal->currentStyle = 40 + (code - 30);
                        else if (code >= 90 && code <= 97) s_pTerminal->currentStyle = 48 + (code - 90);
                        else if (code == 1) {
                            if (s_pTerminal->currentStyle >= 40 && s_pTerminal->currentStyle <= 47)
                                s_pTerminal->currentStyle += 8;
                        }
                    }
                    p++;
                }
                else {
                    while (p < end && !(*p >= 64 && *p <= 126)) p++;
                    if (p < end) p++;
                }
            }
            start = p;
        }
        else if (*p == '\r')
        {
            if (p > start) {
                int L = (int)(p - start);
                int docLen = (int)SendMessage(hwndView, SCI_GETLENGTH, 0, 0);
                SendMessage(hwndView, SCI_APPENDTEXT, L, (LPARAM)start);
                SendMessage(hwndView, SCI_STARTSTYLING, docLen, 0);
                SendMessage(hwndView, SCI_SETSTYLING, L, s_pTerminal->currentStyle);
            }
            p++;
            if (p < end && *p == '\n') {
                start = p;
            } else {
                start = p;
            }
        }
        else
        {
            p++;
        }
    }

    // Flush remaining
    if (p > start) {
        int L = (int)(p - start);
        int docLen = (int)SendMessage(hwndView, SCI_GETLENGTH, 0, 0);
        SendMessage(hwndView, SCI_APPENDTEXT, L, (LPARAM)start);
        SendMessage(hwndView, SCI_STARTSTYLING, docLen, 0);
        SendMessage(hwndView, SCI_SETSTYLING, L, s_pTerminal->currentStyle);
    }

    SendMessage(hwndView, SCI_SCROLLTOEND, 0, 0);
}

//=============================================================================
// Internal: Shell context menu
//=============================================================================

static void ShowShellMenu(HWND hwnd)
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    for (int i = 0; i < SHELL_COUNT; i++)
    {
        if (!s_shells[i].bAvailable) continue;

        WCHAR wszItem[128];
        swprintf_s(wszItem, 128, L"  %s  %s",
                   s_shells[i].wszIcon, s_shells[i].wszLabel);

        UINT flags = MF_STRING;
        if ((ShellType)i == s_activeShell && s_pTerminal && s_pTerminal->bAlive)
            flags |= MF_CHECKED;

        AppendMenuW(hMenu, flags, 5000 + i, wszItem);
    }

    // Position below the dropdown
    POINT pt;
    pt.x = s_rcDropdown.left;
    pt.y = s_rcDropdown.bottom;
    ClientToScreen(hwnd, &pt);

    int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, NULL);

    DestroyMenu(hMenu);

    if (cmd >= 5000 && cmd < 5000 + SHELL_COUNT)
    {
        ShellType newShell = (ShellType)(cmd - 5000);
        HWND hwndParent = GetParent(hwnd);
        Terminal_NewShell(hwndParent, newShell);
    }
}

//=============================================================================
// Internal: Custom header painting
//=============================================================================

static void PaintHeader(HWND hwnd, HDC hdc, RECT* prcClient)
{
    BOOL bDark = DarkMode_IsEnabled();
    int W = prcClient->right;

    // Header background
    RECT rcHeader = { 0, TERMINAL_SPLITTER, W, TERMINAL_SPLITTER + TERMINAL_HEADER_HEIGHT };
    HBRUSH hHdr = CreateSolidBrush(bDark ? TRM_DK_HEADER : TRM_LT_HEADER);
    FillRect(hdc, &rcHeader, hHdr);
    DeleteObject(hHdr);

    // Header bottom border
    RECT rcBorder = rcHeader;
    rcBorder.top = rcBorder.bottom - 1;
    HBRUSH hBdr = CreateSolidBrush(bDark ? TRM_DK_BORDER : TRM_LT_BORDER);
    FillRect(hdc, &rcBorder, hBdr);
    DeleteObject(hBdr);

    SetBkMode(hdc, TRANSPARENT);

    // Shell dropdown area (left side)
    int dropW = 140;
    int dropH = 22;
    int dropX = 10;
    int dropY = TERMINAL_SPLITTER + (TERMINAL_HEADER_HEIGHT - dropH) / 2;
    s_rcDropdown.left = dropX;
    s_rcDropdown.top = dropY;
    s_rcDropdown.right = dropX + dropW;
    s_rcDropdown.bottom = dropY + dropH;

    // Dropdown background
    HBRUSH hDropBg = CreateSolidBrush(bDark ? TRM_DK_DROPDOWN_BG : TRM_LT_DROPDOWN_BG);
    HPEN hDropPen = CreatePen(PS_SOLID, 1, bDark ? TRM_DK_BORDER : TRM_LT_BORDER);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hDropBg);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hDropPen);
    RoundRect(hdc, s_rcDropdown.left, s_rcDropdown.top,
              s_rcDropdown.right, s_rcDropdown.bottom, 6, 6);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hDropBg);
    DeleteObject(hDropPen);

    // Shell name in dropdown
    if (s_hFontDropdown) SelectObject(hdc, s_hFontDropdown);
    SetTextColor(hdc, bDark ? TRM_DK_TEXT : TRM_LT_TEXT);
    RECT rcLabel = s_rcDropdown;
    rcLabel.left += 8;
    rcLabel.right -= 18;
    DrawTextW(hdc, s_shells[s_activeShell].wszLabel, -1,
              &rcLabel, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    // Dropdown chevron
    SetTextColor(hdc, bDark ? TRM_DK_TEXT_DIM : TRM_LT_TEXT_DIM);
    RECT rcChev = s_rcDropdown;
    rcChev.left = rcChev.right - 18;
    DrawTextW(hdc, L"\x25BC", 1, &rcChev, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    // Right-side buttons
    int btnSize = 22;
    int btnY = TERMINAL_SPLITTER + (TERMINAL_HEADER_HEIGHT - btnSize) / 2;
    int btnPad = 4;

    // Close button (X)
    s_rcCloseBtn.left = W - btnSize - 8;
    s_rcCloseBtn.top = btnY;
    s_rcCloseBtn.right = s_rcCloseBtn.left + btnSize;
    s_rcCloseBtn.bottom = btnY + btnSize;

    if (s_bCloseHover)
    {
        HBRUSH hHov = CreateSolidBrush(bDark ? TRM_DK_BTN_HOVER : TRM_LT_BTN_HOVER);
        HPEN hHovPen = CreatePen(PS_SOLID, 1, bDark ? TRM_DK_BTN_HOVER : TRM_LT_BTN_HOVER);
        HBRUSH hOB = (HBRUSH)SelectObject(hdc, hHov);
        HPEN hOP = (HPEN)SelectObject(hdc, hHovPen);
        RoundRect(hdc, s_rcCloseBtn.left, s_rcCloseBtn.top,
                  s_rcCloseBtn.right, s_rcCloseBtn.bottom, 4, 4);
        SelectObject(hdc, hOB);
        SelectObject(hdc, hOP);
        DeleteObject(hHov);
        DeleteObject(hHovPen);
    }
    SetTextColor(hdc, bDark ? TRM_DK_TEXT_DIM : TRM_LT_TEXT_DIM);
    if (s_hFontDropdown) SelectObject(hdc, s_hFontDropdown);
    DrawTextW(hdc, L"\x2715", 1, &s_rcCloseBtn, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    // New terminal button (+)
    s_rcNewBtn.left = s_rcCloseBtn.left - btnSize - btnPad;
    s_rcNewBtn.top = btnY;
    s_rcNewBtn.right = s_rcNewBtn.left + btnSize;
    s_rcNewBtn.bottom = btnY + btnSize;

    if (s_bNewHover)
    {
        HBRUSH hHov = CreateSolidBrush(bDark ? TRM_DK_BTN_HOVER : TRM_LT_BTN_HOVER);
        HPEN hHovPen = CreatePen(PS_SOLID, 1, bDark ? TRM_DK_BTN_HOVER : TRM_LT_BTN_HOVER);
        HBRUSH hOB = (HBRUSH)SelectObject(hdc, hHov);
        HPEN hOP = (HPEN)SelectObject(hdc, hHovPen);
        RoundRect(hdc, s_rcNewBtn.left, s_rcNewBtn.top,
                  s_rcNewBtn.right, s_rcNewBtn.bottom, 4, 4);
        SelectObject(hdc, hOB);
        SelectObject(hdc, hOP);
        DeleteObject(hHov);
        DeleteObject(hHovPen);
    }
    SetTextColor(hdc, bDark ? TRM_DK_TEXT_DIM : TRM_LT_TEXT_DIM);
    DrawTextW(hdc, L"+", 1, &s_rcNewBtn, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
}

//=============================================================================
// Internal: Panel window procedure
//=============================================================================

static LRESULT CALLBACK TermPanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        BOOL bDark = DarkMode_IsEnabled();

        // Surface (behind Scintilla)
        HBRUSH hBg = CreateSolidBrush(bDark ? TRM_DK_SURFACE : TRM_LT_SURFACE);
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);

        // Splitter at top
        RECT rcSplit = rc;
        rcSplit.bottom = TERMINAL_SPLITTER;
        HBRUSH hSplit = CreateSolidBrush(bDark ? TRM_DK_SPLITTER : TRM_LT_SPLITTER);
        FillRect(hdc, &rcSplit, hSplit);
        DeleteObject(hSplit);

        // Header bar
        PaintHeader(hwnd, hdc, &rc);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH hBr = CreateSolidBrush(
            DarkMode_IsEnabled() ? TRM_DK_SURFACE : TRM_LT_SURFACE);
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);
        return 1;
    }

    case WM_SETCURSOR:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (pt.y < TERMINAL_SPLITTER)
        {
            SetCursor(LoadCursor(NULL, IDC_SIZENS));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);

        // Splitter drag
        if (y < TERMINAL_SPLITTER)
        {
            SetCapture(hwnd);
            return 0;
        }

        // Dropdown click
        POINT pt = { x, y };
        if (PtInRect(&s_rcDropdown, pt))
        {
            ShowShellMenu(hwnd);
            return 0;
        }

        // New button
        if (PtInRect(&s_rcNewBtn, pt))
        {
            HWND hwndParent = GetParent(hwnd);
            Terminal_New(hwndParent);
            return 0;
        }

        // Close button
        if (PtInRect(&s_rcCloseBtn, pt))
        {
            Terminal_Hide();
            return 0;
        }

        break;
    }

    case WM_MOUSEMOVE:
    {
        if (GetCapture() == hwnd)
        {
            POINT pt;
            GetCursorPos(&pt);
            HWND hwndParent = GetParent(hwnd);
            ScreenToClient(hwndParent, &pt);
            RECT rcParent;
            GetClientRect(hwndParent, &rcParent);
            int newH = rcParent.bottom - pt.y;
            if (newH < 120) newH = 120;
            if (newH > rcParent.bottom - 100) newH = rcParent.bottom - 100;
            s_iPanelHeight = newH;
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rcParent.right, rcParent.bottom));
        }
        else
        {
            // Hover tracking for buttons
            POINT pt = { LOWORD(lParam), HIWORD(lParam) };
            BOOL bCloseNow = PtInRect(&s_rcCloseBtn, pt);
            BOOL bNewNow = PtInRect(&s_rcNewBtn, pt);

            if (bCloseNow != s_bCloseHover || bNewNow != s_bNewHover)
            {
                s_bCloseHover = bCloseNow;
                s_bNewHover = bNewNow;
                // Repaint header area only
                RECT rcHdr = { 0, TERMINAL_SPLITTER, 9999, TERMINAL_SPLITTER + TERMINAL_HEADER_HEIGHT };
                InvalidateRect(hwnd, &rcHdr, FALSE);
            }

            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (s_bCloseHover || s_bNewHover)
        {
            s_bCloseHover = FALSE;
            s_bNewHover = FALSE;
            RECT rcHdr = { 0, TERMINAL_SPLITTER, 9999, TERMINAL_SPLITTER + TERMINAL_HEADER_HEIGHT };
            InvalidateRect(hwnd, &rcHdr, FALSE);
        }
        break;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        break;

    // Terminal output from reader thread
    case WM_USER + 200:
    {
        char* pData = (char*)wParam;
        int len = (int)lParam;
        if (s_pTerminal && s_pTerminal->hwndView && pData)
            AppendTerminalOutput(s_pTerminal->hwndView, pData, len);
        if (pData) n2e_Free(pData);
        return 0;
    }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

//=============================================================================
// Internal: Keyboard subclass for Scintilla terminal view
//
// CRITICAL: We must intercept ALL keyboard messages to prevent Scintilla
// from processing them (it would modify its internal buffer/state).
// Text-generating keys come through WM_CHAR after TranslateMessage.
// Non-character keys (arrows, delete, etc.) are handled in WM_KEYDOWN.
//=============================================================================

static LRESULT CALLBACK TermViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CHAR:
    {
        // ALL text-generating keystrokes come here (a-z, Ctrl+C, Enter, etc.)
        char ch = (char)wParam;

        if (ch == '\r')
        {
            Terminal_Write("\r", 1);
        }
        else if (ch == '\t')
        {
            Terminal_Write("\t", 1);
        }
        else
        {
            Terminal_Write(&ch, 1);
        }
        return 0; // Block Scintilla
    }

    case WM_KEYDOWN:
    {
        // Handle non-character keys, then BLOCK all from reaching Scintilla
        switch (wParam)
        {
        case VK_UP:
            Terminal_Write("\033[A", 3);
            return 0;
        case VK_DOWN:
            Terminal_Write("\033[B", 3);
            return 0;
        case VK_LEFT:
            Terminal_Write("\033[D", 3);
            return 0;
        case VK_RIGHT:
            Terminal_Write("\033[C", 3);
            return 0;
        case VK_DELETE:
            Terminal_Write("\033[3~", 4);
            return 0;
        case VK_HOME:
            Terminal_Write("\033[H", 3);
            return 0;
        case VK_END:
            Terminal_Write("\033[F", 3);
            return 0;
        case VK_PRIOR:  // Page Up
            Terminal_Write("\033[5~", 4);
            return 0;
        case VK_NEXT:   // Page Down
            Terminal_Write("\033[6~", 4);
            return 0;
        case VK_INSERT:
            Terminal_Write("\033[2~", 4);
            return 0;
        case VK_ESCAPE:
            Terminal_Hide();
            return 0;
        }

        // Ctrl+V: paste from clipboard
        if (wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            if (OpenClipboard(hwnd))
            {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData)
                {
                    const WCHAR* wszText = (const WCHAR*)GlobalLock(hData);
                    if (wszText)
                    {
                        // Convert to UTF-8 for the terminal
                        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
                        if (utf8Len > 0)
                        {
                            char* utf8 = (char*)n2e_Alloc(utf8Len);
                            if (utf8)
                            {
                                WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8, utf8Len, NULL, NULL);
                                Terminal_Write(utf8, utf8Len - 1); // -1 to exclude null terminator
                                n2e_Free(utf8);
                            }
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            return 0;
        }

        // Ctrl+C: copy selection, or send SIGINT if no selection
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            // Check if there's a selection in Scintilla
            int selStart = (int)SendMessage(hwnd, SCI_GETSELECTIONSTART, 0, 0);
            int selEnd = (int)SendMessage(hwnd, SCI_GETSELECTIONEND, 0, 0);
            if (selStart != selEnd)
            {
                // Copy selection to clipboard (let Scintilla handle it)
                SendMessage(hwnd, SCI_COPY, 0, 0);
            }
            // Don't block - Ctrl+C also generates WM_CHAR with 0x03 (ETX)
            // which will be sent to the terminal as SIGINT
            return 0;
        }

        // Block ALL other WM_KEYDOWN from reaching Scintilla.
        // TranslateMessage has already queued WM_CHAR for character keys,
        // so we'll handle those characters in the WM_CHAR handler above.
        return 0;
    }

    case WM_SYSKEYDOWN:
        // Block Alt+key combinations from reaching Scintilla
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        // Block keyup too to keep things consistent
        return 0;

    case WM_LBUTTONDOWN:
        // Allow mouse clicks for selecting text in terminal output
        SetFocus(hwnd);
        break;
    }

    return CallWindowProc(s_pfnOrigViewProc, hwnd, msg, wParam, lParam);
}
