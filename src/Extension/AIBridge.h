#pragma once
/******************************************************************************
*
* Biko
*
* AIBridge.h
*   Named pipe client for communication with the AI engine (biko-engine.exe).
*   Handles connection lifecycle, request/response serialization,
*   and asynchronous I/O with PostMessage-based callback to main thread.
*
******************************************************************************/

#include <wtypes.h>
#include "AIProvider.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Custom window messages for AI communication
//=============================================================================

#define WM_AI_RESPONSE      (WM_USER + 100)
#define WM_AI_STATUS        (WM_USER + 101)
#define WM_AI_CONNECTED     (WM_USER + 102)
#define WM_AI_DISCONNECTED  (WM_USER + 103)
#define WM_AI_CHUNK         (WM_USER + 104)

//=============================================================================
// AI Engine connection status
//=============================================================================

typedef enum
{
    AI_STATUS_OFFLINE = 0,
    AI_STATUS_CONNECTING,
    AI_STATUS_READY,
    AI_STATUS_THINKING,
    AI_STATUS_PATCH_READY,
    AI_STATUS_ERROR
} EAIStatus;

//=============================================================================
// Request types
//=============================================================================

typedef enum
{
    AI_REQ_PATCH = 0,
    AI_REQ_HINT,
    AI_REQ_EXPLAIN,
    AI_REQ_CONTEXT_SYNC,
    AI_REQ_PING,
    AI_REQ_CHAT,
    AI_REQ_SHUTDOWN
} EAIRequestType;

//=============================================================================
// Intent actions
//=============================================================================

typedef enum
{
    AI_ACTION_REFACTOR = 0,
    AI_ACTION_FIX,
    AI_ACTION_EXPLAIN,
    AI_ACTION_COMPLETE,
    AI_ACTION_TRANSFORM,
    AI_ACTION_CHAT,
    AI_ACTION_CUSTOM
} EAIAction;

//=============================================================================
// Intent scope
//=============================================================================

typedef enum
{
    AI_SCOPE_SELECTION = 0,
    AI_SCOPE_FUNCTION,
    AI_SCOPE_FILE,
    AI_SCOPE_PROJECT
} EAIScope;

//=============================================================================
// AI Request structure
//=============================================================================

typedef struct TAIRequest
{
    UINT        uRequestId;
    EAIRequestType eType;
    EAIAction   eAction;
    EAIScope    eScope;
    char*       pszInstruction;   // user instruction (UTF-8, heap-allocated)

    // Context (pointers into AIContext data â€” do not free individually)
    char*       pszFilePath;      // current file path (UTF-8)
    char*       pszFileContent;   // current file content (UTF-8)
    char*       pszLanguage;      // language id string
    char*       pszSelectedText;  // selected text (UTF-8, may be NULL)
    int         iCursorLine;      // 0-based
    int         iCursorCol;       // 0-based
    int         iSelStartLine;
    int         iSelStartCol;
    int         iSelEndLine;
    int         iSelEndCol;
    int         iFirstVisibleLine;
    int         iLastVisibleLine;

    // Repo atlas / mission context
    char*       pszProjectRoot;      // detected project root (UTF-8)
    char*       pszActiveFiles;      // newline-delimited active/relevant files
    char*       pszGitSummary;       // short git summary for atlas
    char*       pszDiagnostics;      // lightweight diagnostics summary
    char*       pszAtlasSummary;     // compact atlas card summary
    char*       pszBuildCommand;     // remembered or inferred build command
    char*       pszTestCommand;      // remembered or inferred test command
    char*       pszHotZones;         // newline-delimited hot zones / likely symbols
    char*       pszBufferHash;       // hash of current buffer content
    UINT        uBufferVersion;      // lightweight version tag for freshness checks

    // Chat-specific
    char*       pszChatMessage;   // user chat message (UTF-8)
} AIRequest;

//=============================================================================
// Patch hunk (parsed from response)
//=============================================================================

typedef struct TAIPatchHunk
{
    int     iOldStart;      // original file line (1-based)
    int     iOldCount;      // number of lines removed
    int     iNewStart;      // new file line (1-based)
    int     iNewCount;      // number of lines added
    char*   pszOldLines;    // original lines (for context matching)
    char*   pszNewLines;    // replacement lines
    BOOL    bSelected;      // user toggle for partial apply
    BOOL    bApplied;       // already applied
    BOOL    bFailed;        // context mismatch
} AIPatchHunk;

//=============================================================================
// AI Patch (per-file)
//=============================================================================

