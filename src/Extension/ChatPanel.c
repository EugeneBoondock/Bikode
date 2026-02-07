/******************************************************************************
*
* Biko
*
* ChatPanel.c
*   Dockable chat panel implementation.
*   Uses a Scintilla control for rich output and a standard edit for input.
*
******************************************************************************/

#include "ChatPanel.h"
#include "MarkdownPreview.h"
#include "AIBridge.h"
#include "AIAgent.h"
#include "AIDirectCall.h"
#include "AIContext.h"
#include "CommonUtils.h"
#include <commctrl.h>
#include <uxtheme.h>
#include "SciCall.h"
#include "Scintilla.h"
#include "DarkMode.h"
#include <string.h>
#include <stdio.h>

//=============================================================================
// Internal state
//=============================================================================

#define CHAT_PANEL_WIDTH    320
#define CHAT_INPUT_HEIGHT   28
#define CHAT_BUTTON_WIDTH   60
#define CHAT_SPLITTER_WIDTH 4

static HWND     s_hwndPanel = NULL;
static HWND     s_hwndOutput = NULL;    // Scintilla control for chat output
static HWND     s_hwndInput = NULL;     // Edit control for user input
static HWND     s_hwndSend = NULL;      // Send button
static BOOL     s_bVisible = FALSE;
static int      s_iPanelWidth = CHAT_PANEL_WIDTH;
static WNDPROC  s_pfnOrigInputProc = NULL;

// Chat history for context
#define MAX_CHAT_HISTORY 100
static char*    s_chatHistory[MAX_CHAT_HISTORY];
static int      s_iHistoryCount = 0;

//=============================================================================
// Forward declarations
//=============================================================================

static LRESULT CALLBACK ChatPanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK ChatInputSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void SetupOutputStyles(HWND hwndOutput);
static void AppendToOutput(const char* prefix, const char* text, int style);

//=============================================================================
// Panel window class
//=============================================================================

static const WCHAR* CHAT_PANEL_CLASS = L"BikoChatPanel";
static BOOL s_bClassRegistered = FALSE;

static void RegisterChatPanelClass(HINSTANCE hInst)
{
    if (s_bClassRegistered) return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ChatPanelWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = CHAT_PANEL_CLASS;

    RegisterClassExW(&wc);
    s_bClassRegistered = TRUE;
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

HWND ChatPanel_GetPanelHwnd(void)
{
    return s_hwndPanel;
}

BOOL ChatPanel_Create(HWND hwndParent)
{
    if (s_hwndPanel) return TRUE;

    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);
    RegisterChatPanelClass(hInst);

    // Create the panel container
    s_hwndPanel = CreateWindowExW(
        0,
        CHAT_PANEL_CLASS,
        L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        hwndParent,
        (HMENU)(UINT_PTR)IDC_CHAT_PANEL,
        hInst,
        NULL);

    if (!s_hwndPanel) return FALSE;

    // Create Scintilla output control
    s_hwndOutput = CreateWindowExW(
        0,
        L"Scintilla",
        L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 0, 0,
        s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_OUTPUT,
        hInst,
        NULL);

    if (s_hwndOutput)
    {
        SetupOutputStyles(s_hwndOutput);
        SendMessage(s_hwndOutput, SCI_SETREADONLY, TRUE, 0);
        SendMessage(s_hwndOutput, SCI_SETWRAPMODE, SC_WRAP_WORD, 0);
    }

    // Create input edit control
    s_hwndInput = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0,
        s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_INPUT,
        hInst,
        NULL);

    // Subclass input for Enter key handling
    if (s_hwndInput)
    {
        s_pfnOrigInputProc = (WNDPROC)SetWindowLongPtrW(
            s_hwndInput, GWLP_WNDPROC, (LONG_PTR)ChatInputSubclassProc);

        // Set placeholder text (cue banner)
        SendMessageW(s_hwndInput, EM_SETCUEBANNER, TRUE, (LPARAM)L"Ask AI something...");
    }

    // Create Send button
    s_hwndSend = CreateWindowExW(
        0,
        L"BUTTON",
        L"Send",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        0, 0, CHAT_BUTTON_WIDTH, CHAT_INPUT_HEIGHT,
        s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_SEND,
        hInst,
        NULL);

    ChatPanel_AppendSystem("Biko AI Ready. \"I write what I like.\"");
    return TRUE;
}

void ChatPanel_Destroy(void)
{
    if (s_hwndPanel)
    {
        DestroyWindow(s_hwndPanel);
        s_hwndPanel = NULL;
        s_hwndOutput = NULL;
        s_hwndInput = NULL;
        s_hwndSend = NULL;
    }

    // Free chat history
    for (int i = 0; i < s_iHistoryCount; i++)
    {
        if (s_chatHistory[i]) n2e_Free(s_chatHistory[i]);
        s_chatHistory[i] = NULL;
    }
    s_iHistoryCount = 0;
    s_bVisible = FALSE;
}

