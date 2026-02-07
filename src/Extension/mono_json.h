#pragma once
/******************************************************************************
*
* Biko
*
* mono_json.h
*   Minimal JSON writer and reader for AI Bridge communication.
*   No external dependencies. Produces and parses length-prefixed JSON
*   for named pipe IPC.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// JSON Writer
//=============================================================================

typedef struct TJsonWriter
{
    char*   pBuffer;
    int     iCapacity;
    int     iLength;
    int     iDepth;
    BOOL    bNeedComma[64];  // stack for comma tracking per nesting level
    BOOL    bError;
} JsonWriter;

BOOL    JsonWriter_Init(JsonWriter* pW, int initialCapacity);
void    JsonWriter_Free(JsonWriter* pW);
void    JsonWriter_Reset(JsonWriter* pW);
char*   JsonWriter_GetBuffer(JsonWriter* pW);
int     JsonWriter_GetLength(const JsonWriter* pW);

void    JsonWriter_BeginObject(JsonWriter* pW);
void    JsonWriter_EndObject(JsonWriter* pW);
void    JsonWriter_BeginArray(JsonWriter* pW);
void    JsonWriter_EndArray(JsonWriter* pW);

void    JsonWriter_Key(JsonWriter* pW, const char* key);

void    JsonWriter_StringValue(JsonWriter* pW, const char* value);
void    JsonWriter_WStringValue(JsonWriter* pW, const WCHAR* value);
void    JsonWriter_IntValue(JsonWriter* pW, int value);
void    JsonWriter_Int64Value(JsonWriter* pW, __int64 value);
void    JsonWriter_DoubleValue(JsonWriter* pW, double value);
void    JsonWriter_BoolValue(JsonWriter* pW, BOOL value);
void    JsonWriter_NullValue(JsonWriter* pW);

// Convenience: Key + Value in one call
void    JsonWriter_String(JsonWriter* pW, const char* key, const char* value);
void    JsonWriter_WString(JsonWriter* pW, const char* key, const WCHAR* value);
void    JsonWriter_Int(JsonWriter* pW, const char* key, int value);
void    JsonWriter_Int64(JsonWriter* pW, const char* key, __int64 value);
void    JsonWriter_Double(JsonWriter* pW, const char* key, double value);
void    JsonWriter_Bool(JsonWriter* pW, const char* key, BOOL value);
void    JsonWriter_Null(JsonWriter* pW, const char* key);

//=============================================================================
// JSON Reader (streaming, pull-based)
//=============================================================================

typedef enum
{
    JSON_NONE = 0,
    JSON_OBJECT_START,
    JSON_OBJECT_END,
    JSON_ARRAY_START,
    JSON_ARRAY_END,
    JSON_KEY,
    JSON_STRING,
    JSON_NUMBER,
    JSON_BOOL,
    JSON_NULL,
    JSON_ERROR
} EJsonToken;

typedef struct TJsonReader
{
    const char* pData;
    int         iLength;
    int         iPos;
    EJsonToken  eToken;
    
    // Current token value
    char        szValue[4096];
    int         iValueLength;
    double      dValue;
    BOOL        bValue;
    
    BOOL        bError;
    char        szError[256];
} JsonReader;

BOOL        JsonReader_Init(JsonReader* pR, const char* pData, int iLength);
EJsonToken  JsonReader_Next(JsonReader* pR);
EJsonToken  JsonReader_Peek(const JsonReader* pR);

const char* JsonReader_GetString(const JsonReader* pR);
int         JsonReader_GetInt(const JsonReader* pR);
__int64     JsonReader_GetInt64(const JsonReader* pR);
double      JsonReader_GetDouble(const JsonReader* pR);
BOOL        JsonReader_GetBool(const JsonReader* pR);

BOOL        JsonReader_SkipValue(JsonReader* pR);
BOOL        JsonReader_IsError(const JsonReader* pR);
const char* JsonReader_GetError(const JsonReader* pR);

// Utility: find a key in the current object, returns TRUE if found (positions reader at the value)
BOOL        JsonReader_FindKey(JsonReader* pR, const char* key);

//=============================================================================
// Length-prefixed message framing
//=============================================================================

// Write a 4-byte little-endian length prefix + JSON payload to a buffer
// Returns total bytes written, or 0 on failure
int         Json_FrameMessage(const char* pJson, int iJsonLen, char** ppOutput);

// Read a length-prefixed message. Returns JSON length, sets *ppJson to start of JSON data.
// Returns -1 if buffer too small.
int         Json_UnframeMessage(const char* pInput, int iInputLen, const char** ppJson);

#ifdef __cplusplus
}
#endif
