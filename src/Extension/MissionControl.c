/******************************************************************************
*
* Biko
*
* MissionControl.c
*   Dedicated Mission Control workspace UI.
*
******************************************************************************/

#include "MissionControl.h"
#include "MissionControlWebView.h"
#include "AgentRuntime.h"
#include "DarkMode.h"
#include "ProofTray.h"
#include "AIAgent.h"
#include "AICommands.h"
#include "MarkdownPreview.h"
#include "Scintilla.h"
#include "FileManager.h"
#include "Externals.h"
#include "ui/theme/BikodeTheme.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <strsafe.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define MC_MAX_ORGS 32

#define IDC_MC_ORG_COMBO       0xFC01
#define IDC_MC_RUN             0xFC02
#define IDC_MC_PAUSE           0xFC03
#define IDC_MC_CANCEL          0xFC04
#define IDC_MC_DUPLICATE       0xFC05
#define IDC_MC_ADD_NODE        0xFC06
#define IDC_MC_TOGGLE_VIEW     0xFC07
#define IDC_MC_HIDE_IDLE       0xFC08
#define IDC_MC_QUICK_CHAT      0xFC09
#define IDC_MC_BOARD           0xFC0A
#define IDC_MC_GRAPH           0xFC0B
#define IDC_MC_INSPECT_TABS    0xFC0C
#define IDC_MC_INSPECT_TEXT    0xFC0D
#define IDC_MC_ACTIVITY        0xFC0E
#define IDC_MC_OPEN_FILE       0xFC0F
#define IDC_MC_OPEN_WORKSPACE  0xFC10
#define IDC_MC_OPEN_TRANSCRIPT 0xFC11
#define IDC_MC_OPEN_PROOF      0xFC12
#define IDC_MC_FALLBACK        0xFC13
#define IDC_MC_PROJECT_PROMPT  0xFC14
#define IDC_MC_PROMPT_LABEL    0xFC15
#define WM_MC_DEFER_INIT       (WM_APP + 0x3A0)
#define WM_MC_REFRESH          (WM_APP + 0x3A1)
#define IDT_MC_GRAPH_READY     0xFC40
#define IDT_MC_REFRESH_COALESCE 0xFC41
#define IDM_MC_ADD_RESEARCH    0xFD20
#define IDM_MC_ADD_REVIEW      0xFD21
#define IDM_MC_ADD_VALIDATE    0xFD22

/* --- The Agency: Engineering Division --- */
#define IDM_MC_ADD_FRONTEND_DEV       0xFD30
#define IDM_MC_ADD_BACKEND_ARCHITECT  0xFD31
#define IDM_MC_ADD_SOFTWARE_ARCHITECT 0xFD32
#define IDM_MC_ADD_CODE_REVIEWER      0xFD33
#define IDM_MC_ADD_SECURITY_ENGINEER  0xFD34
#define IDM_MC_ADD_DEVOPS_AUTOMATOR   0xFD35
#define IDM_MC_ADD_DATABASE_OPTIMIZER 0xFD36
#define IDM_MC_ADD_RAPID_PROTOTYPER   0xFD37
#define IDM_MC_ADD_TECHNICAL_WRITER   0xFD38
#define IDM_MC_ADD_SRE                0xFD39
#define IDM_MC_ADD_GIT_WORKFLOW       0xFD3A
#define IDM_MC_ADD_INCIDENT_COMMANDER 0xFD3B
/* --- The Agency: Design Division --- */
#define IDM_MC_ADD_UX_ARCHITECT       0xFD40
#define IDM_MC_ADD_UI_DESIGNER        0xFD41
#define IDM_MC_ADD_UX_RESEARCHER      0xFD42
/* --- The Agency: Testing Division --- */
#define IDM_MC_ADD_PERF_BENCHMARKER   0xFD48
#define IDM_MC_ADD_ACCESSIBILITY      0xFD49
#define IDM_MC_ADD_API_TESTER         0xFD4A
#define IDM_MC_ADD_REALITY_CHECKER    0xFD4B
/* --- The Agency: Product Division --- */
#define IDM_MC_ADD_SPRINT_PRIORITIZER 0xFD50
#define IDM_MC_ADD_FEEDBACK_SYNTH     0xFD51
#define IDM_MC_ADD_TREND_RESEARCHER   0xFD52
/* --- The Agency: Orchestration --- */
#define IDM_MC_ADD_ORCHESTRATOR       0xFD58
/* --- Promptfoo: LLM Eval & Red Team --- */
#define IDM_MC_ADD_PROMPT_EVALUATOR   0xFD60
#define IDM_MC_ADD_RED_TEAMER         0xFD61
#define IDM_MC_ADD_MODEL_COMPARATOR   0xFD62
#define IDM_MC_ADD_REGRESSION_GUARD   0xFD63
/* --- Impeccable: Frontend Design Quality --- */
#define IDM_MC_ADD_DESIGN_AUDITOR     0xFD68
#define IDM_MC_ADD_DESIGN_CRITIC      0xFD69
#define IDM_MC_ADD_DESIGN_POLISHER    0xFD6A
#define IDM_MC_ADD_TYPOGRAPHY_EXPERT  0xFD6B
#define IDM_MC_ADD_COLOR_SPECIALIST   0xFD6C
#define IDM_MC_ADD_MOTION_DESIGNER    0xFD6D
/* --- OpenViking: Context & Memory Management --- */
#define IDM_MC_ADD_CONTEXT_ARCHITECT  0xFD70
#define IDM_MC_ADD_MEMORY_CURATOR     0xFD71
#define IDM_MC_ADD_RETRIEVAL_OPTIMIZER 0xFD72

/* SWE-Bench: Automated Issue Resolution (inspired by SWE-Agent, Princeton/Stanford) */
#define IDM_MC_ADD_ISSUE_TRIAGER      0xFD78
#define IDM_MC_ADD_BUG_REPRODUCER     0xFD79
#define IDM_MC_ADD_PATCH_WRITER       0xFD7A
#define IDM_MC_ADD_REGRESSION_TESTER  0xFD7B

/* Semgrep: Static Analysis & Security Scanning */
#define IDM_MC_ADD_SAST_SCANNER       0xFDA0
#define IDM_MC_ADD_VULN_FIXER         0xFDA1
#define IDM_MC_ADD_DEPENDENCY_AUDITOR 0xFDA2
#define IDM_MC_ADD_COMPLIANCE_CHECKER 0xFDA3

/* MetaGPT: Software Project Team Simulation */
#define IDM_MC_ADD_PRODUCT_MANAGER    0xFDA8
#define IDM_MC_ADD_TECH_LEAD          0xFDA9
#define IDM_MC_ADD_SENIOR_DEV         0xFDAA
#define IDM_MC_ADD_QA_ENGINEER        0xFDAB
#define IDM_MC_ADD_SCRUM_MASTER       0xFDAC

/* Aider: AI Pair Programming */
#define IDM_MC_ADD_REPO_NAVIGATOR     0xFDB0
#define IDM_MC_ADD_CODE_EDITOR        0xFDB1
#define IDM_MC_ADD_GIT_COMMITTER      0xFDB2
#define IDM_MC_ADD_TEST_RUNNER        0xFDB3

/* CrewAI: Collaborative Multi-Agent Tasks */
#define IDM_MC_ADD_TASK_COORDINATOR   0xFDB8
#define IDM_MC_ADD_DOMAIN_EXPERT      0xFDB9
#define IDM_MC_ADD_CODE_SPECIALIST    0xFDBA
#define IDM_MC_ADD_INTEGRATION_TESTER 0xFDBB
/* --- Backend toggle --- */
#define IDM_MC_TOGGLE_LOCAL_BACKEND   0xFD80

typedef struct MissionControlUi {
    HWND hwndMain;
    HWND hwndPanel;
    HWND hwndOrgCombo;
    HWND hwndRun;
    HWND hwndPause;
    HWND hwndCancel;
    HWND hwndDuplicate;
    HWND hwndAddNode;
    HWND hwndToggleView;
    HWND hwndHideIdle;
    HWND hwndQuickChat;
    HWND hwndBoard;
    HWND hwndGraphHost;
    HWND hwndFallback;
    HWND hwndInspectTabs;
    HWND hwndInspectText;
    HWND hwndOpenFile;
    HWND hwndOpenWorkspace;
    HWND hwndOpenTranscript;
    HWND hwndOpenProof;
    HWND hwndActivity;
    HWND hwndProjectPrompt;
    HWND hwndPromptLabel;
    BOOL visible;
    BOOL graphMode;
    BOOL hideIdle;
    BOOL registered;
    BOOL webViewInitRequested;
    BOOL webViewFaulted;
    BOOL isPopulating;
    BOOL inRefresh;
    BOOL refreshQueued;
    BOOL refreshTimerArmed;
    BOOL initScheduled;
    BOOL initComplete;
    int selectedNode;
    RECT rcHero;
    RECT rcBoardCard;
    RECT rcInspectorCard;
    RECT rcActivityCard;
    HBRUSH hbrAppBg;
    HBRUSH hbrSurfaceMain;
    HBRUSH hbrSurfaceRaised;
    int idleCount;
    int blockedCount;
    int runningCount;
    int doneCount;
    int warningCount;
    int visibleNodeCount;
    BOOL hasRun;
    BOOL isRunning;
    BOOL isPaused;
    BOOL hasProjectContext;
    BOOL orgLoadFailed;
    DWORD lastGraphTick;
    DWORD lastRefreshTick;
    DWORD lastRuntimeEventTick;
    WCHAR workspaceRoot[MAX_PATH];
    OrgSpec orgs[MC_MAX_ORGS];
    int orgCount;
} MissionControlUi;

typedef struct WideTextBuffer {
    WCHAR* data;
    size_t len;
    size_t cap;
} WideTextBuffer;

static MissionControlUi s_mc;
static BOOL s_bUseLocalBackend = FALSE;

/* --- Splitter drag state --- */
#define MC_SPLITTER_GRIP   5        /* pixels: hit-test band width */
#define MC_SPLITTER_NONE   0
#define MC_SPLITTER_HERO   1        /* bottom edge of COMMAND CENTER */
#define MC_SPLITTER_HBOARD 2        /* right edge of BOARD (side-by-side mode) */
#define MC_SPLITTER_VACT   3        /* top edge of TIMELINE */

static int  s_iDragSplitter = MC_SPLITTER_NONE;
static POINT s_ptDragStart;
static int  s_iDragOrigVal;          /* original size before drag began */

/* User-chosen persistent sizes (-1 = use automatic) */
static int  s_iUserHeroH     = -1;   /* hero card height */
static int  s_iUserBoardFrac = -1;   /* board width in side-by-side (px from left) */
static int  s_iUserActivityH = -1;   /* timeline card height */
extern WCHAR szCurFile[MAX_PATH + 40];

static LRESULT CALLBACK MissionControlProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static LRESULT MissionControlProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void ApplyTheme(void);
static void SelectBoardRow(int row);
static BOOL CreateDraftWorkflow(void);
static void ResizeBoardColumns(void);
static void RequestFullPanelRedraw(HWND hwnd);
static void QueueRefresh(void);
static const OrgNodeSpec* FindOrgNodeById(const OrgSpec* pOrg, const char* id);
static BOOL ShouldUseNativeMapFallback(const AgentRuntimeSnapshot* pSnapshot);
static void UpdateMapFallback(const AgentRuntimeSnapshot* pSnapshot);
static UINT ShowAddAgentMenu(HWND hwndAnchor);
static BOOL AddTemplatedNode(OrgSpec* pSpec, UINT commandId, WCHAR* wszAddedTitle, int cchAddedTitle);
static void EnsureWorkflowSupportPanelsVisible(LPCWSTR wszStatus);

static BOOL WideTextBuffer_Init(WideTextBuffer* pBuf, size_t cchInitial)
{
    if (!pBuf)
        return FALSE;
    ZeroMemory(pBuf, sizeof(*pBuf));
    pBuf->cap = cchInitial > 512 ? cchInitial : 512;
    pBuf->data = (WCHAR*)malloc(sizeof(WCHAR) * pBuf->cap);
    if (!pBuf->data)
    {
        pBuf->cap = 0;
        return FALSE;
    }
    pBuf->data[0] = L'\0';
    return TRUE;
}

static void WideTextBuffer_Free(WideTextBuffer* pBuf)
{
    if (!pBuf)
        return;
    if (pBuf->data)
        free(pBuf->data);
    ZeroMemory(pBuf, sizeof(*pBuf));
}

static BOOL WideTextBuffer_Reserve(WideTextBuffer* pBuf, size_t cchAdditional)
{
    WCHAR* grown;
    size_t needed;
    size_t nextCap;

    if (!pBuf || !pBuf->data)
        return FALSE;

    needed = pBuf->len + cchAdditional + 1;
    if (needed <= pBuf->cap)
        return TRUE;

    nextCap = pBuf->cap;
    while (nextCap < needed)
        nextCap *= 2;

    grown = (WCHAR*)realloc(pBuf->data, sizeof(WCHAR) * nextCap);
    if (!grown)
        return FALSE;

    pBuf->data = grown;
    pBuf->cap = nextCap;
    return TRUE;
}

static BOOL WideTextBuffer_AppendN(WideTextBuffer* pBuf, const WCHAR* wszText, size_t cchText)
{
    if (!pBuf || !pBuf->data || !wszText)
        return FALSE;
    if (!WideTextBuffer_Reserve(pBuf, cchText))
        return FALSE;
    memcpy(pBuf->data + pBuf->len, wszText, sizeof(WCHAR) * cchText);
    pBuf->len += cchText;
    pBuf->data[pBuf->len] = L'\0';
    return TRUE;
}

static BOOL WideTextBuffer_Append(WideTextBuffer* pBuf, const WCHAR* wszText)
{
    if (!wszText)
        return TRUE;
    return WideTextBuffer_AppendN(pBuf, wszText, lstrlenW(wszText));
}

static BOOL WideTextBuffer_AppendFormat(WideTextBuffer* pBuf, LPCWSTR wszFormat, ...)
{
    va_list args;
    int needed;

    if (!pBuf || !pBuf->data || !wszFormat)
        return FALSE;

    va_start(args, wszFormat);
    needed = _vscwprintf(wszFormat, args);
    va_end(args);
    if (needed < 0)
        return FALSE;
    if (!WideTextBuffer_Reserve(pBuf, (size_t)needed))
        return FALSE;

    va_start(args, wszFormat);
    if (vswprintf_s(pBuf->data + pBuf->len, pBuf->cap - pBuf->len, wszFormat, args) < 0)
    {
        va_end(args);
        return FALSE;
    }
    va_end(args);

    pBuf->len += (size_t)needed;
    return TRUE;
}

static BOOL WideTextBuffer_AppendEscapedHtml(WideTextBuffer* pBuf, const WCHAR* wszText)
{
    const WCHAR* p;

    if (!wszText)
        return TRUE;

    for (p = wszText; *p; ++p)
    {
        switch (*p)
        {
        case L'&':
            if (!WideTextBuffer_Append(pBuf, L"&amp;"))
                return FALSE;
            break;
        case L'<':
            if (!WideTextBuffer_Append(pBuf, L"&lt;"))
                return FALSE;
            break;
        case L'>':
            if (!WideTextBuffer_Append(pBuf, L"&gt;"))
                return FALSE;
            break;
        case L'"':
            if (!WideTextBuffer_Append(pBuf, L"&quot;"))
                return FALSE;
            break;
        case L'\r':
            break;
        case L'\n':
            if (!WideTextBuffer_Append(pBuf, L"<br/>"))
                return FALSE;
            break;
        default:
            if (!WideTextBuffer_AppendN(pBuf, p, 1))
                return FALSE;
            break;
        }
    }
    return TRUE;
}

static BOOL ResolveProjectRoot(WCHAR* wszOut, int cchOut)
{
    const WCHAR* root = FileManager_GetRootPath();
    if (!wszOut || cchOut <= 0)
        return FALSE;
    wszOut[0] = L'\0';
    if (root && root[0])
    {
        lstrcpynW(wszOut, root, cchOut);
        return TRUE;
    }
    if (szCurFile[0])
    {
        lstrcpynW(wszOut, szCurFile, cchOut);
        PathRemoveFileSpecW(wszOut);
        return wszOut[0] != L'\0';
    }
    return FALSE;
}

static void EnsureWorkflowSupportPanelsVisible(LPCWSTR wszStatus)
{
    const WCHAR* wszRoot = s_mc.workspaceRoot[0] ? s_mc.workspaceRoot : AgentRuntime_GetWorkspaceRoot();

    if (wszRoot && wszRoot[0] &&
        (!FileManager_IsVisible() || _wcsicmp(FileManager_GetRootPath(), wszRoot) != 0))
    {
        FileManager_OpenFolder(wszRoot);
    }

    if (wszStatus && wszStatus[0])
        ProofTray_SetMissionStatus(wszStatus);
    if (!ProofTray_IsVisible())
        ProofTray_Show(s_mc.hwndMain);
}

static void RequestFullPanelRedraw(HWND hwnd)
{
    if (!hwnd)
        return;
    if (s_iDragSplitter)
    {
        /* During drag: skip erase to prevent flicker, just invalidate and
         * update synchronously so the layout feels responsive */
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    else
    {
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

static void QueueRefresh(void)
{
    DWORD now;
    if (!s_mc.hwndPanel || !s_mc.visible)
        return;
    if (s_mc.refreshQueued)
        return;
    now = GetTickCount();
    if (s_mc.isRunning && s_mc.lastRefreshTick && (now - s_mc.lastRefreshTick) < 140)
    {
        if (!s_mc.refreshTimerArmed)
        {
            s_mc.refreshTimerArmed = TRUE;
            SetTimer(s_mc.hwndPanel, IDT_MC_REFRESH_COALESCE, 120, NULL);
        }
        return;
    }
    s_mc.refreshQueued = TRUE;
    PostMessageW(s_mc.hwndPanel, WM_MC_REFRESH, 0, 0);
}

static const OrgNodeSpec* FindOrgNodeById(const OrgSpec* pOrg, const char* id)
{
    int i;
    if (!pOrg || !id || !id[0])
        return NULL;
    for (i = 0; i < pOrg->nodeCount; i++)
    {
        if (lstrcmpiA(pOrg->nodes[i].id, id) == 0)
            return &pOrg->nodes[i];
    }
    return NULL;
}

static void DeleteBrushSafe(HBRUSH* phBrush)
{
    if (phBrush && *phBrush)
    {
        DeleteObject(*phBrush);
        *phBrush = NULL;
    }
}

static void RebuildThemeBrushes(void)
{
    DeleteBrushSafe(&s_mc.hbrAppBg);
    DeleteBrushSafe(&s_mc.hbrSurfaceMain);
    DeleteBrushSafe(&s_mc.hbrSurfaceRaised);
    s_mc.hbrAppBg = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_APP_BG));
    s_mc.hbrSurfaceMain = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN));
    s_mc.hbrSurfaceRaised = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SURFACE_RAISED));
}