//=============================================================================
// Public: Visibility
//=============================================================================

void ChatPanel_Toggle(HWND hwndParent)
{
    if (s_bVisible)
        ChatPanel_Hide();
    else
        ChatPanel_Show(hwndParent);
}

void ChatPanel_Show(HWND hwndParent)
{
    if (!s_hwndPanel)
        ChatPanel_Create(hwndParent);

    if (s_hwndPanel)
    {
        ShowWindow(s_hwndPanel, SW_SHOW);
        s_bVisible = TRUE;

        // Trigger parent relayout
        RECT rc;
        GetClientRect(hwndParent, &rc);
        SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                    MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));

        ChatPanel_FocusInput();
    }
}

void ChatPanel_Hide(void)
{
    if (s_hwndPanel)
    {
        ShowWindow(s_hwndPanel, SW_HIDE);
        s_bVisible = FALSE;

        // Trigger parent relayout
        HWND hwndParent = GetParent(s_hwndPanel);
        if (hwndParent)
        {
            RECT rc;
            GetClientRect(hwndParent, &rc);
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
        }
    }
}

BOOL ChatPanel_IsVisible(void)
{
    return s_bVisible;
}

//=============================================================================
// Public: Layout
//=============================================================================

int ChatPanel_Layout(HWND hwndParent, int parentRight, int editorTop, int editorHeight)
{
    if (!s_bVisible || !s_hwndPanel) return 0;

    int panelLeft = parentRight - s_iPanelWidth;
    if (panelLeft < 200) panelLeft = 200; // minimum editor width

    int totalW = s_iPanelWidth;
    MoveWindow(s_hwndPanel, panelLeft, editorTop, totalW, editorHeight, TRUE);

    int innerW = totalW - CHAT_SPLITTER_WIDTH - 4;
    int x = CHAT_SPLITTER_WIDTH;
    int y = 4;
    int innerH = editorHeight - CHAT_INPUT_HEIGHT - 12;

    // Output area (fills most of the sidebar)
    if (s_hwndOutput)
        MoveWindow(s_hwndOutput, x, y, innerW, innerH, TRUE);
    y += innerH + 4;

    // Input + Send button at the bottom
    int inputW = innerW - CHAT_BUTTON_WIDTH - 4;
    if (s_hwndInput)
        MoveWindow(s_hwndInput, x, y, inputW, CHAT_INPUT_HEIGHT, TRUE);
    if (s_hwndSend)
        MoveWindow(s_hwndSend, x + inputW + 2, y, CHAT_BUTTON_WIDTH, CHAT_INPUT_HEIGHT, TRUE);

    return totalW;
}

//=============================================================================
// Public: Content
//=============================================================================

void ChatPanel_AppendUserMessage(const char* pszMessage)
{
    AppendToOutput("You: ", pszMessage, 20);
}

void ChatPanel_AppendResponse(const char* pszResponse)
{
    AppendToOutput("Biko: ", pszResponse, 21);
}

void ChatPanel_AppendSystem(const char* pszMessage)
{
    AppendToOutput("--- ", pszMessage, 22);
}

void ChatPanel_Clear(void)
{
    if (!s_hwndOutput) return;
    SendMessage(s_hwndOutput, SCI_SETREADONLY, FALSE, 0);
    SendMessage(s_hwndOutput, SCI_CLEARALL, 0, 0);
    SendMessage(s_hwndOutput, SCI_SETREADONLY, TRUE, 0);
}

//=============================================================================
// Public: Input
//=============================================================================

void ChatPanel_SendInput(void)
{
    if (!s_hwndInput) return;

    int len = GetWindowTextLengthW(s_hwndInput);
    if (len <= 0) return;

    WCHAR* wszText = (WCHAR*)n2e_Alloc((len + 1) * sizeof(WCHAR));
    if (!wszText) return;
    GetWindowTextW(s_hwndInput, wszText, len + 1);

    // Convert to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)n2e_Alloc(utf8Len);
    if (utf8)
    {
        WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8, utf8Len, NULL, NULL);

        // Show user message in output
        ChatPanel_AppendUserMessage(utf8);

        // Get provider config from AIBridge
        const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
        if (pCfg && pCfg->szApiKey[0])
        {
            ChatPanel_AppendSystem("Thinking...");

            // Launch agentic AI loop (reads/writes files, runs commands)
            if (!AIAgent_ChatAsync(pCfg, utf8, s_hwndPanel))
            {
                ChatPanel_AppendSystem("AI is busy with a previous request. Please wait.");
            }
        }
        else
        {
            ChatPanel_AppendSystem("No API key configured. Use Biko > AI Settings to set one.");
        }

        n2e_Free(utf8);
    }

    n2e_Free(wszText);

    // Clear input
    SetWindowTextW(s_hwndInput, L"");
}

