/******************************************************************************
*
* Biko
*
* AIBridge.c
*   Named pipe client for AI engine communication.
*   Runs a background thread for pipe I/O, posts results to main window.
*
******************************************************************************/

#include "AIBridge.h"
#include "AIProvider.h"
#include "CommonUtils.h"
#include "mono_json.h"
#include <stdio.h>
#include <string.h>
#include <process.h>

//=============================================================================
// Internal state
//=============================================================================

static HWND         s_hwndMain = NULL;
static HANDLE       s_hPipe = INVALID_HANDLE_VALUE;
static HANDLE       s_hThread = NULL;
static HANDLE       s_hEngineProcess = NULL;
static volatile LONG s_eStatus = AI_STATUS_OFFLINE;
static volatile LONG s_bShutdown = FALSE;
static UINT         s_uNextRequestId = 1;
static AIConfig     s_config;
static CRITICAL_SECTION s_csRequests;
static CRITICAL_SECTION s_csResponses;
static WCHAR        s_wszPipeFullName[256];

// Pending response queue (ring buffer)
#define MAX_PENDING_RESPONSES 64
static AIResponse*  s_pResponses[MAX_PENDING_RESPONSES];
static int          s_iResponseHead = 0;
static int          s_iResponseTail = 0;

// Read buffer for pipe
#define PIPE_READ_BUFFER_SIZE (256 * 1024)
static char*        s_pReadBuffer = NULL;
static int          s_iReadBufferLen = 0;

//=============================================================================
// Forward declarations
//=============================================================================

static unsigned __stdcall AIBridge_ThreadProc(void* pArg);
static BOOL AIBridge_ConnectPipe(void);
static void AIBridge_DisconnectPipe(void);
static BOOL AIBridge_WriteMessage(const char* pJson, int iJsonLen);
static BOOL AIBridge_ReadMessage(char** ppJson, int* piJsonLen);
static char* AIBridge_SerializeRequest(const AIRequest* pReq, int* piLen);
static AIResponse* AIBridge_DeserializeResponse(const char* pJson, int iJsonLen);
static void AIBridge_QueueResponse(AIResponse* pResp);
static void AIBridge_SetStatus(EAIStatus eStatus);

//=============================================================================
// Lifecycle
//=============================================================================

BOOL AIBridge_Init(HWND hwndMain, const AIConfig* pConfig)
{
    s_hwndMain = hwndMain;
    if (pConfig)
        memcpy(&s_config, pConfig, sizeof(AIConfig));

    InitializeCriticalSection(&s_csRequests);
    InitializeCriticalSection(&s_csResponses);

    s_pReadBuffer = (char*)n2e_Alloc(PIPE_READ_BUFFER_SIZE);
    if (!s_pReadBuffer) return FALSE;

    // Build pipe name
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    if (s_config.wszPipeName[0])
    {
        swprintf(s_wszPipeFullName, 256, L"\\\\.\\pipe\\%s_%llX",
                 s_config.wszPipeName, li.QuadPart);
    }
    else
    {
        swprintf(s_wszPipeFullName, 256, L"\\\\.\\pipe\\biko_ai_%llX",
                 li.QuadPart);
    }

    if (s_config.bEnabled && s_config.bAutoStartEngine)
    {
        AIBridge_StartEngine();
    }

    return TRUE;
}

void AIBridge_Shutdown(void)
{
    InterlockedExchange(&s_bShutdown, TRUE);

    // Send shutdown request to engine
    if (s_hPipe != INVALID_HANDLE_VALUE)
    {
        const char* shutdownMsg = "{\"type\":\"shutdown\",\"version\":1}";
        AIBridge_WriteMessage(shutdownMsg, (int)strlen(shutdownMsg));
    }

    AIBridge_DisconnectPipe();

    // Wait for thread
    if (s_hThread)
    {
        WaitForSingleObject(s_hThread, 3000);
        CloseHandle(s_hThread);
        s_hThread = NULL;
    }

    // Stop engine process
    AIBridge_StopEngine();

    // Free response queue
    EnterCriticalSection(&s_csResponses);
    for (int i = 0; i < MAX_PENDING_RESPONSES; i++)
    {
        if (s_pResponses[i])
        {
            AIBridge_FreeResponse(s_pResponses[i]);
            s_pResponses[i] = NULL;
        }
    }
    LeaveCriticalSection(&s_csResponses);

    if (s_pReadBuffer)
    {
        n2e_Free(s_pReadBuffer);
        s_pReadBuffer = NULL;
    }

    DeleteCriticalSection(&s_csRequests);
    DeleteCriticalSection(&s_csResponses);

    s_hwndMain = NULL;
    AIBridge_SetStatus(AI_STATUS_OFFLINE);
}

BOOL AIBridge_IsConnected(void)
{
    return s_hPipe != INVALID_HANDLE_VALUE;
}

EAIStatus AIBridge_GetStatus(void)
{
    return (EAIStatus)InterlockedCompareExchange(&s_eStatus, 0, 0);
}

const WCHAR* AIBridge_GetStatusText(void)
{
    switch (AIBridge_GetStatus())
    {
    case AI_STATUS_OFFLINE:     return L"AI: offline";
    case AI_STATUS_CONNECTING:  return L"AI: connecting...";
    case AI_STATUS_READY:       return L"AI: ready";
    case AI_STATUS_THINKING:    return L"AI: thinking...";
    case AI_STATUS_PATCH_READY: return L"AI: patch ready";
    case AI_STATUS_ERROR:       return L"AI: error";
    default:                    return L"AI: unknown";
    }
}

//=============================================================================
// Engine management
//=============================================================================

BOOL AIBridge_StartEngine(void)
{
    if (s_hEngineProcess) return TRUE; // already running
    if (!s_config.wszEnginePath[0]) return FALSE;

    // Build command line with provider info
    const AIProviderDef* pDef = AIProvider_Get(s_config.providerCfg.eProvider);
    WCHAR wszProvider[64] = L"openai";
    WCHAR wszModel[128] = {0};
    if (pDef)
        MultiByteToWideChar(CP_UTF8, 0, pDef->szSlug, -1, wszProvider, 64);
    if (s_config.providerCfg.szModel[0])
        MultiByteToWideChar(CP_UTF8, 0, s_config.providerCfg.szModel, -1, wszModel, 128);

    WCHAR wszCmdLine[MAX_PATH * 2];
    if (wszModel[0])
    {
        swprintf(wszCmdLine, MAX_PATH * 2,
                 L"\"%s\" --pipe \"%s\" --provider %s --model \"%s\"",
                 s_config.wszEnginePath, s_wszPipeFullName, wszProvider, wszModel);
    }
    else
    {
        swprintf(wszCmdLine, MAX_PATH * 2,
                 L"\"%s\" --pipe \"%s\" --provider %s",
                 s_config.wszEnginePath, s_wszPipeFullName, wszProvider);
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, wszCmdLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        return FALSE;
    }

    s_hEngineProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    // Give engine time to create pipe
    Sleep(500);

    // Start connection thread
    return AIBridge_Connect();
}

