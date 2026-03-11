/******************************************************************************
*
* Biko â€” AI Engine (Multi-Provider)
*
* biko-engine.c
*   Companion process that communicates with the editor via named pipe
*   and with AI providers (OpenAI, Anthropic, Google, local, etc.) via HTTP/REST.
*
*   Architecture:
*     biko.exe â†â†’ named pipe â†â†’ biko-engine.exe â†â†’ HTTP(S) â†â†’ LLM API
*
*   Protocol: length-prefixed JSON over \\.\pipe\biko_ai_{nonce}
*
*   Supported providers (via AIProvider.h registry):
*     Cloud:  OpenAI, Anthropic, Google Gemini, Mistral, Cohere, DeepSeek,
*             xAI (Grok), Groq, OpenRouter, Together, Fireworks, Perplexity
*     Local:  Ollama, LM Studio, llama.cpp, vLLM, LocalAI
*     Custom: Any OpenAI-compatible endpoint (BYOK)
*
*   This is a standalone C program built as a separate .exe.
*   Uses WinHTTP for HTTP/HTTPS requests.
*
******************************************************************************/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

#include "AIProvider.h"

//=============================================================================
// Configuration
//=============================================================================

#define ENGINE_VERSION          "0.2.0"
#define MAX_REQUEST_SIZE        (4 * 1024 * 1024)   // 4 MB
#define MAX_RESPONSE_SIZE       (8 * 1024 * 1024)
#define PIPE_BUFFER_SIZE        (64 * 1024)

typedef struct EngineState {
    AIProviderConfig providerCfg;       // Active provider configuration
    BOOL             bVerbose;
} EngineState;

static EngineState s_state;

//=============================================================================
// Growable string buffer
//=============================================================================

typedef struct StrBuf {
    char*   data;
    int     len;
    int     cap;
} StrBuf;

