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

#include <wincrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "Crypt32.lib")

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
// File helpers
//=============================================================================

static char* ReadFileContents(const char* path, DWORD* outSize)
{
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    DWORD size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(hFile); return NULL; }

    // Limit size to avoid exploding memory (e.g. 10MB limit for attachments)
    if (size > 10 * 1024 * 1024) size = 10 * 1024 * 1024;

    char* buf = (char*)malloc(size + 1);
    if (!buf) { CloseHandle(hFile); return NULL; }

    DWORD read;
    if (!ReadFile(hFile, buf, size, &read, NULL))
    {
        free(buf);
        CloseHandle(hFile);
        return NULL;
    }
    CloseHandle(hFile);
    buf[read] = '\0';
    if (outSize) *outSize = read;
    return buf;
}

static char* ReadFileToBase64(const char* path)
{
    DWORD size = 0;
    char* data = ReadFileContents(path, &size);
    if (!data) return NULL;

    DWORD strLen = 0;
    // CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF (0x40000000 | 0x01) -> 0x40000001
    // But older Headers might not define NOCRLF. Let's use standard BASE64 and strip CRLF ourselves if needed.
    // Actually CRYPT_STRING_BASE64 is usually fine, JSON tolerates whitespace.
    // But for data URIs we want one line.
    
    if (!CryptBinaryToStringA((const BYTE*)data, size, CRYPT_STRING_BASE64 | 0x40000000 /* CRYPT_STRING_NOCRLF */, NULL, &strLen))
    {
        // Try without NOCRLF if that failed (e.g. XP/Win7 support issues?)
        if (!CryptBinaryToStringA((const BYTE*)data, size, CRYPT_STRING_BASE64, NULL, &strLen))
        {
            free(data);
            return NULL;
        }
    }

    char* b64 = (char*)malloc(strLen + 1);
    if (b64)
    {
        CryptBinaryToStringA((const BYTE*)data, size, CRYPT_STRING_BASE64 | 0x40000000, b64, &strLen);
    }
    free(data);
    return b64;
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
    int needleLen = (int)strlen(needle);

    const char* p = json;
    while ((p = strstr(p, needle)) != NULL)
    {
        const char* afterKey = p + needleLen;
        // Skip whitespace
        while (*afterKey && (*afterKey == ' ' || *afterKey == '\t' ||
               *afterKey == '\n' || *afterKey == '\r')) afterKey++;
        // A JSON key MUST be followed by ':'
        if (*afterKey == ':')
        {
            afterKey++; // skip ':'
            while (*afterKey && (*afterKey == ' ' || *afterKey == '\t' ||
                   *afterKey == '\n' || *afterKey == '\r')) afterKey++;
            return afterKey;
        }
        // Not a key (it's a value) — keep searching
        p += needleLen;
    }
    return NULL;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int decode_unicode_escape(const char* p, char* out)
{
    // Parse 4 hex digits: \uXXXX
    int d0 = hex_digit(p[0]), d1 = hex_digit(p[1]);
    int d2 = hex_digit(p[2]), d3 = hex_digit(p[3]);
    if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0) return 0;
    unsigned int cp = (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
}

static char* json_extract_string(const char* json, const char* key)
{
    const char* val = json_find_value(json, key);
    if (!val) return NULL;

    // Handle null value: return NULL to signal "key found but no string"
    if (strncmp(val, "null", 4) == 0) return NULL;

    // Handle array value: search inside for a "text" or string element
    if (*val == '[')
    {
        // Try to find a string inside the array (first element if it's a string)
        const char* p = val + 1;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
        if (*p == '"')
        {
            // Array of strings - extract first element
            val = p;
            // fall through to string extraction below
        }
        else
        {
            return NULL; // array of non-strings, let caller handle
        }
    }

    if (*val != '"') return NULL;
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
            case '/':  sb_append(&sb, "/", 1); break;
            case 'n':  sb_append(&sb, "\n", 1); break;
            case 'r':  sb_append(&sb, "\r", 1); break;
            case 't':  sb_append(&sb, "\t", 1); break;
            case 'b':  sb_append(&sb, "\b", 1); break;
            case 'f':  sb_append(&sb, "\f", 1); break;
            case 'u': {
                if (val[1] && val[2] && val[3] && val[4]) {
                    char utf8[4];
                    int n = decode_unicode_escape(val + 1, utf8);
                    if (n > 0) {
                        sb_append(&sb, utf8, n);
                        val += 4; // skip the 4 hex digits
                    } else {
                        sb_append(&sb, val, 1);
                    }
                } else {
                    sb_append(&sb, val, 1);
                }
                break;
            }
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
        sb_appendf(body, "\"max_completion_tokens\":%d,", cfg->iMaxTokens);
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
    // Try: choices[0].message.content
    char* content = json_extract_nested_content(respJson, "choices", "content");
    if (content && content[0]) return content;
    if (content) free(content);

    // Try: choices[0].message via nested lookup with explicit path
    const char* choices = json_find_value(respJson, "choices");
    if (choices)
    {
        const char* message = json_find_value(choices, "message");
        if (message)
        {
            content = json_extract_string(message, "content");
            if (content && content[0]) return content;
            if (content) free(content);

            // If content is null, check for "refusal" field
            content = json_extract_string(message, "refusal");
            if (content && content[0])
            {
                char* full = (char*)malloc(strlen(content) + 32);
                if (full)
                {
                    sprintf(full, "Model refused: %s", content);
                    free(content);
                    return full;
                }
                return content;
            }
            if (content) free(content);
        }

        // Try direct content from choices array
        content = json_extract_string(choices, "content");
        if (content && content[0]) return content;
        if (content) free(content);

        // Try text field (some OpenAI-compatible providers)
        content = json_extract_string(choices, "text");
        if (content && content[0]) return content;
        if (content) free(content);
    }

    // Fallback: any "content" field at top level
    content = json_extract_string(respJson, "content");
    if (content && content[0]) return content;
    if (content) free(content);

    // Fallback: "result" field (some proxy APIs)
    content = json_extract_string(respJson, "result");
    if (content && content[0]) return content;
    if (content) free(content);

    return NULL;
}

static char* ParseResponse_Anthropic(const char* respJson)
{
    // Anthropic format: {"content":[{"type":"text","text":"response"}], ...}
    // The content field is an ARRAY of content blocks

    const char* contentVal = json_find_value(respJson, "content");
    if (contentVal)
    {
        // If content is an array, look inside for text blocks
        if (*contentVal == '[')
        {
            // Search for "text" key within the array (not "type":"text" value)
            char* text = json_extract_string(contentVal, "text");
            if (text && text[0]) return text;
            if (text) free(text);
        }
        // If content is a string directly (some compatible APIs)
        else if (*contentVal == '"')
        {
            char* text = json_extract_string(respJson, "content");
            if (text && text[0]) return text;
            if (text) free(text);
        }
    }

    // Fallback: nested lookup
    char* text = json_extract_nested_content(respJson, "content", "text");
    if (text && text[0]) return text;
    if (text) free(text);

    // Fallback: direct "text" field
    text = json_extract_string(respJson, "text");
    if (text && text[0]) return text;
    if (text) free(text);

    // Fallback: "completion" field (older Anthropic API)
    text = json_extract_string(respJson, "completion");
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
    if (!respJson || !respJson[0])
        return _strdup("Error: Empty response body from API");

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

    // Universal fallback: try all common response field names
    result = json_extract_string(respJson, "content");
    if (result && result[0]) return result;
    if (result) free(result);
    result = json_extract_string(respJson, "text");
    if (result && result[0]) return result;
    if (result) free(result);
    result = json_extract_string(respJson, "response");
    if (result && result[0]) return result;
    if (result) free(result);
    result = json_extract_string(respJson, "output");
    if (result && result[0]) return result;
    if (result) free(result);

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

    // Dump raw response to temp file for debugging
    {
        char tmpPath[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tmpPath))
        {
            strcat(tmpPath, "biko_api_debug.json");
            HANDLE hDbg = CreateFileA(tmpPath, GENERIC_WRITE, 0, NULL,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hDbg != INVALID_HANDLE_VALUE)
            {
                DWORD written;
                WriteFile(hDbg, respJson, (DWORD)strlen(respJson), &written, NULL);
                CloseHandle(hDbg);
            }
        }
    }

    // Include a snippet of the raw response in the error message
    int respLen = (int)strlen(respJson);
    int snippetLen = respLen < 300 ? respLen : 300;
    char* errBuf = (char*)malloc(snippetLen + 128);
    if (errBuf)
    {
        snprintf(errBuf, snippetLen + 128,
                 "Error: Could not parse API response (fmt=%d, len=%d). "
                 "Raw: %.300s%s",
                 (int)fmt, respLen, respJson,
                 respLen > 300 ? "..." : "");
        return errBuf;
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

    HINTERNET hSession = WinHttpOpen(L"Bikode/1.0",
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
        sb_appendf(body, "\"max_completion_tokens\":%d,", cfg->iMaxTokens);
    sb_append(body, "\"messages\":[", -1);
    for (int i = 0; i < count; i++)
    {
        if (i > 0) sb_append(body, ",", 1);
        sb_appendf(body, "{\"role\":\"%s\",\"content\":", msgs[i].role);

        // Check for attachments
        if (msgs[i].attachmentCount > 0 && msgs[i].attachments)
        {
            // Multi-modal content array
            sb_append(body, "[", 1);
            int hasItems = 0;
            
            // 1. Text content (if any)
            if (msgs[i].content && msgs[i].content[0])
            {
                sb_append(body, "{\"type\":\"text\",\"text\":", -1);
                json_escape_append(body, msgs[i].content);
                sb_append(body, "}", 1);
                hasItems = 1;
            }

            // 2. Attachments
            for (int k = 0; k < msgs[i].attachmentCount; k++)
            {
                AIChatAttachment* att = &msgs[i].attachments[k];
                if (att->isImage)
                {
                    char* b64 = ReadFileToBase64(att->path);
                    if (b64)
                    {
                        if (hasItems) sb_append(body, ",", 1);
                        sb_append(body, "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:", -1);
                        const char* mime = att->contentType[0] ? att->contentType : "image/png";
                        sb_append(body, mime, -1);
                        sb_append(body, ";base64,", -1);
                        sb_append(body, b64, -1);
                        sb_append(body, "\",\"detail\":\"auto\"}}", -1);
                        free(b64);
                        hasItems = 1;
                    }
                }
                else
                {
                    // Text/Code file -> append as text block
                    DWORD size = 0;
                    char* text = ReadFileContents(att->path, &size);
                    if (text)
                    {
                        if (hasItems) sb_append(body, ",", 1);
                        sb_append(body, "{\"type\":\"text\",\"text\":", -1);
                        
                        StrBuf fileMsg;
                        sb_init(&fileMsg, size + 128);
                        sb_appendf(&fileMsg, "\n\n[File: %s]\n```\n%s\n```\n", att->displayName, text);
                        json_escape_append(body, fileMsg.data);
                        sb_free(&fileMsg);
                        
                        sb_append(body, "}", 1);
                        free(text);
                        hasItems = 1;
                    }
                }
            }
            sb_append(body, "]", 1);
        }
        else
        {
            // Simple string content
            json_escape_append(body, msgs[i].content);
        }
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