BOOL AIBridge_StopEngine(void)
{
    if (s_hEngineProcess)
    {
        // Wait briefly, then terminate
        if (WaitForSingleObject(s_hEngineProcess, 2000) == WAIT_TIMEOUT)
        {
            TerminateProcess(s_hEngineProcess, 0);
        }
        CloseHandle(s_hEngineProcess);
        s_hEngineProcess = NULL;
    }
    return TRUE;
}

BOOL AIBridge_RestartEngine(void)
{
    AIBridge_Disconnect();
    AIBridge_StopEngine();
    Sleep(200);
    return AIBridge_StartEngine();
}

//=============================================================================
// Connection
//=============================================================================

BOOL AIBridge_Connect(void)
{
    if (s_hThread) return TRUE; // already connecting/connected

    InterlockedExchange(&s_bShutdown, FALSE);
    AIBridge_SetStatus(AI_STATUS_CONNECTING);

    s_hThread = (HANDLE)_beginthreadex(NULL, 0, AIBridge_ThreadProc, NULL, 0, NULL);
    return s_hThread != NULL;
}

void AIBridge_Disconnect(void)
{
    InterlockedExchange(&s_bShutdown, TRUE);
    AIBridge_DisconnectPipe();

    if (s_hThread)
    {
        WaitForSingleObject(s_hThread, 3000);
        CloseHandle(s_hThread);
        s_hThread = NULL;
    }
    AIBridge_SetStatus(AI_STATUS_OFFLINE);
}

//=============================================================================
// Requests
//=============================================================================

UINT AIBridge_SendRequest(const AIRequest* pReq)
{
    if (!AIBridge_IsConnected()) return 0;

    int iJsonLen = 0;
    char* pJson = AIBridge_SerializeRequest(pReq, &iJsonLen);
    if (!pJson) return 0;

    BOOL bOk = AIBridge_WriteMessage(pJson, iJsonLen);
    n2e_Free(pJson);

    if (bOk)
    {
        AIBridge_SetStatus(AI_STATUS_THINKING);
        return ((AIRequest*)pReq)->uRequestId;
    }
    return 0;
}

BOOL AIBridge_CancelRequest(UINT uRequestId)
{
    // Send cancel message to engine
    if (!AIBridge_IsConnected()) return FALSE;

    JsonWriter w;
    if (!JsonWriter_Init(&w, 256)) return FALSE;

    JsonWriter_BeginObject(&w);
    JsonWriter_String(&w, "type", "cancel");
    JsonWriter_Int(&w, "version", 1);
    JsonWriter_Int(&w, "requestId", (int)uRequestId);
    JsonWriter_EndObject(&w);

    BOOL bOk = AIBridge_WriteMessage(JsonWriter_GetBuffer(&w), JsonWriter_GetLength(&w));
    JsonWriter_Free(&w);

    if (bOk) AIBridge_SetStatus(AI_STATUS_READY);
    return bOk;
}

BOOL AIBridge_CancelAll(void)
{
    if (!AIBridge_IsConnected()) return FALSE;

    const char* msg = "{\"type\":\"cancel_all\",\"version\":1}";
    BOOL bOk = AIBridge_WriteMessage(msg, (int)strlen(msg));
    if (bOk) AIBridge_SetStatus(AI_STATUS_READY);
    return bOk;
}

//=============================================================================
// Response handling
//=============================================================================

AIResponse* AIBridge_GetResponse(UINT uRequestId)
{
    EnterCriticalSection(&s_csResponses);

    for (int i = 0; i < MAX_PENDING_RESPONSES; i++)
    {
        if (s_pResponses[i] && s_pResponses[i]->uRequestId == uRequestId)
        {
            AIResponse* pResp = s_pResponses[i];
            s_pResponses[i] = NULL;
            LeaveCriticalSection(&s_csResponses);
            return pResp;
        }
    }

    LeaveCriticalSection(&s_csResponses);
    return NULL;
}

void AIBridge_FreeResponse(AIResponse* pResp)
{
    if (!pResp) return;

    // Free patches
    if (pResp->pPatches)
    {
        for (int i = 0; i < pResp->iPatchCount; i++)
        {
            AIPatch* p = &pResp->pPatches[i];
            if (p->pszFilePath) n2e_Free(p->pszFilePath);
            if (p->pszRawDiff) n2e_Free(p->pszRawDiff);
            if (p->pszDescription) n2e_Free(p->pszDescription);
            if (p->pszProofSummary) n2e_Free(p->pszProofSummary);
            if (p->pszTouchedSymbols) n2e_Free(p->pszTouchedSymbols);
            if (p->pszAssumptions) n2e_Free(p->pszAssumptions);
            if (p->pszValidations) n2e_Free(p->pszValidations);
            if (p->pszReviewerVotes) n2e_Free(p->pszReviewerVotes);
            if (p->pszResidualRisk) n2e_Free(p->pszResidualRisk);
            if (p->pszRollbackFingerprint) n2e_Free(p->pszRollbackFingerprint);
            if (p->pszBaseBufferHash) n2e_Free(p->pszBaseBufferHash);
            if (p->pHunks)
            {
                for (int j = 0; j < p->iHunkCount; j++)
                {
                    if (p->pHunks[j].pszOldLines) n2e_Free(p->pHunks[j].pszOldLines);
                    if (p->pHunks[j].pszNewLines) n2e_Free(p->pHunks[j].pszNewLines);
                }
                n2e_Free(p->pHunks);
            }
        }
        n2e_Free(pResp->pPatches);
    }

    if (pResp->pszHintText) n2e_Free(pResp->pszHintText);
    if (pResp->pszExplanation) n2e_Free(pResp->pszExplanation);
    if (pResp->pszExplanationDetails) n2e_Free(pResp->pszExplanationDetails);
    if (pResp->pszChatResponse) n2e_Free(pResp->pszChatResponse);
    if (pResp->pszMissionId) n2e_Free(pResp->pszMissionId);
    if (pResp->pszMissionPhase) n2e_Free(pResp->pszMissionPhase);
    if (pResp->pszMissionSummary) n2e_Free(pResp->pszMissionSummary);
    if (pResp->pszMissionQueue) n2e_Free(pResp->pszMissionQueue);
    if (pResp->pszAtlasSummary) n2e_Free(pResp->pszAtlasSummary);
    if (pResp->pszProofSummary) n2e_Free(pResp->pszProofSummary);
    if (pResp->pszBuildCommand) n2e_Free(pResp->pszBuildCommand);
    if (pResp->pszTestCommand) n2e_Free(pResp->pszTestCommand);
    if (pResp->pszScratchpadSummary) n2e_Free(pResp->pszScratchpadSummary);
    if (pResp->pszErrorCode) n2e_Free(pResp->pszErrorCode);
    if (pResp->pszErrorMessage) n2e_Free(pResp->pszErrorMessage);
    if (pResp->pszModel) n2e_Free(pResp->pszModel);
    if (pResp->pszChunk) n2e_Free(pResp->pszChunk);

    n2e_Free(pResp);
}