static void sb_init(StrBuf* sb, int initialCap)
{
    sb->cap = initialCap > 64 ? initialCap : 256;
    sb->data = (char*)malloc(sb->cap);
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void sb_append(StrBuf* sb, const char* s, int slen)
{
    if (slen < 0) slen = (int)strlen(s);
    if (sb->len + slen + 1 > sb->cap)
    {
        sb->cap = (sb->len + slen + 1) * 2;
        sb->data = (char*)realloc(sb->data, sb->cap);
    }
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(StrBuf* sb, const char* fmt, ...)
{
    char tmp[4096];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n > 0) sb_append(sb, tmp, n);
}

static void sb_free(StrBuf* sb)
{
    if (sb->data) free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

//=============================================================================
// JSON escape helper
//=============================================================================

static void json_escape_append(StrBuf* sb, const char* s)
{
    sb_append(sb, "\"", 1);
    while (*s)
    {
        switch (*s)
        {
        case '"':  sb_append(sb, "\\\"", 2); break;
        case '\\': sb_append(sb, "\\\\", 2); break;
        case '\b': sb_append(sb, "\\b", 2); break;
        case '\f': sb_append(sb, "\\f", 2); break;
        case '\n': sb_append(sb, "\\n", 2); break;
        case '\r': sb_append(sb, "\\r", 2); break;
        case '\t': sb_append(sb, "\\t", 2); break;
        default:
            if ((unsigned char)*s < 0x20)
            {
                char esc[8];
                sprintf(esc, "\\u%04x", (unsigned char)*s);
                sb_append(sb, esc, 6);
            }
            else
            {
                sb_append(sb, s, 1);
            }
        }
        s++;
    }
    sb_append(sb, "\"", 1);
}

//=============================================================================
// Minimal JSON value extraction (for parsing API responses)
//=============================================================================

static const char* json_find_value(const char* json, const char* key)
{
    char needle[256];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char* p = strstr(json, needle);
    if (!p) return NULL;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (*p == ':') p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static char* json_extract_string(const char* json, const char* key)
{
    const char* val = json_find_value(json, key);
    if (!val || *val != '"') return NULL;
    val++;

    StrBuf sb;
    sb_init(&sb, 256);
    while (*val && *val != '"')
    {
        if (*val == '\\' && *(val + 1))
        {
            val++;
            switch (*val)
            {
            case '"':  sb_append(&sb, "\"", 1); break;
            case '\\': sb_append(&sb, "\\", 1); break;
            case 'n':  sb_append(&sb, "\n", 1); break;
            case 'r':  sb_append(&sb, "\r", 1); break;
            case 't':  sb_append(&sb, "\t", 1); break;
            default:   sb_append(&sb, val, 1); break;
            }
        }
        else
        {
            sb_append(&sb, val, 1);
        }
        val++;
    }
    return sb.data;
}

// Find a nested value: search for key2 within the value of key1
static char* json_extract_nested_content(const char* json, const char* key1,
                                          const char* key2)
{
    const char* p = json_find_value(json, key1);
    if (!p) return NULL;
    return json_extract_string(p, key2);
}

//=============================================================================
// Pipe request parsing
//=============================================================================

typedef struct PipeRequest {
    char*   pszId;
    char*   pszType;          // "patch", "hint", "explain", "chat"
    char*   pszAction;        // "refactor", "fix", "explain", "complete", "transform", "chat"
    char*   pszScope;
    char*   pszFilePath;
    char*   pszFileContent;
    char*   pszSelection;
    char*   pszInstruction;
    char*   pszChatMessage;
    char*   pszLanguage;
    char*   pszProjectRoot;
    char*   pszActiveFiles;
    char*   pszGitSummary;
    char*   pszDiagnostics;
    char*   pszAtlasSummary;
    char*   pszBuildCommand;
    char*   pszTestCommand;
    char*   pszHotZones;
    char*   pszBufferHash;
    int     iBufferVersion;
    int     iCursorLine;
    int     iCursorCol;
    int     iSelStartLine;
    int     iSelEndLine;

    // Per-request provider override (optional, from editor)
    char*   pszProvider;      // slug like "anthropic", "ollama"
    char*   pszModel;         // model override
    char*   pszApiKey;        // key override
} PipeRequest;

static void FreeRequest(PipeRequest* req)
{
    if (req->pszId) free(req->pszId);
    if (req->pszType) free(req->pszType);
    if (req->pszAction) free(req->pszAction);
    if (req->pszScope) free(req->pszScope);
    if (req->pszFilePath) free(req->pszFilePath);
    if (req->pszFileContent) free(req->pszFileContent);
    if (req->pszSelection) free(req->pszSelection);
    if (req->pszInstruction) free(req->pszInstruction);
    if (req->pszChatMessage) free(req->pszChatMessage);
    if (req->pszLanguage) free(req->pszLanguage);
    if (req->pszProjectRoot) free(req->pszProjectRoot);
    if (req->pszActiveFiles) free(req->pszActiveFiles);
    if (req->pszGitSummary) free(req->pszGitSummary);
    if (req->pszDiagnostics) free(req->pszDiagnostics);
    if (req->pszAtlasSummary) free(req->pszAtlasSummary);
    if (req->pszBuildCommand) free(req->pszBuildCommand);
    if (req->pszTestCommand) free(req->pszTestCommand);
    if (req->pszHotZones) free(req->pszHotZones);
    if (req->pszBufferHash) free(req->pszBufferHash);
    if (req->pszProvider) free(req->pszProvider);
    if (req->pszModel) free(req->pszModel);
    if (req->pszApiKey) free(req->pszApiKey);
    memset(req, 0, sizeof(*req));
}

static void ParseRequest(const char* json, PipeRequest* req)
{
    memset(req, 0, sizeof(*req));
    req->pszId = json_extract_string(json, "id");
    req->pszType = json_extract_string(json, "type");
    req->pszAction = json_extract_string(json, "action");
    req->pszScope = json_extract_string(json, "scope");
    req->pszFilePath = json_extract_string(json, "filePath");
    req->pszFileContent = json_extract_string(json, "fileContent");
    req->pszSelection = json_extract_string(json, "selection");
    req->pszInstruction = json_extract_string(json, "instruction");
    req->pszChatMessage = json_extract_string(json, "chatMessage");
    if (!req->pszChatMessage)
        req->pszChatMessage = json_extract_string(json, "message");
    req->pszLanguage = json_extract_string(json, "language");
    req->pszProjectRoot = json_extract_string(json, "projectRoot");
    req->pszActiveFiles = json_extract_string(json, "activeFilesText");
    req->pszGitSummary = json_extract_string(json, "gitSummary");
    req->pszDiagnostics = json_extract_string(json, "diagnostics");
    req->pszAtlasSummary = json_extract_string(json, "atlasSummary");
    req->pszBuildCommand = json_extract_string(json, "buildCommand");
    req->pszTestCommand = json_extract_string(json, "testCommand");
    req->pszHotZones = json_extract_string(json, "hotZonesText");
    req->pszBufferHash = json_extract_string(json, "bufferHash");

    // Provider overrides (embedded in pipe request by editor)
    req->pszProvider = json_extract_string(json, "provider");
    req->pszModel = json_extract_string(json, "providerModel");
    req->pszApiKey = json_extract_string(json, "providerKey");

    {
        const char* p = json_find_value(json, "bufferVersion");
        if (p)
            req->iBufferVersion = atoi(p);
        p = json_find_value(json, "line");
        if (p)
            req->iCursorLine = atoi(p);
        p = json_find_value(json, "column");
        if (p)
            req->iCursorCol = atoi(p);
        p = json_find_value(json, "startLine");
        if (p)
            req->iSelStartLine = atoi(p);
        p = json_find_value(json, "endLine");
        if (p)
            req->iSelEndLine = atoi(p);
    }
}

//=============================================================================
// Delta Mesh helpers
//=============================================================================

#define MAX_ATLAS_CACHE         16
#define MAX_MISSION_HISTORY     12
#define MAX_ACTIVE_LEASES       12

typedef struct IntentSpec {
    char  szGoal[256];
    char  szRisk[16];
    char  szAcceptance[512];
    int   maxWorkers;
    int   maxSeconds;
    int   maxTokens;
} IntentSpec;

typedef struct AtlasCard {
    char szKind[32];
    char szFile[260];
    char szSummary[512];
    int  score;
} AtlasCard;

typedef struct AtlasLite {
    AtlasCard cards[8];
    int   count;
    char* pszSummary;
    char  szBuildCommand[256];
    char  szTestCommand[256];
} AtlasLite;

typedef struct PatchProof {
    char  szSummary[512];
    char  szTouchedSymbols[512];
    char  szAssumptions[1024];
    char  szValidations[1024];
    char  szReviewerVotes[1024];
    char  szResidualRisk[256];
    char  szRollbackFingerprint[96];
    char  szBuildCommand[256];
    char  szTestCommand[256];
    double dConfidence;
    int    iCandidateRank;
    BOOL   bGhostLayer;
    BOOL   bStale;
} PatchProof;

typedef struct MissionRecord {
    char  szId[64];
    char  szPhase[32];
    char  szSummary[256];
    char  szRisk[16];
    DWORD dwTick;
} MissionRecord;

typedef struct LeaseEntry {
    char  szOwner[64];
    char  szScope[260];
    DWORD dwTick;
    BOOL  bActive;
} LeaseEntry;

typedef struct AtlasCacheEntry {
    char  szKey[512];
    char* pszSummary;
    DWORD dwTick;
} AtlasCacheEntry;

static MissionRecord    s_missionHistory[MAX_MISSION_HISTORY];
static int              s_missionHistoryCount = 0;
static LeaseEntry       s_leases[MAX_ACTIVE_LEASES];
static AtlasCacheEntry  s_atlasCache[MAX_ATLAS_CACHE];
static LONG             s_nextMissionSeq = 1;

static const char* safe_cstr(const char* s)
{
    return s ? s : "";
}

static unsigned __int64 fnv1a64(const char* s)
{
    const unsigned char* p = (const unsigned char*)safe_cstr(s);
    unsigned __int64 h = 1469598103934665603ULL;
    while (*p)
    {
        h ^= (unsigned __int64)(*p++);
        h *= 1099511628211ULL;
    }
    return h;
}

static void hash_to_hex(unsigned __int64 h, char* out, int cchOut)
{
    snprintf(out, cchOut, "%016llx", h);
}

static BOOL PathExistsUtf8(const char* utf8Dir, const char* leaf)
{
    WCHAR wszDir[MAX_PATH];
    WCHAR wszLeaf[MAX_PATH];
    WCHAR wszPath[MAX_PATH];
    if (!utf8Dir || !utf8Dir[0] || !leaf || !leaf[0]) return FALSE;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8Dir, -1, wszDir, MAX_PATH))
        return FALSE;
    if (!MultiByteToWideChar(CP_UTF8, 0, leaf, -1, wszLeaf, MAX_PATH))
        return FALSE;
    _snwprintf_s(wszPath, _countof(wszPath), _TRUNCATE, L"%s\\%s", wszDir, wszLeaf);
    return GetFileAttributesW(wszPath) != INVALID_FILE_ATTRIBUTES;
}

static void CompileIntentSpec(const PipeRequest* req, IntentSpec* spec)
{
    ZeroMemory(spec, sizeof(*spec));
    _snprintf_s(spec->szGoal, sizeof(spec->szGoal), _TRUNCATE, "%s",
                req->pszInstruction && req->pszInstruction[0]
                    ? req->pszInstruction
                    : (req->pszAction && req->pszAction[0] ? req->pszAction : "help with current code"));
    _snprintf_s(spec->szRisk, sizeof(spec->szRisk), _TRUNCATE, "%s",
                (req->pszDiagnostics && req->pszDiagnostics[0]) ? "medium" :
                (req->pszScope && strcmp(req->pszScope, "project") == 0) ? "high" : "low");
    _snprintf_s(spec->szAcceptance, sizeof(spec->szAcceptance), _TRUNCATE,
                "Return a minimal valid result, preserve existing intent, include proof metadata, and stay anchored to buffer hash %s.",
                req->pszBufferHash ? req->pszBufferHash : "unknown");
    spec->maxWorkers = 3;
    spec->maxSeconds = 45;
    spec->maxTokens = 4096;
}

static void GuessTouchedSymbols(const PipeRequest* req, char* out, int cchOut)
{
    out[0] = '\0';
    if (req->pszSelection && req->pszSelection[0])
    {
        const char* p = req->pszSelection;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        const char* end = p;
        while (*end && *end != '\r' && *end != '\n')
            end++;
        int len = (int)(end - p);
        if (len > 120) len = 120;
        if (len > 0)
        {
            memcpy(out, p, len);
            out[len] = '\0';
            return;
        }
    }
    if (req->pszFilePath && req->pszFilePath[0])
    {
        _snprintf_s(out, cchOut, _TRUNCATE, "%s (%s scope)",
                    req->pszFilePath,
                    req->pszScope && req->pszScope[0] ? req->pszScope : "file");
        return;
    }
    _snprintf_s(out, cchOut, _TRUNCATE, "current workspace scope");
}

static char* AtlasCache_Get(const char* key)
{
    for (int i = 0; i < MAX_ATLAS_CACHE; i++)
    {
        if (s_atlasCache[i].pszSummary && strcmp(s_atlasCache[i].szKey, key) == 0)
        {
            s_atlasCache[i].dwTick = GetTickCount();
            return _strdup(s_atlasCache[i].pszSummary);
        }
    }
    return NULL;
}

static void AtlasCache_Put(const char* key, const char* summary)
{
    int slot = -1;
    DWORD oldest = MAXDWORD;
    for (int i = 0; i < MAX_ATLAS_CACHE; i++)
    {
        if (!s_atlasCache[i].pszSummary) { slot = i; break; }
        if (s_atlasCache[i].dwTick < oldest) {
            oldest = s_atlasCache[i].dwTick;
            slot = i;
        }
    }
    if (slot < 0) slot = 0;
    if (s_atlasCache[slot].pszSummary) free(s_atlasCache[slot].pszSummary);
    _snprintf_s(s_atlasCache[slot].szKey, sizeof(s_atlasCache[slot].szKey), _TRUNCATE, "%s", safe_cstr(key));
    s_atlasCache[slot].pszSummary = _strdup(safe_cstr(summary));
    s_atlasCache[slot].dwTick = GetTickCount();
}

static void BuildAtlasLite(const PipeRequest* req, AtlasLite* atlas)
{
    ZeroMemory(atlas, sizeof(*atlas));

    if (req->pszBuildCommand && req->pszBuildCommand[0])
        _snprintf_s(atlas->szBuildCommand, sizeof(atlas->szBuildCommand), _TRUNCATE, "%s", req->pszBuildCommand);
    else if (req->pszProjectRoot && PathExistsUtf8(req->pszProjectRoot, "do_build.cmd"))
        _snprintf_s(atlas->szBuildCommand, sizeof(atlas->szBuildCommand), _TRUNCATE, "cmd /c do_build.cmd");
    else if (req->pszProjectRoot && PathExistsUtf8(req->pszProjectRoot, "Notepad2e.vcxproj"))
        _snprintf_s(atlas->szBuildCommand, sizeof(atlas->szBuildCommand), _TRUNCATE, "msbuild Notepad2e.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64");

    if (req->pszTestCommand && req->pszTestCommand[0])
        _snprintf_s(atlas->szTestCommand, sizeof(atlas->szTestCommand), _TRUNCATE, "%s", req->pszTestCommand);
    else if (req->pszProjectRoot && PathExistsUtf8(req->pszProjectRoot, "test\\Extension\\Notepad2eTests.vcxproj"))
        _snprintf_s(atlas->szTestCommand, sizeof(atlas->szTestCommand), _TRUNCATE, "msbuild test\\Extension\\Notepad2eTests.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64");

    char key[512];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "%s|%s|%s",
                safe_cstr(req->pszProjectRoot), safe_cstr(req->pszFilePath), safe_cstr(req->pszLanguage));
    atlas->pszSummary = AtlasCache_Get(key);
    if (atlas->pszSummary)
        return;

    StrBuf sb;
    sb_init(&sb, 1024);
    if (req->pszProjectRoot && req->pszProjectRoot[0])
        sb_appendf(&sb, "Project root: %s\n", req->pszProjectRoot);
    if (req->pszFilePath && req->pszFilePath[0])
        sb_appendf(&sb, "Current file: %s\n", req->pszFilePath);
    if (req->pszLanguage && req->pszLanguage[0])
        sb_appendf(&sb, "Language: %s\n", req->pszLanguage);
    if (req->pszGitSummary && req->pszGitSummary[0])
        sb_appendf(&sb, "Git: %s\n", req->pszGitSummary);
    if (req->pszDiagnostics && req->pszDiagnostics[0])
        sb_appendf(&sb, "Diagnostics: %s\n", req->pszDiagnostics);
    if (req->pszActiveFiles && req->pszActiveFiles[0])
        sb_appendf(&sb, "Active files:\n%s\n", req->pszActiveFiles);
    if (req->pszHotZones && req->pszHotZones[0])
        sb_appendf(&sb, "Hot zones:\n%s\n", req->pszHotZones);
    if (atlas->szBuildCommand[0])
        sb_appendf(&sb, "Build memory: %s\n", atlas->szBuildCommand);
    if (atlas->szTestCommand[0])
        sb_appendf(&sb, "Test memory: %s\n", atlas->szTestCommand);
    if (req->pszAtlasSummary && req->pszAtlasSummary[0])
        sb_appendf(&sb, "Editor atlas hint: %s\n", req->pszAtlasSummary);

    atlas->cards[0].score = 100;
    _snprintf_s(atlas->cards[0].szKind, sizeof(atlas->cards[0].szKind), _TRUNCATE, "file");
    _snprintf_s(atlas->cards[0].szFile, sizeof(atlas->cards[0].szFile), _TRUNCATE, "%s", safe_cstr(req->pszFilePath));
    _snprintf_s(atlas->cards[0].szSummary, sizeof(atlas->cards[0].szSummary), _TRUNCATE,
                "Current working file in %s, cursor %d:%d",
                safe_cstr(req->pszLanguage), req->iCursorLine, req->iCursorCol);
    atlas->count = 1;

    atlas->pszSummary = sb.data;
    AtlasCache_Put(key, atlas->pszSummary);
}

static void FreeAtlasLite(AtlasLite* atlas)
{
    if (atlas->pszSummary) free(atlas->pszSummary);
    atlas->pszSummary = NULL;
}

