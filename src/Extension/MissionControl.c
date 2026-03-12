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
#define WM_MC_DEFER_INIT       (WM_APP + 0x3A0)
#define WM_MC_REFRESH          (WM_APP + 0x3A1)
#define IDT_MC_GRAPH_READY     0xFC40
#define IDT_MC_REFRESH_COALESCE 0xFC41
#define IDM_MC_ADD_RESEARCH    0xFD20
#define IDM_MC_ADD_REVIEW      0xFD21
#define IDM_MC_ADD_VALIDATE    0xFD22

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

static MissionControlUi s_mc;
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

static void RequestFullPanelRedraw(HWND hwnd)
{
    if (!hwnd)
        return;
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
        col.pszText = L"Agent"; col.cx = 172; ListView_InsertColumn(s_mc.hwndBoard, 1, &col);
        col.pszText = L"Setup"; col.cx = 136; ListView_InsertColumn(s_mc.hwndBoard, 2, &col);
        col.pszText = L"What it's doing"; col.cx = 280; ListView_InsertColumn(s_mc.hwndBoard, 3, &col);
        col.pszText = L"Tools"; col.cx = 54; ListView_InsertColumn(s_mc.hwndBoard, 4, &col);
        col.pszText = L"Files"; col.cx = 54; ListView_InsertColumn(s_mc.hwndBoard, 5, &col);
        col.pszText = L"Time"; col.cx = 64; ListView_InsertColumn(s_mc.hwndBoard, 6, &col);
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
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 2, wszSetup);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 3, wszAction);
        swprintf_s(wszText, ARRAYSIZE(wszText), L"%d", node->toolCount);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 4, wszText);
        swprintf_s(wszText, ARRAYSIZE(wszText), L"%d", node->fileCount);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 5, wszText);
        swprintf_s(wszText, ARRAYSIZE(wszText), L"%lus", (unsigned long)elapsed);
        ListView_SetItemText(s_mc.hwndBoard, item.iItem, 6, wszText);
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
            L"%S\r\n\r\nRole: %S\r\nState: %S\r\nBackend: %S\r\nWorkspace: %S\r\nTools used: %d\r\nFiles touched: %d\r\n\r\nCurrent focus:\r\n%S\r\n\r\nSummary:\r\n%S",
            node->title,
            node->role[0] ? node->role : "agent",
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
    int tabIndex = 0;
    if (!s_mc.hwndInspectText || !s_mc.hwndInspectTabs)
        return;
    tabIndex = TabCtrl_GetCurSel(s_mc.hwndInspectTabs);
    BuildInspectorText(pSnapshot, s_mc.selectedNode, tabIndex, wszText, ARRAYSIZE(wszText));
    SetWindowTextW(s_mc.hwndInspectText, wszText);
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
    WCHAR wszHtml[32768];
    WCHAR wszNode[1024];
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
    ColorToHtml(BikodeTheme_GetColor(BKCLR_APP_BG), wszAppBg, ARRAYSIZE(wszAppBg));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_SURFACE_MAIN), wszCardBg, ARRAYSIZE(wszCardBg));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY), wszText, ARRAYSIZE(wszText));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY), wszMuted, ARRAYSIZE(wszMuted));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_STROKE_SOFT), wszBorder, ARRAYSIZE(wszBorder));
    ColorToHtml(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN), wszAccent, ARRAYSIZE(wszAccent));
    StringCchCopyW(wszHtml, ARRAYSIZE(wszHtml),
        L"<html><head><meta charset='utf-8'><style>"
        L"body{margin:0;font-family:'Segoe UI',sans-serif;background:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszAppBg);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";color:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszText);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";padding:20px 20px 24px;}"
        L".hero{margin-bottom:16px;padding:16px 18px;border:1px solid ");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszBorder);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";border-radius:16px;background:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszCardBg);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";}"
        L".eyebrow{display:inline-block;margin-bottom:8px;padding:4px 10px;border-radius:999px;background:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszAppBg);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";color:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszMuted);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";font-size:12px;letter-spacing:.08em;text-transform:uppercase;}"
        L"h1{margin:0 0 8px;font-size:22px;}"
        L".lede{margin:0;color:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszMuted);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";line-height:1.5;}"
        L".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px;}"
        L".card{border:1px solid ");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszBorder);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";border-radius:16px;padding:14px 16px;background:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszCardBg);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";box-shadow:0 1px 0 rgba(0,0,0,.06);}"
        L".card h3{margin:0 0 8px;font-size:17px;}"
        L".meta,.body{font-size:12px;line-height:1.45;color:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszMuted);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";}"
        L".body{margin-top:10px;font-size:13px;}"
        L".state{display:inline-block;padding:4px 10px;border-radius:999px;background:");
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszAppBg);
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
        L";margin-bottom:10px;font-size:12px;border-left:4px solid var(--accent);}"
        L".accent{height:3px;border-radius:999px;background:var(--accent);margin-top:12px;}"
        L"</style></head><body><div class='hero'><div class='eyebrow'>Map view</div>"
        L"<h1>See the workflow before you drill into a single agent</h1>"
        L"<p class='lede'>Use board view when you want to act. Use map view when you want the big picture.</p>"
        L"</div><div class='grid'>");
    if (pSnapshot->nodeCount <= 0)
    {
        StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
            L"<div class='card' style='--accent:");
        StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszAccent);
        StringCchCatW(wszHtml, ARRAYSIZE(wszHtml),
            L"'><div class='state'>Ready</div><h3>No agents are running yet</h3>"
            L"<div class='body'>Pick a workflow and start a run to populate the map.</div>"
            L"<div class='accent'></div></div>");
    }
    for (int i = 0; i < pSnapshot->nodeCount; i++)
    {
        const AgentNodeSnapshot* node = &pSnapshot->nodes[i];
        WCHAR wszSummary[256];
        WCHAR wszNodeAccent[16];
        Utf8ToWideSafe(node->summary[0] ? node->summary : node->lastAction, wszSummary, ARRAYSIZE(wszSummary));
        ColorToHtml(GetStateAccent(node->state), wszNodeAccent, ARRAYSIZE(wszNodeAccent));
        swprintf_s(wszNode, ARRAYSIZE(wszNode),
            L"<div class='card' style='--accent:%s'><div class='state'>%S</div><h3>%S</h3><div class='meta'>%S / %S</div><div class='body'>%s</div><div class='accent'></div></div>",
            wszNodeAccent,
            AgentRuntime_StateLabel(node->state),
            node->title,
            AgentRuntime_BackendLabel(node->backend),
            AgentRuntime_WorkspaceLabel(node->workspacePolicy),
            wszSummary);
        StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), wszNode);
    }
    StringCchCatW(wszHtml, ARRAYSIZE(wszHtml), L"</div></body></html>");
    __try
    {
        MissionControlWebView_SetHtml(wszHtml);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        s_mc.webViewFaulted = TRUE;
        return;
    }
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
            EnableWindow(s_mc.hwndPause, snapshot->hasRun);
            EnableWindow(s_mc.hwndCancel, snapshot->isRunning || snapshot->hasRun);
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
            EnableWindow(s_mc.hwndDuplicate, canRunWorkflow);
            EnableWindow(s_mc.hwndAddNode, s_mc.hasProjectContext);
            EnableWindow(s_mc.hwndToggleView, FALSE);
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
            L"1. Open a project folder. 2. Pick a workflow. 3. Click Run Workflow.");
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
            L"Choose a workflow, click Run Workflow to start agents, and use Add Agent only to edit the saved workflow.");
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
    heroH = compactHero ? (quickChatOwnRow ? 228 : 208) : 188;
    inspectorW = min(380, max(320, width / 3));
    stackInspector = width < 1240;
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
    controlsTop = s_mc.rcHero.top + 102;
    controlsTop2 = controlsTop + 36;
    controlsTop3 = controlsTop2 + 36;
    hdwp = BeginDeferWindowPos(13);

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
        SetWindowTheme(s_mc.hwndInspectText, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_mc.hwndActivity, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_mc.hwndOrgCombo, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_mc.hwndFallback, L"DarkMode_Explorer", NULL);
        if (ListView_GetHeader(s_mc.hwndBoard))
            SetWindowTheme(ListView_GetHeader(s_mc.hwndBoard), L"DarkMode_Explorer", NULL);
    }
    else
    {
        SetWindowTheme(s_mc.hwndBoard, L"", NULL);
        SetWindowTheme(s_mc.hwndInspectText, L"", NULL);
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

static void RunSelectedOrg(void)
{
    OrgSpec* org = GetSelectedOrg();
    if (!s_mc.hasProjectContext)
    {
        SendMessageW(s_mc.hwndMain, WM_COMMAND, IDM_FILEMGR_OPENFOLDER, 0);
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
    AgentRuntime_Start(org);
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
    HMENU hMenu;
    RECT rc;
    UINT cmd = 0;

    if (!hwndAnchor)
        return 0;

    hMenu = CreatePopupMenu();
    if (!hMenu)
        return 0;

    AppendMenuW(hMenu, MF_STRING, IDM_MC_ADD_RESEARCH, L"Research Agent");
    AppendMenuW(hMenu, MF_STRING, IDM_MC_ADD_REVIEW, L"Review Agent");
    AppendMenuW(hMenu, MF_STRING, IDM_MC_ADD_VALIDATE, L"Validation Agent");

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
    node->backend = AGENT_BACKEND_API;
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
        s_mc.hwndInspectText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_INSPECT_TEXT, NULL, NULL);
        s_mc.hwndOpenFile = CreateWindowExW(0, L"BUTTON", L"Open File", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_FILE, NULL, NULL);
        s_mc.hwndOpenWorkspace = CreateWindowExW(0, L"BUTTON", L"Workspace", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_WORKSPACE, NULL, NULL);
        s_mc.hwndOpenTranscript = CreateWindowExW(0, L"BUTTON", L"Transcript", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_TRANSCRIPT, NULL, NULL);
        s_mc.hwndOpenProof = CreateWindowExW(0, L"BUTTON", L"Review", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_OPEN_PROOF, NULL, NULL);
        s_mc.hwndActivity = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_MC_ACTIVITY, NULL, NULL);

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
            SetThemeFont(s_mc.hwndInspectText, BKFONT_MONO_SMALL);
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
        if (ctrlId == IDC_MC_INSPECT_TEXT || ctrlId == IDC_MC_ACTIVITY || ctrlId == IDC_MC_ORG_COMBO || ctrlId == IDC_MC_FALLBACK)
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
                AgentRuntime_SetPaused(!snapshot->isPaused);
                FreeSnapshot(snapshot);
            }
        }
            return 0;
        case IDC_MC_CANCEL:
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
                    else if (_wcsicmp(wszState, L"Blocked") == 0 || _wcsicmp(wszState, L"Paused") == 0)
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
                if (pEvent->type == AGENT_EVENT_HANDOFF || pEvent->type == AGENT_EVENT_ERROR)
                {
                    WCHAR wszTitle[128];
                    WCHAR wszDetails[2048];
                    Utf8ToWideSafe(pEvent->nodeTitle[0] ? pEvent->nodeTitle : pEvent->nodeId, wszTitle, ARRAYSIZE(wszTitle));
                    Utf8ToWideSafe(pEvent->message, wszDetails, ARRAYSIZE(wszDetails));
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