//=============================================================================
// Request creation helpers
//=============================================================================

AIRequest* AIBridge_CreateRequest(EAIRequestType eType, EAIAction eAction)
{
    AIRequest* pReq = (AIRequest*)n2e_Alloc(sizeof(AIRequest));
    if (!pReq) return NULL;
    ZeroMemory(pReq, sizeof(AIRequest));
    pReq->uRequestId = InterlockedIncrement((LONG*)&s_uNextRequestId);
    pReq->eType = eType;
    pReq->eAction = eAction;
    pReq->eScope = AI_SCOPE_SELECTION;
    return pReq;
}

void AIBridge_FreeRequest(AIRequest* pReq)
{
    if (!pReq) return;
    if (pReq->pszInstruction) n2e_Free(pReq->pszInstruction);
    if (pReq->pszFilePath) n2e_Free(pReq->pszFilePath);
    if (pReq->pszFileContent) n2e_Free(pReq->pszFileContent);
    if (pReq->pszLanguage) n2e_Free(pReq->pszLanguage);
    if (pReq->pszSelectedText) n2e_Free(pReq->pszSelectedText);
    if (pReq->pszProjectRoot) n2e_Free(pReq->pszProjectRoot);
    if (pReq->pszActiveFiles) n2e_Free(pReq->pszActiveFiles);
    if (pReq->pszGitSummary) n2e_Free(pReq->pszGitSummary);
    if (pReq->pszDiagnostics) n2e_Free(pReq->pszDiagnostics);
    if (pReq->pszAtlasSummary) n2e_Free(pReq->pszAtlasSummary);
    if (pReq->pszBuildCommand) n2e_Free(pReq->pszBuildCommand);
    if (pReq->pszTestCommand) n2e_Free(pReq->pszTestCommand);
    if (pReq->pszHotZones) n2e_Free(pReq->pszHotZones);
    if (pReq->pszBufferHash) n2e_Free(pReq->pszBufferHash);
    if (pReq->pszChatMessage) n2e_Free(pReq->pszChatMessage);
    n2e_Free(pReq);
}

//=============================================================================
// Configuration
//=============================================================================

void AIBridge_LoadConfig(AIConfig* pConfig, LPCWSTR wszIniFile)
{
    ZeroMemory(pConfig, sizeof(AIConfig));
    pConfig->bEnabled = GetPrivateProfileIntW(L"AI", L"Enabled", 1, wszIniFile);
    GetPrivateProfileStringW(L"AI", L"EnginePath", L"biko-engine.exe",
                             pConfig->wszEnginePath, MAX_PATH, wszIniFile);
    GetPrivateProfileStringW(L"AI", L"PipeName", L"biko_ai",
                             pConfig->wszPipeName, 128, wszIniFile);
    pConfig->dwTimeoutMs = GetPrivateProfileIntW(L"AI", L"TimeoutMs", 30000, wszIniFile);
    pConfig->dHintConfidenceThreshold = 0.7;
    pConfig->bShowTokenCost = GetPrivateProfileIntW(L"AI", L"ShowTokenCost", 0, wszIniFile);
    pConfig->iMaxContextFiles = GetPrivateProfileIntW(L"AI", L"MaxContextFiles", 10, wszIniFile);
    pConfig->bAutoStartEngine = GetPrivateProfileIntW(L"AI", L"AutoStartEngine", 1, wszIniFile);

    // Legacy fields
    GetPrivateProfileStringW(L"AI", L"ApiEndpoint", L"https://api.openai.com/v1",
                             pConfig->wszApiEndpoint, 512, wszIniFile);
    GetPrivateProfileStringW(L"AI", L"ApiKey", L"",
                             pConfig->wszApiKey, 512, wszIniFile);
    GetPrivateProfileStringW(L"AI", L"Model", L"gpt-4o-mini",
                             pConfig->wszModelName, 128, wszIniFile);

    // Provider config
    {
        char szProviderSlug[64] = "openai";
        WCHAR wszSlug[64];
        GetPrivateProfileStringW(L"AI", L"Provider", L"openai",
                                 wszSlug, 64, wszIniFile);
        WideCharToMultiByte(CP_UTF8, 0, wszSlug, -1, szProviderSlug, 64, NULL, NULL);

        const AIProviderDef* pDef = AIProvider_FindBySlug(szProviderSlug);
        EAIProvider eProvider = pDef ? pDef->id : AI_PROVIDER_OPENAI;
        AIProviderConfig_InitDefaults(&pConfig->providerCfg, eProvider);

        // Model from INI
        WCHAR wszModel[128];
        GetPrivateProfileStringW(L"AI", L"ProviderModel", L"",
                                 wszModel, 128, wszIniFile);
        if (wszModel[0])
            WideCharToMultiByte(CP_UTF8, 0, wszModel, -1,
                                pConfig->providerCfg.szModel,
                                sizeof(pConfig->providerCfg.szModel), NULL, NULL);

        // API key from INI (if set)
        if (pConfig->wszApiKey[0])
        {
            WideCharToMultiByte(CP_UTF8, 0, pConfig->wszApiKey, -1,
                                pConfig->providerCfg.szApiKey,
                                sizeof(pConfig->providerCfg.szApiKey), NULL, NULL);
        }

        // Temperature
        WCHAR wszTemp[32];
        GetPrivateProfileStringW(L"AI", L"Temperature", L"0.2",
                                 wszTemp, 32, wszIniFile);
        char szTemp[32];
        WideCharToMultiByte(CP_UTF8, 0, wszTemp, -1, szTemp, 32, NULL, NULL);
        pConfig->providerCfg.dTemperature = atof(szTemp);

        // Max tokens
        pConfig->providerCfg.iMaxTokens =
            GetPrivateProfileIntW(L"AI", L"MaxTokens", 4096, wszIniFile);

        // Custom host override
        WCHAR wszHost[512];
        GetPrivateProfileStringW(L"AI", L"ProviderHost", L"",
                                 wszHost, 512, wszIniFile);
        if (wszHost[0])
            WideCharToMultiByte(CP_UTF8, 0, wszHost, -1,
                                pConfig->providerCfg.szHost,
                                sizeof(pConfig->providerCfg.szHost), NULL, NULL);

        // Custom port
        int port = GetPrivateProfileIntW(L"AI", L"ProviderPort", 0, wszIniFile);
        if (port > 0)
            pConfig->providerCfg.iPort = port;
    }
}

