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
#include "AICommands.h"
#include "AIBridge.h"
#include "AIProvider.h"
#include "CommonUtils.h"
#include "FileManager.h"
#include "Utils.h"
#include "ui/theme/BikodeTheme.h"
#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <gdiplus.h>
#include <urlmon.h>
#include "SciCall.h"
#include "Scintilla.h"
#include "DarkMode.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "resource.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "urlmon.lib")

//=============================================================================
// Color palette
//=============================================================================

// Background
#define CP_BG              RGB(17, 21, 28)
#define CP_HEADER          RGB(23, 28, 36)

// User bubble (right side) — neutral grey
#define CP_USER_BG         RGB(31, 36, 45)
#define CP_USER_TEXT       RGB(243, 245, 247)

// AI bubble (left side)
#define CP_AI_BG           RGB(23, 28, 36)
#define CP_AI_TEXT         RGB(243, 245, 247)
#define CP_AI_STRIP        RGB(29, 36, 48)

// Attachment chips
#define CP_ATTACHMENT_BG      RGB(29, 36, 48)
#define CP_ATTACHMENT_BORDER  RGB(42, 49, 64)
#define CP_ATTACHMENT_TEXT    RGB(168, 179, 194)

// System messages
#define CP_SYS_TEXT        RGB(110, 119, 133)

// Input area
#define CP_INPUT_BG        RGB(20, 26, 36)
#define CP_INPUT_BD        RGB(42, 49, 64)
#define CP_INPUT_FOCUS_BD  RGB(53, 224, 255)
#define CP_INPUT_WELL_BG   RGB(13, 18, 25)
#define CP_INPUT_DOCK_BG   RGB(17, 21, 28)
#define CP_INPUT_TEXT      RGB(243, 245, 247)

// Accents — no blue, neutral tones
#define CP_ACCENT          RGB(255, 212, 0)
#define CP_TEXT_PRIMARY    RGB(243, 245, 247)
#define CP_TEXT_SECONDARY  RGB(168, 179, 194)
#define CP_CLOSE_HOV       RGB(29, 36, 48)
#define CP_SEND_HOV        RGB(53, 224, 255)
#define CP_SEND_BG         RGB(255, 212, 0)
#define CP_SCROLLBAR_BG    RGB(14, 18, 24)
#define CP_SCROLLBAR_TH    RGB(42, 49, 64)
#define CP_SCROLLBAR_HOV   RGB(53, 224, 255)
#define CP_BTN_BG             RGB(23, 28, 36)
#define CP_BTN_HOV            RGB(29, 36, 48)
#define CP_BTN_DOWN           RGB(17, 21, 28)
#define CP_BTN_BORDER         RGB(42, 49, 64)
#define CP_BTN_BORDER_HOV     RGB(53, 224, 255)
#define CP_BTN_ICON           RGB(243, 245, 247)

#define CP_BTN_PRIMARY_BG     RGB(255, 212, 0)
#define CP_BTN_PRIMARY_HOV    RGB(255, 224, 48)
#define CP_BTN_PRIMARY_DOWN   RGB(223, 181, 0)
#define CP_BTN_PRIMARY_BORDER RGB(255, 212, 0)

//=============================================================================
// Layout constants
//=============================================================================

#define CHAT_PANEL_WIDTH        360
#define CHAT_HEADER_HEIGHT      58
#define CHAT_INPUT_AREA_HEIGHT  108
#define CHAT_INPUT_PAD          10
#define CHAT_INPUT_INNER_PAD    10
#define CHAT_INPUT_RADIUS       8
#define CHAT_SEND_SIZE          32
#define CHAT_BTN_GAP             8
#define CHAT_HDR_BTN_SIZE       24
#define CHAT_STATUS_DOT_R       4
#define CHAT_EMPTY_CARD_INSET   16
#define CHAT_EMPTY_CHIP_H       24
#define CHAT_EMPTY_CHIP_GAP      8
#define CHAT_COMPOSER_TAG_H     20
#define CHAT_COMPOSER_HINT_H    16

#define CHAT_BUBBLE_PAD_H     12
#define CHAT_BUBBLE_PAD_V      7
#define CHAT_BUBBLE_RADIUS    12
#define CHAT_BUBBLE_MAX_PCT   78   // max width = 78% of chat area
#define CHAT_BUBBLE_GAP        6
#define CHAT_MSG_SPACING      12
#define CHAT_LABEL_GAP         4
#define CHAT_SCROLL_W         6

#define CHAT_ATTACHMENT_THUMB_W   96
#define CHAT_ATTACHMENT_THUMB_H   64
#define CHAT_ATTACHMENT_LABEL_H   18
#define CHAT_ATTACHMENT_GAP        6
#define CHAT_PENDING_PAD_TOP       6
#define CHAT_PENDING_PAD_BOTTOM    6
#define CHAT_INLINE_GIF_MAX_W    220
#define CHAT_INLINE_GIF_MAX_H    140
#define CHAT_INLINE_GIF_GAP        6
#define CHAT_INLINE_GIF_FRAME_PAD  6
#define CHAT_INLINE_GIF_FRAME_RAD  8
#define CHAT_GIF_TIMER_ID      0x71A1
#define CHAT_GIF_TIMER_MS         80

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
    // Optional inline GIF rendered under AI text.
    Gdiplus::Image* pInlineGif;
    GUID            gifFrameGuid;
    UINT            gifFrameCount;
    UINT            gifFrameIndex;
    UINT*           gifDelaysCs;   // centiseconds per frame
    DWORD           gifLastTick;
    int             gifDrawW;
    int             gifDrawH;
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
static BOOL     s_bSendDown = FALSE;
static BOOL     s_bAttachDown = FALSE;
static BOOL     s_bSearchDown = FALSE;
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
static RECT     s_rcInputWell = { 0 };
static RECT     s_rcComposerTag = { 0 };
static RECT     s_rcComposerHint = { 0 };
static RECT     s_rcComposerDock = { 0 };
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
                       const ChatAttachment* attachments, int attachmentCount);
static void InvalidateAllHeights(void);
static int  MeasureMsgHeight(HDC hdc, int idx, int chatW);
static int  ComputeContentHeight(HDC hdc, int chatW);
static void EnsureScrollEnd(void);
static void ClampScroll(void);
static void PaintChatView(HWND hwnd, HDC hdc, int cx, int cy);
static void DrawPendingAttachments(HDC hdc);
static int  GetPendingSectionHeight(void);
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
static void DrawAttachmentTile(HDC hdc, ChatAttachment* pAtt, const RECT* prcTile);
static BOOL PendingAttachments_AddFile(const WCHAR* wszSourcePath, const WCHAR* wszDisplayName);
static BOOL PendingAttachments_AddImage(const WCHAR* wszSourcePath, const WCHAR* wszDisplayName);
static BOOL IsMissionIdleState(void);
static int  MeasureThemeTextWidth(HDC hdc, HFONT hFont, LPCWSTR text);
static int  MeasureWrappedTextHeight(HDC hdc, HFONT hFont, LPCWSTR text, int width);
static void Utf8ToWideCompactLabel(const char* text, WCHAR* out, int cchOut);
static void BuildMissionModelLabel(WCHAR* out, int cchOut);
static void BuildMissionStatusLabel(WCHAR* out, int cchOut, COLORREF* pAccent, int* pProgressPct);
static int  DrawQuickActionChip(HDC hdc, int x, int y, LPCWSTR label, COLORREF accent);
static void PaintMissionEmptyState(HDC hdc, int cx, int cy);
static BOOL ExtractGifUrl(const char* text, char* outUrl, int cchOut)
{
    if (!text || !outUrl || cchOut <= 1) return FALSE;
    outUrl[0] = '\0';

    const char* p = text;
    while ((p = strstr(p, "http")) != NULL)
    {
        const char* end = p;
        while (*end && !isspace((unsigned char)*end) && *end != '"' && *end != '\'' && *end != ')' && *end != ']')
            end++;
        int len = (int)(end - p);
        if (len > 0 && len < cchOut)
        {
            char tmp[1024];
            if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
            memcpy(tmp, p, len);
            tmp[len] = '\0';
            while (len > 0 && (tmp[len - 1] == '.' || tmp[len - 1] == ',' || tmp[len - 1] == ';' || tmp[len - 1] == ':'))
                tmp[--len] = '\0';

            if (StrStrIA(tmp, ".gif") != NULL ||
                StrStrIA(tmp, "media.tenor.com/") != NULL ||
                StrStrIA(tmp, "i.giphy.com/") != NULL ||
                StrStrIA(tmp, "media.giphy.com/") != NULL)
            {
                StringCchCopyA(outUrl, cchOut, tmp);
                return TRUE;
            }
        }
        p = end;
    }
    return FALSE;
}

