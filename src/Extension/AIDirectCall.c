/******************************************************************************
*
* Biko
*
* AIDirectCall.c
*   In-process WinHTTP calls to LLM APIs.
*   Extracted and adapted from biko-engine.c for direct in-process use.
*   Runs HTTP requests on a background thread to keep the UI responsive.
*
******************************************************************************/

#include "AIDirectCall.h"
#include "AIProvider.h"
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#pragma comment(lib, "winhttp.lib")

//=============================================================================
// Growable string buffer
//=============================================================================

typedef struct {
    char*   data;
    int     len;
    int     cap;
} StrBuf;

static void sb_init(StrBuf* sb, int initialCap)
{
    sb->cap = initialCap > 64 ? initialCap : 64;
    sb->data = (char*)malloc(sb->cap);
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void sb_append(StrBuf* sb, const char* s, int slen)
{
    if (!sb->data || !s) return;
    if (slen < 0) slen = (int)strlen(s);
    if (sb->len + slen + 1 > sb->cap)
    {
        int newCap = sb->cap * 2;
        if (newCap < sb->len + slen + 1)
            newCap = sb->len + slen + 256;
        char* tmp = (char*)realloc(sb->data, newCap);
        if (!tmp) return;
        sb->data = tmp;
        sb->cap = newCap;
    }
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(StrBuf* sb, const char* fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) sb_append(sb, tmp, n);
}

static void sb_free(StrBuf* sb)
{
    if (sb->data) { free(sb->data); sb->data = NULL; }
    sb->len = sb->cap = 0;
}

//=============================================================================
// JSON helpers
//=============================================================================

static void json_escape_append(StrBuf* sb, const char* s)
{
    if (!s) { sb_append(sb, "\"\"", 2); return; }
    sb_append(sb, "\"", 1);
    while (*s)
    {
        switch (*s)
        {
        case '"':  sb_append(sb, "\\\"", 2); break;
        case '\\': sb_append(sb, "\\\\", 2); break;
        case '\n': sb_append(sb, "\\n", 2);  break;
        case '\r': sb_append(sb, "\\r", 2);  break;
        case '\t': sb_append(sb, "\\t", 2);  break;
        default:
            if ((unsigned char)*s < 0x20)
            {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*s);
                sb_append(sb, esc, 6);
            }
            else
            {
                sb_append(sb, s, 1);
            }
            break;
        }
        s++;
    }
    sb_append(sb, "\"", 1);
}

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
            default:   sb_append(&sb, val, 1);  break;
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

static char* json_extract_nested_content(const char* json, const char* key1,
                                          const char* key2)
{
    const char* p = json_find_value(json, key1);
    if (!p) return NULL;
    return json_extract_string(p, key2);
}

//=============================================================================
// Request body builders (per format)
//=============================================================================

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
    sb_append(body, "{\"role\":\"system\",\"content\":", -1);
    json_escape_append(body, systemPrompt);
    sb_append(body, "},", -1);
    sb_append(body, "{\"role\":\"user\",\"content\":", -1);
    json_escape_append(body, userMessage);
    sb_append(body, "}", -1);
    sb_append(body, "]}", -1);
}

static void BuildBody_Anthropic(StrBuf* body, const AIProviderConfig* cfg,
                                 const char* systemPrompt, const char* userMessage)
{
    const char* model = cfg->szModel[0] ? cfg->szModel : "claude-sonnet-4-20250514";
    sb_append(body, "{", 1);
    sb_appendf(body, "\"model\":\"%s\",", model);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, "\"max_tokens\":%d,", cfg->iMaxTokens);
    else
        sb_append(body, "\"max_tokens\":4096,", -1);
    sb_appendf(body, "\"temperature\":%.2f,", cfg->dTemperature);
    sb_append(body, "\"system\":", -1);
    json_escape_append(body, systemPrompt);
    sb_append(body, ",", 1);
    sb_append(body, "\"messages\":[", -1);
    sb_append(body, "{\"role\":\"user\",\"content\":", -1);
    json_escape_append(body, userMessage);
    sb_append(body, "}", -1);
    sb_append(body, "]}", -1);
}