void AIBridge_SaveConfig(const AIConfig* pConfig, LPCWSTR wszIniFile)
{
    WCHAR buf[32];
    swprintf(buf, 32, L"%d", pConfig->bEnabled);
    WritePrivateProfileStringW(L"AI", L"Enabled", buf, wszIniFile);
    WritePrivateProfileStringW(L"AI", L"EnginePath", pConfig->wszEnginePath, wszIniFile);
    WritePrivateProfileStringW(L"AI", L"PipeName", pConfig->wszPipeName, wszIniFile);
    swprintf(buf, 32, L"%lu", pConfig->dwTimeoutMs);
    WritePrivateProfileStringW(L"AI", L"TimeoutMs", buf, wszIniFile);
    swprintf(buf, 32, L"%d", pConfig->bShowTokenCost);
    WritePrivateProfileStringW(L"AI", L"ShowTokenCost", buf, wszIniFile);
    swprintf(buf, 32, L"%d", pConfig->iMaxContextFiles);
    WritePrivateProfileStringW(L"AI", L"MaxContextFiles", buf, wszIniFile);
    swprintf(buf, 32, L"%d", pConfig->bAutoStartEngine);
    WritePrivateProfileStringW(L"AI", L"AutoStartEngine", buf, wszIniFile);
    WritePrivateProfileStringW(L"AI", L"ApiEndpoint", pConfig->wszApiEndpoint, wszIniFile);
    WritePrivateProfileStringW(L"AI", L"Model", pConfig->wszModelName, wszIniFile);

    // Provider config
    {
        const AIProviderDef* pDef = AIProvider_Get(pConfig->providerCfg.eProvider);
        if (pDef)
        {
            WCHAR wszSlug[64];
            MultiByteToWideChar(CP_UTF8, 0, pDef->szSlug, -1, wszSlug, 64);
            WritePrivateProfileStringW(L"AI", L"Provider", wszSlug, wszIniFile);
        }
        if (pConfig->providerCfg.szModel[0])
        {
            WCHAR wszModel[128];
            MultiByteToWideChar(CP_UTF8, 0, pConfig->providerCfg.szModel, -1, wszModel, 128);
            WritePrivateProfileStringW(L"AI", L"ProviderModel", wszModel, wszIniFile);
        }
        swprintf(buf, 32, L"%.2f", pConfig->providerCfg.dTemperature);
        WritePrivateProfileStringW(L"AI", L"Temperature", buf, wszIniFile);
        swprintf(buf, 32, L"%d", pConfig->providerCfg.iMaxTokens);
        WritePrivateProfileStringW(L"AI", L"MaxTokens", buf, wszIniFile);

        if (pConfig->providerCfg.szHost[0])
        {
            WCHAR wszHost[512];
            MultiByteToWideChar(CP_UTF8, 0, pConfig->providerCfg.szHost, -1, wszHost, 512);
            WritePrivateProfileStringW(L"AI", L"ProviderHost", wszHost, wszIniFile);
        }
        if (pConfig->providerCfg.iPort > 0)
        {
            swprintf(buf, 32, L"%d", pConfig->providerCfg.iPort);
            WritePrivateProfileStringW(L"AI", L"ProviderPort", buf, wszIniFile);
        }
    }
    // Note: ApiKey is NOT saved to INI for security
}

void AIBridge_ApplyConfig(const AIConfig* pConfig)
{
    BOOL wasEnabled = s_config.bEnabled;
    memcpy(&s_config, pConfig, sizeof(AIConfig));

    if (!pConfig->bEnabled && wasEnabled)
    {
        AIBridge_Disconnect();
    }
    else if (pConfig->bEnabled && !wasEnabled)
    {
        if (pConfig->bAutoStartEngine)
            AIBridge_StartEngine();
        else
            AIBridge_Connect();
    }

    // Push updated provider config to engine
    if (AIBridge_IsConnected())
    {
        AIBridge_SendConfigToEngine(&pConfig->providerCfg);
    }
}

//=============================================================================
// Provider management
//=============================================================================

BOOL AIBridge_SetProvider(EAIProvider eProvider, const char* szModel, const char* szApiKey)
{
    const AIProviderDef* pDef = AIProvider_Get(eProvider);
    if (!pDef) return FALSE;

    // Update local config
    AIProviderConfig_InitDefaults(&s_config.providerCfg, eProvider);
    if (szModel && szModel[0])
        strncpy(s_config.providerCfg.szModel, szModel,
                sizeof(s_config.providerCfg.szModel) - 1);
    if (szApiKey && szApiKey[0])
        strncpy(s_config.providerCfg.szApiKey, szApiKey,
                sizeof(s_config.providerCfg.szApiKey) - 1);

    // Push to engine
    if (AIBridge_IsConnected())
        return AIBridge_SendConfigToEngine(&s_config.providerCfg);

    return TRUE;
}

BOOL AIBridge_SendConfigToEngine(const AIProviderConfig* pCfg)
{
    if (!AIBridge_IsConnected() || !pCfg) return FALSE;

    // Build config update JSON
    char* jsonCfg = AIProviderConfig_ToJSON(pCfg);
    if (!jsonCfg) return FALSE;

    // Wrap in a config message
    JsonWriter w;
    if (!JsonWriter_Init(&w, (int)strlen(jsonCfg) + 256))
    {
        free(jsonCfg);
        return FALSE;
    }

    JsonWriter_BeginObject(&w);
    JsonWriter_String(&w, "type", "config");
    JsonWriter_Int(&w, "version", 1);

    // Inline the provider config fields
    const AIProviderDef* pDef = AIProvider_Get(pCfg->eProvider);
    if (pDef)
        JsonWriter_String(&w, "provider", pDef->szSlug);
    if (pCfg->szModel[0])
        JsonWriter_String(&w, "model", pCfg->szModel);
    if (pCfg->szApiKey[0])
        JsonWriter_String(&w, "apiKey", pCfg->szApiKey);
    if (pCfg->szHost[0])
        JsonWriter_String(&w, "host", pCfg->szHost);
    if (pCfg->iPort > 0)
        JsonWriter_Int(&w, "port", pCfg->iPort);

    char tempBuf[32];
    snprintf(tempBuf, sizeof(tempBuf), "%.2f", pCfg->dTemperature);
    JsonWriter_String(&w, "temperature", tempBuf);
    JsonWriter_Int(&w, "maxTokens", pCfg->iMaxTokens);

    JsonWriter_EndObject(&w);

    BOOL bOk = AIBridge_WriteMessage(JsonWriter_GetBuffer(&w), JsonWriter_GetLength(&w));
    JsonWriter_Free(&w);
    free(jsonCfg);

    return bOk;
}