static BOOL IsGifFilePath(const WCHAR* path)
{
    if (!path || !path[0]) return FALSE;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    BYTE hdr[6];
    DWORD read = 0;
    BOOL ok = ReadFile(h, hdr, sizeof(hdr), &read, NULL);
    CloseHandle(h);
    if (!ok || read < 6) return FALSE;
    return (memcmp(hdr, "GIF87a", 6) == 0 || memcmp(hdr, "GIF89a", 6) == 0);
}

static BOOL LooksLikeGifProviderUrl(const char* s)
{
    if (!s) return FALSE;
    return (StrStrIA(s, ".gif") != NULL ||
            StrStrIA(s, "tenor.com/") != NULL ||
            StrStrIA(s, "giphy.com/") != NULL);
}

static void StripGifUrlsInLine(char* s)
{
    if (!s || !s[0]) return;
    char out[2048];
    int oi = 0;
    const char* p = s;
    while (*p && oi < (int)sizeof(out) - 1)
    {
        if ((p[0] == 'h' || p[0] == 'H') &&
            (StrCmpNIA(p, "http://", 7) == 0 || StrCmpNIA(p, "https://", 8) == 0))
        {
            const char* end = p;
            while (*end && !isspace((unsigned char)*end) && *end != '"' && *end != '\'' && *end != ')' && *end != ']')
                end++;
            int len = (int)(end - p);
            if (len > 0 && len < 1024)
            {
                char url[1024];
                memcpy(url, p, len);
                url[len] = '\0';
                if (LooksLikeGifProviderUrl(url))
                {
                    p = end;
                    continue;
                }
            }
        }
        out[oi++] = *p++;
    }
    out[oi] = '\0';
    StringCchCopyA(s, 2048, out);
}

static BOOL LoadGifFromPath(ChatMsg* msg, const WCHAR* wszPath)
{
    if (!msg || !wszPath || !wszPath[0] || !EnsureGdiplus())
        return FALSE;

    Gdiplus::Image* img = Gdiplus::Image::FromFile(wszPath, FALSE);
    if (!img || img->GetLastStatus() != Gdiplus::Ok)
    {
        if (img) delete img;
        return FALSE;
    }

    UINT imgW = img->GetWidth();
    UINT imgH = img->GetHeight();
    if (imgW == 0 || imgH == 0)
    {
        delete img;
        return FALSE;
    }

    msg->pInlineGif = img;
    msg->gifDrawW = CHAT_INLINE_GIF_MAX_W;
    msg->gifDrawH = (int)((double)imgH * (double)msg->gifDrawW / (double)imgW);
    if (msg->gifDrawH > CHAT_INLINE_GIF_MAX_H)
    {
        msg->gifDrawH = CHAT_INLINE_GIF_MAX_H;
        msg->gifDrawW = (int)((double)imgW * (double)msg->gifDrawH / (double)imgH);
    }
    if (msg->gifDrawW < 80) msg->gifDrawW = 80;
    if (msg->gifDrawH < 48) msg->gifDrawH = 48;

    UINT dimCount = img->GetFrameDimensionsCount();
    if (dimCount > 0)
    {
        GUID guid;
        if (img->GetFrameDimensionsList(&guid, 1) == Gdiplus::Ok)
        {
            msg->gifFrameGuid = guid;
            msg->gifFrameCount = img->GetFrameCount(&guid);
            if (msg->gifFrameCount > 1)
            {
                UINT sz = img->GetPropertyItemSize(PropertyTagFrameDelay);
                if (sz > 0)
                {
                    Gdiplus::PropertyItem* pItem = (Gdiplus::PropertyItem*)n2e_Alloc(sz);
                    if (pItem && img->GetPropertyItem(PropertyTagFrameDelay, sz, pItem) == Gdiplus::Ok)
                    {
                        msg->gifDelaysCs = (UINT*)n2e_Alloc(msg->gifFrameCount * sizeof(UINT));
                        if (msg->gifDelaysCs)
                        {
                            for (UINT i = 0; i < msg->gifFrameCount; i++)
                            {
                                UINT delay = ((UINT*)pItem->value)[i];
                                if (delay == 0) delay = 8;
                                msg->gifDelaysCs[i] = delay;
                            }
                        }
                    }
                    if (pItem) n2e_Free(pItem);
                }
            }
        }
    }

    msg->gifFrameIndex = 0;
    msg->gifLastTick = GetTickCount();
    return TRUE;
}

static BOOL TryAttachInlineGif(ChatMsg* msg, const char* sourceText)
{
    if (!msg || !sourceText || msg->role != MSG_AI)
        return FALSE;

    char gifUrl[1024];
    if (!ExtractGifUrl(sourceText, gifUrl, (int)ARRAYSIZE(gifUrl)))
        return FALSE;

    WCHAR wszUrl[1024];
    MultiByteToWideChar(CP_UTF8, 0, gifUrl, -1, wszUrl, ARRAYSIZE(wszUrl));

    WCHAR localPath[MAX_PATH] = L"";
    BOOL downloaded = FALSE;
    if (StrCmpNIW(wszUrl, L"http://", 7) == 0 || StrCmpNIW(wszUrl, L"https://", 8) == 0)
    {
        if (!EnsureAttachmentTempDir())
            return FALSE;

        WCHAR tmpName[MAX_PATH];
        if (!GetTempFileNameW(s_wszAttachmentDir, L"gif", 0, tmpName))
            return FALSE;
        DeleteFileW(tmpName);
        StringCchPrintfW(localPath, MAX_PATH, L"%s.gif", tmpName);
        if (FAILED(URLDownloadToFileW(NULL, wszUrl, localPath, 0, NULL)))
            return FALSE;
        downloaded = TRUE;
    }
    else
    {
        StringCchCopyW(localPath, MAX_PATH, wszUrl);
    }

    if (!IsGifFilePath(localPath))
    {
        if (downloaded) DeleteFileW(localPath);
        return FALSE;
    }

    return LoadGifFromPath(msg, localPath);
}

static void UpdateGifTimerState(void)
{
    if (!s_hwndChat) return;
    BOOL hasAnimatedGif = FALSE;
    for (int i = 0; i < s_nMsgs; i++)
    {
        if (s_msgs[i].pInlineGif && s_msgs[i].gifFrameCount > 1)
        {
            hasAnimatedGif = TRUE;
            break;
        }
    }
    if (hasAnimatedGif)
        SetTimer(s_hwndChat, CHAT_GIF_TIMER_ID, CHAT_GIF_TIMER_MS, NULL);
    else
        KillTimer(s_hwndChat, CHAT_GIF_TIMER_ID);
}