static void ColorToHtml(COLORREF color, WCHAR* wszOut, size_t cchOut)
{
    if (!wszOut || cchOut == 0)
        return;
    StringCchPrintfW(wszOut, cchOut, L"#%02X%02X%02X",
        GetRValue(color), GetGValue(color), GetBValue(color));
}

static COLORREF GetStateAccent(AgentNodeState state)
{
    switch (state)
    {
    case AGENT_NODE_RUNNING:
        return BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
    case AGENT_NODE_DONE:
        return BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN);
    case AGENT_NODE_QUEUED:
    case AGENT_NODE_BLOCKED:
    case AGENT_NODE_PAUSED:
        return BikodeTheme_GetColor(BKCLR_WARNING_ORANGE);
    case AGENT_NODE_ERROR:
    case AGENT_NODE_CANCELED:
        return BikodeTheme_GetColor(BKCLR_DANGER_RED);
    default:
        return BikodeTheme_GetColor(BKCLR_TEXT_MUTED);
    }
}

static const WCHAR* GetEventTypeLabel(AgentEventType type)
{
    switch (type)
    {
    case AGENT_EVENT_STATUS:
        return L"Status";
    case AGENT_EVENT_TOOL:
        return L"Tool";
    case AGENT_EVENT_FILE:
        return L"File";
    case AGENT_EVENT_HANDOFF:
        return L"Handoff";
    case AGENT_EVENT_ERROR:
        return L"Error";
    case AGENT_EVENT_METRIC:
        return L"Metric";
    default:
        return L"System";
    }
}

static void SetThemeFont(HWND hwnd, BikodeFontRole role)
{
    if (hwnd)
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)BikodeTheme_GetFont(role), TRUE);
}

static void ResetHandles(void)
{
    s_mc.hwndPanel = NULL;
    s_mc.hwndOrgCombo = NULL;
    s_mc.hwndRun = NULL;
    s_mc.hwndPause = NULL;
    s_mc.hwndCancel = NULL;
    s_mc.hwndDuplicate = NULL;
    s_mc.hwndAddNode = NULL;
    s_mc.hwndToggleView = NULL;
    s_mc.hwndHideIdle = NULL;
    s_mc.hwndQuickChat = NULL;
    s_mc.hwndBoard = NULL;
    s_mc.hwndGraphHost = NULL;
    s_mc.hwndFallback = NULL;
    s_mc.hwndInspectTabs = NULL;
    s_mc.hwndInspectText = NULL;
    s_mc.hwndOpenFile = NULL;
    s_mc.hwndOpenWorkspace = NULL;
    s_mc.hwndOpenTranscript = NULL;
    s_mc.hwndOpenProof = NULL;
    s_mc.hwndActivity = NULL;
    s_mc.hwndProjectPrompt = NULL;
    s_mc.hwndPromptLabel = NULL;
    s_mc.webViewInitRequested = FALSE;
    s_mc.webViewFaulted = FALSE;
    s_mc.isPopulating = FALSE;
    s_mc.inRefresh = FALSE;
    s_mc.refreshQueued = FALSE;
    s_mc.refreshTimerArmed = FALSE;
    s_mc.initScheduled = FALSE;
    s_mc.initComplete = FALSE;
    s_mc.lastGraphTick = 0;
    s_mc.lastRefreshTick = 0;
    s_mc.lastRuntimeEventTick = 0;
    s_mc.selectedNode = -1;
    SetRectEmpty(&s_mc.rcHero);
    SetRectEmpty(&s_mc.rcBoardCard);
    SetRectEmpty(&s_mc.rcInspectorCard);
    SetRectEmpty(&s_mc.rcActivityCard);
    DeleteBrushSafe(&s_mc.hbrAppBg);
    DeleteBrushSafe(&s_mc.hbrSurfaceMain);
    DeleteBrushSafe(&s_mc.hbrSurfaceRaised);
    s_mc.idleCount = 0;
    s_mc.blockedCount = 0;
    s_mc.runningCount = 0;
    s_mc.doneCount = 0;
    s_mc.warningCount = 0;
    s_mc.visibleNodeCount = 0;
    s_mc.hasRun = FALSE;
    s_mc.isRunning = FALSE;
    s_mc.isPaused = FALSE;
    s_mc.hasProjectContext = FALSE;
    s_mc.orgLoadFailed = FALSE;
    s_mc.workspaceRoot[0] = L'\0';
    s_mc.orgCount = 0;
}

static AgentRuntimeSnapshot* AllocSnapshot(void)
{
    AgentRuntimeSnapshot* snapshot = (AgentRuntimeSnapshot*)malloc(sizeof(AgentRuntimeSnapshot));
    if (!snapshot)
        return NULL;
    AgentRuntime_GetSnapshot(snapshot);
    return snapshot;
}

static void FreeSnapshot(AgentRuntimeSnapshot* snapshot)
{
    if (snapshot)
        free(snapshot);
}

static void Utf8ToWideSafe(const char* text, WCHAR* wszOut, int cchOut)
{
    if (!wszOut || cchOut <= 0)
        return;
    wszOut[0] = L'\0';
    if (text && text[0])
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wszOut, cchOut);
}

static void SetChildFont(HWND hwnd)
{
    SetThemeFont(hwnd, BKFONT_UI);
}

static void RecoverFromPanelFailure(HWND hwnd, UINT msg)
{
    s_mc.visible = FALSE;
    s_mc.initScheduled = FALSE;
    s_mc.initComplete = FALSE;
    OutputDebugStringW(L"MissionControl: recovered from an exception inside the panel callback.\r\n");
    if (hwnd && msg != WM_CREATE && msg != WM_DESTROY)
        ShowWindow(hwnd, SW_HIDE);
    if (s_mc.hwndMain)
    {
        RECT rc;
        GetClientRect(s_mc.hwndMain, &rc);
        PostMessageW(s_mc.hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    }
}

static char* ReadUtf8FileLocal(LPCWSTR wszPath)
{
    HANDLE hFile;
    LARGE_INTEGER size;
    DWORD read = 0;
    char* buffer;
    hFile = CreateFileW(wszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(hFile, &size) || size.HighPart != 0 || size.LowPart > (4 * 1024 * 1024))
    {
        CloseHandle(hFile);
        return NULL;
    }
    buffer = (char*)malloc(size.LowPart + 1);
    if (!buffer)
    {
        CloseHandle(hFile);
        return NULL;
    }
    if (size.LowPart > 0 && !ReadFile(hFile, buffer, size.LowPart, &read, NULL))
    {
        free(buffer);
        CloseHandle(hFile);
        return NULL;
    }
    buffer[read] = '\0';
    CloseHandle(hFile);
    return buffer;
}

static void EnsurePanel(void)
{
    WNDCLASSW wc;
    if (s_mc.hwndPanel)
        return;
    if (!s_mc.registered)
    {
        ZeroMemory(&wc, sizeof(wc));
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MissionControlProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"BikoMissionControl";
        RegisterClassW(&wc);
        s_mc.registered = TRUE;
    }

    s_mc.hwndPanel = CreateWindowExW(0, L"BikoMissionControl", L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        s_mc.hwndMain, NULL, GetModuleHandleW(NULL), NULL);
}

static int GetSelectedOrgIndex(void)
{
    if (!s_mc.hwndOrgCombo)
        return -1;
    return (int)SendMessageW(s_mc.hwndOrgCombo, CB_GETCURSEL, 0, 0);
}

static OrgSpec* GetSelectedOrg(void)
{
    int index = GetSelectedOrgIndex();
    if (index < 0 || index >= s_mc.orgCount)
        return NULL;
    return &s_mc.orgs[index];
}

static void LoadOrgs(void)
{
    WCHAR wszLast[MAX_PATH];
    int selected = -1;
    s_mc.orgCount = 0;
    s_mc.orgLoadFailed = FALSE;
    s_mc.hasProjectContext = ResolveProjectRoot(s_mc.workspaceRoot, ARRAYSIZE(s_mc.workspaceRoot));
    if (s_mc.hwndOrgCombo)
    {
        SendMessageW(s_mc.hwndOrgCombo, CB_RESETCONTENT, 0, 0);
        EnableWindow(s_mc.hwndOrgCombo, s_mc.hasProjectContext);
    }
    if (!s_mc.hasProjectContext)
    {
        if (s_mc.hwndOrgCombo)
        {
            SendMessageW(s_mc.hwndOrgCombo, CB_ADDSTRING, 0, (LPARAM)L"Open a project folder first");
            SendMessageW(s_mc.hwndOrgCombo, CB_SETCURSEL, 0, 0);
        }
        return;
    }
    if (!AgentRuntime_LoadOrgSpecs(s_mc.workspaceRoot, s_mc.orgs, MC_MAX_ORGS, &s_mc.orgCount))
        s_mc.orgLoadFailed = TRUE;
    if (s_mc.hwndOrgCombo)
    {
        for (int i = 0; i < s_mc.orgCount; i++)
        {
            WCHAR wszName[128];
            Utf8ToWideSafe(s_mc.orgs[i].name, wszName, ARRAYSIZE(wszName));
            SendMessageW(s_mc.hwndOrgCombo, CB_ADDSTRING, 0, (LPARAM)wszName);
        }
        if (s_mc.orgCount <= 0)
        {
            SendMessageW(s_mc.hwndOrgCombo, CB_ADDSTRING, 0,
                (LPARAM)(s_mc.orgLoadFailed ? L"Starter workflows unavailable" : L"No workflows found"));
        }
        if (AgentRuntime_GetLastSelectedOrg(wszLast, ARRAYSIZE(wszLast)))
        {
            for (int i = 0; i < s_mc.orgCount; i++)
            {
                if (_wcsicmp(wszLast, s_mc.orgs[i].path) == 0)
                {
                    selected = i;
                    break;
                }
            }
        }
        if (selected < 0 && s_mc.orgCount > 0)
            selected = 0;
        SendMessageW(s_mc.hwndOrgCombo, CB_SETCURSEL, selected >= 0 ? selected : 0, 0);
    }
}

static void OpenEditorPath(LPCWSTR wszPath)
{
    WCHAR* wszCopy;
    size_t cb;
    if (!wszPath || !wszPath[0])
        return;
    cb = (size_t)(lstrlenW(wszPath) + 1) * sizeof(WCHAR);
    wszCopy = (WCHAR*)malloc(cb);
    if (!wszCopy)
        return;
    memcpy(wszCopy, wszPath, cb);
    PostMessageW(s_mc.hwndMain, WM_AI_OPEN_FILE, 0, (LPARAM)wszCopy);
}

static void ResizeBoardColumns(void)
{
    RECT rc;
    int width;
    int statusW = 78;
    int agentW = 136;
    int setupW = 112;
    int toolsW = 48;
    int filesW = 48;
    int timeW = 60;
    int actionW;
    if (!s_mc.hwndBoard || ListView_GetColumnWidth(s_mc.hwndBoard, 0) == 0)
        return;
    GetClientRect(s_mc.hwndBoard, &rc);
    width = max((rc.right - rc.left) - GetSystemMetrics(SM_CXVSCROLL) - 6, 420);
    actionW = width - statusW - agentW - setupW - toolsW - filesW - timeW;
    if (actionW < 180)
    {
        actionW = 180;
        agentW = 120;
        setupW = 96;
    }
    ListView_SetColumnWidth(s_mc.hwndBoard, 0, statusW);
    ListView_SetColumnWidth(s_mc.hwndBoard, 1, agentW);
    ListView_SetColumnWidth(s_mc.hwndBoard, 2, setupW);
    ListView_SetColumnWidth(s_mc.hwndBoard, 3, actionW);
    ListView_SetColumnWidth(s_mc.hwndBoard, 4, toolsW);
    ListView_SetColumnWidth(s_mc.hwndBoard, 5, filesW);
    ListView_SetColumnWidth(s_mc.hwndBoard, 6, timeW);
}

static void UpdateSnapshotSummary(const AgentRuntimeSnapshot* pSnapshot)
{
    s_mc.idleCount = 0;
    s_mc.blockedCount = 0;
    s_mc.runningCount = 0;
    s_mc.doneCount = 0;
    s_mc.warningCount = 0;
    s_mc.visibleNodeCount = 0;
    s_mc.hasRun = FALSE;
    s_mc.isRunning = FALSE;
    s_mc.isPaused = FALSE;
    if (!pSnapshot)
        return;

    s_mc.hasRun = pSnapshot->hasRun;
    s_mc.isRunning = pSnapshot->isRunning;
    s_mc.isPaused = pSnapshot->isPaused;
    for (int i = 0; i < pSnapshot->nodeCount; i++)
    {
        const AgentNodeSnapshot* node = &pSnapshot->nodes[i];
        if (!s_mc.hideIdle || node->state != AGENT_NODE_IDLE)
            s_mc.visibleNodeCount++;
        switch (node->state)
        {
        case AGENT_NODE_IDLE:
            s_mc.idleCount++;
            break;
        case AGENT_NODE_RUNNING:
            s_mc.runningCount++;
            break;
        case AGENT_NODE_DONE:
            s_mc.doneCount++;
            break;
        case AGENT_NODE_QUEUED:
        case AGENT_NODE_BLOCKED:
        case AGENT_NODE_PAUSED:
            s_mc.blockedCount++;
            break;
        case AGENT_NODE_ERROR:
        case AGENT_NODE_CANCELED:
            s_mc.warningCount++;
            break;
        default:
            break;
        }
    }
}

static void PopulateBoard(const AgentRuntimeSnapshot* pSnapshot)
{
    LVCOLUMNW col;
    int selectedRow = -1;
    int fallbackRow = -1;
    s_mc.isPopulating = TRUE;
    ListView_DeleteAllItems(s_mc.hwndBoard);
    if (!pSnapshot)
    {
        s_mc.isPopulating = FALSE;
        return;
    }
    if (ListView_GetColumnWidth(s_mc.hwndBoard, 0) == 0)
    {
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = L"Status"; col.cx = 92; ListView_InsertColumn(s_mc.hwndBoard, 0, &col);
        col.pszText = L"Agent"; col.cx = 152; ListView_InsertColumn(s_mc.hwndBoard, 1, &col);
        col.pszText = L"Source"; col.cx = 82; ListView_InsertColumn(s_mc.hwndBoard, 2, &col);
        col.pszText = L"Setup"; col.cx = 120; ListView_InsertColumn(s_mc.hwndBoard, 3, &col);
        col.pszText = L"What it's doing"; col.cx = 240; ListView_InsertColumn(s_mc.hwndBoard, 4, &col);
        col.pszText = L"Tools"; col.cx = 48; ListView_InsertColumn(s_mc.hwndBoard, 5, &col);
        col.pszText = L"Files"; col.cx = 48; ListView_InsertColumn(s_mc.hwndBoard, 6, &col);
        col.pszText = L"Time"; col.cx = 58; ListView_InsertColumn(s_mc.hwndBoard, 7, &col);
    }
    ResizeBoardColumns();
    for (int i = 0; i < pSnapshot->nodeCount; i++)
    {
        const AgentNodeSnapshot* node = &pSnapshot->nodes[i];
        WCHAR wszText[512];
        WCHAR wszState[64];
        WCHAR wszSetup[128];
        WCHAR wszAction[256];
        LVITEMW item;
        DWORD endTick = node->endTick ? node->endTick : GetTickCount();
        DWORD elapsed = node->startTick ? (endTick - node->startTick) / 1000 : 0;
        if (s_mc.hideIdle && node->state == AGENT_NODE_IDLE)
            continue;
        Utf8ToWideSafe(AgentRuntime_StateLabel(node->state), wszState, ARRAYSIZE(wszState));
        StringCchPrintfW(wszSetup, ARRAYSIZE(wszSetup), L"%S / %S",
            AgentRuntime_BackendLabel(node->backend),
            AgentRuntime_WorkspaceLabel(node->workspacePolicy));
        Utf8ToWideSafe(node->lastAction, wszAction, ARRAYSIZE(wszAction));
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(s_mc.hwndBoard);
        item.pszText = wszState;
        item.lParam = i;
        ListView_InsertItem(s_mc.hwndBoard, &item);
        Utf8ToWideSafe(node->title, wszText, ARRAYSIZE(wszText));
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 1, wszText);
        Utf8ToWideSafe(node->group[0] ? node->group : "—", wszText, ARRAYSIZE(wszText));
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 2, wszText);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 3, wszSetup);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 4, wszAction);
        swprintf_s(wszText, ARRAYSIZE(wszText), L"%d", node->toolCount);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 5, wszText);
        swprintf_s(wszText, ARRAYSIZE(wszText), L"%d", node->fileCount);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 6, wszText);
        swprintf_s(wszText, ARRAYSIZE(wszText), L"%lus", (unsigned long)elapsed);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 7, wszText);
        if (i == s_mc.selectedNode)
            selectedRow = item.iItem;
        if (fallbackRow < 0)
            fallbackRow = item.iItem;
    }
    if (selectedRow < 0 && fallbackRow >= 0)
    {
        SelectBoardRow(fallbackRow);
        selectedRow = fallbackRow;
    }
    if (selectedRow >= 0)
    {
        ListView_SetItemState(s_mc.hwndBoard, selectedRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    else
    {
        s_mc.selectedNode = -1;
    }
    s_mc.isPopulating = FALSE;
}

static void BuildInspectorText(const AgentRuntimeSnapshot* pSnapshot, int nodeIndex, int tabIndex, WCHAR* wszOut, int cchOut)
{
    const AgentNodeSnapshot* node;
    if (!wszOut || cchOut <= 0)
        return;
    wszOut[0] = L'\0';
    if (!s_mc.hasProjectContext)
    {
        lstrcpynW(wszOut,
            L"Open a project folder to use Command Center.\r\n\r\n"
            L"Then choose a saved workflow and click Run Workflow.",
            cchOut);
        return;
    }
    if (s_mc.orgCount <= 0)
    {
        lstrcpynW(wszOut,
            L"Create a workflow first, then Command Center will show each agent here.\r\n\r\n"
            L"Use Create Workflow first, then add more agents as needed.",
            cchOut);
        return;
    }
    if (!pSnapshot || nodeIndex < 0 || nodeIndex >= pSnapshot->nodeCount)
    {
        lstrcpynW(wszOut,
            L"Select an agent on the left to inspect its summary, transcript, touched files, and review notes.\r\n\r\n"
            L"The board is for taking action. The inspector is where you understand what happened.",
            cchOut);
        return;
    }
    node = &pSnapshot->nodes[nodeIndex];
    switch (tabIndex)
    {
    case 0:
        swprintf_s(wszOut, cchOut,
            L"%S\r\n\r\nRole: %S\r\nSource: %S\r\nState: %S\r\nBackend: %S\r\nWorkspace: %S\r\nTools used: %d\r\nFiles touched: %d\r\n\r\nCurrent focus:\r\n%S\r\n\r\nSummary:\r\n%S",
            node->title,
            node->role[0] ? node->role : "agent",
            node->group[0] ? node->group : "custom",
            AgentRuntime_StateLabel(node->state),
            AgentRuntime_BackendLabel(node->backend),
            AgentRuntime_WorkspaceLabel(node->workspacePolicy),
            node->toolCount,
            node->fileCount,
            node->lastAction,
            node->summary);
        break;
    case 1:
        if (node->transcriptPath[0] && GetFileAttributesW(node->transcriptPath) != INVALID_FILE_ATTRIBUTES)
        {
            char* text = ReadUtf8FileLocal(node->transcriptPath);
            if (text)
            {
                Utf8ToWideSafe(text, wszOut, cchOut);
                free(text);
                break;
            }
        }
        lstrcpynW(wszOut, L"This agent has not produced a transcript yet.", cchOut);
        break;
    case 2:
        if (node->fileCount <= 0)
        {
            lstrcpynW(wszOut, L"No file changes have been recorded yet.", cchOut);
            break;
        }
        for (int i = 0; i < min(node->fileCount, AGENT_RUNTIME_MAX_CHANGED_FILES); i++)
        {
            StringCchCatW(wszOut, cchOut, node->changedFiles[i]);
            StringCchCatW(wszOut, cchOut, L"\r\n");
        }
        break;
    case 3:
        swprintf_s(wszOut, cchOut, L"Review summary:\r\n%S\r\n\r\nWorkspace:\r\n%s", node->summary, node->workspacePath);
        break;
    default:
        swprintf_s(wszOut, cchOut,
            L"Tool calls: %d\r\nFiles touched: %d\r\nInput tokens: %d\r\nOutput tokens: %d\r\nWorkspace:\r\n%s",
            node->toolCount, node->fileCount, node->inputTokens, node->outputTokens, node->workspacePath);
        break;
    }
}

static void UpdateInspector(const AgentRuntimeSnapshot* pSnapshot)
{
    WCHAR wszText[16384];
    char utf8Buf[49152];
    int tabIndex = 0;
    int len;
    if (!s_mc.hwndInspectText || !s_mc.hwndInspectTabs)
        return;
    tabIndex = TabCtrl_GetCurSel(s_mc.hwndInspectTabs);
    BuildInspectorText(pSnapshot, s_mc.selectedNode, tabIndex, wszText, ARRAYSIZE(wszText));
    /* Convert wide text to UTF-8 for Scintilla */
    len = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8Buf, sizeof(utf8Buf) - 1, NULL, NULL);
    if (len <= 0) { utf8Buf[0] = '\0'; len = 1; }
    utf8Buf[sizeof(utf8Buf) - 1] = '\0';
    len = lstrlenA(utf8Buf);
    SendMessage(s_mc.hwndInspectText, SCI_SETREADONLY, FALSE, 0);
    SendMessage(s_mc.hwndInspectText, SCI_CLEARALL, 0, 0);
    SendMessage(s_mc.hwndInspectText, SCI_ADDTEXT, (WPARAM)len, (LPARAM)utf8Buf);
    /* Apply markdown styling for Summary and Transcript tabs */
    if (tabIndex == 0 || tabIndex == 1 || tabIndex == 3)
        MarkdownPreview_StyleRange(s_mc.hwndInspectText, 0, len, utf8Buf);
    SendMessage(s_mc.hwndInspectText, SCI_SETREADONLY, TRUE, 0);
    SendMessage(s_mc.hwndInspectText, SCI_GOTOPOS, 0, 0);
}

