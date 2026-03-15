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
#include "AISubscriptionAgent.h"
#include "AIProvider.h"
#include "AgentRuntime.h"
#include "Externals.h"
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
#include <process.h>
#include "resource.h"


#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "urlmon.lib")

//=============================================================================
// Color palette — matched to BikodeWebsite neutral gray + blue accent
//=============================================================================

// Background
#define CP_BG              (BikodeTheme_GetColor(BKCLR_APP_BG))
#define CP_HEADER          (BikodeTheme_GetColor(BKCLR_SURFACE_MAIN))

// User bubble (right side)
#define CP_USER_BG         (BikodeTheme_GetColor(BKCLR_SURFACE_RAISED))
#define CP_USER_TEXT       (BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY))

// AI bubble (left side)
#define CP_AI_BG           (BikodeTheme_GetColor(BKCLR_SURFACE_MAIN))
#define CP_AI_TEXT         (BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY))
#define CP_AI_STRIP        (BikodeTheme_GetColor(BKCLR_SURFACE_RAISED))

// Attachment chips
#define CP_ATTACHMENT_BG      (BikodeTheme_GetColor(BKCLR_SURFACE_RAISED))
#define CP_ATTACHMENT_BORDER  (BikodeTheme_GetColor(BKCLR_STROKE_SOFT))
#define CP_ATTACHMENT_TEXT    (BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY))

// System messages
#define CP_SYS_TEXT        (BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY))

// Input area
#define CP_INPUT_BG        (BikodeTheme_GetColor(BKCLR_SURFACE_MAIN))
#define CP_INPUT_BD        (BikodeTheme_GetColor(BKCLR_STROKE_SOFT))
#define CP_INPUT_FOCUS_BD  (BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN))
#define CP_INPUT_WELL_BG   (BikodeTheme_GetColor(BKCLR_APP_BG))
#define CP_INPUT_DOCK_BG   (BikodeTheme_GetColor(BKCLR_APP_BG))
#define CP_INPUT_TEXT      (BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY))

// Accents — blue accent from website
#define CP_ACCENT          (BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN))
#define CP_TEXT_PRIMARY    (BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY))
#define CP_TEXT_SECONDARY  (BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY))
#define CP_CLOSE_HOV       (BikodeTheme_GetColor(BKCLR_SURFACE_RAISED))
#define CP_SEND_HOV        (BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 112))
#define CP_SEND_BG         (BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN))
#define CP_SCROLLBAR_BG    (BikodeTheme_GetColor(BKCLR_APP_BG))
#define CP_SCROLLBAR_TH    (BikodeTheme_GetColor(BKCLR_TEXT_MUTED))
#define CP_SCROLLBAR_HOV   (BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN))
#define CP_BTN_BG             (BikodeTheme_GetColor(BKCLR_SURFACE_MAIN))
#define CP_BTN_HOV            (BikodeTheme_GetColor(BKCLR_SURFACE_RAISED))
#define CP_BTN_DOWN           (BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_STROKE_DARK), 96))
#define CP_BTN_BORDER         (BikodeTheme_GetColor(BKCLR_STROKE_SOFT))
#define CP_BTN_BORDER_HOV     (BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN))
#define CP_BTN_ICON           (BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY))

#define CP_BTN_PRIMARY_BG     (BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN))
#define CP_BTN_PRIMARY_HOV    (BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), 42))
#define CP_BTN_PRIMARY_DOWN   (BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), BikodeTheme_GetColor(BKCLR_STROKE_DARK), 176))
#define CP_BTN_PRIMARY_BORDER (BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN))

// Additional tokens for status card
#define CP_BORDER          (BikodeTheme_GetColor(BKCLR_STROKE_SOFT))
#define CP_MUTED           (BikodeTheme_GetColor(BKCLR_TEXT_MUTED))
#define CP_DIVIDER         (BikodeTheme_GetColor(BKCLR_STROKE_SOFT))

//=============================================================================
// Layout constants
//=============================================================================

#define CHAT_PANEL_WIDTH        360
#define CHAT_HEADER_HEIGHT      80
#define CHAT_INPUT_AREA_HEIGHT  132
#define CHAT_INPUT_PAD          10
#define CHAT_INPUT_INNER_PAD    10
#define CHAT_INPUT_RADIUS       8
#define CHAT_COMPOSER_FOOTER_H  38
#define CHAT_COMPOSER_FOOTER_GAP 8
#define CHAT_ACTION_BTN_SIZE    32
#define CHAT_ACTION_SEND_W      94
#define CHAT_SEND_SIZE          32
#define CHAT_BTN_GAP             6
#define CHAT_HDR_BTN_SIZE       24
#define CHAT_HEADER_MODE_COUNT   5
#define CHAT_HEADER_MODE_H      24
#define CHAT_HEADER_MODE_GAP     2
#define CHAT_HEADER_ACTION_W    72
#define CHAT_HEADER_ACTION_H    24
#define CHAT_SPLITTER_W          5
#define CHAT_STATUS_DOT_R       4
#define CHAT_EMPTY_CARD_INSET   16
#define CHAT_EMPTY_CHIP_H       24
#define CHAT_EMPTY_CHIP_GAP      8
#define CHAT_EMPTY_TITLE        L"I write what I like."
#define CHAT_EMPTY_BODY         L"Bikode AI keeps the website's voice inside the editor: trace a crash, explain a symbol, search the repo, or draft a patch from the current file while protecting your voice and intent."
#define CHAT_EMPTY_HINT         L"Paste screenshots, drop files, or use search to pull repo context into the thread."
#define CHAT_COMPOSER_TAG_H     22
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
#define CHAT_GIF_TIMER_ID          0x71A1
#define CHAT_GIF_MIN_DELAY_MS         16
#define CHAT_GIF_FALLBACK_DELAY_CS     8
#define CHAT_GIF_MAX_CATCHUP_FRAMES   12

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
static BOOL     s_bPromptDeckHover = FALSE;
static BOOL     s_bSendDown = FALSE;
static BOOL     s_bAttachDown = FALSE;
static BOOL     s_bSearchDown = FALSE;
static BOOL     s_bInputFocused = FALSE;

// Header chrome
static RECT     s_rcCloseBtn   = { 0 };
static RECT     s_rcHeaderModes[CHAT_HEADER_MODE_COUNT] = { { 0 } };
static RECT     s_rcHeaderModeDock = { 0 };
static RECT     s_rcHeaderAction = { 0 };
static BOOL     s_bCloseHover  = FALSE;
static int      s_iHeaderHotMode = -1;
static BOOL     s_bHeaderActionHover = FALSE;

// Fonts
static HFONT    s_hFontHeader  = NULL;
static HFONT    s_hFontInput   = NULL;
static HFONT    s_hFontStatus  = NULL;
static HFONT    s_hFontLabel   = NULL;
static HFONT    s_hFontBubble  = NULL;
static HFONT    s_hFontBrandWordmark = NULL;

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
static RECT     s_rcComposerActionDock = { 0 };
static RECT     s_rcPendingStrip = { 0 };
static int      s_lastLayoutParentRight = 0;
static int      s_lastLayoutEditorTop = 0;
static int      s_lastLayoutEditorHeight = 0;

#define IDM_CHAT_PROMPT_SUMMARY  0xFCE0
#define IDM_CHAT_PROMPT_BUG      0xFCE1
#define IDM_CHAT_PROMPT_REVIEW   0xFCE2
#define IDM_CHAT_PROMPT_PLAN     0xFCE3
#define IDM_CHAT_PROMPT_COMMANDS 0xFCE4

/* Agency persona prompts */
#define IDM_CHAT_PERSONA_ARCHITECT    0xFCA0
#define IDM_CHAT_PERSONA_CODE_REVIEW  0xFCA1
#define IDM_CHAT_PERSONA_SECURITY     0xFCA2
#define IDM_CHAT_PERSONA_FRONTEND     0xFCA3
#define IDM_CHAT_PERSONA_DEVOPS       0xFCA4
#define IDM_CHAT_PERSONA_DB_OPTIMIZER 0xFCA5
#define IDM_CHAT_PERSONA_BACKEND      0xFCA6
#define IDM_CHAT_PERSONA_RAPID_PROTO  0xFCA7
/* Promptfoo persona prompts */
#define IDM_CHAT_PERSONA_EVALUATOR    0xFCA8
#define IDM_CHAT_PERSONA_RED_TEAM     0xFCA9
#define IDM_CHAT_PERSONA_TECH_WRITER  0xFCAA
#define IDM_CHAT_PERSONA_SRE          0xFCAB
/* Impeccable persona prompts */
#define IDM_CHAT_PERSONA_DESIGN_AUDIT 0xFCAC
#define IDM_CHAT_PERSONA_DESIGN_CRIT  0xFCAD
#define IDM_CHAT_PERSONA_TYPOGRAPHY   0xFCAE
#define IDM_CHAT_PERSONA_COLOR        0xFCAF
/* OpenViking persona prompts */
#define IDM_CHAT_PERSONA_CONTEXT_ARCH 0xFCB0
#define IDM_CHAT_PERSONA_MEMORY       0xFCB1
#define IDM_CHAT_PERSONA_UX_ARCH      0xFCB2
#define IDM_CHAT_PERSONA_UI_DESIGNER  0xFCB3
#define IDM_CHAT_PERSONA_UX_RESEARCH  0xFCB4
#define IDM_CHAT_PERSONA_PERF_BENCH   0xFCB5
#define IDM_CHAT_PERSONA_ACCESSIBILITY 0xFCB6
#define IDM_CHAT_PERSONA_API_TESTER   0xFCB7
#define IDM_CHAT_PERSONA_REALITY      0xFCB8
#define IDM_CHAT_PERSONA_SPRINT       0xFCB9
#define IDM_CHAT_PERSONA_FEEDBACK     0xFCBA
#define IDM_CHAT_PERSONA_TREND        0xFCBB
#define IDM_CHAT_PERSONA_ORCHESTRATOR 0xFCBC
#define IDM_CHAT_PERSONA_GIT_WORKFLOW 0xFCBD
#define IDM_CHAT_PERSONA_INCIDENT     0xFCBE
/* Clear persona */
#define IDM_CHAT_PERSONA_CLEAR        0xFCBF
#define IDM_CHAT_PERSONA_MODEL_COMPARE 0xFCC0
#define IDM_CHAT_PERSONA_REGRESSION   0xFCC1
#define IDM_CHAT_PERSONA_DESIGN_POLISH 0xFCC2
#define IDM_CHAT_PERSONA_MOTION       0xFCC3
#define IDM_CHAT_PERSONA_RETRIEVAL    0xFCC4

// Active persona (injected into system prompt)
static char  s_szActivePersona[512] = { 0 };
static WCHAR s_wszActivePersonaLabel[64] = { 0 };

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
// AI Working Status Card
//=============================================================================

static BOOL   s_bAIWorking         = FALSE;
static char   s_szStatusText[256]  = "";
static char   s_szToolText[512]    = "";
static WCHAR  s_wszStatusText[256] = L"";
static WCHAR  s_wszToolText[512]   = L"";
static int    s_statusDotPhase     = 0;     // 0, 1, 2 for pulsing animation
static DWORD  s_statusDotTick      = 0;

#define STATUS_DOT_TIMER_ID    0x71A2
#define STATUS_DOT_TIMER_MS    400
#define STATUS_CARD_PAD_H      16
#define STATUS_CARD_PAD_V      12
#define STATUS_CARD_RADIUS     10
#define STATUS_CARD_MARGIN     10
#define STATUS_DOT_R           4
#define STATUS_TEXT_GAP        8
#define STATUS_TOOL_GAP        4
#define STATUS_TOOL_PAD_H      10
#define STATUS_TOOL_PAD_V       7
#define STATUS_TOOL_RADIUS      8

//=============================================================================
// Activity GIF Pool
//=============================================================================

typedef enum {
    ACTIVITY_THINKING = 0,
    ACTIVITY_READING,
    ACTIVITY_WRITING,
    ACTIVITY_RUNNING,
    ACTIVITY_COUNT
} ActivityCategory;