static BOOL LeaseScheduler_Acquire(const char* owner, const char* scope)
{
    if (!scope || !scope[0]) return TRUE;
    for (int i = 0; i < MAX_ACTIVE_LEASES; i++)
    {
        if (s_leases[i].bActive && strcmp(s_leases[i].szScope, scope) == 0 &&
            strcmp(s_leases[i].szOwner, safe_cstr(owner)) != 0)
            return FALSE;
    }
    for (int i = 0; i < MAX_ACTIVE_LEASES; i++)
    {
        if (!s_leases[i].bActive)
        {
            s_leases[i].bActive = TRUE;
            s_leases[i].dwTick = GetTickCount();
            _snprintf_s(s_leases[i].szOwner, sizeof(s_leases[i].szOwner), _TRUNCATE, "%s", safe_cstr(owner));
            _snprintf_s(s_leases[i].szScope, sizeof(s_leases[i].szScope), _TRUNCATE, "%s", safe_cstr(scope));
            return TRUE;
        }
    }
    return TRUE;
}

static void LeaseScheduler_Release(const char* owner)
{
    for (int i = 0; i < MAX_ACTIVE_LEASES; i++)
    {
        if (s_leases[i].bActive && strcmp(s_leases[i].szOwner, safe_cstr(owner)) == 0)
            ZeroMemory(&s_leases[i], sizeof(s_leases[i]));
    }
}

static void MissionQueue_Push(const char* id, const char* phase, const char* summary, const char* risk)
{
    MissionRecord rec;
    ZeroMemory(&rec, sizeof(rec));
    _snprintf_s(rec.szId, sizeof(rec.szId), _TRUNCATE, "%s", safe_cstr(id));
    _snprintf_s(rec.szPhase, sizeof(rec.szPhase), _TRUNCATE, "%s", safe_cstr(phase));
    _snprintf_s(rec.szSummary, sizeof(rec.szSummary), _TRUNCATE, "%s", safe_cstr(summary));
    _snprintf_s(rec.szRisk, sizeof(rec.szRisk), _TRUNCATE, "%s", safe_cstr(risk));
    rec.dwTick = GetTickCount();

    if (s_missionHistoryCount < MAX_MISSION_HISTORY)
        s_missionHistory[s_missionHistoryCount++] = rec;
    else
    {
        memmove(&s_missionHistory[0], &s_missionHistory[1], sizeof(MissionRecord) * (MAX_MISSION_HISTORY - 1));
        s_missionHistory[MAX_MISSION_HISTORY - 1] = rec;
    }
}

static void MissionQueue_Format(StrBuf* sb)
{
    for (int i = 0; i < s_missionHistoryCount; i++)
    {
        sb_appendf(sb, "%s [%s] %s",
                   s_missionHistory[i].szId,
                   s_missionHistory[i].szPhase,
                   s_missionHistory[i].szSummary);
        if (i + 1 < s_missionHistoryCount)
            sb_append(sb, "\n", 1);
    }
}

static void FillProofFromHeuristics(const PipeRequest* req, const AtlasLite* atlas,
                                    const IntentSpec* intent, const char* diff,
                                    PatchProof* proof)
{
    char touched[512];
    char fp[96];
    unsigned __int64 h = fnv1a64(safe_cstr(diff));
    hash_to_hex(h, fp, sizeof(fp));
    GuessTouchedSymbols(req, touched, sizeof(touched));

    _snprintf_s(proof->szTouchedSymbols, sizeof(proof->szTouchedSymbols), _TRUNCATE, "%s", touched);
    _snprintf_s(proof->szAssumptions, sizeof(proof->szAssumptions), _TRUNCATE,
                "Assumed current buffer hash %s, scope %s, and editor atlas summary are current.",
                req->pszBufferHash ? req->pszBufferHash : "unknown",
                req->pszScope ? req->pszScope : "selection");
    _snprintf_s(proof->szValidations, sizeof(proof->szValidations), _TRUNCATE,
                "Verifier: request anchored to project root %s. Diff present: %s. Build memory: %s. Test memory: %s.",
                req->pszProjectRoot ? req->pszProjectRoot : "(none)",
                (diff && diff[0]) ? "yes" : "no",
                atlas->szBuildCommand[0] ? atlas->szBuildCommand : "(none)",
                atlas->szTestCommand[0] ? atlas->szTestCommand : "(none)");
    _snprintf_s(proof->szReviewerVotes, sizeof(proof->szReviewerVotes), _TRUNCATE,
                "Scout: pass (atlas-lite ready). Verifier: %s. Adversary: %s.",
                (diff && strstr(diff, "@@")) ? "pass (unified diff detected)" : "warn (no hunks found)",
                (diff && (strstr(diff, "DeleteFile") || strstr(diff, "TerminateProcess") || strstr(diff, "RemoveDirectory")))
                    ? "warn (destructive API touched)"
                    : "pass (no obvious destructive pattern)");
    _snprintf_s(proof->szResidualRisk, sizeof(proof->szResidualRisk), _TRUNCATE,
                "%s risk. Review edge cases around %s.",
                intent->szRisk[0] ? intent->szRisk : "medium",
                touched[0] ? touched : "touched symbols");
    _snprintf_s(proof->szRollbackFingerprint, sizeof(proof->szRollbackFingerprint), _TRUNCATE, "%s", fp);
    _snprintf_s(proof->szBuildCommand, sizeof(proof->szBuildCommand), _TRUNCATE, "%s", atlas->szBuildCommand);
    _snprintf_s(proof->szTestCommand, sizeof(proof->szTestCommand), _TRUNCATE, "%s", atlas->szTestCommand);
    _snprintf_s(proof->szSummary, sizeof(proof->szSummary), _TRUNCATE,
                "Mission compiled, atlas-lite consulted, worker diff generated, verifier/adversary heuristics attached.");
    proof->dConfidence = (diff && strstr(diff, "@@")) ? 0.78 : 0.46;
    proof->iCandidateRank = 1;
    proof->bGhostLayer = TRUE;
    proof->bStale = FALSE;
}

static void BuildMissionId(const char* reqId, char* out, int cchOut)
{
    LONG seq = InterlockedIncrement(&s_nextMissionSeq);
    _snprintf_s(out, cchOut, _TRUNCATE, "mission_%s_%ld",
                (reqId && reqId[0]) ? reqId : "anon", seq);
}

//=============================================================================
// Prompt construction
//=============================================================================

static const char* SYSTEM_PROMPT_PATCH =
    "You are the PatchWorker inside Bikode Delta Mesh, the editing engine for Bikode, an AI-first IDE. "
    "Bikode's motto is \"I write what I like.\" Preserve the author's intent and voice while making the smallest safe change. "
    "Respond with ONLY a unified diff. "
    "Use standard unified diff format with --- and +++ headers and @@ hunk markers. "
    "Do not include any prose outside the diff. "
    "Keep changes minimal, safe, and directly applicable to the provided file content.";

static const char* SYSTEM_PROMPT_EXPLAIN =
    "You are Bikode's code explanation assistant inside an AI-first IDE. "
    "Bikode's motto is \"I write what I like.\" Help the user understand the code well enough to keep writing in their own voice. "
    "Provide a clear, concise explanation of the given code. "
    "Focus on what the code does, any notable patterns, and potential issues.";

static const char* SYSTEM_PROMPT_CHAT =
    "You are an AI coding assistant embedded in Bikode, an AI-first IDE. "
    "Bikode's motto is \"I write what I like.\" Help the user write with conviction, preserve their voice, and keep AI in service of the author's intent. "
    "You help with programming questions, code review, debugging, and general development tasks. "
    "Be concise and practical. When suggesting code changes, provide them as diffs when appropriate.";

static const char* SYSTEM_PROMPT_STYLE_RULES =
    " In prose, use typographic opening and closing quote marks instead of straight ASCII quotes when the format allows it. "
    "Keep ASCII quotes only where syntax requires them, such as code, JSON, diffs, shell commands, or patches. "
    "Do not use em dashes. "
    "Avoid negative-parallel constructions such as \"While X is true, Y...\" and \"Not only X, but also Y...\". "
    "Avoid sycophantic language. Write with concise authority. "
    "Never use these words or close variants in prose: crucial, delve, amplify, archetypal, at the heart of, augment, blend, catalyze, catalyst, catering, centerpiece, cohesion, cohesive, comprehensive, conceptualize, confluence, digital bazaar, dynamics, elucidate, embark, embodiment, embody, emanate, encompass, envisage, epitomize, evoke, exemplify, extrapolate, facilitating, facet, fusion, harmony, harnessing, holistic, illuminating, immanent, implications, in essence, infuse, inflection, inherent, instigate, integral, integration, intrinsic, intricacies, iteration, leverage, manifestation, mosaic, nuance, paradigm, pinnacle, prerequisite, quintessential, reinforce, resilience, resonate, reverberate, subtlety, substantiate, symbiosis, synergy, synthesize, tapestry, underlying, unify, unity, unravel, unveil.";