static void PopulateActivity(const AgentRuntimeSnapshot* pSnapshot)
{
    WCHAR wszRow[1024];
    SendMessageW(s_mc.hwndActivity, LB_RESETCONTENT, 0, 0);
    if (!s_mc.hasProjectContext)
    {
        SendMessageW(s_mc.hwndActivity, LB_ADDSTRING, 0,
            (LPARAM)L"Open a project folder, choose a workflow, then click Run Workflow.");
        return;
    }
    if (s_mc.orgCount <= 0)
    {
        SendMessageW(s_mc.hwndActivity, LB_ADDSTRING, 0,
            (LPARAM)L"Click Create Workflow to make your first workflow.");
        return;
    }
    if (!pSnapshot)
        return;
    if (pSnapshot->eventCount <= 0)
    {
        SendMessageW(s_mc.hwndActivity, LB_ADDSTRING, 0,
            (LPARAM)L"Choose a workflow from the dropdown, then click Run Workflow.");
        return;
    }
    for (int i = 0; i < pSnapshot->eventCount; i++)
    {
        const AgentEvent* ev = &pSnapshot->events[i];
        StringCchCopyW(wszRow, ARRAYSIZE(wszRow), GetEventTypeLabel(ev->type));
        StringCchCatW(wszRow, ARRAYSIZE(wszRow), L"  ");
        Utf8ToWideSafe(ev->nodeTitle[0] ? ev->nodeTitle : ev->nodeId,
            wszRow + lstrlenW(wszRow), ARRAYSIZE(wszRow) - lstrlenW(wszRow));
        StringCchCatW(wszRow, ARRAYSIZE(wszRow), L"  ");
        Utf8ToWideSafe(ev->message, wszRow + lstrlenW(wszRow), ARRAYSIZE(wszRow) - lstrlenW(wszRow));
        SendMessageW(s_mc.hwndActivity, LB_ADDSTRING, 0, (LPARAM)wszRow);
    }
    if (pSnapshot->eventCount > 0)
        SendMessageW(s_mc.hwndActivity, LB_SETTOPINDEX, pSnapshot->eventCount - 1, 0);
}

static BOOL ShouldUseNativeMapFallback(const AgentRuntimeSnapshot* pSnapshot)
{
    if (!pSnapshot)
        return TRUE;
    if (pSnapshot->isRunning)
        return TRUE;
    if (s_mc.webViewFaulted)
        return TRUE;
    if (s_mc.lastRuntimeEventTick && (GetTickCount() - s_mc.lastRuntimeEventTick) < 1600)
        return TRUE;
    return MissionControlWebView_IsReady() ? FALSE : TRUE;
}

static void UpdateMapFallback(const AgentRuntimeSnapshot* pSnapshot)
{
    WCHAR wszText[12288];
    WCHAR wszLine[768];
    int i;

    if (!s_mc.hwndFallback)
        return;

    wszText[0] = L'\0';
    if (!pSnapshot || pSnapshot->nodeCount <= 0)
    {
        StringCchCopyW(wszText, ARRAYSIZE(wszText),
            L"Workflow Map\r\n\r\nPick a workflow and click Run Workflow to see the team structure here.");
        SetWindowTextW(s_mc.hwndFallback, wszText);
        return;
    }

    if (pSnapshot->isRunning)
    {
        StringCchCopyW(wszText, ARRAYSIZE(wszText),
            L"Workflow Map\r\n\r\nLive runs stay calm in this view. Use Board View for per-agent detail while the team is working.\r\n\r\n");
    }
    else
    {
        StringCchCopyW(wszText, ARRAYSIZE(wszText),
            L"Workflow Map\r\n\r\nStable snapshot of the selected workflow.\r\n\r\n");
    }

    for (i = 0; i < pSnapshot->nodeCount; i++)
    {
        const AgentNodeSnapshot* node = &pSnapshot->nodes[i];
        WCHAR wszAction[200];
        Utf8ToWideSafe(node->summary[0] ? node->summary : node->lastAction, wszAction, ARRAYSIZE(wszAction));

        StringCchPrintfW(wszLine, ARRAYSIZE(wszLine), L"%d. %S  [%S]\r\n", i + 1, node->title, AgentRuntime_StateLabel(node->state));
        StringCchCatW(wszText, ARRAYSIZE(wszText), wszLine);

        StringCchPrintfW(wszLine, ARRAYSIZE(wszLine), L"   %S | %S\r\n",
            AgentRuntime_BackendLabel(node->backend),
            AgentRuntime_WorkspaceLabel(node->workspacePolicy));
        StringCchCatW(wszText, ARRAYSIZE(wszText), wszLine);

        if (i < pSnapshot->org.nodeCount && pSnapshot->org.nodes[i].dependsOnCount > 0)
        {
            int dep;
            StringCchCopyW(wszLine, ARRAYSIZE(wszLine), L"   Waits for: ");
            for (dep = 0; dep < pSnapshot->org.nodes[i].dependsOnCount; dep++)
            {
                const OrgNodeSpec* depNode = FindOrgNodeById(&pSnapshot->org, pSnapshot->org.nodes[i].dependsOn[dep]);
                if (dep > 0)
                    StringCchCatW(wszLine, ARRAYSIZE(wszLine), L", ");
                if (depNode)
                {
                    WCHAR wszDepTitle[96];
                    Utf8ToWideSafe(depNode->title, wszDepTitle, ARRAYSIZE(wszDepTitle));
                    StringCchCatW(wszLine, ARRAYSIZE(wszLine), wszDepTitle);
                }
                else
                {
                    WCHAR wszDepId[64];
                    Utf8ToWideSafe(pSnapshot->org.nodes[i].dependsOn[dep], wszDepId, ARRAYSIZE(wszDepId));
                    StringCchCatW(wszLine, ARRAYSIZE(wszLine), wszDepId);
                }
            }
            StringCchCatW(wszLine, ARRAYSIZE(wszLine), L"\r\n");
            StringCchCatW(wszText, ARRAYSIZE(wszText), wszLine);
        }

        if (wszAction[0])
        {
            StringCchPrintfW(wszLine, ARRAYSIZE(wszLine), L"   Focus: %s\r\n", wszAction);
            StringCchCatW(wszText, ARRAYSIZE(wszText), wszLine);
        }
        StringCchCatW(wszText, ARRAYSIZE(wszText), L"\r\n");
    }

    SetWindowTextW(s_mc.hwndFallback, wszText);
}

static void UpdateGraph(const AgentRuntimeSnapshot* pSnapshot)
{
    WideTextBuffer html;
    WCHAR wszAppBg[16];
    WCHAR wszCardBg[16];
    WCHAR wszText[16];
    WCHAR wszMuted[16];
    WCHAR wszBorder[16];
    WCHAR wszAccent[16];
    if (!pSnapshot || !s_mc.hwndGraphHost)
        return;
    if (pSnapshot->isRunning)
        return;
    if (s_mc.webViewFaulted)
        return;
    if (s_mc.lastRuntimeEventTick && (GetTickCount() - s_mc.lastRuntimeEventTick) < 1600)
        return;
    if (!s_mc.webViewInitRequested)
    {
        __try
        {
            s_mc.webViewInitRequested = TRUE;
            MissionControlWebView_Initialize(s_mc.hwndGraphHost);
            if (s_mc.hwndPanel)
                SetTimer(s_mc.hwndPanel, IDT_MC_GRAPH_READY, 200, NULL);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s_mc.webViewFaulted = TRUE;
        }
        return;
    }
    if (!MissionControlWebView_IsReady())
        return;
    if ((GetTickCount() - s_mc.lastGraphTick) < 600)
        return;
    if (!WideTextBuffer_Init(&html, 8192))
        return;
    ColorToHtml(BikodeTheme_GetColor(BKCLR_APP_BG), wszAppBg, ARRAYSIZE(wszAppBg));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), wszCardBg, ARRAYSIZE(wszCardBg));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), wszText, ARRAYSIZE(wszText));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY), wszMuted, ARRAYSIZE(wszMuted));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_STROKE_SOFT), wszBorder, ARRAYSIZE(wszBorder));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), wszAccent, ARRAYSIZE(wszAccent));
    if (!WideTextBuffer_AppendFormat(&html,
        L"<html><head><meta charset='utf-8'><style>"
        L"body{margin:0;font-family:'Segoe UI',sans-serif;background:%s;color:%s;padding:20px 20px 24px;}"
        L".hero{margin-bottom:16px;padding:16px 18px;border:1px solid %s;border-radius:16px;background:%s;}"
        L".eyebrow{display:inline-block;margin-bottom:8px;padding:4px 10px;border-radius:999px;background:%s;color:%s;font-size:12px;letter-spacing:.08em;text-transform:uppercase;}"
        L"h1{margin:0 0 8px;font-size:22px;}"
        L".lede{margin:0;color:%s;line-height:1.5;}"
        L".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px;}"
        L".card{border:1px solid %s;border-radius:16px;padding:14px 16px;background:%s;box-shadow:0 1px 0 rgba(0,0,0,.06);}"
        L".card h3{margin:0 0 8px;font-size:17px;}"
        L".meta,.body{font-size:12px;line-height:1.45;color:%s;}"
        L".body{margin-top:10px;font-size:13px;}"
        L".state{display:inline-block;padding:4px 10px;border-radius:999px;background:%s;margin-bottom:10px;font-size:12px;border-left:4px solid var(--accent);}"
        L".source{display:inline-block;padding:3px 8px;border-radius:999px;background:%s;color:%s;font-size:11px;margin-left:6px;text-transform:uppercase;letter-spacing:.06em;}"
        L".accent{height:3px;border-radius:999px;background:var(--accent);margin-top:12px;}"
        L"</style></head><body><div class='hero'><div class='eyebrow'>Map view</div>"
        L"<h1>See the workflow before you drill into a single agent</h1>"
        L"<p class='lede'>Use board view when you want to act. Use map view when you want the big picture.</p>"
        L"</div><div class='grid'>",
        wszAppBg, wszText, wszBorder, wszCardBg, wszAppBg, wszMuted,
        wszMuted, wszBorder, wszCardBg, wszMuted, wszAppBg, wszBorder, wszMuted))
    {
        WideTextBuffer_Free(&html);
        return;
    }
    if (pSnapshot->nodeCount <= 0)
    {
        if (!WideTextBuffer_AppendFormat(&html,
            L"<div class='card' style='--accent:%s'><div class='state'>Ready</div>"
            L"<h3>No agents are running yet</h3>"
            L"<div class='body'>Pick a workflow and start a run to populate the map.</div>"
            L"<div class='accent'></div></div>",
            wszAccent))
        {
            WideTextBuffer_Free(&html);
            return;
        }
    }
    for (int i = 0; i < pSnapshot->nodeCount; i++)
    {
        const AgentNodeSnapshot* node = &pSnapshot->nodes[i];
        WCHAR wszSummary[512];
        WCHAR wszState[64];
        WCHAR wszTitle[256];
        WCHAR wszBackend[64];
        WCHAR wszWorkspace[64];
        WCHAR wszGroup[64];
        WCHAR wszNodeAccent[16];
        Utf8ToWideSafe(node->summary[0] ? node->summary : node->lastAction, wszSummary, ARRAYSIZE(wszSummary));
        Utf8ToWideSafe(AgentRuntime_StateLabel(node->state), wszState, ARRAYSIZE(wszState));
        Utf8ToWideSafe(node->title, wszTitle, ARRAYSIZE(wszTitle));
        Utf8ToWideSafe(AgentRuntime_BackendLabel(node->backend), wszBackend, ARRAYSIZE(wszBackend));
        Utf8ToWideSafe(AgentRuntime_WorkspaceLabel(node->workspacePolicy), wszWorkspace, ARRAYSIZE(wszWorkspace));
        Utf8ToWideSafe(node->group[0] ? node->group : "", wszGroup, ARRAYSIZE(wszGroup));
        ColorToHtml(GetStateAccent(node->state), wszNodeAccent, ARRAYSIZE(wszNodeAccent));
        if (!WideTextBuffer_AppendFormat(&html, L"<div class='card' style='--accent:%s'><div class='state'>", wszNodeAccent) ||
            !WideTextBuffer_AppendEscapedHtml(&html, wszState) ||
            !WideTextBuffer_Append(&html, node->group[0] ? L"</div><span class='source'>" : L"</div>") ||
            (node->group[0] && (!WideTextBuffer_AppendEscapedHtml(&html, wszGroup) ||
            !WideTextBuffer_Append(&html, L"</span>"))) ||
            !WideTextBuffer_Append(&html, L"<h3>") ||
            !WideTextBuffer_AppendEscapedHtml(&html, wszTitle) ||
            !WideTextBuffer_Append(&html, L"</h3><div class='meta'>") ||
            !WideTextBuffer_AppendEscapedHtml(&html, wszBackend) ||
            !WideTextBuffer_Append(&html, L" / ") ||
            !WideTextBuffer_AppendEscapedHtml(&html, wszWorkspace) ||
            !WideTextBuffer_Append(&html, L"</div><div class='body'>") ||
            !WideTextBuffer_AppendEscapedHtml(&html, wszSummary) ||
            !WideTextBuffer_Append(&html, L"</div><div class='accent'></div></div>"))
        {
            WideTextBuffer_Free(&html);
            return;
        }
    }
    if (!WideTextBuffer_Append(&html, L"</div></body></html>"))
    {
        WideTextBuffer_Free(&html);
        return;
    }
    __try
    {
        MissionControlWebView_SetHtml(html.data);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        WideTextBuffer_Free(&html);
        s_mc.webViewFaulted = TRUE;
        return;
    }
    WideTextBuffer_Free(&html);
    s_mc.lastGraphTick = GetTickCount();
}

