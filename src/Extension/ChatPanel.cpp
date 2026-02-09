/******************************************************************************
*
* Biko
*
* ChatPanel.c
*   Dockable chat panel with custom owner-drawn message bubbles.
*   User messages right-aligned, AI messages left-aligned.
*   Double-buffered GDI rendering with smooth scrolling.
*
******************************************************************************/

#include "ChatPanel.h"
#include "MarkdownPreview.h"
#include "AIBridge.h"
#include "AIAgent.h"
#include "AIDirectCall.h"
#include "AIContext.h"
#include "CommonUtils.h"
#include "Utils.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <gdiplus.h>
#include "SciCall.h"
#include "Scintilla.h"
#include "DarkMode.h"
#include <string.h>
#include <stdio.h>
#include "resource.h"

#pragma comment(lib, "gdiplus.lib")

//=============================================================================
// Color palette
//=============================================================================

// Background
#define CP_BG              RGB(24, 24, 24)
#define CP_HEADER          RGB(24, 24, 24)

// User bubble (right side) — neutral grey
#define CP_USER_BG         RGB(55, 55, 60)
#define CP_USER_TEXT       RGB(230, 230, 235)

// AI bubble (left side)
#define CP_AI_BG           RGB(44, 44, 48)
#define CP_AI_TEXT         RGB(220, 220, 225)
#define CP_AI_STRIP        RGB(48, 49, 55)

// Attachment chips
#define CP_ATTACHMENT_BG      RGB(38, 39, 45)
#define CP_ATTACHMENT_BORDER  RGB(70, 72, 82)
#define CP_ATTACHMENT_TEXT    RGB(205, 205, 210)

// System messages
#define CP_SYS_TEXT        RGB(100, 100, 110)

// Input area
#define CP_INPUT_BG        RGB(36, 36, 40)
#define CP_INPUT_BD        RGB(60, 60, 65)
#define CP_INPUT_FOCUS_BD  RGB(80, 80, 86)
#define CP_INPUT_TEXT      RGB(220, 220, 225)

// Accents — no blue, neutral tones
#define CP_ACCENT          RGB(200, 200, 205)
#define CP_TEXT_PRIMARY    RGB(230, 230, 235)
#define CP_TEXT_SECONDARY  RGB(140, 140, 150)
#define CP_CLOSE_HOV       RGB(50, 50, 56)
#define CP_SEND_HOV        RGB(65, 65, 70)
#define CP_SEND_BG         RGB(44, 44, 48)
#define CP_SCROLLBAR_BG    RGB(30, 30, 34)
#define CP_SCROLLBAR_TH    RGB(60, 60, 68)
#define CP_SCROLLBAR_HOV   RGB(80, 80, 90)

//=============================================================================
// Layout constants
//=============================================================================

#define CHAT_PANEL_WIDTH        380
#define CHAT_HEADER_HEIGHT      44
#define CHAT_INPUT_AREA_HEIGHT  92
#define CHAT_INPUT_PAD          10
#define CHAT_INPUT_INNER_PAD    10
#define CHAT_INPUT_RADIUS       8
#define CHAT_SEND_SIZE          30
#define CHAT_HDR_BTN_SIZE       28
#define CHAT_STATUS_DOT_R       4

#define CHAT_BUBBLE_PAD_H     14
#define CHAT_BUBBLE_PAD_V      8
#define CHAT_BUBBLE_RADIUS    12
#define CHAT_BUBBLE_MAX_PCT   75   // max width = 75% of chat area
#define CHAT_BUBBLE_GAP        6
#define CHAT_MSG_SPACING      16
#define CHAT_LABEL_GAP         4
#define CHAT_SCROLL_W         6

#define CHAT_ATTACHMENT_THUMB_W   96
#define CHAT_ATTACHMENT_THUMB_H   64
#define CHAT_ATTACHMENT_LABEL_H   18
#define CHAT_ATTACHMENT_GAP        6
#define CHAT_PENDING_PAD_TOP       6
#define CHAT_PENDING_PAD_BOTTOM    6

//=============================================================================
// Message model
//=============================================================================

typedef enum { MSG_USER = 0, MSG_AI, MSG_SYSTEM } MsgRole;

typedef struct {
    AIChatAttachment meta;
    WCHAR    wzDisplayName[AI_ATTACHMENT_NAME_MAX];
    WCHAR    wzPath[MAX_PATH];
    HBITMAP  hPreview;
    int      previewW;
    int      previewH;
    RECT     tileRect;      // for pending hit testing
    BOOL     hasTileRect;
} ChatAttachment;

typedef struct {
    MsgRole role;
    char*   text;          // UTF-8
    WCHAR*  wtext;         // wide copy for drawing
    int     wtextLen;
    ChatAttachment* attachments;
    int     attachmentCount;
    int     cachedH;       // cached rendered height (invalidate = -1)
    int     cachedW;       // width used when cachedH was computed
} ChatMsg;

#define MAX_MSGS 256

//=============================================================================
// Internal state
//=============================================================================

static HWND     s_hwndPanel   = NULL;
static HWND     s_hwndChat    = NULL;   // custom chat view child
static HWND     s_hwndInput   = NULL;
static HWND     s_hwndSend    = NULL;
static HWND     s_hwndAttach  = NULL;
static HWND     s_hwndSearch  = NULL;
static BOOL     s_bVisible    = FALSE;
static int      s_iPanelWidth = CHAT_PANEL_WIDTH;
static WNDPROC  s_pfnOrigInputProc = NULL;
static WNDPROC  s_pfnOrigSendProc  = NULL;
static WNDPROC  s_pfnOrigAttachProc = NULL;
static WNDPROC  s_pfnOrigSearchProc = NULL;
static BOOL     s_bSendHover  = FALSE;
static BOOL     s_bAttachHover = FALSE;
static BOOL     s_bSearchHover = FALSE;
static BOOL     s_bInputFocused = FALSE;

// Header close button
static RECT     s_rcCloseBtn   = { 0 };
static BOOL     s_bCloseHover  = FALSE;

// Fonts
static HFONT    s_hFontHeader  = NULL;
static HFONT    s_hFontInput   = NULL;
static HFONT    s_hFontStatus  = NULL;
static HFONT    s_hFontLabel   = NULL;
static HFONT    s_hFontBubble  = NULL;

// Logo icon
static HICON    s_hIconLogo    = NULL;

// Message store
static ChatMsg  s_msgs[MAX_MSGS];
static int      s_nMsgs = 0;

// Scroll state
static int      s_scrollY     = 0;     // pixels scrolled from top
static int      s_contentH    = 0;     // total content height
static int      s_viewH       = 0;     // visible chat area height
static int      s_chatW       = 0;     // chat area width

// Input card rect
static RECT     s_rcInputCard = { 0 };
static RECT     s_rcPendingStrip = { 0 };
static int      s_lastLayoutParentRight = 0;
static int      s_lastLayoutEditorTop = 0;
static int      s_lastLayoutEditorHeight = 0;

// Pending attachments (before send)
static ChatAttachment s_pendingAttachments[AI_MAX_CHAT_ATTACHMENTS];
static int            s_pendingAttachmentCount = 0;

// Attachment temp path + GDI+
static WCHAR          s_wszAttachmentDir[MAX_PATH] = L"";
static BOOL           s_bAttachmentDirReady = FALSE;
static ULONG_PTR      s_gdiplusToken = 0;
static BOOL           s_gdiplusInitialized = FALSE;

// Chat history (kept for API compat)
#define MAX_CHAT_HISTORY 100
static char*    s_chatHistory[MAX_CHAT_HISTORY];
static int      s_iHistoryCount = 0;

//=============================================================================
// Forward declarations
//=============================================================================

static LRESULT CALLBACK ChatPanelWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ChatViewWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ChatInputSubclassProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SendBtnProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK AttachBtnProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SearchBtnProc(HWND, UINT, WPARAM, LPARAM);
static void AddMessage(MsgRole role, const char* text,
                       ChatAttachment* attachments, int attachmentCount);