const AIProviderConfig* AIBridge_GetProviderConfig(void)
{
    return &s_config.providerCfg;
}

//=============================================================================
// Internal: Pipe I/O
//=============================================================================

static BOOL AIBridge_ConnectPipe(void)
{
    // Try to connect to existing pipe
    s_hPipe = CreateFileW(
        s_wszPipeFullName,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);

    if (s_hPipe == INVALID_HANDLE_VALUE)
    {
        // Retry a few times
        for (int i = 0; i < 10; i++)
        {
            Sleep(500);
            if (InterlockedCompareExchange(&s_bShutdown, 0, 0)) return FALSE;

            s_hPipe = CreateFileW(
                s_wszPipeFullName,
                GENERIC_READ | GENERIC_WRITE,
                0,
                NULL,
                OPEN_EXISTING,
                0,
                NULL);

            if (s_hPipe != INVALID_HANDLE_VALUE) break;
        }
    }

    if (s_hPipe == INVALID_HANDLE_VALUE) return FALSE;

    // Set pipe to message mode
    DWORD dwMode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(s_hPipe, &dwMode, NULL, NULL);

    return TRUE;
}

static void AIBridge_DisconnectPipe(void)
{
    if (s_hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(s_hPipe);
        s_hPipe = INVALID_HANDLE_VALUE;
    }
}

static BOOL AIBridge_WriteMessage(const char* pJson, int iJsonLen)
{
    if (s_hPipe == INVALID_HANDLE_VALUE) return FALSE;

    char* pFramed = NULL;
    int iFramedLen = Json_FrameMessage(pJson, iJsonLen, &pFramed);
    if (iFramedLen <= 0) return FALSE;

    DWORD dwWritten = 0;
    BOOL bOk = WriteFile(s_hPipe, pFramed, (DWORD)iFramedLen, &dwWritten, NULL);
    n2e_Free(pFramed);

    return bOk && (int)dwWritten == iFramedLen;
}

static BOOL AIBridge_ReadMessage(char** ppJson, int* piJsonLen)
{
    if (s_hPipe == INVALID_HANDLE_VALUE) return FALSE;

    // Read length prefix (4 bytes)
    char lenBuf[4];
    DWORD dwRead = 0;
    DWORD dwTotal = 0;

    while (dwTotal < 4)
    {
        if (!ReadFile(s_hPipe, lenBuf + dwTotal, 4 - dwTotal, &dwRead, NULL))
            return FALSE;
        if (dwRead == 0) return FALSE;
        dwTotal += dwRead;
    }

    unsigned int msgLen = (unsigned char)lenBuf[0]
                        | ((unsigned char)lenBuf[1] << 8)
                        | ((unsigned char)lenBuf[2] << 16)
                        | ((unsigned char)lenBuf[3] << 24);

    if (msgLen == 0 || msgLen > 10 * 1024 * 1024) return FALSE; // sanity check: 10MB max

    char* pMsg = (char*)n2e_Alloc(msgLen + 1);
    if (!pMsg) return FALSE;

    dwTotal = 0;
    while (dwTotal < msgLen)
    {
        if (!ReadFile(s_hPipe, pMsg + dwTotal, msgLen - dwTotal, &dwRead, NULL))
        {
            n2e_Free(pMsg);
            return FALSE;
        }
        if (dwRead == 0)
        {
            n2e_Free(pMsg);
            return FALSE;
        }
        dwTotal += dwRead;
    }
    pMsg[msgLen] = '\0';

    *ppJson = pMsg;
    *piJsonLen = (int)msgLen;
    return TRUE;
}

//=============================================================================
// Internal: Serialization
//=============================================================================

static const char* ai_reqTypeStr(EAIRequestType t)
{
    switch (t)
    {
    case AI_REQ_PATCH:    return "patch";
    case AI_REQ_HINT:     return "hint";
    case AI_REQ_EXPLAIN:  return "explain";
    case AI_REQ_CHAT:     return "chat";
    case AI_REQ_CONTEXT_SYNC: return "context_sync";
    case AI_REQ_PING:     return "ping";
    case AI_REQ_SHUTDOWN: return "shutdown";
    default:              return "unknown";
    }
}

static const char* ai_actionStr(EAIAction a)
{
    switch (a)
    {
    case AI_ACTION_REFACTOR:  return "refactor";
    case AI_ACTION_FIX:       return "fix";
    case AI_ACTION_EXPLAIN:   return "explain";
    case AI_ACTION_COMPLETE:  return "complete";
    case AI_ACTION_TRANSFORM: return "transform";
    case AI_ACTION_CHAT:      return "chat";
    case AI_ACTION_CUSTOM:    return "custom";
    default:                  return "custom";
    }
}

static const char* ai_scopeStr(EAIScope s)
{
    switch (s)
    {
    case AI_SCOPE_SELECTION: return "selection";
    case AI_SCOPE_FUNCTION:  return "function";
    case AI_SCOPE_FILE:      return "file";
    case AI_SCOPE_PROJECT:   return "project";
    default:                 return "selection";
    }
}

static void JsonWriter_StringArrayFromLines(JsonWriter* pW, const char* pszLines)
{
    JsonWriter_BeginArray(pW);
    if (pszLines && pszLines[0])
    {
        const char* p = pszLines;
        while (*p)
        {
            const char* end = p;
            while (*end && *end != '\r' && *end != '\n')
                end++;
            if (end > p)
            {
                int len = (int)(end - p);
                char line[1024];
                if (len >= (int)sizeof(line))
                    len = (int)sizeof(line) - 1;
                memcpy(line, p, len);
                line[len] = '\0';
                JsonWriter_StringValue(pW, line);
            }
            while (*end == '\r' || *end == '\n')
                end++;
            p = end;
        }
    }
    JsonWriter_EndArray(pW);
}

