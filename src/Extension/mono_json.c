/******************************************************************************
*
* Biko
*
* mono_json.c
*   Minimal JSON writer and reader implementation.
*
******************************************************************************/

#include "mono_json.h"
#include "CommonUtils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

//=============================================================================
// Internal helpers
//=============================================================================

static void jw_ensure(JsonWriter* pW, int needed)
{
    if (pW->bError) return;
    while (pW->iLength + needed >= pW->iCapacity)
    {
        int newCap = pW->iCapacity * 2;
        char* pNew = (char*)n2e_Realloc(pW->pBuffer, newCap);
        if (!pNew)
        {
            pW->bError = TRUE;
            return;
        }
        pW->pBuffer = pNew;
        pW->iCapacity = newCap;
    }
}

static void jw_append(JsonWriter* pW, const char* s, int len)
{
    if (pW->bError) return;
    jw_ensure(pW, len + 1);
    if (pW->bError) return;
    memcpy(pW->pBuffer + pW->iLength, s, len);
    pW->iLength += len;
    pW->pBuffer[pW->iLength] = '\0';
}

static void jw_appendChar(JsonWriter* pW, char c)
{
    jw_append(pW, &c, 1);
}

static void jw_comma(JsonWriter* pW)
{
    if (pW->bError) return;
    if (pW->iDepth > 0 && pW->bNeedComma[pW->iDepth])
    {
        jw_appendChar(pW, ',');
    }
    if (pW->iDepth > 0)
    {
        pW->bNeedComma[pW->iDepth] = TRUE;
    }
}

static void jw_escapeString(JsonWriter* pW, const char* s)
{
    jw_appendChar(pW, '"');
    if (s)
    {
        while (*s)
        {
            switch (*s)
            {
            case '"':  jw_append(pW, "\\\"", 2); break;
            case '\\': jw_append(pW, "\\\\", 2); break;
            case '\b': jw_append(pW, "\\b", 2);  break;
            case '\f': jw_append(pW, "\\f", 2);  break;
            case '\n': jw_append(pW, "\\n", 2);  break;
            case '\r': jw_append(pW, "\\r", 2);  break;
            case '\t': jw_append(pW, "\\t", 2);  break;
            default:
                if ((unsigned char)*s < 0x20)
                {
                    char buf[8];
                    sprintf(buf, "\\u%04x", (unsigned char)*s);
                    jw_append(pW, buf, 6);
                }
                else
                {
                    jw_appendChar(pW, *s);
                }
                break;
            }
            s++;
        }
    }
    jw_appendChar(pW, '"');
}

//=============================================================================
// JSON Writer Implementation
//=============================================================================

BOOL JsonWriter_Init(JsonWriter* pW, int initialCapacity)
{
    if (initialCapacity < 256) initialCapacity = 256;
    pW->pBuffer = (char*)n2e_Alloc(initialCapacity);
    if (!pW->pBuffer) return FALSE;
    pW->iCapacity = initialCapacity;
    pW->iLength = 0;
    pW->iDepth = 0;
    pW->bError = FALSE;
    pW->pBuffer[0] = '\0';
    memset(pW->bNeedComma, 0, sizeof(pW->bNeedComma));
    return TRUE;
}

void JsonWriter_Free(JsonWriter* pW)
{
    if (pW->pBuffer)
    {
        n2e_Free(pW->pBuffer);
        pW->pBuffer = NULL;
    }
    pW->iCapacity = 0;
    pW->iLength = 0;
}

void JsonWriter_Reset(JsonWriter* pW)
{
    pW->iLength = 0;
    pW->iDepth = 0;
    pW->bError = FALSE;
    if (pW->pBuffer) pW->pBuffer[0] = '\0';
    memset(pW->bNeedComma, 0, sizeof(pW->bNeedComma));
}

char* JsonWriter_GetBuffer(JsonWriter* pW)
{
    return pW->pBuffer;
}

int JsonWriter_GetLength(const JsonWriter* pW)
{
    return pW->iLength;
}