static void InvalidateAllHeights(void);
static int  MeasureMsgHeight(HDC hdc, int idx, int chatW);
static int  ComputeContentHeight(HDC hdc, int chatW);
static void EnsureScrollEnd(void);
static void ClampScroll(void);
static void PaintChatView(HWND hwnd, HDC hdc, int cx, int cy);
static void DrawPendingAttachments(HDC hdc);
static int  GetPendingSectionHeight(int availableWidth);
static void ChatPanel_InvokeAttachmentPicker(void);
static void PendingAttachments_Reset(void);
static BOOL EnsureAttachmentTempDir(void);
static BOOL EnsureGdiplus(void);
static BOOL PendingAttachments_AddFromPath(const WCHAR* wszSourcePath);
static BOOL PendingAttachments_AddClipboardImage(void);
static ChatAttachment* PendingAttachments_Detach(int* pcAttachments);
static BOOL PendingAttachments_AddImageFromPath(const WCHAR* wszSourcePath,
                                               const WCHAR* wszDisplayName,
                                               const char* contentTypeOverride);
static void PendingAttachments_NotifyChanged(void);
static BOOL AddAttachmentsFromDrop(HDROP hDrop);
static BOOL HandleClipboardPaste(void);
static BOOL SaveBitmapToTempPng(HBITMAP hBitmap, WCHAR* wszOutPath, size_t cchOut);
static BOOL BuildAttachmentSummary(const ChatAttachment* attachments, int count,
                                   AIChatAttachment* pOutMeta, int* pcOutMeta);
static void FreeChatMessage(ChatMsg* msg);
static int  ComputeAttachmentRows(int count, int availableWidth);
static int  ComputeAttachmentAreaHeight(int count, int availableWidth);
static void DrawMessageAttachments(HDC hdc, const ChatMsg* msg,
                                   int startX, int startY, int availableWidth);

//=============================================================================
// Class registration
//=============================================================================

static const WCHAR* CHAT_PANEL_CLASS = L"BikoChatPanel";
static const WCHAR* CHAT_VIEW_CLASS  = L"BikoChatView";
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

    // Chat view (scrollable message area)
    WNDCLASSEXW wv;
    ZeroMemory(&wv, sizeof(wv));
    wv.cbSize        = sizeof(wv);
    wv.style         = CS_HREDRAW | CS_VREDRAW;
    wv.lpfnWndProc   = ChatViewWndProc;
    wv.hInstance      = hInst;
    wv.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wv.hbrBackground = NULL;
    wv.lpszClassName  = CHAT_VIEW_CLASS;
    RegisterClassExW(&wv);

    s_bClassRegistered = TRUE;
}

static BOOL EnsureAttachmentTempDir(void)
{
    if (s_bAttachmentDirReady && s_wszAttachmentDir[0])
        return TRUE;

    DWORD dw = GetTempPathW(MAX_PATH, s_wszAttachmentDir);
    if (dw == 0 || dw >= MAX_PATH)
        return FALSE;

    if (wcscat_s(s_wszAttachmentDir, MAX_PATH, L"BikoAttachments") != 0)
        return FALSE;

    if (!CreateDirectoryW(s_wszAttachmentDir, NULL))
    {
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;
    }

    s_bAttachmentDirReady = TRUE;
    return TRUE;
}

static BOOL EnsureGdiplus(void)
{
    if (s_gdiplusInitialized)
        return TRUE;

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    if (Gdiplus::GdiplusStartup(&s_gdiplusToken, &gdiplusStartupInput, NULL) != Gdiplus::Ok)
        return FALSE;

    s_gdiplusInitialized = TRUE;
    return TRUE;
}

static void PendingAttachments_NotifyChanged(void)
{
    InvalidateRect(s_hwndPanel, &s_rcInputCard, FALSE);
    ChatPanel_Layout(GetParent(s_hwndPanel), 0, 0, 0);
    InvalidateRect(s_hwndPanel, NULL, FALSE);
}

static BOOL PendingAttachments_AddFile(const WCHAR* wszSourcePath, const WCHAR* wszDisplayName)
{
    if (!wszSourcePath || !wszSourcePath[0] || s_pendingAttachmentCount >= AI_MAX_CHAT_ATTACHMENTS)
        return FALSE;

    if (!EnsureAttachmentTempDir())
        return FALSE;

    WCHAR destPath[MAX_PATH];
    if (FAILED(StringCchCopyW(destPath, MAX_PATH, s_wszAttachmentDir)))
        return FALSE;
    if (FAILED(StringCchCatW(destPath, MAX_PATH, L"\\")))
        return FALSE;

    WCHAR fileName[MAX_PATH];
    if (!GetTempFileNameW(s_wszAttachmentDir, L"bkf", 0, fileName))
        return FALSE;

    // Replace temp file with copy of source
    DeleteFileW(fileName);
    if (!CopyFileW(wszSourcePath, fileName, FALSE))
        return FALSE;

    ChatAttachment* pSlot = &s_pendingAttachments[s_pendingAttachmentCount];
    ZeroMemory(pSlot, sizeof(*pSlot));
    StringCchCopyW(pSlot->wzPath, MAX_PATH, fileName);

    if (wszDisplayName && wszDisplayName[0])
        StringCchCopyW(pSlot->wzDisplayName, AI_ATTACHMENT_NAME_MAX, wszDisplayName);
    else
        StringCchCopyW(pSlot->wzDisplayName, AI_ATTACHMENT_NAME_MAX, PathFindFileNameW(wszSourcePath));

    WideCharToMultiByte(CP_UTF8, 0, pSlot->wzPath, -1, pSlot->meta.path, sizeof(pSlot->meta.path), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, pSlot->wzDisplayName, -1, pSlot->meta.displayName, sizeof(pSlot->meta.displayName), NULL, NULL);
    StringCchCopyA(pSlot->meta.contentType, AI_ATTACHMENT_TYPE_MAX, "application/octet-stream");
    pSlot->meta.isImage = FALSE;

    s_pendingAttachmentCount++;
    PendingAttachments_NotifyChanged();
    return TRUE;
}

static BOOL PendingAttachments_AddImage(const WCHAR* wszSourcePath, const WCHAR* wszDisplayName)
{
    if (!PendingAttachments_AddFile(wszSourcePath, wszDisplayName))
        return FALSE;

    ChatAttachment* pSlot = &s_pendingAttachments[s_pendingAttachmentCount - 1];
    pSlot->meta.isImage = TRUE;
    StringCchCopyA(pSlot->meta.contentType, AI_ATTACHMENT_TYPE_MAX, "image/png");

    if (!EnsureGdiplus())
        return TRUE; // still accept attachment without preview

    Gdiplus::Bitmap bitmap(pSlot->wzPath);
    if (bitmap.GetLastStatus() != Gdiplus::Ok)
        return TRUE;

    const int targetW = CHAT_ATTACHMENT_THUMB_W - 8;
    const int targetH = CHAT_ATTACHMENT_THUMB_H - 8;

    Gdiplus::Bitmap thumb(targetW, targetH, PixelFormat32bppARGB);
    if (thumb.GetLastStatus() != Gdiplus::Ok)
        return TRUE;

    Gdiplus::Graphics g(&thumb);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.FillRectangle(&Gdiplus::SolidBrush(Gdiplus::Color(0, 0, 0, 0)), 0, 0, targetW, targetH);
    g.DrawImage(&bitmap, 0, 0, targetW, targetH);

    HBITMAP hBmp = NULL;
    if (thumb.GetHBITMAP(Gdiplus::Color(0,0,0,0), &hBmp) == Gdiplus::Ok)
    {
        pSlot->hPreview = hBmp;
        pSlot->previewW = targetW;
        pSlot->previewH = targetH;
    }

    PendingAttachments_NotifyChanged();
    return TRUE;
}
//=============================================================================
// Font hierarchy
//=============================================================================