static void BuildPrompt(PipeRequest* req, const IntentSpec* intent, const AtlasLite* atlas,
                        const char* pszMissionId, StrBuf* sbSystem, StrBuf* sbUser)
{
    if (req->pszType && strcmp(req->pszType, "chat") == 0)
        sb_append(sbSystem, SYSTEM_PROMPT_CHAT, -1);
    else if (req->pszType && strcmp(req->pszType, "explain") == 0)
        sb_append(sbSystem, SYSTEM_PROMPT_EXPLAIN, -1);
    else
        sb_append(sbSystem, SYSTEM_PROMPT_PATCH, -1);

    sb_append(sbSystem, SYSTEM_PROMPT_STYLE_RULES, -1);

    if (pszMissionId && pszMissionId[0])
        sb_appendf(sbSystem, "\nMission id: %s", pszMissionId);
    if (intent && intent->szRisk[0])
        sb_appendf(sbSystem, "\nRisk tier: %s", intent->szRisk);

    if (req->pszType && strcmp(req->pszType, "chat") == 0)
    {
        if (req->pszFilePath && req->pszFilePath[0])
        {
            sb_appendf(sbUser, "Current file: %s", req->pszFilePath);
            if (req->pszLanguage && req->pszLanguage[0])
                sb_appendf(sbUser, " (language: %s)", req->pszLanguage);
            sb_append(sbUser, "\n\n", 2);
        }
        if (req->pszSelection && req->pszSelection[0])
        {
            sb_append(sbUser, "Selected code:\n```\n", -1);
            sb_append(sbUser, req->pszSelection, -1);
            sb_append(sbUser, "\n```\n\n", -1);
        }
        if (req->pszChatMessage)
            sb_append(sbUser, req->pszChatMessage, -1);
    }
    else if (req->pszType && strcmp(req->pszType, "explain") == 0)
    {
        if (atlas && atlas->pszSummary)
            sb_appendf(sbUser, "Atlas-lite:\n%s\n\n", atlas->pszSummary);
        if (req->pszSelection && req->pszSelection[0])
        {
            sb_appendf(sbUser, "Explain this %s code:\n\n```\n%s\n```",
                req->pszLanguage ? req->pszLanguage : "",
                req->pszSelection);
        }
        else if (req->pszFileContent)
        {
            sb_appendf(sbUser, "Explain this file (%s):\n\n```\n%s\n```",
                req->pszFilePath ? req->pszFilePath : "unknown",
                req->pszFileContent);
        }
    }
    else
    {
        if (intent)
        {
            sb_appendf(sbUser, "Goal: %s\n", intent->szGoal);
            sb_appendf(sbUser, "Acceptance: %s\n", intent->szAcceptance);
            sb_appendf(sbUser, "Risk tier: %s\n\n", intent->szRisk);
        }
        if (atlas && atlas->pszSummary)
        {
            sb_append(sbUser, "Atlas-lite:\n", -1);
            sb_append(sbUser, atlas->pszSummary, -1);
            sb_append(sbUser, "\n", 1);
        }
        if (req->pszFilePath)
            sb_appendf(sbUser, "File: %s\n", req->pszFilePath);
        if (req->pszLanguage)
            sb_appendf(sbUser, "Language: %s\n\n", req->pszLanguage);
        if (req->pszFileContent)
        {
            sb_append(sbUser, "Full file content:\n```\n", -1);
            sb_append(sbUser, req->pszFileContent, -1);
            sb_append(sbUser, "\n```\n\n", -1);
        }
        if (req->pszSelection && req->pszSelection[0])
        {
            sb_appendf(sbUser, "Selected code (lines %d-%d):\n```\n%s\n```\n\n",
                req->iSelStartLine, req->iSelEndLine, req->pszSelection);
        }
        if (req->pszGitSummary && req->pszGitSummary[0])
            sb_appendf(sbUser, "Git summary: %s\n", req->pszGitSummary);
        if (req->pszDiagnostics && req->pszDiagnostics[0])
            sb_appendf(sbUser, "Diagnostics: %s\n", req->pszDiagnostics);
        if (atlas && atlas->szBuildCommand[0])
            sb_appendf(sbUser, "Build command memory: %s\n", atlas->szBuildCommand);
        if (atlas && atlas->szTestCommand[0])
            sb_appendf(sbUser, "Test command memory: %s\n", atlas->szTestCommand);
        if (req->pszInstruction && req->pszInstruction[0])
            sb_appendf(sbUser, "Instruction: %s\n", req->pszInstruction);
        else if (req->pszAction)
            sb_appendf(sbUser, "Action: %s the selected code.\n", req->pszAction);
    }
}

//=============================================================================
// Request body builders (per-format)
//=============================================================================

// Build OpenAI-compatible request body (used by most providers)
static void BuildBody_OpenAI(StrBuf* body, const AIProviderConfig* cfg,
                              const char* systemPrompt, const char* userMessage)
{
    const char* model = cfg->szModel[0] ? cfg->szModel : "gpt-4o-mini";

    sb_append(body, "{", 1);
    sb_appendf(body, "\"model\":\"%s\",", model);
    sb_appendf(body, "\"temperature\":%.2f,", cfg->dTemperature);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, "\"max_tokens\":%d,", cfg->iMaxTokens);

    sb_append(body, "\"messages\":[", -1);

    // System message
    sb_append(body, "{\"role\":\"system\",\"content\":", -1);
    json_escape_append(body, systemPrompt);
    sb_append(body, "},", -1);

    // User message
    sb_append(body, "{\"role\":\"user\",\"content\":", -1);
    json_escape_append(body, userMessage);
    sb_append(body, "}", -1);

    sb_append(body, "]}", -1);
}

// Build Anthropic request body (Messages API)
static void BuildBody_Anthropic(StrBuf* body, const AIProviderConfig* cfg,
                                 const char* systemPrompt, const char* userMessage)
{
    const char* model = cfg->szModel[0] ? cfg->szModel : "claude-sonnet-4-20250514";

    sb_append(body, "{", 1);
    sb_appendf(body, "\"model\":\"%s\",", model);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, "\"max_tokens\":%d,", cfg->iMaxTokens);
    else
        sb_append(body, "\"max_tokens\":4096,", -1);  // Required for Anthropic

    sb_appendf(body, "\"temperature\":%.2f,", cfg->dTemperature);

    // System prompt is a top-level field, NOT in messages
    sb_append(body, "\"system\":", -1);
    json_escape_append(body, systemPrompt);
    sb_append(body, ",", 1);

    // Messages (user only â€” system is separate)
    sb_append(body, "\"messages\":[", -1);
    sb_append(body, "{\"role\":\"user\",\"content\":", -1);
    json_escape_append(body, userMessage);
    sb_append(body, "}", -1);
    sb_append(body, "]}", -1);
}

// Build Google Gemini request body
static void BuildBody_Google(StrBuf* body, const AIProviderConfig* cfg,
                              const char* systemPrompt, const char* userMessage)
{
    sb_append(body, "{", 1);

    // System instruction
    sb_append(body, "\"systemInstruction\":{\"parts\":[{\"text\":", -1);
    json_escape_append(body, systemPrompt);
    sb_append(body, "}]},", -1);

    // Contents
    sb_append(body, "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":", -1);
    json_escape_append(body, userMessage);
    sb_append(body, "}]}],", -1);

    // Generation config
    sb_appendf(body, "\"generationConfig\":{\"temperature\":%.2f", cfg->dTemperature);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, ",\"maxOutputTokens\":%d", cfg->iMaxTokens);
    sb_append(body, "}}", -1);
}

// Build Cohere v2 request body
static void BuildBody_Cohere(StrBuf* body, const AIProviderConfig* cfg,
                              const char* systemPrompt, const char* userMessage)
{
    const char* model = cfg->szModel[0] ? cfg->szModel : "command-r-plus";

    sb_append(body, "{", 1);
    sb_appendf(body, "\"model\":\"%s\",", model);
    sb_appendf(body, "\"temperature\":%.2f,", cfg->dTemperature);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, "\"max_tokens\":%d,", cfg->iMaxTokens);

    sb_append(body, "\"messages\":[", -1);
    sb_append(body, "{\"role\":\"system\",\"content\":", -1);
    json_escape_append(body, systemPrompt);
    sb_append(body, "},", -1);
    sb_append(body, "{\"role\":\"user\",\"content\":", -1);
    json_escape_append(body, userMessage);
    sb_append(body, "}", -1);
    sb_append(body, "]}", -1);
}

// Dispatch to correct body builder based on format
static void BuildRequestBody(StrBuf* body, const AIProviderConfig* cfg,
                              const AIProviderDef* pDef,
                              const char* systemPrompt, const char* userMessage)
{
    EAIRequestFormat fmt = pDef ? pDef->eFormat : AI_FORMAT_OPENAI;

    switch (fmt)
    {
    case AI_FORMAT_ANTHROPIC:
        BuildBody_Anthropic(body, cfg, systemPrompt, userMessage);
        break;
    case AI_FORMAT_GOOGLE:
        BuildBody_Google(body, cfg, systemPrompt, userMessage);
        break;
    case AI_FORMAT_COHERE:
        BuildBody_Cohere(body, cfg, systemPrompt, userMessage);
        break;
    case AI_FORMAT_OPENAI:
    default:
        BuildBody_OpenAI(body, cfg, systemPrompt, userMessage);
        break;
    }
}

//=============================================================================
// Auth header construction
//=============================================================================

static void BuildAuthHeaders(HINTERNET hRequest, const AIProviderConfig* cfg,
                              const AIProviderDef* pDef)
{
    if (!pDef) return;

    WCHAR wszHeader[2048];

    switch (pDef->eAuth)
    {
    case AI_AUTH_BEARER:
        if (cfg->szApiKey[0])
        {
            swprintf(wszHeader, 2048, L"Authorization: Bearer %hs", cfg->szApiKey);
            WinHttpAddRequestHeaders(hRequest, wszHeader, -1, WINHTTP_ADDREQ_FLAG_ADD);
        }
        break;

    case AI_AUTH_XAPIKEY:
        if (cfg->szApiKey[0])
        {
            swprintf(wszHeader, 2048, L"x-api-key: %hs", cfg->szApiKey);
            WinHttpAddRequestHeaders(hRequest, wszHeader, -1, WINHTTP_ADDREQ_FLAG_ADD);
        }
        break;

    case AI_AUTH_QUERY_PARAM:
        // Key is added to URL path, not as header â€” handled in CallLLMAPI
        break;

    case AI_AUTH_NONE:
    default:
        break;
    }

    // Content-Type is always JSON
    WinHttpAddRequestHeaders(hRequest,
        L"Content-Type: application/json", -1, WINHTTP_ADDREQ_FLAG_ADD);

    // Extra headers from provider definition (semicolon-separated)
    if (pDef->szExtraHeaders && pDef->szExtraHeaders[0])
    {
        char extraBuf[1024];
        strncpy(extraBuf, pDef->szExtraHeaders, sizeof(extraBuf) - 1);
        extraBuf[sizeof(extraBuf) - 1] = '\0';

        char* savePtr = NULL;
        char* token = strtok_s(extraBuf, ";", &savePtr);
        while (token)
        {
            while (*token == ' ') token++;
            WCHAR wszExtra[512];
            MultiByteToWideChar(CP_UTF8, 0, token, -1, wszExtra, 512);
            WinHttpAddRequestHeaders(hRequest, wszExtra, -1, WINHTTP_ADDREQ_FLAG_ADD);
            token = strtok_s(NULL, ";", &savePtr);
        }
    }
}

//=============================================================================
// Response parsing (per-format)
//=============================================================================