static void BuildBody_Google(StrBuf* body, const AIProviderConfig* cfg,
                              const char* systemPrompt, const char* userMessage)
{
    sb_append(body, "{", 1);
    sb_append(body, "\"systemInstruction\":{\"parts\":[{\"text\":", -1);
    json_escape_append(body, systemPrompt);
    sb_append(body, "}]},", -1);
    sb_append(body, "\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":", -1);
    json_escape_append(body, userMessage);
    sb_append(body, "}]}],", -1);
    sb_appendf(body, "\"generationConfig\":{\"temperature\":%.2f", cfg->dTemperature);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, ",\"maxOutputTokens\":%d", cfg->iMaxTokens);
    sb_append(body, "}}", -1);
}

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

static void BuildRequestBody(StrBuf* body, const AIProviderConfig* cfg,
                              const AIProviderDef* pDef,
                              const char* systemPrompt, const char* userMessage)
{
    EAIRequestFormat fmt = pDef ? pDef->eFormat : AI_FORMAT_OPENAI;
    switch (fmt)
    {
    case AI_FORMAT_ANTHROPIC: BuildBody_Anthropic(body, cfg, systemPrompt, userMessage); break;
    case AI_FORMAT_GOOGLE:    BuildBody_Google(body, cfg, systemPrompt, userMessage);    break;
    case AI_FORMAT_COHERE:    BuildBody_Cohere(body, cfg, systemPrompt, userMessage);    break;
    case AI_FORMAT_OPENAI:
    default:                  BuildBody_OpenAI(body, cfg, systemPrompt, userMessage);    break;
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
        break; // handled in URL path

    case AI_AUTH_NONE:
    default:
        break;
    }

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
// Response parsing (per format)
//=============================================================================

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

static char* ParseLLMResponse(const char* respJson, EAIRequestFormat fmt)
{
    char* result = NULL;
    switch (fmt)
    {
    case AI_FORMAT_ANTHROPIC: result = ParseResponse_Anthropic(respJson); break;
    case AI_FORMAT_GOOGLE:    result = ParseResponse_Google(respJson);    break;
    case AI_FORMAT_COHERE:    result = ParseResponse_Cohere(respJson);    break;
    case AI_FORMAT_OPENAI:
    default:                  result = ParseResponse_OpenAI(respJson);    break;
    }

    if (result) return result;

    // Check for error in response
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
// Shared WinHTTP call (takes pre-built body, returns parsed response)
//=============================================================================

#define MAX_RESPONSE_SIZE (4 * 1024 * 1024)

static char* SendHTTPRequest(const AIProviderConfig* pCfg, const char* body, int bodyLen)
{
    const AIProviderDef* pDef = AIProvider_Get(pCfg->eProvider);
    if (!pDef)
        return _strdup("Error: Unknown provider");

    if (pDef->bRequiresKey && !pCfg->szApiKey[0])
    {
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf),
                 "Error: No API key configured for %s",
                 pDef->szName);
        return _strdup(errBuf);
    }

    const char *szHost, *szPath;
    int iPort, bSSL;
    AIProviderConfig_Resolve(pCfg, &szHost, &szPath, &iPort, &bSSL);

    WCHAR wszHost[512];
    MultiByteToWideChar(CP_UTF8, 0, szHost, -1, wszHost, 512);

    WCHAR wszPath[1024];
    if (pDef->eAuth == AI_AUTH_QUERY_PARAM && pCfg->szApiKey[0])
    {
        char fullPath[1024];
        snprintf(fullPath, sizeof(fullPath), "%s?key=%s", szPath, pCfg->szApiKey);
        MultiByteToWideChar(CP_UTF8, 0, fullPath, -1, wszPath, 1024);
    }
    else
    {
        MultiByteToWideChar(CP_UTF8, 0, szPath, -1, wszPath, 1024);
    }

    HINTERNET hSession = WinHttpOpen(L"Biko/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return _strdup("Error: WinHttpOpen failed");

    int timeoutMs = pCfg->iTimeoutSec > 0 ? pCfg->iTimeoutSec * 1000 : 120000;
    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, wszHost, (INTERNET_PORT)iPort, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return _strdup("Error: WinHttpConnect failed -- check host/port");
    }

    DWORD dwFlags = bSSL ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wszPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return _strdup("Error: WinHttpOpenRequest failed");
    }

    if (!bSSL)
    {
        DWORD dwSecFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                         &dwSecFlags, sizeof(dwSecFlags));
    }

    BuildAuthHeaders(hRequest, pCfg, pDef);

    BOOL bSent = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body, bodyLen, bodyLen, 0);
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

    DWORD dwStatusCode = 0;
    DWORD dwSize = sizeof(dwStatusCode);
    WinHttpQueryHeaders(hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        NULL, &dwStatusCode, &dwSize, NULL);

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

    char* content = ParseLLMResponse(resp.data, pDef->eFormat);
    sb_free(&resp);

    return content ? content : _strdup("Error: Empty response from API");
}

