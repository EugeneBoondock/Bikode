/******************************************************************************
*
* Biko
*
* Terminal.c
*   Embedded terminal using ConPTY on Windows 10 1809+.
*   Uses a Scintilla control as the terminal viewport.
*
******************************************************************************/

#include "Terminal.h"
#include "DarkMode.h"
#include "CommonUtils.h"
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
// Terminal state
//=============================================================================

#define TERMINAL_HEIGHT     200
#define TERMINAL_SPLITTER   4

typedef struct TerminalInstance {
    HPCON           hPseudoConsole;
    HANDLE          hProcess;
    HANDLE          hThread;
    HANDLE          hPipeIn;        // Write end â†’ shell stdin
    HANDLE          hPipeOut;       // Read end â† shell stdout
    HANDLE          hInputRead;     // Shell reads from here
    HANDLE          hOutputWrite;   // Shell writes to here
    HANDLE          hReadThread;    // Background reader thread
    HWND            hwndView;       // Scintilla control
    volatile BOOL   bAlive;
} TerminalInstance;

static HWND                 s_hwndMain = NULL;
static HWND                 s_hwndPanel = NULL;
static TerminalInstance*    s_pTerminal = NULL;
static BOOL                 s_bVisible = FALSE;
static int                  s_iPanelHeight = TERMINAL_HEIGHT;
static WNDPROC              s_pfnOrigViewProc = NULL;

static const WCHAR* TERMINAL_PANEL_CLASS = L"BikoTerminalPanel";
static BOOL s_bClassRegistered = FALSE;

//=============================================================================
// Forward declarations
//=============================================================================

static LRESULT CALLBACK TermPanelWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK TermViewSubclassProc(HWND, UINT, WPARAM, LPARAM);
static DWORD WINAPI TerminalReaderThread(LPVOID lpParam);
static BOOL InitConPTY(void);
static BOOL CreateTerminalProcess(TerminalInstance* pTerm, COORD size);
static void DestroyTerminalInstance(TerminalInstance* pTerm);
static void SetupTerminalStyles(HWND hwndView);
static void AppendTerminalOutput(HWND hwndView, const char* data, int len);

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
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = TERMINAL_PANEL_CLASS;

    RegisterClassExW(&wc);
    s_bClassRegistered = TRUE;
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

BOOL Terminal_Init(HWND hwndMain)
{
    s_hwndMain = hwndMain;
    s_bConPTYAvailable = InitConPTY();
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

    s_bVisible = FALSE;
}

//=============================================================================
// Public: Terminal management
//=============================================================================