static char* AIBridge_SerializeRequest(const AIRequest* pReq, int* piLen)
{
    JsonWriter w;
    if (!JsonWriter_Init(&w, 8192)) return NULL;

    JsonWriter_BeginObject(&w);

    // Header
    char reqId[32];
    sprintf(reqId, "req_%u", pReq->uRequestId);
    JsonWriter_String(&w, "id", reqId);
    JsonWriter_String(&w, "type", ai_reqTypeStr(pReq->eType));
    JsonWriter_Int(&w, "version", 2);

    // Context
    if (pReq->eType == AI_REQ_PATCH || pReq->eType == AI_REQ_HINT ||
        pReq->eType == AI_REQ_EXPLAIN || pReq->eType == AI_REQ_CHAT)
    {
        JsonWriter_Key(&w, "context");
        JsonWriter_BeginObject(&w);
        {
            // File
            JsonWriter_Key(&w, "file");
            JsonWriter_BeginObject(&w);
            if (pReq->pszFilePath)
                JsonWriter_String(&w, "path", pReq->pszFilePath);
            if (pReq->pszLanguage)
                JsonWriter_String(&w, "language", pReq->pszLanguage);
            if (pReq->pszFileContent)
                JsonWriter_String(&w, "content", pReq->pszFileContent);
            JsonWriter_String(&w, "encoding", "utf-8");
            JsonWriter_EndObject(&w);

            // Cursor
            JsonWriter_Key(&w, "cursor");
            JsonWriter_BeginObject(&w);
            JsonWriter_Int(&w, "line", pReq->iCursorLine);
            JsonWriter_Int(&w, "column", pReq->iCursorCol);
            if (pReq->pszSelectedText)
            {
                JsonWriter_Key(&w, "selection");
                JsonWriter_BeginObject(&w);
                JsonWriter_Int(&w, "startLine", pReq->iSelStartLine);
                JsonWriter_Int(&w, "startCol", pReq->iSelStartCol);
                JsonWriter_Int(&w, "endLine", pReq->iSelEndLine);
                JsonWriter_Int(&w, "endCol", pReq->iSelEndCol);
                JsonWriter_String(&w, "text", pReq->pszSelectedText);
                JsonWriter_EndObject(&w);
            }
            JsonWriter_EndObject(&w);

            // Viewport
            JsonWriter_Key(&w, "viewport");
            JsonWriter_BeginObject(&w);
            JsonWriter_Int(&w, "firstVisibleLine", pReq->iFirstVisibleLine);
            JsonWriter_Int(&w, "lastVisibleLine", pReq->iLastVisibleLine);
            JsonWriter_EndObject(&w);

            // Project / repo atlas-lite
            JsonWriter_Key(&w, "project");
            JsonWriter_BeginObject(&w);
            if (pReq->pszProjectRoot)
                JsonWriter_String(&w, "root", pReq->pszProjectRoot);
            JsonWriter_Key(&w, "activeFiles");
            JsonWriter_StringArrayFromLines(&w, pReq->pszActiveFiles);
            JsonWriter_EndObject(&w);

            JsonWriter_Key(&w, "repo");
            JsonWriter_BeginObject(&w);
            if (pReq->pszGitSummary)
                JsonWriter_String(&w, "gitSummary", pReq->pszGitSummary);
            if (pReq->pszDiagnostics)
                JsonWriter_String(&w, "diagnostics", pReq->pszDiagnostics);
            if (pReq->pszBuildCommand)
                JsonWriter_String(&w, "buildCommand", pReq->pszBuildCommand);
            if (pReq->pszTestCommand)
                JsonWriter_String(&w, "testCommand", pReq->pszTestCommand);
            JsonWriter_Key(&w, "hotZones");
            JsonWriter_StringArrayFromLines(&w, pReq->pszHotZones);
            JsonWriter_EndObject(&w);

            JsonWriter_Key(&w, "atlas");
            JsonWriter_BeginObject(&w);
            if (pReq->pszAtlasSummary)
                JsonWriter_String(&w, "summary", pReq->pszAtlasSummary);
            if (pReq->pszBufferHash)
                JsonWriter_String(&w, "bufferHash", pReq->pszBufferHash);
            JsonWriter_Int(&w, "bufferVersion", (int)pReq->uBufferVersion);
            JsonWriter_EndObject(&w);
        }
        JsonWriter_EndObject(&w);

        // Flat compatibility fields for the current engine parser.
        if (pReq->pszFilePath)
            JsonWriter_String(&w, "filePath", pReq->pszFilePath);
        if (pReq->pszLanguage)
            JsonWriter_String(&w, "language", pReq->pszLanguage);
        if (pReq->pszFileContent)
            JsonWriter_String(&w, "fileContent", pReq->pszFileContent);
        if (pReq->pszSelectedText)
            JsonWriter_String(&w, "selection", pReq->pszSelectedText);
        if (pReq->pszProjectRoot)
            JsonWriter_String(&w, "projectRoot", pReq->pszProjectRoot);
        if (pReq->pszActiveFiles)
            JsonWriter_String(&w, "activeFilesText", pReq->pszActiveFiles);
        if (pReq->pszGitSummary)
            JsonWriter_String(&w, "gitSummary", pReq->pszGitSummary);
        if (pReq->pszDiagnostics)
            JsonWriter_String(&w, "diagnostics", pReq->pszDiagnostics);
        if (pReq->pszAtlasSummary)
            JsonWriter_String(&w, "atlasSummary", pReq->pszAtlasSummary);
        if (pReq->pszBuildCommand)
            JsonWriter_String(&w, "buildCommand", pReq->pszBuildCommand);
        if (pReq->pszTestCommand)
            JsonWriter_String(&w, "testCommand", pReq->pszTestCommand);
        if (pReq->pszHotZones)
            JsonWriter_String(&w, "hotZonesText", pReq->pszHotZones);
        if (pReq->pszBufferHash)
            JsonWriter_String(&w, "bufferHash", pReq->pszBufferHash);
        JsonWriter_Int(&w, "bufferVersion", (int)pReq->uBufferVersion);
    }

    // Intent
    if (pReq->eType == AI_REQ_PATCH || pReq->eType == AI_REQ_HINT || pReq->eType == AI_REQ_EXPLAIN)
    {
        JsonWriter_Key(&w, "intent");
        JsonWriter_BeginObject(&w);
        JsonWriter_String(&w, "action", ai_actionStr(pReq->eAction));
        if (pReq->pszInstruction)
            JsonWriter_String(&w, "instruction", pReq->pszInstruction);
        JsonWriter_String(&w, "scope", ai_scopeStr(pReq->eScope));
        JsonWriter_EndObject(&w);

        JsonWriter_String(&w, "action", ai_actionStr(pReq->eAction));
        if (pReq->pszInstruction)
            JsonWriter_String(&w, "instruction", pReq->pszInstruction);
        JsonWriter_String(&w, "scope", ai_scopeStr(pReq->eScope));
    }

    // Chat message
    if (pReq->eType == AI_REQ_CHAT && pReq->pszChatMessage)
    {
        JsonWriter_String(&w, "message", pReq->pszChatMessage);
        JsonWriter_String(&w, "chatMessage", pReq->pszChatMessage);
    }

    // Provider override (include active provider config for per-request routing)
    {
        const AIProviderConfig* pCfg = &s_config.providerCfg;
        const AIProviderDef* pDef = AIProvider_Get(pCfg->eProvider);
        if (pDef)
        {
            JsonWriter_String(&w, "provider", pDef->szSlug);
            if (pCfg->szModel[0])
                JsonWriter_String(&w, "providerModel", pCfg->szModel);
            if (pCfg->szApiKey[0])
                JsonWriter_String(&w, "providerKey", pCfg->szApiKey);
        }
    }

    JsonWriter_EndObject(&w);

    *piLen = JsonWriter_GetLength(&w);
    char* pResult = (char*)n2e_Alloc(*piLen + 1);
    if (pResult)
    {
        memcpy(pResult, JsonWriter_GetBuffer(&w), *piLen + 1);
    }
    JsonWriter_Free(&w);
    return pResult;
}