static void RefreshUi(void)
{
    AgentRuntimeSnapshot* snapshot = NULL;
    OrgSpec* org;
    BOOL hasSelection = FALSE;
    BOOL canRunWorkflow;
    BOOL useNativeMap = TRUE;
    BOOL failed = FALSE;
    if (!s_mc.hwndPanel)
        return;
    if (!s_mc.initComplete)
        return;
    if (s_mc.inRefresh)
        return;
    s_mc.inRefresh = TRUE;
    __try
    {
        snapshot = AllocSnapshot();
        org = GetSelectedOrg();
        canRunWorkflow = s_mc.hasProjectContext && s_mc.orgCount > 0 && org != NULL;
        if (snapshot)
        {
            UpdateSnapshotSummary(snapshot);
            PopulateBoard(snapshot);
            PopulateActivity(snapshot);
            UpdateInspector(snapshot);
            if (s_mc.graphMode)
            {
                UpdateMapFallback(snapshot);
                UpdateGraph(snapshot);
            }
            useNativeMap = ShouldUseNativeMapFallback(snapshot);
            hasSelection = (s_mc.selectedNode >= 0 && s_mc.selectedNode < snapshot->nodeCount);
            EnableWindow(s_mc.hwndRun, !snapshot->isRunning);
            EnableWindow(s_mc.hwndPause, snapshot->isRunning);
            EnableWindow(s_mc.hwndCancel, snapshot->isRunning);
            EnableWindow(s_mc.hwndDuplicate, canRunWorkflow && !snapshot->isRunning);
            EnableWindow(s_mc.hwndAddNode, s_mc.hasProjectContext && !snapshot->isRunning);
            EnableWindow(s_mc.hwndToggleView, snapshot->nodeCount > 0 || snapshot->hasRun);
            EnableWindow(s_mc.hwndHideIdle, snapshot->nodeCount > 0);
            EnableWindow(s_mc.hwndOpenFile, hasSelection && snapshot->nodes[s_mc.selectedNode].fileCount > 0);
            EnableWindow(s_mc.hwndOpenWorkspace, hasSelection && snapshot->nodes[s_mc.selectedNode].workspacePath[0]);
            EnableWindow(s_mc.hwndOpenTranscript, hasSelection && snapshot->nodes[s_mc.selectedNode].transcriptPath[0]);
            EnableWindow(s_mc.hwndOpenProof, hasSelection && snapshot->nodes[s_mc.selectedNode].summaryPath[0]);
            SetWindowTextW(s_mc.hwndRun,
                !s_mc.hasProjectContext ? L"Open Folder..." : (canRunWorkflow ? L"Run Workflow" : L"Create Workflow"));
            SetWindowTextW(s_mc.hwndPause, snapshot->isPaused ? L"Resume Queue" : L"Pause Queue");
            SetWindowTextW(s_mc.hwndCancel, L"Stop Run");
            SetWindowTextW(s_mc.hwndDuplicate, L"Copy Workflow");
            SetWindowTextW(s_mc.hwndAddNode, canRunWorkflow ? L"Add Agent..." : L"Create Workflow");
            SetWindowTextW(s_mc.hwndToggleView, s_mc.graphMode ? L"Board View" : L"Workflow Map");
            SetWindowTextW(s_mc.hwndHideIdle, s_mc.hideIdle ? L"Showing active only" : L"Show active only");
            SetWindowTextW(s_mc.hwndQuickChat, L"Quick Chat");
        }
        else
        {
            UpdateSnapshotSummary(NULL);
            EnableWindow(s_mc.hwndRun, s_mc.hasProjectContext);
            EnableWindow(s_mc.hwndPause, FALSE);
            EnableWindow(s_mc.hwndCancel, FALSE);
            EnableWindow(s_mc.hwndDuplicate, canRunWorkflow);
            EnableWindow(s_mc.hwndAddNode, s_mc.hasProjectContext);
            EnableWindow(s_mc.hwndToggleView, FALSE);
            EnableWindow(s_mc.hwndHideIdle, FALSE);
            EnableWindow(s_mc.hwndOpenFile, FALSE);
            EnableWindow(s_mc.hwndOpenWorkspace, FALSE);
            EnableWindow(s_mc.hwndOpenTranscript, FALSE);
            EnableWindow(s_mc.hwndOpenProof, FALSE);
            SetWindowTextW(s_mc.hwndRun,
                !s_mc.hasProjectContext ? L"Open Folder..." : (canRunWorkflow ? L"Run Workflow" : L"Create Workflow"));
            SetWindowTextW(s_mc.hwndAddNode, canRunWorkflow ? L"Add Agent..." : L"Create Workflow");
            SetWindowTextW(s_mc.hwndToggleView, L"Workflow Map");
            UpdateMapFallback(NULL);
        }
        EnableWindow(s_mc.hwndOrgCombo, s_mc.hasProjectContext && !(snapshot && snapshot->isRunning));
        CheckDlgButton(s_mc.hwndPanel, IDC_MC_HIDE_IDLE, s_mc.hideIdle ? BST_CHECKED : BST_UNCHECKED);
        if (s_mc.graphMode)
        {
            ShowWindow(s_mc.hwndBoard, SW_HIDE);
            ShowWindow(s_mc.hwndGraphHost, useNativeMap ? SW_HIDE : SW_SHOW);
            ShowWindow(s_mc.hwndFallback, useNativeMap ? SW_SHOW : SW_HIDE);
        }
        else
        {
            ShowWindow(s_mc.hwndBoard, SW_SHOW);
            ShowWindow(s_mc.hwndGraphHost, SW_HIDE);
            ShowWindow(s_mc.hwndFallback, SW_HIDE);
        }
        InvalidateRect(s_mc.hwndPanel, NULL, FALSE);
        s_mc.lastRefreshTick = GetTickCount();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        failed = TRUE;
        RecoverFromPanelFailure(s_mc.hwndPanel, WM_MC_REFRESH);
    }
    if (snapshot)
        FreeSnapshot(snapshot);
    if (failed)
        s_mc.refreshQueued = FALSE;
    s_mc.inRefresh = FALSE;
}

static void DeferredInitializePanel(void)
{
    if (!s_mc.hwndPanel)
        return;
    if (s_mc.initComplete)
        return;
    s_mc.initScheduled = FALSE;
    LoadOrgs();
    ApplyTheme();
    s_mc.initComplete = TRUE;
    RefreshUi();
}

static void SelectBoardRow(int row)
{
    LVITEMW item;
    AgentRuntimeSnapshot* snapshot;
    if (row < 0)
        return;
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (ListView_GetItem(s_mc.hwndBoard, &item))
    {
        s_mc.selectedNode = (int)item.lParam;
        snapshot = AllocSnapshot();
        if (snapshot)
        {
            UpdateInspector(snapshot);
            FreeSnapshot(snapshot);
        }
    }
}

static void PaintSectionHeading(HDC hdc, const RECT* rcCard, LPCWSTR eyebrow, LPCWSTR title, LPCWSTR subtitle)
{
    RECT rcEyebrow;
    RECT rcTitle;
    RECT rcSubtitle;
    if (!rcCard || IsRectEmpty(rcCard))
        return;

    rcEyebrow.left = rcCard->left + 12;
    rcEyebrow.top = rcCard->top + 10;
    rcEyebrow.right = rcEyebrow.left + 120;
    rcEyebrow.bottom = rcEyebrow.top + 22;
    BikodeTheme_DrawChip(hdc, &rcEyebrow, eyebrow,
        BikodeTheme_GetColor(BKCLR_APP_BG),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
        BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE, BikodeTheme_GetColor(BKCLR_HOT_MAGENTA));

    rcTitle.left = rcCard->left + 12;
    rcTitle.top = rcEyebrow.bottom + 6;
    rcTitle.right = rcCard->right - 12;
    rcTitle.bottom = rcTitle.top + 20;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_BOLD));
    DrawTextW(hdc, title, -1, &rcTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    rcSubtitle.left = rcCard->left + 12;
    rcSubtitle.top = rcTitle.bottom + 2;
    rcSubtitle.right = rcCard->right - 12;
    rcSubtitle.bottom = rcSubtitle.top + 18;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL));
    DrawTextW(hdc, subtitle, -1, &rcSubtitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void PaintPanel(HWND hwnd, HDC hdc)
{
    RECT rcClient;
    RECT rcEyebrow;
    RECT rcTitle;
    RECT rcStep;
    RECT rcControlBand;
    RECT rcHeroBand;
    OrgSpec* org;
    WCHAR wszTitle[160];
    WCHAR wszStep[320];

    GetClientRect(hwnd, &rcClient);
    FillRect(hdc, &rcClient, s_mc.hbrAppBg ? s_mc.hbrAppBg : (HBRUSH)(COLOR_WINDOW + 1));

    if (!IsRectEmpty(&s_mc.rcHero))
    {
        HBRUSH hBandBrush;
        BikodeTheme_DrawCutCornerPanel(hdc, &s_mc.rcHero,
            BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            14, TRUE);

        rcHeroBand = s_mc.rcHero;
        rcHeroBand.left += 10;
        rcHeroBand.top += 14;
        rcHeroBand.right = rcHeroBand.left + 5;
        rcHeroBand.bottom -= 14;
        hBandBrush = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
        FillRect(hdc, &rcHeroBand, hBandBrush);
        DeleteObject(hBandBrush);
    }

    org = GetSelectedOrg();
    StringCchCopyW(wszTitle, ARRAYSIZE(wszTitle), L"Agent Command Center");
    if (!s_mc.hasProjectContext)
    {
        StringCchCopyW(wszStep, ARRAYSIZE(wszStep),
            L"1. Open a project folder. 2. Describe what to build. 3. Pick a workflow. 4. Run.");
    }
    else if (s_mc.orgLoadFailed)
    {
        StringCchCopyW(wszStep, ARRAYSIZE(wszStep),
            L"Workflow files were found but could not be read cleanly. Click Create Workflow to generate a fresh draft.");
    }
    else if (s_mc.orgCount <= 0)
    {
        StringCchCopyW(wszStep, ARRAYSIZE(wszStep),
            L"Create your first workflow, then use Add Agent to shape the team before you run it.");
    }
    else if (s_mc.isRunning)
    {
        StringCchCopyW(wszStep, ARRAYSIZE(wszStep),
            L"Agents are working. Use Board View for detail, or Workflow Map for a calm overview. Workflow edits unlock after the run ends.");
    }
    else if (s_mc.hasRun)
    {
        StringCchCopyW(wszStep, ARRAYSIZE(wszStep),
            L"Pick another workflow or rerun the current one. The last run stays visible until you replace it.");
    }
    else
    {
        StringCchCopyW(wszStep, ARRAYSIZE(wszStep),
            L"Describe what you want to build, pick a workflow, and click Run Workflow.");
    }

    rcEyebrow.left = s_mc.rcHero.left + 24;
    rcEyebrow.top = s_mc.rcHero.top + 12;
    rcEyebrow.right = rcEyebrow.left + 138;
    rcEyebrow.bottom = rcEyebrow.top + 22;
    BikodeTheme_DrawChip(hdc, &rcEyebrow, L"COMMAND CENTER",
        BikodeTheme_GetColor(BKCLR_APP_BG),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
        BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE,
        BikodeTheme_GetColor(BKCLR_HOT_MAGENTA));

    rcTitle.left = s_mc.rcHero.left + 24;
    rcTitle.top = rcEyebrow.bottom + 6;
    rcTitle.right = s_mc.rcHero.right - 16;
    rcTitle.bottom = rcTitle.top + 20;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_BOLD));
    DrawTextW(hdc, wszTitle, -1, &rcTitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    rcStep.left = s_mc.rcHero.left + 24;
    rcStep.top = rcTitle.bottom + 3;
    rcStep.right = s_mc.rcHero.right - 24;
    rcStep.bottom = rcStep.top + 16;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL));
    DrawTextW(hdc, wszStep, -1, &rcStep, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    rcControlBand.left = s_mc.rcHero.left + 14;
    rcControlBand.top = s_mc.rcHero.top + 84;
    rcControlBand.right = s_mc.rcHero.right - 14;
    rcControlBand.bottom = s_mc.rcHero.bottom - 12;
    BikodeTheme_DrawRoundedPanel(hdc, &rcControlBand,
        BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        10, FALSE);

    if (!IsRectEmpty(&s_mc.rcBoardCard))
    {
        WCHAR wszBoardSubtitle[160];
        if (!s_mc.hasProjectContext)
        {
            StringCchCopyW(wszBoardSubtitle, ARRAYSIZE(wszBoardSubtitle),
                L"Open a project folder to load workflows here.");
        }
        else if (org)
        {
            WCHAR wszOrgName[128];
            Utf8ToWideSafe(org->name, wszOrgName, ARRAYSIZE(wszOrgName));
            StringCchPrintfW(wszBoardSubtitle, ARRAYSIZE(wszBoardSubtitle),
                L"Workflow: %s", wszOrgName);
        }
        else
        {
            StringCchCopyW(wszBoardSubtitle, ARRAYSIZE(wszBoardSubtitle),
                s_mc.graphMode
                    ? L"Use this view when you want the big picture."
                    : L"Use this view when you want to act on a specific agent.");
        }
        BikodeTheme_DrawRoundedPanel(hdc, &s_mc.rcBoardCard,
            BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT), 12, FALSE);
        PaintSectionHeading(hdc, &s_mc.rcBoardCard, s_mc.graphMode ? L"MAP" : L"BOARD",
            s_mc.graphMode ? L"Workflow Map" : L"Agent Board",
            wszBoardSubtitle);
    }

    if (!IsRectEmpty(&s_mc.rcInspectorCard))
    {
        WCHAR wszInspectorTitle[128];
        if (s_mc.selectedNode >= 0)
            StringCchCopyW(wszInspectorTitle, ARRAYSIZE(wszInspectorTitle), L"Selected Agent");
        else
            StringCchCopyW(wszInspectorTitle, ARRAYSIZE(wszInspectorTitle), L"Inspector");
        BikodeTheme_DrawRoundedPanel(hdc, &s_mc.rcInspectorCard,
            BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT), 12, FALSE);
        PaintSectionHeading(hdc, &s_mc.rcInspectorCard, L"INSPECT",
            wszInspectorTitle,
            s_mc.selectedNode >= 0
                ? L"Open files, transcripts, and review notes for the current agent."
                : L"Choose an agent on the left to see what it did and why.");
    }

    if (!IsRectEmpty(&s_mc.rcActivityCard))
    {
        BikodeTheme_DrawRoundedPanel(hdc, &s_mc.rcActivityCard,
            BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
            BikodeTheme_GetColor(BKCLR_STROKE_DARK),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT), 12, FALSE);
        PaintSectionHeading(hdc, &s_mc.rcActivityCard, L"TIMELINE",
            L"Live Activity",
            !s_mc.hasProjectContext
                ? L"Start by opening a project folder."
                : L"Handoffs, tool calls, blockers, and system updates appear here in order.");
    }
}