BOOL Terminal_New(HWND hwndParent)
{
    if (!s_hwndPanel)
    {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);
        RegisterTermPanelClass(hInst);

        s_hwndPanel = CreateWindowExW(
            0, TERMINAL_PANEL_CLASS, L"",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0,
            hwndParent,
            (HMENU)(UINT_PTR)IDC_TERMINAL_PANEL,
            hInst, NULL);

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

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);

    // Create Scintilla view for terminal output
    s_pTerminal->hwndView = CreateWindowExW(
        0, L"Scintilla", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, TERMINAL_SPLITTER, 0, 0,
        s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_TERMINAL_VIEW,
        hInst, NULL);

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

    if (!CreateTerminalProcess(s_pTerminal, size))
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

    Terminal_Show(hwndParent);
    return TRUE;
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

    if (s_pTerminal && s_pTerminal->hwndView)
    {
        MoveWindow(s_pTerminal->hwndView,
                   0, TERMINAL_SPLITTER,
                   parentWidth, s_iPanelHeight - TERMINAL_SPLITTER,
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

static BOOL CreateTerminalProcess(TerminalInstance* pTerm, COORD size)
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    // Pipes for ConPTY
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
        // Create pseudo console
        HRESULT hr = s_pfnCreatePC(size, hPipeInRead, hPipeOutWrite, 0,
                                   &pTerm->hPseudoConsole);
        if (FAILED(hr))
        {
            // Fall back to direct pipe mode
            s_bConPTYAvailable = FALSE;
        }
    }

    // Get shell path
    WCHAR wszShell[MAX_PATH];
    DWORD dwLen = GetEnvironmentVariableW(L"COMSPEC", wszShell, MAX_PATH);
    if (dwLen == 0)
        wcscpy_s(wszShell, MAX_PATH, L"cmd.exe");

    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL bCreated = FALSE;

    if (s_bConPTYAvailable && pTerm->hPseudoConsole)
    {
        // Use ConPTY: set up attribute list for the pseudo console handle
        SIZE_T attrListSize = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)n2e_Alloc(attrListSize);
        if (si.lpAttributeList)
        {
            if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrListSize))
            {
                // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016
                UpdateProcThreadAttribute(si.lpAttributeList, 0,
                    0x00020016 /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE */,
                    pTerm->hPseudoConsole, sizeof(HPCON), NULL, NULL);

                si.StartupInfo.dwFlags = 0;

                WCHAR wszCmd[MAX_PATH];
                wcscpy_s(wszCmd, MAX_PATH, wszShell);

                bCreated = CreateProcessW(NULL, wszCmd, NULL, NULL, FALSE,
                    EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                    &si.StartupInfo, &pi);
            }
            // Don't delete the attribute list yet â€” process needs it
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

        WCHAR wszCmd[MAX_PATH];
        wcscpy_s(wszCmd, MAX_PATH, wszShell);

        bCreated = CreateProcessW(NULL, wszCmd, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, NULL,
            &si.StartupInfo, &pi);
    }

    if (bCreated)
    {
        pTerm->hProcess = pi.hProcess;
        pTerm->hThread = pi.hThread;
    }

    // Cleanup handles the shell doesn't need on our side
    // (ConPTY handles the pipe endpoints internally)
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

    return bCreated;
}

//=============================================================================
// Internal: Destroy terminal instance
//=============================================================================