static void AdvanceGifFrames(void)
{
    DWORD now = GetTickCount();
    BOOL changed = FALSE;
    for (int i = 0; i < s_nMsgs; i++)
    {
        ChatMsg* m = &s_msgs[i];
        if (!m->pInlineGif || m->gifFrameCount <= 1 || !m->gifDelaysCs)
            continue;

        UINT delayMs = max(40U, m->gifDelaysCs[m->gifFrameIndex] * 10U);
        if (now - m->gifLastTick >= delayMs)
        {
            m->gifFrameIndex = (m->gifFrameIndex + 1) % m->gifFrameCount;
            m->pInlineGif->SelectActiveFrame(&m->gifFrameGuid, m->gifFrameIndex);
            m->gifLastTick = now;
            changed = TRUE;
        }
    }
    if (changed && s_hwndChat)
        InvalidateRect(s_hwndChat, NULL, FALSE);
}
static BOOL TryAttachInlineGif(ChatMsg* msg, const char* sourceText);
static void UpdateGifTimerState(void);
static void AdvanceGifFrames(void);
static char* SanitizeAIResponseForDisplay(const char* text, BOOL hideGifUrls);

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
    if (!s_hwndPanel) {
        return;
    }
    InvalidateRect(s_hwndPanel, &s_rcInputCard, FALSE);
    HWND hwndParent = GetParent(s_hwndPanel);
    if (hwndParent && s_lastLayoutParentRight > 0 && s_lastLayoutEditorHeight > 0) {
        ChatPanel_Layout(hwndParent, s_lastLayoutParentRight, s_lastLayoutEditorTop, s_lastLayoutEditorHeight);
    }
    if (s_hwndChat) {
        InvalidateRect(s_hwndChat, NULL, FALSE);
    }
    InvalidateRect(s_hwndPanel, NULL, FALSE);
}

static BOOL PendingAttachments_AddFromPath(const WCHAR* wszSourcePath)
{
    if (!wszSourcePath || !wszSourcePath[0]) {
        return FALSE;
    }
    WCHAR* ext = PathFindExtensionW(wszSourcePath);
    if (ext && (*ext != L'\0') &&
        (_wcsicmp(ext, L".png") == 0 || _wcsicmp(ext, L".jpg") == 0 ||
         _wcsicmp(ext, L".jpeg") == 0 || _wcsicmp(ext, L".bmp") == 0 ||
         _wcsicmp(ext, L".gif") == 0 || _wcsicmp(ext, L".webp") == 0)) {
        return PendingAttachments_AddImage(wszSourcePath, NULL);
    }
    return PendingAttachments_AddFile(wszSourcePath, NULL);
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
    s_hFontHeader = BikodeTheme_GetFont(BKFONT_TITLE);
    s_hFontInput = BikodeTheme_GetFont(BKFONT_UI);
    s_hFontStatus = BikodeTheme_GetFont(BKFONT_MONO_SMALL);
    s_hFontLabel = BikodeTheme_GetFont(BKFONT_UI_BOLD);
    s_hFontBubble = BikodeTheme_GetFont(BKFONT_UI);
}

static void DestroyFonts(void)
{
    s_hFontHeader = NULL;
    s_hFontInput = NULL;
    s_hFontStatus = NULL;
    s_hFontLabel = NULL;
    s_hFontBubble = NULL;
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
    POINT plane[3];
    plane[0].x = cx - 5; plane[0].y = cy - 5;
    plane[1].x = cx + 6; plane[1].y = cy;
    plane[2].x = cx - 5; plane[2].y = cy + 5;

    HBRUSH hFill = CreateSolidBrush(clr);
    HPEN hPen = CreatePen(PS_SOLID, 1, clr);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, hFill);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    Polygon(hdc, plane, 3);

    HPEN hTail = CreatePen(PS_SOLID, 2, clr);
    SelectObject(hdc, hTail);
    MoveToEx(hdc, cx - 2, cy, NULL);
    LineTo(hdc, cx + 2, cy);

    SelectObject(hdc, hOldBr);
    SelectObject(hdc, hOldPen);
    DeleteObject(hTail);
    DeleteObject(hPen);
    DeleteObject(hFill);
}

static void DrawSearchIcon(HDC hdc, int cx, int cy, COLORREF clr)
{
    HPEN hPen = CreatePen(PS_SOLID, 2, clr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, cx - 5, cy - 6, cx + 4, cy + 3);
    MoveToEx(hdc, cx + 2, cy + 2, NULL);
    LineTo(hdc, cx + 6, cy + 6);
    SelectObject(hdc, hOldBr);
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

static void DrawAddIcon(HDC hdc, int cx, int cy, COLORREF clr)
{
    HPEN hPen = CreatePen(PS_SOLID, 2, clr);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, cx, cy - 6, NULL); LineTo(hdc, cx, cy + 6);
    MoveToEx(hdc, cx - 6, cy, NULL); LineTo(hdc, cx + 6, cy);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

static void DrawButtonFace(HDC hdc, const RECT* rc, BOOL hover, BOOL down, BOOL primary)
{
    if (!rc) return;

    COLORREF fill = primary
        ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), 56)
        : CP_INPUT_DOCK_BG;
    COLORREF border = primary ? CP_BTN_PRIMARY_BORDER : CP_BTN_BORDER;
    if (down) {
        fill = primary ? CP_BTN_PRIMARY_DOWN : BikodeTheme_GetColor(BKCLR_SURFACE_MAIN);
        border = primary ? CP_BTN_PRIMARY_BORDER : BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
    } else if (hover) {
        fill = primary
            ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 84)
            : BikodeTheme_GetColor(BKCLR_SURFACE_RAISED);
        if (!primary) border = CP_BTN_BORDER_HOV;
    }

    RECT rcButton = *rc;
    InflateRect(&rcButton, -1, -1);
    FillRoundRect(hdc, &rcButton, 8, fill, border);

    RECT rcInner = rcButton;
    InflateRect(&rcInner, -1, -1);
    COLORREF innerFill = fill;
    if (!down) {
        innerFill = primary
            ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), CP_INPUT_DOCK_BG, hover ? 140 : 112)
            : CP_INPUT_BG;
        if (hover && !primary)
            innerFill = BikodeTheme_GetColor(BKCLR_SURFACE_MAIN);
    }
    FillRoundRectSolid(hdc, &rcInner, 7, innerFill);

    RECT rcHighlight = rcInner;
    InflateRect(&rcHighlight, -2, -2);
    COLORREF hl = primary
        ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), 104)
        : (hover ? BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT));
    HPEN hPen = CreatePen(PS_SOLID, 1, hl);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, rcHighlight.left + 3, rcHighlight.top + 1, NULL);
    LineTo(hdc, rcHighlight.right - 3, rcHighlight.top + 1);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

static BOOL IsMissionIdleState(void)
{
    if (s_nMsgs == 0)
        return TRUE;
    if (s_nMsgs != 1 || s_msgs[0].role != MSG_SYSTEM)
        return FALSE;
    return (s_msgs[0].wtext && StrStrIW(s_msgs[0].wtext, L"ready") != NULL);
}

static int MeasureThemeTextWidth(HDC hdc, HFONT hFont, LPCWSTR text)
{
    SIZE sz = { 0 };
    HFONT hOld;

    if (!hdc || !text || !text[0])
        return 0;

    hOld = (HFONT)SelectObject(hdc, hFont ? hFont : BikodeTheme_GetFont(BKFONT_UI_SMALL));
    GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
    SelectObject(hdc, hOld);
    return sz.cx;
}