static const char* s_activityGifUrls[ACTIVITY_COUNT][3] = {
    // ACTIVITY_THINKING
    {
        "https://media.tenor.com/1UmAI4RVPqYAAAAM/thinking-emoji.gif",
        "https://media.tenor.com/DHIiPSvCLjMAAAAM/thinking.gif",
        "https://media.tenor.com/Lg5g-PIALQUAAAAM/hmm-thinking.gif",
    },
    // ACTIVITY_READING
    {
        "https://media.tenor.com/UnFbz_wCOEAAAAAM/cat-typing.gif",
        "https://media.tenor.com/FawYo00tBekAAAAM/reading-read.gif",
        "https://media.tenor.com/xBzl1i2YOQQAAAAM/searching.gif",
    },
    // ACTIVITY_WRITING
    {
        "https://media.tenor.com/UnFbz_wCOEAAAAAM/cat-typing.gif",
        "https://media.tenor.com/y2JXkY1pXkwAAAAM/cat-computer.gif",
        "https://media.tenor.com/FHMi0_bJ110AAAAM/typing-fast.gif",
    },
    // ACTIVITY_RUNNING
    {
        "https://media.tenor.com/2rckjS5gbfUAAAAM/hacker-hackerman.gif",
        "https://media.tenor.com/bMH3-4F_zwkAAAAM/coding.gif",
        "https://media.tenor.com/GfSX-u7VGM4AAAAM/coding.gif",
    },
};

// Status card GIF state (separate from message GIFs)
static Gdiplus::Image* s_pStatusGif         = NULL;
static GUID            s_statusGifFrameGuid;
static UINT            s_statusGifFrameCount = 0;
static UINT            s_statusGifFrameIndex = 0;
static UINT*           s_statusGifDelaysCs   = NULL;
static DWORD           s_statusGifLastTick   = 0;
static int             s_statusGifDrawW      = 0;
static int             s_statusGifDrawH      = 0;
static ActivityCategory s_currentActivity    = ACTIVITY_THINKING;
static WCHAR           s_wszStatusGifPath[MAX_PATH] = L"";
static LONG            s_statusGifDownloading = 0;

#define STATUS_GIF_MAX_W   180
#define STATUS_GIF_MAX_H   100
#define STATUS_GIF_GAP      8

#define WM_STATUS_GIF_READY  (WM_USER + 0x710)

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
// Drawing helpers (defined later, forward-declared for status card)
static void FillRoundRect(HDC hdc, const RECT* prc, int r, COLORREF fill, COLORREF border);
static void FillRoundRectSolid(HDC hdc, const RECT* prc, int r, COLORREF fill);
static void DrawStatusDot(HDC hdc, int cx, int cy, int r, COLORREF clr);
// Status card
static void StatusCard_Activate(const char* initialStatus);
static void StatusCard_UpdateStatus(const char* statusText);
static void StatusCard_UpdateTool(const char* toolText);
static void StatusCard_Deactivate(void);
static int  MeasureStatusCardHeight(HDC hdc, int chatW);
static void PaintStatusCard(HDC hdc, int cx, int y);
static UINT GetGifFrameDelayMs(const UINT* delaysCs, UINT frameIndex);
static UINT GetGifRemainingDelayMs(DWORD lastTick, UINT delayMs, DWORD now);
static BOOL AdvanceAnimatedGifFrames(Gdiplus::Image* img, const GUID* frameGuid,
                                     UINT frameCount, UINT* frameIndex,
                                     const UINT* delaysCs, DWORD* lastTick, DWORD now);
static UINT GetNextGifWakeDelayMs(void);
static void UpdateGifTimerState(void);
static void AdvanceGifFrames(void);
// Activity GIF
static ActivityCategory ClassifyToolActivity(const char* toolText);
static void StatusGif_StartDownload(ActivityCategory category);
static BOOL LoadStatusGif(const WCHAR* wszPath);
static void FreeStatusGif(void);
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

static UINT GetGifFrameDelayMs(const UINT* delaysCs, UINT frameIndex)
{
    UINT delayCs = CHAT_GIF_FALLBACK_DELAY_CS;
    if (delaysCs)
        delayCs = delaysCs[frameIndex];
    if (delayCs == 0)
        delayCs = CHAT_GIF_FALLBACK_DELAY_CS;
    return max((UINT)CHAT_GIF_MIN_DELAY_MS, delayCs * 10U);
}

static UINT GetGifRemainingDelayMs(DWORD lastTick, UINT delayMs, DWORD now)
{
    DWORD elapsed = now - lastTick;
    if (elapsed >= delayMs)
        return 0;
    return (UINT)(delayMs - elapsed);
}

static BOOL AdvanceAnimatedGifFrames(Gdiplus::Image* img, const GUID* frameGuid,
                                     UINT frameCount, UINT* frameIndex,
                                     const UINT* delaysCs, DWORD* lastTick, DWORD now)
{
    BOOL changed = FALSE;
    UINT guard = 0;

    if (!img || !frameGuid || frameCount <= 1 || !frameIndex || !lastTick)
        return FALSE;

    while (guard < CHAT_GIF_MAX_CATCHUP_FRAMES)
    {
        UINT delayMs = GetGifFrameDelayMs(delaysCs, *frameIndex);
        DWORD elapsed = now - *lastTick;
        if (elapsed < delayMs)
            break;

        *lastTick += delayMs;
        *frameIndex = (*frameIndex + 1) % frameCount;
        img->SelectActiveFrame(frameGuid, *frameIndex);
        changed = TRUE;
        ++guard;
    }

    if (guard == CHAT_GIF_MAX_CATCHUP_FRAMES)
        *lastTick = now;

    return changed;
}

static UINT GetNextGifWakeDelayMs(void)
{
    DWORD now = GetTickCount();
    UINT nextDelay = 0xFFFFFFFFu;
    BOOL hasAnimatedGif = FALSE;

    for (int i = 0; i < s_nMsgs; i++)
    {
        ChatMsg* m = &s_msgs[i];
        if (!m->pInlineGif || m->gifFrameCount <= 1)
            continue;

        hasAnimatedGif = TRUE;
        {
            UINT delayMs = GetGifFrameDelayMs(m->gifDelaysCs, m->gifFrameIndex);
            UINT remaining = GetGifRemainingDelayMs(m->gifLastTick, delayMs, now);
            if (remaining < nextDelay)
                nextDelay = remaining;
        }
    }

    if (s_pStatusGif && s_statusGifFrameCount > 1)
    {
        UINT delayMs = GetGifFrameDelayMs(s_statusGifDelaysCs, s_statusGifFrameIndex);
        UINT remaining = GetGifRemainingDelayMs(s_statusGifLastTick, delayMs, now);
        hasAnimatedGif = TRUE;
        if (remaining < nextDelay)
            nextDelay = remaining;
    }

    if (!hasAnimatedGif)
        return 0;

    if (nextDelay == 0xFFFFFFFFu)
        nextDelay = CHAT_GIF_MIN_DELAY_MS;

    return max(1U, nextDelay);
}

static void UpdateGifTimerState(void)
{
    if (!s_hwndChat) return;
    UINT delayMs = GetNextGifWakeDelayMs();
    if (delayMs > 0)
        SetTimer(s_hwndChat, CHAT_GIF_TIMER_ID, delayMs, NULL);
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
        if (!m->pInlineGif || m->gifFrameCount <= 1)
            continue;

        changed |= AdvanceAnimatedGifFrames(m->pInlineGif, &m->gifFrameGuid,
            m->gifFrameCount, &m->gifFrameIndex, m->gifDelaysCs, &m->gifLastTick, now);
    }
    if (s_pStatusGif && s_statusGifFrameCount > 1)
        changed |= AdvanceAnimatedGifFrames(s_pStatusGif, &s_statusGifFrameGuid,
            s_statusGifFrameCount, &s_statusGifFrameIndex, s_statusGifDelaysCs, &s_statusGifLastTick, now);

    if (changed && s_hwndChat)
        InvalidateRect(s_hwndChat, NULL, FALSE);
    UpdateGifTimerState();
}
static BOOL TryAttachInlineGif(ChatMsg* msg, const char* sourceText);
static char* SanitizeAIResponseForDisplay(const char* text, BOOL hideGifUrls);

//=============================================================================
// Activity GIF: classify tool, load, free, download
//=============================================================================

static ActivityCategory ClassifyToolActivity(const char* toolText)
{
    if (!toolText || !toolText[0]) return ACTIVITY_THINKING;
    const char* p = toolText;
    if (*p == '[') p++;

    if (StrCmpNIA(p, "read_file", 9) == 0 ||
        StrCmpNIA(p, "list_dir", 8) == 0 ||
        StrCmpNIA(p, "get_active_document", 19) == 0)
        return ACTIVITY_READING;

    if (StrCmpNIA(p, "write_file", 10) == 0 ||
        StrCmpNIA(p, "replace_in_file", 15) == 0 ||
        StrCmpNIA(p, "replace_editor", 14) == 0 ||
        StrCmpNIA(p, "new_file_in_editor", 18) == 0 ||
        StrCmpNIA(p, "insert_in_editor", 16) == 0)
        return ACTIVITY_WRITING;

    if (StrCmpNIA(p, "run_command", 11) == 0 ||
        StrCmpNIA(p, "make_dir", 8) == 0 ||
        StrCmpNIA(p, "init_repo", 9) == 0)
        return ACTIVITY_RUNNING;

    return ACTIVITY_THINKING;
}

static void FreeStatusGif(void)
{
    if (s_pStatusGif) { delete s_pStatusGif; s_pStatusGif = NULL; }
    if (s_statusGifDelaysCs) { n2e_Free(s_statusGifDelaysCs); s_statusGifDelaysCs = NULL; }
    s_statusGifFrameCount = 0;
    s_statusGifFrameIndex = 0;
    s_statusGifDrawW = 0;
    s_statusGifDrawH = 0;
    s_wszStatusGifPath[0] = L'\0';
}

static BOOL LoadStatusGif(const WCHAR* wszPath)
{
    if (!wszPath || !wszPath[0] || !EnsureGdiplus())
        return FALSE;

    FreeStatusGif();

    Gdiplus::Image* img = Gdiplus::Image::FromFile(wszPath, FALSE);
    if (!img || img->GetLastStatus() != Gdiplus::Ok)
    {
        if (img) delete img;
        return FALSE;
    }

    UINT imgW = img->GetWidth();
    UINT imgH = img->GetHeight();
    if (imgW == 0 || imgH == 0) { delete img; return FALSE; }

    s_pStatusGif = img;
    s_statusGifDrawW = STATUS_GIF_MAX_W;
    s_statusGifDrawH = (int)((double)imgH * s_statusGifDrawW / (double)imgW);
    if (s_statusGifDrawH > STATUS_GIF_MAX_H) {
        s_statusGifDrawH = STATUS_GIF_MAX_H;
        s_statusGifDrawW = (int)((double)imgW * s_statusGifDrawH / (double)imgH);
    }
    if (s_statusGifDrawW < 60) s_statusGifDrawW = 60;
    if (s_statusGifDrawH < 36) s_statusGifDrawH = 36;

    UINT dimCount = img->GetFrameDimensionsCount();
    s_statusGifFrameCount = 1;
    s_statusGifFrameIndex = 0;
    if (dimCount > 0) {
        GUID guid;
        if (img->GetFrameDimensionsList(&guid, 1) == Gdiplus::Ok) {
            s_statusGifFrameGuid = guid;
            s_statusGifFrameCount = img->GetFrameCount(&guid);
            if (s_statusGifFrameCount > 1) {
                UINT sz = img->GetPropertyItemSize(PropertyTagFrameDelay);
                if (sz > 0) {
                    Gdiplus::PropertyItem* pItem = (Gdiplus::PropertyItem*)n2e_Alloc(sz);
                    if (pItem && img->GetPropertyItem(PropertyTagFrameDelay, sz, pItem) == Gdiplus::Ok) {
                        s_statusGifDelaysCs = (UINT*)n2e_Alloc(s_statusGifFrameCount * sizeof(UINT));
                        if (s_statusGifDelaysCs) {
                            for (UINT i = 0; i < s_statusGifFrameCount; i++) {
                                UINT delay = ((UINT*)pItem->value)[i];
                                if (delay == 0) delay = 8;
                                s_statusGifDelaysCs[i] = delay;
                            }
                        }
                    }
                    if (pItem) n2e_Free(pItem);
                }
            }
        }
    }
    s_statusGifLastTick = GetTickCount();
    StringCchCopyW(s_wszStatusGifPath, MAX_PATH, wszPath);
    return TRUE;
}

typedef struct {
    char     url[1024];
    WCHAR    destPath[MAX_PATH];
    HWND     hwndChat;
} StatusGifDownloadCtx;

static unsigned __stdcall StatusGifDownloadThread(void* pArg)
{
    StatusGifDownloadCtx* ctx = (StatusGifDownloadCtx*)pArg;
    WCHAR wszUrl[1024];
    MultiByteToWideChar(CP_UTF8, 0, ctx->url, -1, wszUrl, ARRAYSIZE(wszUrl));

    HRESULT hr = URLDownloadToFileW(NULL, wszUrl, ctx->destPath, 0, NULL);
    if (SUCCEEDED(hr)) {
        WCHAR* pathCopy = (WCHAR*)malloc((wcslen(ctx->destPath) + 1) * sizeof(WCHAR));
        if (pathCopy) {
            wcscpy(pathCopy, ctx->destPath);
            PostMessage(ctx->hwndChat, WM_STATUS_GIF_READY, 0, (LPARAM)pathCopy);
        }
    }
    InterlockedExchange(&s_statusGifDownloading, 0);
    free(ctx);
    return 0;
}

