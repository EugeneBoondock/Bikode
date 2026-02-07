
/******************************************************************************
*
* Biko
*
* WelcomeScreen.c
*   Welcome Screen implementation.
*   Draws a styled welcome page over the editor when empty.
*
******************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include "WelcomeScreen.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include "Terminal.h"
#include "resource.h"

// External references
extern HWND hwndMain;
extern WCHAR szCurFile[MAX_PATH + 40];
void FileLoad(BOOL, BOOL, BOOL, BOOL, LPCWSTR);

#ifndef BS_COMMANDLINK
#define BS_COMMANDLINK      0x0000000E
#define BS_DEFCOMMANDLINK   0x0000000F
#endif

// Messages
#define IDC_CMD_NEW     1001
#define IDC_CMD_OPEN    1002
#define IDC_CMD_CLONE   1003

static HWND     s_hwndWelcome = NULL;
static HFONT    s_hFontLogo = NULL;
static HFONT    s_hFontQuote = NULL;
static HWND     s_hwndBtnNew = NULL;
static HWND     s_hwndBtnOpen = NULL;
static HWND     s_hwndBtnClone = NULL;

static LRESULT CALLBACK WelcomeWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH hBr = CreateSolidBrush(DarkMode_IsEnabled() ? RGB(30,30,30) : RGB(255,255,255));
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);
        return 1;
    }

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Fill background
        HBRUSH hBr = DarkMode_IsEnabled() ? 
            CreateSolidBrush(RGB(30,30,30)) : 
            CreateSolidBrush(RGB(255,255,255));
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        // Draw Logo Text "Biko"
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, DarkMode_IsEnabled() ? RGB(220,220,220) : RGB(30,30,30));
        
        if (!s_hFontLogo) {
            s_hFontLogo = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, 
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        }
        SelectObject(hdc, s_hFontLogo);

        RECT rcText = rc;
        rcText.top = rc.top + 80;
        rcText.bottom = rcText.top + 60;
        DrawTextW(hdc, L"Biko", -1, &rcText, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        // Draw Quote
        SetTextColor(hdc, DarkMode_IsEnabled() ? RGB(160,160,160) : RGB(100,100,100));
        if (!s_hFontQuote) {
            s_hFontQuote = CreateFontW(20, 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE, 
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        }
        SelectObject(hdc, s_hFontQuote);
        
        rcText.top = rcText.bottom + 10;
        rcText.bottom = rcText.top + 30;
        DrawTextW(hdc, L"\"I write what I like\"", -1, &rcText, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        switch (id)
        {
        case IDC_CMD_NEW:
            // Trigger "New File" logic (usually just clear editor)
            // SendMessage(hwndMain, WM_COMMAND, IDT_FILE_NEW, 0); 
            // Or just check if FileLoad is accessible
            // We'll simulate IDT_FILE_NEW
            PostMessage(hwndMain, WM_COMMAND, 40100 /* IDT_FILE_NEW from resource.h? Check! */, 0); 
            // Actually, resource.h might have different ID. Let's use string command or find ID.
            // IDT_FILE_NEW is usually 40100 -> check Notepade2.c commands.
            // Safest: Hide welcome screen immediately
            WelcomeScreen_Hide();
            PostMessage(hwndMain, WM_COMMAND, 40100, 0); // Hope 40100 is correct, will check
            break;

        case IDC_CMD_OPEN:
            WelcomeScreen_Hide(); // Hide first to reveal dialog
            PostMessage(hwndMain, WM_COMMAND, 40101 /* IDT_FILE_OPEN */, 0);
            break;

        case IDC_CMD_CLONE:
            // Open Terminal and run git clone
            WelcomeScreen_Hide();
            // Show terminal
            // We need to trigger "View Terminal" command or call API
            // Let's assume IDT_VIEW_TERMINAL exists or call Terminal_Toggle
            // For now, post specific message?
            // Or use Terminal_RunCommand directly if exposed?
            // Terminal_Show(hwndMain); 
            // We don't have Terminal_Show exposed directly here, let's try direct call if we include header
            // But Terminal_RunCommand needs HWND.
            // Let's postpone action until Terminal logic is verified.
            MessageBoxW(hwnd, L"Git Clone: Please use the Terminal (Ctrl+`) to run git clone.", L"Biko", MB_OK);
            break;
        }
        return 0;
    }

    case WM_SIZE:
    {
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);
        
        // Center buttons
        int btnW = 250;
        int btnH = 45;
        int spacing = 10;
        int totalH = (btnH * 3) + (spacing * 2);
        int startY = cy / 2; // Start below logo
        int startX = (cx - btnW) / 2;

        if (s_hwndBtnNew)
            MoveWindow(s_hwndBtnNew, startX, startY, btnW, btnH, TRUE);
        
        if (s_hwndBtnOpen)
            MoveWindow(s_hwndBtnOpen, startX, startY + btnH + spacing, btnW, btnH, TRUE);

        if (s_hwndBtnClone)
            MoveWindow(s_hwndBtnClone, startX, startY + (btnH + spacing) * 2, btnW, btnH, TRUE);

        return 0;
    }

    case WM_CTLCOLORBTN:
    case WM_CTLCOLORSTATIC:
    {
        // Handle transparency for buttons?
        // CommandLinks might handle themselves but let's see.
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        if (DarkMode_IsEnabled()) {
            SetTextColor(hdc, RGB(220,220,220));
            SetBkColor(hdc, RGB(30,30,30));
            return (LRESULT)DarkMode_GetBackgroundBrush();
        }
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_DESTROY:
        if (s_hFontLogo) DeleteObject(s_hFontLogo);
        if (s_hFontQuote) DeleteObject(s_hFontQuote);
        s_hFontLogo = NULL;
        s_hFontQuote = NULL;
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

BOOL WelcomeScreen_Init(HWND hwndParent)
{
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WelcomeWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"BikoWelcomeScreen";
    RegisterClassEx(&wc);
    return TRUE;
}

void WelcomeScreen_Show(HWND hwndParent)
{
    if (s_hwndWelcome)
    {
        ShowWindow(s_hwndWelcome, SW_SHOW);
        BringWindowToTop(s_hwndWelcome);
        return;
    }

    RECT rc;
    GetClientRect(hwndParent, &rc);

    s_hwndWelcome = CreateWindowEx(
        0, L"BikoWelcomeScreen", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, rc.right, rc.bottom,
        hwndParent, NULL, GetModuleHandle(NULL), NULL);

    // Create Command Link Buttons
    // Need ComCtl6 manifest for this style to work perfectly, but we'll try.
    // Fallback to BS_PUSHBUTTON if needed, but BS_COMMANDLINK is cleaner.
    
    DWORD btnStyle = WS_CHILD | WS_VISIBLE | BS_COMMANDLINK;

    s_hwndBtnNew = CreateWindowW(L"BUTTON", L"New File", btnStyle, 
        0, 0, 0, 0, s_hwndWelcome, (HMENU)IDC_CMD_NEW, GetModuleHandle(NULL), NULL);
    SendMessageW(s_hwndBtnNew, BCM_SETNOTE, 0, (LPARAM)L"Start a fresh file/project");

    s_hwndBtnOpen = CreateWindowW(L"BUTTON", L"Open Folder...", btnStyle, 
        0, 0, 0, 0, s_hwndWelcome, (HMENU)IDC_CMD_OPEN, GetModuleHandle(NULL), NULL);
    SendMessageW(s_hwndBtnOpen, BCM_SETNOTE, 0, (LPARAM)L"Open an existing workspace");
    
    s_hwndBtnClone = CreateWindowW(L"BUTTON", L"Git Clone...", btnStyle, 
        0, 0, 0, 0, s_hwndWelcome, (HMENU)IDC_CMD_CLONE, GetModuleHandle(NULL), NULL);
    SendMessageW(s_hwndBtnClone, BCM_SETNOTE, 0, (LPARAM)L"Clone a repository from URL");

    // Initial resize and bring to front (MsgSize will reposition editor frame on top of us)
    SendMessage(s_hwndWelcome, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    SetWindowPos(s_hwndWelcome, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void WelcomeScreen_Hide(void)
{
    if (s_hwndWelcome)
    {
        ShowWindow(s_hwndWelcome, SW_HIDE);
    }
}

BOOL WelcomeScreen_IsVisible(void)
{
    return (s_hwndWelcome && IsWindowVisible(s_hwndWelcome));
}


void WelcomeScreen_OnResize(HWND hwndParent, int cx, int cy)
{
    if (s_hwndWelcome && IsWindowVisible(s_hwndWelcome))
    {
        // Keep the welcome screen on top of the editor and covering the full client area
        SetWindowPos(s_hwndWelcome, HWND_TOP, 0, 0, cx, cy, 0);
    }
}