void ChatPanel_FocusInput(void)
{
    if (s_hwndInput)
        SetFocus(s_hwndInput);
}

//=============================================================================
// Public: Command/Notify handlers
//=============================================================================

BOOL ChatPanel_HandleCommand(WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    if (LOWORD(wParam) == IDC_CHAT_SEND && HIWORD(wParam) == BN_CLICKED)
    {
        ChatPanel_SendInput();
        return TRUE;
    }
    return FALSE;
}

BOOL ChatPanel_HandleNotify(LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    return FALSE;
}

//=============================================================================
// Public: Dark mode refresh
//=============================================================================

void ChatPanel_ApplyDarkMode(void)
{
    if (!s_hwndPanel) return;

    BOOL bDark = DarkMode_IsEnabled();

    // Re-setup Scintilla output colors
    if (s_hwndOutput)
        SetupOutputStyles(s_hwndOutput);

    // Apply dark theme to the Edit input control
    if (s_hwndInput)
    {
        if (bDark)
        {
            SetWindowTheme(s_hwndInput, L"DarkMode_CFD", NULL);
        }
        else
        {
            SetWindowTheme(s_hwndInput, NULL, NULL);
        }
        InvalidateRect(s_hwndInput, NULL, TRUE);
    }

    // Apply dark theme to the Send button
    if (s_hwndSend)
    {
        if (bDark)
        {
            SetWindowTheme(s_hwndSend, L"DarkMode_Explorer", NULL);
        }
        else
        {
            SetWindowTheme(s_hwndSend, NULL, NULL);
        }
        InvalidateRect(s_hwndSend, NULL, TRUE);
    }

    // Repaint the panel itself
    InvalidateRect(s_hwndPanel, NULL, TRUE);
}

//=============================================================================
// Internal: Output styling
//=============================================================================

static void SetupOutputStyles(HWND hwndOutput)
{
    // Base markdown styles
    MarkdownPreview_SetupStyles(hwndOutput);

    // Chat specific styles (using indices > 20 to avoid conflict with MD styles)
    // Style 20: User messages
    SendMessage(hwndOutput, SCI_STYLESETFONT, 20, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 20, 10);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 20, RGB(30, 80, 180));
    SendMessage(hwndOutput, SCI_STYLESETBOLD, 20, TRUE);

    // Style 21: AI Prefix
    SendMessage(hwndOutput, SCI_STYLESETFONT, 21, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 21, 10);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 21, RGB(20, 120, 20)); // Greenish
    SendMessage(hwndOutput, SCI_STYLESETBOLD, 21, TRUE);

    // Style 22: System messages
    SendMessage(hwndOutput, SCI_STYLESETFONT, 22, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 22, 10);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 22, RGB(160, 160, 160));
    SendMessage(hwndOutput, SCI_STYLESETITALIC, 22, TRUE);

    // Apply dark mode overrides
    if (DarkMode_IsEnabled())
    {
        SendMessage(hwndOutput, SCI_STYLESETFORE, 20, RGB(100, 160, 255)); // User (lighter blue)
        SendMessage(hwndOutput, SCI_STYLESETFORE, 21, RGB(80, 200, 80));   // AI (lighter green)
        SendMessage(hwndOutput, SCI_STYLESETFORE, 22, RGB(120, 120, 120)); // System
    }

    // Margins - add some padding
    SendMessage(hwndOutput, SCI_SETMARGINWIDTHN, 0, 8); // left padding
    SendMessage(hwndOutput, SCI_SETMARGINWIDTHN, 1, 0);

    // Extra line spacing for readability
    SendMessage(hwndOutput, SCI_SETEXTRAASCENT, 2, 0);
    SendMessage(hwndOutput, SCI_SETEXTRADESCENT, 2, 0);
}