static void StatusGif_StartDownload(ActivityCategory category)
{
    if (InterlockedCompareExchange(&s_statusGifDownloading, 1, 0) != 0)
        return;

    if (!EnsureAttachmentTempDir()) {
        InterlockedExchange(&s_statusGifDownloading, 0);
        return;
    }

    int idx = GetTickCount() % 3;
    const char* url = s_activityGifUrls[category][idx];

    StatusGifDownloadCtx* ctx = (StatusGifDownloadCtx*)malloc(sizeof(*ctx));
    if (!ctx) { InterlockedExchange(&s_statusGifDownloading, 0); return; }

    StringCchCopyA(ctx->url, ARRAYSIZE(ctx->url), url);
    WCHAR tmpName[MAX_PATH];
    GetTempFileNameW(s_wszAttachmentDir, L"aig", 0, tmpName);
    DeleteFileW(tmpName);
    StringCchPrintfW(ctx->destPath, MAX_PATH, L"%s.gif", tmpName);
    ctx->hwndChat = s_hwndChat;

    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, StatusGifDownloadThread, ctx, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
    else {
        free(ctx);
        InterlockedExchange(&s_statusGifDownloading, 0);
    }
}

//=============================================================================
// Status Card: activate / update / deactivate
//=============================================================================

static BOOL IsStatusWideSpace(WCHAR ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

static void TrimWideInPlace(WCHAR* text)
{
    WCHAR* start;
    size_t len;

    if (!text) return;

    start = text;
    while (*start && IsStatusWideSpace(*start))
        start++;
    if (start != text)
        MoveMemory(text, start, (wcslen(start) + 1) * sizeof(WCHAR));

    len = wcslen(text);
    while (len > 0 && IsStatusWideSpace(text[len - 1]))
        text[--len] = L'\0';
}

static void StripLeadingStatusLeadIn(WCHAR* text)
{
    WCHAR* p;

    if (!text || !text[0]) return;

    p = text;
    for (;;)
    {
        while (*p && IsStatusWideSpace(*p))
            p++;

        if (*p == (WCHAR)0x2022 || *p == L'-' || *p == L'+' || *p == L'>')
        {
            p++;
            continue;
        }

        if (*p >= L'0' && *p <= L'9')
        {
            WCHAR* q = p;
            while (*q >= L'0' && *q <= L'9')
                q++;
            if ((*q == L'.' || *q == L')' || *q == L':') && IsStatusWideSpace(q[1]))
            {
                p = q + 1;
                continue;
            }
        }
        break;
    }

    while (*p && IsStatusWideSpace(*p))
        p++;
    if (p != text)
        MoveMemory(text, p, (wcslen(p) + 1) * sizeof(WCHAR));
}

static BOOL StripWrappedTokenInPlace(WCHAR* text, WCHAR openCh, WCHAR closeCh)
{
    size_t len;

    if (!text) return FALSE;

    len = wcslen(text);
    if (len >= 2 && text[0] == openCh && text[len - 1] == closeCh)
    {
        MoveMemory(text, text + 1, (len - 1) * sizeof(WCHAR));
        text[len - 2] = L'\0';
        return TRUE;
    }
    return FALSE;
}

static void RemoveStatusMarkdownGlyphsInPlace(WCHAR* text)
{
    WCHAR* src;
    WCHAR* dst;

    if (!text) return;

    src = text;
    dst = text;
    while (*src)
    {
        if (*src != L'*' && *src != (WCHAR)0x0060)
            *dst++ = *src;
        src++;
    }
    *dst = L'\0';
}

static void CollapseStatusWhitespaceInPlace(WCHAR* text)
{
    WCHAR* src;
    WCHAR* dst;
    BOOL needSpace = FALSE;

    if (!text) return;

    src = text;
    dst = text;
    while (*src)
    {
        if (IsStatusWideSpace(*src))
        {
            if (dst != text)
                needSpace = TRUE;
        }
        else
        {
            if (needSpace && dst != text)
                *dst++ = L' ';
            *dst++ = *src;
            needSpace = FALSE;
        }
        src++;
    }
    *dst = L'\0';
}

static void NormalizeStatusCardText(const char* text, WCHAR* out, int cchOut, BOOL toolLine)
{
    if (!out || cchOut <= 0)
        return;

    out[0] = L'\0';
    if (!text || !text[0])
        return;

    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, cchOut))
        MultiByteToWideChar(CP_ACP, 0, text, -1, out, cchOut);

    TrimWideInPlace(out);
    StripLeadingStatusLeadIn(out);
    TrimWideInPlace(out);

    for (;;)
    {
        BOOL changed = FALSE;
        changed |= StripWrappedTokenInPlace(out, L'*', L'*');
        changed |= StripWrappedTokenInPlace(out, (WCHAR)0x0060, (WCHAR)0x0060);
        changed |= StripWrappedTokenInPlace(out, L'"', L'"');
        changed |= StripWrappedTokenInPlace(out, L'\'', L'\'');
        if (!changed)
            break;
        TrimWideInPlace(out);
    }

    RemoveStatusMarkdownGlyphsInPlace(out);
    CollapseStatusWhitespaceInPlace(out);
    TrimWideInPlace(out);

    if (!toolLine && !out[0])
        StringCchCopyW(out, cchOut, L"Working");
}

static BOOL IsHiddenStatusCardText(const char* text)
{
    if (!text)
        return FALSE;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    return _strnicmp(text, "Task graph:", 11) == 0 ||
        _strnicmp(text, "Context ledger:", 15) == 0 ||
        _strnicmp(text, "Intent=", 7) == 0;
}

static void StatusCard_Activate(const char* initialStatus)
{
    s_bAIWorking = TRUE;
    StringCchCopyA(s_szStatusText, ARRAYSIZE(s_szStatusText),
                   initialStatus ? initialStatus : "Thinking...");
    s_szToolText[0] = '\0';
    NormalizeStatusCardText(s_szStatusText, s_wszStatusText,
                            ARRAYSIZE(s_wszStatusText), FALSE);
    s_wszToolText[0] = L'\0';
    s_statusDotPhase = 0;
    s_statusDotTick = GetTickCount();
    s_currentActivity = ACTIVITY_THINKING;

    if (s_hwndChat)
        SetTimer(s_hwndChat, STATUS_DOT_TIMER_ID, STATUS_DOT_TIMER_MS, NULL);

    StatusGif_StartDownload(ACTIVITY_THINKING);

    InvalidateAllHeights();
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
static void StatusCard_UpdateStatus(const char* statusText)
{
    if (!s_bAIWorking) return;
    StringCchCopyA(s_szStatusText, ARRAYSIZE(s_szStatusText),
                   statusText ? statusText : "");
    NormalizeStatusCardText(s_szStatusText, s_wszStatusText,
                            ARRAYSIZE(s_wszStatusText), FALSE);
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
static void StatusCard_UpdateTool(const char* toolText)
{
    if (!s_bAIWorking) return;

    // Parse "[toolName: detail]" -> "toolName: detail"
    const char* src = toolText;
    if (src && src[0] == '[') src++;
    char clean[512];
    StringCchCopyA(clean, ARRAYSIZE(clean), src ? src : "");
    int len = (int)strlen(clean);
    if (len > 0 && clean[len - 1] == ']') clean[len - 1] = '\0';

    StringCchCopyA(s_szToolText, ARRAYSIZE(s_szToolText), clean);
    NormalizeStatusCardText(s_szToolText, s_wszToolText,
                            ARRAYSIZE(s_wszToolText), TRUE);

    ActivityCategory newCat = ClassifyToolActivity(toolText);
    if (newCat != s_currentActivity) {
        s_currentActivity = newCat;
        FreeStatusGif();
        StatusGif_StartDownload(newCat);
    }

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
static void StatusCard_Deactivate(void)
{
    s_bAIWorking = FALSE;
    s_szStatusText[0] = '\0';
    s_szToolText[0] = '\0';
    s_wszStatusText[0] = L'\0';
    s_wszToolText[0] = L'\0';
    if (s_hwndChat)
        KillTimer(s_hwndChat, STATUS_DOT_TIMER_ID);
    FreeStatusGif();
    UpdateGifTimerState();
    if (s_hwndChat) InvalidateRect(s_hwndChat, NULL, FALSE);
}

//=============================================================================
// Status Card: measure and paint
//=============================================================================

static int MeasureStatusCardHeight(HDC hdc, int chatW)
{
    if (!s_bAIWorking) return 0;

    int cardInnerW = chatW - 2 * STATUS_CARD_MARGIN - 2 * STATUS_CARD_PAD_H;
    int textXOffset = STATUS_DOT_R * 2 + STATUS_TEXT_GAP + 8;
    int textW = cardInnerW - textXOffset;
    int totalH = STATUS_CARD_PAD_V;
    HFONT hOld;

    if (textW < 72) textW = 72;

    if (s_pStatusGif) {
        totalH += s_statusGifDrawH + CHAT_INLINE_GIF_FRAME_PAD * 2 + STATUS_GIF_GAP;
    }

    hOld = (HFONT)SelectObject(hdc, s_hFontLabel ? s_hFontLabel : s_hFontBubble);
    if (s_wszStatusText[0]) {
        RECT rc = { 0, 0, textW, 0 };
        DrawTextW(hdc, s_wszStatusText, -1, &rc,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        totalH += max(18, rc.bottom);
    } else {
        totalH += 18;
    }
    SelectObject(hdc, hOld);

    if (s_wszToolText[0]) {
        hOld = (HFONT)SelectObject(hdc, s_hFontStatus);
        RECT rcTool = { 0, 0, max(48, textW - 2 * STATUS_TOOL_PAD_H), 0 };
        DrawTextW(hdc, s_wszToolText, -1, &rcTool,
                  DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        totalH += STATUS_TOOL_GAP;
        totalH += rcTool.bottom + STATUS_TOOL_PAD_V * 2;
        SelectObject(hdc, hOld);
    }

    totalH += STATUS_CARD_PAD_V;
    return totalH;
}
static void PaintStatusCard(HDC hdc, int cx, int y)
{
    if (!s_bAIWorking) return;

    int cardX = STATUS_CARD_MARGIN;
    int cardW = cx - 2 * STATUS_CARD_MARGIN;
    int cardH = MeasureStatusCardHeight(hdc, cx);
    int contentX;
    int contentY;
    int contentW;
    int dotCenterX;
    int dotCenterY;
    int textX;
    int textW;
    HFONT hOld;

    RECT rcCard = { cardX, y, cardX + cardW, y + cardH };
    FillRoundRect(hdc, &rcCard, STATUS_CARD_RADIUS, CP_AI_BG, CP_BORDER);

    contentX = cardX + STATUS_CARD_PAD_H;
    contentY = y + STATUS_CARD_PAD_V;
    contentW = cardW - 2 * STATUS_CARD_PAD_H;

    if (s_pStatusGif) {
        int frameW = s_statusGifDrawW + CHAT_INLINE_GIF_FRAME_PAD * 2;
        int frameH = s_statusGifDrawH + CHAT_INLINE_GIF_FRAME_PAD * 2;
        RECT rcFrame = { contentX, contentY, contentX + frameW, contentY + frameH };
        FillRoundRect(hdc, &rcFrame, CHAT_INLINE_GIF_FRAME_RAD, CP_BG, CP_BORDER);

        int gifX = contentX + CHAT_INLINE_GIF_FRAME_PAD;
        int gifY = contentY + CHAT_INLINE_GIF_FRAME_PAD;
        Gdiplus::Graphics g(hdc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(s_pStatusGif, gifX, gifY, s_statusGifDrawW, s_statusGifDrawH);

        contentY += frameH + STATUS_GIF_GAP;
    }

    COLORREF dotColor;
    switch (s_statusDotPhase) {
        case 0:  dotColor = CP_ACCENT; break;
        case 1:  dotColor = BikodeTheme_Mix(CP_ACCENT, CP_AI_BG, 160); break;
        default: dotColor = BikodeTheme_Mix(CP_ACCENT, CP_AI_BG, 80); break;
    }
    dotCenterX = contentX + STATUS_DOT_R;
    dotCenterY = contentY + 10;
    DrawStatusDot(hdc, dotCenterX, dotCenterY, STATUS_DOT_R, dotColor);

    textX = dotCenterX + STATUS_DOT_R + STATUS_TEXT_GAP;
    textW = contentX + contentW - textX;
    if (textW < 72) textW = 72;

    hOld = (HFONT)SelectObject(hdc, s_hFontLabel ? s_hFontLabel : s_hFontBubble);
    SetTextColor(hdc, CP_TEXT_PRIMARY);
    RECT rcStatus = { textX, contentY, textX + textW, contentY + 240 };
    RECT rcStatusCalc = { 0, 0, textW, 0 };
    DrawTextW(hdc, s_wszStatusText, -1, &rcStatus,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    DrawTextW(hdc, s_wszStatusText, -1, &rcStatusCalc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    contentY += max(18, rcStatusCalc.bottom);
    SelectObject(hdc, hOld);

    if (s_wszToolText[0]) {
        RECT rcToolMeasure = { 0, 0, max(48, textW - 2 * STATUS_TOOL_PAD_H), 0 };
        RECT rcToolBox;
        RECT rcToolText;

        hOld = (HFONT)SelectObject(hdc, s_hFontStatus);
        DrawTextW(hdc, s_wszToolText, -1, &rcToolMeasure,
                  DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        SelectObject(hdc, hOld);

        contentY += STATUS_TOOL_GAP;
        rcToolBox.left = textX;
        rcToolBox.top = contentY;
        rcToolBox.right = textX + textW;
        rcToolBox.bottom = contentY + rcToolMeasure.bottom + STATUS_TOOL_PAD_V * 2;
        FillRoundRect(hdc, &rcToolBox, STATUS_TOOL_RADIUS, CP_INPUT_WELL_BG, CP_BORDER);

        rcToolText.left = rcToolBox.left + STATUS_TOOL_PAD_H;
        rcToolText.top = rcToolBox.top + STATUS_TOOL_PAD_V;
        rcToolText.right = rcToolBox.right - STATUS_TOOL_PAD_H;
        rcToolText.bottom = rcToolBox.bottom - STATUS_TOOL_PAD_V;

        hOld = (HFONT)SelectObject(hdc, s_hFontStatus);
        SetTextColor(hdc, CP_TEXT_SECONDARY);
        DrawTextW(hdc, s_wszToolText, -1, &rcToolText,
                  DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        SelectObject(hdc, hOld);
    }
}
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
    s_hFontBrandWordmark = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH | FF_ROMAN, L"Georgia");
    if (!s_hFontBrandWordmark)
        s_hFontBrandWordmark = BikodeTheme_GetFont(BKFONT_TITLE);
}

static void DestroyFonts(void)
{
    s_hFontHeader = NULL;
    s_hFontInput = NULL;
    s_hFontStatus = NULL;
    s_hFontLabel = NULL;
    s_hFontBubble = NULL;
    if (s_hFontBrandWordmark && s_hFontBrandWordmark != BikodeTheme_GetFont(BKFONT_TITLE))
        DeleteObject(s_hFontBrandWordmark);
    s_hFontBrandWordmark = NULL;
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

static int DrawBrandWordmark(HDC hdc, int left, int top)
{
    SIZE szBi = { 0 };
    SIZE szKo = { 0 };
    SIZE szDe = { 0 };
    HFONT hOld;
    int x = left;

    if (s_hIconLogo) {
        DrawIconEx(hdc, x, top + 2, s_hIconLogo, 18, 18, 0, NULL, DI_NORMAL);
        x += 24;
    }

    hOld = (HFONT)SelectObject(hdc, s_hFontBrandWordmark ? s_hFontBrandWordmark : s_hFontHeader);
    SetBkMode(hdc, TRANSPARENT);
    GetTextExtentPoint32W(hdc, L"Bi", 2, &szBi);
    GetTextExtentPoint32W(hdc, L"ko", 2, &szKo);
    GetTextExtentPoint32W(hdc, L"de.", 3, &szDe);

    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    TextOutW(hdc, x, top, L"Bi", 2);
    x += szBi.cx;

    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));
    TextOutW(hdc, x, top, L"ko", 2);
    x += szKo.cx;

    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    TextOutW(hdc, x, top, L"de.", 3);
    x += szDe.cx + 8;

    SelectObject(hdc, hOld);

    {
        RECT rcAI = { x, top + 3, x + 42, top + 21 };
        BikodeTheme_DrawChip(hdc, &rcAI, L"AI",
            BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
            BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
            BikodeTheme_GetColor(BKCLR_HOT_MAGENTA));
        x = rcAI.right;
    }

    return x;
}

static void DrawButtonFace(HDC hdc, const RECT* rc, BOOL hover, BOOL down, BOOL primary, COLORREF accent)
{
    RECT rcShadow;
    RECT rcButton;
    RECT rcUnderline;
    RECT rcHighlight;
    COLORREF fill;
    COLORREF border;
    COLORREF underline;

    if (!rc) return;

    fill = primary
        ? BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 132)
        : BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), BikodeTheme_GetColor(BKCLR_APP_BG), 220);
    border = primary
        ? BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_STROKE_DARK), 96)
        : BikodeTheme_GetColor(BKCLR_STROKE_SOFT);
    underline = primary
        ? BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), 188)
        : BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), hover ? 170 : 112);

    if (hover && !primary)
    {
        fill = BikodeTheme_Mix(accent, fill, 18);
        border = BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), 138);
    }
    if (down)
    {
        fill = BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_STROKE_DARK), fill, 76);
        border = accent;
        underline = accent;
    }

    rcShadow = *rc;
    if (!down)
    {
        OffsetRect(&rcShadow, 0, 1);
        FillRoundRectSolid(hdc, &rcShadow, 9,
            BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_APP_BG), BikodeTheme_GetColor(BKCLR_STROKE_DARK), 188));
    }

    rcButton = *rc;
    InflateRect(&rcButton, -1, -1);
    if (down)
        OffsetRect(&rcButton, 0, 1);

    BikodeTheme_DrawRoundedPanel(hdc, &rcButton, fill,
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        border,
        9, FALSE);

    rcHighlight = rcButton;
    rcHighlight.left += 10;
    rcHighlight.top += 4;
    rcHighlight.right -= 10;
    rcHighlight.bottom = rcHighlight.top + 1;
    {
        HPEN hPen = CreatePen(PS_SOLID, 1,
            BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), fill, primary ? 164 : 124));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, rcHighlight.left, rcHighlight.top, NULL);
        LineTo(hdc, rcHighlight.right, rcHighlight.top);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    rcUnderline = rcButton;
    rcUnderline.left += 8;
    rcUnderline.right -= 8;
    rcUnderline.bottom -= 4;
    rcUnderline.top = rcUnderline.bottom - (primary ? 3 : 2);
    FillRoundRectSolid(hdc, &rcUnderline, 2, underline);
}