// Parse OpenAI-style response: choices[0].message.content
static char* ExtractOpenAIContentText(const char* jsonScope)
{
    if (!jsonScope) return NULL;

    char* text = json_extract_string(jsonScope, "content");
    if (text && text[0]) return text;
    if (text) free(text);

    const char* contentVal = json_find_value(jsonScope, "content");
    if (contentVal && (*contentVal == '[' || *contentVal == '{'))
    {
        text = json_extract_string(contentVal, "text");
        if (text && text[0]) return text;
        if (text) free(text);

        text = json_extract_string(contentVal, "refusal");
        if (text && text[0])
        {
            char* full = (char*)malloc(strlen(text) + 32);
            if (full)
            {
                sprintf(full, "Model refused: %s", text);
                free(text);
                return full;
            }
            return text;
        }
        if (text) free(text);
    }

    text = json_extract_string(jsonScope, "text");
    if (text && text[0]) return text;
    if (text) free(text);

    return NULL;
}

static char* ParseResponse_OpenAI(const char* respJson)
{
    char* content = json_extract_nested_content(respJson, "choices", "content");
    if (content && content[0]) return content;
    if (content) free(content);

    const char* choices = json_find_value(respJson, "choices");
    if (choices)
    {
        const char* message = json_find_value(choices, "message");
        if (message)
        {
            content = ExtractOpenAIContentText(message);
            if (content && content[0]) return content;
            if (content) free(content);
        }

        content = ExtractOpenAIContentText(choices);
        if (content && content[0]) return content;
        if (content) free(content);
    }

    content = ExtractOpenAIContentText(respJson);
    if (content && content[0]) return content;
    if (content) free(content);

    return NULL;
}

// Parse Anthropic response: content[0].text
static char* ParseResponse_Anthropic(const char* respJson)
{
    char* text = json_extract_nested_content(respJson, "content", "text");
    if (text && text[0]) return text;
    if (text) free(text);

    text = json_extract_string(respJson, "text");
    if (text && text[0]) return text;
    if (text) free(text);

    return NULL;
}

// Parse Google Gemini response: candidates[0].content.parts[0].text
static char* ParseResponse_Google(const char* respJson)
{
    const char* p = json_find_value(respJson, "candidates");
    if (p)
    {
        const char* content = json_find_value(p, "content");
        if (content)
        {
            const char* parts = json_find_value(content, "parts");
            if (parts)
            {
                char* text = json_extract_string(parts, "text");
                if (text && text[0]) return text;
                if (text) free(text);
            }
        }
    }

    char* text = json_extract_string(respJson, "text");
    if (text && text[0]) return text;
    if (text) free(text);

    return NULL;
}

// Parse Cohere response: message.content[0].text or text
static char* ParseResponse_Cohere(const char* respJson)
{
    const char* msg = json_find_value(respJson, "message");
    if (msg)
    {
        char* text = json_extract_nested_content(msg, "content", "text");
        if (text && text[0]) return text;
        if (text) free(text);
    }

    char* text = json_extract_string(respJson, "text");
    if (text && text[0]) return text;
    if (text) free(text);

    return NULL;
}

// Dispatch to correct parser
static char* ParseLLMResponse(const char* respJson, EAIRequestFormat fmt)
{
    char* result = NULL;

    switch (fmt)
    {
    case AI_FORMAT_ANTHROPIC:
        result = ParseResponse_Anthropic(respJson);
        break;
    case AI_FORMAT_GOOGLE:
        result = ParseResponse_Google(respJson);
        break;
    case AI_FORMAT_COHERE:
        result = ParseResponse_Cohere(respJson);
        break;
    case AI_FORMAT_OPENAI:
    default:
        result = ParseResponse_OpenAI(respJson);
        break;
    }

    if (result) return result;

    // Check for error in response (common across providers)
    char* errMsg = json_extract_string(respJson, "message");
    if (!errMsg) errMsg = json_extract_string(respJson, "error");
    if (errMsg)
    {
        char* full = (char*)malloc(strlen(errMsg) + 32);
        if (full) sprintf(full, "API Error: %s", errMsg);
        free(errMsg);
        return full ? full : _strdup("Error: API returned an error");
    }

    return _strdup("Error: Could not parse API response");
}

//=============================================================================
// WinHTTP: Multi-provider LLM call
//=============================================================================

static char* CallLLMAPI(const AIProviderConfig* cfg,
                         const char* systemPrompt, const char* userMessage)
{
    const AIProviderDef* pDef = AIProvider_Get(cfg->eProvider);
    if (!pDef)
        return _strdup("Error: Unknown provider");

    // Check API key requirement
    if (pDef->bRequiresKey && !cfg->szApiKey[0])
    {
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf),
                 "Error: No API key configured for %s. Set %s environment variable or use --key.",
                 pDef->szName,
                 pDef->szEnvVarKey ? pDef->szEnvVarKey : "biko_ai_KEY");
        return _strdup(errBuf);
    }

    // Resolve connection parameters
    const char *szHost, *szPath;
    int iPort;
    int bSSL;
    AIProviderConfig_Resolve(cfg, &szHost, &szPath, &iPort, &bSSL);

    if (s_state.bVerbose)
    {
        fprintf(stderr, "biko-engine: provider=%s model=%s host=%s path=%s port=%d ssl=%d\n",
                pDef->szName, cfg->szModel, szHost, szPath, iPort, bSSL);
    }

    // Build request body
    StrBuf body;
    sb_init(&body, 4096);
    BuildRequestBody(&body, cfg, pDef, systemPrompt, userMessage);

    if (s_state.bVerbose)
        fprintf(stderr, "biko-engine: request body (%d bytes)\n", body.len);

    // --- WinHTTP session ---
    WCHAR wszHost[512];
    MultiByteToWideChar(CP_UTF8, 0, szHost, -1, wszHost, 512);

    // Build path (with query param auth for Google)
    WCHAR wszPath[1024];
    if (pDef->eAuth == AI_AUTH_QUERY_PARAM && cfg->szApiKey[0])
    {
        char fullPath[1024];
        snprintf(fullPath, sizeof(fullPath), "%s?key=%s", szPath, cfg->szApiKey);
        MultiByteToWideChar(CP_UTF8, 0, fullPath, -1, wszPath, 1024);
    }
    else
    {
        MultiByteToWideChar(CP_UTF8, 0, szPath, -1, wszPath, 1024);
    }

    HINTERNET hSession = WinHttpOpen(L"MonoEngine/0.2",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) { sb_free(&body); return _strdup("Error: WinHttpOpen failed"); }

    // Set timeouts
    int timeoutMs = cfg->iTimeoutSec > 0 ? cfg->iTimeoutSec * 1000 : 120000;
    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, wszHost,
        (INTERNET_PORT)iPort, 0);

    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        sb_free(&body);
        return _strdup("Error: WinHttpConnect failed -- check host/port");
    }

    DWORD dwFlags = bSSL ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wszPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        dwFlags);

    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        sb_free(&body);
        return _strdup("Error: WinHttpOpenRequest failed");
    }

    // For local models on HTTP, disable SSL certificate checks
    if (!bSSL)
    {
        DWORD dwSecFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                         &dwSecFlags, sizeof(dwSecFlags));
    }

    // Add auth & content headers
    BuildAuthHeaders(hRequest, cfg, pDef);

    // Send request
    BOOL bSent = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        body.data, body.len, body.len, 0);

    sb_free(&body);

    if (!bSent)
    {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        char buf[128];
        snprintf(buf, sizeof(buf), "Error: WinHttpSendRequest failed (err=%lu)", err);
        return _strdup(buf);
    }

    if (!WinHttpReceiveResponse(hRequest, NULL))
    {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        char buf[128];
        snprintf(buf, sizeof(buf), "Error: WinHttpReceiveResponse failed (err=%lu)", err);
        return _strdup(buf);
    }

    // Check HTTP status code
    DWORD dwStatusCode = 0;
    DWORD dwSize = sizeof(dwStatusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &dwStatusCode, &dwSize, NULL);

    // Read response body
    StrBuf resp;
    sb_init(&resp, 4096);

    DWORD dwRead;
    char buf[8192];
    while (WinHttpReadData(hRequest, buf, sizeof(buf), &dwRead) && dwRead > 0)
    {
        sb_append(&resp, buf, dwRead);
        if (resp.len >= MAX_RESPONSE_SIZE) break;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (s_state.bVerbose)
    {
        fprintf(stderr, "biko-engine: HTTP %lu, response (%d bytes)\n",
                dwStatusCode, resp.len);
        if (resp.len < 2000)
            fprintf(stderr, "biko-engine: %s\n", resp.data);
    }

    // Non-2xx status
    if (dwStatusCode < 200 || dwStatusCode >= 300)
    {
        char* httpErr = (char*)malloc(resp.len + 128);
        if (httpErr)
        {
            char* errMsg = json_extract_string(resp.data, "message");
            if (!errMsg) errMsg = json_extract_string(resp.data, "error");
            if (errMsg)
            {
                snprintf(httpErr, resp.len + 128, "API Error (HTTP %lu): %s",
                         dwStatusCode, errMsg);
                free(errMsg);
            }
            else
            {
                snprintf(httpErr, resp.len + 128, "API Error: HTTP %lu from %s",
                         dwStatusCode, pDef->szName);
            }
            sb_free(&resp);
            return httpErr;
        }
        sb_free(&resp);
        return _strdup("Error: Non-2xx HTTP status");
    }

    // Parse the response according to provider format
    char* content = ParseLLMResponse(resp.data, pDef->eFormat);
    sb_free(&resp);

    return content ? content : _strdup("Error: Empty response from API");
}

//=============================================================================
// Web Search using Playwright (DuckDuckGo)
//=============================================================================