//=============================================================================
// Single-message call (delegates to SendHTTPRequest)
//=============================================================================

char* AIDirectCall_Chat(const AIProviderConfig* pCfg,
                        const char* szSystemPrompt,
                        const char* szUserMessage)
{
    AIChatMessage msgs[2];
    msgs[0].role = "system";
    msgs[0].content = szSystemPrompt;
    msgs[1].role = "user";
    msgs[1].content = szUserMessage;
    return AIDirectCall_ChatMulti(pCfg, msgs, 2);
}

//=============================================================================
// Multi-message body builders
//=============================================================================

static void BuildBodyMulti_OpenAI(StrBuf* body, const AIProviderConfig* cfg,
                                   const AIChatMessage* msgs, int count)
{
    const char* model = cfg->szModel[0] ? cfg->szModel : "gpt-4o-mini";
    sb_append(body, "{", 1);
    sb_appendf(body, "\"model\":\"%s\",", model);
    sb_appendf(body, "\"temperature\":%.2f,", cfg->dTemperature);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, "\"max_tokens\":%d,", cfg->iMaxTokens);
    sb_append(body, "\"messages\":[", -1);
    for (int i = 0; i < count; i++)
    {
        if (i > 0) sb_append(body, ",", 1);
        sb_appendf(body, "{\"role\":\"%s\",\"content\":", msgs[i].role);
        json_escape_append(body, msgs[i].content);
        sb_append(body, "}", 1);
    }
    sb_append(body, "]}", -1);
}

static void BuildBodyMulti_Anthropic(StrBuf* body, const AIProviderConfig* cfg,
                                      const AIChatMessage* msgs, int count)
{
    const char* model = cfg->szModel[0] ? cfg->szModel : "claude-sonnet-4-20250514";
    sb_append(body, "{", 1);
    sb_appendf(body, "\"model\":\"%s\",", model);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, "\"max_tokens\":%d,", cfg->iMaxTokens);
    else
        sb_append(body, "\"max_tokens\":4096,", -1);
    sb_appendf(body, "\"temperature\":%.2f,", cfg->dTemperature);

    // Extract system prompt (first system message)
    const char* sysPrompt = "";
    int firstNonSys = 0;
    for (int i = 0; i < count; i++)
    {
        if (strcmp(msgs[i].role, "system") == 0)
        {
            sysPrompt = msgs[i].content;
            firstNonSys = i + 1;
        }
        else break;
    }

    sb_append(body, "\"system\":", -1);
    json_escape_append(body, sysPrompt);
    sb_append(body, ",", 1);

    sb_append(body, "\"messages\":[", -1);
    int first = 1;
    for (int i = firstNonSys; i < count; i++)
    {
        if (strcmp(msgs[i].role, "system") == 0) continue;
        if (!first) sb_append(body, ",", 1);
        first = 0;
        sb_appendf(body, "{\"role\":\"%s\",\"content\":", msgs[i].role);
        json_escape_append(body, msgs[i].content);
        sb_append(body, "}", 1);
    }
    sb_append(body, "]}", -1);
}