static void DrawPromptDeckChip(HDC hdc, const RECT* rc)
{
    BOOL hasPersona;
    COLORREF accentColor;
    LPCWSTR chipLabel;

    if (!rc || IsRectEmpty(rc))
        return;

    hasPersona = s_wszActivePersonaLabel[0] != L'\0';
    chipLabel = hasPersona ? s_wszActivePersonaLabel : L"Prompt deck";
    accentColor = hasPersona ? BikodeTheme_GetColor(BKCLR_HOT_MAGENTA) : BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);

    BikodeTheme_DrawChip(hdc, rc, chipLabel,
        s_bPromptDeckHover
            ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), 24)
            : (hasPersona
                ? BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), BikodeTheme_GetColor(BKCLR_HOT_MAGENTA), 20)
                : BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_APP_BG), 228)),
        s_bPromptDeckHover ? BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN) : (hasPersona ? BikodeTheme_GetColor(BKCLR_HOT_MAGENTA) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT)),
        s_bPromptDeckHover ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY) : BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
        BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
        accentColor);
}

static void PaintHeaderModeDock(HDC hdc, const RECT* rc)
{
    if (!rc || IsRectEmpty(rc))
        return;

    BikodeTheme_DrawRoundedPanel(hdc, rc,
        BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_APP_BG), 228),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        11, FALSE);

    for (int i = 1; i < CHAT_HEADER_MODE_COUNT; i++)
    {
        if (IsRectEmpty(&s_rcHeaderModes[i - 1]) || IsRectEmpty(&s_rcHeaderModes[i]))
            continue;

        {
            int dividerX = (s_rcHeaderModes[i - 1].right + s_rcHeaderModes[i].left) / 2;
            HPEN hPen = CreatePen(PS_SOLID, 1,
                BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_STROKE_SOFT), BikodeTheme_GetColor(BKCLR_APP_BG), 176));
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
            MoveToEx(hdc, dividerX, rc->top + 6, NULL);
            LineTo(hdc, dividerX, rc->bottom - 6);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);
        }
    }
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
    int brandH = 28;
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

    titleH = max(18, MeasureWrappedTextHeight(hdc, BikodeTheme_GetFont(BKFONT_MONO),
        CHAT_EMPTY_TITLE, contentWidth));
    bodyH = MeasureWrappedTextHeight(hdc, BikodeTheme_GetFont(BKFONT_UI),
        CHAT_EMPTY_BODY,
        contentWidth);
    hintH = MeasureWrappedTextHeight(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL),
        CHAT_EMPTY_HINT,
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

    curY = 18 + 24 + 14 + brandH + 8 + titleH + 8 + bodyH + 12 + chipBottom + 12 + hintH + 16;
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
    DrawBrandWordmark(hdc, contentLeft, curY);
    curY += brandH;

    rcTitle.left = contentLeft;
    rcTitle.top = curY;
    rcTitle.right = contentRight;
    rcTitle.bottom = rcTitle.top + titleH;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_MONO));
    DrawTextW(hdc, CHAT_EMPTY_TITLE, -1, &rcTitle,
        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    rcBody.left = contentLeft;
    rcBody.top = rcTitle.bottom + 8;
    rcBody.right = contentRight;
    rcBody.bottom = rcBody.top + bodyH;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI));
    DrawTextW(hdc, CHAT_EMPTY_BODY, -1, &rcBody,
        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

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
    DrawTextW(hdc, CHAT_EMPTY_HINT, -1, &rcHint,
        DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
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
    // Add status card height if AI is working
    if (s_bAIWorking) {
        totalH += CHAT_MSG_SPACING;
        totalH += MeasureStatusCardHeight(hdc, chatW);
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
    BikodeTheme_FillHalftone(hdc, &rcBg, CP_BG);

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
                FillRoundRect(hdc, &rcFrame, CHAT_INLINE_GIF_FRAME_RAD, CP_BG, CP_BORDER);

                int gifX = frameX + CHAT_INLINE_GIF_FRAME_PAD;
                int gifY = frameY + CHAT_INLINE_GIF_FRAME_PAD;
                Gdiplus::Graphics g(hdc);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(m->pInlineGif, gifX, gifY, m->gifDrawW, m->gifDrawH);
            }
        }

        y += msgH + CHAT_MSG_SPACING;
    }

    // Draw status card if AI is working
    if (s_bAIWorking) {
        PaintStatusCard(hdc, cx, y);
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
        if (wParam == STATUS_DOT_TIMER_ID)
        {
            s_statusDotPhase = (s_statusDotPhase + 1) % 3;
            if (s_hwndChat) InvalidateRect(s_hwndChat, NULL, FALSE);
            return 0;
        }
        break;

    default:
        if (msg == WM_STATUS_GIF_READY) {
            WCHAR* path = (WCHAR*)lParam;
            if (path) {
                if (s_bAIWorking) {
                    LoadStatusGif(path);
                    UpdateGifTimerState();
                    InvalidateAllHeights();
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
                free(path);
            }
            return 0;
        }
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

    AddMessage(MSG_SYSTEM, "Ready.", NULL, 0);
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
    StatusCard_Deactivate();
    FreeStatusGif();

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

static void ChatPanel_ShowInternal(HWND hwndParent, BOOL focusInput)
{
    if (!s_hwndPanel) ChatPanel_Create(hwndParent);
    if (s_hwndPanel) {
        ShowWindow(s_hwndPanel, focusInput ? SW_SHOW : SW_SHOWNA);
        s_bVisible = TRUE;
        RECT rc;
        GetClientRect(hwndParent, &rc);
        SendMessage(hwndParent, WM_SIZE, SIZE_RESTORED,
                    MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
        if (focusInput)
            ChatPanel_FocusInput();
    }
}

void ChatPanel_Show(HWND hwndParent)
{
    ChatPanel_ShowInternal(hwndParent, TRUE);
}

void ChatPanel_ShowPassive(HWND hwndParent)
{
    ChatPanel_ShowInternal(hwndParent, FALSE);
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

    // Header chrome
    int cbSz = CHAT_HDR_BTN_SIZE;
    int headerCardLeft = 8;
    int headerCardRight = totalW - 8;
    int modeTop = CHAT_HEADER_HEIGHT - CHAT_HEADER_MODE_H - 11;
    int actionTop = modeTop;
    int modesLeft = headerCardLeft + 12;
    int actionRight;
    int actionLeft;

    s_rcCloseBtn.right  = headerCardRight - 6;
    s_rcCloseBtn.left   = s_rcCloseBtn.right - cbSz;
    s_rcCloseBtn.top    = 10;
    s_rcCloseBtn.bottom = s_rcCloseBtn.top + cbSz;

    actionRight = headerCardRight - 12;
    actionLeft = actionRight - CHAT_HEADER_ACTION_W;
    SetRect(&s_rcHeaderAction, actionLeft, actionTop, actionRight, actionTop + CHAT_HEADER_ACTION_H);
    SetRect(&s_rcHeaderModeDock, modesLeft - 4, modeTop - 3, actionLeft - 10, modeTop + CHAT_HEADER_MODE_H + 3);

    {
        const int baseWidths[CHAT_HEADER_MODE_COUNT] = { 46, 64, 68, 58, 58 };
        const int baseWidthSum = 294;
        int dockLeft = s_rcHeaderModeDock.left + 5;
        int dockRight = s_rcHeaderModeDock.right - 5;
        int widthBudget = max(0, dockRight - dockLeft - (CHAT_HEADER_MODE_GAP * (CHAT_HEADER_MODE_COUNT - 1)));
        int usedWidth = 0;

        for (int i = 0; i < CHAT_HEADER_MODE_COUNT; i++)
        {
            int width = (i == CHAT_HEADER_MODE_COUNT - 1)
                ? max(32, widthBudget - usedWidth)
                : max(32, min(baseWidths[i], (baseWidths[i] * widthBudget) / baseWidthSum));
            int modeRight = min(dockRight, dockLeft + width);

            if (modeRight <= dockLeft)
                SetRectEmpty(&s_rcHeaderModes[i]);
            else
                SetRect(&s_rcHeaderModes[i], dockLeft, modeTop, modeRight, modeTop + CHAT_HEADER_MODE_H);

            dockLeft = modeRight + CHAT_HEADER_MODE_GAP;
            usedWidth += width;
        }
    }

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
    int footerButtonTop;
    int sendLeft;
    int attachLeft;
    int searchLeft;

    s_rcInputWell.left = cardLeft + ip;
    s_rcInputWell.top = composerTop;
    s_rcInputWell.right = cardRight - ip;
    s_rcInputWell.bottom = cardBottom - ip - CHAT_COMPOSER_FOOTER_H - CHAT_COMPOSER_FOOTER_GAP;
    if (s_rcInputWell.bottom < s_rcInputWell.top + 42)
        s_rcInputWell.bottom = s_rcInputWell.top + 42;

    s_rcComposerDock.left = s_rcInputWell.left;
    s_rcComposerDock.top = s_rcInputWell.bottom + CHAT_COMPOSER_FOOTER_GAP;
    s_rcComposerDock.right = s_rcInputWell.right;
    s_rcComposerDock.bottom = cardBottom - ip;

    footerButtonTop = s_rcComposerDock.top + ((s_rcComposerDock.bottom - s_rcComposerDock.top) - CHAT_ACTION_BTN_SIZE) / 2;
    sendLeft = s_rcComposerDock.right - 8 - CHAT_ACTION_SEND_W;
    if (s_hwndSend)
        MoveWindow(s_hwndSend, sendLeft, footerButtonTop,
                   CHAT_ACTION_SEND_W, CHAT_ACTION_BTN_SIZE, TRUE);

    attachLeft = sendLeft - CHAT_ACTION_BTN_SIZE - CHAT_BTN_GAP;
    if (s_hwndAttach)
        MoveWindow(s_hwndAttach, attachLeft, footerButtonTop,
                   CHAT_ACTION_BTN_SIZE, CHAT_ACTION_BTN_SIZE, TRUE);

    searchLeft = attachLeft - CHAT_ACTION_BTN_SIZE - CHAT_BTN_GAP;
    if (s_hwndSearch)
        MoveWindow(s_hwndSearch, searchLeft, footerButtonTop,
                   CHAT_ACTION_BTN_SIZE, CHAT_ACTION_BTN_SIZE, TRUE);

    s_rcComposerActionDock.left = searchLeft - 4;
    s_rcComposerActionDock.top = footerButtonTop - 4;
    s_rcComposerActionDock.right = sendLeft + CHAT_ACTION_SEND_W + 4;
    s_rcComposerActionDock.bottom = footerButtonTop + CHAT_ACTION_BTN_SIZE + 4;

    s_rcComposerTag.left = s_rcComposerDock.left + 8;
    s_rcComposerTag.top = s_rcComposerDock.top + ((s_rcComposerDock.bottom - s_rcComposerDock.top) - CHAT_COMPOSER_TAG_H) / 2;
    s_rcComposerTag.right = min(s_rcComposerActionDock.left - 12, s_rcComposerTag.left + 122);
    s_rcComposerTag.bottom = s_rcComposerTag.top + CHAT_COMPOSER_TAG_H;

    s_rcComposerHint.left = s_rcComposerTag.right + 8;
    s_rcComposerHint.top = s_rcComposerDock.top;
    s_rcComposerHint.right = max(s_rcComposerHint.left, s_rcComposerActionDock.left - 10);
    s_rcComposerHint.bottom = s_rcComposerDock.bottom;

    int editLeft = s_rcInputWell.left + 10;
    int editTop = s_rcInputWell.top + 8;
    int editBottom = s_rcInputWell.bottom - 8;
    int editRight = s_rcInputWell.right - 10;
    if (s_hwndInput)
    {
        MoveWindow(s_hwndInput, editLeft, editTop,
                   max(24, editRight - editLeft), max(24, editBottom - editTop), TRUE);
    }

    if (s_pendingAttachmentCount > 0)
    {
        s_rcPendingStrip.left   = s_rcInputWell.left;
        s_rcPendingStrip.right  = s_rcInputWell.right;
        s_rcPendingStrip.top    = cardTop + ip;
        s_rcPendingStrip.bottom = s_rcPendingStrip.top + pendingHeight - CHAT_PENDING_PAD_TOP;
    }    else
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
    // Deactivate the status card (replaces old "remove Thinking" logic)
    StatusCard_Deactivate();

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

static const EAIChatAccessMode kHeaderModes[CHAT_HEADER_MODE_COUNT] = {
    AI_CHAT_ACCESS_API_PROVIDER,
    AI_CHAT_ACCESS_CODEX,
    AI_CHAT_ACCESS_CLAUDE,
    AI_CHAT_ACCESS_CODEX_CLAUDE,
    AI_CHAT_ACCESS_LOCAL
};

static EAIChatAccessMode GetCurrentChatAccessMode(void)
{
    const AIConfig* pCfg = AIBridge_GetConfig();
    return pCfg ? pCfg->eChatAccessMode : AI_CHAT_ACCESS_API_PROVIDER;
}

static LPCWSTR GetHeaderModeLabel(int idx)
{
    switch (idx)
    {
    case 0: return L"API";
    case 1: return L"Codex";
    case 2: return L"Claude";
    case 3: return L"Relay";
    case 4: return L"Local";
    default: return L"AI";
    }
}

static COLORREF GetHeaderModeAccent(EAIChatAccessMode mode)
{
    switch (mode)
    {
    case AI_CHAT_ACCESS_CODEX:
        return BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
    case AI_CHAT_ACCESS_CLAUDE:
        return BikodeTheme_GetColor(BKCLR_HOT_MAGENTA);
    case AI_CHAT_ACCESS_CODEX_CLAUDE:
        return BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN);
    case AI_CHAT_ACCESS_LOCAL:
        return BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN);
    case AI_CHAT_ACCESS_API_PROVIDER:
    default:
        return BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);
    }
}

static int HitTestHeaderMode(POINT pt)
{
    for (int i = 0; i < CHAT_HEADER_MODE_COUNT; i++)
    {
        if (!IsRectEmpty(&s_rcHeaderModes[i]) && PtInRect(&s_rcHeaderModes[i], pt))
            return i;
    }
    return -1;
}

static void GetHeaderActionLabel(WCHAR* out, int cchOut)
{
    EAIChatAccessMode mode;

    if (!out || cchOut <= 0)
        return;

    out[0] = L'\0';
    mode = GetCurrentChatAccessMode();
    if (mode == AI_CHAT_ACCESS_API_PROVIDER)
    {
        lstrcpynW(out, L"Settings", cchOut);
        return;
    }
    if (mode == AI_CHAT_ACCESS_LOCAL)
    {
        lstrcpynW(out, L"Detect", cchOut);
        return;
    }

    lstrcpynW(out,
        AISubscriptionAgent_IsAuthenticated(mode) ? L"Logout" : L"Login",
        cchOut);
}

static void PaintHeaderPill(HDC hdc, const RECT* rc, LPCWSTR text, COLORREF accent,
                            BOOL active, BOOL hover, BOOL primary)
{
    RECT rcText;
    COLORREF fg;
    HFONT hFont;

    if (!rc || IsRectEmpty(rc))
        return;

    if (primary)
    {
        RECT rcAccent = *rc;
        BikodeTheme_DrawRoundedPanel(hdc, rc,
            BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_APP_BG), hover ? 212 : 230),
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            hover ? accent : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            11, FALSE);
        rcAccent.left += 8;
        rcAccent.right = rcAccent.left + 4;
        rcAccent.top += 6;
        rcAccent.bottom -= 6;
        FillRoundRectSolid(hdc, &rcAccent, 2, accent);
    }
    else if (active || hover)
    {
        RECT rcFill = *rc;
        InflateRect(&rcFill, -2, -2);
        BikodeTheme_DrawRoundedPanel(hdc, &rcFill,
            active
                ? BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 54)
                : BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), 18),
            active ? BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_STROKE_DARK), 110)
                   : BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            active ? BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), 152)
                   : BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_STROKE_SOFT), 164),
            9, FALSE);
        if (active)
        {
            RECT rcAccent = rcFill;
            rcAccent.left += 10;
            rcAccent.right -= 10;
            rcAccent.top += 4;
            rcAccent.bottom = rcAccent.top + 3;
            FillRoundRectSolid(hdc, &rcAccent, 2, accent);
        }
    }

    rcText = *rc;
    rcText.left += primary ? 12 : 8;
    rcText.right -= 8;
    SetBkMode(hdc, TRANSPARENT);
    fg = (active || hover || primary)
        ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY)
        : BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY);
    SetTextColor(hdc, fg);
    hFont = (active || primary) ? BikodeTheme_GetFont(BKFONT_UI_BOLD) : BikodeTheme_GetFont(BKFONT_MONO_SMALL);
    SelectObject(hdc, hFont);
    DrawTextW(hdc, text ? text : L"", -1, &rcText,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void UpdateHeaderHoverState(HWND hwnd, const POINT* pPt)
{
    BOOL oldClose = s_bCloseHover;
    int oldMode = s_iHeaderHotMode;
    BOOL oldAction = s_bHeaderActionHover;
    BOOL oldPromptDeck = s_bPromptDeckHover;

    if (!pPt)
    {
        s_bCloseHover = FALSE;
        s_iHeaderHotMode = -1;
        s_bHeaderActionHover = FALSE;
        s_bPromptDeckHover = FALSE;
    }
    else
    {
        s_bCloseHover = PtInRect(&s_rcCloseBtn, *pPt);
        s_iHeaderHotMode = HitTestHeaderMode(*pPt);
        s_bHeaderActionHover = !IsRectEmpty(&s_rcHeaderAction) && PtInRect(&s_rcHeaderAction, *pPt);
        s_bPromptDeckHover = !IsRectEmpty(&s_rcComposerTag) && PtInRect(&s_rcComposerTag, *pPt);
    }

    if (oldClose != s_bCloseHover || oldMode != s_iHeaderHotMode || oldAction != s_bHeaderActionHover || oldPromptDeck != s_bPromptDeckHover)
        InvalidateRect(hwnd, NULL, FALSE);

    if (pPt && (s_bCloseHover || s_iHeaderHotMode >= 0 || s_bHeaderActionHover || s_bPromptDeckHover))
    {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
    }
}

static BOOL ApplyHeaderChatMode(EAIChatAccessMode mode)
{
    const AIConfig* pCurrent = AIBridge_GetConfig();
    AIConfig nextCfg;

    if (!pCurrent)
        return FALSE;
    if (pCurrent->eChatAccessMode == mode)
        return TRUE;
    if (s_bAIWorking || AISubscriptionAgent_IsBusy())
    {
        ChatPanel_AppendSystem("Finish the current run before switching chat mode.");
        return FALSE;
    }

    memcpy(&nextCfg, pCurrent, sizeof(nextCfg));
    nextCfg.eChatAccessMode = mode;
    AIBridge_ApplyConfig(&nextCfg);
    if (szIniFile[0])
        AIBridge_SaveConfig(&nextCfg, szIniFile);

    if (s_hwndPanel)
        InvalidateRect(s_hwndPanel, NULL, FALSE);
    return TRUE;
}

static void TriggerHeaderAction(void)
{
    EAIChatAccessMode mode = GetCurrentChatAccessMode();

    if (s_bAIWorking || AISubscriptionAgent_IsBusy())
    {
        ChatPanel_AppendSystem("Finish the current run before changing chat access.");
        return;
    }

    if (mode == AI_CHAT_ACCESS_API_PROVIDER)
    {
        HWND hwndMain = GetParent(s_hwndPanel);
        if (hwndMain)
            SendMessage(hwndMain, WM_COMMAND, MAKEWPARAM(IDM_AI_SETTINGS, 0), 0);
        return;
    }

    if (AISubscriptionAgent_IsAuthenticated(mode))
    {
        if (!AISubscriptionAgent_Logout(mode))
        {
            ChatPanel_AppendSystem("Could not remove the current embedded vendor login.");
            return;
        }
        ChatPanel_AppendSystem("Embedded vendor credentials removed.");
    }
    else
    {
        if (!AISubscriptionAgent_OpenLoginFlow(mode, s_hwndPanel))
        {
            ChatPanel_AppendSystem("Could not launch the selected login flow.");
            return;
        }
        ChatPanel_AppendSystem("Login flow opened. Finish sign-in and return to Bikode.");
    }

    if (s_hwndPanel)
        InvalidateRect(s_hwndPanel, NULL, FALSE);
}

static void SetComposerPrompt(LPCWSTR wszPrompt)
{
    if (!s_hwndInput || !wszPrompt)
        return;

    SetWindowTextW(s_hwndInput, wszPrompt);
    SendMessageW(s_hwndInput, EM_SETSEL, (WPARAM)lstrlenW(wszPrompt), (LPARAM)lstrlenW(wszPrompt));
    SetFocus(s_hwndInput);
}

static void SetActivePersona(const char* persona, LPCWSTR label)
{
    if (persona && persona[0])
    {
        StringCchCopyA(s_szActivePersona, ARRAYSIZE(s_szActivePersona), persona);
        StringCchCopyW(s_wszActivePersonaLabel, ARRAYSIZE(s_wszActivePersonaLabel), label ? label : L"Agent");
        {
            WCHAR wszMsg[256];
            StringCchPrintfW(wszMsg, ARRAYSIZE(wszMsg), L"Persona active: %s. Your next message uses this specialist.", s_wszActivePersonaLabel);
            ChatPanel_AppendSystem(NULL); /* clear status */
            {
                char szMsg[256];
                WideCharToMultiByte(CP_UTF8, 0, wszMsg, -1, szMsg, ARRAYSIZE(szMsg), NULL, NULL);
                ChatPanel_AppendSystem(szMsg);
            }
        }
    }
    else
    {
        s_szActivePersona[0] = '\0';
        s_wszActivePersonaLabel[0] = L'\0';
        ChatPanel_AppendSystem("Persona cleared. Using automatic role detection.");
    }
    if (s_hwndPanel)
        InvalidateRect(s_hwndPanel, NULL, FALSE);
}

static void TriggerPromptDeck(void)
{
    HMENU hMenu, hEngSub, hAgencyDesignSub, hTestingSub, hProductSub, hOrchSub, hEvalSub, hDesignSub, hCtxSub;
    RECT rcAnchor;
    POINT pt;
    UINT cmd;

    if (!s_hwndPanel || IsRectEmpty(&s_rcComposerTag))
        return;

    hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    AppendMenuW(hMenu, MF_STRING, IDM_CHAT_PROMPT_SUMMARY,  L"Explain this project");
    AppendMenuW(hMenu, MF_STRING, IDM_CHAT_PROMPT_BUG,      L"Find the likeliest bug");
    AppendMenuW(hMenu, MF_STRING, IDM_CHAT_PROMPT_REVIEW,   L"Review for risks");
    AppendMenuW(hMenu, MF_STRING, IDM_CHAT_PROMPT_PLAN,     L"Plan the smallest safe fix");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    /* Agency personas: Engineering */
    hEngSub = CreatePopupMenu();
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_ARCHITECT,    L"Software Architect");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_CODE_REVIEW,  L"Code Reviewer");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_SECURITY,     L"Security Engineer");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_FRONTEND,     L"Frontend Developer");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_BACKEND,      L"Backend Architect");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_DEVOPS,       L"DevOps Automator");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_DB_OPTIMIZER, L"Database Optimizer");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_RAPID_PROTO,  L"Rapid Prototyper");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_TECH_WRITER,  L"Technical Writer");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_SRE,          L"SRE");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_GIT_WORKFLOW, L"Git Workflow Master");
    AppendMenuW(hEngSub, MF_STRING, IDM_CHAT_PERSONA_INCIDENT,     L"Incident Commander");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hEngSub, L"Engineering Personas");

    hAgencyDesignSub = CreatePopupMenu();
    AppendMenuW(hAgencyDesignSub, MF_STRING, IDM_CHAT_PERSONA_UX_ARCH,     L"UX Architect");
    AppendMenuW(hAgencyDesignSub, MF_STRING, IDM_CHAT_PERSONA_UI_DESIGNER, L"UI Designer");
    AppendMenuW(hAgencyDesignSub, MF_STRING, IDM_CHAT_PERSONA_UX_RESEARCH, L"UX Researcher");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hAgencyDesignSub, L"Design Personas");

    hTestingSub = CreatePopupMenu();
    AppendMenuW(hTestingSub, MF_STRING, IDM_CHAT_PERSONA_PERF_BENCH,    L"Performance Benchmarker");
    AppendMenuW(hTestingSub, MF_STRING, IDM_CHAT_PERSONA_ACCESSIBILITY, L"Accessibility Auditor");
    AppendMenuW(hTestingSub, MF_STRING, IDM_CHAT_PERSONA_API_TESTER,    L"API Tester");
    AppendMenuW(hTestingSub, MF_STRING, IDM_CHAT_PERSONA_REALITY,       L"Reality Checker");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hTestingSub, L"Testing Personas");

    hProductSub = CreatePopupMenu();
    AppendMenuW(hProductSub, MF_STRING, IDM_CHAT_PERSONA_SPRINT,   L"Sprint Prioritizer");
    AppendMenuW(hProductSub, MF_STRING, IDM_CHAT_PERSONA_FEEDBACK, L"Feedback Synthesizer");
    AppendMenuW(hProductSub, MF_STRING, IDM_CHAT_PERSONA_TREND,    L"Trend Researcher");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hProductSub, L"Product Personas");

    hOrchSub = CreatePopupMenu();
    AppendMenuW(hOrchSub, MF_STRING, IDM_CHAT_PERSONA_ORCHESTRATOR, L"Orchestrator");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hOrchSub, L"Agency Orchestration");

    /* Eval & Red Team personas */
    hEvalSub = CreatePopupMenu();
    AppendMenuW(hEvalSub, MF_STRING, IDM_CHAT_PERSONA_EVALUATOR,    L"Prompt Evaluator");
    AppendMenuW(hEvalSub, MF_STRING, IDM_CHAT_PERSONA_RED_TEAM,     L"Red Teamer");
    AppendMenuW(hEvalSub, MF_STRING, IDM_CHAT_PERSONA_MODEL_COMPARE, L"Model Comparator");
    AppendMenuW(hEvalSub, MF_STRING, IDM_CHAT_PERSONA_REGRESSION,   L"Regression Guard");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hEvalSub, L"Eval && Red Team");

    /* Design quality personas */
    hDesignSub = CreatePopupMenu();
    AppendMenuW(hDesignSub, MF_STRING, IDM_CHAT_PERSONA_DESIGN_AUDIT, L"Design Auditor");
    AppendMenuW(hDesignSub, MF_STRING, IDM_CHAT_PERSONA_DESIGN_CRIT,  L"Design Critic");
    AppendMenuW(hDesignSub, MF_STRING, IDM_CHAT_PERSONA_DESIGN_POLISH, L"Design Polisher");
    AppendMenuW(hDesignSub, MF_STRING, IDM_CHAT_PERSONA_TYPOGRAPHY,   L"Typography Expert");
    AppendMenuW(hDesignSub, MF_STRING, IDM_CHAT_PERSONA_COLOR,        L"Color Specialist");
    AppendMenuW(hDesignSub, MF_STRING, IDM_CHAT_PERSONA_MOTION,       L"Motion Designer");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hDesignSub, L"Frontend Design");

    /* Context & Memory personas */
    hCtxSub = CreatePopupMenu();
    AppendMenuW(hCtxSub, MF_STRING, IDM_CHAT_PERSONA_CONTEXT_ARCH, L"Context Architect");
    AppendMenuW(hCtxSub, MF_STRING, IDM_CHAT_PERSONA_MEMORY,       L"Memory Curator");
    AppendMenuW(hCtxSub, MF_STRING, IDM_CHAT_PERSONA_RETRIEVAL,    L"Retrieval Optimizer");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hCtxSub, L"Context && Memory");

    if (s_szActivePersona[0])
    {
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hMenu, MF_STRING, IDM_CHAT_PERSONA_CLEAR, L"Clear active persona");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_CHAT_PROMPT_COMMANDS, L"Open Command Palette...");

    rcAnchor = s_rcComposerTag;
    pt.x = rcAnchor.left;
    pt.y = rcAnchor.bottom;
    ClientToScreen(s_hwndPanel, &pt);
    cmd = (UINT)TrackPopupMenuEx(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
        pt.x, pt.y, s_hwndPanel, NULL);
    DestroyMenu(hMenu);

    switch (cmd)
    {
    case IDM_CHAT_PROMPT_SUMMARY:
        SetComposerPrompt(L"What does this project do? Inspect the workspace and summarize it simply.");
        break;
    case IDM_CHAT_PROMPT_BUG:
        SetComposerPrompt(L"Inspect this workspace and point out the most likely bug, broken edge case, or shaky implementation detail.");
        break;
    case IDM_CHAT_PROMPT_REVIEW:
        SetComposerPrompt(L"Review this code with a critical eye. Call out the biggest correctness risks, regressions, and missing checks.");
        break;
    case IDM_CHAT_PROMPT_PLAN:
        SetComposerPrompt(L"Before editing, inspect the workspace and outline the smallest safe implementation plan for this request.");
        break;
    case IDM_CHAT_PROMPT_COMMANDS:
    {
        HWND hwndMain = GetParent(s_hwndPanel);
        if (hwndMain)
            PostMessageW(hwndMain, WM_COMMAND, MAKEWPARAM(IDM_BIKO_COMMAND_PALETTE, 0), 0);
        break;
    }
    /* --- Engineering personas --- */
    case IDM_CHAT_PERSONA_ARCHITECT:
        SetActivePersona(
            "You are Software Architect. Design maintainable, scalable systems aligned with business domains. "
            "Think in bounded contexts and trade-off matrices. No architecture astronautics. "
            "Domain first, technology second. Present at least two options with trade-offs. "
            "Use context_store to persist architectural decisions and eval_prompt to validate prompt quality.",
            L"Software Architect");
        break;
    case IDM_CHAT_PERSONA_CODE_REVIEW:
        SetActivePersona(
            "You are Code Reviewer. Provide thorough, constructive reviews focused on correctness, security, "
            "maintainability, and performance. Prioritize as blockers, suggestions, nits. "
            "Be specific with line references. Praise good patterns.",
            L"Code Reviewer");
        break;
    case IDM_CHAT_PERSONA_SECURITY:
        SetActivePersona(
            "You are Security Engineer. Conduct threat modeling, vulnerability assessment, and secure code review. "
            "Apply STRIDE analysis. Review for OWASP Top 10 and CWE Top 25. "
            "Assume all input is malicious. Pair findings with actionable remediation. "
            "Use red_team_prompt and eval_prompt tools to systematically analyze prompts and code for vulnerabilities.",
            L"Security Engineer");
        break;
    case IDM_CHAT_PERSONA_FRONTEND:
        SetActivePersona(
            "You are Frontend Developer. Build responsive, accessible, performant web applications. "
            "Focus on Core Web Vitals, component architecture, and mobile-first responsive design. "
            "Use distinctive design choices, not generic templates. "
            "Use design_audit tool to check CSS/HTML files for typography, color contrast, and accessibility issues.",
            L"Frontend Dev");
        break;
    case IDM_CHAT_PERSONA_BACKEND:
        SetActivePersona(
            "You are Backend Architect. Design and implement scalable APIs, data models, and service boundaries. "
            "Think in failure modes, contracts, migrations, and observability. "
            "Choose boring, reliable patterns before cleverness. "
            "Include security, validation, monitoring, and rollback in the design.",
            L"Backend Architect");
        break;
    case IDM_CHAT_PERSONA_DEVOPS:
        SetActivePersona(
            "You are DevOps Automator. Design Infrastructure as Code, CI/CD pipelines, and cloud operations. "
            "Implement zero-downtime deployment strategies. Include monitoring, alerting, and automated rollback.",
            L"DevOps");
        break;
    case IDM_CHAT_PERSONA_DB_OPTIMIZER:
        SetActivePersona(
            "You are Database Optimizer. Think in query plans, indexes, and connection pools. "
            "Design schemas that scale. Debug slow queries with EXPLAIN ANALYZE. "
            "Every foreign key gets an index. Every migration is reversible.",
            L"DB Optimizer");
        break;
    case IDM_CHAT_PERSONA_RAPID_PROTO:
        SetActivePersona(
            "You are Rapid Prototyper. Build the fastest useful proof-of-concept without losing the path to production. "
            "Prioritize core user flows, explicit assumptions, and reversible implementation choices. "
            "Ship a thin slice first, then tighten the rough edges that matter.",
            L"Rapid Prototyper");
        break;
    case IDM_CHAT_PERSONA_TECH_WRITER:
        SetActivePersona(
            "You are Technical Writer. Produce clear developer-facing documentation, guides, changelogs, and architecture notes. "
            "Optimize for scanability, correct terminology, and examples that actually help someone ship the work. "
            "Expose ambiguity and missing assumptions instead of papering over them.",
            L"Technical Writer");
        break;
    case IDM_CHAT_PERSONA_SRE:
        SetActivePersona(
            "You are SRE. Think in SLOs, error budgets, rollback safety, capacity, and observability. "
            "Reduce operational risk through automation, guardrails, and graceful degradation. "
            "Prioritize reliability work that materially improves production behavior.",
            L"SRE");
        break;
    case IDM_CHAT_PERSONA_GIT_WORKFLOW:
        SetActivePersona(
            "You are Git Workflow Master. Design clean branch strategy, release flow, rebasing plans, and conventional commits. "
            "Optimize for reviewability, traceability, and low-friction collaboration. "
            "Preserve useful history while avoiding unnecessary churn.",
            L"Git Workflow");
        break;
    case IDM_CHAT_PERSONA_INCIDENT:
        SetActivePersona(
            "You are Incident Commander. Triage by user impact and severity, define the blast radius, and coordinate the response. "
            "Keep status communication crisp, timelines accurate, and the mitigation path practical. "
            "Drive toward reduced MTTR and a blameless postmortem with concrete follow-ups.",
            L"Incident Cmdr");
        break;
    case IDM_CHAT_PERSONA_UX_ARCH:
        SetActivePersona(
            "You are UX Architect. Define layout systems, component structure, interaction flows, and accessible foundations. "
            "Turn product goals into implementation-ready UI architecture with strong hierarchy and responsive behavior. "
            "Favor systems that developers can actually build and maintain.",
            L"UX Architect");
        break;
    case IDM_CHAT_PERSONA_UI_DESIGNER:
        SetActivePersona(
            "You are UI Designer. Craft a distinctive visual language with strong typography, spacing, and compositional rhythm. "
            "Push beyond generic templates while preserving clarity, usability, and brand coherence. "
            "Make visual choices intentional and defensible.",
            L"UI Designer");
        break;
    case IDM_CHAT_PERSONA_UX_RESEARCH:
        SetActivePersona(
            "You are UX Researcher. Evaluate the user journey, identify pain points, and ground recommendations in evidence. "
            "Separate assumptions from observed behavior. "
            "Turn messy qualitative input into concrete product and design decisions.",
            L"UX Researcher");
        break;
    case IDM_CHAT_PERSONA_PERF_BENCH:
        SetActivePersona(
            "You are Performance Benchmarker. Establish baselines, profile bottlenecks, and measure the effect of each change. "
            "Think in latency budgets, Core Web Vitals, hot paths, and regression thresholds. "
            "Quantify impact instead of guessing.",
            L"Perf Bench");
        break;
    case IDM_CHAT_PERSONA_ACCESSIBILITY:
        SetActivePersona(
            "You are Accessibility Auditor. Review for WCAG AA, keyboard navigation, screen reader support, focus handling, motion safety, and contrast. "
            "Catch the real-world issues automated scans miss and pair every finding with a concrete fix. "
            "Accessibility is a functional requirement, not a polishing pass.",
            L"A11y Auditor");
        break;
    case IDM_CHAT_PERSONA_API_TESTER:
        SetActivePersona(
            "You are API Tester. Stress API contracts, error handling, auth boundaries, pagination, edge cases, and payload validation. "
            "Think like a hostile but systematic client. "
            "Verify schemas, status codes, and failure behavior with precision.",
            L"API Tester");
        break;
    case IDM_CHAT_PERSONA_REALITY:
        SetActivePersona(
            "You are Reality Checker. Demand evidence for correctness, readiness, and claims of completion. "
            "Assess reliability, security, UX, validation coverage, and rollback posture before approving shipment. "
            "No hand-waving, only defensible go or no-go reasoning.",
            L"Reality Checker");
        break;
    case IDM_CHAT_PERSONA_SPRINT:
        SetActivePersona(
            "You are Sprint Prioritizer. Break work into the smallest valuable increments using explicit trade-offs, dependencies, and acceptance criteria. "
            "Use product judgment, not backlog theater. "
            "Optimize for outcome and delivery confidence rather than raw activity.",
            L"Sprint Prioritizer");
        break;
    case IDM_CHAT_PERSONA_FEEDBACK:
        SetActivePersona(
            "You are Feedback Synthesizer. Aggregate user feedback into recurring themes, urgency, and opportunity areas. "
            "Distinguish symptoms from root needs. "
            "Turn raw qualitative input into clear recommendations the team can act on.",
            L"Feedback Synth");
        break;
    case IDM_CHAT_PERSONA_TREND:
        SetActivePersona(
            "You are Trend Researcher. Scan the competitive and technical landscape for meaningful signals, not hype. "
            "Identify adoption patterns, strategic risks, and market opportunities with practical implications for the product. "
            "Separate noise from actionable trend data.",
            L"Trend Researcher");
        break;
    case IDM_CHAT_PERSONA_ORCHESTRATOR:
        SetActivePersona(
            "You are Orchestrator. Coordinate multi-agent work into a coherent pipeline with explicit handoffs, quality gates, and status updates. "
            "Sequence the work so each specialist has the right context at the right time. "
            "Keep the workflow pragmatic, observable, and outcome-focused.",
            L"Orchestrator");
        break;
    /* --- Eval & Red Team personas --- */
    case IDM_CHAT_PERSONA_EVALUATOR:
        SetActivePersona(
            "You are Prompt Evaluator. Design test cases measuring prompt quality across correctness, relevance, safety. "
            "Use assertion-based grading. Track metrics across iterations. Report pass/fail with confidence. "
            "Use eval_prompt tool to run automated quality checks on prompts. Use red_team_prompt for security analysis.",
            L"Evaluator");
        break;
    case IDM_CHAT_PERSONA_RED_TEAM:
        SetActivePersona(
            "You are Red Teamer. Probe AI systems for vulnerabilities: prompt injection, jailbreaks, data leakage, bias. "
            "Generate adversarial test cases. Classify by severity. Provide remediation strategies. "
            "Use red_team_prompt tool to systematically scan prompts for injection, jailbreak, leakage, and bias vulnerabilities.",
            L"Red Teamer");
        break;
    case IDM_CHAT_PERSONA_MODEL_COMPARE:
        SetActivePersona(
            "You are Model Comparator. Compare candidate models and prompt variants across quality, safety, cost, and latency. "
            "Build side-by-side evaluation criteria and make the trade-offs explicit. "
            "Recommend the right model for the actual workload, not the benchmark leaderboard.",
            L"Model Comparator");
        break;
    case IDM_CHAT_PERSONA_REGRESSION:
        SetActivePersona(
            "You are Regression Guard. Define golden tests and release gates that catch output quality drift before changes ship. "
            "Enforce thresholds for safety, correctness, and relevance. "
            "Treat prompt regressions like real production regressions.",
            L"Regression Guard");
        break;
    /* --- Design quality personas --- */
    case IDM_CHAT_PERSONA_DESIGN_AUDIT:
        SetActivePersona(
            "You are Design Auditor. Run technical quality checks: accessibility (WCAG AA), performance, responsive behavior. "
            "Flag anti-patterns: nested cards, gray-on-color text, pure black/white, overused fonts, glassmorphism overuse. "
            "Use design_audit tool to check files for typography, color, spacing, hierarchy, and a11y issues.",
            L"Design Audit");
        break;
    case IDM_CHAT_PERSONA_DESIGN_CRIT:
        SetActivePersona(
            "You are Design Critic. Review for hierarchy clarity, emotional resonance, and intentional aesthetics. "
            "Push for distinctive design over safe defaults. Does it have a bold direction or generic AI slop?",
            L"Design Critic");
        break;
    case IDM_CHAT_PERSONA_DESIGN_POLISH:
        SetActivePersona(
            "You are Design Polisher. Apply the last production pass: refine spacing, states, micro-interactions, motion restraint, and copy clarity. "
            "Remove unnecessary complexity and make every surface feel intentional. "
            "Use design_audit to verify the implementation still holds up after the polish.",
            L"Design Polisher");
        break;
    case IDM_CHAT_PERSONA_TYPOGRAPHY:
        SetActivePersona(
            "You are Typography Expert. Design modular type scales with fluid sizing. Choose distinctive fonts. "
            "Create hierarchy with fewer sizes and more contrast. Use OKLCH color. Implement proper measure (65ch). "
            "Use design_audit tool with checks='typography' to analyze font usage in CSS/HTML files.",
            L"Typography");
        break;
    case IDM_CHAT_PERSONA_COLOR:
        SetActivePersona(
            "You are Color Specialist. Use OKLCH for perceptually uniform palettes. Tint neutrals toward brand hue. "
            "Follow 60-30-10 rule. Never use pure black or white. Ensure WCAG AA contrast on all text. "
            "Use design_audit tool with checks='color,a11y' to analyze color declarations and contrast ratios.",
            L"Color Expert");
        break;
    case IDM_CHAT_PERSONA_MOTION:
        SetActivePersona(
            "You are Motion Designer. Use animation to clarify state changes, not decorate randomly. "
            "Favor a few high-impact transitions, coherent timing, and respect for prefers-reduced-motion. "
            "Build motion that feels intentional, modern, and easy to maintain.",
            L"Motion Designer");
        break;
    /* --- Context & Memory personas --- */
    case IDM_CHAT_PERSONA_CONTEXT_ARCH:
        SetActivePersona(
            "You are Context Architect. Organize agent context using a filesystem paradigm. "
            "Design tiered context loading (L0/L1/L2) to reduce token consumption. "
            "Structure directories for recursive retrieval. Define schemas for session management. "
            "Use context_store tool to persist and retrieve architectural knowledge entries.",
            L"Context Arch");
        break;
    case IDM_CHAT_PERSONA_MEMORY:
        SetActivePersona(
            "You are Memory Curator. Extract and compress long-term memories from sessions. "
            "Separate task memory from user memory. Organize hierarchically for efficient retrieval. "
            "Prune outdated entries. Make the agent smarter each session. "
            "Use context_store tool to store, retrieve, and list workspace knowledge entries.",
            L"Memory");
        break;
    case IDM_CHAT_PERSONA_RETRIEVAL:
        SetActivePersona(
            "You are Retrieval Optimizer. Improve how relevant context is discovered and loaded under tight token budgets. "
            "Combine directory structure, semantic search, and retrieval observability so context selection is debuggable. "
            "Use context_store to test what should be stored, loaded, and pruned.",
            L"Retrieval Opt");
        break;
    case IDM_CHAT_PERSONA_CLEAR:
        SetActivePersona(NULL, NULL);
        break;
    }
}