static char* WebSearch(const char* query, int maxResults)
{
    if (!query || !query[0])
        return _strdup("{\"success\":false,\"error\":\"No query provided\",\"results\":[]}");

    // Resolve path to web-search.js in common dev/runtime layouts.
    WCHAR wszExePath[MAX_PATH];
    GetModuleFileNameW(NULL, wszExePath, MAX_PATH);
    WCHAR* lastSlash = wcsrchr(wszExePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';

    WCHAR wszScriptPath[MAX_PATH];
    BOOL foundScript = FALSE;
    const WCHAR* candidates[] = {
        L"src\\Extension\\tools\\web-search.js",
        L"src\\tools\\web-search.js",
        L"..\\..\\..\\src\\Extension\\tools\\web-search.js",
        L"..\\..\\..\\src\\tools\\web-search.js",
        NULL
    };
    for (int i = 0; candidates[i]; i++)
    {
        if (i < 2)
            swprintf(wszScriptPath, MAX_PATH, L"%s", candidates[i]);
        else
            swprintf(wszScriptPath, MAX_PATH, L"%s\\%s", wszExePath, candidates[i]);

        DWORD attrs = GetFileAttributesW(wszScriptPath);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            foundScript = TRUE;
            break;
        }
    }

    if (!foundScript)
        return _strdup("{\"success\":false,\"error\":\"web-search.js not found\",\"results\":[]}");

    // Build command: node "<script>" "query" --max=N
    WCHAR wszCmd[4096];
    WCHAR wszQuery[2048];
    MultiByteToWideChar(CP_UTF8, 0, query, -1, wszQuery, 2048);

    // Escape quotes in query
    for (WCHAR* p = wszQuery; *p; p++)
        if (*p == L'"') *p = L'\'';

    swprintf(wszCmd, 4096,
        L"node \"%s\" \"%s\" --max=%d",
        wszScriptPath, wszQuery, maxResults > 0 ? maxResults : 5);

    // Create pipe for reading stdout
    HANDLE hReadPipe, hWritePipe;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return _strdup("{\"success\":false,\"error\":\"CreatePipe failed\",\"results\":[]}");

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // Set up process
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessW(NULL, wszCmd, NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return _strdup("{\"success\":false,\"error\":\"Node.js not found or script missing\",\"results\":[]}");
    }

    CloseHandle(hWritePipe);

    // Read output with timeout (30 seconds max)
    StrBuf output;
    sb_init(&output, 4096);
    char buf[1024];
    DWORD dwRead;
    DWORD startTick = GetTickCount();

    while (GetTickCount() - startTick < 30000)
    {
        DWORD avail = 0;
        if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0)
        {
            if (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &dwRead, NULL) && dwRead > 0)
            {
                buf[dwRead] = '\0';
                sb_append(&output, buf, dwRead);
            }
        }
        else
        {
            DWORD exitCode;
            if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode != STILL_ACTIVE)
                break;
            Sleep(50);
        }
    }

    // Clean up
    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (output.len == 0)
    {
        sb_free(&output);
        return _strdup("{\"success\":false,\"error\":\"Search timeout or no output\",\"results\":[]}");
    }

    return output.data;
}

//=============================================================================
// Resolve per-request provider config
//=============================================================================

static void ResolveRequestConfig(const PipeRequest* req, AIProviderConfig* outCfg)
{
    // Start with engine's base config
    memcpy(outCfg, &s_state.providerCfg, sizeof(AIProviderConfig));

    // Apply per-request overrides from editor
    if (req->pszProvider && req->pszProvider[0])
    {
        const AIProviderDef* pOverride = AIProvider_FindBySlug(req->pszProvider);
        if (pOverride)
        {
            outCfg->eProvider = pOverride->id;
            if (pOverride->szDefaultModel)
                strncpy(outCfg->szModel, pOverride->szDefaultModel,
                        sizeof(outCfg->szModel) - 1);
            if (pOverride->szEnvVarKey && !outCfg->szApiKey[0])
            {
                char envBuf[512];
                DWORD len = GetEnvironmentVariableA(pOverride->szEnvVarKey,
                                                     envBuf, sizeof(envBuf));
                if (len > 0 && len < sizeof(envBuf))
                    strncpy(outCfg->szApiKey, envBuf, sizeof(outCfg->szApiKey) - 1);
            }
        }
    }

    if (req->pszModel && req->pszModel[0])
        strncpy(outCfg->szModel, req->pszModel, sizeof(outCfg->szModel) - 1);

    if (req->pszApiKey && req->pszApiKey[0])
        strncpy(outCfg->szApiKey, req->pszApiKey, sizeof(outCfg->szApiKey) - 1);
}

//=============================================================================
// Process a single request from the editor
//=============================================================================

static void BuildTextResponse(StrBuf* sb, const char* id, const char* type,
                              const char* content, const char* model,
                              const char* missionId, const char* missionPhase,
                              const char* missionSummary, const char* missionQueue,
                              const AtlasLite* atlas)
{
    sb_append(sb, "{", 1);
    sb_append(sb, "\"id\":", -1);
    json_escape_append(sb, id ? id : "");
    sb_append(sb, ",\"status\":\"ok\"", -1);

    if (type && strcmp(type, "chat") == 0)
    {
        sb_append(sb, ",\"chat_response\":", -1);
        json_escape_append(sb, content ? content : "");
    }
    else if (type && strcmp(type, "explain") == 0)
    {
        sb_append(sb, ",\"explanation\":{\"summary\":", -1);
        json_escape_append(sb, content ? content : "");
        sb_append(sb, ",\"details\":", -1);
        json_escape_append(sb, content ? content : "");
        sb_append(sb, "}", 1);
    }

    if (missionId && missionId[0]) {
        sb_append(sb, ",\"missionId\":", -1);
        json_escape_append(sb, missionId);
    }
    if (missionPhase && missionPhase[0]) {
        sb_append(sb, ",\"missionPhase\":", -1);
        json_escape_append(sb, missionPhase);
    }
    if (missionSummary && missionSummary[0]) {
        sb_append(sb, ",\"missionSummary\":", -1);
        json_escape_append(sb, missionSummary);
    }
    if (missionQueue && missionQueue[0]) {
        sb_append(sb, ",\"missionQueue\":", -1);
        json_escape_append(sb, missionQueue);
    }
    if (atlas && atlas->pszSummary && atlas->pszSummary[0]) {
        sb_append(sb, ",\"atlasSummary\":", -1);
        json_escape_append(sb, atlas->pszSummary);
    }

    sb_append(sb, ",\"meta\":{\"model\":", -1);
    json_escape_append(sb, model ? model : "");
    sb_append(sb, "}", 1);
    sb_append(sb, "}", 1);
}

static void BuildPatchResponse(StrBuf* sb, const PipeRequest* req,
                               const char* missionId, const char* missionPhase,
                               const char* missionSummary, const char* missionQueue,
                               const AtlasLite* atlas, const PatchProof* proof,
                               const char* diff, const char* model)
{
    sb_append(sb, "{", 1);
    sb_append(sb, "\"id\":", -1);
    json_escape_append(sb, req->pszId ? req->pszId : "");
    sb_append(sb, ",\"status\":\"ok\"", -1);

    sb_append(sb, ",\"patches\":[{", -1);
    sb_append(sb, "\"file\":", -1);
    json_escape_append(sb, req->pszFilePath ? req->pszFilePath : "");
    sb_append(sb, ",\"diff\":", -1);
    json_escape_append(sb, diff ? diff : "");
    sb_append(sb, ",\"description\":", -1);
    json_escape_append(sb, req->pszInstruction && req->pszInstruction[0]
                              ? req->pszInstruction
                              : "Bikode Delta Mesh candidate layer");
    sb_append(sb, ",\"proofSummary\":", -1);
    json_escape_append(sb, proof->szSummary);
    sb_append(sb, ",\"touchedSymbols\":", -1);
    json_escape_append(sb, proof->szTouchedSymbols);
    sb_append(sb, ",\"assumptions\":", -1);
    json_escape_append(sb, proof->szAssumptions);
    sb_append(sb, ",\"validations\":", -1);
    json_escape_append(sb, proof->szValidations);
    sb_append(sb, ",\"reviewerVotes\":", -1);
    json_escape_append(sb, proof->szReviewerVotes);
    sb_append(sb, ",\"residualRisk\":", -1);
    json_escape_append(sb, proof->szResidualRisk);
    sb_append(sb, ",\"rollbackFingerprint\":", -1);
    json_escape_append(sb, proof->szRollbackFingerprint);
    sb_append(sb, ",\"baseBufferHash\":", -1);
    json_escape_append(sb, req->pszBufferHash ? req->pszBufferHash : "");
    sb_appendf(sb, ",\"baseBufferVersion\":%d", req->iBufferVersion);
    sb_appendf(sb, ",\"confidence\":%.2f", proof->dConfidence);
    sb_appendf(sb, ",\"candidateRank\":%d", proof->iCandidateRank);
    sb_appendf(sb, ",\"ghostLayer\":%s", proof->bGhostLayer ? "true" : "false");
    sb_appendf(sb, ",\"stale\":%s", proof->bStale ? "true" : "false");
    sb_append(sb, "}]", -1);

    if (missionId && missionId[0]) {
        sb_append(sb, ",\"missionId\":", -1);
        json_escape_append(sb, missionId);
    }
    if (missionPhase && missionPhase[0]) {
        sb_append(sb, ",\"missionPhase\":", -1);
        json_escape_append(sb, missionPhase);
    }
    if (missionSummary && missionSummary[0]) {
        sb_append(sb, ",\"missionSummary\":", -1);
        json_escape_append(sb, missionSummary);
    }
    if (missionQueue && missionQueue[0]) {
        sb_append(sb, ",\"missionQueue\":", -1);
        json_escape_append(sb, missionQueue);
    }
    if (atlas && atlas->pszSummary && atlas->pszSummary[0]) {
        sb_append(sb, ",\"atlasSummary\":", -1);
        json_escape_append(sb, atlas->pszSummary);
    }
    sb_append(sb, ",\"proofSummary\":", -1);
    json_escape_append(sb, proof->szSummary);
    if (proof->szBuildCommand[0]) {
        sb_append(sb, ",\"buildCommand\":", -1);
        json_escape_append(sb, proof->szBuildCommand);
    }
    if (proof->szTestCommand[0]) {
        sb_append(sb, ",\"testCommand\":", -1);
        json_escape_append(sb, proof->szTestCommand);
    }
    sb_append(sb, ",\"scratchpadSummary\":", -1);
    json_escape_append(sb, "Scout summary compacted; worker diff generated; verifier and adversary notes attached.");

    sb_append(sb, ",\"meta\":{\"model\":", -1);
    json_escape_append(sb, model ? model : "");
    sb_append(sb, "}", 1);
    sb_append(sb, "}", 1);
}