static void CreateFonts(void)
{
    if (s_hFontHeader) return;

    s_hFontHeader = CreateFontW(
        -14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");
    if (!s_hFontHeader)
        s_hFontHeader = CreateFontW(
            -14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    s_hFontInput = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    s_hFontStatus = CreateFontW(
        -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    s_hFontLabel = CreateFontW(
        -11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Semibold");

    s_hFontBubble = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void DestroyFonts(void)
{
    if (s_hFontHeader) { DeleteObject(s_hFontHeader); s_hFontHeader = NULL; }
    if (s_hFontInput)  { DeleteObject(s_hFontInput);  s_hFontInput  = NULL; }
    if (s_hFontStatus) { DeleteObject(s_hFontStatus);  s_hFontStatus = NULL; }
    if (s_hFontLabel)  { DeleteObject(s_hFontLabel);  s_hFontLabel  = NULL; }
    if (s_hFontBubble) { DeleteObject(s_hFontBubble); s_hFontBubble = NULL; }
}

//=============================================================================
// Drawing helpers
//=============================================================================

static void FillRoundRect(HDC hdc, const RECT* prc, int r,
                          COLORREF fill, COLORREF border)
{
    HBRUSH hBr  = CreateSolidBrush(fill);
    HPEN   hPen = CreatePen(PS_SOLID, 1, border);
    HBRUSH hOldBr  = (HBRUSH)SelectObject(hdc, hBr);
    HPEN   hOldPen = (HPEN)SelectObject(hdc, hPen);
    RoundRect(hdc, prc->left, prc->top, prc->right, prc->bottom, r, r);
    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBr);
    DeleteObject(hPen);
}

static void FillRoundRectSolid(HDC hdc, const RECT* prc, int r, COLORREF fill)
{
    FillRoundRect(hdc, prc, r, fill, fill);
}

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

static void DrawSendArrow(HDC hdc, int cx, int cy, COLORREF clr)
{
    HPEN hPen = CreatePen(PS_SOLID, 2, clr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, cx, cy + 5, NULL); LineTo(hdc, cx, cy - 5);
    MoveToEx(hdc, cx, cy - 5, NULL); LineTo(hdc, cx - 4, cy - 1);
    MoveToEx(hdc, cx, cy - 5, NULL); LineTo(hdc, cx + 4, cy - 1);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

static void DrawSearchIcon(HDC hdc, int cx, int cy, COLORREF clr)
{
    HPEN hPen = CreatePen(PS_SOLID, 2, clr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    // Magnifying glass circle
    Ellipse(hdc, cx - 5, cy - 6, cx + 4, cy + 3);
    // Handle
    MoveToEx(hdc, cx + 3, cy + 3, NULL); LineTo(hdc, cx + 6, cy + 6);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

static void DrawCloseX(HDC hdc, const RECT* prc, COLORREF clr)
{
    int cx = (prc->left + prc->right) / 2;
    int cy = (prc->top + prc->bottom) / 2;
    int d = 5;
    HPEN hPen = CreatePen(PS_SOLID, 1, clr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, cx - d, cy - d, NULL); LineTo(hdc, cx + d + 1, cy + d + 1);
    MoveToEx(hdc, cx + d, cy - d, NULL); LineTo(hdc, cx - d - 1, cy + d + 1);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

//=============================================================================
// Message store
//=============================================================================

static void AddMessage(MsgRole role, const char* text,
                       const ChatAttachment* attachments, int attachmentCount)
{
    if (s_nMsgs >= MAX_MSGS)
    {
        // Shift out oldest
        FreeChatMessage(&s_msgs[0]);
        memmove(&s_msgs[0], &s_msgs[1], sizeof(ChatMsg) * (MAX_MSGS - 1));
        s_nMsgs = MAX_MSGS - 1;
    }

    ChatMsg* m = &s_msgs[s_nMsgs];
    ZeroMemory(m, sizeof(ChatMsg));
    m->role = role;
    m->cachedH = -1;  // not yet measured

    int len = (int)strlen(text);
    m->text = (char*)n2e_Alloc(len + 1);
    if (m->text) {
        memcpy(m->text, text, len);
        m->text[len] = 0;
    }

    // Convert to wide for DrawText
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    m->wtext = (WCHAR*)n2e_Alloc(wlen * sizeof(WCHAR));
    if (m->wtext) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, m->wtext, wlen);
        m->wtextLen = wlen - 1;
    }

    // Copy attachments
    if (attachments && attachmentCount > 0)
    {
        m->attachmentCount = attachmentCount;
        m->attachments = (ChatAttachment*)n2e_Alloc(attachmentCount * sizeof(ChatAttachment));
        if (m->attachments)
        {
            for (int i = 0; i < attachmentCount; i++)
            {
                m->attachments[i] = attachments[i];
                // Clone bitmap if present so we own it
                if (attachments[i].hPreview)
                {
                    // CopyBitmap helper or just share?
                    // Proper GDI ownership requires copy.
                    // For simplicity, we'll rely on the fact that pending attachments
                    // are reset (destroyed) after send, so we must copy or move ownership.
                    // Let's MOVE ownership by assuming the caller (ChatPanel_SendInput)
                    // will clear the source handles without deleting objects.
                    // Wait, PendingAttachments_Reset deletes objects.
                    // So we must Copy.
                    // Actually, let's implement CopyBitmap.
                    BITMAP bmp;
                    GetObject(attachments[i].hPreview, sizeof(bmp), &bmp);
                    m->attachments[i].hPreview = (HBITMAP)CopyImage(attachments[i].hPreview, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
                }
            }
        }
    }

    s_nMsgs++;
}

static void FreeChatMessage(ChatMsg* m)
{
    if (m->text) n2e_Free(m->text);
    if (m->wtext) n2e_Free(m->wtext);
    if (m->attachments)
    {
        for (int i = 0; i < m->attachmentCount; i++)
        {
            if (m->attachments[i].hPreview)
                DeleteObject(m->attachments[i].hPreview);
        }
        n2e_Free(m->attachments);
    }
    ZeroMemory(m, sizeof(ChatMsg));
}

static void FreeMessages(void)
{
    for (int i = 0; i < s_nMsgs; i++) {
        FreeChatMessage(&s_msgs[i]);
    }
    s_nMsgs = 0;
    s_scrollY = 0;
    s_contentH = 0;
}

static void InvalidateAllHeights(void) {
    for (int i = 0; i < s_nMsgs; i++)
        s_msgs[i].cachedH = -1;
}

static void PendingAttachments_Reset(void)
{
    for (int i = 0; i < s_pendingAttachmentCount; i++)
    {
        if (s_pendingAttachments[i].hPreview)
        {
            DeleteObject(s_pendingAttachments[i].hPreview);
            s_pendingAttachments[i].hPreview = NULL;
        }
        s_pendingAttachments[i].hasTileRect = FALSE;
    }
    s_pendingAttachmentCount = 0;
    SetRectEmpty(&s_rcPendingStrip);
}

//=============================================================================
// Measurement
//=============================================================================

static int MeasureMsgHeight(HDC hdc, int idx, int chatW)
{
    ChatMsg* m = &s_msgs[idx];
    if (m->cachedH >= 0 && m->cachedW == chatW) return m->cachedH;

    int maxBubbleW = (chatW * CHAT_BUBBLE_MAX_PCT) / 100;
    if (maxBubbleW < 100) maxBubbleW = 100;
    int textAreaW = maxBubbleW - 2 * CHAT_BUBBLE_PAD_H;
    if (textAreaW < 40) textAreaW = 40;

    if (m->role == MSG_SYSTEM)
    {
        // System: small centered italic text
        HFONT hOld = (HFONT)SelectObject(hdc, s_hFontStatus);
        RECT rc = { 0, 0, chatW - 40, 0 };
        DrawTextW(hdc, m->wtext, m->wtextLen, &rc,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, hOld);
        m->cachedH = rc.bottom + 4;
        m->cachedW = chatW;
        return m->cachedH;
    }

    // Label height
    int labelH = 14;

    // Bubble text height
    HFONT hOld = (HFONT)SelectObject(hdc, s_hFontBubble);
    RECT rc = { 0, 0, textAreaW, 0 };
    DrawTextW(hdc, m->wtext, m->wtextLen, &rc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, hOld);

    int bubbleH = rc.bottom + 2 * CHAT_BUBBLE_PAD_V;
    
    // Attachments height
    int attachH = 0;
    if (m->attachmentCount > 0)
    {
        int availW = maxBubbleW - 2 * CHAT_BUBBLE_PAD_H; // approximate inner width
        attachH = ComputeAttachmentAreaHeight(m->attachmentCount, availW);
    }
    
    m->cachedH = labelH + CHAT_LABEL_GAP + bubbleH + attachH;
    m->cachedW = chatW;
    return m->cachedH;
}

static int ComputeContentHeight(HDC hdc, int chatW)
{
    int totalH = 8;  // top padding
    for (int i = 0; i < s_nMsgs; i++) {
        totalH += MeasureMsgHeight(hdc, i, chatW);
        if (i < s_nMsgs - 1)
            totalH += CHAT_MSG_SPACING;
    }
    totalH += 8;  // bottom padding
    return totalH;
}

static void ClampScroll(void)
{
    int maxScroll = s_contentH - s_viewH;
    if (maxScroll < 0) maxScroll = 0;
    if (s_scrollY > maxScroll) s_scrollY = maxScroll;
    if (s_scrollY < 0) s_scrollY = 0;
}

static void EnsureScrollEnd(void)
{
    int maxScroll = s_contentH - s_viewH;
    if (maxScroll < 0) maxScroll = 0;
    s_scrollY = maxScroll;
}

//=============================================================================
// Paint chat view
//=============================================================================

static void PaintChatView(HWND hwnd, HDC hdc, int cx, int cy)
{
    s_viewH = cy;
    s_chatW = cx;

    // Compute content height
    s_contentH = ComputeContentHeight(hdc, cx);
    ClampScroll();

    // Background
    RECT rcBg = { 0, 0, cx, cy };
    HBRUSH hBg = CreateSolidBrush(CP_BG);
    FillRect(hdc, &rcBg, hBg);
    DeleteObject(hBg);

    SetBkMode(hdc, TRANSPARENT);

    int y = 8 - s_scrollY;  // start with top padding

    for (int i = 0; i < s_nMsgs; i++)
    {
        ChatMsg* m = &s_msgs[i];
        int msgH = MeasureMsgHeight(hdc, i, cx);

        // Skip if entirely above viewport
        if (y + msgH < 0) {
            y += msgH + CHAT_MSG_SPACING;
            continue;
        }
        // Stop if entirely below viewport
        if (y > cy) break;

        if (m->role == MSG_SYSTEM)
        {
            // System message: centered, small, muted
            HFONT hOld = (HFONT)SelectObject(hdc, s_hFontStatus);
            SetTextColor(hdc, CP_SYS_TEXT);
            RECT rcSys = { 20, y, cx - 20, y + msgH };
            DrawTextW(hdc, m->wtext, m->wtextLen, &rcSys,
                      DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(hdc, hOld);
        }
        else
        {
            BOOL isUser = (m->role == MSG_USER);
            int maxBubbleW = (cx * CHAT_BUBBLE_MAX_PCT) / 100;
            if (maxBubbleW < 100) maxBubbleW = 100;
            int textAreaW = maxBubbleW - 2 * CHAT_BUBBLE_PAD_H;
            if (textAreaW < 40) textAreaW = 40;

            // Measure actual text extent for tighter bubbles
            HFONT hOld = (HFONT)SelectObject(hdc, s_hFontBubble);
            RECT rcCalc = { 0, 0, textAreaW, 0 };
            DrawTextW(hdc, m->wtext, m->wtextLen, &rcCalc,
                      DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(hdc, hOld);

            int actualTextW = rcCalc.right;
            int actualTextH = rcCalc.bottom;
            int bubbleW = actualTextW + 2 * CHAT_BUBBLE_PAD_H;
            int bubbleH = actualTextH + 2 * CHAT_BUBBLE_PAD_V;
            if (bubbleW > maxBubbleW) bubbleW = maxBubbleW;

            // Label
            int labelH = 14;
            const WCHAR* label = isUser ? L"You" : L"Biko";
            HFONT hOldF = (HFONT)SelectObject(hdc, s_hFontLabel);
            SetTextColor(hdc, isUser ? CP_ACCENT : CP_TEXT_SECONDARY);

            if (isUser) {
                // Right-aligned label
                RECT rcLabel = { cx - bubbleW - 10, y, cx - 10, y + labelH };
                DrawTextW(hdc, label, -1, &rcLabel,
                          DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
            } else {
                RECT rcLabel = { 10, y, 10 + bubbleW, y + labelH };
                DrawTextW(hdc, label, -1, &rcLabel,
                          DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(hdc, hOldF);

            int bubbleY = y + labelH + CHAT_LABEL_GAP;
            int bubbleX;

            if (isUser) {
                // Right-aligned
                bubbleX = cx - bubbleW - 10;
            } else {
                // Left-aligned
                bubbleX = 10;
            }

            // Draw bubble
            RECT rcBubble = { bubbleX, bubbleY,
                              bubbleX + bubbleW, bubbleY + bubbleH };
            COLORREF bubbleBg = isUser ? CP_USER_BG : CP_AI_BG;
            FillRoundRectSolid(hdc, &rcBubble, CHAT_BUBBLE_RADIUS, bubbleBg);

            // Draw text inside bubble
            RECT rcText = {
                bubbleX + CHAT_BUBBLE_PAD_H,
                bubbleY + CHAT_BUBBLE_PAD_V,
                bubbleX + bubbleW - CHAT_BUBBLE_PAD_H,
                bubbleY + bubbleH - CHAT_BUBBLE_PAD_V
            };
            hOld = (HFONT)SelectObject(hdc, s_hFontBubble);
            SetTextColor(hdc, isUser ? CP_USER_TEXT : CP_AI_TEXT);
            DrawTextW(hdc, m->wtext, m->wtextLen, &rcText,
                      DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(hdc, hOld);

            // Draw attachments if any
            if (m->attachmentCount > 0)
            {
                int attachY = rcText.bottom + CHAT_BUBBLE_PAD_V; // Start after text
                // However, bubbleH includes attachment area.
                // We need to just draw below the text area.
                // Actually rcText.bottom is the bottom of the TEXT rect, which might be smaller than actual text.
                // We computed rcCalc earlier, that's the text height.
                // bubbleY + CHAT_BUBBLE_PAD_V + actualTextH
                
                DrawMessageAttachments(hdc, m, bubbleX + CHAT_BUBBLE_PAD_H, 
                                       bubbleY + CHAT_BUBBLE_PAD_V + actualTextH, 
                                       textAreaW);
            }
        }

        y += msgH + CHAT_MSG_SPACING;
    }

    // Draw mini scrollbar
    if (s_contentH > cy)
    {
        int sbX = cx - CHAT_SCROLL_W;
        int sbH = cy;
        int thumbH = (cy * cy) / s_contentH;
        if (thumbH < 20) thumbH = 20;
        int maxScroll = s_contentH - cy;
        int thumbY = (maxScroll > 0) ? (s_scrollY * (sbH - thumbH)) / maxScroll : 0;

        RECT rcThumb = { sbX, thumbY, cx, thumbY + thumbH };
        FillRoundRectSolid(hdc, &rcThumb, 3, CP_SCROLLBAR_TH);
    }
}

// Helper to draw attachments inside a bubble
static int ComputeAttachmentRows(int count, int availableWidth)
{
    int tileW = CHAT_ATTACHMENT_THUMB_W;
    int perRow = max(1, (availableWidth + CHAT_ATTACHMENT_GAP) / (tileW + CHAT_ATTACHMENT_GAP));
    return (count + perRow - 1) / perRow;
}

static int ComputeAttachmentAreaHeight(int count, int availableWidth)
{
    int rows = ComputeAttachmentRows(count, availableWidth);
    int rowH = CHAT_ATTACHMENT_THUMB_H + CHAT_ATTACHMENT_LABEL_H + 2;
    return rows * (rowH + CHAT_ATTACHMENT_GAP) + CHAT_PENDING_PAD_TOP;
}

static void DrawMessageAttachments(HDC hdc, const ChatMsg* msg,
                                   int startX, int startY, int availableWidth)
{
    int count = msg->attachmentCount;
    if (count <= 0) return;

    int tileW = CHAT_ATTACHMENT_THUMB_W;
    int rowH = CHAT_ATTACHMENT_THUMB_H + CHAT_ATTACHMENT_LABEL_H + 2;
    int perRow = max(1, (availableWidth + CHAT_ATTACHMENT_GAP) / (tileW + CHAT_ATTACHMENT_GAP));

    for (int i = 0; i < count; i++)
    {
        int row = i / perRow;
        int col = i % perRow;
        int x = startX + col * (tileW + CHAT_ATTACHMENT_GAP);
        int y = startY + row * (rowH + CHAT_ATTACHMENT_GAP) + CHAT_PENDING_PAD_TOP;
        
        // Use generic tile drawing from pending logic (need to make sure DrawAttachmentTile is generic enough)
        // DrawAttachmentTile uses ChatAttachment* and RECT*.
        RECT rcTile = { x, y, x + tileW, y + rowH };
        // We need non-const pointer for DrawAttachmentTile but it only reads... 
        // actually DrawAttachmentTile takes ChatAttachment* (non-const).
        // safely cast since we know it's drawing.
        DrawAttachmentTile(hdc, (ChatAttachment*)&msg->attachments[i], &rcTile);
    }
}

//=============================================================================
// Chat view window proc
//=============================================================================

static LRESULT CALLBACK ChatViewWndProc(HWND hwnd, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cx = rc.right, cy = rc.bottom;

        HDC hm = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc, cx, cy);
        HBITMAP hOld = (HBITMAP)SelectObject(hm, hBmp);

        PaintChatView(hwnd, hm, cx, cy);

        BitBlt(hdc, 0, 0, cx, cy, hm, 0, 0, SRCCOPY);
        SelectObject(hm, hOld);
        DeleteObject(hBmp);
        DeleteDC(hm);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        s_scrollY -= delta / 2;  // smooth-ish scrolling
        ClampScroll();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_SIZE:
        InvalidateAllHeights();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
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
        WS_EX_ACCEPTFILES, CHAT_PANEL_CLASS, L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwndParent,
        (HMENU)(UINT_PTR)IDC_CHAT_PANEL, hInst, NULL);

    if (!s_hwndPanel) return FALSE;

    // Custom owner-drawn chat view (replaces Scintilla)
    s_hwndChat = CreateWindowExW(
        0, CHAT_VIEW_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_OUTPUT, hInst, NULL);

    // Multiline input (borderless)
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

    // Owner-drawn send button
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

    // Owner-drawn attach button
    s_hwndAttach = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, CHAT_SEND_SIZE, CHAT_SEND_SIZE, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_ATTACH, hInst, NULL);

    if (s_hwndAttach)
    {
        s_pfnOrigAttachProc = (WNDPROC)SetWindowLongPtrW(
            s_hwndAttach, GWLP_WNDPROC, (LONG_PTR)AttachBtnProc);
    }

    // Owner-drawn search button
    s_hwndSearch = CreateWindowExW(
        0, L"BUTTON", L"",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, CHAT_SEND_SIZE, CHAT_SEND_SIZE, s_hwndPanel,
        (HMENU)(UINT_PTR)IDC_CHAT_SEARCH, hInst, NULL);

    if (s_hwndSearch)
    {
        s_pfnOrigSearchProc = (WNDPROC)SetWindowLongPtrW(
            s_hwndSearch, GWLP_WNDPROC, (LONG_PTR)SearchBtnProc);
    }

    // Load white Biko logo icon for header
    s_hIconLogo = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDR_MAINWND),
                                    IMAGE_ICON, 18, 18, LR_DEFAULTCOLOR);

    AddMessage(MSG_SYSTEM, "Biko AI \xe2\x80\xa2 Ready", NULL, 0);
    return TRUE;
}

void ChatPanel_Destroy(void)
{
    if (s_hwndPanel)
    {
        DestroyWindow(s_hwndPanel);
        s_hwndPanel  = NULL;
        s_hwndChat   = NULL;
        s_hwndInput  = NULL;
        s_hwndSend   = NULL;
        s_hwndAttach = NULL;
        s_hwndSearch = NULL;
    }

    FreeMessages();
    PendingAttachments_Reset();

    for (int i = 0; i < s_iHistoryCount; i++) {
        if (s_chatHistory[i]) n2e_Free(s_chatHistory[i]);
        s_chatHistory[i] = NULL;
    }
    s_iHistoryCount = 0;
    s_bVisible = FALSE;

    if (s_hIconLogo) { DestroyIcon(s_hIconLogo); s_hIconLogo = NULL; }
    DestroyFonts();
}

//=============================================================================
// Public: Visibility
//=============================================================================

void ChatPanel_Toggle(HWND hwndParent)
{
    if (s_bVisible) ChatPanel_Hide();
    else ChatPanel_Show(hwndParent);
}

void ChatPanel_Show(HWND hwndParent)
{
    if (!s_hwndPanel) ChatPanel_Create(hwndParent);
    if (s_hwndPanel) {
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
    if (s_hwndPanel) {
        ShowWindow(s_hwndPanel, SW_HIDE);
        s_bVisible = FALSE;
        HWND hwndParent = GetParent(s_hwndPanel);
        if (hwndParent) {
            RECT rc;
            GetClientRect(hwndParent, &rc);
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
        }
    }
}

BOOL ChatPanel_IsVisible(void) { return s_bVisible; }

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

    int x = 0;
    int innerW = totalW;
    int y = CHAT_HEADER_HEIGHT;

    // Close button
    int cbSz = CHAT_HDR_BTN_SIZE;
    s_rcCloseBtn.right  = totalW - 8;
    s_rcCloseBtn.left   = s_rcCloseBtn.right - cbSz;
    s_rcCloseBtn.top    = (CHAT_HEADER_HEIGHT - cbSz) / 2;
    s_rcCloseBtn.bottom = s_rcCloseBtn.top + cbSz;

    // Chat view
    int outputH = editorHeight - CHAT_HEADER_HEIGHT - CHAT_INPUT_AREA_HEIGHT;
    if (outputH < 80) outputH = 80;
    if (s_hwndChat)
        MoveWindow(s_hwndChat, x, y, innerW, outputH, TRUE);
    y += outputH;

    // Input card
    int cardLeft   = x + CHAT_INPUT_PAD;
    int cardTop    = y + 6;
    int cardRight  = x + innerW - CHAT_INPUT_PAD;
    int pendingHeight = GetPendingSectionHeight();
    int cardBottom = y + CHAT_INPUT_AREA_HEIGHT - 6 + pendingHeight;
    s_rcInputCard.left   = cardLeft;
    s_rcInputCard.top    = cardTop;
    s_rcInputCard.right  = cardRight;
    s_rcInputCard.bottom = cardBottom;

    int ip = CHAT_INPUT_INNER_PAD;
    int editLeft   = cardLeft + ip;
    int editTop    = cardTop + ip + pendingHeight;
    int editRight  = cardRight - ip - CHAT_SEND_SIZE - 6;
    int editBottom = cardBottom - ip;
    if (s_hwndInput)
        MoveWindow(s_hwndInput, editLeft, editTop,
                   editRight - editLeft, editBottom - editTop, TRUE);

    int sendLeft = cardRight - ip - CHAT_SEND_SIZE;
    int sendTop  = cardBottom - ip - CHAT_SEND_SIZE;
    if (s_hwndSend)
        MoveWindow(s_hwndSend, sendLeft, sendTop,
                   CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

    int attachLeft = sendLeft - CHAT_SEND_SIZE - 6;
        MoveWindow(s_hwndAttach, attachLeft, sendTop,
                   CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

    int searchLeft = attachLeft - CHAT_SEND_SIZE - 6;
    if (s_hwndSearch)
        MoveWindow(s_hwndSearch, searchLeft, sendTop,
                   CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

    // Adjust input right edge to avoid overlapping buttons
    if (s_hwndInput)
    {
        int inputsRight = searchLeft - 6;
        MoveWindow(s_hwndInput, editLeft, editTop,
                   inputsRight - editLeft, editBottom - editTop, TRUE);
    }

    if (s_pendingAttachmentCount > 0)
    {
        s_rcPendingStrip.left   = editLeft;
        s_rcPendingStrip.right  = editRight;
        s_rcPendingStrip.top    = cardTop + ip;
        s_rcPendingStrip.bottom = s_rcPendingStrip.top + pendingHeight - CHAT_PENDING_PAD_TOP;
    }
    else
    {
        SetRectEmpty(&s_rcPendingStrip);
    }

    return s_iPanelWidth;
}

//=============================================================================
// Public: Content
//=============================================================================

void ChatPanel_AppendUserMessage(const char* pszMessage,
                                 const AIChatAttachment* pAttachments,
                                 int cAttachments)
{
    AddMessage(MSG_USER, pszMessage, (ChatAttachment*)pAttachments, cAttachments);
    UNREFERENCED_PARAMETER(pAttachments);
    UNREFERENCED_PARAMETER(cAttachments);
    if (s_hwndChat) {
        HDC hdc = GetDC(s_hwndChat);
        RECT rc;
        GetClientRect(s_hwndChat, &rc);
        s_contentH = ComputeContentHeight(hdc, rc.right);
        ReleaseDC(s_hwndChat, hdc);
        EnsureScrollEnd();
        InvalidateRect(s_hwndChat, NULL, FALSE);
    }
}

void ChatPanel_AppendResponse(const char* pszResponse)
{
    // Remove "Thinking..." system message if present
    if (s_nMsgs > 0 && s_msgs[s_nMsgs - 1].role == MSG_SYSTEM)
    {
        ChatMsg* last = &s_msgs[s_nMsgs - 1];
        if (last->text && strstr(last->text, "Thinking"))
        {
            if (last->text) n2e_Free(last->text);
            if (last->wtext) n2e_Free(last->wtext);
            s_nMsgs--;
        }
    }
    AddMessage(MSG_AI, pszResponse, NULL, 0);
    if (s_hwndChat) {
        HDC hdc = GetDC(s_hwndChat);
        RECT rc;
        GetClientRect(s_hwndChat, &rc);
        s_contentH = ComputeContentHeight(hdc, rc.right);
        ReleaseDC(s_hwndChat, hdc);
        EnsureScrollEnd();
        InvalidateRect(s_hwndChat, NULL, FALSE);
    }
}

void ChatPanel_AppendSystem(const char* pszMessage)
{
    AddMessage(MSG_SYSTEM, pszMessage, NULL, 0);
    if (s_hwndChat) {
        HDC hdc = GetDC(s_hwndChat);
        RECT rc;
        GetClientRect(s_hwndChat, &rc);
        s_contentH = ComputeContentHeight(hdc, rc.right);
        ReleaseDC(s_hwndChat, hdc);
        EnsureScrollEnd();
        InvalidateRect(s_hwndChat, NULL, FALSE);
    }
}

void ChatPanel_Clear(void)
{
    FreeMessages();
    if (s_hwndChat)
        InvalidateRect(s_hwndChat, NULL, FALSE);
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
    if (utf8) {
        WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8, utf8Len, NULL, NULL);
    ChatPanel_AppendUserMessage(utf8, s_pendingAttachmentCount > 0 ? (AIChatAttachment*)s_pendingAttachments : NULL, s_pendingAttachmentCount);

        const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
        if (pCfg && pCfg->szApiKey[0]) {
            ChatPanel_AppendSystem("Thinking\xe2\x80\xa6");
            HWND hwndMainWnd = GetParent(s_hwndPanel);
            
            // Convert pending attachments to AIChatAttachment array for AIAgent
            int cAtt = s_pendingAttachmentCount;
            AIChatAttachment* pAttMeta = NULL;
            if (cAtt > 0) {
                pAttMeta = (AIChatAttachment*)malloc(cAtt * sizeof(AIChatAttachment));
                if (pAttMeta) {
                    for (int i=0; i<cAtt; i++) pAttMeta[i] = s_pendingAttachments[i].meta;
                }
            }

            if (!AIAgent_ChatAsync(pCfg, utf8, pAttMeta, cAtt, s_hwndPanel, hwndMainWnd))
                ChatPanel_AppendSystem("AI is busy. Please wait.");
                
            if (pAttMeta) free(pAttMeta);
        } else {
            ChatPanel_AppendSystem("No API key. Use Biko \xe2\x86\x92 AI Settings.");
        }
        n2e_Free(utf8);
    }
    n2e_Free(wszText);
    SetWindowTextW(s_hwndInput, L"");
    PendingAttachments_Reset();
}

void ChatPanel_SendSearchInput(void)
{
    if (!s_hwndInput) return;

    int len = GetWindowTextLengthW(s_hwndInput);
    if (len <= 0) return;

    WCHAR* wszText = (WCHAR*)n2e_Alloc((len + 1) * sizeof(WCHAR));
    if (!wszText) return;
    GetWindowTextW(s_hwndInput, wszText, len + 1);

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)n2e_Alloc(utf8Len);
    if (utf8) {
        WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8, utf8Len, NULL, NULL);

        // Display as user message
        ChatPanel_AppendUserMessage(utf8, NULL, 0);

        // Prefix for AI to trigger tool
        StrBuf sb;
        // Simple buffer allocation since we don't have StrBuf exposed here easily, using explicit alloc
        char* searchPrompt = (char*)n2e_Alloc(utf8Len + 32);
        if (searchPrompt) {
            sprintf(searchPrompt, "Web Search: %s", utf8);
            
            const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
            if (pCfg && pCfg->szApiKey[0]) {
                ChatPanel_AppendSystem("Searching web\xe2\x80\xa6");
                HWND hwndMainWnd = GetParent(s_hwndPanel);
                if (!AIAgent_ChatAsync(pCfg, searchPrompt, NULL, 0, s_hwndPanel, hwndMainWnd))
                    ChatPanel_AppendSystem("AI is busy. Please wait.");
            } else {
                ChatPanel_AppendSystem("No API key. Use Biko \xe2\x86\x92 AI Settings.");
            }
            n2e_Free(searchPrompt);
        }
        n2e_Free(utf8);
    }
    n2e_Free(wszText);
    SetWindowTextW(s_hwndInput, L"");
}

void ChatPanel_FocusInput(void)
{
    if (s_hwndInput) SetFocus(s_hwndInput);
}

//=============================================================================
// Public: Command / Notify
//=============================================================================

BOOL ChatPanel_HandleCommand(WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    if (LOWORD(wParam) == IDC_CHAT_SEND && HIWORD(wParam) == BN_CLICKED) {
        ChatPanel_SendInput();
        return TRUE;
    }
    if (LOWORD(wParam) == IDC_CHAT_ATTACH && HIWORD(wParam) == BN_CLICKED) {
        ChatPanel_InvokeAttachmentPicker();
        return TRUE;
    }
    if (LOWORD(wParam) == IDC_CHAT_SEARCH && HIWORD(wParam) == BN_CLICKED) {
        ChatPanel_SendSearchInput();
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
    InvalidateAllHeights();
    InvalidateRect(s_hwndPanel, NULL, TRUE);
    if (s_hwndChat)  InvalidateRect(s_hwndChat, NULL, TRUE);
    if (s_hwndInput) InvalidateRect(s_hwndInput, NULL, TRUE);
    if (s_hwndSend)  InvalidateRect(s_hwndSend, NULL, TRUE);
    if (s_hwndSend)  InvalidateRect(s_hwndSend, NULL, TRUE);
    if (s_hwndAttach) InvalidateRect(s_hwndAttach, NULL, TRUE);
    if (s_hwndSearch) InvalidateRect(s_hwndSearch, NULL, TRUE);
}

//=============================================================================
// Internal: Panel window procedure
//=============================================================================

static LRESULT CALLBACK ChatPanelWndProc(HWND hwnd, UINT msg,
                                         WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cxW = rc.right, cyH = rc.bottom;

        HDC hm = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc, cxW, cyH);
        HBITMAP hOldBmp = (HBITMAP)SelectObject(hm, hBmp);
        SetBkMode(hm, TRANSPARENT);

        // Background
        {
            HBRUSH hBg = CreateSolidBrush(CP_BG);
            FillRect(hm, &rc, hBg);
            DeleteObject(hBg);
        }

        // Header — Biko logo icon + title
        {
            int iconX = 10;
            int iconY = (CHAT_HEADER_HEIGHT - 18) / 2;
            if (s_hIconLogo)
                DrawIconEx(hm, iconX, iconY, s_hIconLogo,
                           18, 18, 0, NULL, DI_NORMAL);

            if (s_hFontHeader) SelectObject(hm, s_hFontHeader);
            SetTextColor(hm, CP_TEXT_PRIMARY);
            RECT rcTitle = { iconX + 18 + 6, 0,
                             s_rcCloseBtn.left - 8, CHAT_HEADER_HEIGHT };
            DrawTextW(hm, L"Biko AI", -1, &rcTitle,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            if (s_bCloseHover)
                FillRoundRectSolid(hm, &s_rcCloseBtn, 6, CP_CLOSE_HOV);
            DrawCloseX(hm, &s_rcCloseBtn, CP_TEXT_SECONDARY);
        }

        // Input card
        {
            COLORREF cardBd = s_bInputFocused ? CP_INPUT_FOCUS_BD : CP_INPUT_BD;
            FillRoundRect(hm, &s_rcInputCard, CHAT_INPUT_RADIUS,
                          CP_INPUT_BG, cardBd);
        }

        // Pending attachments strip
        DrawPendingAttachments(hm);

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
        SetTextColor(hdcEdit, CP_INPUT_TEXT);
        SetBkColor(hdcEdit, CP_INPUT_BG);
        SetBkMode(hdcEdit, OPAQUE);
        static HBRUSH s_hBrEdit = NULL;
        static COLORREF s_lastClr = 0;
        if (!s_hBrEdit || s_lastClr != CP_INPUT_BG) {
            if (s_hBrEdit) DeleteObject(s_hBrEdit);
            s_hBrEdit = CreateSolidBrush(CP_INPUT_BG);
            s_lastClr = CP_INPUT_BG;
        }
        return (LRESULT)s_hBrEdit;
    }

    case WM_CTLCOLORSTATIC:
    {
        HBRUSH hBr = DarkMode_HandleCtlColor((HDC)wParam);
        if (hBr) return (LRESULT)hBr;
        break;
    }

    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT* pDIS = (DRAWITEMSTRUCT*)lParam;
        if (pDIS)
        {
            if (pDIS->CtlID == IDC_CHAT_SEND)
            {
                COLORREF bgClr = s_bSendHover ? CP_SEND_HOV : CP_SEND_BG;
                COLORREF arrowClr = RGB(255, 255, 255);
                FillRoundRectSolid(pDIS->hDC, &pDIS->rcItem, 6, bgClr);
                int bcx = (pDIS->rcItem.left + pDIS->rcItem.right) / 2;
                int bcy = (pDIS->rcItem.top + pDIS->rcItem.bottom) / 2;
                DrawSendArrow(pDIS->hDC, bcx, bcy, arrowClr);
                return TRUE;
            }
            if (pDIS->CtlID == IDC_CHAT_ATTACH)
            {
                COLORREF bgClr = s_bAttachHover ? CP_SEND_HOV : CP_SEND_BG;
                COLORREF plusClr = RGB(255, 255, 255);
                FillRoundRectSolid(pDIS->hDC, &pDIS->rcItem, 6, bgClr);
                int cx = (pDIS->rcItem.left + pDIS->rcItem.right) / 2;
                int cy = (pDIS->rcItem.top + pDIS->rcItem.bottom) / 2;
                HPEN hPen = CreatePen(PS_SOLID, 2, plusClr);
                HPEN hOldPen = (HPEN)SelectObject(pDIS->hDC, hPen);
                MoveToEx(pDIS->hDC, cx, cy - 6, NULL); LineTo(pDIS->hDC, cx, cy + 6);
                MoveToEx(pDIS->hDC, cx - 6, cy, NULL); LineTo(pDIS->hDC, cx + 6, cy);
                SelectObject(pDIS->hDC, hOldPen);
                DeleteObject(hPen);
                return TRUE;
            }
            if (pDIS->CtlID == IDC_CHAT_SEARCH)
            {
                COLORREF bgClr = s_bSearchHover ? CP_SEND_HOV : CP_SEND_BG;
                COLORREF iconClr = RGB(255, 255, 255);
                FillRoundRectSolid(pDIS->hDC, &pDIS->rcItem, 6, bgClr);
                int cx = (pDIS->rcItem.left + pDIS->rcItem.right) / 2;
                int cy = (pDIS->rcItem.top + pDIS->rcItem.bottom) / 2;
                DrawSearchIcon(pDIS->hDC, cx, cy, iconClr);
                return TRUE;
            }
        }
        break;
    }

    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        if (GetCapture() == hwnd) {
            POINT ptScr;
            GetCursorPos(&ptScr);
            HWND hwndParent = GetParent(hwnd);
            ScreenToClient(hwndParent, &ptScr);
            RECT rcParent;
            GetClientRect(hwndParent, &rcParent);
            int newWidth = rcParent.right - ptScr.x;
            if (newWidth < 280) newWidth = 280;
            if (newWidth > rcParent.right - 200) newWidth = rcParent.right - 200;
            s_iPanelWidth = newWidth;
            SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                        MAKELPARAM(rcParent.right, rcParent.bottom));
            break;
        }
        BOOL wasHover = s_bCloseHover;
        s_bCloseHover = PtInRect(&s_rcCloseBtn, pt);
        if (s_bCloseHover != wasHover) {
            InvalidateRect(hwnd, &s_rcCloseBtn, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (s_bCloseHover) {
            s_bCloseHover = FALSE;
            InvalidateRect(hwnd, &s_rcCloseBtn, FALSE);
        }
        break;

    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&s_rcCloseBtn, pt)) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        if (PtInRect(&s_rcCloseBtn, pt)) {
            ChatPanel_Hide();
            return 0;
        }
        break;
    }

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        break;

    case WM_COMMAND:
        if (ChatPanel_HandleCommand(wParam, lParam)) return 0;
        break;

    default:
        if (msg == WM_AI_DIRECT_RESPONSE) {
            char* pszResponse = (char*)lParam;
            if (pszResponse) {
                ChatPanel_AppendResponse(pszResponse);
                free(pszResponse);
            }
            return 0;
        }
        if (msg == WM_AI_AGENT_STATUS) {
            char* pszStatus = (char*)lParam;
            if (pszStatus) {
                ChatPanel_AppendSystem(pszStatus);
                free(pszStatus);
            }
            return 0;
        }
        if (msg == WM_AI_AGENT_TOOL) {
            char* pszTool = (char*)lParam;
            if (pszTool) {
                ChatPanel_AppendSystem(pszTool);
                free(pszTool);
            }
            return 0;
        }
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

//=============================================================================
// Internal: Input subclass
//=============================================================================

static LRESULT CALLBACK ChatInputSubclassProc(HWND hwnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PASTE:
        if (HandleClipboardPaste())
            return 0; // Handled as attachment
        break;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
            ChatPanel_SendInput();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            ChatPanel_Hide();
            return 0;
        }
        break;

    case WM_CHAR:
        if (wParam == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000))
            return 0;
        break;

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
// Internal: Send button subclass
//=============================================================================

static LRESULT CALLBACK SendBtnProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!s_bSendHover) {
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

static LRESULT CALLBACK AttachBtnProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!s_bAttachHover)
        {
            s_bAttachHover = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        s_bAttachHover = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return CallWindowProc(s_pfnOrigAttachProc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK SearchBtnProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_MOUSEMOVE:
        if (!s_bSearchHover)
        {
            s_bSearchHover = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        s_bSearchHover = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_HAND));
        return TRUE;
    }
    return CallWindowProc(s_pfnOrigSearchProc, hwnd, msg, wParam, lParam);
}

static void ChatPanel_InvokeAttachmentPicker(void)
{
    // Filter: All Attachments | *.*  (could be specific)
    // Actually we support Images + Code
    WCHAR szFile[MAX_PATH] = L"";
    OPENFILENAMEW ofn;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = s_hwndPanel;
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"All Files\0*.*\0Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE)
    {
        // Check extension for image
        WCHAR* ext = PathFindExtensionW(szFile);
        if (_wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 ||
            _wcsicmp(ext, L".jpeg") == 0 || _wcsicmp(ext, L".bmp") == 0)
        {
            PendingAttachments_AddImage(szFile, NULL);
        }
        else
        {
            PendingAttachments_AddFile(szFile, NULL);
        }
    }
}

static int GetPendingSectionHeight(void)
{
    if (s_pendingAttachmentCount <= 0)
        return 0;
    return CHAT_PENDING_PAD_TOP + CHAT_ATTACHMENT_THUMB_W * 0 + CHAT_ATTACHMENT_THUMB_H + CHAT_ATTACHMENT_LABEL_H + CHAT_PENDING_PAD_TOP;
}

static BOOL SaveBitmapToTempPng(HBITMAP hBitmap, WCHAR* wszOutPath, size_t cchOut)
{
    if (!EnsureGdiplus()) return FALSE;
    if (!EnsureAttachmentTempDir()) return FALSE;

    WCHAR destPath[MAX_PATH];
    if (FAILED(StringCchCopyW(destPath, MAX_PATH, s_wszAttachmentDir))) return FALSE;
    if (FAILED(StringCchCatW(destPath, MAX_PATH, L"\\"))) return FALSE;
    WCHAR fileName[MAX_PATH];
    if (!GetTempFileNameW(s_wszAttachmentDir, L"img", 0, fileName)) return FALSE;
    
    // We need .png extension for MIME type guessing later (and GDI+ encoder lookup) but GetTempFileName creates .tmp
    // Let's rename or just use .tmp and force png mime.
    // Actually GDI+ saves based on encoder CLSID.
    StringCchPrintfW(wszOutPath, cchOut, L"%s.png", fileName);
    MoveFileW(fileName, wszOutPath); // Rename empty tmp to png

    Gdiplus::Bitmap bmp(hBitmap, (HPALETTE)NULL);
    
    CLSID pngClsid;
    // GetEncoderClsid(L"image/png", &pngClsid); // Helper needed
    // Hardcode or find logic.
    // Simple loop to find encoder:
    UINT  num = 0;
    UINT  size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return FALSE;
    Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
    if (!pImageCodecInfo) return FALSE;
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    
    BOOL found = FALSE;
    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(pImageCodecInfo[j].MimeType, L"image/png") == 0)
        {
            pngClsid = pImageCodecInfo[j].Clsid;
            found = TRUE;
            break;
        }
    }
    free(pImageCodecInfo);
    
    if (!found) return FALSE;

    return (bmp.Save(wszOutPath, &pngClsid, NULL) == Gdiplus::Ok);
}

static BOOL PendingAttachments_AddClipboardImage(void)
{
    if (!OpenClipboard(NULL)) return FALSE;
    
    HBITMAP hBmp = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (hBmp)
    {
        WCHAR szPath[MAX_PATH];
        if (SaveBitmapToTempPng(hBmp, szPath, MAX_PATH))
        {
            PendingAttachments_AddImage(szPath, L"Pasted Image");
        }
    }
    CloseClipboard();
    return TRUE;
}

static BOOL HandleClipboardPaste(void)
{
    // 1. Check for bitmap
    if (IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB))
    {
        PendingAttachments_AddClipboardImage();
        return TRUE;
    }
    // 2. Check for file drop (CF_HDROP)
    if (IsClipboardFormatAvailable(CF_HDROP))
    {
        if (OpenClipboard(NULL))
        {
            HDROP hDrop = (HDROP)GetClipboardData(CF_HDROP);
            if (hDrop)
            {
                AddAttachmentsFromDrop(hDrop);
                CloseClipboard();
                return TRUE;
            }
            CloseClipboard();
        }
    }
    return FALSE; // Let default handler handle text
}