static void DestroyTerminalInstance(TerminalInstance* pTerm)
{
    if (!pTerm) return;

    pTerm->bAlive = FALSE;

    // Close pseudo console (sends exit signal)
    if (pTerm->hPseudoConsole && s_pfnClosePC)
    {
        s_pfnClosePC(pTerm->hPseudoConsole);
        pTerm->hPseudoConsole = NULL;
    }

    // Close pipes
    if (pTerm->hPipeIn) { CloseHandle(pTerm->hPipeIn); pTerm->hPipeIn = NULL; }
    if (pTerm->hPipeOut) { CloseHandle(pTerm->hPipeOut); pTerm->hPipeOut = NULL; }
    if (pTerm->hInputRead) { CloseHandle(pTerm->hInputRead); pTerm->hInputRead = NULL; }
    if (pTerm->hOutputWrite) { CloseHandle(pTerm->hOutputWrite); pTerm->hOutputWrite = NULL; }

    // Wait for reader thread
    if (pTerm->hReadThread)
    {
        WaitForSingleObject(pTerm->hReadThread, 2000);
        CloseHandle(pTerm->hReadThread);
        pTerm->hReadThread = NULL;
    }

    // Terminate process
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

    // Destroy view
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
            // Post output to the view (must be on the main thread for Scintilla)
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
// Internal: Styles
//=============================================================================

static void SetupTerminalStyles(HWND hwndView)
{
    if (!hwndView) return;

    SendMessage(hwndView, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Consolas");
    SendMessage(hwndView, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);

    if (DarkMode_IsEnabled())
    {
        SendMessage(hwndView, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(12, 12, 12));
        SendMessage(hwndView, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(204, 204, 204));
    }
    else
    {
        SendMessage(hwndView, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(255, 255, 255));
        SendMessage(hwndView, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0, 0, 0));
    }

    SendMessage(hwndView, SCI_STYLECLEARALL, 0, 0);
    SendMessage(hwndView, SCI_SETMARGINWIDTHN, 0, 0);
    SendMessage(hwndView, SCI_SETMARGINWIDTHN, 1, 0);
    SendMessage(hwndView, SCI_SETWRAPMODE, SC_WRAP_CHAR, 0);
    SendMessage(hwndView, SCI_SETREADONLY, FALSE, 0);
    SendMessage(hwndView, SCI_SETCARETFORE,
        DarkMode_IsEnabled() ? RGB(204, 204, 204) : RGB(0, 0, 0), 0);
}

//=============================================================================
// Internal: Append output
//=============================================================================

static void AppendTerminalOutput(HWND hwndView, const char* data, int len)
{
    if (!hwndView || !data || len <= 0) return;

    // Simple approach: append raw text, stripping ANSI escape sequences
    // (A full VT100 parser would be ideal but is out of scope for MVP)

    const char* p = data;
    const char* end = data + len;
    char clean[4096];
    int ci = 0;

    while (p < end && ci < (int)sizeof(clean) - 1)
    {
        if (*p == '\033')
        {
            // Skip ANSI escape sequence (\033[...m or similar)
            p++;
            if (p < end && *p == '[')
            {
                p++;
                while (p < end && *p != 'm' && *p != 'H' && *p != 'J' &&
                       *p != 'K' && *p != 'A' && *p != 'B' && *p != 'C' &&
                       *p != 'D' && *p != 'h' && *p != 'l')
                    p++;
                if (p < end) p++;
            }
        }
        else if (*p == '\r')
        {
            // Skip carriage return (handle \r\n â†’ \n)
            p++;
        }
        else
        {
            clean[ci++] = *p++;
        }
    }
    clean[ci] = '\0';

    if (ci > 0)
    {
        int docLen = (int)SendMessage(hwndView, SCI_GETLENGTH, 0, 0);
        SendMessage(hwndView, SCI_APPENDTEXT, ci, (LPARAM)clean);
        SendMessage(hwndView, SCI_SCROLLTOEND, 0, 0);
        SendMessage(hwndView, SCI_GOTOPOS, docLen + ci, 0);
    }
}

//=============================================================================
// Internal: Window procedures
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

        // Fill entire panel background
        HBRUSH hBgBrush = CreateSolidBrush(
            DarkMode_IsEnabled() ? RGB(37, 37, 38) : RGB(243, 243, 243));
        FillRect(hdc, &rc, hBgBrush);
        DeleteObject(hBgBrush);

        // Draw the splitter bar at the top
        rc.bottom = TERMINAL_SPLITTER;
        HBRUSH hSplitBrush = CreateSolidBrush(
            DarkMode_IsEnabled() ? RGB(60, 60, 60) : RGB(200, 200, 200));
        FillRect(hdc, &rc, hSplitBrush);
        DeleteObject(hSplitBrush);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH hBrush = CreateSolidBrush(
            DarkMode_IsEnabled() ? RGB(37, 37, 38) : RGB(243, 243, 243));
        FillRect(hdc, &rc, hBrush);
        DeleteObject(hBrush);
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
        int y = HIWORD(lParam);
        if (y < TERMINAL_SPLITTER)
        {
            SetCapture(hwnd);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd)
        {
            POINT pt;
            GetCursorPos(&pt);
            HWND hwndParent = GetParent(hwnd);
            ScreenToClient(hwndParent, &pt);
            RECT rcParent;
            GetClientRect(hwndParent, &rcParent);
            int newH = rcParent.bottom - pt.y;
            if (newH < 80) newH = 80;
            if (newH > rcParent.bottom - 100) newH = rcParent.bottom - 100;
            s_iPanelHeight = newH;
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rcParent.right, rcParent.bottom));
        }
        break;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        break;

    // Handle terminal output posted from reader thread
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

static LRESULT CALLBACK TermViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CHAR:
    {
        // Forward typed characters to the shell
        char ch = (char)wParam;
        Terminal_Write(&ch, 1);
        return 0; // Don't let Scintilla handle it
    }

    case WM_KEYDOWN:
        switch (wParam)
        {
        case VK_RETURN:
            Terminal_Write("\r\n", 2);
            return 0;
        case VK_BACK:
            Terminal_Write("\b", 1);
            return 0;
        case VK_ESCAPE:
            Terminal_Hide();
            return 0;
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
        case VK_TAB:
            Terminal_Write("\t", 1);
            return 0;
        }

        // Ctrl+C â†’ send interrupt
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            Terminal_Write("\x03", 1);
            return 0;
        }
        break;
    }

    return CallWindowProc(s_pfnOrigViewProc, hwnd, msg, wParam, lParam);
}