static char* ProcessRequest(const char* jsonReq)
{
    PipeRequest req;
    ParseRequest(jsonReq, &req);

    // Handle ping
    if (req.pszType && strcmp(req.pszType, "ping") == 0)
    {
        const AIProviderDef* pDef = AIProvider_Get(s_state.providerCfg.eProvider);
        char pong[512];
        snprintf(pong, sizeof(pong),
                 "{\"type\":\"pong\",\"status\":\"ok\",\"version\":\"%s\","
                 "\"provider\":\"%s\",\"model\":\"%s\"}",
                 ENGINE_VERSION,
                 pDef ? pDef->szName : "unknown",
                 s_state.providerCfg.szModel);
        FreeRequest(&req);
        return _strdup(pong);
    }

    // Handle shutdown
    if (req.pszType && strcmp(req.pszType, "shutdown") == 0)
    {
        FreeRequest(&req);
        return NULL;
    }

    // Handle provider config update (from editor settings dialog)
    if (req.pszType && strcmp(req.pszType, "config") == 0)
    {
        AIProviderConfig_FromJSON(jsonReq, &s_state.providerCfg);
        const AIProviderDef* pDef = AIProvider_Get(s_state.providerCfg.eProvider);
        char resp[512];
        snprintf(resp, sizeof(resp),
                 "{\"type\":\"config_ack\",\"status\":\"ok\","
                 "\"provider\":\"%s\",\"model\":\"%s\"}",
                 pDef ? pDef->szName : "unknown",
                 s_state.providerCfg.szModel);
        FreeRequest(&req);
        return _strdup(resp);
    }

    // Handle list-providers query
    if (req.pszType && strcmp(req.pszType, "list_providers") == 0)
    {
        StrBuf sb;
        sb_init(&sb, 4096);
        sb_append(&sb, "{\"type\":\"providers\",\"status\":\"ok\",\"providers\":[", -1);

        for (int i = 0; i < AI_PROVIDER_COUNT; i++)
        {
            const AIProviderDef* p = AIProvider_Get((EAIProvider)i);
            if (!p) continue;
            if (i > 0) sb_append(&sb, ",", 1);
            sb_append(&sb, "{", 1);
            sb_appendf(&sb, "\"id\":%d,", i);
            sb_append(&sb, "\"name\":", -1);
            json_escape_append(&sb, p->szName);
            sb_append(&sb, ",\"slug\":", -1);
            json_escape_append(&sb, p->szSlug);
            sb_appendf(&sb, ",\"local\":%s", p->bIsLocal ? "true" : "false");
            sb_appendf(&sb, ",\"requiresKey\":%s", p->bRequiresKey ? "true" : "false");
            sb_append(&sb, ",\"models\":", -1);
            json_escape_append(&sb, p->szModels ? p->szModels : "");
            sb_append(&sb, "}", 1);
        }

        sb_append(&sb, "]}", -1);

        FreeRequest(&req);
        char* result = sb.data;
        sb.data = NULL;
        return result;
    }

    // Handle web search request
    if (req.pszType && strcmp(req.pszType, "websearch") == 0)
    {
        char* query = req.pszChatMessage ? req.pszChatMessage : req.pszInstruction;
        if (!query || !query[0])
        {
            FreeRequest(&req);
            return _strdup("{\"type\":\"websearch_result\",\"success\":false,"
                           "\"error\":\"No search query provided\",\"results\":[]}");
        }

        // Call web search (default 5 results)
        char* searchResult = WebSearch(query, 5);

        // Wrap in response format
        StrBuf resp;
        sb_init(&resp, 4096);
        sb_append(&resp, "{\"type\":\"websearch_result\",\"query\":", -1);
        json_escape_append(&resp, query);
        sb_append(&resp, ",", 1);

        // Append the search result JSON (strip outer braces to merge)
        if (searchResult && searchResult[0] == '{')
        {
            // Include the results directly
            sb_append(&resp, searchResult + 1, strlen(searchResult) - 2);
        }
        else
        {
            sb_append(&resp, "\"success\":false,\"error\":\"Search failed\"", -1);
        }
        sb_append(&resp, "}", 1);

        if (searchResult) free(searchResult);
        FreeRequest(&req);
        char* result = resp.data;
        resp.data = NULL;
        return result;
    }

    // Resolve provider config for this request
    AIProviderConfig reqCfg;
    ResolveRequestConfig(&req, &reqCfg);

    IntentSpec intent;
    CompileIntentSpec(&req, &intent);

    AtlasLite atlas;
    BuildAtlasLite(&req, &atlas);

    char missionId[64];
    char missionPhase[32] = "planning";
    char missionSummary[256];
    BuildMissionId(req.pszId, missionId, sizeof(missionId));
    _snprintf_s(missionSummary, sizeof(missionSummary), _TRUNCATE,
                "%s", intent.szGoal);

    if (!LeaseScheduler_Acquire(missionId, req.pszFilePath ? req.pszFilePath : req.pszProjectRoot))
    {
        StrBuf deny;
        sb_init(&deny, 512);
        sb_append(&deny, "{\"id\":", -1);
        json_escape_append(&deny, req.pszId ? req.pszId : "");
        sb_append(&deny, ",\"status\":\"error\",\"error\":{\"code\":\"lease_conflict\",\"message\":\"Another mission already owns this scope.\"}}", -1);
        FreeAtlasLite(&atlas);
        FreeRequest(&req);
        return deny.data;
    }

    MissionQueue_Push(missionId, missionPhase, missionSummary, intent.szRisk);

    // Build prompt
    StrBuf sbSystem, sbUser;
    sb_init(&sbSystem, 1024);
    sb_init(&sbUser, 8192);

    BuildPrompt(&req, &intent, &atlas, missionId, &sbSystem, &sbUser);

    if (s_state.bVerbose)
    {
        const AIProviderDef* pDef = AIProvider_Get(reqCfg.eProvider);
        fprintf(stderr, "biko-engine: calling %s/%s for %s request\n",
                pDef ? pDef->szName : "?",
                reqCfg.szModel,
                req.pszType ? req.pszType : "?");
    }

    // Call LLM with retry
    char* llmResponse = NULL;
    int retries = reqCfg.iMaxRetries > 0 ? reqCfg.iMaxRetries : 0;

    for (int attempt = 0; attempt <= retries; attempt++)
    {
        if (attempt > 0)
        {
            if (s_state.bVerbose)
                fprintf(stderr, "biko-engine: retry %d/%d\n", attempt, retries);
            Sleep(1000 * attempt);
        }

        llmResponse = CallLLMAPI(&reqCfg, sbSystem.data, sbUser.data);

        // Don't retry on successful response
        if (llmResponse && strncmp(llmResponse, "Error:", 6) != 0 &&
            strncmp(llmResponse, "API Error:", 10) != 0)
            break;

        if (attempt < retries && llmResponse)
        {
            free(llmResponse);
            llmResponse = NULL;
        }
    }

    sb_free(&sbSystem);
    sb_free(&sbUser);

    PatchProof proof;
    ZeroMemory(&proof, sizeof(proof));
    FillProofFromHeuristics(&req, &atlas, &intent, llmResponse, &proof);

    if (req.pszType && strcmp(req.pszType, "chat") == 0)
        _snprintf_s(missionPhase, sizeof(missionPhase), _TRUNCATE, "reply_ready");
    else if (req.pszType && strcmp(req.pszType, "explain") == 0)
        _snprintf_s(missionPhase, sizeof(missionPhase), _TRUNCATE, "explain_ready");
    else
        _snprintf_s(missionPhase, sizeof(missionPhase), _TRUNCATE, "proof_ready");

    _snprintf_s(missionSummary, sizeof(missionSummary), _TRUNCATE,
                "%s [%s]", intent.szGoal, missionPhase);
    MissionQueue_Push(missionId, missionPhase, missionSummary, intent.szRisk);

    StrBuf missionQueue;
    sb_init(&missionQueue, 512);
    MissionQueue_Format(&missionQueue);

    // Build pipe response
    StrBuf sbResp;
    sb_init(&sbResp, 4096);

    if (llmResponse && (strncmp(llmResponse, "Error:", 6) == 0 || strncmp(llmResponse, "API Error:", 10) == 0))
    {
        sb_append(&sbResp, "{\"id\":", -1);
        json_escape_append(&sbResp, req.pszId ? req.pszId : "");
        sb_append(&sbResp, ",\"status\":\"error\",\"error\":{\"code\":\"model_error\",\"message\":", -1);
        json_escape_append(&sbResp, llmResponse);
        sb_append(&sbResp, "}}", -1);
    }
    else if (req.pszType && strcmp(req.pszType, "chat") == 0)
        BuildTextResponse(&sbResp, req.pszId, "chat", llmResponse, reqCfg.szModel,
                          missionId, missionPhase, missionSummary, missionQueue.data, &atlas);
    else if (req.pszType && strcmp(req.pszType, "explain") == 0)
        BuildTextResponse(&sbResp, req.pszId, "explain", llmResponse, reqCfg.szModel,
                          missionId, missionPhase, missionSummary, missionQueue.data, &atlas);
    else
        BuildPatchResponse(&sbResp, &req, missionId, missionPhase, missionSummary,
                           missionQueue.data, &atlas, &proof, llmResponse, reqCfg.szModel);

    if (llmResponse) free(llmResponse);
    sb_free(&missionQueue);
    FreeAtlasLite(&atlas);
    LeaseScheduler_Release(missionId);
    FreeRequest(&req);

    char* result = sbResp.data;
    sbResp.data = NULL;
    return result;
}

//=============================================================================
// Named pipe server
//=============================================================================

static BOOL ReadFramedMessage(HANDLE hPipe, char** ppMessage)
{
    DWORD len = 0;
    DWORD dwRead;

    if (!ReadFile(hPipe, &len, 4, &dwRead, NULL) || dwRead != 4)
        return FALSE;

    if (len == 0 || len > MAX_REQUEST_SIZE)
        return FALSE;

    char* msg = (char*)malloc(len + 1);
    if (!msg) return FALSE;

    DWORD totalRead = 0;
    while (totalRead < len)
    {
        if (!ReadFile(hPipe, msg + totalRead, len - totalRead, &dwRead, NULL) || dwRead == 0)
        {
            free(msg);
            return FALSE;
        }
        totalRead += dwRead;
    }
    msg[len] = '\0';

    *ppMessage = msg;
    return TRUE;
}