static void AppendToOutput(const char* prefix, const char* text, int style)
{
    if (!s_hwndOutput || !text) return;

    SendMessage(s_hwndOutput, SCI_SETREADONLY, FALSE, 0);

    int len = (int)SendMessage(s_hwndOutput, SCI_GETLENGTH, 0, 0);
    if (len > 0)
    {
        SendMessage(s_hwndOutput, SCI_APPENDTEXT, 1, (LPARAM)"\n\n"); // More spacing between messages
        len += 2;
    }

    // Append prefix
    if (prefix && prefix[0])
    {
        int prefixLen = (int)strlen(prefix);
        SendMessage(s_hwndOutput, SCI_APPENDTEXT, prefixLen, (LPARAM)prefix);

        // Style the prefix (use relevant style or default to System if unknown)
        int prefixStyle = (style == 21) ? 21 : (style == 20 ? 20 : 22);
        
        int docLen = (int)SendMessage(s_hwndOutput, SCI_GETLENGTH, 0, 0);
        if (len + prefixLen <= docLen && prefixLen > 0)
        {
            SendMessage(s_hwndOutput, SCI_STARTSTYLING, len, 0);
            SendMessage(s_hwndOutput, SCI_SETSTYLING, prefixLen, prefixStyle);
        }
        len += prefixLen;
    }

    // Append text
    int textLen = (int)strlen(text);
    SendMessage(s_hwndOutput, SCI_APPENDTEXT, textLen, (LPARAM)text);

    // Style the text
    if (style == 21 && textLen > 0) // AI Message -> Markdown
    {
        // Use the Markdown renderer
        // Ensure styles are allocated first by calling SCI_COLOURISE
        SendMessage(s_hwndOutput, SCI_COLOURISE, 0, -1);
        MarkdownPreview_StyleRange(s_hwndOutput, len, textLen, text);
    }
    else if (textLen > 0)
    {
        // Plain styling - verify bounds
        int docLen = (int)SendMessage(s_hwndOutput, SCI_GETLENGTH, 0, 0);
        int styleLen = textLen;
        if (len + styleLen > docLen)
            styleLen = docLen - len;
        if (styleLen > 0)
        {
            SendMessage(s_hwndOutput, SCI_STARTSTYLING, len, 0);
            SendMessage(s_hwndOutput, SCI_SETSTYLING, styleLen, style);
        }
    }

    SendMessage(s_hwndOutput, SCI_SETREADONLY, TRUE, 0);
    SendMessage(s_hwndOutput, SCI_SCROLLTOEND, 0, 0);
}

//=============================================================================
// Internal: Window procedures
//=============================================================================

static LRESULT CALLBACK ChatPanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);

        // Fill entire panel background with dark/light surface color
        HBRUSH hBgBrush = CreateSolidBrush(
            DarkMode_IsEnabled() ? RGB(37, 37, 38) : RGB(243, 243, 243));
        FillRect(hdc, &rc, hBgBrush);
        DeleteObject(hBgBrush);

        // Draw vertical splitter handle at left edge
        rc.right = CHAT_SPLITTER_WIDTH;
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

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
        if (DarkMode_IsEnabled())
        {
            HBRUSH hBr = DarkMode_HandleCtlColor((HDC)wParam);
            if (hBr) return (LRESULT)hBr;
        }
        break;
    }

    case WM_SETCURSOR:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (pt.x < CHAT_SPLITTER_WIDTH)
        {
            SetCursor(LoadCursor(NULL, IDC_SIZEWE));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        int x = LOWORD(lParam);
        if (x < CHAT_SPLITTER_WIDTH)
        {
            SetCapture(hwnd);
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

            int newWidth = rcParent.right - pt.x;
            if (newWidth < 200) newWidth = 200;
            if (newWidth > rcParent.right - 200) newWidth = rcParent.right - 200;

            s_iPanelWidth = newWidth;

            // Force relayout
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rcParent.right, rcParent.bottom));
        }
        break;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd)
            ReleaseCapture();
        break;

    case WM_COMMAND:
        if (ChatPanel_HandleCommand(wParam, lParam))
            return 0;
        break;

    default:
        // Handle AI direct response from background thread
        if (msg == WM_AI_DIRECT_RESPONSE)
        {
            char* pszResponse = (char*)lParam;
            if (pszResponse)
            {
                ChatPanel_AppendResponse(pszResponse);
                free(pszResponse);
            }
            return 0;
        }
        // Handle agent status updates
        if (msg == WM_AI_AGENT_STATUS)
        {
            char* pszStatus = (char*)lParam;
            if (pszStatus)
            {
                ChatPanel_AppendSystem(pszStatus);
                free(pszStatus);
            }
            return 0;
        }
        // Handle agent tool call display
        if (msg == WM_AI_AGENT_TOOL)
        {
            char* pszTool = (char*)lParam;
            if (pszTool)
            {
                AppendToOutput("", pszTool, 22);
                free(pszTool);
            }
            return 0;
        }
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK ChatInputSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN)
        {
            ChatPanel_SendInput();
            return 0;
        }
        if (wParam == VK_ESCAPE)
        {
            ChatPanel_Hide();
            return 0;
        }
        break;

    case WM_CHAR:
        if (wParam == VK_RETURN)
            return 0; // Suppress the beep
        break;
    }

    return CallWindowProc(s_pfnOrigInputProc, hwnd, msg, wParam, lParam);
}