static void LayoutChildren(HWND hwnd)
{
    RECT rc;
    int width;
    int height;
    int outerPad = 12;
    int heroH;
    int sectionGap = 12;
    int cardHeaderH = 62;
    int cardPad = 10;
    int activityH = 156;
    int inspectorW;
    int controlsTop;
    int controlsTop2;
    int controlsTop3;
    int x;
    int heroLeft;
    int heroRight;
    int heroInnerW;
    int quickChatW;
    int row1Gap = 8;
    int row2Gap = 8;
    int runW = 92;
    int pauseW = 100;
    int cancelW = 88;
    int duplicateW = 108;
    int addW = 108;
    int toggleW = 92;
    int hideIdleW;
    int comboW;
    int compactRowW;
    int boardOuterW;
    int boardOuterH;
    int boardInnerW;
    int boardInnerH;
    int contentTop;
    int contentHeight;
    int inspectorInnerW;
    int actionGap = 8;
    int actionRowW;
    int actionTop;
    int stackedInspectorH;
    int row1Right;
    int row2Right;
    int comboDropH = 260;
    HDWP hdwp;
    BOOL stackInspector;
    BOOL compactHero;
    BOOL quickChatOwnRow;
    BOOL compactInspectorActions;
#define DEFER_CHILD(_hwnd, _x, _y, _w, _h) \
    do { \
        if (hdwp && (_hwnd)) \
            hdwp = DeferWindowPos(hdwp, (_hwnd), NULL, (_x), (_y), (_w), (_h), \
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS); \
    } while (0)
    GetClientRect(hwnd, &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    compactHero = width < 1320;
    quickChatOwnRow = width < 1160;
    heroH = compactHero ? (quickChatOwnRow ? 268 : 248) : 228;
    if (s_iUserHeroH > 0)
        heroH = max(min(s_iUserHeroH, height - 300), 120);
    inspectorW = min(380, max(320, width / 3));
    stackInspector = width < 1240;
    if (s_iUserActivityH > 0)
        activityH = max(min(s_iUserActivityH, height - heroH - 200), 80);
    SetRect(&s_mc.rcHero, outerPad, outerPad, width - outerPad, outerPad + heroH);
    SetRect(&s_mc.rcActivityCard,
        outerPad,
        height - outerPad - activityH,
        width - outerPad,
        height - outerPad);
    contentTop = s_mc.rcHero.bottom + sectionGap;
    contentHeight = max(s_mc.rcActivityCard.top - contentTop - sectionGap, 220);
    if (stackInspector)
    {
        stackedInspectorH = max(min(260, contentHeight / 3), 186);
        boardOuterH = max(contentHeight - stackedInspectorH - sectionGap, 220);
        SetRect(&s_mc.rcBoardCard,
            outerPad,
            contentTop,
            width - outerPad,
            contentTop + boardOuterH);
        SetRect(&s_mc.rcInspectorCard,
            outerPad,
            s_mc.rcBoardCard.bottom + sectionGap,
            width - outerPad,
            s_mc.rcActivityCard.top - sectionGap);
    }
    else
    {
        if (s_iUserBoardFrac > 0)
            boardOuterW = max(min(s_iUserBoardFrac, width - outerPad * 2 - sectionGap - 200), 240);
        else
            boardOuterW = max(width - inspectorW - (outerPad * 3), 240);
        boardOuterH = max(contentHeight, 220);
        SetRect(&s_mc.rcBoardCard,
            outerPad,
            contentTop,
            outerPad + boardOuterW,
            contentTop + boardOuterH);
        SetRect(&s_mc.rcInspectorCard,
            s_mc.rcBoardCard.right + sectionGap,
            s_mc.rcBoardCard.top,
            width - outerPad,
            s_mc.rcBoardCard.bottom);
    }

    heroLeft = s_mc.rcHero.left + 22;
    heroRight = s_mc.rcHero.right - 22;
    heroInnerW = max(heroRight - heroLeft, 320);
    quickChatW = min(112, max(96, heroInnerW / 7));
    controlsTop = s_mc.rcHero.top + 142;
    controlsTop2 = controlsTop + 36;
    controlsTop3 = controlsTop2 + 36;
    hdwp = BeginDeferWindowPos(15);

    {
        int promptLabelW = 180;
        int promptEditW = max(heroInnerW - promptLabelW - row1Gap, 240);
        int promptTop = s_mc.rcHero.top + 100;
        DEFER_CHILD(s_mc.hwndPromptLabel, heroLeft, promptTop + 3, promptLabelW, 20);
        DEFER_CHILD(s_mc.hwndProjectPrompt, heroLeft + promptLabelW + row1Gap, promptTop, promptEditW, 24);
    }

    x = heroLeft;
    row1Right = heroRight - (quickChatOwnRow ? 0 : (quickChatW + row1Gap));
    comboW = max(180, min(340, row1Right - heroLeft - runW - pauseW - cancelW - (row1Gap * 3)));
    DEFER_CHILD(s_mc.hwndOrgCombo, x, controlsTop, comboW, comboDropH); x += comboW + row1Gap;
    DEFER_CHILD(s_mc.hwndRun, x, controlsTop, runW, 28); x += runW + row1Gap;
    DEFER_CHILD(s_mc.hwndPause, x, controlsTop, pauseW, 28); x += pauseW + row1Gap;
    DEFER_CHILD(s_mc.hwndCancel, x, controlsTop, cancelW, 28);
    if (quickChatOwnRow)
        DEFER_CHILD(s_mc.hwndQuickChat, heroRight - quickChatW, controlsTop3, quickChatW, 28);
    else
        DEFER_CHILD(s_mc.hwndQuickChat, heroRight - quickChatW, controlsTop, quickChatW, 28);

    x = heroLeft;
    row2Right = quickChatOwnRow ? (heroRight - quickChatW - row2Gap) : heroRight;
    compactRowW = max(row2Right - heroLeft - (row2Gap * 3), 420);
    duplicateW = min(108, max(88, compactRowW / 4));
    addW = min(108, max(92, compactRowW / 4));
    toggleW = min(96, max(88, compactRowW / 4));
    hideIdleW = max(124, compactRowW - duplicateW - addW - toggleW);
    DEFER_CHILD(s_mc.hwndDuplicate, x, controlsTop2, duplicateW, 28); x += duplicateW + row2Gap;
    DEFER_CHILD(s_mc.hwndAddNode, x, controlsTop2, addW, 28); x += addW + row2Gap;
    DEFER_CHILD(s_mc.hwndToggleView, x, controlsTop2, toggleW, 28); x += toggleW + row2Gap;
    DEFER_CHILD(s_mc.hwndHideIdle, x, controlsTop2, hideIdleW, 28);

    boardInnerW = max((s_mc.rcBoardCard.right - s_mc.rcBoardCard.left) - (cardPad * 2), 120);
    boardInnerH = max((s_mc.rcBoardCard.bottom - s_mc.rcBoardCard.top) - cardHeaderH - cardPad, 120);
    DEFER_CHILD(s_mc.hwndBoard, s_mc.rcBoardCard.left + cardPad, s_mc.rcBoardCard.top + cardHeaderH, boardInnerW, boardInnerH);
    DEFER_CHILD(s_mc.hwndGraphHost, s_mc.rcBoardCard.left + cardPad, s_mc.rcBoardCard.top + cardHeaderH, boardInnerW, boardInnerH);
    DEFER_CHILD(s_mc.hwndFallback, s_mc.rcBoardCard.left + cardPad, s_mc.rcBoardCard.top + cardHeaderH, boardInnerW, boardInnerH);
    ResizeBoardColumns();

    DEFER_CHILD(s_mc.hwndInspectTabs,
        s_mc.rcInspectorCard.left + cardPad,
        s_mc.rcInspectorCard.top + 58,
        (s_mc.rcInspectorCard.right - s_mc.rcInspectorCard.left) - (cardPad * 2),
        28);
    inspectorInnerW = (s_mc.rcInspectorCard.right - s_mc.rcInspectorCard.left) - (cardPad * 2);
    compactInspectorActions = inspectorInnerW < 420;
    actionTop = s_mc.rcInspectorCard.bottom - (compactInspectorActions ? 70 : 38);
    DEFER_CHILD(s_mc.hwndInspectText,
        s_mc.rcInspectorCard.left + cardPad,
        s_mc.rcInspectorCard.top + 92,
        inspectorInnerW,
        max((s_mc.rcInspectorCard.bottom - s_mc.rcInspectorCard.top) - (compactInspectorActions ? 174 : 140), 120));
    if (compactInspectorActions)
    {
        actionRowW = max((inspectorInnerW - actionGap) / 2, 90);
        DEFER_CHILD(s_mc.hwndOpenFile, s_mc.rcInspectorCard.left + cardPad, actionTop, actionRowW, 28);
        DEFER_CHILD(s_mc.hwndOpenWorkspace, s_mc.rcInspectorCard.left + cardPad + actionRowW + actionGap, actionTop, actionRowW, 28);
        DEFER_CHILD(s_mc.hwndOpenTranscript, s_mc.rcInspectorCard.left + cardPad, actionTop + 32, actionRowW, 28);
        DEFER_CHILD(s_mc.hwndOpenProof, s_mc.rcInspectorCard.left + cardPad + actionRowW + actionGap, actionTop + 32, actionRowW, 28);
    }
    else
    {
        actionRowW = max((inspectorInnerW - (actionGap * 3)) / 4, 72);
        DEFER_CHILD(s_mc.hwndOpenFile, s_mc.rcInspectorCard.left + cardPad, actionTop, actionRowW, 28);
        DEFER_CHILD(s_mc.hwndOpenWorkspace, s_mc.rcInspectorCard.left + cardPad + actionRowW + actionGap, actionTop, actionRowW, 28);
        DEFER_CHILD(s_mc.hwndOpenTranscript, s_mc.rcInspectorCard.left + cardPad + ((actionRowW + actionGap) * 2), actionTop, actionRowW, 28);
        DEFER_CHILD(s_mc.hwndOpenProof, s_mc.rcInspectorCard.left + cardPad + ((actionRowW + actionGap) * 3), actionTop, actionRowW, 28);
    }

    DEFER_CHILD(s_mc.hwndActivity,
        s_mc.rcActivityCard.left + cardPad,
        s_mc.rcActivityCard.top + 58,
        (s_mc.rcActivityCard.right - s_mc.rcActivityCard.left) - (cardPad * 2),
        max((s_mc.rcActivityCard.bottom - s_mc.rcActivityCard.top) - 68, 72));

    if (hdwp)
        EndDeferWindowPos(hdwp);

    ShowWindow(s_mc.hwndBoard, s_mc.graphMode ? SW_HIDE : SW_SHOW);
    if (s_mc.graphMode)
    {
        BOOL useNativeMap = s_mc.isRunning || s_mc.webViewFaulted ||
            !MissionControlWebView_IsReady() ||
            (s_mc.lastRuntimeEventTick && (GetTickCount() - s_mc.lastRuntimeEventTick) < 1600);
        ShowWindow(s_mc.hwndGraphHost, useNativeMap ? SW_HIDE : SW_SHOW);
        ShowWindow(s_mc.hwndFallback, useNativeMap ? SW_SHOW : SW_HIDE);
    }
    else
    {
        ShowWindow(s_mc.hwndGraphHost, SW_HIDE);
        ShowWindow(s_mc.hwndFallback, SW_HIDE);
    }

    if (s_mc.graphMode && s_mc.hwndGraphHost && MissionControlWebView_IsReady() && !s_mc.isRunning)
    {
        RECT graphRc;
        GetClientRect(s_mc.hwndGraphHost, &graphRc);
        __try
        {
            MissionControlWebView_Resize(&graphRc);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            s_mc.webViewFaulted = TRUE;
        }
    }

    RequestFullPanelRedraw(hwnd);

#undef DEFER_CHILD
}

static void ApplyTheme(void)
{
    if (!s_mc.hwndPanel)
        return;
    RebuildThemeBrushes();
    if (DarkMode_IsEnabled())
    {
        DarkMode_ApplyToDialog(s_mc.hwndPanel);
        SetWindowTheme(s_mc.hwndBoard, L"DarkMode_Explorer", NULL);
        MarkdownPreview_SetupStyles(s_mc.hwndInspectText);
        SetWindowTheme(s_mc.hwndActivity, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_mc.hwndOrgCombo, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_mc.hwndFallback, L"DarkMode_Explorer", NULL);
        if (ListView_GetHeader(s_mc.hwndBoard))
            SetWindowTheme(ListView_GetHeader(s_mc.hwndBoard), L"DarkMode_Explorer", NULL);
    }
    else
    {
        SetWindowTheme(s_mc.hwndBoard, L"", NULL);
        MarkdownPreview_SetupStyles(s_mc.hwndInspectText);
        SetWindowTheme(s_mc.hwndActivity, L"", NULL);
        SetWindowTheme(s_mc.hwndOrgCombo, L"", NULL);
        SetWindowTheme(s_mc.hwndFallback, L"", NULL);
        if (ListView_GetHeader(s_mc.hwndBoard))
            SetWindowTheme(ListView_GetHeader(s_mc.hwndBoard), L"", NULL);
    }
    ListView_SetBkColor(s_mc.hwndBoard, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED));
    ListView_SetTextBkColor(s_mc.hwndBoard, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED));
    ListView_SetTextColor(s_mc.hwndBoard, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    SendMessageW(s_mc.hwndActivity, LB_SETITEMHEIGHT, 0, 20);
    InvalidateRect(s_mc.hwndPanel, NULL, TRUE);
    if (s_mc.initComplete)
        QueueRefresh();
}

static BOOL CreateEmptyProject(void)
{
    WCHAR szFolder[MAX_PATH] = {0};
    BROWSEINFOW bi = {0};
    LPITEMIDLIST pidl;
    bi.hwndOwner = s_mc.hwndPanel;
    bi.pszDisplayName = szFolder;
    bi.lpszTitle = L"Select or create a folder for your new project";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return FALSE;
    SHGetPathFromIDListW(pidl, szFolder);
    CoTaskMemFree(pidl);
    if (!szFolder[0])
        return FALSE;
    /* Ensure folder exists */
    if (!PathIsDirectoryW(szFolder))
        CreateDirectoryW(szFolder, NULL);
    /* Open in file manager and set as workspace */
    FileManager_OpenFolder(szFolder);
    return TRUE;
}

static void RunSelectedOrg(void)
{
    OrgSpec* org = GetSelectedOrg();
    WCHAR wszDetails[640];
    if (!s_mc.hasProjectContext)
    {
        /* Let user pick an existing folder or create a new empty one */
        if (!CreateEmptyProject())
        {
            /* Fallback: standard open-folder dialog */
            SendMessageW(s_mc.hwndMain, WM_COMMAND, IDM_FILEMGR_OPENFOLDER, 0);
        }
        LoadOrgs();
        RefreshUi();
        return;
    }
    if (!org || s_mc.orgCount <= 0)
    {
        if (CreateDraftWorkflow())
        {
            LoadOrgs();
            RefreshUi();
        }
        return;
    }
    /* Copy user's project prompt into the org spec before starting */
    {
        WCHAR wszPrompt[2048] = {0};
        GetWindowTextW(s_mc.hwndProjectPrompt, wszPrompt, ARRAYSIZE(wszPrompt));
        if (wszPrompt[0])
        {
            int cb = WideCharToMultiByte(CP_UTF8, 0, wszPrompt, -1, org->userPrompt, sizeof(org->userPrompt), NULL, NULL);
            if (cb <= 0) org->userPrompt[0] = '\0';
        }
        else
        {
            org->userPrompt[0] = '\0';
        }
    }
    if (AgentRuntime_Start(org))
    {
        EnsureWorkflowSupportPanelsVisible(L"Workflow run in progress");
        StringCchPrintfW(wszDetails, ARRAYSIZE(wszDetails),
            L"%S is now running in %s.\r\n\r\nThe file sidebar tracks this repo, and this tray will surface changed files, handoffs, and blockers during the run.",
            org->name[0] ? org->name : "Selected workflow",
            s_mc.workspaceRoot[0] ? s_mc.workspaceRoot : L"the active workspace");
        ProofTray_PublishMissionNote(L"Workflow run started", wszDetails);
    }
    RefreshUi();
}

static void DuplicateSelectedOrg(void)
{
    WCHAR wszOutPath[MAX_PATH];
    OrgSpec* org = GetSelectedOrg();
    if (!org)
        return;
    if (AgentRuntime_DuplicateOrgSpec(org, wszOutPath, ARRAYSIZE(wszOutPath)))
        AgentRuntime_SetLastSelectedOrg(wszOutPath);
    LoadOrgs();
    RefreshUi();
}

static BOOL CreateDraftWorkflow(void)
{
    WCHAR wszPath[MAX_PATH];
    if (!s_mc.hasProjectContext)
        return FALSE;
    if (!AgentRuntime_CreateDraftOrgSpec(s_mc.workspaceRoot, wszPath, ARRAYSIZE(wszPath)))
        return FALSE;
    AgentRuntime_SetLastSelectedOrg(wszPath);
    return TRUE;
}

static UINT ShowAddAgentMenu(HWND hwndAnchor)
{
    HMENU hMenu, hEngineering, hDesign, hTesting, hProduct;
    RECT rc;
    UINT cmd = 0;

    if (!hwndAnchor)
        return 0;

    hMenu = CreatePopupMenu();
    if (!hMenu)
        return 0;

    /* Quick-add basics */
    AppendMenuW(hMenu, MF_STRING, IDM_MC_ADD_RESEARCH, L"Research Agent");
    AppendMenuW(hMenu, MF_STRING, IDM_MC_ADD_REVIEW, L"Review Agent");
    AppendMenuW(hMenu, MF_STRING, IDM_MC_ADD_VALIDATE, L"Validation Agent");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);

    /* The Agency: Engineering Division */
    hEngineering = CreatePopupMenu();
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_FRONTEND_DEV,       L"Frontend Developer");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_BACKEND_ARCHITECT,  L"Backend Architect");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_SOFTWARE_ARCHITECT, L"Software Architect");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_CODE_REVIEWER,      L"Code Reviewer");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_SECURITY_ENGINEER,  L"Security Engineer");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_DEVOPS_AUTOMATOR,   L"DevOps Automator");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_DATABASE_OPTIMIZER, L"Database Optimizer");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_RAPID_PROTOTYPER,   L"Rapid Prototyper");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_TECHNICAL_WRITER,   L"Technical Writer");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_SRE,                L"SRE");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_GIT_WORKFLOW,       L"Git Workflow Master");
    AppendMenuW(hEngineering, MF_STRING, IDM_MC_ADD_INCIDENT_COMMANDER, L"Incident Commander");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hEngineering, L"Engineering");

    /* The Agency: Design Division */
    hDesign = CreatePopupMenu();
    AppendMenuW(hDesign, MF_STRING, IDM_MC_ADD_UX_ARCHITECT,  L"UX Architect");
    AppendMenuW(hDesign, MF_STRING, IDM_MC_ADD_UI_DESIGNER,   L"UI Designer");
    AppendMenuW(hDesign, MF_STRING, IDM_MC_ADD_UX_RESEARCHER, L"UX Researcher");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hDesign, L"Design");

    /* The Agency: Testing Division */
    hTesting = CreatePopupMenu();
    AppendMenuW(hTesting, MF_STRING, IDM_MC_ADD_PERF_BENCHMARKER, L"Performance Benchmarker");
    AppendMenuW(hTesting, MF_STRING, IDM_MC_ADD_ACCESSIBILITY,    L"Accessibility Auditor");
    AppendMenuW(hTesting, MF_STRING, IDM_MC_ADD_API_TESTER,       L"API Tester");
    AppendMenuW(hTesting, MF_STRING, IDM_MC_ADD_REALITY_CHECKER,  L"Reality Checker");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hTesting, L"Testing");

    /* The Agency: Product Division */
    hProduct = CreatePopupMenu();
    AppendMenuW(hProduct, MF_STRING, IDM_MC_ADD_SPRINT_PRIORITIZER, L"Sprint Prioritizer");
    AppendMenuW(hProduct, MF_STRING, IDM_MC_ADD_FEEDBACK_SYNTH,     L"Feedback Synthesizer");
    AppendMenuW(hProduct, MF_STRING, IDM_MC_ADD_TREND_RESEARCHER,   L"Trend Researcher");
    AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hProduct, L"Product");

    /* Promptfoo: LLM Eval & Red Team */
    {
        HMENU hPromptfoo = CreatePopupMenu();
        AppendMenuW(hPromptfoo, MF_STRING, IDM_MC_ADD_PROMPT_EVALUATOR, L"Prompt Evaluator");
        AppendMenuW(hPromptfoo, MF_STRING, IDM_MC_ADD_RED_TEAMER,       L"Red Teamer");
        AppendMenuW(hPromptfoo, MF_STRING, IDM_MC_ADD_MODEL_COMPARATOR, L"Model Comparator");
        AppendMenuW(hPromptfoo, MF_STRING, IDM_MC_ADD_REGRESSION_GUARD, L"Regression Guard");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hPromptfoo, L"Eval && Red Team (Promptfoo)");
    }

    /* Impeccable: Frontend Design Quality */
    {
        HMENU hImpeccable = CreatePopupMenu();
        AppendMenuW(hImpeccable, MF_STRING, IDM_MC_ADD_DESIGN_AUDITOR,    L"Design Auditor");
        AppendMenuW(hImpeccable, MF_STRING, IDM_MC_ADD_DESIGN_CRITIC,     L"Design Critic");
        AppendMenuW(hImpeccable, MF_STRING, IDM_MC_ADD_DESIGN_POLISHER,   L"Design Polisher");
        AppendMenuW(hImpeccable, MF_STRING, IDM_MC_ADD_TYPOGRAPHY_EXPERT, L"Typography Expert");
        AppendMenuW(hImpeccable, MF_STRING, IDM_MC_ADD_COLOR_SPECIALIST,  L"Color Specialist");
        AppendMenuW(hImpeccable, MF_STRING, IDM_MC_ADD_MOTION_DESIGNER,   L"Motion Designer");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hImpeccable, L"Frontend Design (Impeccable)");
    }

    /* OpenViking: Context & Memory Management */
    {
        HMENU hViking = CreatePopupMenu();
        AppendMenuW(hViking, MF_STRING, IDM_MC_ADD_CONTEXT_ARCHITECT,   L"Context Architect");
        AppendMenuW(hViking, MF_STRING, IDM_MC_ADD_MEMORY_CURATOR,      L"Memory Curator");
        AppendMenuW(hViking, MF_STRING, IDM_MC_ADD_RETRIEVAL_OPTIMIZER, L"Retrieval Optimizer");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hViking, L"Context && Memory (OpenViking)");
    }

    /* SWE-Bench: Automated Issue Resolution */
    {
        HMENU hSWEBench = CreatePopupMenu();
        AppendMenuW(hSWEBench, MF_STRING, IDM_MC_ADD_ISSUE_TRIAGER,     L"Issue Triager");
        AppendMenuW(hSWEBench, MF_STRING, IDM_MC_ADD_BUG_REPRODUCER,    L"Bug Reproducer");
        AppendMenuW(hSWEBench, MF_STRING, IDM_MC_ADD_PATCH_WRITER,      L"Patch Writer");
        AppendMenuW(hSWEBench, MF_STRING, IDM_MC_ADD_REGRESSION_TESTER, L"Regression Tester");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hSWEBench, L"Issue Resolution (SWE-Bench)");
    }

    /* Semgrep: Static Analysis & Security */
    {
        HMENU hSemgrep = CreatePopupMenu();
        AppendMenuW(hSemgrep, MF_STRING, IDM_MC_ADD_SAST_SCANNER,       L"SAST Scanner");
        AppendMenuW(hSemgrep, MF_STRING, IDM_MC_ADD_VULN_FIXER,         L"Vulnerability Fixer");
        AppendMenuW(hSemgrep, MF_STRING, IDM_MC_ADD_DEPENDENCY_AUDITOR, L"Dependency Auditor");
        AppendMenuW(hSemgrep, MF_STRING, IDM_MC_ADD_COMPLIANCE_CHECKER, L"Compliance Checker");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hSemgrep, L"Security Scanning (Semgrep)");
    }

    /* MetaGPT: Software Project Team */
    {
        HMENU hMetaGPT = CreatePopupMenu();
        AppendMenuW(hMetaGPT, MF_STRING, IDM_MC_ADD_PRODUCT_MANAGER, L"Product Manager");
        AppendMenuW(hMetaGPT, MF_STRING, IDM_MC_ADD_TECH_LEAD,       L"Tech Lead");
        AppendMenuW(hMetaGPT, MF_STRING, IDM_MC_ADD_SENIOR_DEV,      L"Senior Developer");
        AppendMenuW(hMetaGPT, MF_STRING, IDM_MC_ADD_QA_ENGINEER,     L"QA Engineer");
        AppendMenuW(hMetaGPT, MF_STRING, IDM_MC_ADD_SCRUM_MASTER,    L"Scrum Master");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hMetaGPT, L"Project Team (MetaGPT)");
    }

    /* Aider: AI Pair Programming */
    {
        HMENU hAider = CreatePopupMenu();
        AppendMenuW(hAider, MF_STRING, IDM_MC_ADD_REPO_NAVIGATOR, L"Repo Navigator");
        AppendMenuW(hAider, MF_STRING, IDM_MC_ADD_CODE_EDITOR,    L"Code Editor");
        AppendMenuW(hAider, MF_STRING, IDM_MC_ADD_GIT_COMMITTER,  L"Git Committer");
        AppendMenuW(hAider, MF_STRING, IDM_MC_ADD_TEST_RUNNER,    L"Test Runner");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hAider, L"Pair Programming (Aider)");
    }

    /* CrewAI: Collaborative Multi-Agent */
    {
        HMENU hCrewAI = CreatePopupMenu();
        AppendMenuW(hCrewAI, MF_STRING, IDM_MC_ADD_TASK_COORDINATOR,   L"Task Coordinator");
        AppendMenuW(hCrewAI, MF_STRING, IDM_MC_ADD_DOMAIN_EXPERT,      L"Domain Expert");
        AppendMenuW(hCrewAI, MF_STRING, IDM_MC_ADD_CODE_SPECIALIST,    L"Code Specialist");
        AppendMenuW(hCrewAI, MF_STRING, IDM_MC_ADD_INTEGRATION_TESTER, L"Integration Tester");
        AppendMenuW(hMenu, MF_STRING | MF_POPUP, (UINT_PTR)hCrewAI, L"Collaborative (CrewAI)");
    }

    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_MC_ADD_ORCHESTRATOR, L"Orchestrator (The Agency)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (s_bUseLocalBackend ? MF_CHECKED : MF_UNCHECKED),
        IDM_MC_TOGGLE_LOCAL_BACKEND, L"Use Local Model Backend");

    GetWindowRect(hwndAnchor, &rc);
    cmd = (UINT)TrackPopupMenuEx(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
        rc.left, rc.bottom, s_mc.hwndPanel ? s_mc.hwndPanel : hwndAnchor, NULL);
    DestroyMenu(hMenu);
    return cmd;
}