static int MeasureWrappedTextHeight(HDC hdc, HFONT hFont, LPCWSTR text, int width)
{
    RECT rcCalc = { 0, 0, max(width, 1), 0 };
    HFONT hOld;

    if (!hdc || !text || !text[0] || width <= 0)
        return 0;

    hOld = (HFONT)SelectObject(hdc, hFont ? hFont : BikodeTheme_GetFont(BKFONT_UI_SMALL));
    DrawTextW(hdc, text, -1, &rcCalc, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(hdc, hOld);
    return rcCalc.bottom - rcCalc.top;
}

static void Utf8ToWideCompactLabel(const char* text, WCHAR* out, int cchOut)
{
    const char* tail = text;
    if (!out || cchOut <= 0)
        return;
    out[0] = L'\0';
    if (!text || !text[0])
        return;

    {
        const char* slash = strrchr(text, '/');
        if (slash && slash[1])
            tail = slash + 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, tail, -1, out, cchOut);
}

static void BuildMissionModelLabel(WCHAR* out, int cchOut)
{
    const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
    const AIProviderDef* pDef = pCfg ? AIProvider_Get(pCfg->eProvider) : NULL;

    if (!out || cchOut <= 0)
        return;
    out[0] = L'\0';

    if (!pCfg || !pDef)
    {
        lstrcpynW(out, L"AI", cchOut);
        return;
    }

    if (pCfg->szModel[0] &&
        (!pDef->szDefaultModel || _stricmp(pCfg->szModel, pDef->szDefaultModel) != 0))
    {
        Utf8ToWideCompactLabel(pCfg->szModel, out, cchOut);
        if (out[0])
            return;
    }

    if (pDef->szName)
    {
        MultiByteToWideChar(CP_UTF8, 0, pDef->szName, -1, out, cchOut);
    }
    if (!out[0])
        lstrcpynW(out, L"AI", cchOut);
}

static void BuildMissionStatusLabel(WCHAR* out, int cchOut, COLORREF* pAccent, int* pProgressPct)
{
    WCHAR statusBuf[128];
    const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
    const AIProviderDef* pDef = pCfg ? AIProvider_Get(pCfg->eProvider) : NULL;
    EAIStatus aiStatus = AIBridge_GetStatus();
    BOOL hasConfiguredAccess = FALSE;

    if (!out || cchOut <= 0)
        return;
    out[0] = L'\0';

    if (pAccent)
        *pAccent = BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
    if (pProgressPct)
        *pProgressPct = 18;

    if (pDef)
        hasConfiguredAccess = pDef->bIsLocal || !pDef->bRequiresKey || (pCfg && pCfg->szApiKey[0]);

    AICommands_GetStatusText(statusBuf, ARRAYSIZE(statusBuf));
    if (statusBuf[0] && _wcsnicmp(statusBuf, L"AI: ", 4) != 0)
    {
        lstrcpynW(out, statusBuf, cchOut);
        if (pAccent)
            *pAccent = BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
        if (pProgressPct)
            *pProgressPct = 72;
        return;
    }

    switch (aiStatus)
    {
    case AI_STATUS_READY:
        lstrcpynW(out, L"Ready", cchOut);
        if (pAccent) *pAccent = BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN);
        if (pProgressPct) *pProgressPct = 100;
        break;
    case AI_STATUS_THINKING:
        lstrcpynW(out, L"Thinking", cchOut);
        if (pAccent) *pAccent = BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
        if (pProgressPct) *pProgressPct = 74;
        break;
    case AI_STATUS_PATCH_READY:
        lstrcpynW(out, L"Patch Ready", cchOut);
        if (pAccent) *pAccent = BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);
        if (pProgressPct) *pProgressPct = 100;
        break;
    case AI_STATUS_CONNECTING:
        lstrcpynW(out, L"Connecting", cchOut);
        if (pAccent) *pAccent = BikodeTheme_GetColor(BKCLR_WARNING_ORANGE);
        if (pProgressPct) *pProgressPct = 38;
        break;
    case AI_STATUS_ERROR:
        lstrcpynW(out, L"Error", cchOut);
        if (pAccent) *pAccent = BikodeTheme_GetColor(BKCLR_DANGER_RED);
        if (pProgressPct) *pProgressPct = 100;
        break;
    case AI_STATUS_OFFLINE:
    default:
        if (hasConfiguredAccess)
        {
            lstrcpynW(out, L"Configured", cchOut);
            if (pAccent) *pAccent = BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);
            if (pProgressPct) *pProgressPct = 46;
        }
        else
        {
            lstrcpynW(out, L"Needs Key", cchOut);
            if (pAccent) *pAccent = BikodeTheme_GetColor(BKCLR_HOT_MAGENTA);
            if (pProgressPct) *pProgressPct = 12;
        }
        break;
    }
}

static int DrawQuickActionChip(HDC hdc, int x, int y, LPCWSTR label, COLORREF accent)
{
    RECT rcChip = { 0 };
    int chipW = MeasureThemeTextWidth(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL), label) + 28;

    rcChip.left = x;
    rcChip.top = y;
    rcChip.right = x + chipW;
    rcChip.bottom = y + CHAT_EMPTY_CHIP_H;

    BikodeTheme_DrawChip(hdc, &rcChip, label,
        BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
        BikodeTheme_GetFont(BKFONT_UI_SMALL), TRUE, accent);

    return chipW;
}

static void PaintMissionEmptyState(HDC hdc, int cx, int cy)
{
    RECT rcHero;
    RECT rcEyebrow;
    RECT rcTitle;
    RECT rcBody;
    RECT rcHint;
    RECT rcRail;
    int chipX;
    int chipY;
    int curY;
    int contentLeft;
    int contentRight;
    int contentWidth;
    int titleH;
    int bodyH;
    int hintH;
    int chipBottom;
    static const WCHAR* s_labels[] = {
        L"Trace this crash",
        L"Explain selection",
        L"Map this repo"
    };
    static const BikodeColorToken s_accents[] = {
        BKCLR_SIGNAL_YELLOW,
        BKCLR_ELECTRIC_CYAN,
        BKCLR_HOT_MAGENTA
    };

    rcHero.left = CHAT_EMPTY_CARD_INSET;
    rcHero.top = CHAT_EMPTY_CARD_INSET;
    rcHero.right = cx - CHAT_EMPTY_CARD_INSET;
    rcHero.bottom = cy - CHAT_EMPTY_CARD_INSET;

    if (rcHero.right <= rcHero.left || rcHero.bottom <= rcHero.top)
        return;

    contentLeft = rcHero.left + 24;
    contentRight = rcHero.right - 24;
    contentWidth = contentRight - contentLeft;
    if (contentWidth <= 40)
        return;

    titleH = max(28, MeasureWrappedTextHeight(hdc, BikodeTheme_GetFont(BKFONT_TITLE),
        L"Start with a real mission.", contentWidth));
    bodyH = MeasureWrappedTextHeight(hdc, BikodeTheme_GetFont(BKFONT_UI),
        L"Ask Bikode AI to trace a crash, explain a symbol, search the repo, or write a patch with the current file context.",
        contentWidth);
    hintH = MeasureWrappedTextHeight(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL),
        L"Paste screenshots, drop files, or use search to pull repo context into the thread.",
        contentWidth);

    chipX = contentLeft;
    chipY = 0;
    for (int i = 0; i < ARRAYSIZE(s_labels); i++)
    {
        int chipW = MeasureThemeTextWidth(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL), s_labels[i]) + 28;
        if (chipX != contentLeft && chipX + chipW > contentRight)
        {
            chipX = contentLeft;
            chipY += CHAT_EMPTY_CHIP_H + CHAT_EMPTY_CHIP_GAP;
        }
        chipX += chipW + CHAT_EMPTY_CHIP_GAP;
    }
    chipBottom = chipY + CHAT_EMPTY_CHIP_H;

    curY = 18 + 24 + 14 + titleH + 8 + bodyH + 12 + chipBottom + 12 + hintH + 16;
    rcHero.bottom = min(cy - CHAT_EMPTY_CARD_INSET, rcHero.top + max(curY, 228));

    BikodeTheme_DrawCutCornerPanel(hdc, &rcHero,
        BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        12, TRUE);

    rcRail = rcHero;
    rcRail.left += 12;
    rcRail.top += 14;
    rcRail.right = rcRail.left + 5;
    rcRail.bottom -= 14;
    {
        HBRUSH hRail = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
        FillRect(hdc, &rcRail, hRail);
        DeleteObject(hRail);
    }

    curY = rcHero.top + 18;
    rcEyebrow.left = contentLeft;
    rcEyebrow.top = curY;
    rcEyebrow.right = rcEyebrow.left + 130;
    rcEyebrow.bottom = rcEyebrow.top + 24;
    BikodeTheme_DrawChip(hdc, &rcEyebrow, L"MISSION BOARD",
        BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
        BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
        BikodeTheme_GetColor(BKCLR_HOT_MAGENTA));

    curY = rcEyebrow.bottom + 14;
    rcTitle.left = contentLeft;
    rcTitle.top = curY;
    rcTitle.right = contentRight;
    rcTitle.bottom = rcTitle.top + titleH;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_TITLE));
    DrawTextW(hdc, L"Start with a real mission.", -1, &rcTitle,
        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    rcBody.left = contentLeft;
    rcBody.top = rcTitle.bottom + 8;
    rcBody.right = contentRight;
    rcBody.bottom = rcBody.top + bodyH;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI));
    DrawTextW(hdc,
        L"Ask Bikode AI to trace a crash, explain a symbol, search the repo, or write a patch with the current file context.",
        -1, &rcBody, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    chipX = contentLeft;
    chipY = rcBody.bottom + 12;
    for (int i = 0; i < ARRAYSIZE(s_labels); i++)
    {
        int chipW = MeasureThemeTextWidth(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL), s_labels[i]) + 28;
        if (chipX != contentLeft && chipX + chipW > contentRight)
        {
            chipX = contentLeft;
            chipY += CHAT_EMPTY_CHIP_H + CHAT_EMPTY_CHIP_GAP;
        }
        chipW = DrawQuickActionChip(hdc, chipX, chipY, s_labels[i],
            BikodeTheme_GetColor(s_accents[i]));
        chipX += chipW + CHAT_EMPTY_CHIP_GAP;
    }

    rcHint.left = contentLeft;
    rcHint.top = chipY + CHAT_EMPTY_CHIP_H + 12;
    rcHint.right = contentRight;
    rcHint.bottom = rcHint.top + hintH;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL));
    DrawTextW(hdc, L"Paste screenshots, drop files, or use search to pull repo context into the thread.", -1,
        &rcHint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
}