static char* ai_strdup(const char* s)
{
    if (!s) return NULL;
    int len = (int)strlen(s);
    char* p = (char*)n2e_Alloc(len + 1);
    if (p) memcpy(p, s, len + 1);
    return p;
}

static AIResponse* AIBridge_DeserializeResponse(const char* pJson, int iJsonLen)
{
    AIResponse* pResp = (AIResponse*)n2e_Alloc(sizeof(AIResponse));
    if (!pResp) return NULL;
    ZeroMemory(pResp, sizeof(AIResponse));

    JsonReader r;
    if (!JsonReader_Init(&r, pJson, iJsonLen))
    {
        n2e_Free(pResp);
        return NULL;
    }

    EJsonToken tok = JsonReader_Next(&r);
    if (tok != JSON_OBJECT_START)
    {
        n2e_Free(pResp);
        return NULL;
    }

    while ((tok = JsonReader_Next(&r)) == JSON_KEY)
    {
        const char* key = JsonReader_GetString(&r);

        if (strcmp(key, "id") == 0)
        {
            JsonReader_Next(&r);
            // Parse "req_NNN"
            const char* s = JsonReader_GetString(&r);
            if (s && strncmp(s, "req_", 4) == 0)
                pResp->uRequestId = (UINT)atoi(s + 4);
        }
        else if (strcmp(key, "status") == 0)
        {
            JsonReader_Next(&r);
            const char* s = JsonReader_GetString(&r);
            if (s)
            {
                pResp->bSuccess = (strcmp(s, "ok") == 0);
                pResp->bPartial = (strcmp(s, "partial") == 0);
            }
        }
        else if (strcmp(key, "patches") == 0)
        {
            tok = JsonReader_Next(&r);
            if (tok == JSON_ARRAY_START)
            {
                // Count patches (simplified: max 32)
                // For now, parse one level
                int count = 0;
                AIPatch patches[32];
                ZeroMemory(patches, sizeof(patches));

                while ((tok = JsonReader_Next(&r)) != JSON_ARRAY_END && tok != JSON_ERROR && tok != JSON_NONE)
                {
                    if (tok == JSON_OBJECT_START && count < 32)
                    {
                        while ((tok = JsonReader_Next(&r)) == JSON_KEY)
                        {
                            const char* pkey = JsonReader_GetString(&r);
                            JsonReader_Next(&r);
                            if (strcmp(pkey, "file") == 0)
                                patches[count].pszFilePath = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "diff") == 0)
                                patches[count].pszRawDiff = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "description") == 0)
                                patches[count].pszDescription = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "proofSummary") == 0)
                                patches[count].pszProofSummary = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "touchedSymbols") == 0)
                                patches[count].pszTouchedSymbols = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "assumptions") == 0)
                                patches[count].pszAssumptions = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "validations") == 0)
                                patches[count].pszValidations = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "reviewerVotes") == 0)
                                patches[count].pszReviewerVotes = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "residualRisk") == 0)
                                patches[count].pszResidualRisk = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "rollbackFingerprint") == 0)
                                patches[count].pszRollbackFingerprint = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "baseBufferHash") == 0)
                                patches[count].pszBaseBufferHash = ai_strdup(JsonReader_GetString(&r));
                            else if (strcmp(pkey, "confidence") == 0)
                                patches[count].dConfidence = JsonReader_GetDouble(&r);
                            else if (strcmp(pkey, "baseBufferVersion") == 0)
                                patches[count].uBaseBufferVersion = (UINT)JsonReader_GetInt(&r);
                            else if (strcmp(pkey, "candidateRank") == 0)
                                patches[count].iCandidateRank = JsonReader_GetInt(&r);
                            else if (strcmp(pkey, "ghostLayer") == 0)
                                patches[count].bGhostLayer = JsonReader_GetBool(&r);
                            else if (strcmp(pkey, "stale") == 0)
                                patches[count].bStale = JsonReader_GetBool(&r);
                            else
                                JsonReader_SkipValue(&r);
                        }
                        patches[count].bSelected = TRUE;
                        count++;
                    }
                }

                if (count > 0)
                {
                    pResp->iPatchCount = count;
                    pResp->pPatches = (AIPatch*)n2e_Alloc(sizeof(AIPatch) * count);
                    if (pResp->pPatches)
                        memcpy(pResp->pPatches, patches, sizeof(AIPatch) * count);
                }
            }
        }
        else if (strcmp(key, "hint") == 0)
        {
            tok = JsonReader_Next(&r);
            if (tok == JSON_OBJECT_START)
            {
                while ((tok = JsonReader_Next(&r)) == JSON_KEY)
                {
                    const char* hkey = JsonReader_GetString(&r);
                    JsonReader_Next(&r);
                    if (strcmp(hkey, "text") == 0)
                        pResp->pszHintText = ai_strdup(JsonReader_GetString(&r));
                    else if (strcmp(hkey, "line") == 0)
                        pResp->iHintLine = JsonReader_GetInt(&r);
                    else if (strcmp(hkey, "column") == 0)
                        pResp->iHintCol = JsonReader_GetInt(&r);
                    else if (strcmp(hkey, "confidence") == 0)
                        pResp->dHintConfidence = JsonReader_GetDouble(&r);
                    else
                        JsonReader_SkipValue(&r);
                }
            }
        }
        else if (strcmp(key, "explanation") == 0)
        {
            tok = JsonReader_Next(&r);
            if (tok == JSON_OBJECT_START)
            {
                while ((tok = JsonReader_Next(&r)) == JSON_KEY)
                {
                    const char* ekey = JsonReader_GetString(&r);
                    JsonReader_Next(&r);
                    if (strcmp(ekey, "summary") == 0)
                        pResp->pszExplanation = ai_strdup(JsonReader_GetString(&r));
                    else if (strcmp(ekey, "details") == 0)
                        pResp->pszExplanationDetails = ai_strdup(JsonReader_GetString(&r));
                    else
                        JsonReader_SkipValue(&r);
                }
            }
            else if (tok == JSON_STRING)
            {
                pResp->pszExplanation = ai_strdup(JsonReader_GetString(&r));
            }
        }
        else if (strcmp(key, "chat_response") == 0 || strcmp(key, "chatResponse") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszChatResponse = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "missionId") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszMissionId = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "missionPhase") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszMissionPhase = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "missionSummary") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszMissionSummary = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "missionQueue") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszMissionQueue = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "atlasSummary") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszAtlasSummary = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "proofSummary") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszProofSummary = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "buildCommand") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszBuildCommand = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "testCommand") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszTestCommand = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "scratchpadSummary") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszScratchpadSummary = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "chunk") == 0)
        {
            JsonReader_Next(&r);
            pResp->pszChunk = ai_strdup(JsonReader_GetString(&r));
        }
        else if (strcmp(key, "error") == 0)
        {
            tok = JsonReader_Next(&r);
            if (tok == JSON_OBJECT_START)
            {
                while ((tok = JsonReader_Next(&r)) == JSON_KEY)
                {
                    const char* ekey = JsonReader_GetString(&r);
                    JsonReader_Next(&r);
                    if (strcmp(ekey, "code") == 0)
                        pResp->pszErrorCode = ai_strdup(JsonReader_GetString(&r));
                    else if (strcmp(ekey, "message") == 0)
                        pResp->pszErrorMessage = ai_strdup(JsonReader_GetString(&r));
                    else
                        JsonReader_SkipValue(&r);
                }
            }
        }
        else if (strcmp(key, "meta") == 0)
        {
            tok = JsonReader_Next(&r);
            if (tok == JSON_OBJECT_START)
            {
                while ((tok = JsonReader_Next(&r)) == JSON_KEY)
                {
                    const char* mkey = JsonReader_GetString(&r);
                    JsonReader_Next(&r);
                    if (strcmp(mkey, "model") == 0)
                        pResp->pszModel = ai_strdup(JsonReader_GetString(&r));
                    else if (strcmp(mkey, "inputTokens") == 0)
                        pResp->iInputTokens = JsonReader_GetInt(&r);
                    else if (strcmp(mkey, "outputTokens") == 0)
                        pResp->iOutputTokens = JsonReader_GetInt(&r);
                    else if (strcmp(mkey, "latencyMs") == 0)
                        pResp->iLatencyMs = JsonReader_GetInt(&r);
                    else
                        JsonReader_SkipValue(&r);
                }
            }
        }
        else
        {
            JsonReader_Next(&r);
            JsonReader_SkipValue(&r);
        }
    }

    return pResp;
}