static BOOL DispatchChatRequest(const char* prompt,
                                const AIChatAttachment* pAttachments,
                                int cAttachments,
                                const char* statusText,
                                const char* missingAccessMessage)
{
    const AIConfig* pCfg = AIBridge_GetConfig();
    EAIChatAccessMode mode;
    char* effectivePrompt = NULL;
    BOOL result;

    if (!prompt || !prompt[0])
        return FALSE;
    if (!pCfg)
    {
        ChatPanel_AppendSystem("AI settings are not available.");
        return FALSE;
    }

    /* Inject active persona context if set */
    if (s_szActivePersona[0])
    {
        int personaLen = (int)strlen(s_szActivePersona);
        int promptLen = (int)strlen(prompt);
        effectivePrompt = (char*)n2e_Alloc(personaLen + promptLen + 64);
        if (effectivePrompt)
            StringCchPrintfA(effectivePrompt, personaLen + promptLen + 64,
                "[Active persona: %s]\n\n%s", s_szActivePersona, prompt);
    }

    mode = pCfg->eChatAccessMode;
    if (mode == AI_CHAT_ACCESS_API_PROVIDER)
    {
        const AIProviderConfig* pProviderCfg = AIBridge_GetProviderConfig();
        HWND hwndMainWnd = GetParent(s_hwndPanel);
        if (!AIBridge_HasChatAccess() || !pProviderCfg)
        {
            ChatPanel_AppendSystem(missingAccessMessage);
            if (effectivePrompt) n2e_Free(effectivePrompt);
            return FALSE;
        }
        result = AIAgent_ChatAsync(pProviderCfg,
            effectivePrompt ? effectivePrompt : prompt,
            pAttachments, cAttachments, s_hwndPanel, hwndMainWnd);
        if (effectivePrompt) n2e_Free(effectivePrompt);
        if (!result)
        {
            ChatPanel_AppendSystem("AI is busy. Please wait.");
            return FALSE;
        }
        StatusCard_Activate(statusText);
        return TRUE;
    }

    if (mode == AI_CHAT_ACCESS_LOCAL)
    {
        HWND hwndMainWnd = GetParent(s_hwndPanel);
        EAIProvider detected = AIProvider_DetectLocal();
        if (detected >= AI_PROVIDER_COUNT)
        {
            ChatPanel_AppendSystem("No local model server detected. Start Ollama, LM Studio, llama.cpp, vLLM, or LocalAI first.");
            if (effectivePrompt) n2e_Free(effectivePrompt);
            return FALSE;
        }
        {
            AIProviderConfig localCfg;
            const AIProviderDef* pDef = AIProvider_Get(detected);
            AIProviderConfig_InitDefaults(&localCfg, detected);
            ChatPanel_AppendSystem(pDef ? pDef->szName : "Local model");
            result = AIAgent_ChatAsync(&localCfg,
                effectivePrompt ? effectivePrompt : prompt,
                pAttachments, cAttachments, s_hwndPanel, hwndMainWnd);
        }
        if (effectivePrompt) n2e_Free(effectivePrompt);
        if (!result)
        {
            ChatPanel_AppendSystem("AI is busy. Please wait.");
            return FALSE;
        }
        StatusCard_Activate(statusText);
        return TRUE;
    }

    if (!AISubscriptionAgent_IsAuthenticated(mode))
    {
        ChatPanel_AppendSystem("Login required. Use LOGIN in the chat header.");
        if (effectivePrompt) n2e_Free(effectivePrompt);
        return FALSE;
    }
    result = AISubscriptionAgent_ChatAsync(pCfg,
        effectivePrompt ? effectivePrompt : prompt,
        pAttachments, cAttachments, s_hwndPanel);
    if (effectivePrompt) n2e_Free(effectivePrompt);
    if (!result)
    {
        ChatPanel_AppendSystem("AI is busy. Please wait.");
        return FALSE;
    }
    StatusCard_Activate(statusText);
    return TRUE;
}
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

        DispatchChatRequest(displayText,
            cAtt > 0 ? aiAttachments : NULL,
            cAtt,
            "Thinking...",
            "No API access configured. Use SETTINGS in the chat header.");

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

        ChatPanel_AppendUserMessage(utf8, NULL, 0);

        char* searchPrompt = (char*)n2e_Alloc(utf8Len + 32);
        if (searchPrompt) {
            sprintf(searchPrompt, "Web Search: %s", utf8);
            DispatchChatRequest(searchPrompt,
                NULL,
                0,
                "Searching web...",
                "No API access configured. Use SETTINGS in the chat header.");
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
            BikodeTheme_FillHalftone(hm, &rc, CP_BG);
        }

        // Header - selector and auth chrome
        {
            RECT rcHeaderCard;
            RECT rcAction;
            WCHAR wszAction[32];
            int brandLeft;

            rcHeaderCard.left = 8;
            rcHeaderCard.top = 4;
            rcHeaderCard.right = cxW - 8;
            rcHeaderCard.bottom = CHAT_HEADER_HEIGHT - 4;
            BikodeTheme_DrawCutCornerPanel(hm, &rcHeaderCard,
                BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), BikodeTheme_GetColor(BKCLR_APP_BG), 232),
                BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                8, FALSE);

            {
                HPEN hPen = CreatePen(PS_SOLID, 1,
                    BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_STROKE_SOFT), BikodeTheme_GetColor(BKCLR_APP_BG), 188));
                HPEN hOldPen = (HPEN)SelectObject(hm, hPen);
                MoveToEx(hm, rcHeaderCard.left + 12, rcHeaderCard.top + 34, NULL);
                LineTo(hm, rcHeaderCard.right - 12, rcHeaderCard.top + 34);
                SelectObject(hm, hOldPen);
                DeleteObject(hPen);
            }

            DrawStatusDot(hm, rcHeaderCard.left + 12, rcHeaderCard.top + 14, 3,
                BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
            brandLeft = rcHeaderCard.left + 22;
            DrawBrandWordmark(hm, brandLeft, rcHeaderCard.top + 4);

            PaintHeaderModeDock(hm, &s_rcHeaderModeDock);

            GetHeaderActionLabel(wszAction, ARRAYSIZE(wszAction));
            rcAction = s_rcHeaderAction;
            PaintHeaderPill(hm, &rcAction, wszAction,
                GetHeaderModeAccent(GetCurrentChatAccessMode()),
                FALSE, s_bHeaderActionHover, TRUE);

            for (int i = 0; i < CHAT_HEADER_MODE_COUNT; i++)
            {
                PaintHeaderPill(hm, &s_rcHeaderModes[i], GetHeaderModeLabel(i),
                    GetHeaderModeAccent(kHeaderModes[i]),
                    GetCurrentChatAccessMode() == kHeaderModes[i],
                    s_iHeaderHotMode == i,
                    FALSE);
            }
            if (s_bCloseHover)
            {
                BikodeTheme_DrawRoundedPanel(hm, &s_rcCloseBtn,
                    BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_APP_BG), 230),
                    BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                    BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                    8, FALSE);
            }
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
            rcRail.bottom = min(s_rcComposerDock.bottom, s_rcInputCard.bottom - 10);
            {
                HBRUSH hRail = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
                FillRect(hm, &rcRail, hRail);
                DeleteObject(hRail);
            }

            if (!IsRectEmpty(&s_rcInputWell))
            {
                BikodeTheme_DrawRoundedPanel(hm, &s_rcInputWell,
                    CP_INPUT_WELL_BG,
                    BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                    s_bInputFocused ? CP_INPUT_FOCUS_BD : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                    10, FALSE);
            }

            if (!IsRectEmpty(&s_rcComposerDock))
            {
                BikodeTheme_DrawRoundedPanel(hm, &s_rcComposerDock,
                    BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), BikodeTheme_GetColor(BKCLR_APP_BG), 226),
                    BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                    s_bInputFocused ? CP_INPUT_FOCUS_BD : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                    10, FALSE);
            }

            if (!IsRectEmpty(&s_rcComposerActionDock))
            {
                BikodeTheme_DrawRoundedPanel(hm, &s_rcComposerActionDock,
                    BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), BikodeTheme_GetColor(BKCLR_APP_BG), 220),
                    BikodeTheme_GetColor(BKCLR_STROKE_DARK),
                    BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
                    10, FALSE);
            }

            if (!IsRectEmpty(&s_rcComposerTag))
                DrawPromptDeckChip(hm, &s_rcComposerTag);

            rcMeta = s_rcComposerHint;
            SetBkMode(hm, TRANSPARENT);
            SetTextColor(hm, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
            SelectObject(hm, BikodeTheme_GetFont(BKFONT_UI_SMALL));
            DrawTextW(hm, L"Shift+Enter", -1, &rcMeta,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
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
                RECT rcBadge = pDIS->rcItem;
                RECT rcText = pDIS->rcItem;
                HFONT hOld;

                DrawButtonFace(pDIS->hDC, &pDIS->rcItem, s_bSendHover, down, TRUE,
                    BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));

                rcBadge.left += 8;
                rcBadge.right = rcBadge.left + 22;
                rcBadge.top += 5;
                rcBadge.bottom -= 5;
                FillRoundRectSolid(pDIS->hDC, &rcBadge, 7,
                    BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_STROKE_DARK), BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), 180));
                DrawSendArrow(pDIS->hDC,
                    (rcBadge.left + rcBadge.right) / 2,
                    (rcBadge.top + rcBadge.bottom) / 2,
                    BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));

                rcText.left = rcBadge.right + 9;
                rcText.right -= 10;
                SetBkMode(pDIS->hDC, TRANSPARENT);
                SetTextColor(pDIS->hDC, BikodeTheme_GetColor(BKCLR_STROKE_DARK));
                hOld = (HFONT)SelectObject(pDIS->hDC, BikodeTheme_GetFont(BKFONT_UI_BOLD));
                DrawTextW(pDIS->hDC, L"Send", -1, &rcText,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(pDIS->hDC, hOld);
                return TRUE;
            }
            if (pDIS->CtlID == IDC_CHAT_ATTACH)
            {
                BOOL down = ((pDIS->itemState & ODS_SELECTED) != 0) || s_bAttachDown;
                RECT rcBadge = pDIS->rcItem;
                RECT rcGlyph = pDIS->rcItem;
                COLORREF accent = BikodeTheme_GetColor(BKCLR_HOT_MAGENTA);
                COLORREF icon = (down || s_bAttachHover)
                    ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY)
                    : accent;
                DrawButtonFace(pDIS->hDC, &pDIS->rcItem, s_bAttachHover, down, FALSE,
                    accent);
                InflateRect(&rcBadge, -6, -6);
                FillRoundRectSolid(pDIS->hDC, &rcBadge, 6,
                    BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), s_bAttachHover ? 64 : 32));
                rcGlyph = rcBadge;
                InflateRect(&rcGlyph, -4, -4);
                BikodeTheme_DrawGlyph(pDIS->hDC, BKGLYPH_OPEN, &rcGlyph, icon, 2);
                return TRUE;
            }
            if (pDIS->CtlID == IDC_CHAT_SEARCH)
            {
                BOOL down = ((pDIS->itemState & ODS_SELECTED) != 0) || s_bSearchDown;
                RECT rcBadge = pDIS->rcItem;
                RECT rcGlyph = pDIS->rcItem;
                COLORREF accent = BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
                COLORREF icon = (down || s_bSearchHover)
                    ? BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY)
                    : accent;
                DrawButtonFace(pDIS->hDC, &pDIS->rcItem, s_bSearchHover, down, FALSE,
                    accent);
                InflateRect(&rcBadge, -6, -6);
                FillRoundRectSolid(pDIS->hDC, &rcBadge, 6,
                    BikodeTheme_Mix(accent, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), s_bSearchHover ? 64 : 32));
                rcGlyph = rcBadge;
                InflateRect(&rcGlyph, -4, -4);
                BikodeTheme_DrawGlyph(pDIS->hDC, BKGLYPH_SEARCH, &rcGlyph, icon, 2);
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
        UpdateHeaderHoverState(hwnd, &pt);
        break;
    }

    case WM_MOUSELEAVE:
        UpdateHeaderHoverState(hwnd, NULL);
        break;

    case WM_SETCURSOR:
    {
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (pt.x <= CHAT_SPLITTER_W) {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                return TRUE;
            }
            if (PtInRect(&s_rcCloseBtn, pt) || HitTestHeaderMode(pt) >= 0 ||
                (!IsRectEmpty(&s_rcHeaderAction) && PtInRect(&s_rcHeaderAction, pt)) ||
                (!IsRectEmpty(&s_rcComposerTag) && PtInRect(&s_rcComposerTag, pt))) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
        break;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        int modeIdx;
        if (pt.x <= CHAT_SPLITTER_W) {
            SetCapture(hwnd);
            return 0;
        }
        if (PtInRect(&s_rcCloseBtn, pt)) {
            ChatPanel_Hide();
            return 0;
        }
        modeIdx = HitTestHeaderMode(pt);
        if (modeIdx >= 0) {
            ApplyHeaderChatMode(kHeaderModes[modeIdx]);
            return 0;
        }
        if (!IsRectEmpty(&s_rcHeaderAction) && PtInRect(&s_rcHeaderAction, pt)) {
            TriggerHeaderAction();
            return 0;
        }
        if (!IsRectEmpty(&s_rcComposerTag) && PtInRect(&s_rcComposerTag, pt)) {
            TriggerPromptDeck();
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
                if (!IsHiddenStatusCardText(pszStatus))
                {
                    if (!s_bAIWorking)
                        StatusCard_Activate(pszStatus);
                    else
                        StatusCard_UpdateStatus(pszStatus);
                }
                free(pszStatus);
            }
            return 0;
        }
        if (msg == WM_AI_AGENT_TOOL) {
            char* pszTool = (char*)lParam;
            if (pszTool) {
                StatusCard_UpdateTool(pszTool);
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









