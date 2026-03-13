#pragma once
/******************************************************************************
*
* Biko
*
* AgentRuntime.h
*   Mission Control runtime and org-spec orchestration for multi-agent runs.
*
******************************************************************************/

#include <wtypes.h>
#include "AIBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WM_AGENT_RUNTIME_EVENT  (WM_USER + 0x750)

#define AGENT_RUNTIME_MAX_NODES          16
#define AGENT_RUNTIME_MAX_DEPENDS         8
#define AGENT_RUNTIME_MAX_TOOLS           8
#define AGENT_RUNTIME_MAX_CHANGED_FILES  32
#define AGENT_RUNTIME_MAX_EVENTS        256
#define AGENT_RUNTIME_TEXT_SMALL        64
#define AGENT_RUNTIME_TEXT_MEDIUM      128
#define AGENT_RUNTIME_TEXT_LARGE       512
#define AGENT_RUNTIME_TEXT_HUGE       4096

typedef enum AgentWorkspacePolicy {
    AGENT_WORKSPACE_ISOLATED = 0,
    AGENT_WORKSPACE_SHARED_READONLY,
    AGENT_WORKSPACE_SHARED_MUTATING
} AgentWorkspacePolicy;

typedef enum AgentBackend {
    AGENT_BACKEND_API = 0,
    AGENT_BACKEND_CODEX,
    AGENT_BACKEND_CLAUDE,
    AGENT_BACKEND_RELAY,
    AGENT_BACKEND_LOCAL
} AgentBackend;

typedef enum AgentNodeState {
    AGENT_NODE_IDLE = 0,
    AGENT_NODE_QUEUED,
    AGENT_NODE_BLOCKED,
    AGENT_NODE_RUNNING,
    AGENT_NODE_DONE,
    AGENT_NODE_ERROR,
    AGENT_NODE_CANCELED,
    AGENT_NODE_PAUSED
} AgentNodeState;

typedef enum AgentEventType {
    AGENT_EVENT_SYSTEM = 0,
    AGENT_EVENT_STATUS,
    AGENT_EVENT_TOOL,
    AGENT_EVENT_FILE,
    AGENT_EVENT_HANDOFF,
    AGENT_EVENT_ERROR,
    AGENT_EVENT_METRIC
} AgentEventType;

typedef struct OrgNodeSpec {
    char id[AGENT_RUNTIME_TEXT_SMALL];
    char title[AGENT_RUNTIME_TEXT_MEDIUM];
    char role[AGENT_RUNTIME_TEXT_SMALL];
    AgentBackend backend;
    char model[AGENT_RUNTIME_TEXT_SMALL];
    char prompt[AGENT_RUNTIME_TEXT_HUGE];
    AgentWorkspacePolicy workspacePolicy;
    char dependsOn[AGENT_RUNTIME_MAX_DEPENDS][AGENT_RUNTIME_TEXT_SMALL];
    int dependsOnCount;
    char tools[AGENT_RUNTIME_MAX_TOOLS][AGENT_RUNTIME_TEXT_SMALL];
    int toolCount;
    char group[AGENT_RUNTIME_TEXT_SMALL];
} OrgNodeSpec;

typedef struct OrgSpec {
    int version;
    WCHAR path[MAX_PATH];
    WCHAR root[MAX_PATH];
    char name[AGENT_RUNTIME_TEXT_MEDIUM];
    char layout[AGENT_RUNTIME_TEXT_SMALL];
    AgentWorkspacePolicy defaultWorkspacePolicy;
    OrgNodeSpec nodes[AGENT_RUNTIME_MAX_NODES];
    int nodeCount;
} OrgSpec;

typedef struct AgentEvent {
    int sequence;
    AgentEventType type;
    DWORD tick;
    char nodeId[AGENT_RUNTIME_TEXT_SMALL];
    char nodeTitle[AGENT_RUNTIME_TEXT_MEDIUM];
    char message[AGENT_RUNTIME_TEXT_HUGE];
} AgentEvent;

typedef struct AgentNodeSnapshot {
    char id[AGENT_RUNTIME_TEXT_SMALL];
    char title[AGENT_RUNTIME_TEXT_MEDIUM];
    char role[AGENT_RUNTIME_TEXT_SMALL];
    char group[AGENT_RUNTIME_TEXT_SMALL];
    AgentBackend backend;
    AgentWorkspacePolicy workspacePolicy;
    AgentNodeState state;
    DWORD startTick;
    DWORD endTick;
    DWORD lastUpdateTick;
    int toolCount;
    int fileCount;
    int inputTokens;
    int outputTokens;
    WCHAR workspacePath[MAX_PATH];
    WCHAR transcriptPath[MAX_PATH];
    WCHAR summaryPath[MAX_PATH];
    char lastAction[AGENT_RUNTIME_TEXT_MEDIUM];
    char summary[AGENT_RUNTIME_TEXT_HUGE];
    WCHAR changedFiles[AGENT_RUNTIME_MAX_CHANGED_FILES][MAX_PATH];
} AgentNodeSnapshot;

typedef struct AgentRuntimeSnapshot {
    BOOL hasRun;
    BOOL isRunning;
    BOOL isPaused;
    BOOL isCanceled;
    WCHAR workspaceRoot[MAX_PATH];
    WCHAR runRoot[MAX_PATH];
    char runId[AGENT_RUNTIME_TEXT_SMALL];
    OrgSpec org;
    AgentNodeSnapshot nodes[AGENT_RUNTIME_MAX_NODES];
    int nodeCount;
    AgentEvent events[AGENT_RUNTIME_MAX_EVENTS];
    int eventCount;
    int activeNodeCount;
} AgentRuntimeSnapshot;

BOOL AgentRuntime_Init(HWND hwndMain);
void AgentRuntime_Shutdown(void);

BOOL AgentRuntime_EnsureWorkspaceAssets(const WCHAR* wszWorkspaceRoot);
BOOL AgentRuntime_LoadOrgSpecs(const WCHAR* wszWorkspaceRoot, OrgSpec* pSpecs, int cMaxSpecs, int* pnSpecs);
BOOL AgentRuntime_SaveOrgSpec(const OrgSpec* pSpec);
BOOL AgentRuntime_CreateDraftOrgSpec(const WCHAR* wszWorkspaceRoot, WCHAR* wszOutPath, int cchOutPath);
BOOL AgentRuntime_DuplicateOrgSpec(const OrgSpec* pSpec, WCHAR* wszOutPath, int cchOutPath);
BOOL AgentRuntime_AddDefaultNode(OrgSpec* pSpec);

BOOL AgentRuntime_Start(const OrgSpec* pSpec);
void AgentRuntime_Cancel(void);
void AgentRuntime_SetPaused(BOOL bPaused);
BOOL AgentRuntime_IsRunning(void);
BOOL AgentRuntime_GetSnapshot(AgentRuntimeSnapshot* pSnapshot);
BOOL AgentRuntime_GetLastSelectedOrg(WCHAR* wszPath, int cchPath);
void AgentRuntime_SetLastSelectedOrg(LPCWSTR wszPath);

const WCHAR* AgentRuntime_GetWorkspaceRoot(void);
void AgentRuntime_OpenPathInExplorer(LPCWSTR wszPath);

const char* AgentRuntime_BackendLabel(AgentBackend backend);
const char* AgentRuntime_WorkspaceLabel(AgentWorkspacePolicy policy);
const char* AgentRuntime_StateLabel(AgentNodeState state);

#ifdef __cplusplus
}
#endif