static BOOL StartsWithNoCase(const char* s, const char* prefix)
{
    if (!s || !prefix) return FALSE;
    while (*prefix)
    {
        if (*s == '\0') return FALSE;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return FALSE;
        s++;
        prefix++;
    }
    return TRUE;
}

static char* TrimLeadingSpaces(char* s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void TrimTrailingSpaces(char* s)
{
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static BOOL IsInternalReasoningLine(const char* line)
{
    const char* p = line;
    if (p[0] == '-' && p[1] == ' ') p += 2;
    if (p[0] == '*' && p[1] == ' ') p += 2;

    if (StartsWithNoCase(p, "take stock")) return TRUE;
    if (StartsWithNoCase(p, "goal:")) return TRUE;
    if (StartsWithNoCase(p, "current state:")) return TRUE;
    if (StartsWithNoCase(p, "current stock:")) return TRUE;
    if (StartsWithNoCase(p, "constraint:")) return TRUE;
    if (StartsWithNoCase(p, "constraints:")) return TRUE;
    if (StartsWithNoCase(p, "unknown:")) return TRUE;
    if (StartsWithNoCase(p, "unknowns:")) return TRUE;
    if (StartsWithNoCase(p, "### take stock")) return TRUE;
    if (StartsWithNoCase(p, "[web_search:")) return TRUE;
    return FALSE;
}

static char* SanitizeAIResponseForDisplay(const char* text, BOOL hideGifUrls)
{
    if (!text) return NULL;
    int srcLen = (int)strlen(text);
    if (srcLen <= 0) {
        char* empty = (char*)n2e_Alloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    char* tmp = (char*)n2e_Alloc(srcLen + 1);
    if (!tmp) {
        char* copy = (char*)n2e_Alloc(srcLen + 1);
        if (copy) memcpy(copy, text, srcLen + 1);
        return copy;
    }
    memcpy(tmp, text, srcLen + 1);

    int cap = srcLen * 2 + 64;
    char* out = (char*)n2e_Alloc(cap);
    if (!out) {
        n2e_Free(tmp);
        char* copy = (char*)n2e_Alloc(srcLen + 1);
        if (copy) memcpy(copy, text, srcLen + 1);
        return copy;
    }
    out[0] = '\0';
    int outLen = 0;
    BOOL prevBlank = FALSE;

    char* ctx = NULL;
    for (char* line = strtok_s(tmp, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx))
    {
        char* p = TrimLeadingSpaces(line);
        TrimTrailingSpaces(p);

        while (*p == '#') {
            p++;
            while (*p == ' ') p++;
        }

        if (IsInternalReasoningLine(p)) {
            continue;
        }

        if (hideGifUrls && LooksLikeGifProviderUrl(p))
            continue;

        char cleaned[2048];
        int ci = 0;
        for (const char* c = p; *c && ci < (int)sizeof(cleaned) - 1; c++)
        {
            if (*c == '*' || *c == '`' || *c == '#') continue;
            cleaned[ci++] = *c;
        }
        cleaned[ci] = '\0';

        if (hideGifUrls)
            StripGifUrlsInLine(cleaned);

        char* finalLine = TrimLeadingSpaces(cleaned);
        TrimTrailingSpaces(finalLine);
        if (!finalLine[0]) {
            if (!prevBlank && outLen > 0) {
                if (outLen + 1 < cap) out[outLen++] = '\n';
                prevBlank = TRUE;
            }
            continue;
        }

        int need = (int)strlen(finalLine) + 2;
        if (outLen + need >= cap) {
            int newCap = cap * 2 + need;
            char* grown = (char*)n2e_Realloc(out, newCap);
            if (!grown) break;
            out = grown;
            cap = newCap;
        }
        memcpy(out + outLen, finalLine, strlen(finalLine));
        outLen += (int)strlen(finalLine);
        out[outLen++] = '\n';
        out[outLen] = '\0';
        prevBlank = FALSE;
    }

    while (outLen > 0 && (out[outLen - 1] == '\n' || out[outLen - 1] == '\r' || out[outLen - 1] == ' ')) {
        out[--outLen] = '\0';
    }

    n2e_Free(tmp);
    if (outLen == 0) {
        n2e_Free(out);
        if (hideGifUrls)
        {
            char* empty = (char*)n2e_Alloc(1);
            if (empty) empty[0] = '\0';
            return empty;
        }
        const char* fallback = "I could not find clear results for that query. Try adding keywords like official, github, location, or industry.";
        int n = (int)strlen(fallback);
        char* msg = (char*)n2e_Alloc(n + 1);
        if (msg) memcpy(msg, fallback, n + 1);
        return msg;
    }
    return out;
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

    const char* sourceText = text ? text : "";
    BOOL hasInlineGif = FALSE;
    BOOL hasGifUrl = FALSE;
    char gifProbe[1024];
    if (role == MSG_AI) {
        hasGifUrl = ExtractGifUrl(sourceText, gifProbe, (int)ARRAYSIZE(gifProbe));
    }
    if (role == MSG_AI) {
        hasInlineGif = TryAttachInlineGif(m, sourceText);
    }
    char* sanitized = NULL;
    if (role == MSG_AI) {
        sanitized = SanitizeAIResponseForDisplay(sourceText, (hasInlineGif || hasGifUrl));
        if (sanitized) sourceText = sanitized;
    }

    int len = (int)strlen(sourceText);
    m->text = (char*)n2e_Alloc(len + 1);
    if (m->text) {
        memcpy(m->text, sourceText, len);
        m->text[len] = 0;
    }

    // Convert to wide for DrawText
    int wlen = MultiByteToWideChar(CP_UTF8, 0, sourceText, -1, NULL, 0);
    m->wtext = (WCHAR*)n2e_Alloc(wlen * sizeof(WCHAR));
    if (m->wtext) {
        MultiByteToWideChar(CP_UTF8, 0, sourceText, -1, m->wtext, wlen);
        m->wtextLen = wlen - 1;
    }
    if (sanitized)
        n2e_Free(sanitized);

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
    UpdateGifTimerState();
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
    if (m->pInlineGif)
    {
        delete m->pInlineGif;
        m->pInlineGif = NULL;
    }
    if (m->gifDelaysCs)
    {
        n2e_Free(m->gifDelaysCs);
        m->gifDelaysCs = NULL;
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
    UpdateGifTimerState();
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

static BOOL BuildAttachmentSummary(const ChatAttachment* attachments, int count,
                                   AIChatAttachment* pOutMeta, int* pcOutMeta)
{
    if (pcOutMeta) {
        *pcOutMeta = 0;
    }
    if (!attachments || count <= 0 || !pOutMeta || !pcOutMeta) {
        return FALSE;
    }
    int n = count;
    if (n > AI_MAX_CHAT_ATTACHMENTS) {
        n = AI_MAX_CHAT_ATTACHMENTS;
    }
    for (int i = 0; i < n; i++) {
        pOutMeta[i] = attachments[i].meta;
    }
    *pcOutMeta = n;
    return TRUE;
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
    
    int gifH = 0;
    if (m->pInlineGif)
        gifH = CHAT_INLINE_GIF_GAP + m->gifDrawH + CHAT_INLINE_GIF_FRAME_PAD * 2;

    m->cachedH = labelH + CHAT_LABEL_GAP + bubbleH + attachH + gifH;
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

    if (IsMissionIdleState())
    {
        PaintMissionEmptyState(hdc, cx, cy);
        return;
    }

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
            if (m->pInlineGif) {
                int minGifBubbleW = m->gifDrawW + 2 * CHAT_BUBBLE_PAD_H + CHAT_INLINE_GIF_FRAME_PAD * 2;
                if (bubbleW < minGifBubbleW) bubbleW = min(maxBubbleW, minGifBubbleW);
            }

            // Label
            int labelH = 14;
            const WCHAR* label = isUser ? L"You" : L"Bikode";
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
            int attachAreaH = 0;
            if (m->attachmentCount > 0)
            {
                attachAreaH = ComputeAttachmentAreaHeight(m->attachmentCount, textAreaW);
                DrawMessageAttachments(hdc, m, bubbleX + CHAT_BUBBLE_PAD_H, 
                                       bubbleY + CHAT_BUBBLE_PAD_V + actualTextH, 
                                       textAreaW);
            }

            if (m->pInlineGif)
            {
                int frameX = bubbleX + CHAT_BUBBLE_PAD_H;
                int frameY = bubbleY + CHAT_BUBBLE_PAD_V + actualTextH + attachAreaH + CHAT_INLINE_GIF_GAP;
                int frameW = m->gifDrawW + CHAT_INLINE_GIF_FRAME_PAD * 2;
                int frameH = m->gifDrawH + CHAT_INLINE_GIF_FRAME_PAD * 2;
                RECT rcFrame = { frameX, frameY, frameX + frameW, frameY + frameH };
                FillRoundRect(hdc, &rcFrame, CHAT_INLINE_GIF_FRAME_RAD, RGB(34, 34, 38), RGB(74, 74, 80));

                int gifX = frameX + CHAT_INLINE_GIF_FRAME_PAD;
                int gifY = frameY + CHAT_INLINE_GIF_FRAME_PAD;
                Gdiplus::Graphics g(hdc);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(m->pInlineGif, gifX, gifY, m->gifDrawW, m->gifDrawH);
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

    case WM_TIMER:
        if (wParam == CHAT_GIF_TIMER_ID)
        {
            AdvanceGifFrames();
            return 0;
        }
        break;

    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

#ifdef __cplusplus
extern "C" {
#endif

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
        SetWindowTheme(s_hwndInput, L"", L"");
        SendMessage(s_hwndInput, WM_SETFONT, (WPARAM)s_hFontInput, TRUE);
        SendMessageW(s_hwndInput, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(8, 8));
        SendMessageW(s_hwndInput, EM_SETCUEBANNER, TRUE,
                     (LPARAM)L"Message Bikode\x2026");
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

    AddMessage(MSG_SYSTEM, "Bikode AI \xe2\x80\xa2 Ready", NULL, 0);
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
    if (s_gdiplusInitialized) {
        Gdiplus::GdiplusShutdown(s_gdiplusToken);
        s_gdiplusToken = 0;
        s_gdiplusInitialized = FALSE;
    }
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
    s_lastLayoutParentRight = parentRight;
    s_lastLayoutEditorTop = editorTop;
    s_lastLayoutEditorHeight = editorHeight;

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
    int cardTop    = y + 4;
    int cardRight  = x + innerW - CHAT_INPUT_PAD;
    int pendingHeight = GetPendingSectionHeight();
    int cardBottom = y + CHAT_INPUT_AREA_HEIGHT - 4 + pendingHeight;
    s_rcInputCard.left   = cardLeft;
    s_rcInputCard.top    = cardTop;
    s_rcInputCard.right  = cardRight;
    s_rcInputCard.bottom = cardBottom;

    int ip = CHAT_INPUT_INNER_PAD;
    int composerTop = cardTop + ip + pendingHeight;
    s_rcComposerTag.left = cardLeft + ip + 4;
    s_rcComposerTag.top = composerTop;
    s_rcComposerTag.right = min(cardRight - ip - 4, s_rcComposerTag.left + 116);
    s_rcComposerTag.bottom = s_rcComposerTag.top + CHAT_COMPOSER_TAG_H;

    s_rcInputWell.left = cardLeft + ip;
    s_rcInputWell.top = s_rcComposerTag.bottom + 6;
    s_rcInputWell.right = cardRight - ip;
    s_rcInputWell.bottom = cardBottom - ip;
    s_rcComposerHint.left = s_rcComposerTag.right + 8;
    s_rcComposerHint.top = s_rcComposerTag.top;
    s_rcComposerHint.right = s_rcInputWell.right;
    s_rcComposerHint.bottom = s_rcComposerTag.bottom;

    int editLeft   = s_rcInputWell.left + 8;
    int editTop    = s_rcInputWell.top + 6;
    int editBottom = s_rcInputWell.bottom - 6;

    int sendLeft = s_rcInputWell.right - 8 - CHAT_SEND_SIZE;
    int sendTop  = s_rcInputWell.bottom - 4 - CHAT_SEND_SIZE;
    if (s_hwndSend)
        MoveWindow(s_hwndSend, sendLeft, sendTop,
                   CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

    int attachLeft = sendLeft - CHAT_SEND_SIZE - CHAT_BTN_GAP;
    if (s_hwndAttach)
        MoveWindow(s_hwndAttach, attachLeft, sendTop,
                   CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

    int searchLeft = attachLeft - CHAT_SEND_SIZE - CHAT_BTN_GAP;
    if (s_hwndSearch)
        MoveWindow(s_hwndSearch, searchLeft, sendTop,
                   CHAT_SEND_SIZE, CHAT_SEND_SIZE, TRUE);

    // Adjust input right edge to avoid overlapping buttons
    int editRight = searchLeft - CHAT_BTN_GAP - 4;
    s_rcComposerDock.left = searchLeft - 6;
    s_rcComposerDock.top = sendTop - 4;
    s_rcComposerDock.right = s_rcInputWell.right + 2;
    s_rcComposerDock.bottom = sendTop + CHAT_SEND_SIZE + 4;
    if (s_hwndInput)
    {
        int inputsRight = editRight;
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
    ChatAttachment* converted = NULL;
    if (pAttachments && cAttachments > 0)
    {
        converted = (ChatAttachment*)n2e_Alloc(cAttachments * sizeof(ChatAttachment));
        if (converted)
        {
            ZeroMemory(converted, cAttachments * sizeof(ChatAttachment));
            for (int i = 0; i < cAttachments; i++)
            {
                converted[i].meta = pAttachments[i];
                MultiByteToWideChar(CP_UTF8, 0, pAttachments[i].displayName, -1,
                                    converted[i].wzDisplayName, AI_ATTACHMENT_NAME_MAX);
                MultiByteToWideChar(CP_UTF8, 0, pAttachments[i].path, -1,
                                    converted[i].wzPath, MAX_PATH);
            }
        }
    }

    AddMessage(MSG_USER, pszMessage, converted, cAttachments);

    if (converted)
        n2e_Free(converted);

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
    if (len <= 0 && s_pendingAttachmentCount <= 0) return;

    WCHAR* wszText = (WCHAR*)n2e_Alloc((len + 1) * sizeof(WCHAR));
    if (!wszText) return;
    GetWindowTextW(s_hwndInput, wszText, len + 1);

    int utf8Len = 1;
    if (len > 0)
        utf8Len = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)n2e_Alloc(utf8Len);
    if (utf8) {
        if (len > 0) {
            WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8, utf8Len, NULL, NULL);
        } else {
            utf8[0] = '\0';
        }

        AIChatAttachment aiAttachments[AI_MAX_CHAT_ATTACHMENTS];
        int cAtt = 0;
        BuildAttachmentSummary(s_pendingAttachments, s_pendingAttachmentCount, aiAttachments, &cAtt);

        const char* displayText = utf8;
        if (displayText[0] == '\0' && cAtt > 0) {
            displayText = "(attachment)";
        }
        ChatPanel_AppendUserMessage(displayText, cAtt > 0 ? aiAttachments : NULL, cAtt);

        const AIProviderConfig* pCfg = AIBridge_GetProviderConfig();
        if (pCfg && pCfg->szApiKey[0]) {
            ChatPanel_AppendSystem("Thinking\xe2\x80\xa6");
            HWND hwndMainWnd = GetParent(s_hwndPanel);

            if (!AIAgent_ChatAsync(pCfg, displayText, cAtt > 0 ? aiAttachments : NULL, cAtt, s_hwndPanel, hwndMainWnd))
                ChatPanel_AppendSystem("AI is busy. Please wait.");
        } else {
            ChatPanel_AppendSystem("No API key. Use Bikode \xe2\x86\x92 AI Settings.");
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
                ChatPanel_AppendSystem("No API key. Use Bikode \xe2\x86\x92 AI Settings.");
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

#ifdef __cplusplus
}
#endif

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

        // Header - mission-control chrome
        {
            WCHAR wszStatus[128];
            WCHAR wszModel[128] = L"AI";
            WCHAR wszContext[128] = L"CTX Current";
            RECT rcHeaderCard;
            RECT rcModel;
            RECT rcStatus;
            RECT rcContext;
            RECT rcProgress;
            RECT rcAccent;
            COLORREF statusAccent = BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
            int progressPct = 18;
            int modelW;
            int contextW;
            int statusW;
            int availableChipsW;
            int totalChipW;
            int iconX = 14;
            int iconY = 11;

            rcHeaderCard.left = 8;
            rcHeaderCard.top = 4;
            rcHeaderCard.right = cxW - 8;
            rcHeaderCard.bottom = CHAT_HEADER_HEIGHT - 4;
            BikodeTheme_DrawCutCornerPanel(hm, &rcHeaderCard,
                BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
                BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                8, TRUE);

            rcAccent = rcHeaderCard;
            rcAccent.left += 8;
            rcAccent.top += 9;
            rcAccent.right = rcAccent.left + 3;
            rcAccent.bottom -= 9;
            {
                HBRUSH hAcc = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
                FillRect(hm, &rcAccent, hAcc);
                DeleteObject(hAcc);
            }
            if (s_hIconLogo)
                DrawIconEx(hm, iconX, iconY, s_hIconLogo,
                           18, 18, 0, NULL, DI_NORMAL);

            if (s_hFontHeader) SelectObject(hm, s_hFontHeader);
            SetTextColor(hm, CP_TEXT_PRIMARY);
            RECT rcTitle = { iconX + 18 + 8, rcHeaderCard.top + 2,
                             s_rcCloseBtn.left - 10, rcHeaderCard.top + 22 };
            DrawTextW(hm, L"Bikode AI", -1, &rcTitle,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            BuildMissionModelLabel(wszModel, ARRAYSIZE(wszModel));
            BuildMissionStatusLabel(wszStatus, ARRAYSIZE(wszStatus), &statusAccent, &progressPct);
            {
                const WCHAR* root = FileManager_GetRootPath();
                const WCHAR* leaf = NULL;
                if (root && root[0]) {
                    leaf = wcsrchr(root, L'\\');
                    leaf = (leaf && leaf[1]) ? leaf + 1 : root;
                    _snwprintf_s(wszContext, ARRAYSIZE(wszContext), _TRUNCATE, L"CTX %s", leaf);
                }
            }

            modelW = min(126, max(84, MeasureThemeTextWidth(hm, BikodeTheme_GetFont(BKFONT_MONO_SMALL), wszModel) + 26));
            contextW = min(116, max(84, MeasureThemeTextWidth(hm, BikodeTheme_GetFont(BKFONT_MONO_SMALL), wszContext) + 26));
            statusW = min(112, max(84, MeasureThemeTextWidth(hm, BikodeTheme_GetFont(BKFONT_UI_SMALL), wszStatus) + 26));
            availableChipsW = (s_rcCloseBtn.left - 8) - rcTitle.left;
            totalChipW = modelW + contextW + statusW + 12;
            if (totalChipW > availableChipsW)
            {
                int excess = totalChipW - availableChipsW;
                int shrink = min(excess, modelW - 72);
                modelW -= shrink;
                excess -= shrink;
                shrink = min(excess, contextW - 76);
                contextW -= shrink;
                excess -= shrink;
                shrink = min(excess, statusW - 76);
                statusW -= shrink;
            }

            rcModel.left = rcTitle.left;
            rcModel.top = rcTitle.bottom + 3;
            rcModel.right = rcModel.left + modelW;
            rcModel.bottom = rcModel.top + 20;
            rcContext = rcModel;
            rcContext.left = rcModel.right + 6;
            rcContext.right = rcContext.left + contextW;
            rcStatus = rcContext;
            rcStatus.left = rcContext.right + 6;
            rcStatus.right = min(s_rcCloseBtn.left - 8, rcStatus.left + statusW);

            BikodeTheme_DrawChip(hm, &rcModel, wszModel,
                BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
                BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
                BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
            BikodeTheme_DrawChip(hm, &rcContext, wszContext,
                BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
                BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
                BikodeTheme_GetColor(BKCLR_HOT_MAGENTA));
            BikodeTheme_DrawChip(hm, &rcStatus, wszStatus,
                BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
                BikodeTheme_GetFont(BKFONT_UI_SMALL), TRUE,
                statusAccent);

            rcProgress.left = rcTitle.left;
            rcProgress.right = s_rcCloseBtn.left - 8;
            rcProgress.top = rcStatus.bottom + 5;
            rcProgress.bottom = rcProgress.top + 4;
            {
                RECT rcFill = rcProgress;
                rcFill.right = rcProgress.left + ((rcProgress.right - rcProgress.left) * progressPct) / 100;
                HBRUSH hBase = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_STROKE_SOFT));
                HBRUSH hFill = CreateSolidBrush(statusAccent);
                FillRect(hm, &rcProgress, hBase);
                if (rcFill.right > rcFill.left)
                    FillRect(hm, &rcFill, hFill);
                DeleteObject(hBase);
                DeleteObject(hFill);
            }

            if (s_bCloseHover)
                FillRoundRectSolid(hm, &s_rcCloseBtn, 5, CP_CLOSE_HOV);
            DrawCloseX(hm, &s_rcCloseBtn, CP_TEXT_SECONDARY);
        }

        // Input card
        {
            COLORREF cardBd = s_bInputFocused ? CP_INPUT_FOCUS_BD : CP_INPUT_BD;
            RECT rcRail = s_rcInputCard;
            RECT rcMeta;
            BikodeTheme_DrawCutCornerPanel(hm, &s_rcInputCard,
                BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
                BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                cardBd,
                10, TRUE);

            rcRail.left += 10;
            rcRail.top += 10;
            rcRail.right = rcRail.left + 3;
            rcRail.bottom = min(s_rcInputWell.bottom, s_rcInputCard.bottom - 10);
            {
                HBRUSH hRail = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
                FillRect(hm, &rcRail, hRail);
                DeleteObject(hRail);
            }

            if (!IsRectEmpty(&s_rcComposerTag))
            {
                BikodeTheme_DrawChip(hm, &s_rcComposerTag, L"PROMPT DECK",
                    BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
                    BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                    BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
                    BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
                    BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
            }

            rcMeta = s_rcComposerHint;
            SetBkMode(hm, TRANSPARENT);
            SetTextColor(hm, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
            SelectObject(hm, BikodeTheme_GetFont(BKFONT_UI_SMALL));
            DrawTextW(hm, L"Shift+Enter newline", -1, &rcMeta,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            if (!IsRectEmpty(&s_rcInputWell))
            {
                BikodeTheme_DrawRoundedPanel(hm, &s_rcInputWell,
                    CP_INPUT_WELL_BG,
                    BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                    s_bInputFocused ? CP_INPUT_FOCUS_BD : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                    8, FALSE);
            }

            if (!IsRectEmpty(&s_rcComposerDock))
            {
                BikodeTheme_DrawRoundedPanel(hm, &s_rcComposerDock,
                    CP_INPUT_DOCK_BG,
                    BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                    BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                    8, FALSE);
            }
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
        SetBkColor(hdcEdit, CP_INPUT_WELL_BG);
        SetBkMode(hdcEdit, OPAQUE);
        static HBRUSH s_hBrEdit = NULL;
        static COLORREF s_lastClr = 0;
        if (!s_hBrEdit || s_lastClr != CP_INPUT_WELL_BG) {
            if (s_hBrEdit) DeleteObject(s_hBrEdit);
            s_hBrEdit = CreateSolidBrush(CP_INPUT_WELL_BG);
            s_lastClr = CP_INPUT_WELL_BG;
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
                BOOL down = ((pDIS->itemState & ODS_SELECTED) != 0) || s_bSendDown;
                DrawButtonFace(pDIS->hDC, &pDIS->rcItem, s_bSendHover, down, TRUE);
                int bcx = (pDIS->rcItem.left + pDIS->rcItem.right) / 2;
                int bcy = (pDIS->rcItem.top + pDIS->rcItem.bottom) / 2;
                COLORREF icon = down ? RGB(236, 238, 245) : CP_BTN_ICON;
                DrawSendArrow(pDIS->hDC, bcx, bcy, icon);
                return TRUE;
            }
            if (pDIS->CtlID == IDC_CHAT_ATTACH)
            {
                BOOL down = ((pDIS->itemState & ODS_SELECTED) != 0) || s_bAttachDown;
                RECT rcIcon = pDIS->rcItem;
                DrawButtonFace(pDIS->hDC, &pDIS->rcItem, s_bAttachHover, down, FALSE);
                COLORREF icon = down ? RGB(228, 232, 240) : CP_BTN_ICON;
                InflateRect(&rcIcon, -8, -8);
                BikodeTheme_DrawGlyph(pDIS->hDC, BKGLYPH_NEW, &rcIcon, icon, 2);
                return TRUE;
            }
            if (pDIS->CtlID == IDC_CHAT_SEARCH)
            {
                BOOL down = ((pDIS->itemState & ODS_SELECTED) != 0) || s_bSearchDown;
                RECT rcIcon = pDIS->rcItem;
                DrawButtonFace(pDIS->hDC, &pDIS->rcItem, s_bSearchHover, down, FALSE);
                COLORREF icon = down ? RGB(228, 232, 240) : CP_BTN_ICON;
                InflateRect(&rcIcon, -8, -8);
                BikodeTheme_DrawGlyph(pDIS->hDC, BKGLYPH_SEARCH, &rcIcon, icon, 2);
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
            if (newWidth < 320) newWidth = 320;
            if (newWidth > rcParent.right - 240) newWidth = rcParent.right - 240;
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
    case WM_DROPFILES:
    {
        HDROP hDrop = (HDROP)wParam;
        AddAttachmentsFromDrop(hDrop);
        return 0;
    }

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
    case WM_LBUTTONDOWN:
        s_bSendDown = TRUE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_LBUTTONUP:
        s_bSendDown = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
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
    case WM_LBUTTONDOWN:
        s_bAttachDown = TRUE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_LBUTTONUP:
        s_bAttachDown = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
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
    case WM_LBUTTONDOWN:
        s_bSearchDown = TRUE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_LBUTTONUP:
        s_bSearchDown = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    return CallWindowProc(s_pfnOrigSearchProc, hwnd, msg, wParam, lParam);
}

static void ChatPanel_InvokeAttachmentPicker(void)
{
    WCHAR szFiles[8192];
    ZeroMemory(szFiles, sizeof(szFiles));
    OPENFILENAMEW ofn;

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = s_hwndPanel;
    ofn.lpstrFile   = szFiles;
    ofn.nMaxFile    = (DWORD)(sizeof(szFiles) / sizeof(szFiles[0]));
    ofn.lpstrFilter = L"All Files\0*.*\0Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn) == TRUE)
    {
        WCHAR* p = szFiles;
        WCHAR* first = p;
        size_t firstLen = wcslen(first);
        p += firstLen + 1;

        if (*p == L'\0') {
            PendingAttachments_AddFromPath(first);
            return;
        }

        WCHAR fullPath[MAX_PATH];
        while (*p != L'\0') {
            if (FAILED(StringCchCopyW(fullPath, MAX_PATH, first))) {
                break;
            }
            if (FAILED(StringCchCatW(fullPath, MAX_PATH, L"\\"))) {
                break;
            }
            if (FAILED(StringCchCatW(fullPath, MAX_PATH, p))) {
                break;
            }
            PendingAttachments_AddFromPath(fullPath);
            p += wcslen(p) + 1;
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

    BOOL bAdded = FALSE;
    HBITMAP hBmp = (HBITMAP)GetClipboardData(CF_BITMAP);
    if (hBmp)
    {
        WCHAR szPath[MAX_PATH];
        if (SaveBitmapToTempPng(hBmp, szPath, MAX_PATH))
        {
            bAdded = PendingAttachments_AddImage(szPath, L"Pasted Image");
        }
    }
    CloseClipboard();
    return bAdded;
}

static BOOL HandleClipboardPaste(void)
{
    // Prefer file-drop payload when copying files from Explorer.
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

    // Bitmap / screenshot
    if (IsClipboardFormatAvailable(CF_BITMAP) || IsClipboardFormatAvailable(CF_DIB))
    {
        return PendingAttachments_AddClipboardImage();
    }
    return FALSE; // Let default handler handle text
}

static BOOL AddAttachmentsFromDrop(HDROP hDrop)
{
    int count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
    BOOL bAnyAdded = FALSE;
    for (int i = 0; i < count; i++)
    {
        WCHAR szFile[MAX_PATH];
        DragQueryFileW(hDrop, i, szFile, MAX_PATH);
        bAnyAdded |= PendingAttachments_AddFromPath(szFile);
    }
    DragFinish(hDrop);
    return bAnyAdded;
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