static BOOL AddTemplatedNode(OrgSpec* pSpec, UINT commandId, WCHAR* wszAddedTitle, int cchAddedTitle)
{
    OrgNodeSpec* node;
    int nodeNumber;
    const OrgNodeSpec* prevNode;
    if (!pSpec || pSpec->nodeCount >= AGENT_RUNTIME_MAX_NODES)
        return FALSE;

    nodeNumber = pSpec->nodeCount + 1;
    prevNode = pSpec->nodeCount > 0 ? &pSpec->nodes[pSpec->nodeCount - 1] : NULL;
    node = &pSpec->nodes[pSpec->nodeCount++];
    ZeroMemory(node, sizeof(*node));

    StringCchPrintfA(node->id, ARRAYSIZE(node->id), "node-%d", nodeNumber);
    node->backend = s_bUseLocalBackend ? AGENT_BACKEND_LOCAL : AGENT_BACKEND_API;
    node->workspacePolicy = pSpec->defaultWorkspacePolicy;
    StringCchCopyA(node->group, ARRAYSIZE(node->group), "custom");

    switch (commandId)
    {
    case IDM_MC_ADD_REVIEW:
        StringCchPrintfA(node->title, ARRAYSIZE(node->title), "Review %d", nodeNumber);
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "review");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "Review the latest handoff, call out risks or regressions, and leave a concise approval note or blocker.");
        break;
    case IDM_MC_ADD_VALIDATE:
        StringCchPrintfA(node->title, ARRAYSIZE(node->title), "Validate %d", nodeNumber);
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "validate");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "Verify the latest changes, run the most relevant checks, and report what still needs attention before shipping.");
        break;

    /* ---- The Agency: Engineering Division ---- */
    case IDM_MC_ADD_FRONTEND_DEV:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Frontend Developer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Frontend Developer, an expert specializing in modern web technologies, React/Vue/Angular frameworks, UI implementation, and performance optimization. "
            "Build responsive, accessible, and performant web applications with pixel-perfect design implementation. "
            "Focus on Core Web Vitals, component architecture, and mobile-first responsive design. "
            "Deliver clean, well-structured code with proper state management and seamless API integration.");
        break;
    case IDM_MC_ADD_BACKEND_ARCHITECT:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Backend Architect");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Backend Architect, a senior architect specializing in scalable system design, database architecture, and cloud infrastructure. "
            "Design microservices architectures that scale horizontally. Create database schemas optimized for performance and consistency. "
            "Implement robust API architectures with proper versioning. Build event-driven systems for high throughput. "
            "Include comprehensive security measures, error handling, circuit breakers, and monitoring in all designs.");
        break;
    case IDM_MC_ADD_SOFTWARE_ARCHITECT:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Software Architect");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Software Architect, an expert who designs maintainable, scalable systems aligned with business domains. "
            "Think in bounded contexts, trade-off matrices, and architectural decision records. "
            "No architecture astronautics: every abstraction must justify its complexity. Trade-offs over best practices. "
            "Domain first, technology second. Prefer reversible decisions. Document decisions with ADRs capturing WHY, not just WHAT. "
            "Present at least two options with trade-offs for every significant decision.");
        break;
    case IDM_MC_ADD_CODE_REVIEWER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Code Reviewer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Code Reviewer, an expert who provides thorough, constructive code reviews. "
            "Focus on correctness, security, maintainability, and performance. "
            "Be specific: cite lines and explain why. Prioritize with blockers, suggestions, and nits. "
            "Praise good code. Check for SQL injection, XSS, auth bypass, race conditions, missing error handling. "
            "One review, complete feedback. Start with summary, end with encouragement.");
        break;
    case IDM_MC_ADD_SECURITY_ENGINEER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Security Engineer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Security Engineer, an expert application security engineer specializing in threat modeling, vulnerability assessment, and secure code review. "
            "Conduct STRIDE analysis, review for OWASP Top 10 and CWE Top 25. "
            "Never recommend disabling security controls. Assume all user input is malicious. "
            "Prefer well-tested libraries over custom crypto. No hardcoded credentials. Default to deny. "
            "Pair every vulnerability finding with clear, actionable remediation guidance.");
        break;
    case IDM_MC_ADD_DEVOPS_AUTOMATOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "DevOps Automator");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are DevOps Automator, an expert DevOps engineer specializing in infrastructure automation, CI/CD pipeline development, and cloud operations. "
            "Design Infrastructure as Code with Terraform, CloudFormation, or CDK. Build CI/CD pipelines with automated testing and deployment. "
            "Implement zero-downtime deployment strategies. Include monitoring, alerting, and automated rollback. "
            "Optimize costs with resource right-sizing and multi-environment automation.");
        break;
    case IDM_MC_ADD_DATABASE_OPTIMIZER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Database Optimizer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Database Optimizer, a performance expert who thinks in query plans, indexes, and connection pools. "
            "Design schemas that scale, write queries that fly, and debug slow queries with EXPLAIN ANALYZE. "
            "Focus on indexing strategies (B-tree, GiST, GIN, partial), N+1 detection, connection pooling, "
            "migration strategies, and zero-downtime deployments. Every foreign key gets an index, every migration is reversible.");
        break;
    case IDM_MC_ADD_RAPID_PROTOTYPER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Rapid Prototyper");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Rapid Prototyper, a specialist in ultra-fast proof-of-concept development and MVP creation. "
            "Create working prototypes quickly using the most efficient tools and frameworks. "
            "Focus on core user flows and primary value propositions. Build modular architectures for quick iteration. "
            "Document assumptions and hypotheses. Plan transition paths from prototype to production.");
        break;
    case IDM_MC_ADD_TECHNICAL_WRITER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Technical Writer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Technical Writer, an expert who creates clear, accurate technical documentation. "
            "Write developer docs, API references, tutorials, and architecture guides. "
            "Structure content for scanability. Use consistent terminology. Include working code examples. "
            "Maintain a glossary for domain-specific terms. Target the right audience level.");
        break;
    case IDM_MC_ADD_SRE:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "SRE");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are SRE, a site reliability engineer focused on SLOs, error budgets, observability, and chaos engineering. "
            "Define meaningful SLIs/SLOs tied to user experience. Build error budgets that balance velocity and reliability. "
            "Implement structured observability with metrics, logs, and traces. Reduce toil through automation. "
            "Plan capacity based on growth projections. Design for graceful degradation.");
        break;
    case IDM_MC_ADD_GIT_WORKFLOW:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Git Workflow Master");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Git Workflow Master, an expert in branching strategies, conventional commits, and advanced Git operations. "
            "Design CI-friendly branch management. Enforce conventional commit messages. "
            "Plan history cleanup and rebasing strategies. Manage release branching and hotfix workflows. "
            "Ensure traceability from commits to issues.");
        break;
    case IDM_MC_ADD_INCIDENT_COMMANDER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Incident Commander");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "debug");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "engineering");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Incident Response Commander, managing production incidents with structured severity assessment, "
            "clear communication, and blameless post-mortems. Triage by user impact. Coordinate responders. "
            "Maintain incident timelines. Write actionable post-mortems with concrete follow-up items. "
            "Build runbooks for recurring scenarios. Focus on MTTR reduction.");
        break;

    /* ---- The Agency: Design Division ---- */
    case IDM_MC_ADD_UX_ARCHITECT:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "UX Architect");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are UX Architect, a technical architecture and UX specialist who creates solid foundations for developers. "
            "Provide CSS design systems with variables, spacing scales, and typography hierarchies. "
            "Design layout frameworks using modern Grid/Flexbox patterns. Establish component architecture and naming conventions. "
            "Convert visual requirements into implementable technical architecture. Include light/dark theme support.");
        break;
    case IDM_MC_ADD_UI_DESIGNER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "UI Designer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are UI Designer, an expert in visual design, component libraries, and design systems. "
            "Create cohesive visual language with consistent spacing, typography, and color. "
            "Build reusable component libraries. Ensure brand consistency across all surfaces. "
            "Design for accessibility, responsiveness, and delight.");
        break;
    case IDM_MC_ADD_UX_RESEARCHER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "UX Researcher");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are UX Researcher, an expert in user testing, behavior analysis, and design research. "
            "Design research plans with clear hypotheses. Conduct usability evaluations. "
            "Synthesize findings into actionable recommendations. Identify user pain points and opportunities. "
            "Ground every recommendation in observed user behavior, not assumptions.");
        break;

    /* ---- The Agency: Testing Division ---- */
    case IDM_MC_ADD_PERF_BENCHMARKER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Performance Benchmarker");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "testing");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Performance Benchmarker, an expert performance testing and optimization specialist. "
            "Execute load testing, stress testing, and scalability assessment. Establish performance baselines. "
            "Identify bottlenecks through systematic analysis. Optimize for Core Web Vitals: LCP < 2.5s, FID < 100ms, CLS < 0.1. "
            "Create performance budgets and enforce quality gates. All systems must meet SLAs with 95% confidence.");
        break;
    case IDM_MC_ADD_ACCESSIBILITY:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Accessibility Auditor");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "testing");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Accessibility Auditor, an expert who ensures products are usable by everyone. "
            "Audit against WCAG 2.2 AA: Perceivable, Operable, Understandable, Robust. "
            "Verify screen reader compatibility, keyboard-only navigation, voice control, and zoom usability. "
            "Automated tools catch 30% of issues -- you catch the other 70%. "
            "Check ARIA roles, focus management, error announcements, and cognitive accessibility.");
        break;
    case IDM_MC_ADD_API_TESTER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "API Tester");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "testing");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are API Tester, an expert in API validation and integration testing. "
            "Verify endpoint correctness, authentication flows, rate limiting, and error handling. "
            "Test edge cases: malformed input, missing fields, concurrent requests, large payloads. "
            "Validate response schemas, status codes, and pagination. Check for security vulnerabilities in API surface.");
        break;
    case IDM_MC_ADD_REALITY_CHECKER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Reality Checker");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "testing");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Reality Checker, an evidence-based certification and quality gate specialist. "
            "Require proof for every claim. Verify production readiness across reliability, security, performance, and UX. "
            "Check that monitoring, alerting, and rollback procedures are in place. "
            "No ship without evidence. Provide clear pass/fail verdicts with specific blockers.");
        break;

    /* ---- The Agency: Product Division ---- */
    case IDM_MC_ADD_SPRINT_PRIORITIZER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Sprint Prioritizer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "product");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Sprint Prioritizer, an expert product manager specializing in agile sprint planning and feature prioritization. "
            "Use RICE, MoSCoW, and value-vs-effort frameworks. Analyze team velocity and capacity. "
            "Identify cross-team dependencies. Balance technical debt against new features. "
            "Define clear sprint goals with measurable outcomes. Prevent scope creep.");
        break;
    case IDM_MC_ADD_FEEDBACK_SYNTH:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Feedback Synthesizer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "product");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Feedback Synthesizer, an expert in user feedback analysis and insight extraction. "
            "Aggregate feedback from multiple channels. Identify patterns and recurring themes. "
            "Quantify sentiment and urgency. Translate raw feedback into prioritized product recommendations. "
            "Distinguish between what users say they want and what they actually need.");
        break;
    case IDM_MC_ADD_TREND_RESEARCHER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Trend Researcher");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "product");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Trend Researcher, an expert in market intelligence and competitive analysis. "
            "Identify emerging trends and market opportunities. Analyze competitor strategies and positioning. "
            "Assess technology adoption curves. Provide actionable intelligence for product decisions. "
            "Separate signal from noise in market data.");
        break;

    /* ---- Promptfoo: LLM Eval & Red Team ---- */
    case IDM_MC_ADD_PROMPT_EVALUATOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Prompt Evaluator");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "eval");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Prompt Evaluator, an LLM evaluation specialist inspired by promptfoo methodology. "
            "Design test cases that measure prompt quality across correctness, relevance, and safety. "
            "Create evaluation matrices comparing expected vs actual outputs. "
            "Use assertion-based grading: exact match, contains, similarity score, LLM-as-judge. "
            "Track metrics across iterations to ensure prompts improve over time. Report pass/fail rates with confidence. "
            "You have access to the eval_prompt tool -- use it to systematically evaluate prompt quality against criteria like clarity, injection risk, bias, and specificity.");
        break;
    case IDM_MC_ADD_RED_TEAMER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Red Teamer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "eval");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Red Teamer, an LLM security specialist inspired by promptfoo red-teaming methodology. "
            "Probe AI systems for vulnerabilities: prompt injection, jailbreaks, data leakage, harmful outputs, bias. "
            "Generate adversarial test cases systematically. Test boundary conditions and edge cases. "
            "Classify findings by severity. Provide remediation strategies for each vulnerability found. "
            "Focus on defensive security: make AI apps safer, not exploitable. "
            "You have access to the red_team_prompt tool -- use it to probe prompts for injection, jailbreak, leakage, and bias vulnerabilities. "
            "Also use eval_prompt to assess prompt quality before and after hardening.");
        break;
    case IDM_MC_ADD_MODEL_COMPARATOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Model Comparator");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "eval");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Model Comparator, an LLM benchmarking specialist inspired by promptfoo. "
            "Compare model outputs side-by-side across identical prompts. Evaluate on cost, latency, quality, and safety. "
            "Design A/B test frameworks for prompt variants. Track token usage and pricing across providers. "
            "Produce comparison matrices with clear winner/loser verdicts per use case. "
            "Recommend optimal model selection based on the specific workload requirements. "
            "You have access to the eval_prompt tool -- use it to score prompt variants for clarity and robustness before benchmarking.");
        break;
    case IDM_MC_ADD_REGRESSION_GUARD:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Regression Guard");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "eval");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Regression Guard, a CI/CD quality gate specialist for LLM outputs inspired by promptfoo. "
            "Define golden test sets that must pass before prompt changes ship. "
            "Monitor output quality drift over time. Alert on degradation in any evaluation dimension. "
            "Enforce minimum pass rates for safety, correctness, and relevance. "
            "Produce automated go/no-go verdicts for prompt deployments. "
            "You have access to eval_prompt and red_team_prompt tools -- use them to run automated quality and security checks on prompts before approving changes.");
        break;

    /* ---- Impeccable: Frontend Design Quality ---- */
    case IDM_MC_ADD_DESIGN_AUDITOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Design Auditor");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Design Auditor, a frontend quality specialist inspired by Impeccable. "
            "Run technical quality checks: accessibility (WCAG AA), performance (Core Web Vitals), responsive behavior. "
            "Check for anti-patterns: gray text on colored backgrounds, cards nested in cards, pure black/white, "
            "overused fonts (Inter, Roboto, Arial), identical card grids, glassmorphism overuse. "
            "Verify type scale consistency, color contrast ratios, and spatial rhythm. Report issues with severity. "
            "You have access to the design_audit tool -- use it to check files for typography, color contrast, spacing, hierarchy, and a11y issues systematically.");
        break;
    case IDM_MC_ADD_DESIGN_CRITIC:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Design Critic");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Design Critic, a UX design reviewer inspired by Impeccable. "
            "Review for hierarchy clarity, emotional resonance, and intentional aesthetics. "
            "Evaluate whether the design has a bold, committed direction or falls into generic AI slop. "
            "Check: Is there real visual hierarchy? Does spacing create rhythm? Is color used with purpose? "
            "Push for distinctive design over safe defaults. Praise bold creative choices. "
            "You have access to the design_audit tool -- use it to run technical checks on CSS/HTML files for typography, color, and hierarchy issues.");
        break;
    case IDM_MC_ADD_DESIGN_POLISHER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Design Polisher");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Design Polisher, a final-pass refinement specialist inspired by Impeccable. "
            "Apply production-grade polish: micro-interactions, hover states, focus indicators, loading states. "
            "Normalize to design system standards. Strip unnecessary complexity. Clarify UX copy. "
            "Ensure every decorative element serves a functional or emotional purpose. "
            "This is the last pass before shipping -- make every pixel intentional. "
            "You have access to the design_audit tool -- use it to verify typography, color, spacing, and a11y before finalizing.");
        break;
    case IDM_MC_ADD_TYPOGRAPHY_EXPERT:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Typography Expert");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Typography Expert, a type systems specialist inspired by Impeccable. "
            "Design modular type scales with fluid sizing (clamp). Choose distinctive fonts -- avoid Inter, Roboto, Arial. "
            "Create clear hierarchy with fewer sizes and more contrast. Set vertical rhythm based on line-height. "
            "Use proper measure (max-width: 65ch). Pair fonts on multiple contrast axes. "
            "Implement performant font loading to prevent layout shifts. "
            "You have access to the design_audit tool with checks='typography' -- use it to analyze type scale, line-height, and font choices in target files.");
        break;
    case IDM_MC_ADD_COLOR_SPECIALIST:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Color Specialist");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Color Specialist, a color systems expert inspired by Impeccable. "
            "Use OKLCH for perceptually uniform palettes. Tint neutrals toward brand hue. "
            "Follow 60-30-10 rule for visual weight distribution. Reduce chroma at extreme lightness. "
            "Build functional palettes: primary, neutral (9-11 shades), semantic, surface. "
            "Never use pure black or pure white. Support light-dark() for theme switching. "
            "Ensure WCAG AA contrast ratios on all text. "
            "You have access to the design_audit tool with checks='color,a11y' -- use it to analyze color values, contrast ratios, and accessibility in target files.");
        break;
    case IDM_MC_ADD_MOTION_DESIGNER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Motion Designer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "design");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Motion Designer, an animation specialist inspired by Impeccable. "
            "Use motion to convey state changes: entrances, exits, feedback. "
            "Apply exponential easing (ease-out-quart/quint) for natural deceleration. "
            "Orchestrate staggered page load reveals. Respect prefers-reduced-motion. "
            "Avoid bounce/elastic easing (feels dated). Focus on high-impact moments. "
            "One well-orchestrated transition beats scattered micro-interactions.");
        break;

    /* ---- OpenViking: Context & Memory Management ---- */
    case IDM_MC_ADD_CONTEXT_ARCHITECT:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Context Architect");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "context");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Context Architect, a context database designer inspired by OpenViking. "
            "Organize agent context using a filesystem paradigm: unify memories, resources, and skills. "
            "Design tiered context loading (L0 always-loaded, L1 on-demand, L2 deep retrieval) to reduce token consumption. "
            "Structure context directories for recursive retrieval. Define context schemas that support "
            "automatic session management and long-term memory extraction. Minimize fragmentation. "
            "You have access to the context_store tool -- use it to store and retrieve architectural knowledge, context schemas, and memory structures for the workspace.");
        break;
    case IDM_MC_ADD_MEMORY_CURATOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Memory Curator");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "context");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Memory Curator, an agent memory specialist inspired by OpenViking. "
            "Extract and compress long-term memories from conversation sessions. "
            "Distinguish between task memory (what the agent accomplished) and user memory (preferences, context). "
            "Organize memories hierarchically for efficient retrieval. Prune outdated or conflicting memories. "
            "Ensure memory iteration makes the agent smarter with each session, not just bigger. "
            "You have access to the context_store tool -- use it to persist extracted memories, retrieve existing knowledge, and list stored context entries.");
        break;
    case IDM_MC_ADD_RETRIEVAL_OPTIMIZER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Retrieval Optimizer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "context");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Retrieval Optimizer, a context retrieval specialist inspired by OpenViking. "
            "Combine directory positioning with semantic search for precise context acquisition. "
            "Optimize retrieval trajectories for observability and debuggability. "
            "Implement tiered loading strategies that balance completeness against token budget. "
            "Visualize retrieval paths to identify and fix retrieval failures. "
            "Ensure retrieved context is relevant, minimal, and sufficient for the agent's task. "
            "You have access to the context_store tool -- use it to test retrieval patterns and optimize what context gets stored vs retrieved.");
        break;

    /* ---- SWE-Bench: Automated Issue Resolution ---- */
    case IDM_MC_ADD_ISSUE_TRIAGER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Issue Triager");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "debug");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "swebench");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Issue Triager, inspired by SWE-Agent (Princeton/Stanford). "
            "Parse the issue description, identify the failing behavior, locate the relevant source files, "
            "and produce a minimal reproduction plan. Classify severity and estimate blast radius. "
            "Narrow the search space to the smallest set of files that could contain the bug. "
            "Hand off a focused brief with file paths, suspected root cause, and reproduction steps.");
        break;
    case IDM_MC_ADD_BUG_REPRODUCER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Bug Reproducer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "debug");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "swebench");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Bug Reproducer, inspired by SWE-Agent. "
            "Read the identified source files, trace the execution path, and confirm the root cause. "
            "Write a minimal test case or command that demonstrates the failure. "
            "Document the exact conditions under which the bug manifests. "
            "If the bug cannot be reproduced, explain why and suggest alternative investigation paths.");
        break;
    case IDM_MC_ADD_PATCH_WRITER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Patch Writer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "swebench");
        node->backend = s_bUseLocalBackend ? AGENT_BACKEND_LOCAL : AGENT_BACKEND_RELAY;
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Patch Writer, inspired by SWE-Agent. "
            "Write the minimal, focused patch that fixes the confirmed bug without introducing regressions. "
            "Keep changes as small as possible. Follow the existing code style exactly. "
            "Add or update tests to cover the fixed behavior. "
            "Document what was changed and why in a clear commit-message-style summary.");
        break;
    case IDM_MC_ADD_REGRESSION_TESTER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Regression Tester");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "swebench");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Regression Tester, inspired by SWE-Agent. "
            "Verify the patch fixes the original issue without breaking existing functionality. "
            "Run related tests and analyze results. Check edge cases around the fix boundary. "
            "Confirm the reproduction case now passes. Report any regressions or incomplete fixes. "
            "Provide a clear pass/fail verdict with evidence.");
        break;

    /* ---- Semgrep: Static Analysis & Security Scanning ---- */
    case IDM_MC_ADD_SAST_SCANNER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "SAST Scanner");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "security");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are SAST Scanner, a static application security testing specialist inspired by Semgrep. "
            "Scan source code for security vulnerabilities using pattern-based analysis. "
            "Check for OWASP Top 10: injection, broken auth, sensitive data exposure, XXE, broken access control, "
            "misconfiguration, XSS, insecure deserialization, known vulnerable components, insufficient logging. "
            "Also check CWE Top 25. Report findings with severity, CWE ID, file location, and remediation guidance. "
            "Minimize false positives by understanding data flow context.");
        break;
    case IDM_MC_ADD_VULN_FIXER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Vulnerability Fixer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "security");
        node->backend = s_bUseLocalBackend ? AGENT_BACKEND_LOCAL : AGENT_BACKEND_RELAY;
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Vulnerability Fixer, inspired by Semgrep auto-fix capabilities. "
            "Apply security patches for findings from the SAST scan. Fix critical and high severity first. "
            "Use parameterized queries instead of string concatenation. Sanitize all user input. "
            "Replace custom crypto with well-tested libraries. Remove hardcoded secrets. "
            "Ensure fixes don't break functionality. Each fix should be minimal and well-documented.");
        break;
    case IDM_MC_ADD_DEPENDENCY_AUDITOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Dependency Auditor");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "security");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Dependency Auditor, inspired by Semgrep Supply Chain and Snyk. "
            "Audit project dependencies for known vulnerabilities, license compliance, and supply chain risks. "
            "Check package manifests (package.json, requirements.txt, go.mod, Cargo.toml, etc.). "
            "Identify outdated dependencies with known CVEs. Flag transitive dependency risks. "
            "Recommend version upgrades with breaking change analysis. Check for typosquatting and malicious packages.");
        break;
    case IDM_MC_ADD_COMPLIANCE_CHECKER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Compliance Checker");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "security");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Compliance Checker, inspired by Semgrep and Checkov policy engines. "
            "Verify code and infrastructure against compliance frameworks: SOC2, HIPAA, PCI-DSS, GDPR as applicable. "
            "Check for proper data handling, encryption at rest and in transit, access controls, and audit logging. "
            "Verify infrastructure-as-code templates follow security best practices. "
            "Produce a compliance report with pass/fail status per control and remediation steps for failures.");
        break;

    /* ---- MetaGPT: Software Project Team Simulation ---- */
    case IDM_MC_ADD_PRODUCT_MANAGER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Product Manager");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "metagpt");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Product Manager, inspired by MetaGPT's role-based project simulation. "
            "Translate user requirements into detailed product requirements documents (PRDs). "
            "Define user stories with clear acceptance criteria. Prioritize features by business value. "
            "Create competitive analysis. Define the MVP scope. "
            "Hand off a structured PRD with user stories, wireframe descriptions, and success metrics.");
        break;
    case IDM_MC_ADD_TECH_LEAD:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Tech Lead");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "metagpt");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Tech Lead, inspired by MetaGPT. "
            "Translate the PRD into a technical design document with system architecture, data models, "
            "API specifications, and technology stack decisions. Define module boundaries and interfaces. "
            "Create a development plan with task breakdown and dependencies. "
            "Identify technical risks and mitigation strategies. Estimate effort for each component.");
        break;
    case IDM_MC_ADD_SENIOR_DEV:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Senior Developer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "metagpt");
        node->backend = s_bUseLocalBackend ? AGENT_BACKEND_LOCAL : AGENT_BACKEND_RELAY;
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Senior Developer, inspired by MetaGPT. "
            "Implement the technical design from the Tech Lead's specification. "
            "Write clean, well-structured, production-quality code following the architecture. "
            "Implement proper error handling, logging, and input validation. "
            "Write unit tests alongside implementation. Follow SOLID principles. "
            "Leave clear documentation for complex logic. Commit code in logical, reviewable chunks.");
        break;
    case IDM_MC_ADD_QA_ENGINEER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "QA Engineer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "metagpt");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are QA Engineer, inspired by MetaGPT. "
            "Design comprehensive test plans from the PRD and technical design. "
            "Write integration tests, end-to-end tests, and edge case tests. "
            "Verify all acceptance criteria from user stories are met. "
            "Test error handling, boundary conditions, and concurrent scenarios. "
            "Report bugs with clear reproduction steps, expected vs actual behavior, and severity.");
        break;
    case IDM_MC_ADD_SCRUM_MASTER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Scrum Master");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "metagpt");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Scrum Master, inspired by MetaGPT. "
            "Review the full delivery pipeline output for process quality. "
            "Check that requirements trace from PRD through design to implementation to tests. "
            "Identify gaps in test coverage, missing error handling, or incomplete features. "
            "Produce a sprint retrospective: what went well, what needs improvement, action items. "
            "Provide a clear ship/no-ship recommendation with evidence.");
        break;

    /* ---- Aider: AI Pair Programming ---- */
    case IDM_MC_ADD_REPO_NAVIGATOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Repo Navigator");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "aider");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Repo Navigator, inspired by Aider's repo-map capability. "
            "Map the repository structure, identify key modules, entry points, and dependency relationships. "
            "Build a mental model of the codebase architecture. Identify which files are relevant to the current task. "
            "Summarize the codebase layout, tech stack, build system, and testing infrastructure. "
            "Hand off a focused context brief with the exact files and functions that need attention.");
        break;
    case IDM_MC_ADD_CODE_EDITOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Code Editor");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "aider");
        node->backend = s_bUseLocalBackend ? AGENT_BACKEND_LOCAL : AGENT_BACKEND_RELAY;
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Code Editor, inspired by Aider's edit-focused pair programming approach. "
            "Make targeted, surgical code changes based on the navigator's context brief. "
            "Edit files directly using write_file and replace_in_file tools. "
            "Follow existing code conventions exactly. Make the smallest change that achieves the goal. "
            "Explain each change clearly. Keep the codebase clean -- no leftover debug code or commented-out blocks.");
        break;
    case IDM_MC_ADD_GIT_COMMITTER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Git Committer");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "reviewer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "aider");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Git Committer, inspired by Aider's auto-commit workflow. "
            "Review the changes made by the Code Editor for correctness and completeness. "
            "Verify changes match the original task intent. Check for accidentally modified files. "
            "Suggest clear, conventional commit messages that describe why, not just what. "
            "Flag any changes that should be split into separate commits for cleaner history.");
        break;
    case IDM_MC_ADD_TEST_RUNNER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Test Runner");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "aider");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Test Runner, inspired by Aider's test-driven workflow. "
            "Run existing tests to verify the code changes don't break anything. "
            "If tests fail, analyze the failure and report which changes caused the regression. "
            "Suggest new test cases for the changed code. Verify edge cases and error paths. "
            "Produce a clear test report with pass/fail counts and any failures requiring attention.");
        break;

    /* ---- CrewAI: Collaborative Multi-Agent Tasks ---- */
    case IDM_MC_ADD_TASK_COORDINATOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Task Coordinator");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "crewai");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Task Coordinator, inspired by CrewAI's role-based orchestration. "
            "Break down the complex task into sub-tasks with clear ownership and dependencies. "
            "Define success criteria for each sub-task. Identify which specialist agent should handle each part. "
            "Create a structured execution plan with parallel tracks where possible. "
            "Ensure each handoff includes complete context so downstream agents can work independently.");
        break;
    case IDM_MC_ADD_DOMAIN_EXPERT:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Domain Expert");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "crewai");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Domain Expert, inspired by CrewAI's specialist agent pattern. "
            "Provide deep domain knowledge for the current task. Research the codebase to understand "
            "existing patterns, conventions, and architectural decisions. "
            "Identify domain-specific constraints, edge cases, and best practices. "
            "Advise the implementation agents on the right approach based on domain expertise. "
            "Document key domain insights that should inform all downstream decisions.");
        break;
    case IDM_MC_ADD_CODE_SPECIALIST:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Code Specialist");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "implementer");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "crewai");
        node->backend = s_bUseLocalBackend ? AGENT_BACKEND_LOCAL : AGENT_BACKEND_RELAY;
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Code Specialist, inspired by CrewAI's collaborative execution model. "
            "Implement your assigned sub-task from the coordinator's plan with domain expert guidance. "
            "Write production-quality code that integrates cleanly with the existing codebase. "
            "Follow established patterns and conventions. Handle errors gracefully. "
            "Leave integration notes for other agents working on related sub-tasks.");
        break;
    case IDM_MC_ADD_INTEGRATION_TESTER:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Integration Tester");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "tester");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "crewai");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Integration Tester, inspired by CrewAI's collaborative quality assurance. "
            "Verify that all sub-tasks integrate correctly when combined. "
            "Test the interfaces between components built by different agents. "
            "Check for data consistency, API contract compliance, and end-to-end workflow correctness. "
            "Report integration issues with clear identification of which components conflict. "
            "Provide a final integration verdict with pass/fail status.");
        break;

    /* ---- The Agency: Orchestration ---- */
    case IDM_MC_ADD_ORCHESTRATOR:
        StringCchCopyA(node->title, ARRAYSIZE(node->title), "Orchestrator");
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "planner");
        StringCchCopyA(node->group, ARRAYSIZE(node->group), "agency");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "You are Agents Orchestrator, the autonomous pipeline manager who coordinates multi-agent development workflows. "
            "Manage the full pipeline from specification to production-ready implementation. "
            "Ensure each phase completes before advancing. Coordinate handoffs with proper context. "
            "Implement continuous quality loops: task-by-task validation, automatic retry, quality gates. "
            "Provide clear status updates and completion summaries. "
            "You have access to context_store (persist/retrieve knowledge), eval_prompt (evaluate prompt quality), and design_audit (check UI files) tools.");
        break;

    case IDM_MC_ADD_RESEARCH:
    default:
        StringCchPrintfA(node->title, ARRAYSIZE(node->title), "Research %d", nodeNumber);
        StringCchCopyA(node->role, ARRAYSIZE(node->role), "research");
        StringCchCopyA(node->prompt, ARRAYSIZE(node->prompt),
            "Inspect the workspace, contribute one focused finding, and hand off a concise summary.");
        break;
    }

    if (prevNode)
    {
        StringCchCopyA(node->dependsOn[0], ARRAYSIZE(node->dependsOn[0]), prevNode->id);
        node->dependsOnCount = 1;
    }

    if (wszAddedTitle && cchAddedTitle > 0)
        Utf8ToWideSafe(node->title, wszAddedTitle, cchAddedTitle);
    return TRUE;
}

