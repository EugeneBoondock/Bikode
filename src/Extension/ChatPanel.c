/******************************************************************************
*
* Biko
*
* ChatPanel.c
*   World-class dockable chat panel.
*   Uses a Scintilla control for rich output and a custom input area.
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
// Design constants
//=============================================================================

#define CHAT_PANEL_WIDTH        380
#define CHAT_HEADER_HEIGHT      36
#define CHAT_INPUT_AREA_HEIGHT  80
#define CHAT_INPUT_PADDING      8
#define CHAT_SPLITTER_WIDTH     3
#define CHAT_SEND_SIZE          28

// Colors - Dark mode (monochrome black & white)
#define CLR_DK_SURFACE       RGB(13, 13, 13)
#define CLR_DK_SURFACE2      RGB(20, 20, 20)
#define CLR_DK_HEADER        RGB(18, 18, 18)
#define CLR_DK_INPUT_BG      RGB(28, 28, 28)
#define CLR_DK_INPUT_BORDER  RGB(55, 55, 55)
#define CLR_DK_TEXT          RGB(230, 230, 230)
#define CLR_DK_TEXT_DIM      RGB(110, 110, 110)
#define CLR_DK_ACCENT        RGB(255, 255, 255)
#define CLR_DK_USER_TEXT     RGB(255, 255, 255)
#define CLR_DK_AI_TEXT       RGB(200, 200, 200)
#define CLR_DK_SYSTEM_TEXT   RGB(80, 80, 80)
#define CLR_DK_SPLITTER      RGB(40, 40, 40)
#define CLR_DK_SEND_BG      RGB(230, 230, 230)
#define CLR_DK_SEND_HOVER   RGB(255, 255, 255)

// Colors - Light mode (monochrome black & white)
#define CLR_LT_SURFACE       RGB(255, 255, 255)
#define CLR_LT_SURFACE2      RGB(248, 248, 248)
#define CLR_LT_HEADER        RGB(250, 250, 250)
#define CLR_LT_INPUT_BG      RGB(242, 242, 242)
#define CLR_LT_INPUT_BORDER  RGB(200, 200, 200)
#define CLR_LT_TEXT          RGB(15, 15, 15)
#define CLR_LT_TEXT_DIM      RGB(120, 120, 120)
#define CLR_LT_ACCENT        RGB(0, 0, 0)
#define CLR_LT_USER_TEXT     RGB(0, 0, 0)
#define CLR_LT_AI_TEXT       RGB(50, 50, 50)
#define CLR_LT_SYSTEM_TEXT   RGB(160, 160, 160)
#define CLR_LT_SPLITTER      RGB(220, 220, 220)
#define CLR_LT_SEND_BG      RGB(20, 20, 20)
#define CLR_LT_SEND_HOVER   RGB(0, 0, 0)

//=============================================================================
// Internal state
//=============================================================================

static HWND     s_hwndPanel = NULL;
static HWND     s_hwndOutput = NULL;
static HWND     s_hwndInput = NULL;
static HWND     s_hwndSend = NULL;
static BOOL     s_bVisible = FALSE;
static int      s_iPanelWidth = CHAT_PANEL_WIDTH;
static WNDPROC  s_pfnOrigInputProc = NULL;
static WNDPROC  s_pfnOrigSendProc = NULL;
static BOOL     s_bSendHover = FALSE;
static HFONT    s_hFontHeader = NULL;
static HFONT    s_hFontInput = NULL;

#define MAX_CHAT_HISTORY 100
static char*    s_chatHistory[MAX_CHAT_HISTORY];
static int      s_iHistoryCount = 0;

//=============================================================================
// Forward declarations
//=============================================================================

static LRESULT CALLBACK ChatPanelWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ChatInputSubclassProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SendBtnProc(HWND, UINT, WPARAM, LPARAM);
static void SetupOutputStyles(HWND hwndOutput);
static void AppendToOutput(const char* prefix, const char* text, int style);
static void DrawSendButton(HDC hdc, RECT rc, BOOL bHover, BOOL bDark);

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
    wc.hbrBackground = NULL;
    wc.lpszClassName = CHAT_PANEL_CLASS;

    RegisterClassExW(&wc);
    s_bClassRegistered = TRUE;
}

static void CreateFonts(void)
{
    if (!s_hFontHeader)
    {
        s_hFontHeader = CreateFontW(
            -14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
    if (!s_hFontInput)
    {
        s_hFontInput = CreateFontW(
            -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    }
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
    CreateFonts();

    s_hwndPanel = CreateWindowExW(
        0, CHAT_PANEL_CLASS, L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwndParent,
        (HMENU)(UINT_PTR)IDC_CHAT_PANEL, hInst, NULL);

    if (!s_hwndPanel) return FALSE;

    // --- Scintilla output ---
    s_hwndOutput = CreateWindowExW(
        0, L"Scintilla", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 0, 0, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_OUTPUT, hInst, NULL);

    if (s_hwndOutput)
    {
        SetupOutputStyles(s_hwndOutput);
        SendMessage(s_hwndOutput, SCI_SETREADONLY, TRUE, 0);
        SendMessage(s_hwndOutput, SCI_SETWRAPMODE, SC_WRAP_WORD, 0);
    }

    // --- Multiline input ---
    s_hwndInput = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_INPUT, hInst, NULL);

    if (s_hwndInput)
    {
        s_pfnOrigInputProc = (WNDPROC)SetWindowLongPtrW(
            s_hwndInput, GWLP_WNDPROC, (LONG_PTR)ChatInputSubclassProc);
        SendMessage(s_hwndInput, WM_SETFONT, (WPARAM)s_hFontInput, TRUE);
        SendMessageW(s_hwndInput, EM_SETCUEBANNER, TRUE, (LPARAM)L"Message Biko...");
    }

    // --- Owner-drawn send button ---
    s_hwndSend = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, CHAT_SEND_SIZE, CHAT_SEND_SIZE, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_SEND, hInst, NULL);

    if (s_hwndSend)
    {
        s_pfnOrigSendProc = (WNDPROC)SetWindowLongPtrW(
            s_hwndSend, GWLP_WNDPROC, (LONG_PTR)SendBtnProc);
    }

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

    for (int i = 0; i < s_iHistoryCount; i++)
    {
        if (s_chatHistory[i]) n2e_Free(s_chatHistory[i]);
        s_chatHistory[i] = NULL;
    }
    s_iHistoryCount = 0;
    s_bVisible = FALSE;

    if (s_hFontHeader) { DeleteObject(s_hFontHeader); s_hFontHeader = NULL; }
    if (s_hFontInput) { DeleteObject(s_hFontInput); s_hFontInput = NULL; }
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
    if (panelLeft < 200) panelLeft = 200;

    int totalW = s_iPanelWidth;
    MoveWindow(s_hwndPanel, panelLeft, editorTop, totalW, editorHeight, TRUE);

    int innerW = totalW - CHAT_SPLITTER_WIDTH;
    int pad = CHAT_INPUT_PADDING;
    int x = CHAT_SPLITTER_WIDTH;

    // Header
    int y = CHAT_HEADER_HEIGHT;

    // Output
    int outputH = editorHeight - CHAT_HEADER_HEIGHT - CHAT_INPUT_AREA_HEIGHT;
    if (outputH < 80) outputH = 80;

    if (s_hwndOutput)
        MoveWindow(s_hwndOutput, x, y, innerW, outputH, TRUE);
    y += outputH;

    // Input area
    int inputH = CHAT_INPUT_AREA_HEIGHT - pad * 2;
    int sendBtnY = y + CHAT_INPUT_AREA_HEIGHT - pad - CHAT_SEND_SIZE;
    int inputW = innerW - pad * 2 - CHAT_SEND_SIZE - 6;

    if (s_hwndInput)
        MoveWindow(s_hwndInput, x + pad, y + pad, inputW, inputH, TRUE);
    if (s_hwndSend)
        MoveWindow(s_hwndSend, x + pad + inputW + 6, sendBtnY, CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

    return totalW;
}

//=============================================================================
// Public: Content
//=============================================================================

void ChatPanel_AppendUserMessage(const char* pszMessage)
{
    AppendToOutput("You", pszMessage, 20);
}

void ChatPanel_AppendResponse(const char* pszResponse)
{
    AppendToOutput("Biko", pszResponse, 21);
}

void ChatPanel_AppendSystem(const char* pszMessage)
{
    AppendToOutput("", pszMessage, 22);
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

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)n2e_Alloc(utf8Len);
    if (utf8)
    {
        WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8, utf8Len, NULL, NULL);

        ChatPanel_AppendUserMessage(utf8);

        const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
        if (pCfg && pCfg->szApiKey[0])
        {
            ChatPanel_AppendSystem("Thinking...");

            if (!AIAgent_ChatAsync(pCfg, utf8, s_hwndPanel))
            {
                ChatPanel_AppendSystem("AI is busy. Please wait.");
            }
        }
        else
        {
            ChatPanel_AppendSystem("No API key. Use Biko \xE2\x86\x92 AI Settings.");
        }

        n2e_Free(utf8);
    }
    n2e_Free(wszText);

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

    if (s_hwndOutput)
        SetupOutputStyles(s_hwndOutput);

    InvalidateRect(s_hwndPanel, NULL, TRUE);
    if (s_hwndInput) InvalidateRect(s_hwndInput, NULL, TRUE);
    if (s_hwndSend) InvalidateRect(s_hwndSend, NULL, TRUE);
}

//=============================================================================
// Internal: Output styling
//=============================================================================

static void SetupOutputStyles(HWND hwndOutput)
{
    BOOL bDark = DarkMode_IsEnabled();

    // Default style
    SendMessage(hwndOutput, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Cascadia Code");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    SendMessage(hwndOutput, SCI_STYLESETBACK, STYLE_DEFAULT,
        bDark ? CLR_DK_SURFACE : CLR_LT_SURFACE);
    SendMessage(hwndOutput, SCI_STYLESETFORE, STYLE_DEFAULT,
        bDark ? CLR_DK_TEXT : CLR_LT_TEXT);
    SendMessage(hwndOutput, SCI_STYLECLEARALL, 0, 0);

    // Caret
    SendMessage(hwndOutput, SCI_SETCARETFORE,
        bDark ? CLR_DK_TEXT : CLR_LT_TEXT, 0);

    // Selection
    SendMessage(hwndOutput, SCI_SETSELBACK, TRUE,
        bDark ? RGB(55, 55, 70) : RGB(180, 215, 255));
    SendMessage(hwndOutput, SCI_SETSELFORE, TRUE,
        bDark ? CLR_DK_TEXT : CLR_LT_TEXT);

    // Markdown base styles
    MarkdownPreview_SetupStyles(hwndOutput);

    // Style 20: User label
    SendMessage(hwndOutput, SCI_STYLESETFONT, 20, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 20, 10);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 20,
        bDark ? CLR_DK_USER_TEXT : CLR_LT_USER_TEXT);
    SendMessage(hwndOutput, SCI_STYLESETBOLD, 20, TRUE);
    SendMessage(hwndOutput, SCI_STYLESETBACK, 20,
        bDark ? CLR_DK_SURFACE : CLR_LT_SURFACE);

    // Style 21: AI label
    SendMessage(hwndOutput, SCI_STYLESETFONT, 21, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 21, 10);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 21,
        bDark ? CLR_DK_AI_TEXT : CLR_LT_AI_TEXT);
    SendMessage(hwndOutput, SCI_STYLESETBOLD, 21, TRUE);
    SendMessage(hwndOutput, SCI_STYLESETBACK, 21,
        bDark ? CLR_DK_SURFACE : CLR_LT_SURFACE);

    // Style 22: System (dim italic)
    SendMessage(hwndOutput, SCI_STYLESETFONT, 22, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 22, 9);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 22,
        bDark ? CLR_DK_SYSTEM_TEXT : CLR_LT_SYSTEM_TEXT);
    SendMessage(hwndOutput, SCI_STYLESETITALIC, 22, TRUE);
    SendMessage(hwndOutput, SCI_STYLESETBACK, 22,
        bDark ? CLR_DK_SURFACE : CLR_LT_SURFACE);

    // Left padding
    SendMessage(hwndOutput, SCI_SETMARGINWIDTHN, 0, 12);
    SendMessage(hwndOutput, SCI_SETMARGINWIDTHN, 1, 0);

    // Line spacing
    SendMessage(hwndOutput, SCI_SETEXTRAASCENT, 3, 0);
    SendMessage(hwndOutput, SCI_SETEXTRADESCENT, 3, 0);

    // Dark scrollbar
    if (bDark)
        SetWindowTheme(hwndOutput, L"DarkMode_Explorer", NULL);
    else
        SetWindowTheme(hwndOutput, NULL, NULL);
}

//=============================================================================
// Internal: Append to output
//=============================================================================

static void AppendToOutput(const char* prefix, const char* text, int style)
{
    if (!s_hwndOutput || !text) return;

    SendMessage(s_hwndOutput, SCI_SETREADONLY, FALSE, 0);

    int len = (int)SendMessage(s_hwndOutput, SCI_GETLENGTH, 0, 0);

    if (len > 0)
    {
        SendMessage(s_hwndOutput, SCI_APPENDTEXT, 2, (LPARAM)"\n\n");
        len += 2;
    }

    // Prefix on its own line
    if (prefix && prefix[0])
    {
        int prefixLen = (int)strlen(prefix);
        SendMessage(s_hwndOutput, SCI_APPENDTEXT, prefixLen, (LPARAM)prefix);

        int prefixStyle = (style == 21) ? 21 : (style == 20 ? 20 : 22);
        int docLen = (int)SendMessage(s_hwndOutput, SCI_GETLENGTH, 0, 0);
        if (len + prefixLen <= docLen && prefixLen > 0)
        {
            SendMessage(s_hwndOutput, SCI_STARTSTYLING, len, 0);
            SendMessage(s_hwndOutput, SCI_SETSTYLING, prefixLen, prefixStyle);
        }
        len += prefixLen;

        SendMessage(s_hwndOutput, SCI_APPENDTEXT, 1, (LPARAM)"\n");
        len += 1;
    }

    int textLen = (int)strlen(text);
    if (textLen <= 0) goto done;

    SendMessage(s_hwndOutput, SCI_APPENDTEXT, textLen, (LPARAM)text);

    if (style == 21 && textLen > 0)
    {
        SendMessage(s_hwndOutput, SCI_COLOURISE, 0, -1);
        MarkdownPreview_StyleRange(s_hwndOutput, len, textLen, text);
    }
    else if (textLen > 0)
    {
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

done:
    SendMessage(s_hwndOutput, SCI_SETREADONLY, TRUE, 0);
    SendMessage(s_hwndOutput, SCI_SCROLLTOEND, 0, 0);
}

//=============================================================================
// Internal: Custom drawing helpers
//=============================================================================

static void FillRoundRect(HDC hdc, RECT* prc, int radius, COLORREF fill)
{
    HBRUSH hBr = CreateSolidBrush(fill);
    HPEN hPen = CreatePen(PS_SOLID, 1, fill);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hBr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    RoundRect(hdc, prc->left, prc->top, prc->right, prc->bottom, radius, radius);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

static void DrawSendButton(HDC hdc, RECT rc, BOOL bHover, BOOL bDark)
{
    COLORREF bgClr = bHover
        ? (bDark ? CLR_DK_SEND_HOVER : CLR_LT_SEND_HOVER)
        : (bDark ? CLR_DK_SEND_BG : CLR_LT_SEND_BG);

    FillRoundRect(hdc, &rc, CHAT_SEND_SIZE, bgClr);

    // Arrow icon - contrast against button bg
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    int sz = 5;

    POINT pts[3] = {
        { cx - sz + 1, cy - sz },
        { cx + sz,     cy },
        { cx - sz + 1, cy + sz }
    };

    COLORREF arrowClr = bDark ? RGB(13, 13, 13) : RGB(255, 255, 255);
    HBRUSH hBr = CreateSolidBrush(arrowClr);
    HPEN hPen = CreatePen(PS_SOLID, 1, arrowClr);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hBr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
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
        BOOL bDark = DarkMode_IsEnabled();

        // Surface
        HBRUSH hBg = CreateSolidBrush(bDark ? CLR_DK_SURFACE : CLR_LT_SURFACE);
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);

        // Splitter
        RECT rcSplit = rc;
        rcSplit.right = CHAT_SPLITTER_WIDTH;
        HBRUSH hSplit = CreateSolidBrush(bDark ? CLR_DK_SPLITTER : CLR_LT_SPLITTER);
        FillRect(hdc, &rcSplit, hSplit);
        DeleteObject(hSplit);

        // Header
        RECT rcHeader = rc;
        rcHeader.left = CHAT_SPLITTER_WIDTH;
        rcHeader.bottom = CHAT_HEADER_HEIGHT;
        HBRUSH hHdr = CreateSolidBrush(bDark ? CLR_DK_HEADER : CLR_LT_HEADER);
        FillRect(hdc, &rcHeader, hHdr);
        DeleteObject(hHdr);

        // Header bottom line
        RECT rcLine = rcHeader;
        rcLine.top = rcLine.bottom - 1;
        HBRUSH hLine = CreateSolidBrush(bDark ? RGB(40, 40, 40) : RGB(220, 220, 220));
        FillRect(hdc, &rcLine, hLine);
        DeleteObject(hLine);

        // Header text
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, bDark ? CLR_DK_TEXT : CLR_LT_TEXT);
        if (s_hFontHeader) SelectObject(hdc, s_hFontHeader);
        RECT rcTitle = rcHeader;
        rcTitle.left += 14;
        rcTitle.right -= 40;
        DrawTextW(hdc, L"BIKO", -1, &rcTitle, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

        // Input area background
        int outputBottom = CHAT_HEADER_HEIGHT;
        if (s_hwndOutput)
        {
            RECT rcOut;
            GetWindowRect(s_hwndOutput, &rcOut);
            MapWindowPoints(NULL, hwnd, (POINT*)&rcOut, 2);
            outputBottom = rcOut.bottom;
        }
        RECT rcInputArea = rc;
        rcInputArea.top = outputBottom;
        rcInputArea.left = CHAT_SPLITTER_WIDTH;
        HBRUSH hInputBg = CreateSolidBrush(bDark ? CLR_DK_SURFACE2 : CLR_LT_SURFACE2);
        FillRect(hdc, &rcInputArea, hInputBg);
        DeleteObject(hInputBg);

        // Input top border
        RECT rcIBorder = rcInputArea;
        rcIBorder.bottom = rcIBorder.top + 1;
        HBRUSH hILine = CreateSolidBrush(bDark ? RGB(40, 40, 40) : RGB(220, 220, 220));
        FillRect(hdc, &rcIBorder, hILine);
        DeleteObject(hILine);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH hBr = CreateSolidBrush(
            DarkMode_IsEnabled() ? CLR_DK_SURFACE : CLR_LT_SURFACE);
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);
        return 1;
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdcEdit = (HDC)wParam;
        BOOL bDark = DarkMode_IsEnabled();
        SetTextColor(hdcEdit, bDark ? CLR_DK_TEXT : CLR_LT_TEXT);
        SetBkColor(hdcEdit, bDark ? CLR_DK_INPUT_BG : CLR_LT_INPUT_BG);
        SetBkMode(hdcEdit, OPAQUE);
        static HBRUSH s_hBrDarkEdit = NULL;
        static HBRUSH s_hBrLightEdit = NULL;
        if (bDark)
        {
            if (!s_hBrDarkEdit) s_hBrDarkEdit = CreateSolidBrush(CLR_DK_INPUT_BG);
            return (LRESULT)s_hBrDarkEdit;
        }
        else
        {
            if (!s_hBrLightEdit) s_hBrLightEdit = CreateSolidBrush(CLR_LT_INPUT_BG);
            return (LRESULT)s_hBrLightEdit;
        }
    }

    case WM_CTLCOLORSTATIC:
    {
        if (DarkMode_IsEnabled())
        {
            HBRUSH hBr = DarkMode_HandleCtlColor((HDC)wParam);
            if (hBr) return (LRESULT)hBr;
        }
        break;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
        if (pDIS && pDIS->CtlID == IDC_CHAT_SEND)
        {
            DrawSendButton(pDIS->hDC, pDIS->rcItem, s_bSendHover, DarkMode_IsEnabled());
            return TRUE;
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
            if (newWidth < 280) newWidth = 280;
            if (newWidth > rcParent.right - 200) newWidth = rcParent.right - 200;

            s_iPanelWidth = newWidth;

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
        // Enter sends; Shift+Enter inserts newline
        if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000))
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
        if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000))
            return 0;
        break;
    }

    return CallWindowProc(s_pfnOrigInputProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK SendBtnProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
    {
        if (!s_bSendHover)
        {
            s_bSendHover = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }
    case WM_MOUSELEAVE:
        s_bSendHover = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }

    return CallWindowProc(s_pfnOrigSendProc, hwnd, msg, wParam, lParam);
}