//=============================================================================
// Internal: Response queue
//=============================================================================

static void AIBridge_QueueResponse(AIResponse* pResp)
{
    EnterCriticalSection(&s_csResponses);

    // Find empty slot
    for (int i = 0; i < MAX_PENDING_RESPONSES; i++)
    {
        if (!s_pResponses[i])
        {
            s_pResponses[i] = pResp;
            LeaveCriticalSection(&s_csResponses);
            return;
        }
    }

    // Queue full â€” overwrite oldest
    AIBridge_FreeResponse(s_pResponses[0]);
    s_pResponses[0] = pResp;
    LeaveCriticalSection(&s_csResponses);
}

static void AIBridge_SetStatus(EAIStatus eStatus)
{
    InterlockedExchange(&s_eStatus, (LONG)eStatus);
    if (s_hwndMain)
    {
        PostMessage(s_hwndMain, WM_AI_STATUS, (WPARAM)eStatus, 0);
    }
}

//=============================================================================
// Background thread
//=============================================================================

static unsigned __stdcall AIBridge_ThreadProc(void* pArg)
{
    UNREFERENCED_PARAMETER(pArg);

    if (!AIBridge_ConnectPipe())
    {
        AIBridge_SetStatus(AI_STATUS_OFFLINE);
        if (s_hwndMain)
            PostMessage(s_hwndMain, WM_AI_DISCONNECTED, 0, 0);
        return 1;
    }

    AIBridge_SetStatus(AI_STATUS_READY);
    if (s_hwndMain)
        PostMessage(s_hwndMain, WM_AI_CONNECTED, 0, 0);

    // Read loop
    while (!InterlockedCompareExchange(&s_bShutdown, 0, 0))
    {
        char* pJson = NULL;
        int iJsonLen = 0;

        if (!AIBridge_ReadMessage(&pJson, &iJsonLen))
        {
            // Pipe broken
            AIBridge_SetStatus(AI_STATUS_OFFLINE);
            if (s_hwndMain)
                PostMessage(s_hwndMain, WM_AI_DISCONNECTED, 0, 0);
            break;
        }

        AIResponse* pResp = AIBridge_DeserializeResponse(pJson, iJsonLen);
        n2e_Free(pJson);

        if (pResp)
        {
            AIBridge_QueueResponse(pResp);

            if (pResp->bPartial)
            {
                if (s_hwndMain)
                    PostMessage(s_hwndMain, WM_AI_CHUNK, (WPARAM)pResp->uRequestId, 0);
            }
            else
            {
                AIBridge_SetStatus(
                    (pResp->pPatches && pResp->iPatchCount > 0)
                    ? AI_STATUS_PATCH_READY
                    : AI_STATUS_READY);
                if (s_hwndMain)
                    PostMessage(s_hwndMain, WM_AI_RESPONSE, (WPARAM)pResp->uRequestId, 0);
            }
        }
    }

    AIBridge_DisconnectPipe();
    return 0;
}