static BOOL WriteFramedMessage(HANDLE hPipe, const char* pszMessage)
{
    DWORD len = (DWORD)strlen(pszMessage);
    DWORD dwWritten;

    if (!WriteFile(hPipe, &len, 4, &dwWritten, NULL) || dwWritten != 4)
        return FALSE;

    DWORD totalWritten = 0;
    while (totalWritten < len)
    {
        if (!WriteFile(hPipe, pszMessage + totalWritten,
                       len - totalWritten, &dwWritten, NULL) || dwWritten == 0)
            return FALSE;
        totalWritten += dwWritten;
    }

    FlushFileBuffers(hPipe);
    return TRUE;
}

static int RunPipeServer(const char* pipeName)
{
    WCHAR wszPipeName[256];
    MultiByteToWideChar(CP_UTF8, 0, pipeName, -1, wszPipeName, 256);

    HANDLE hPipe = CreateNamedPipeW(
        wszPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        PIPE_BUFFER_SIZE,
        PIPE_BUFFER_SIZE,
        0,
        NULL);

    if (hPipe == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "Error: CreateNamedPipe failed (%lu)\n", GetLastError());
        return 1;
    }

    if (s_state.bVerbose)
    {
        const AIProviderDef* pDef = AIProvider_Get(s_state.providerCfg.eProvider);
        fprintf(stderr, "biko-engine %s: listening on %s\n", ENGINE_VERSION, pipeName);
        fprintf(stderr, "  provider: %s\n", pDef ? pDef->szName : "unknown");
        fprintf(stderr, "  model:    %s\n", s_state.providerCfg.szModel);
        fprintf(stderr, "  has key:  %s\n", s_state.providerCfg.szApiKey[0] ? "yes" : "no");
    }

    if (!ConnectNamedPipe(hPipe, NULL))
    {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED)
        {
            fprintf(stderr, "Error: ConnectNamedPipe failed (%lu)\n", err);
            CloseHandle(hPipe);
            return 1;
        }
    }

    if (s_state.bVerbose)
        fprintf(stderr, "biko-engine: client connected\n");

    BOOL bRunning = TRUE;
    while (bRunning)
    {
        char* pszRequest = NULL;
        if (!ReadFramedMessage(hPipe, &pszRequest))
        {
            if (s_state.bVerbose)
                fprintf(stderr, "biko-engine: pipe read failed, shutting down\n");
            break;
        }

        if (s_state.bVerbose)
            fprintf(stderr, "biko-engine: received request (%d bytes)\n",
                    (int)strlen(pszRequest));

        char* pszResponse = ProcessRequest(pszRequest);
        free(pszRequest);

        if (!pszResponse)
        {
            bRunning = FALSE;
            continue;
        }

        if (!WriteFramedMessage(hPipe, pszResponse))
        {
            fprintf(stderr, "biko-engine: pipe write failed\n");
            free(pszResponse);
            break;
        }

        free(pszResponse);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);

    if (s_state.bVerbose)
        fprintf(stderr, "biko-engine: shutdown complete\n");

    return 0;
}

//=============================================================================
// Load configuration
//=============================================================================

static void LoadConfig(int argc, char* argv[])
{
    memset(&s_state, 0, sizeof(s_state));

    // Default provider from BIKO_AI_PROVIDER env var, or "openai"
    char providerSlug[64] = "openai";
    GetEnvironmentVariableA("BIKO_AI_PROVIDER", providerSlug, sizeof(providerSlug));

    const AIProviderDef* pDef = AIProvider_FindBySlug(providerSlug);
    EAIProvider eProvider = pDef ? pDef->id : AI_PROVIDER_OPENAI;

    // Init defaults for chosen provider
    AIProviderConfig_InitDefaults(&s_state.providerCfg, eProvider);

    // Override API key from generic env vars
    if (!s_state.providerCfg.szApiKey[0])
    {
        char keyBuf[512];
        DWORD len = GetEnvironmentVariableA("biko_ai_KEY", keyBuf, sizeof(keyBuf));
        if (len > 0 && len < sizeof(keyBuf))
            strncpy(s_state.providerCfg.szApiKey, keyBuf,
                    sizeof(s_state.providerCfg.szApiKey) - 1);
    }

    // Model override
    char buf[256];
    if (GetEnvironmentVariableA("biko_ai_MODEL", buf, sizeof(buf)) > 0)
        strncpy(s_state.providerCfg.szModel, buf,
                sizeof(s_state.providerCfg.szModel) - 1);

    // Host override
    if (GetEnvironmentVariableA("biko_ai_HOST", buf, sizeof(buf)) > 0)
        strncpy(s_state.providerCfg.szHost, buf,
                sizeof(s_state.providerCfg.szHost) - 1);

    // Path override
    if (GetEnvironmentVariableA("biko_ai_PATH", buf, sizeof(buf)) > 0)
        strncpy(s_state.providerCfg.szPath, buf,
                sizeof(s_state.providerCfg.szPath) - 1);

    // Port override
    if (GetEnvironmentVariableA("biko_ai_PORT", buf, sizeof(buf)) > 0)
        s_state.providerCfg.iPort = atoi(buf);

    // Verbose
    if (GetEnvironmentVariableA("MONO_VERBOSE", buf, sizeof(buf)) > 0)
        s_state.bVerbose = (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');

    // Parse command line arguments (override env vars)
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
        {
            s_state.bVerbose = TRUE;
        }
        else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
        {
            const char* slug = argv[++i];
            const AIProviderDef* p = AIProvider_FindBySlug(slug);
            if (p)
            {
                char savedKey[512];
                strncpy(savedKey, s_state.providerCfg.szApiKey, sizeof(savedKey) - 1);
                savedKey[sizeof(savedKey) - 1] = '\0';

                AIProviderConfig_InitDefaults(&s_state.providerCfg, p->id);

                if (!s_state.providerCfg.szApiKey[0] && savedKey[0])
                    strncpy(s_state.providerCfg.szApiKey, savedKey,
                            sizeof(s_state.providerCfg.szApiKey) - 1);
            }
            else
            {
                fprintf(stderr, "Warning: unknown provider '%s', using openai\n", slug);
            }
        }
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
        {
            strncpy(s_state.providerCfg.szModel, argv[++i],
                    sizeof(s_state.providerCfg.szModel) - 1);
        }
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
        {
            strncpy(s_state.providerCfg.szApiKey, argv[++i],
                    sizeof(s_state.providerCfg.szApiKey) - 1);
        }
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
        {
            strncpy(s_state.providerCfg.szHost, argv[++i],
                    sizeof(s_state.providerCfg.szHost) - 1);
        }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
        {
            s_state.providerCfg.iPort = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--temperature") == 0 && i + 1 < argc)
        {
            s_state.providerCfg.dTemperature = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc)
        {
            s_state.providerCfg.iMaxTokens = atoi(argv[++i]);
        }
    }
}

//=============================================================================
// Entry point
//=============================================================================

int main(int argc, char* argv[])
{
    LoadConfig(argc, argv);

    const char* pipeName = NULL;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc)
        {
            pipeName = argv[++i];
        }
        else if (strcmp(argv[i], "--version") == 0)
        {
            printf("biko-engine %s (multi-provider)\n", ENGINE_VERSION);
            return 0;
        }
        else if (strcmp(argv[i], "--list-providers") == 0)
        {
            printf("Available providers:\n\n");
            printf("  %-16s %-30s %-8s %s\n", "SLUG", "NAME", "LOCAL", "DEFAULT MODEL");
            printf("  %-16s %-30s %-8s %s\n", "----", "----", "-----", "-------------");
            for (int j = 0; j < AI_PROVIDER_COUNT; j++)
            {
                const AIProviderDef* p = AIProvider_Get((EAIProvider)j);
                if (p)
                {
                    printf("  %-16s %-30s %-8s %s\n",
                           p->szSlug, p->szName,
                           p->bIsLocal ? "yes" : "no",
                           p->szDefaultModel);
                }
            }
            printf("\nUse --provider <slug> to select a provider.\n");
            return 0;
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            printf("biko-engine %s (multi-provider)\n\n", ENGINE_VERSION);
            printf("Usage: biko-engine --pipe <pipe_name> [options]\n\n");
            printf("Provider options:\n");
            printf("  --provider <slug>   AI provider (default: openai)\n");
            printf("  --model <name>      Model name\n");
            printf("  --key <key>         API key (or set provider env var)\n");
            printf("  --host <host>       Override API host\n");
            printf("  --port <port>       Override API port\n");
            printf("  --temperature <t>   Temperature (default: 0.2)\n");
            printf("  --max-tokens <n>    Max output tokens (default: 4096)\n");
            printf("  --list-providers    Show all supported providers\n\n");
            printf("General options:\n");
            printf("  --pipe <name>       Named pipe path (required)\n");
            printf("  --verbose, -v       Enable verbose logging to stderr\n");
            printf("  --version           Print version and exit\n");
            printf("  --help              Show this help\n\n");
            printf("Environment variables:\n");
            printf("  biko_ai_PROVIDER    Provider slug (default: openai)\n");
            printf("  biko_ai_KEY         Generic API key fallback\n");
            printf("  biko_ai_MODEL       Model name override\n");
            printf("  biko_ai_HOST        API host override\n");
            printf("  biko_ai_PATH        API path override\n");
            printf("  biko_ai_PORT        API port override\n");
            printf("  MONO_VERBOSE        Enable verbose mode (1/y)\n\n");
            printf("Provider-specific env vars:\n");
            for (int j = 0; j < AI_PROVIDER_COUNT; j++)
            {
                const AIProviderDef* p = AIProvider_Get((EAIProvider)j);
                if (p && p->szEnvVarKey)
                    printf("  %-22s  %s\n", p->szEnvVarKey, p->szName);
            }
            printf("\n");
            return 0;
        }
    }

    if (!pipeName)
    {
        fprintf(stderr, "Error: --pipe argument is required\n");
        fprintf(stderr, "Run 'biko-engine --help' for usage\n");
        return 1;
    }

    return RunPipeServer(pipeName);
}