void JsonWriter_BeginObject(JsonWriter* pW)
{
    jw_comma(pW);
    jw_appendChar(pW, '{');
    pW->iDepth++;
    if (pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = FALSE;
}

void JsonWriter_EndObject(JsonWriter* pW)
{
    jw_appendChar(pW, '}');
    if (pW->iDepth > 0) pW->iDepth--;
}

void JsonWriter_BeginArray(JsonWriter* pW)
{
    jw_comma(pW);
    jw_appendChar(pW, '[');
    pW->iDepth++;
    if (pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = FALSE;
}

void JsonWriter_EndArray(JsonWriter* pW)
{
    jw_appendChar(pW, ']');
    if (pW->iDepth > 0) pW->iDepth--;
}

void JsonWriter_Key(JsonWriter* pW, const char* key)
{
    jw_comma(pW);
    jw_escapeString(pW, key);
    jw_appendChar(pW, ':');
    // reset comma flag so the value doesn't get a comma prefix
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = FALSE;
}

void JsonWriter_StringValue(JsonWriter* pW, const char* value)
{
    jw_comma(pW);
    jw_escapeString(pW, value);
}

void JsonWriter_WStringValue(JsonWriter* pW, const WCHAR* value)
{
    if (!value)
    {
        JsonWriter_NullValue(pW);
        return;
    }
    // Convert WCHAR to UTF-8
    int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL);
    if (needed <= 0)
    {
        JsonWriter_StringValue(pW, "");
        return;
    }
    char* utf8 = (char*)n2e_Alloc(needed);
    if (!utf8)
    {
        pW->bError = TRUE;
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, needed, NULL, NULL);
    JsonWriter_StringValue(pW, utf8);
    n2e_Free(utf8);
}

void JsonWriter_IntValue(JsonWriter* pW, int value)
{
    char buf[32];
    jw_comma(pW);
    sprintf(buf, "%d", value);
    jw_append(pW, buf, (int)strlen(buf));
}

void JsonWriter_Int64Value(JsonWriter* pW, __int64 value)
{
    char buf[32];
    jw_comma(pW);
    sprintf(buf, "%lld", value);
    jw_append(pW, buf, (int)strlen(buf));
}

void JsonWriter_DoubleValue(JsonWriter* pW, double value)
{
    char buf[64];
    jw_comma(pW);
    sprintf(buf, "%.15g", value);
    jw_append(pW, buf, (int)strlen(buf));
}

void JsonWriter_BoolValue(JsonWriter* pW, BOOL value)
{
    jw_comma(pW);
    if (value)
        jw_append(pW, "true", 4);
    else
        jw_append(pW, "false", 5);
}

void JsonWriter_NullValue(JsonWriter* pW)
{
    jw_comma(pW);
    jw_append(pW, "null", 4);
}

// Convenience: Key + Value
void JsonWriter_String(JsonWriter* pW, const char* key, const char* value)
{
    JsonWriter_Key(pW, key);
    jw_escapeString(pW, value);
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = TRUE;
}

void JsonWriter_WString(JsonWriter* pW, const char* key, const WCHAR* value)
{
    JsonWriter_Key(pW, key);
    JsonWriter_WStringValue(pW, value);
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = TRUE;
}

void JsonWriter_Int(JsonWriter* pW, const char* key, int value)
{
    JsonWriter_Key(pW, key);
    char buf[32];
    sprintf(buf, "%d", value);
    jw_append(pW, buf, (int)strlen(buf));
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = TRUE;
}

void JsonWriter_Int64(JsonWriter* pW, const char* key, __int64 value)
{
    JsonWriter_Key(pW, key);
    char buf[32];
    sprintf(buf, "%lld", value);
    jw_append(pW, buf, (int)strlen(buf));
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = TRUE;
}

void JsonWriter_Double(JsonWriter* pW, const char* key, double value)
{
    JsonWriter_Key(pW, key);
    char buf[64];
    sprintf(buf, "%.15g", value);
    jw_append(pW, buf, (int)strlen(buf));
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = TRUE;
}

void JsonWriter_Bool(JsonWriter* pW, const char* key, BOOL value)
{
    JsonWriter_Key(pW, key);
    if (value)
        jw_append(pW, "true", 4);
    else
        jw_append(pW, "false", 5);
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = TRUE;
}

void JsonWriter_Null(JsonWriter* pW, const char* key)
{
    JsonWriter_Key(pW, key);
    jw_append(pW, "null", 4);
    if (pW->iDepth > 0 && pW->iDepth < 64)
        pW->bNeedComma[pW->iDepth] = TRUE;
}

//=============================================================================
// JSON Reader Implementation
//=============================================================================

static void jr_skipWhitespace(JsonReader* pR)
{
    while (pR->iPos < pR->iLength)
    {
        char c = pR->pData[pR->iPos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            pR->iPos++;
        else
            break;
    }
}

static void jr_setError(JsonReader* pR, const char* msg)
{
    pR->bError = TRUE;
    pR->eToken = JSON_ERROR;
    strncpy(pR->szError, msg, sizeof(pR->szError) - 1);
    pR->szError[sizeof(pR->szError) - 1] = '\0';
}

static BOOL jr_readString(JsonReader* pR)
{
    if (pR->iPos >= pR->iLength || pR->pData[pR->iPos] != '"')
    {
        jr_setError(pR, "Expected '\"'");
        return FALSE;
    }
    pR->iPos++; // skip opening quote
    pR->iValueLength = 0;

    while (pR->iPos < pR->iLength)
    {
        char c = pR->pData[pR->iPos++];
        if (c == '"')
        {
            pR->szValue[pR->iValueLength] = '\0';
            return TRUE;
        }
        if (c == '\\')
        {
            if (pR->iPos >= pR->iLength)
            {
                jr_setError(pR, "Unexpected end of string escape");
                return FALSE;
            }
            char esc = pR->pData[pR->iPos++];
            switch (esc)
            {
            case '"':  c = '"'; break;
            case '\\': c = '\\'; break;
            case '/':  c = '/'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u':
                // Read 4 hex digits, convert to UTF-8
                if (pR->iPos + 4 > pR->iLength)
                {
                    jr_setError(pR, "Incomplete \\u escape");
                    return FALSE;
                }
                {
                    char hex[5];
                    memcpy(hex, pR->pData + pR->iPos, 4);
                    hex[4] = '\0';
                    pR->iPos += 4;
                    unsigned int codepoint = (unsigned int)strtoul(hex, NULL, 16);
                    if (codepoint < 0x80)
                    {
                        if (pR->iValueLength < (int)sizeof(pR->szValue) - 1)
                            pR->szValue[pR->iValueLength++] = (char)codepoint;
                    }
                    else if (codepoint < 0x800)
                    {
                        if (pR->iValueLength < (int)sizeof(pR->szValue) - 2)
                        {
                            pR->szValue[pR->iValueLength++] = (char)(0xC0 | (codepoint >> 6));
                            pR->szValue[pR->iValueLength++] = (char)(0x80 | (codepoint & 0x3F));
                        }
                    }
                    else
                    {
                        if (pR->iValueLength < (int)sizeof(pR->szValue) - 3)
                        {
                            pR->szValue[pR->iValueLength++] = (char)(0xE0 | (codepoint >> 12));
                            pR->szValue[pR->iValueLength++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                            pR->szValue[pR->iValueLength++] = (char)(0x80 | (codepoint & 0x3F));
                        }
                    }
                }
                continue; // already handled
            default:
                jr_setError(pR, "Invalid escape character");
                return FALSE;
            }
        }
        if (pR->iValueLength < (int)sizeof(pR->szValue) - 1)
            pR->szValue[pR->iValueLength++] = c;
    }
    jr_setError(pR, "Unterminated string");
    return FALSE;
}

static BOOL jr_readNumber(JsonReader* pR)
{
    int start = pR->iPos;
    if (pR->pData[pR->iPos] == '-') pR->iPos++;

    while (pR->iPos < pR->iLength &&
           pR->pData[pR->iPos] >= '0' && pR->pData[pR->iPos] <= '9')
        pR->iPos++;

    if (pR->iPos < pR->iLength && pR->pData[pR->iPos] == '.')
    {
        pR->iPos++;
        while (pR->iPos < pR->iLength &&
               pR->pData[pR->iPos] >= '0' && pR->pData[pR->iPos] <= '9')
            pR->iPos++;
    }

    if (pR->iPos < pR->iLength && (pR->pData[pR->iPos] == 'e' || pR->pData[pR->iPos] == 'E'))
    {
        pR->iPos++;
        if (pR->iPos < pR->iLength && (pR->pData[pR->iPos] == '+' || pR->pData[pR->iPos] == '-'))
            pR->iPos++;
        while (pR->iPos < pR->iLength &&
               pR->pData[pR->iPos] >= '0' && pR->pData[pR->iPos] <= '9')
            pR->iPos++;
    }

    int len = pR->iPos - start;
    if (len <= 0 || len >= (int)sizeof(pR->szValue))
    {
        jr_setError(pR, "Invalid number");
        return FALSE;
    }
    memcpy(pR->szValue, pR->pData + start, len);
    pR->szValue[len] = '\0';
    pR->iValueLength = len;
    pR->dValue = atof(pR->szValue);
    return TRUE;
}

BOOL JsonReader_Init(JsonReader* pR, const char* pData, int iLength)
{
    if (!pData || iLength <= 0) return FALSE;
    pR->pData = pData;
    pR->iLength = iLength;
    pR->iPos = 0;
    pR->eToken = JSON_NONE;
    pR->szValue[0] = '\0';
    pR->iValueLength = 0;
    pR->dValue = 0.0;
    pR->bValue = FALSE;
    pR->bError = FALSE;
    pR->szError[0] = '\0';
    return TRUE;
}

EJsonToken JsonReader_Next(JsonReader* pR)
{
    if (pR->bError) return JSON_ERROR;

    jr_skipWhitespace(pR);
    if (pR->iPos >= pR->iLength)
    {
        pR->eToken = JSON_NONE;
        return JSON_NONE;
    }

    char c = pR->pData[pR->iPos];

    // Consume separators
    while (c == ',' || c == ':')
    {
        pR->iPos++;
        jr_skipWhitespace(pR);
        if (pR->iPos >= pR->iLength)
        {
            pR->eToken = JSON_NONE;
            return JSON_NONE;
        }
        c = pR->pData[pR->iPos];
    }

    switch (c)
    {
    case '{':
        pR->iPos++;
        pR->eToken = JSON_OBJECT_START;
        return JSON_OBJECT_START;

    case '}':
        pR->iPos++;
        pR->eToken = JSON_OBJECT_END;
        return JSON_OBJECT_END;

    case '[':
        pR->iPos++;
        pR->eToken = JSON_ARRAY_START;
        return JSON_ARRAY_START;

    case ']':
        pR->iPos++;
        pR->eToken = JSON_ARRAY_END;
        return JSON_ARRAY_END;

    case '"':
    {
        if (!jr_readString(pR)) return JSON_ERROR;
        // Determine if this is a key or a string value
        // Look ahead for ':'
        int savedPos = pR->iPos;
        jr_skipWhitespace(pR);
        if (pR->iPos < pR->iLength && pR->pData[pR->iPos] == ':')
        {
            pR->eToken = JSON_KEY;
            // Don't consume the colon â€” it will be consumed as separator
        }
        else
        {
            pR->iPos = savedPos;
            pR->eToken = JSON_STRING;
        }
        return pR->eToken;
    }

    case 't':
        if (pR->iPos + 4 <= pR->iLength && memcmp(pR->pData + pR->iPos, "true", 4) == 0)
        {
            pR->iPos += 4;
            pR->bValue = TRUE;
            pR->eToken = JSON_BOOL;
            return JSON_BOOL;
        }
        jr_setError(pR, "Invalid token");
        return JSON_ERROR;

    case 'f':
        if (pR->iPos + 5 <= pR->iLength && memcmp(pR->pData + pR->iPos, "false", 5) == 0)
        {
            pR->iPos += 5;
            pR->bValue = FALSE;
            pR->eToken = JSON_BOOL;
            return JSON_BOOL;
        }
        jr_setError(pR, "Invalid token");
        return JSON_ERROR;

    case 'n':
        if (pR->iPos + 4 <= pR->iLength && memcmp(pR->pData + pR->iPos, "null", 4) == 0)
        {
            pR->iPos += 4;
            pR->eToken = JSON_NULL;
            return JSON_NULL;
        }
        jr_setError(pR, "Invalid token");
        return JSON_ERROR;

    default:
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            if (!jr_readNumber(pR)) return JSON_ERROR;
            pR->eToken = JSON_NUMBER;
            return JSON_NUMBER;
        }
        jr_setError(pR, "Unexpected character");
        return JSON_ERROR;
    }
}

EJsonToken JsonReader_Peek(const JsonReader* pR)
{
    return pR->eToken;
}

const char* JsonReader_GetString(const JsonReader* pR)
{
    return pR->szValue;
}

int JsonReader_GetInt(const JsonReader* pR)
{
    return atoi(pR->szValue);
}

__int64 JsonReader_GetInt64(const JsonReader* pR)
{
    return _atoi64(pR->szValue);
}

double JsonReader_GetDouble(const JsonReader* pR)
{
    return pR->dValue;
}

BOOL JsonReader_GetBool(const JsonReader* pR)
{
    return pR->bValue;
}

BOOL JsonReader_SkipValue(JsonReader* pR)
{
    EJsonToken tok = pR->eToken;
    if (tok == JSON_STRING || tok == JSON_NUMBER || tok == JSON_BOOL || tok == JSON_NULL || tok == JSON_KEY)
    {
        // Already consumed â€” if it was a key, skip the value too
        if (tok == JSON_KEY)
        {
            JsonReader_Next(pR);
            return JsonReader_SkipValue(pR);
        }
        return TRUE;
    }
    if (tok == JSON_OBJECT_START)
    {
        int depth = 1;
        while (depth > 0)
        {
            EJsonToken t = JsonReader_Next(pR);
            if (t == JSON_OBJECT_START) depth++;
            else if (t == JSON_OBJECT_END) depth--;
            else if (t == JSON_ERROR || t == JSON_NONE) return FALSE;
        }
        return TRUE;
    }
    if (tok == JSON_ARRAY_START)
    {
        int depth = 1;
        while (depth > 0)
        {
            EJsonToken t = JsonReader_Next(pR);
            if (t == JSON_ARRAY_START) depth++;
            else if (t == JSON_ARRAY_END) depth--;
            else if (t == JSON_ERROR || t == JSON_NONE) return FALSE;
        }
        return TRUE;
    }
    return FALSE;
}

BOOL JsonReader_IsError(const JsonReader* pR)
{
    return pR->bError;
}

const char* JsonReader_GetError(const JsonReader* pR)
{
    return pR->szError;
}

BOOL JsonReader_FindKey(JsonReader* pR, const char* key)
{
    // Assumes we are inside an object (just after JSON_OBJECT_START)
    while (!pR->bError)
    {
        EJsonToken tok = JsonReader_Next(pR);
        if (tok == JSON_OBJECT_END || tok == JSON_NONE || tok == JSON_ERROR)
            return FALSE;
        if (tok == JSON_KEY)
        {
            if (strcmp(pR->szValue, key) == 0)
            {
                // Position reader at the value â€” caller should call JsonReader_Next()
                return TRUE;
            }
            // Skip this key's value
            JsonReader_Next(pR);
            JsonReader_SkipValue(pR);
        }
    }
    return FALSE;
}

//=============================================================================
// Message Framing
//=============================================================================

int Json_FrameMessage(const char* pJson, int iJsonLen, char** ppOutput)
{
    if (!pJson || iJsonLen <= 0 || !ppOutput) return 0;
    int totalLen = 4 + iJsonLen;
    char* pOut = (char*)n2e_Alloc(totalLen);
    if (!pOut) return 0;

    // Little-endian 4-byte length prefix
    pOut[0] = (char)(iJsonLen & 0xFF);
    pOut[1] = (char)((iJsonLen >> 8) & 0xFF);
    pOut[2] = (char)((iJsonLen >> 16) & 0xFF);
    pOut[3] = (char)((iJsonLen >> 24) & 0xFF);
    memcpy(pOut + 4, pJson, iJsonLen);

    *ppOutput = pOut;
    return totalLen;
}

int Json_UnframeMessage(const char* pInput, int iInputLen, const char** ppJson)
{
    if (!pInput || iInputLen < 4 || !ppJson) return -1;

    unsigned int len = (unsigned char)pInput[0]
                     | ((unsigned char)pInput[1] << 8)
                     | ((unsigned char)pInput[2] << 16)
                     | ((unsigned char)pInput[3] << 24);

    if ((int)(4 + len) > iInputLen) return -1;

    *ppJson = pInput + 4;
    return (int)len;
}