static void BuildBodyMulti_Google(StrBuf* body, const AIProviderConfig* cfg,
                                   const AIChatMessage* msgs, int count)
{
    sb_append(body, "{", 1);

    // System instruction
    const char* sysPrompt = "";
    for (int i = 0; i < count; i++)
    {
        if (strcmp(msgs[i].role, "system") == 0)
            sysPrompt = msgs[i].content;
        else break;
    }
    sb_append(body, "\"systemInstruction\":{\"parts\":[{\"text\":", -1);
    json_escape_append(body, sysPrompt);
    sb_append(body, "}]},", -1);

    // Contents
    sb_append(body, "\"contents\":[", -1);
    int first = 1;
    for (int i = 0; i < count; i++)
    {
        if (strcmp(msgs[i].role, "system") == 0) continue;
        if (!first) sb_append(body, ",", 1);
        first = 0;
        const char* gRole = strcmp(msgs[i].role, "assistant") == 0 ? "model" : "user";
        sb_appendf(body, "{\"role\":\"%s\",\"parts\":[{\"text\":", gRole);
        json_escape_append(body, msgs[i].content);
        sb_append(body, "}]}", -1);
    }
    sb_append(body, "],", -1);

    sb_appendf(body, "\"generationConfig\":{\"temperature\":%.2f", cfg->dTemperature);
    if (cfg->iMaxTokens > 0)
        sb_appendf(body, ",\"maxOutputTokens\":%d", cfg->iMaxTokens);
    sb_append(body, "}}", -1);
}

static void BuildRequestBodyMulti(StrBuf* body, const AIProviderConfig* cfg,
                                   const AIProviderDef* pDef,
                                   const AIChatMessage* msgs, int count)
{
    EAIRequestFormat fmt = pDef ? pDef->eFormat : AI_FORMAT_OPENAI;
    switch (fmt)
    {
    case AI_FORMAT_ANTHROPIC: BuildBodyMulti_Anthropic(body, cfg, msgs, count); break;
    case AI_FORMAT_GOOGLE:    BuildBodyMulti_Google(body, cfg, msgs, count);    break;
    case AI_FORMAT_OPENAI:
    case AI_FORMAT_COHERE:
    default:                  BuildBodyMulti_OpenAI(body, cfg, msgs, count);    break;
    }
}

//=============================================================================
// Multi-message synchronous call
//=============================================================================

char* AIDirectCall_ChatMulti(const AIProviderConfig* pCfg,
                             const AIChatMessage* messages,
                             int messageCount)
{
    const AIProviderDef* pDef = AIProvider_Get(pCfg->eProvider);
    if (!pDef)
        return _strdup("Error: Unknown provider");

    StrBuf body;
    sb_init(&body, 4096);
    BuildRequestBodyMulti(&body, pCfg, pDef, messages, messageCount);

    char* result = SendHTTPRequest(pCfg, body.data, body.len);
    sb_free(&body);
    return result;
}

//=============================================================================
// Async wrapper (background thread)
//=============================================================================

typedef struct {
    AIProviderConfig cfg;
    char*   szSystemPrompt;
    char*   szUserMessage;
    HWND    hwndTarget;
} AsyncCallParams;

static unsigned __stdcall AsyncCallThreadProc(void* pArg)
{
    AsyncCallParams* p = (AsyncCallParams*)pArg;

    char* result = AIDirectCall_Chat(&p->cfg, p->szSystemPrompt, p->szUserMessage);

    // Post result back to UI thread
    // lParam is a heap-allocated string that the receiver must free()
    PostMessage(p->hwndTarget, WM_AI_DIRECT_RESPONSE, 0, (LPARAM)result);

    free(p->szSystemPrompt);
    free(p->szUserMessage);
    free(p);
    return 0;
}

BOOL AIDirectCall_ChatAsync(const AIProviderConfig* pCfg,
                            const char* szSystemPrompt,
                            const char* szUserMessage,
                            HWND hwndTarget)
{
    AsyncCallParams* p = (AsyncCallParams*)malloc(sizeof(AsyncCallParams));
    if (!p) return FALSE;

    memcpy(&p->cfg, pCfg, sizeof(AIProviderConfig));
    p->szSystemPrompt = _strdup(szSystemPrompt ? szSystemPrompt : "");
    p->szUserMessage = _strdup(szUserMessage ? szUserMessage : "");
    p->hwndTarget = hwndTarget;

    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, AsyncCallThreadProc, p, 0, NULL);
    if (!hThread)
    {
        free(p->szSystemPrompt);
        free(p->szUserMessage);
        free(p);
        return FALSE;
    }

    CloseHandle(hThread); // fire-and-forget
    return TRUE;
}