static void AddNodeToSelectedOrg(void)
{
    WCHAR wszAddedTitle[128];
    WCHAR wszInfo[512];
    UINT addChoice;
    OrgSpec* org = GetSelectedOrg();
    if (!s_mc.hasProjectContext)
        return;
    if (AgentRuntime_IsRunning())
    {
        MessageBoxW(s_mc.hwndPanel,
            L"Finish or stop the current run before editing the workflow.\r\n\r\n"
            L"Add Agent now opens an explicit choice menu and only changes the saved workflow. It never starts a run on its own.",
            L"Command Center",
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!org)
    {
        if (CreateDraftWorkflow())
        {
            LoadOrgs();
            RefreshUi();
            org = GetSelectedOrg();
        }
        if (!org)
            return;
    }
    addChoice = ShowAddAgentMenu(s_mc.hwndAddNode);
    if (!addChoice)
        return;
    if (addChoice == IDM_MC_TOGGLE_LOCAL_BACKEND)
    {
        s_bUseLocalBackend = !s_bUseLocalBackend;
        return;
    }
    wszAddedTitle[0] = L'\0';
    if (AddTemplatedNode(org, addChoice, wszAddedTitle, ARRAYSIZE(wszAddedTitle)))
    {
        AgentRuntime_SaveOrgSpec(org);
        AgentRuntime_SetLastSelectedOrg(org->path);
        StringCchPrintfW(wszInfo, ARRAYSIZE(wszInfo),
            L"Added %s to this workflow.\r\n\r\nIt will join the next run after you click Run Workflow.",
            wszAddedTitle[0] ? wszAddedTitle : L"the new agent");
        MessageBoxW(s_mc.hwndPanel, wszInfo, L"Command Center", MB_OK | MB_ICONINFORMATION);
    }
    LoadOrgs();
    RefreshUi();
}

static void OpenCurrentSelection(int what)
{
    AgentRuntimeSnapshot* snapshot;
    const AgentNodeSnapshot* node;
    snapshot = AllocSnapshot();
    if (!snapshot)
        return;
    if (s_mc.selectedNode < 0 || s_mc.selectedNode >= snapshot->nodeCount)
    {
        FreeSnapshot(snapshot);
        return;
    }
    node = &snapshot->nodes[s_mc.selectedNode];
    switch (what)
    {
    case IDC_MC_OPEN_FILE:
        if (node->fileCount > 0)
            OpenEditorPath(node->changedFiles[0]);
        break;
    case IDC_MC_OPEN_WORKSPACE:
        AgentRuntime_OpenPathInExplorer(node->workspacePath);
        break;
    case IDC_MC_OPEN_TRANSCRIPT:
        OpenEditorPath(node->transcriptPath);
        break;
    case IDC_MC_OPEN_PROOF:
        OpenEditorPath(node->summaryPath);
        break;
    }
    FreeSnapshot(snapshot);
}

/* --- Splitter hit-testing --- */
static int HitTestSplitter(HWND hwnd, int mx, int my)
{
    RECT rc;
    int grip = MC_SPLITTER_GRIP;
    GetClientRect(hwnd, &rc);
    (void)rc;

    /* Bottom edge of Hero card */
    if (my >= s_mc.rcHero.bottom - grip && my <= s_mc.rcHero.bottom + grip &&
        mx >= s_mc.rcHero.left && mx <= s_mc.rcHero.right)
        return MC_SPLITTER_HERO;

    /* Top edge of Activity/Timeline card */
    if (my >= s_mc.rcActivityCard.top - grip && my <= s_mc.rcActivityCard.top + grip &&
        mx >= s_mc.rcActivityCard.left && mx <= s_mc.rcActivityCard.right)
        return MC_SPLITTER_VACT;

    /* Right edge of Board card (side-by-side mode only) */
    if (!IsRectEmpty(&s_mc.rcInspectorCard) &&
        s_mc.rcBoardCard.right < s_mc.rcInspectorCard.right &&
        mx >= s_mc.rcBoardCard.right - grip && mx <= s_mc.rcBoardCard.right + grip &&
        my >= s_mc.rcBoardCard.top && my <= s_mc.rcBoardCard.bottom)
        return MC_SPLITTER_HBOARD;

    return MC_SPLITTER_NONE;
}

static LRESULT MissionControlProcImpl(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
        InitCommonControlsEx(&icc);
        s_mc.hwndOrgCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_ORG_COMBO, NULL, NULL);
        s_mc.hwndRun = CreateWindowExW(0, L"BUTTON", L"Run Workflow", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_RUN, NULL, NULL);
        s_mc.hwndPause = CreateWindowExW(0, L"BUTTON", L"Pause Queue", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_PAUSE, NULL, NULL);
        s_mc.hwndCancel = CreateWindowExW(0, L"BUTTON", L"Stop Run", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_CANCEL, NULL, NULL);
        s_mc.hwndDuplicate = CreateWindowExW(0, L"BUTTON", L"Copy Workflow", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_DUPLICATE, NULL, NULL);
        s_mc.hwndAddNode = CreateWindowExW(0, L"BUTTON", L"Add Agent...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_ADD_NODE, NULL, NULL);
        s_mc.hwndToggleView = CreateWindowExW(0, L"BUTTON", L"Workflow Map", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_TOGGLE_VIEW, NULL, NULL);
        s_mc.hwndHideIdle = CreateWindowExW(0, L"BUTTON", L"Show active only", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_HIDE_IDLE, NULL, NULL);
        s_mc.hwndQuickChat = CreateWindowExW(0, L"BUTTON", L"Quick Chat", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_QUICK_CHAT, NULL, NULL);
        s_mc.hwndBoard = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_BOARD, NULL, NULL);
        s_mc.hwndGraphHost = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_GRAPH, NULL, NULL);
        s_mc.hwndFallback = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_FALLBACK, NULL, NULL);
        s_mc.hwndInspectTabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_INSPECT_TABS, NULL, NULL);
        s_mc.hwndInspectText = CreateWindowExW(0, L"Scintilla", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_INSPECT_TEXT, NULL, NULL);
        if (s_mc.hwndInspectText)
        {
            MarkdownPreview_SetupStyles(s_mc.hwndInspectText);
            SendMessage(s_mc.hwndInspectText, SCI_SETREADONLY, TRUE, 0);
            SendMessage(s_mc.hwndInspectText, SCI_SETWRAPMODE, SC_WRAP_WORD, 0);
            SendMessage(s_mc.hwndInspectText, SCI_SETMARGINWIDTHN, 0, 0);
            SendMessage(s_mc.hwndInspectText, SCI_SETMARGINWIDTHN, 1, 0);
            SendMessage(s_mc.hwndInspectText, SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE, 0);
        }
        s_mc.hwndOpenFile = CreateWindowExW(0, L"BUTTON", L"Open File", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_FILE, NULL, NULL);
        s_mc.hwndOpenWorkspace = CreateWindowExW(0, L"BUTTON", L"Workspace", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_WORKSPACE, NULL, NULL);
        s_mc.hwndOpenTranscript = CreateWindowExW(0, L"BUTTON", L"Transcript", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_TRANSCRIPT, NULL, NULL);
        s_mc.hwndOpenProof = CreateWindowExW(0, L"BUTTON", L"Review", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_PROOF, NULL, NULL);
        s_mc.hwndActivity = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_ACTIVITY, NULL, NULL);
        s_mc.hwndPromptLabel = CreateWindowExW(0, L"STATIC", L"What do you want to build?",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_PROMPT_LABEL, NULL, NULL);
        s_mc.hwndProjectPrompt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_PROJECT_PROMPT, NULL, NULL);
        SendMessageW(s_mc.hwndProjectPrompt, EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. Build a task management app with drag-and-drop kanban board...");

        if (!s_mc.hwndOrgCombo || !s_mc.hwndRun || !s_mc.hwndPause || !s_mc.hwndCancel ||
            !s_mc.hwndDuplicate || !s_mc.hwndAddNode || !s_mc.hwndToggleView || !s_mc.hwndHideIdle ||
            !s_mc.hwndQuickChat || !s_mc.hwndBoard || !s_mc.hwndGraphHost || !s_mc.hwndFallback ||
            !s_mc.hwndInspectTabs || !s_mc.hwndInspectText || !s_mc.hwndOpenFile ||
            !s_mc.hwndOpenWorkspace || !s_mc.hwndOpenTranscript || !s_mc.hwndOpenProof || !s_mc.hwndActivity)
        {
            return -1;
        }

        {
            HWND controls[] = {
                s_mc.hwndOrgCombo, s_mc.hwndRun, s_mc.hwndPause, s_mc.hwndCancel, s_mc.hwndDuplicate, s_mc.hwndAddNode,
                s_mc.hwndToggleView, s_mc.hwndHideIdle, s_mc.hwndQuickChat, s_mc.hwndBoard, s_mc.hwndInspectTabs,
                s_mc.hwndInspectText, s_mc.hwndOpenFile, s_mc.hwndOpenWorkspace, s_mc.hwndOpenTranscript, s_mc.hwndOpenProof,
                s_mc.hwndActivity, s_mc.hwndFallback
            };
            int i;
            for (i = 0; i < (int)ARRAYSIZE(controls); i++)
                SetChildFont(controls[i]);
            /* Scintilla inspector uses MarkdownPreview styles, not theme font */
            SetThemeFont(s_mc.hwndFallback, BKFONT_UI_SMALL);
        }

        ListView_SetExtendedListViewStyle(s_mc.hwndBoard, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        TCITEMW item = { TCIF_TEXT };
        item.pszText = L"Summary"; TabCtrl_InsertItem(s_mc.hwndInspectTabs, 0, &item);
        item.pszText = L"Transcript"; TabCtrl_InsertItem(s_mc.hwndInspectTabs, 1, &item);
        item.pszText = L"Files"; TabCtrl_InsertItem(s_mc.hwndInspectTabs, 2, &item);
        item.pszText = L"Review"; TabCtrl_InsertItem(s_mc.hwndInspectTabs, 3, &item);
        item.pszText = L"Metrics"; TabCtrl_InsertItem(s_mc.hwndInspectTabs, 4, &item);
        return 0;
    }
    case WM_MC_DEFER_INIT:
        DeferredInitializePanel();
        return 0;
    case WM_SIZE:
        if (s_mc.visible)
            LayoutChildren(hwnd);
        return 0;
    case WM_ERASEBKGND:
    {
        if (s_iDragSplitter)
            return 1;  /* suppress erase during drag to prevent flicker */
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, s_mc.hbrAppBg ? s_mc.hbrAppBg : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintPanel(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
    {
        if ((HWND)wParam == hwnd && LOWORD(lParam) == HTCLIENT)
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int hit = s_iDragSplitter ? s_iDragSplitter : HitTestSplitter(hwnd, pt.x, pt.y);
            if (hit == MC_SPLITTER_HBOARD)
            {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                return TRUE;
            }
            if (hit == MC_SPLITTER_HERO || hit == MC_SPLITTER_VACT)
            {
                SetCursor(LoadCursor(NULL, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN:
    {
        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);
        int hit = HitTestSplitter(hwnd, mx, my);
        if (hit != MC_SPLITTER_NONE)
        {
            s_iDragSplitter = hit;
            s_ptDragStart.x = mx;
            s_ptDragStart.y = my;
            switch (hit)
            {
            case MC_SPLITTER_HERO:
                s_iDragOrigVal = s_mc.rcHero.bottom - s_mc.rcHero.top;
                break;
            case MC_SPLITTER_HBOARD:
                s_iDragOrigVal = s_mc.rcBoardCard.right - s_mc.rcBoardCard.left;
                break;
            case MC_SPLITTER_VACT:
                s_iDragOrigVal = s_mc.rcActivityCard.bottom - s_mc.rcActivityCard.top;
                break;
            }
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
    {
        if (s_iDragSplitter && (wParam & MK_LBUTTON))
        {
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            int delta;
            switch (s_iDragSplitter)
            {
            case MC_SPLITTER_HERO:
                delta = my - s_ptDragStart.y;
                s_iUserHeroH = s_iDragOrigVal + delta;
                break;
            case MC_SPLITTER_HBOARD:
                delta = mx - s_ptDragStart.x;
                s_iUserBoardFrac = s_iDragOrigVal + delta;
                break;
            case MC_SPLITTER_VACT:
                delta = my - s_ptDragStart.y;
                s_iUserActivityH = s_iDragOrigVal - delta;
                break;
            }
            LayoutChildren(hwnd);
            return 0;
        }
        else
        {
            /* Update cursor on hover even without drag */
            int mx = GET_X_LPARAM(lParam);
            int my = GET_Y_LPARAM(lParam);
            int hit = HitTestSplitter(hwnd, mx, my);
            if (hit == MC_SPLITTER_HBOARD)
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
            else if (hit == MC_SPLITTER_HERO || hit == MC_SPLITTER_VACT)
                SetCursor(LoadCursor(NULL, IDC_SIZENS));
        }
        break;
    }
    case WM_LBUTTONUP:
    {
        if (s_iDragSplitter)
        {
            s_iDragSplitter = MC_SPLITTER_NONE;
            ReleaseCapture();
            return 0;
        }
        break;
    }
    case WM_CAPTURECHANGED:
    {
        s_iDragSplitter = MC_SPLITTER_NONE;
        break;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
        HDC hdc = (HDC)wParam;
        HWND hwndCtl = (HWND)lParam;
        int ctrlId = hwndCtl ? GetDlgCtrlID(hwndCtl) : 0;
        SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
        SetBkMode(hdc, OPAQUE);
        if (ctrlId == IDC_MC_ACTIVITY || ctrlId == IDC_MC_ORG_COMBO || ctrlId == IDC_MC_FALLBACK)
        {
            SetBkColor(hdc, BikodeTheme_GetColor(BKCLR_SURFACE_RAISED));
            return (LRESULT)(s_mc.hbrSurfaceRaised ? s_mc.hbrSurfaceRaised : (HBRUSH)(COLOR_WINDOW + 1));
        }
        SetBkColor(hdc, BikodeTheme_GetColor(BKCLR_APP_BG));
        return (LRESULT)(s_mc.hbrAppBg ? s_mc.hbrAppBg : (HBRUSH)(COLOR_WINDOW + 1));
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MC_RUN:
            RunSelectedOrg();
            return 0;
        case IDC_MC_PAUSE:
        {
            AgentRuntimeSnapshot* snapshot = AllocSnapshot();
            if (snapshot)
            {
                if (snapshot->isRunning)
                    AgentRuntime_SetPaused(!snapshot->isPaused);
                FreeSnapshot(snapshot);
            }
        }
            return 0;
        case IDC_MC_CANCEL:
            if (AgentRuntime_IsRunning())
                AgentRuntime_Cancel();
            return 0;
        case IDC_MC_DUPLICATE:
            DuplicateSelectedOrg();
            return 0;
        case IDC_MC_ADD_NODE:
            AddNodeToSelectedOrg();
            return 0;
        case IDC_MC_TOGGLE_VIEW:
            s_mc.graphMode = !s_mc.graphMode;
            if (!s_mc.graphMode)
                KillTimer(hwnd, IDT_MC_GRAPH_READY);
            LayoutChildren(hwnd);
            QueueRefresh();
            return 0;
        case IDC_MC_HIDE_IDLE:
            s_mc.hideIdle = (IsDlgButtonChecked(hwnd, IDC_MC_HIDE_IDLE) == BST_CHECKED);
            QueueRefresh();
            return 0;
        case IDC_MC_QUICK_CHAT:
            PostMessageW(s_mc.hwndMain, WM_COMMAND, IDM_AI_TOGGLE_CHAT, 0);
            return 0;
        case IDC_MC_OPEN_FILE:
        case IDC_MC_OPEN_WORKSPACE:
        case IDC_MC_OPEN_TRANSCRIPT:
        case IDC_MC_OPEN_PROOF:
            OpenCurrentSelection(LOWORD(wParam));
            return 0;
        case IDC_MC_ORG_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                OrgSpec* org = GetSelectedOrg();
                if (org)
                    AgentRuntime_SetLastSelectedOrg(org->path);
                QueueRefresh();
            }
            return 0;
        }
        break;
    case WM_MC_REFRESH:
        s_mc.refreshQueued = FALSE;
        RefreshUi();
        return 0;
    case WM_TIMER:
        if (wParam == IDT_MC_GRAPH_READY)
        {
            if (!s_mc.graphMode)
            {
                KillTimer(hwnd, IDT_MC_GRAPH_READY);
                return 0;
            }
            if (MissionControlWebView_IsReady())
            {
                KillTimer(hwnd, IDT_MC_GRAPH_READY);
                QueueRefresh();
            }
            return 0;
        }
        if (wParam == IDT_MC_REFRESH_COALESCE)
        {
            KillTimer(hwnd, IDT_MC_REFRESH_COALESCE);
            s_mc.refreshTimerArmed = FALSE;
            if (!s_mc.refreshQueued)
            {
                s_mc.refreshQueued = TRUE;
                PostMessageW(hwnd, WM_MC_REFRESH, 0, 0);
            }
            return 0;
        }
        break;
    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lParam;
        if (!hdr)
            break;
        if (hdr->idFrom == IDC_MC_BOARD && hdr->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* nm = (NMLISTVIEW*)lParam;
            if (s_mc.isPopulating)
                return 0;
            if ((nm->uNewState & LVIS_SELECTED) != 0)
                SelectBoardRow(nm->iItem);
            return 0;
        }
        if (hdr->idFrom == IDC_MC_BOARD && hdr->code == NM_CUSTOMDRAW)
        {
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
            WCHAR wszState[64];
            switch (cd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT:
                cd->clrText = BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY);
                cd->clrTextBk = BikodeTheme_GetColor(BKCLR_SURFACE_RAISED);
                if ((cd->nmcd.uItemState & CDIS_SELECTED) != 0)
                {
                    cd->clrTextBk = BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN),
                        BikodeTheme_GetColor(BKCLR_SURFACE_RAISED), 28);
                }
                else
                {
                    ListView_GetItemText(s_mc.hwndBoard, (int)cd->nmcd.dwItemSpec, 0, wszState, ARRAYSIZE(wszState));
                    if (_wcsicmp(wszState, L"Running") == 0)
                        cd->clrText = BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN);
                    else if (_wcsicmp(wszState, L"Done") == 0)
                        cd->clrText = BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN);
                    else if (_wcsicmp(wszState, L"Queued") == 0 || _wcsicmp(wszState, L"Blocked") == 0 || _wcsicmp(wszState, L"Paused") == 0)
                        cd->clrText = BikodeTheme_GetColor(BKCLR_WARNING_ORANGE);
                    else if (_wcsicmp(wszState, L"Error") == 0 || _wcsicmp(wszState, L"Canceled") == 0)
                        cd->clrText = BikodeTheme_GetColor(BKCLR_DANGER_RED);
                }
                return CDRF_DODEFAULT;
            default:
                break;
            }
        }
        if (hdr->idFrom == IDC_MC_INSPECT_TABS && hdr->code == TCN_SELCHANGE)
        {
            RefreshUi();
            return 0;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hwnd, IDT_MC_GRAPH_READY);
        KillTimer(hwnd, IDT_MC_REFRESH_COALESCE);
        MissionControlWebView_Destroy();
        ResetHandles();
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK MissionControlProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    __try
    {
        return MissionControlProcImpl(hwnd, msg, wParam, lParam);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        RecoverFromPanelFailure(hwnd, msg);
        return (msg == WM_CREATE) ? (LRESULT)-1 : 0;
    }
}

BOOL MissionControl_Init(HWND hwndMain)
{
    ZeroMemory(&s_mc, sizeof(s_mc));
    s_mc.hwndMain = hwndMain;
    s_mc.selectedNode = -1;
    s_mc.visible = FALSE;
    return TRUE;
}

void MissionControl_Shutdown(void)
{
    MissionControlWebView_Destroy();
    if (s_mc.hwndPanel)
    {
        DestroyWindow(s_mc.hwndPanel);
        s_mc.hwndPanel = NULL;
    }
    ZeroMemory(&s_mc, sizeof(s_mc));
}

void MissionControl_Show(HWND hwndParent)
{
    EnsurePanel();
    if (!s_mc.hwndPanel)
        return;
    s_mc.visible = TRUE;
    ShowWindow(s_mc.hwndPanel, SW_SHOW);
    if (hwndParent)
    {
        RECT rc;
        GetClientRect(hwndParent, &rc);
        PostMessageW(hwndParent, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    }
    if (!s_mc.initComplete && !s_mc.initScheduled)
    {
        s_mc.initScheduled = TRUE;
        PostMessageW(s_mc.hwndPanel, WM_MC_DEFER_INIT, 0, 0);
    }
    else
    {
        QueueRefresh();
    }
}

void MissionControl_Hide(void)
{
    if (!s_mc.hwndPanel)
        return;
    s_mc.visible = FALSE;
    DestroyWindow(s_mc.hwndPanel);
    if (s_mc.hwndMain)
    {
        RECT rc;
        GetClientRect(s_mc.hwndMain, &rc);
        PostMessageW(s_mc.hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    }
}

void MissionControl_Toggle(HWND hwndParent)
{
    if (s_mc.visible)
        MissionControl_Hide();
    else
        MissionControl_Show(hwndParent);
}

BOOL MissionControl_IsVisible(void)
{
    return s_mc.visible;
}

void MissionControl_Layout(HWND hwndParent, int x, int y, int cx, int cy)
{
    UNREFERENCED_PARAMETER(hwndParent);
    EnsurePanel();
    if (!s_mc.hwndPanel)
        return;
    if (s_mc.visible)
    {
        MoveWindow(s_mc.hwndPanel, x, y, cx, cy, TRUE);
        LayoutChildren(s_mc.hwndPanel);
    }
}

void MissionControl_ApplyTheme(void)
{
    ApplyTheme();
}

BOOL MissionControl_HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(wParam);
    if (msg == WM_AGENT_RUNTIME_EVENT)
    {
        __try
        {
            AgentEvent* pEvent = (AgentEvent*)lParam;
            if (pEvent)
            {
                s_mc.lastRuntimeEventTick = GetTickCount();
                if (pEvent->type == AGENT_EVENT_FILE || pEvent->type == AGENT_EVENT_HANDOFF || pEvent->type == AGENT_EVENT_ERROR)
                {
                    WCHAR wszTitle[128];
                    WCHAR wszDetails[2048];
                    LPCWSTR wszStatus = pEvent->type == AGENT_EVENT_ERROR
                        ? L"Workflow needs attention"
                        : (pEvent->type == AGENT_EVENT_HANDOFF ? L"Agent handoff ready" : L"Workflow changes ready to inspect");
                    Utf8ToWideSafe(pEvent->nodeTitle[0] ? pEvent->nodeTitle : pEvent->nodeId, wszTitle, ARRAYSIZE(wszTitle));
                    Utf8ToWideSafe(pEvent->message, wszDetails, ARRAYSIZE(wszDetails));
                    EnsureWorkflowSupportPanelsVisible(wszStatus);
                    if (wszTitle[0] && wszDetails[0])
                        ProofTray_PublishMissionNote(wszTitle, wszDetails);
                }
                free(pEvent);
            }
            QueueRefresh();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            RecoverFromPanelFailure(s_mc.hwndPanel, msg);
        }
        return TRUE;
    }
    return FALSE;
}

HWND MissionControl_GetHwnd(void)
{
    return s_mc.hwndPanel;
}