typedef struct TAIPatch
{
    char*           pszFilePath;    // target file (UTF-8)
    char*           pszRawDiff;     // full unified diff text
    char*           pszDescription; // AI-provided description
    char*           pszProofSummary;
    char*           pszTouchedSymbols;
    char*           pszAssumptions;
    char*           pszValidations;
    char*           pszReviewerVotes;
    char*           pszResidualRisk;
    char*           pszRollbackFingerprint;
    char*           pszBaseBufferHash;
    double          dConfidence;
    UINT            uBaseBufferVersion;
    int             iCandidateRank;
    BOOL            bGhostLayer;
    BOOL            bStale;
    AIPatchHunk*    pHunks;
    int             iHunkCount;
    BOOL            bSelected;      // user toggle for multi-file
} AIPatch;

//=============================================================================
// AI Response structure
//=============================================================================

typedef struct TAIResponse
{
    UINT        uRequestId;         // echoed from request
    BOOL        bSuccess;
    BOOL        bPartial;           // streaming chunk

    // Patches (for patch requests)
    AIPatch*    pPatches;
    int         iPatchCount;

    // Hint
    char*       pszHintText;
    int         iHintLine;
    int         iHintCol;
    double      dHintConfidence;

    // Explanation
    char*       pszExplanation;
    char*       pszExplanationDetails;

    // Chat response
    char*       pszChatResponse;

    // Mission / proof metadata
    char*       pszMissionId;
    char*       pszMissionPhase;
    char*       pszMissionSummary;
    char*       pszMissionQueue;
    char*       pszAtlasSummary;
    char*       pszProofSummary;
    char*       pszBuildCommand;
    char*       pszTestCommand;
    char*       pszScratchpadSummary;

    // Error
    char*       pszErrorCode;
    char*       pszErrorMessage;

    // Metadata
    char*       pszModel;
    int         iInputTokens;
    int         iOutputTokens;
    int         iLatencyMs;

    // Streaming chunk
    char*       pszChunk;
} AIResponse;

//=============================================================================
// Configuration
//=============================================================================

typedef struct TAIConfig
{
    BOOL    bEnabled;
    WCHAR   wszEnginePath[MAX_PATH];
    WCHAR   wszPipeName[128];
    DWORD   dwTimeoutMs;
    double  dHintConfidenceThreshold;
    BOOL    bShowTokenCost;
    int     iMaxContextFiles;
    BOOL    bAutoStartEngine;

    // Provider configuration (multi-provider support)
    AIProviderConfig providerCfg;    // active provider + model + key + params

    // Legacy fields (kept for backward compatibility with INI)
    WCHAR   wszApiKey[512];         // for cloud backends (maps to providerCfg.szApiKey)
    WCHAR   wszApiEndpoint[512];
    WCHAR   wszModelName[128];
} AIConfig;

//=============================================================================
// Public API
//=============================================================================

// Lifecycle
BOOL    AIBridge_Init(HWND hwndMain, const AIConfig* pConfig);
void    AIBridge_Shutdown(void);
BOOL    AIBridge_IsConnected(void);
EAIStatus AIBridge_GetStatus(void);
const WCHAR* AIBridge_GetStatusText(void);

// Engine management
BOOL    AIBridge_StartEngine(void);
BOOL    AIBridge_StopEngine(void);
BOOL    AIBridge_RestartEngine(void);

// Connection
BOOL    AIBridge_Connect(void);
void    AIBridge_Disconnect(void);

// Requests (async â€” response arrives via WM_AI_RESPONSE)
UINT    AIBridge_SendRequest(const AIRequest* pReq);
BOOL    AIBridge_CancelRequest(UINT uRequestId);
BOOL    AIBridge_CancelAll(void);

// Response handling (call from WM_AI_RESPONSE handler)
AIResponse* AIBridge_GetResponse(UINT uRequestId);
void    AIBridge_FreeResponse(AIResponse* pResp);

// Request helpers
AIRequest*  AIBridge_CreateRequest(EAIRequestType eType, EAIAction eAction);
void    AIBridge_FreeRequest(AIRequest* pReq);

// Configuration
void    AIBridge_LoadConfig(AIConfig* pConfig, LPCWSTR wszIniFile);
void    AIBridge_SaveConfig(const AIConfig* pConfig, LPCWSTR wszIniFile);
void    AIBridge_ApplyConfig(const AIConfig* pConfig);

// Provider management
BOOL    AIBridge_SetProvider(EAIProvider eProvider, const char* szModel, const char* szApiKey);
BOOL    AIBridge_SendConfigToEngine(const AIProviderConfig* pCfg);
const AIProviderConfig* AIBridge_GetProviderConfig(void);

#ifdef __cplusplus
}
#endif
