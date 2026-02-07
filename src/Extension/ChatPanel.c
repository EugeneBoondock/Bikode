/******************************************************************************
*
* Biko
*
* ChatPanel.c
*   Premium dockable chat panel with world-class owner-drawn UI.
*   Design language mirrors WelcomeScreen: cards, accent stripes,
*   font hierarchy, double-buffered rendering, rounded rects with borders.
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
// Color palette  (mirrors WelcomeScreen design language)
//=============================================================================

#define C(dk, lt) (DarkMode_IsEnabled() ? (dk) : (lt))

// --- Dark mode ---
#define DK_BG            RGB(18, 18, 18)
#define DK_SURFACE       RGB(24, 24, 24)
#define DK_SURFACE2      RGB(30, 30, 32)
#define DK_HEADER        RGB(20, 20, 20)
#define DK_BORDER        RGB(48, 48, 48)
#define DK_BORDER_HOV    RGB(65, 65, 68)
#define DK_BORDER_FOCUS  RGB(75, 139, 245)
#define DK_TEXT1         RGB(230, 230, 230)
#define DK_TEXT2         RGB(150, 150, 150)
#define DK_MUTED         RGB(80, 80, 80)
#define DK_ACCENT        RGB(75, 139, 245)
#define DK_ACCENT_HOV    RGB(100, 160, 255)
#define DK_DIVIDER       RGB(40, 40, 40)
#define DK_INPUT_BG      RGB(28, 28, 30)
#define DK_INPUT_BD      RGB(55, 55, 58)
#define DK_CLOSE_HOV     RGB(42, 42, 45)
#define DK_BADGE_BG      RGB(42, 42, 42)
#define DK_BADGE_BD      RGB(60, 60, 60)

// --- Light mode ---
#define LT_BG            RGB(248, 249, 251)
#define LT_SURFACE       RGB(255, 255, 255)
#define LT_SURFACE2      RGB(245, 246, 248)
#define LT_HEADER        RGB(250, 250, 252)
#define LT_BORDER        RGB(215, 215, 218)
#define LT_BORDER_HOV    RGB(190, 190, 195)
#define LT_BORDER_FOCUS  RGB(50, 110, 215)
#define LT_TEXT1         RGB(28, 28, 28)
#define LT_TEXT2         RGB(100, 100, 100)
#define LT_MUTED         RGB(165, 165, 165)
#define LT_ACCENT        RGB(50, 110, 215)
#define LT_ACCENT_HOV    RGB(30, 90, 195)
#define LT_DIVIDER       RGB(228, 228, 228)
#define LT_INPUT_BG      RGB(242, 243, 245)
#define LT_INPUT_BD      RGB(200, 200, 205)
#define LT_CLOSE_HOV     RGB(232, 232, 235)
#define LT_BADGE_BG      RGB(238, 238, 240)
#define LT_BADGE_BD      RGB(210, 210, 215)

//=============================================================================
// Layout constants
//=============================================================================

#define CHAT_PANEL_WIDTH        380
#define CHAT_HEADER_HEIGHT      44
#define CHAT_INPUT_AREA_HEIGHT  92
#define CHAT_INPUT_PAD          10
#define CHAT_INPUT_INNER_PAD    10
#define CHAT_INPUT_RADIUS       10
#define CHAT_SEND_SIZE          30
#define CHAT_SPLITTER_WIDTH     1
#define CHAT_HDR_BTN_SIZE       28
#define CHAT_STATUS_DOT_R       4

//=============================================================================
// Internal state
//=============================================================================

static HWND     s_hwndPanel   = NULL;
static HWND     s_hwndOutput  = NULL;
static HWND     s_hwndInput   = NULL;
static HWND     s_hwndSend    = NULL;
static BOOL     s_bVisible    = FALSE;
static int      s_iPanelWidth = CHAT_PANEL_WIDTH;
static WNDPROC  s_pfnOrigInputProc = NULL;
static WNDPROC  s_pfnOrigSendProc  = NULL;
static BOOL     s_bSendHover  = FALSE;
static BOOL     s_bInputFocused = FALSE;

// Header close button
static RECT     s_rcCloseBtn   = { 0 };
static BOOL     s_bCloseHover  = FALSE;

// Fonts
static HFONT    s_hFontHeader  = NULL;
static HFONT    s_hFontInput   = NULL;
static HFONT    s_hFontStatus  = NULL;

// Chat history
#define MAX_CHAT_HISTORY 100
static char*    s_chatHistory[MAX_CHAT_HISTORY];
static int      s_iHistoryCount = 0;

// Input card rect (computed in Layout, used in Paint)
static RECT     s_rcInputCard  = { 0 };

//=============================================================================
// Forward declarations
//=============================================================================

static LRESULT CALLBACK ChatPanelWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ChatInputSubclassProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SendBtnProc(HWND, UINT, WPARAM, LPARAM);
static void SetupOutputStyles(HWND hwndOutput);
static void AppendToOutput(const char* prefix, const char* text, int style);

//=============================================================================
// Class registration
//=============================================================================

static const WCHAR* CHAT_PANEL_CLASS = L"BikoChatPanel";
static BOOL s_bClassRegistered = FALSE;

static void RegisterChatPanelClass(HINSTANCE hInst)
{
    if (s_bClassRegistered) return;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = ChatPanelWndProc;
    wc.hInstance      = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName  = CHAT_PANEL_CLASS;

    RegisterClassExW(&wc);
    s_bClassRegistered = TRUE;
}

//=============================================================================
// Font hierarchy
//=============================================================================

static void CreateFonts(void)
{
    if (s_hFontHeader) return;

    // Header title: Segoe UI Semibold, 13pt
    s_hFontHeader = CreateFontW(
        -14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");
    if (!s_hFontHeader)
        s_hFontHeader = CreateFontW(
            -14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // Input field text: Segoe UI, 13pt
    s_hFontInput = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // Status / small text: Segoe UI, 11pt
    s_hFontStatus = CreateFontW(
        -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void DestroyFonts(void)
{
    if (s_hFontHeader) { DeleteObject(s_hFontHeader); s_hFontHeader = NULL; }
    if (s_hFontInput)  { DeleteObject(s_hFontInput);  s_hFontInput  = NULL; }
    if (s_hFontStatus) { DeleteObject(s_hFontStatus); s_hFontStatus = NULL; }
}

//=============================================================================
// Drawing helpers  (WelcomeScreen design pattern)
//=============================================================================

// Rounded rect with BOTH fill and border colors
static void FillRoundRect(HDC hdc, const RECT* prc, int r,
                          COLORREF fill, COLORREF border)
{
    HBRUSH hBr  = CreateSolidBrush(fill);
    HPEN   hPen = CreatePen(PS_SOLID, 1, border);
    HBRUSH hOldBr  = (HBRUSH)SelectObject(hdc, hBr);
    HPEN   hOldPen = (HPEN)SelectObject(hdc, hPen);
    RoundRect(hdc, prc->left, prc->top, prc->right, prc->bottom, r * 2, r * 2);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

// Filled rounded rect (no distinct border)
static void FillRoundRectSolid(HDC hdc, const RECT* prc, int r, COLORREF fill)
{
    FillRoundRect(hdc, prc, r, fill, fill);
}

// Accent status dot
static void DrawStatusDot(HDC hdc, int cx, int cy, int r, COLORREF clr)
{
    HBRUSH hBr = CreateSolidBrush(clr);
    HPEN hPen  = CreatePen(PS_SOLID, 1, clr);
    HBRUSH hOldBr  = (HBRUSH)SelectObject(hdc, hBr);
    HPEN   hOldPen = (HPEN)SelectObject(hdc, hPen);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

// Clean upward arrow for send button
static void DrawSendArrow(HDC hdc, int cx, int cy, COLORREF clr)
{
    HPEN hPen = CreatePen(PS_SOLID, 2, clr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

    // Vertical shaft
    MoveToEx(hdc, cx, cy + 5, NULL);
    LineTo(hdc, cx, cy - 5);

    // Arrowhead left wing
    MoveToEx(hdc, cx, cy - 5, NULL);
    LineTo(hdc, cx - 4, cy - 1);

    // Arrowhead right wing
    MoveToEx(hdc, cx, cy - 5, NULL);
    LineTo(hdc, cx + 4, cy - 1);

    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

// Close X drawn with two diagonal lines
static void DrawCloseX(HDC hdc, const RECT* prc, COLORREF clr)
{
    int cx = (prc->left + prc->right) / 2;
    int cy = (prc->top + prc->bottom) / 2;
    int d = 5;

    HPEN hPen = CreatePen(PS_SOLID, 1, clr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

    MoveToEx(hdc, cx - d, cy - d, NULL);
    LineTo(hdc, cx + d + 1, cy + d + 1);
    MoveToEx(hdc, cx + d, cy - d, NULL);
    LineTo(hdc, cx - d - 1, cy + d + 1);

    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
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

    // --- Multiline input (borderless -- we draw our own card) ---
    s_hwndInput = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP
        | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_INPUT, hInst, NULL);

    if (s_hwndInput)
    {
        s_pfnOrigInputProc = (WNDPROC)SetWindowLongPtrW(
            s_hwndInput, GWLP_WNDPROC, (LONG_PTR)ChatInputSubclassProc);
        SendMessage(s_hwndInput, WM_SETFONT, (WPARAM)s_hFontInput, TRUE);
        SendMessageW(s_hwndInput, EM_SETCUEBANNER, TRUE,
                     (LPARAM)L"Message Biko\x2026");
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

    ChatPanel_AppendSystem("Biko AI \xe2\x80\xa2 Ready");
    return TRUE;
}

void ChatPanel_Destroy(void)
{
    if (s_hwndPanel)
    {
        DestroyWindow(s_hwndPanel);
        s_hwndPanel  = NULL;
        s_hwndOutput = NULL;
        s_hwndInput  = NULL;
        s_hwndSend   = NULL;
    }

    for (int i = 0; i < s_iHistoryCount; i++)
    {
        if (s_chatHistory[i]) n2e_Free(s_chatHistory[i]);
        s_chatHistory[i] = NULL;
    }
    s_iHistoryCount = 0;
    s_bVisible = FALSE;

    DestroyFonts();
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

int ChatPanel_Layout(HWND hwndParent, int parentRight, int editorTop,
                     int editorHeight)
{
    if (!s_bVisible || !s_hwndPanel) return 0;

    int panelLeft = parentRight - s_iPanelWidth;
    if (panelLeft < 200) panelLeft = 200;

    int totalW = s_iPanelWidth;
    MoveWindow(s_hwndPanel, panelLeft, editorTop, totalW, editorHeight, TRUE);

    int x = CHAT_SPLITTER_WIDTH;
    int innerW = totalW - CHAT_SPLITTER_WIDTH;

    // Header
    int y = CHAT_HEADER_HEIGHT;

    // Close button rect (right side of header)
    int cbSz = CHAT_HDR_BTN_SIZE;
    s_rcCloseBtn.right  = totalW - 8;
    s_rcCloseBtn.left   = s_rcCloseBtn.right - cbSz;
    s_rcCloseBtn.top    = (CHAT_HEADER_HEIGHT - cbSz) / 2;
    s_rcCloseBtn.bottom = s_rcCloseBtn.top + cbSz;

    // Output
    int outputH = editorHeight - CHAT_HEADER_HEIGHT - CHAT_INPUT_AREA_HEIGHT;
    if (outputH < 80) outputH = 80;

    if (s_hwndOutput)
        MoveWindow(s_hwndOutput, x, y, innerW, outputH, TRUE);
    y += outputH;

    // Input card
    int cardLeft   = x + CHAT_INPUT_PAD;
    int cardTop    = y + 8;
    int cardRight  = x + innerW - CHAT_INPUT_PAD;
    int cardBottom = y + CHAT_INPUT_AREA_HEIGHT - 8;

    s_rcInputCard.left   = cardLeft;
    s_rcInputCard.top    = cardTop;
    s_rcInputCard.right  = cardRight;
    s_rcInputCard.bottom = cardBottom;

    // EDIT inside the card with internal padding
    int ip = CHAT_INPUT_INNER_PAD;
    int editLeft   = cardLeft + ip;
    int editTop    = cardTop + ip;
    int editRight  = cardRight - ip - CHAT_SEND_SIZE - 6;
    int editBottom = cardBottom - ip;

    if (s_hwndInput)
        MoveWindow(s_hwndInput, editLeft, editTop,
                   editRight - editLeft, editBottom - editTop, TRUE);

    // Send button: bottom-right inside card
    int sendLeft = cardRight - ip - CHAT_SEND_SIZE;
    int sendTop  = cardBottom - ip - CHAT_SEND_SIZE;

    if (s_hwndSend)
        MoveWindow(s_hwndSend, sendLeft, sendTop,
                   CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

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
    char* utf8  = (char*)n2e_Alloc(utf8Len);
    if (utf8)
    {
        WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8, utf8Len, NULL, NULL);

        ChatPanel_AppendUserMessage(utf8);

        const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
        if (pCfg && pCfg->szApiKey[0])
        {
            ChatPanel_AppendSystem("Thinking\xe2\x80\xa6");

            if (!AIAgent_ChatAsync(pCfg, utf8, s_hwndPanel))
            {
                ChatPanel_AppendSystem("AI is busy. Please wait.");
            }
        }
        else
        {
            ChatPanel_AppendSystem("No API key. Use Biko \xe2\x86\x92 AI Settings.");
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
// Public: Command / Notify
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
    if (s_hwndSend)  InvalidateRect(s_hwndSend, NULL, TRUE);
}

//=============================================================================
// Internal: Scintilla output styling
//=============================================================================

static void SetupOutputStyles(HWND hwndOutput)
{
    BOOL bDark = DarkMode_IsEnabled();
    COLORREF bgClr = bDark ? DK_SURFACE : LT_SURFACE;

    // Default style
    SendMessage(hwndOutput, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
    SendMessage(hwndOutput, SCI_STYLESETBACK, STYLE_DEFAULT, bgClr);
    SendMessage(hwndOutput, SCI_STYLESETFORE, STYLE_DEFAULT,
                bDark ? DK_TEXT1 : LT_TEXT1);
    SendMessage(hwndOutput, SCI_STYLECLEARALL, 0, 0);

    // Caret
    SendMessage(hwndOutput, SCI_SETCARETFORE,
                bDark ? DK_TEXT1 : LT_TEXT1, 0);

    // Selection
    SendMessage(hwndOutput, SCI_SETSELBACK, TRUE,
                bDark ? RGB(50, 60, 85) : RGB(180, 215, 255));
    SendMessage(hwndOutput, SCI_SETSELFORE, TRUE,
                bDark ? DK_TEXT1 : LT_TEXT1);

    // Markdown base styles
    MarkdownPreview_SetupStyles(hwndOutput);

    // Style 20: User label (accent, bold)
    SendMessage(hwndOutput, SCI_STYLESETFONT, 20, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 20, 10);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 20,
                bDark ? DK_ACCENT : LT_ACCENT);
    SendMessage(hwndOutput, SCI_STYLESETBOLD, 20, TRUE);
    SendMessage(hwndOutput, SCI_STYLESETBACK, 20, bgClr);

    // Style 21: AI label (primary text, bold)
    SendMessage(hwndOutput, SCI_STYLESETFONT, 21, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 21, 10);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 21,
                bDark ? DK_TEXT1 : LT_TEXT1);
    SendMessage(hwndOutput, SCI_STYLESETBOLD, 21, TRUE);
    SendMessage(hwndOutput, SCI_STYLESETBACK, 21, bgClr);

    // Style 22: System (muted italic)
    SendMessage(hwndOutput, SCI_STYLESETFONT, 22, (LPARAM)"Segoe UI");
    SendMessage(hwndOutput, SCI_STYLESETSIZE, 22, 9);
    SendMessage(hwndOutput, SCI_STYLESETFORE, 22,
                bDark ? DK_MUTED : LT_MUTED);
    SendMessage(hwndOutput, SCI_STYLESETITALIC, 22, TRUE);
    SendMessage(hwndOutput, SCI_STYLESETBACK, 22, bgClr);

    // Left margin
    SendMessage(hwndOutput, SCI_SETMARGINWIDTHN, 0, 14);
    SendMessage(hwndOutput, SCI_SETMARGINWIDTHN, 1, 0);

    // Extra line spacing
    SendMessage(hwndOutput, SCI_SETEXTRAASCENT, 3, 0);
    SendMessage(hwndOutput, SCI_SETEXTRADESCENT, 3, 0);

    // Scrollbar theme
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

    // Blank line separator
    if (len > 0)
    {
        SendMessage(s_hwndOutput, SCI_APPENDTEXT, 2, (LPARAM)"\n\n");
        len += 2;
    }

    // Prefix label
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

    // Message body
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
// Internal: Panel window procedure  (double-buffered, owner-drawn)
//=============================================================================

static LRESULT CALLBACK ChatPanelWndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;  // All painting in WM_PAINT

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cxW = rc.right, cyH = rc.bottom;
        BOOL bDark = DarkMode_IsEnabled();

        // --- Double-buffer ---
        HDC hm = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc, cxW, cyH);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hm, hBmp);

        SetBkMode(hm, TRANSPARENT);

        // ---- Background ----
        {
            HBRUSH hBg = CreateSolidBrush(C(DK_SURFACE, LT_SURFACE));
            FillRect(hm, &rc, hBg);
            DeleteObject(hBg);
        }

        // ---- Left edge splitter  (1px subtle divider) ----
        {
            RECT rcSplit = { 0, 0, CHAT_SPLITTER_WIDTH, cyH };
            HBRUSH hSpl = CreateSolidBrush(C(DK_DIVIDER, LT_DIVIDER));
            FillRect(hm, &rcSplit, hSpl);
            DeleteObject(hSpl);
        }

        // ---- Header ----
        {
            RECT rcHdr = { CHAT_SPLITTER_WIDTH, 0, cxW, CHAT_HEADER_HEIGHT };
            HBRUSH hHdr = CreateSolidBrush(C(DK_HEADER, LT_HEADER));
            FillRect(hm, &rcHdr, hHdr);
            DeleteObject(hHdr);

            // Header bottom divider
            RECT rcDiv = { CHAT_SPLITTER_WIDTH, CHAT_HEADER_HEIGHT - 1,
                           cxW, CHAT_HEADER_HEIGHT };
            HBRUSH hDiv = CreateSolidBrush(C(DK_DIVIDER, LT_DIVIDER));
            FillRect(hm, &rcDiv, hDiv);
            DeleteObject(hDiv);

            // Status dot (accent blue)
            int dotX = CHAT_SPLITTER_WIDTH + 16;
            int dotY = CHAT_HEADER_HEIGHT / 2;
            DrawStatusDot(hm, dotX, dotY, CHAT_STATUS_DOT_R,
                          C(DK_ACCENT, LT_ACCENT));

            // "Biko AI" title
            if (s_hFontHeader) SelectObject(hm, s_hFontHeader);
            SetTextColor(hm, C(DK_TEXT1, LT_TEXT1));
            RECT rcTitle = {
                dotX + CHAT_STATUS_DOT_R + 8,
                0,
                s_rcCloseBtn.left - 8,
                CHAT_HEADER_HEIGHT
            };
            DrawTextW(hm, L"Biko AI", -1, &rcTitle,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            // Close button (rounded rect on hover + X icon)
            if (s_bCloseHover)
            {
                FillRoundRectSolid(hm, &s_rcCloseBtn, 6,
                                   C(DK_CLOSE_HOV, LT_CLOSE_HOV));
            }
            DrawCloseX(hm, &s_rcCloseBtn, C(DK_TEXT2, LT_TEXT2));
        }

        // ---- Input area background & card ----
        {
            int outputBottom = CHAT_HEADER_HEIGHT;
            if (s_hwndOutput)
            {
                RECT rcOut;
                GetWindowRect(s_hwndOutput, &rcOut);
                MapWindowPoints(NULL, hwnd, (POINT*)&rcOut, 2);
                outputBottom = rcOut.bottom;
            }

            // Surface behind input area
            RECT rcIA = { CHAT_SPLITTER_WIDTH, outputBottom, cxW, cyH };
            HBRUSH hIA = CreateSolidBrush(C(DK_SURFACE, LT_SURFACE));
            FillRect(hm, &rcIA, hIA);
            DeleteObject(hIA);

            // Divider above input area
            RECT rcIDiv = { CHAT_SPLITTER_WIDTH, outputBottom,
                            cxW, outputBottom + 1 };
            HBRUSH hIDv = CreateSolidBrush(C(DK_DIVIDER, LT_DIVIDER));
            FillRect(hm, &rcIDiv, hIDv);
            DeleteObject(hIDv);

            // Input card (rounded rect with accent border on focus)
            COLORREF cardBd = s_bInputFocused
                ? C(DK_BORDER_FOCUS, LT_BORDER_FOCUS)
                : C(DK_INPUT_BD, LT_INPUT_BD);
            FillRoundRect(hm, &s_rcInputCard, CHAT_INPUT_RADIUS,
                          C(DK_INPUT_BG, LT_INPUT_BG), cardBd);

            // Left accent stripe when focused (WelcomeScreen hover pattern)
            if (s_bInputFocused)
            {
                RECT rcStripe = {
                    s_rcInputCard.left + 1,
                    s_rcInputCard.top + CHAT_INPUT_RADIUS,
                    s_rcInputCard.left + 3,
                    s_rcInputCard.bottom - CHAT_INPUT_RADIUS
                };
                HBRUSH hStr = CreateSolidBrush(C(DK_ACCENT, LT_ACCENT));
                FillRect(hm, &rcStripe, hStr);
                DeleteObject(hStr);
            }
        }

        // ---- Blit ----
        BitBlt(hdc, 0, 0, cxW, cyH, hm, 0, 0, SRCCOPY);
        SelectObject(hm, hOldBmp);
        DeleteObject(hBmp);
        DeleteDC(hm);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdcEdit = (HDC)wParam;
        BOOL bDark = DarkMode_IsEnabled();
        COLORREF bgClr = bDark ? DK_INPUT_BG : LT_INPUT_BG;
        SetTextColor(hdcEdit, bDark ? DK_TEXT1 : LT_TEXT1);
        SetBkColor(hdcEdit, bgClr);
        SetBkMode(hdcEdit, OPAQUE);
        // Persistent brush (avoids GDI leak)
        static HBRUSH s_hBrEdit = NULL;
        static COLORREF s_lastClr = 0;
        if (!s_hBrEdit || s_lastClr != bgClr)
        {
            if (s_hBrEdit) DeleteObject(s_hBrEdit);
            s_hBrEdit = CreateSolidBrush(bgClr);
            s_lastClr = bgClr;
        }
        return (LRESULT)s_hBrEdit;
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
            COLORREF bgClr = s_bSendHover
                ? C(DK_ACCENT_HOV, LT_ACCENT_HOV)
                : C(DK_ACCENT, LT_ACCENT);

            // Accent circle
            FillRoundRectSolid(pDIS->hDC, &pDIS->rcItem,
                               CHAT_SEND_SIZE, bgClr);

            // White arrow
            int bcx = (pDIS->rcItem.left + pDIS->rcItem.right) / 2;
            int bcy = (pDIS->rcItem.top + pDIS->rcItem.bottom) / 2;
            DrawSendArrow(pDIS->hDC, bcx, bcy, RGB(255, 255, 255));

            return TRUE;
        }
        break;
    }

    // ---- Header close button mouse tracking ----
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

        // Splitter drag
        if (GetCapture() == hwnd)
        {
            POINT ptScr;
            GetCursorPos(&ptScr);
            HWND hwndParent = GetParent(hwnd);
            ScreenToClient(hwndParent, &ptScr);

            RECT rcParent;
            GetClientRect(hwndParent, &rcParent);

            int newWidth = rcParent.right - ptScr.x;
            if (newWidth < 280) newWidth = 280;
            if (newWidth > rcParent.right - 200)
                newWidth = rcParent.right - 200;

            s_iPanelWidth = newWidth;
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rcParent.right, rcParent.bottom));
            break;
        }

        // Close button hover
        BOOL wasHover = s_bCloseHover;
        s_bCloseHover = PtInRect(&s_rcCloseBtn, pt);
        if (s_bCloseHover != wasHover)
        {
            InvalidateRect(hwnd, &s_rcCloseBtn, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (s_bCloseHover)
        {
            s_bCloseHover = FALSE;
            InvalidateRect(hwnd, &s_rcCloseBtn, FALSE);
        }
        break;

    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);

            if (pt.x < CHAT_SPLITTER_WIDTH + 3)
            {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                return TRUE;
            }
            if (PtInRect(&s_rcCloseBtn, pt))
            {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };

        // Splitter grab
        if (pt.x < CHAT_SPLITTER_WIDTH + 3)
        {
            SetCapture(hwnd);
            return 0;
        }

        // Close button
        if (PtInRect(&s_rcCloseBtn, pt))
        {
            ChatPanel_Hide();
            return 0;
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
        // AI messages
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

//=============================================================================
// Internal: Input subclass  (Enter sends, Shift+Enter newline, focus tracking)
//=============================================================================

static LRESULT CALLBACK ChatInputSubclassProc(HWND hwnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
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

    // Focus tracking for input card accent border
    case WM_SETFOCUS:
        s_bInputFocused = TRUE;
        InvalidateRect(GetParent(hwnd), NULL, FALSE);
        break;

    case WM_KILLFOCUS:
        s_bInputFocused = FALSE;
        InvalidateRect(GetParent(hwnd), NULL, FALSE);
        break;
    }

    return CallWindowProc(s_pfnOrigInputProc, hwnd, msg, wParam, lParam);
}

//=============================================================================
// Internal: Send button subclass  (hover tracking)
//=============================================================================

static LRESULT CALLBACK SendBtnProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!s_bSendHover)
        {
            s_bSendHover = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;

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
