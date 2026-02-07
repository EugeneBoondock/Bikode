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
    char*   pszFilePath;
    char*   pszFileContent;
    char*   pszSelection;
    char*   pszInstruction;
    char*   pszChatMessage;
    char*   pszLanguage;
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
    if (req->pszFilePath) free(req->pszFilePath);
    if (req->pszFileContent) free(req->pszFileContent);
    if (req->pszSelection) free(req->pszSelection);
    if (req->pszInstruction) free(req->pszInstruction);
    if (req->pszChatMessage) free(req->pszChatMessage);
    if (req->pszLanguage) free(req->pszLanguage);
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
    req->pszFilePath = json_extract_string(json, "filePath");
    req->pszFileContent = json_extract_string(json, "fileContent");
    req->pszSelection = json_extract_string(json, "selection");
    req->pszInstruction = json_extract_string(json, "instruction");
    req->pszChatMessage = json_extract_string(json, "chatMessage");
    req->pszLanguage = json_extract_string(json, "language");

    // Provider overrides (embedded in pipe request by editor)
    req->pszProvider = json_extract_string(json, "provider");
    req->pszModel = json_extract_string(json, "providerModel");
    req->pszApiKey = json_extract_string(json, "providerKey");
}

//=============================================================================
// Prompt construction
//=============================================================================

static const char* SYSTEM_PROMPT_PATCH =
    "You are a code editing assistant for Biko, a lightweight AI IDE. "
    "When asked to modify code, respond with ONLY a unified diff. "
    "Use standard unified diff format with --- and +++ headers and @@ hunk markers. "
    "Do not include explanations outside the diff. "
    "The diff should be directly applicable to the given file content.";

static const char* SYSTEM_PROMPT_EXPLAIN =
    "You are a code explanation assistant. "
    "Provide a clear, concise explanation of the given code. "
    "Focus on what the code does, any notable patterns, and potential issues.";

static const char* SYSTEM_PROMPT_CHAT =
    "You are an AI coding assistant embedded in Biko, a lightweight AI IDE. "
    "You help with programming questions, code review, debugging, and general development tasks. "
    "Be concise and practical. When suggesting code changes, provide them as diffs when appropriate.";

static void BuildPrompt(PipeRequest* req, StrBuf* sbSystem, StrBuf* sbUser)
{
    if (req->pszType && strcmp(req->pszType, "chat") == 0)
        sb_append(sbSystem, SYSTEM_PROMPT_CHAT, -1);
    else if (req->pszType && strcmp(req->pszType, "explain") == 0)
        sb_append(sbSystem, SYSTEM_PROMPT_EXPLAIN, -1);
    else
        sb_append(sbSystem, SYSTEM_PROMPT_PATCH, -1);

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
static char* ParseResponse_OpenAI(const char* respJson)
{
    char* content = json_extract_nested_content(respJson, "choices", "content");
    if (content && content[0]) return content;
    if (content) free(content);

    content = json_extract_string(respJson, "content");
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

static void BuildResponse(StrBuf* sb, const char* id, const char* type,
                          const char* content, const char* diff,
                          const char* model)
{
    sb_append(sb, "{", 1);

    sb_append(sb, "\"id\":", -1);
    json_escape_append(sb, id ? id : "");

    sb_append(sb, ",\"type\":", -1);
    json_escape_append(sb, type ? type : "response");

    sb_append(sb, ",\"status\":\"ok\"", -1);

    if (diff && diff[0])
    {
        sb_append(sb, ",\"diff\":", -1);
        json_escape_append(sb, diff);
    }

    if (content && content[0])
    {
        if (type && strcmp(type, "chat") == 0)
        {
            sb_append(sb, ",\"chatResponse\":", -1);
            json_escape_append(sb, content);
        }
        else if (type && strcmp(type, "explain") == 0)
        {
            sb_append(sb, ",\"explanation\":", -1);
            json_escape_append(sb, content);
        }
        else
        {
            sb_append(sb, ",\"diff\":", -1);
            json_escape_append(sb, content);
        }
    }

    if (model && model[0])
    {
        sb_append(sb, ",\"meta\":{\"model\":", -1);
        json_escape_append(sb, model);
        sb_append(sb, "}", 1);
    }

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

    // Resolve provider config for this request
    AIProviderConfig reqCfg;
    ResolveRequestConfig(&req, &reqCfg);

    // Build prompt
    StrBuf sbSystem, sbUser;
    sb_init(&sbSystem, 1024);
    sb_init(&sbUser, 8192);

    BuildPrompt(&req, &sbSystem, &sbUser);

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

    // Build pipe response
    StrBuf sbResp;
    sb_init(&sbResp, 4096);

    if (req.pszType && strcmp(req.pszType, "chat") == 0)
        BuildResponse(&sbResp, req.pszId, "chat", llmResponse, NULL, reqCfg.szModel);
    else if (req.pszType && strcmp(req.pszType, "explain") == 0)
        BuildResponse(&sbResp, req.pszId, "explain", llmResponse, NULL, reqCfg.szModel);
    else
        BuildResponse(&sbResp, req.pszId, "patch", NULL, llmResponse, reqCfg.szModel);

    if (llmResponse) free(llmResponse);
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