static BOOL AddAttachmentsFromDrop(HDROP hDrop)
{
    int count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    for (int i = 0; i < count; i++)
    {
        WCHAR szFile[MAX_PATH];
        DragQueryFileW(hDrop, i, szFile, MAX_PATH);
        
        WCHAR* ext = PathFindExtensionW(szFile);
        if (_wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 ||
            _wcsicmp(ext, L".jpeg") == 0 || _wcsicmp(ext, L".bmp") == 0 ||
            _wcsicmp(ext, L".gif") == 0)
        {
            PendingAttachments_AddImage(szFile, NULL);
        }
        else
        {
            PendingAttachments_AddFile(szFile, NULL);
        }
    }
    DragFinish(hDrop);
    return TRUE;
}

static void DrawAttachmentTile(HDC hdc, ChatAttachment* pAtt, const RECT* prcTile)
{
    RECT thumbRect = *prcTile;
    thumbRect.bottom = thumbRect.top + CHAT_ATTACHMENT_THUMB_H;
    RECT textRect = *prcTile;
    textRect.top = thumbRect.bottom + 2;

    FillRoundRect(hdc, &thumbRect, 6, CP_ATTACHMENT_BG, CP_ATTACHMENT_BORDER);

    if (pAtt->hPreview)
    {
        HDC mem = CreateCompatibleDC(hdc);
        if (mem)
        {
            HBITMAP oldBmp = (HBITMAP)SelectObject(mem, pAtt->hPreview);
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc,
                       thumbRect.left + 2,
                       thumbRect.top + 2,
                       thumbRect.right - thumbRect.left - 4,
                       thumbRect.bottom - thumbRect.top - 4,
                       mem,
                       0, 0,
                       max(1, pAtt->previewW),
                       max(1, pAtt->previewH),
                       SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteDC(mem);
        }
    }
    else
    {
        // Simple placeholder icon (paperclip-like)
        HPEN hPen = CreatePen(PS_SOLID, 2, CP_TEXT_SECONDARY);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        int cx = (thumbRect.left + thumbRect.right) / 2;
        int cy = (thumbRect.top + thumbRect.bottom) / 2;
        Ellipse(hdc, cx - 12, cy - 18, cx + 12, cy + 18);
        MoveToEx(hdc, cx + 12, cy, NULL);
        LineTo(hdc, cx + 12, cy + 14);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    HFONT hOld = NULL;
    if (s_hFontStatus)
        hOld = (HFONT)SelectObject(hdc, s_hFontStatus);
    SetTextColor(hdc, CP_ATTACHMENT_TEXT);
    DrawTextW(hdc, pAtt->wzDisplayName[0] ? pAtt->wzDisplayName : L"attachment",
              -1, &textRect,
              DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (hOld)
        SelectObject(hdc, hOld);
}

static void DrawPendingAttachments(HDC hdc)
{
    if (s_pendingAttachmentCount <= 0 || IsRectEmpty(&s_rcPendingStrip))
        return;

    int availableW = s_rcPendingStrip.right - s_rcPendingStrip.left;
    if (availableW <= 0)
        return;

    const int tileW = CHAT_ATTACHMENT_THUMB_W;
    const int tileH = CHAT_ATTACHMENT_THUMB_H + CHAT_ATTACHMENT_LABEL_H + 2;
    int perRow = max(1, (availableW + CHAT_ATTACHMENT_GAP) / (tileW + CHAT_ATTACHMENT_GAP));

    int x = s_rcPendingStrip.left;
    int y = s_rcPendingStrip.top + CHAT_PENDING_PAD_TOP;

    for (int i = 0; i < s_pendingAttachmentCount; i++)
    {
        int row = i / perRow;
        int col = i % perRow;
        int tileX = s_rcPendingStrip.left + col * (tileW + CHAT_ATTACHMENT_GAP);
        int tileY = y + row * (tileH + CHAT_ATTACHMENT_GAP);

        RECT rcTile = { tileX, tileY, tileX + tileW, tileY + tileH };
        s_pendingAttachments[i].tileRect = rcTile;
        s_pendingAttachments[i].hasTileRect = TRUE;

        DrawAttachmentTile(hdc, &s_pendingAttachments[i], &rcTile);
    }
}
