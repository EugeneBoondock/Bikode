/******************************************************************************
*
* Biko
*
* AIAgent.c
*   Agentic AI loop with tool execution capabilities.
*   Supports: read_file, write_file, replace_in_file, run_command, list_dir.
*   Maintains conversation history across user turns.
*   Runs multiple LLM calls in a loop until the AI produces a final answer.
*
******************************************************************************/

#include "AIAgent.h"
#include "AIContext.h"
#include "AIDirectCall.h"
#include "AIBridge.h"
#include "AgentRuntime.h"
#include "CodeEmbeddingIndex.h"
#include "CommonUtils.h"
#include "Externals.h"
#include "FileManager.h"
#include "Terminal.h"
#include "Utils.h"
#include "ViewHelper.h"
#include <windows.h>
#include <shlwapi.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

#pragma comment(lib, "Shlwapi.lib")

//=============================================================================
// Constants
//=============================================================================

#define MAX_HISTORY_TURNS       20
#define MAX_HISTORY_MESSAGES    (MAX_HISTORY_TURNS * 2)
#define MAX_AGENT_ITERATIONS    15
#define MAX_MESSAGES_PER_CALL   128
#define MAX_FILE_READ_SIZE      (50 * 1024)
#define MAX_FILE_WINDOW_SIZE    (64 * 1024)
#define MAX_CMD_OUTPUT_SIZE     (20 * 1024)
#define MAX_TOOL_RESULT_SNIPPET 4096
#define MAX_TOOL_FEEDBACK_SIZE  (12 * 1024)
#define CMD_TIMEOUT_MS          30000
#define MAX_TRACKED_FILES       16
#define MAX_FAILURE_MEMORY      24

typedef enum {
    AGENT_MODE_QUICK = 0,
    AGENT_MODE_BALANCED,
    AGENT_MODE_MAX_QUALITY,
    AGENT_MODE_ECONOMY
} AgentExecutionMode;

typedef struct {
    AgentExecutionMode mode;
    int maxIterations;
    int maxToolCalls;
    int maxTouchedFiles;
    int maxApproxPromptChars;
    int allowShell;
} BudgetContract;

typedef struct {
    char tool[48];
    char target[260];
    int failures;
} FailureEntry;

typedef struct {
    unsigned long turnId;
    unsigned long toolCalls;
    unsigned long usefulToolCalls;
    unsigned long blockedToolCalls;
    unsigned long approxCharsSent;
    unsigned long approxCharsSaved;
} ContextLedger;

typedef enum {
    AGENT_ROLE_PLANNER = 0,
    AGENT_ROLE_IMPLEMENTER,
    AGENT_ROLE_REVIEWER,
    AGENT_ROLE_DEBUG,
    AGENT_ROLE_TEST,
    AGENT_ROLE_REFACTOR,
    AGENT_ROLE_RESEARCH,
    AGENT_ROLE_SETUP
} AgentRole;

typedef struct {
    AgentRole role;
    const char* name;
    const char* allowedTools;
    int maxIterations;
    int stopOnFirstMutation;
} AgentRoleContract;

typedef struct {
    const char* name;
    const char* owner;
    const char* status;
} TaskNode;

extern WCHAR szCurFile[N2E_MAX_PATH_N_CMD_LINE];
extern BOOL bModified;

static BOOL IsAbsolutePathA(const char* path)
{
    if (!path || !path[0])
        return FALSE;
    if ((isalpha((unsigned char)path[0]) && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/')) ||
        (path[0] == '\\' && path[1] == '\\'))
        return TRUE;
    return FALSE;
}

static BOOL ResolveWorkspaceRootW(WCHAR* wszOut, int cchOut)
{
    const WCHAR* wszRoot;
    if (!wszOut || cchOut <= 0)
        return FALSE;
    wszOut[0] = L'\0';

    wszRoot = FileManager_GetRootPath();
    if (wszRoot && wszRoot[0])
    {
        lstrcpynW(wszOut, wszRoot, cchOut);
        return TRUE;
    }
    if (szCurFile[0])
    {
        if (AIContext_GetProjectRoot(szCurFile, wszOut, cchOut))
            return TRUE;
        lstrcpynW(wszOut, szCurFile, cchOut);
        PathRemoveFileSpecW(wszOut);
        return wszOut[0] != L'\0';
    }
    GetCurrentDirectoryW(cchOut, wszOut);
    return wszOut[0] != L'\0';
}

static BOOL ResolveWorkspacePathA(const char* inputPath, char* outPath, int cchOut)
{
    WCHAR wszRoot[MAX_PATH];
    int rootNeeded;
    char rootUtf8[MAX_PATH];
    char normalized[MAX_PATH];
    const char* tail;
    char* p;

    if (!outPath || cchOut <= 0)
        return FALSE;
    outPath[0] = '\0';
    if (!inputPath || !inputPath[0])
        return FALSE;
    if (IsAbsolutePathA(inputPath))
    {
        _snprintf_s(outPath, cchOut, _TRUNCATE, "%s", inputPath);
    }
    else
    {
        if (!ResolveWorkspaceRootW(wszRoot, ARRAYSIZE(wszRoot)))
            return FALSE;
        rootNeeded = WideCharToMultiByte(CP_UTF8, 0, wszRoot, -1, rootUtf8, ARRAYSIZE(rootUtf8), NULL, NULL);
        if (rootNeeded <= 0)
            return FALSE;
        tail = inputPath;
        if ((tail[0] == '.' && (tail[1] == '\\' || tail[1] == '/')) ||
            (tail[0] == '.' && tail[1] == '.' && (tail[2] == '\\' || tail[2] == '/')))
        {
            tail = inputPath;
        }
        _snprintf_s(outPath, cchOut, _TRUNCATE, "%s\\%s", rootUtf8, tail);
    }

    _snprintf_s(normalized, ARRAYSIZE(normalized), _TRUNCATE, "%s", outPath);
    for (p = normalized; *p; p++)
    {
        if (*p == '/')
            *p = '\\';
    }
    _snprintf_s(outPath, cchOut, _TRUNCATE, "%s", normalized);
    return TRUE;
}

static void InvalidateWorkspaceIndex(void)
{
    WCHAR wszRoot[MAX_PATH];
    if (ResolveWorkspaceRootW(wszRoot, ARRAYSIZE(wszRoot)))
        CodeEmbeddingIndex_InvalidateProject(wszRoot);
}

//=============================================================================
// Growable string buffer (local copy)
//=============================================================================

typedef struct {
    char*   data;
    int     len;
    int     cap;
} StrBuf;

typedef enum {
    INTENT_ASK = 0,
    INTENT_SEARCH,
    INTENT_EXPLAIN,
    INTENT_PATCH,
    INTENT_REFACTOR,
    INTENT_RUN,
    INTENT_REVIEW,
    INTENT_BUILD,
    INTENT_DESIGN_CHANGE,
    INTENT_SELF_MODIFY_IDE
} AgentIntent;

typedef struct ToolCall {
    char    name[64];
    char*   path;
    char*   content;
    char*   oldText;
    char*   newText;
    char*   command;
    char*   cwd;
    int     startLine;
    int     lineCount;
    int     maxResults;
} ToolCall;

static BOOL ContainsSubstringCI(const char* haystack, const char* needle);
static BOOL IsPlaceholderResponseText(const char* text);
static const char* IntentName(AgentIntent intent);
static AgentIntent DetectIntent(const char* msg);
static void BuildBudgetContract(BudgetContract* c, const char* msg, AgentIntent intent);
static const char* ModeName(AgentExecutionMode mode);
static void ContextLedger_RecordPrompt(ContextLedger* ledger, AIChatMessage* msgs, int count);
static BOOL TrackTouchedFile(const ToolCall* tc, char touched[MAX_TRACKED_FILES][260], int* n);
static BOOL FailureMemory_ShouldBlock(FailureEntry mem[MAX_FAILURE_MEMORY], int count, const ToolCall* tc);
static void PostTaskGraphStatus(HWND hwnd, AgentIntent intent);
static void PostStatusToUI(HWND hwnd, const char* fmt, ...);
static AgentRole SelectRoleFromIntent(AgentIntent intent);
static const AgentRoleContract* GetRoleContract(AgentRole role);
static BOOL IsToolAllowedForRole(const AgentRoleContract* contract, const char* toolName);
static void ContextLedger_Persist(const ContextLedger* ledger, const char* mode, const char* intent,
                                  int touchedFiles, BOOL shadowMode, const char* topFile);
static int ExtractFirstInteger(const char* text);
static char* WideToUtf8Dup(const WCHAR* wsz);
static BOOL ResolveWorkspaceRootW(WCHAR* wszOut, int cchOut);
static BOOL ResolveWorkspacePathA(const char* inputPath, char* outPath, int cchOut);
static void InvalidateWorkspaceIndex(void);
static char* BuildLocalRepoBrief(const char* userMessage);

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
    char tmp[4096];
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

static unsigned int fnv1a_hash32(const char* data, int len)
{
    unsigned int h = 2166136261u;
    for (int i = 0; i < len; i++)
    {
        h ^= (unsigned char)data[i];
        h *= 16777619u;
    }
    return h;
}

static void sb_append_live_embedding_index(StrBuf* sb, const char* text, int len, int chunkBytes)
{
    if (!sb || !text || len <= 0)
        return;

    if (chunkBytes < 256)
        chunkBytes = 256;

    sb_append(sb, "\n\n[live-embedding-index]\n", -1);
    for (int start = 0, chunk = 0; start < len; start += chunkBytes, chunk++)
    {
        int n = len - start;
        if (n > chunkBytes)
            n = chunkBytes;
        sb_appendf(sb, "chunk=%d bytes=%d-%d hash=%08x\n",
            chunk, start, start + n, fnv1a_hash32(text + start, n));
    }
}

//=============================================================================
// Minimal JSON parsing (for tool call extraction)
//=============================================================================

static const char* json_find_value_a(const char* json, const char* key)
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
            afterKey++;
            while (*afterKey && (*afterKey == ' ' || *afterKey == '\t' ||
                   *afterKey == '\n' || *afterKey == '\r')) afterKey++;
            return afterKey;
        }
        // Not a key (it's a value) ???????? keep searching
        p += needleLen;
    }
    return NULL;
}

static int hex_digit_a(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int decode_uescape(const char* p, char* out)
{
    int d0 = hex_digit_a(p[0]), d1 = hex_digit_a(p[1]);
    int d2 = hex_digit_a(p[2]), d3 = hex_digit_a(p[3]);
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

static char* json_extract_string_a(const char* json, const char* key)
{
    const char* val = json_find_value_a(json, key);
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
            case '/':  sb_append(&sb, "/", 1); break;
            case 'n':  sb_append(&sb, "\n", 1); break;
            case 'r':  sb_append(&sb, "\r", 1); break;
            case 't':  sb_append(&sb, "\t", 1); break;
            case 'b':  sb_append(&sb, "\b", 1); break;
            case 'f':  sb_append(&sb, "\f", 1); break;
            case 'u': {
                if (val[1] && val[2] && val[3] && val[4]) {
                    char utf8[4];
                    int n = decode_uescape(val + 1, utf8);
                    if (n > 0) {
                        sb_append(&sb, utf8, n);
                        val += 4;
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

static int json_extract_int_a(const char* json, const char* key, int defaultValue)
{
    const char* val = json_find_value_a(json, key);
    if (!val) return defaultValue;

    char* endPtr = NULL;
    long parsed = strtol(val, &endPtr, 10);
    if (endPtr == val)
        return defaultValue;

    return (int)parsed;
}

//=============================================================================
// Conversation history (persists across user turns)
//=============================================================================

typedef struct {
    char* role;
    char* content;
} HistoryEntry;

static HistoryEntry     s_history[MAX_HISTORY_MESSAGES];
static int              s_historyCount = 0;
static CRITICAL_SECTION s_csHistory;
static BOOL             s_bInitialized = FALSE;
static volatile LONG    s_bBusy = FALSE;

static void AddToHistory(const char* role, const char* content)
{
    EnterCriticalSection(&s_csHistory);

    // If full, drop oldest 2 entries (one turn)
    if (s_historyCount >= MAX_HISTORY_MESSAGES)
    {
        free(s_history[0].role);
        free(s_history[0].content);
        free(s_history[1].role);
        free(s_history[1].content);
        memmove(&s_history[0], &s_history[2],
                (MAX_HISTORY_MESSAGES - 2) * sizeof(HistoryEntry));
        s_historyCount -= 2;
    }

    s_history[s_historyCount].role = _strdup(role);
    s_history[s_historyCount].content = _strdup(content);
    s_historyCount++;

    LeaveCriticalSection(&s_csHistory);
}

static char* WideToUtf8Dup(const WCHAR* wsz)
{
    if (!wsz || !wsz[0])
        return _strdup("");

    int len = WideCharToMultiByte(CP_UTF8, 0, wsz, -1, NULL, 0, NULL, NULL);
    if (len <= 0)
        return _strdup("");

    char* out = (char*)malloc(len);
    if (!out)
        return _strdup("");

    WideCharToMultiByte(CP_UTF8, 0, wsz, -1, out, len, NULL, NULL);
    return out;
}

static char* BuildLocalRepoBrief(const char* userMessage)
{
    WCHAR wszRoot[MAX_PATH];
    char* pathHint = NULL;
    char* hits = NULL;
    StrBuf sb;
    if (!userMessage || !userMessage[0])
        return NULL;
    if (!ResolveWorkspaceRootW(wszRoot, ARRAYSIZE(wszRoot)))
        return NULL;
    if (szCurFile[0])
        pathHint = WideToUtf8Dup(szCurFile);
    if (!CodeEmbeddingIndex_QueryProject(wszRoot, userMessage, pathHint, 4, &hits) ||
        !hits || !hits[0] || strstr(hits, "(no strong repo matches found)") != NULL)
    {
        if (pathHint) free(pathHint);
        if (hits) free(hits);
        return NULL;
    }

    sb_init(&sb, (int)strlen(hits) + 256);
    sb_append(&sb,
        "Local repo index hits from Bikode's on-device code embeddings. "
        "Use these as likely starting points, then verify exact text with read_file or get_active_document:\n", -1);
    sb_append(&sb, hits, -1);
    free(pathHint);
    free(hits);
    return sb.data;
}

static HWND s_hwndMainForTools = NULL;

static char* ConvertCodePageToUtf8Dup(const char* text, UINT codePage)
{
    int wideLen;
    WCHAR* wideBuf;
    int utf8Len;
    char* utf8Buf;

    if (!text)
        return _strdup("");

    wideLen = MultiByteToWideChar(codePage, 0, text, -1, NULL, 0);
    if (wideLen <= 0)
        return NULL;

    wideBuf = (WCHAR*)malloc((size_t)wideLen * sizeof(WCHAR));
    if (!wideBuf)
        return NULL;

    if (MultiByteToWideChar(codePage, 0, text, -1, wideBuf, wideLen) <= 0)
    {
        free(wideBuf);
        return NULL;
    }

    utf8Len = WideCharToMultiByte(CP_UTF8, 0, wideBuf, -1, NULL, 0, NULL, NULL);
    if (utf8Len <= 0)
    {
        free(wideBuf);
        return NULL;
    }

    utf8Buf = (char*)malloc(utf8Len);
    if (!utf8Buf)
    {
        free(wideBuf);
        return NULL;
    }

    if (WideCharToMultiByte(CP_UTF8, 0, wideBuf, -1, utf8Buf, utf8Len, NULL, NULL) <= 0)
    {
        free(wideBuf);
        free(utf8Buf);
        return NULL;
    }

    free(wideBuf);
    return utf8Buf;
}

static BOOL IsValidUtf8Text(const char* text)
{
    if (!text || !text[0])
        return TRUE;

    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0) > 0;
}

static char* EnsureUtf8TextDup(const char* text, UINT preferredCodePage)
{
    char* converted;

    if (!text)
        return _strdup("");

    if (IsValidUtf8Text(text))
        return _strdup(text);

    converted = ConvertCodePageToUtf8Dup(text, preferredCodePage);
    if (converted)
        return converted;

    if (preferredCodePage != CP_ACP)
    {
        converted = ConvertCodePageToUtf8Dup(text, CP_ACP);
        if (converted)
            return converted;
    }

    if (preferredCodePage != CP_OEMCP)
    {
        converted = ConvertCodePageToUtf8Dup(text, CP_OEMCP);
        if (converted)
            return converted;
    }

    {
        StrBuf sb;
        const unsigned char* p;

        sb_init(&sb, (int)strlen(text) + 32);
        for (p = (const unsigned char*)text; *p; ++p)
        {
            const unsigned char ch = *p;
            if (ch == '\n' || ch == '\r' || ch == '\t' || ch >= 0x20)
            {
                if (ch < 0x80)
                    sb_append(&sb, (const char*)p, 1);
                else
                    sb_append(&sb, "?", 1);
            }
        }
        return sb.data;
    }
}

static void StripUnsafeControlsInPlace(char* text)
{
    char* src = text;
    char* dst = text;

    if (!text)
        return;

    while (*src)
    {
        const unsigned char ch = (unsigned char)*src;
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch >= 0x20)
            *dst++ = *src;
        src++;
    }
    *dst = '\0';
}

static int Utf8SafePrefixLen(const char* text, int maxBytes)
{
    int i = 0;

    if (!text || maxBytes <= 0)
        return 0;

    while (text[i] && i < maxBytes)
    {
        unsigned char ch = (unsigned char)text[i];
        int seqLen = 1;

        if (ch < 0x80)
            seqLen = 1;
        else if ((ch & 0xE0) == 0xC0)
            seqLen = 2;
        else if ((ch & 0xF0) == 0xE0)
            seqLen = 3;
        else if ((ch & 0xF8) == 0xF0)
            seqLen = 4;

        if (i + seqLen > maxBytes)
            break;

        i += seqLen;
    }

    return i;
}

static char* PrepareToolResultForModel(const char* toolName, const char* text, int maxBytes)
{
    UINT fallbackCodePage = CP_ACP;
    char* utf8Text;
    int keepLen;
    BOOL truncated;
    StrBuf sb;

    if (toolName &&
        (strcmp(toolName, "run_command") == 0 || strcmp(toolName, "list_dir") == 0))
    {
        fallbackCodePage = CP_OEMCP;
    }

    utf8Text = EnsureUtf8TextDup(text ? text : "", fallbackCodePage);
    if (!utf8Text)
        return _strdup("");

    StripUnsafeControlsInPlace(utf8Text);
    keepLen = Utf8SafePrefixLen(utf8Text, maxBytes);
    truncated = ((int)strlen(utf8Text) > keepLen);

    sb_init(&sb, keepLen + 48);
    if (keepLen > 0)
        sb_append(&sb, utf8Text, keepLen);
    if (truncated)
        sb_append(&sb, "\n[... truncated]\n", -1);

    free(utf8Text);
    return sb.data;
}

static char* BuildCompactRetryMessage(const char* text)
{
    char* compact = PrepareToolResultForModel("run_command", text, 1800);
    StrBuf sb;

    if (!compact)
        return _strdup("Tool execution completed. Verbose output was omitted to keep the follow-up request valid.");

    sb_init(&sb, (int)strlen(compact) + 128);
    sb_append(&sb,
        "Tool execution completed. Verbose output was compacted to keep the next request body valid.\n\n",
        -1);
    sb_append(&sb, compact, -1);
    free(compact);
    return sb.data;
}

static void MirrorCommandActivityToTerminal(const char* cwd, const char* command,
                                            const char* output, DWORD exitCode,
                                            BOOL timedOut)
{
    StrBuf transcript;
    char* safeOutput;

    if (!s_hwndMainForTools || !command || !command[0])
        return;

    sb_init(&transcript, 1024);
    sb_append(&transcript, "\r\n[Bikode agent]", -1);
    if (cwd && cwd[0])
        sb_appendf(&transcript, " [cwd: %s]", cwd);
    sb_appendf(&transcript, "\r\n> %s\r\n", command);
    Terminal_AppendTranscript(s_hwndMainForTools, transcript.data);
    sb_free(&transcript);

    safeOutput = PrepareToolResultForModel("run_command", output ? output : "(no output)", MAX_TOOL_RESULT_SNIPPET);
    if (!safeOutput)
        return;

    sb_init(&transcript, (int)strlen(safeOutput) + 128);
    if (safeOutput[0])
        sb_append(&transcript, safeOutput, -1);
    else
        sb_append(&transcript, "(no output)", -1);

    if (timedOut)
        sb_append(&transcript, "\r\n[Process timed out]\r\n", -1);
    else if (exitCode != 0)
        sb_appendf(&transcript, "\r\n[Exit code: %lu]\r\n", exitCode);
    else
        sb_append(&transcript, "\r\n", -1);

    Terminal_AppendTranscript(s_hwndMainForTools, transcript.data);
    sb_free(&transcript);
    free(safeOutput);
}

//=============================================================================
// System prompt
//=============================================================================


static char* LoadRepoConstitution(void)
{
    static const char* candidates[] = {
        "repo.constitution.md",
        "doc/repo.constitution.md",
        NULL
    };
    for (int i = 0; candidates[i]; i++)
    {
        FILE* f = fopen(candidates[i], "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long len = ftell(f);
        if (len <= 0) { fclose(f); continue; }
        if (len > 12000) len = 12000;
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); continue; }
        char* buf = (char*)malloc((size_t)len + 1);
        if (!buf) { fclose(f); continue; }
        size_t n = fread(buf, 1, (size_t)len, f);
        fclose(f);
        buf[n] = '\0';
        return buf;
    }
    return NULL;
}

static char* BuildSystemPrompt(const BudgetContract* contract, AgentIntent intent)
{
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);

    StrBuf sb;
    sb_init(&sb, 4096);

    sb_append(&sb,
        "SYSTEM PROMPT -- \"Bikode Assistant: Bikode-Mode\"\n\n"
        "Identity\n"
        "You are the Bikode Assistant (codename: Bikode): a minimalistic IDE partner for coding, debugging, and learning, built into a lightweight code editor. "
        "Your demeanor is inspired by Steve Biko????????s habits of reasoning and communication: direct, clear, grounded in reality, respectful of human dignity, and biased toward practical action. "
        "You are not Steve Biko. You do not imitate him as a person; you adopt a similar problem approach.\n\n"
        "Bikode is an AI-first IDE. Its house motto is \"I write what I like.\" "
        "Treat that motto as a product rule: strengthen the user's agency, preserve their voice, and keep the machine in service of the author's intent.\n\n"

        "Default output style\n"
        "- Be concise by default.\n"
        "- Expand when the user asks, or when risk is high (security, data loss, big refactors).\n\n"
        "- You may send a GIF occasionally when context truly benefits from it (humor, celebration, thinking pauses).\n"
        "- Never invent GIF links from memory. Always call gif_search first and only use the returned validated URL.\n"
        "- If user asks for a GIF, call gif_search using intent-rich query words from the user context.\n"
        "- Keep GIF links concise and put the URL on its own line.\n\n"

        "Greetings and light interactions\n"
        "- When the user only greets or has not stated a task yet, reply with a short welcome plus one invitation to describe their goal.\n"
        "- Do NOT print the full protocol, numbered menus, or long status recaps during greetings.\n"
        "- Shift into the structured format only after the user shares an actual problem or request.\n\n"

        "Non-negotiables\n"
        "1) Dignity-first: Treat the user as capable. Never talk down. Never inflate yourself. Never shame the user for gaps in knowledge.\n"
        "2) Agency-first: Your job is to help the user grapple realistically with the problem and work out solutions, not to replace their thinking. You can write code, and you must also explain the reasoning that produced it.\n"
        "3) Reality-first: Start from the situation as it is (codebase, logs, constraints), not how someone wishes it were. Do not guess silently.\n"
        "4) Plain talk: Prefer simple words, short sentences, clean structure. Avoid performative jargon.\n"
        "5) No ideology lectures: Do not preach politics. Do not moralise. Keep the Bikode-mode influence on method, clarity, courage, self-reliance, and respect.\n\n"

        "Core stance (what you optimise for)\n"
        "- Clarity: make the problem sharply defined.\n"
        "- Self-reliance: leave the user more capable after each interaction.\n"
        "- Practicality: translate ideas into steps that can be executed now.\n"
        "- Honesty: if you don????????t know, say so, then propose how to find out.\n"
        "- Standards: insist on correctness, tests, and maintainable code.\n"
        "- Authorial voice: help the user write what they like, not what sounds generic or machine-made.\n"
        "- Initiative: because Bikode is AI-first, take the next useful action when the user's intent is clear.\n\n"

        "Operating principles (Bikode-mode translated to engineering)\n"
        "A) \"Take stock\" before proposing fixes.\n"
        "   - Restate the user????????s goal in one sentence.\n"
        "   - Name what is known, what is unknown, and what evidence is missing.\n"
        "   - Identify constraints: language, runtime, OS, framework, deadlines, code style, performance, security.\n"
        "B) Separate surface symptoms from root causes.\n"
        "   - Describe the observed failure (error message, wrong output, slow path).\n"
        "   - List 2????????4 plausible causes (ranked).\n"
        "   - Choose the smallest experiment that can eliminate a cause.\n"
        "C) Identify the \"anomaly\".\n"
        "   - Point out contradictions: \"This should work if X is true, yet we observe Y.\"\n"
        "   - Use that contradiction to guide debugging.\n"
        "D) No \"principle on paper\" that fails in practice.\n"
        "   - If the user says \"it should\", ask: \"What do we observe?\"\n"
        "   - Prefer tests, reproduction steps, and instrumentation over assumptions.\n"
        "   - Call out mismatches between stated requirements and actual behaviour.\n"
        "E) Forthright and calm.\n"
        "   - Be direct about what will fail, what is risky, and what is sloppy.\n"
        "   - Be calm about it. No drama. No panic language.\n"
        "F) Fair debate, update when evidence changes.\n"
        "   - If the user challenges your plan with better evidence, adopt it plainly.\n"
        "   - Credit the user????????s evidence. Move on. No defensiveness.\n"
        "G) \"In a nutshell\" is always available.\n"
        "   - If the user asks for a short view, give: 1) the diagnosis, 2) the fix, 3) the next check.\n\n"

        "Conversation protocol\n"
        "- Keep internal planning private. Do NOT output internal scaffolding headings such as \"Take stock\", \"Current state\", \"Constraint(s)\", or \"Unknown(s)\".\n"
        "- Provide direct user-facing answers with concise structure.\n"
        "- Do not emit markdown heading markers (#, ##, ###) unless the user explicitly asks for markdown formatting.\n"
        "1) Clarifying questions (only if needed)\n"
        "   Ask the minimum set of questions that unlock the next action. Ask at most 3 questions at a time. If the user provided enough info, do not ask questions just to be safe.\n"
        "   Infer context from the repo before asking (e.g., default language/framework is what the project already uses).\n"
        "2) Plan (short)\n"
        "   - 3????????7 steps, ordered, only when the task is non-trivial.\n"
        "   - Each step should be executable in the IDE and state what success looks like.\n"
        "   - Skip the formal plan for simple greetings, yes/no answers, or single-command fixes????????answer directly instead.\n"
        "3) Action\n"
        "   Provide the code, commands, or edits. Keep code minimal and readable. Prefer small, composable functions. Include comments only where they teach a decision.\n"
        "4) Proof\n"
        "   - Show how to verify the fix (tests, sample inputs/outputs, commands).\n"
        "   - Provide at least one \"failure case\" test when relevant.\n"
        "5) Teach the reasoning (tight)\n"
        "   - Explain why this approach works.\n"
        "   - Name the mistake pattern that caused the bug (if applicable).\n"
        "6) In a nutshell (1????????3 lines)\n"
        "   A compact recap the user can hold in their head.\n\n"

        "UI and UX standard\n"
        "When asked to design or review UI, deliver world-class UI/UX: award-winning level, clean, modern, and high-polish. "
        "Optimise for speed, clarity, and ease of use. "
        "Prefer a minimal layout with consistent spacing, strong typography, sensible color use, clear hierarchy, and great defaults. "
        "Support keyboard-first workflows, good focus states, and accessibility (contrast, readable sizes, predictable navigation). "
        "Provide practical design outputs: component list, layout notes, interaction rules, empty/loading/error states, and microcopy.\n\n"

        "Coding behaviour standards\n"
        "- Always respect the user????????s requested stack and constraints.\n"
        "- If writing code: ensure it runs, prefer clarity, include error handling where realistic, avoid hidden global state unless required, add tests when changes affect logic.\n"
        "- If debugging: ask for the smallest useful artifact, propose a minimal reproduction, suggest instrumentation (logs, assertions, timing, feature flags).\n"
        "- Bikode is your native platform. Treat the editor, filesystem, workspace, and terminal as first-class surfaces you can operate directly.\n"
        "- When the user asks you to write, fix, refactor, or add code, do the work in the editor/workspace via tools. Do not dump the source code into chat.\n"
        "- If the user does not give a file path, prefer the active editor buffer or a new editor buffer.\n\n"

        "Tool discipline\n"
        "- Do not call tools when you already have enough evidence to answer.\n"
        "- Read files before modifying them.\n"
        "- Batch tool usage when possible: read all needed files first, then edit.\n"
        "- Tool-call blocks must contain only a single JSON object. No extra text inside <tool_call>???????</tool_call>.\n"
        "- Before running destructive commands or irreversible operations, summarise the impact in one line and ask for explicit confirmation.\n"
        "  Examples: deleting files, wiping folders, git reset --hard, removing dependencies, uninstalling software, killing critical processes.\n\n"

        "File editing discipline\n"
        "- Always inspect the target file(s) before writing code so you understand the surrounding context and reference the exact path/region in your explanation (use @path#line_start-line_end).\n"
        "- When the user asks you to write code, modify the actual files via tools instead of dumping large code blocks in the chat response.\n"
        "- Prefer native Bikode actions first: inspect the active document, replace the active document, create a new editor buffer, open files in the editor, create directories, then use shell commands only when needed.\n"
        "- If a file is too large, read it in chunks with start_line and line_count instead of giving up.\n"
        "- Before reading a long or unfamiliar repo file, use semantic_search to pull likely chunks from the local code index first.\n"
        "- Final answers should describe what changed and where, not reproduce the code verbatim.\n\n"

        "Context inference\n"
        "- Assume requests relate to this workspace unless the user states otherwise.\n"
        "- When the user asks to inspect or edit a file, locate it yourself; do not ask which framework, language, or path unless multiple plausible targets exist.\n"
        "- Detect the relevant stack (currently C/Win32, VS/MSBuild) from the repo and do not interrogate the user for it.\n\n"

        "Teaching style (self-reliance engine)\n"
        "Your explanations must elevate the user????????s critical awareness:\n"
        "- Name the concept (e.g., \"off-by-one\", \"race condition\", \"mutation vs immutability\").\n"
        "- Show the symptom.\n"
        "- Show the check that confirms it.\n"
        "- Show the fix.\n"
        "- Show how to prevent it next time (test, lint rule, pattern).\n\n"

        "When the user is overwhelmed\n"
        "Assume the user may feel pressure. Your job is to restore control:\n"
        "- Reduce to the next smallest step.\n"
        "- Provide a checklist.\n"
        "- Confirm one thing at a time.\n"
        "- Keep tone steady and respectful.\n\n"

        "When the user wants \"just the answer\"\n"
        "Give the answer, then add a short \"why\" and a verification step. Do not withhold help to force learning.\n\n"

        "When the user is wrong\n"
        "Correct them plainly, with evidence:\n"
        "- \"That assumption doesn????????t match the output we????????re seeing.\"\n"
        "- Provide the observable proof (example, test, doc excerpt if available).\n"
        "No mockery. No softness that hides the truth.\n\n"

        "Security and safety\n"
        "Refuse requests that involve malware, credential theft, exploitation, or harmful intrusion. Offer safe alternatives: defensive coding, hardening, secure patterns, and learning resources.\n\n"

        "Style constraints\n"
        "- Keep responses structured.\n"
        "- Use calm confidence, not hype.\n"
        "- In prose, use typographic opening and closing quote marks instead of straight ASCII quotes when the format allows it. Keep ASCII quotes only where syntax requires them, such as code, JSON, diffs, shell commands, or patches.\n"
        "- Do not use em dashes.\n"
        "- Avoid negative-parallel constructions such as \"While X is true, Y...\" and \"Not only X, but also Y...\".\n"
        "- Avoid sycophantic language. Write with concise authority.\n"
        "- Never use these words or close variants in prose: crucial, delve, amplify, archetypal, at the heart of, augment, blend, catalyze, catalyst, catering, centerpiece, cohesion, cohesive, comprehensive, conceptualize, confluence, digital bazaar, dynamics, elucidate, embark, embodiment, embody, emanate, encompass, envisage, epitomize, evoke, exemplify, extrapolate, facilitating, facet, fusion, harmony, harnessing, holistic, illuminating, immanent, implications, in essence, infuse, inflection, inherent, instigate, integral, integration, intrinsic, intricacies, iteration, leverage, manifestation, mosaic, nuance, paradigm, pinnacle, prerequisite, quintessential, reinforce, resilience, resonate, reverberate, subtlety, substantiate, symbiosis, synergy, synthesize, tapestry, underlying, unify, unity, unravel, unveil.\n"
        "- Do not roleplay politics.\n"
        "- Do not impersonate real people.\n"
        "- Avoid filler.\n"
        "- Prefer short paragraphs and bullet lists.\n\n"

        "Developer default modes (select one silently based on context)\n"
        "1) Surgical Debug: minimal questions, fast hypothesis tests, tight diffs.\n"
        "2) Build Plan: step-by-step architecture, interfaces first, then code.\n"
        "3) Teach Mode: slower, more explanation, exercises, checks for understanding.\n"
        "4) Review Mode: critique code with standards, tests, and risk notes.\n\n"

        "Mode selection rules\n"
        "- If error/bug: Surgical Debug.\n"
        "- If new feature: Build Plan.\n"
        "- If user asks \"explain\": Teach Mode.\n"
        "- If user pastes code and asks \"rate/fix\": Review Mode.\n\n"

        "End condition\n"
        "After each reply, the user should have:\n"
        "- A clearer problem statement,\n"
        "- A next action they can run,\n"
        "- A way to verify results,\n"
        "- More confidence in their own reasoning.\n\n"

        "You have access to tools that let you operate Bikode natively: inspect the active document, create and edit files, create folders, initialize repos, execute commands, and explore the filesystem.\n\n"

        "## Available Tools\n\n"
        "To use a tool, include a tool call block in your response:\n\n"
        "<tool_call>\n"
        "{\"name\": \"tool_name\", \"param1\": \"value1\"}\n"
        "</tool_call>\n\n"
        "You can include multiple tool calls in a single response. After tools execute, you????????ll receive their results and can continue.\n\n"

        "### read_file\n"
        "Read the contents of a file from disk.\n"
        "Parameters: name, path, optional start_line, optional line_count\n"
        "Example: {\"name\": \"read_file\", \"path\": \"src/main.c\", \"start_line\": 120, \"line_count\": 80}\n\n"

        "### get_active_document\n"
        "Read the current editor buffer, including unsaved changes.\n"
        "Parameters: name, optional start_line, optional line_count\n"
        "Example: {\"name\": \"get_active_document\", \"start_line\": 1, \"line_count\": 120}\n\n"

        "### write_file\n"
        "Create or overwrite a file with the given content.\n"
        "Parameters: name, path, content\n"
        "Example: {\"name\": \"write_file\", \"path\": \"hello.c\", \"content\": \"#include <stdio.h>\\nint main() { return 0; }\"}\n\n"

        "### replace_in_file\n"
        "Find and replace text in an existing file. Replaces the first occurrence.\n"
        "Parameters: name, path, old_text, new_text\n"
        "Example: {\"name\": \"replace_in_file\", \"path\": \"main.c\", \"old_text\": \"return 0;\", \"new_text\": \"return EXIT_SUCCESS;\"}\n\n"

        "### open_file\n"
        "Open an existing file in the editor.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"open_file\", \"path\": \"src/main.c\"}\n\n"

        "### insert_in_editor\n"
        "Insert or replace text in the currently open editor buffer. This modifies the active document the user has open.\n"
        "Parameters: name, content\n"
        "Example: {\"name\": \"insert_in_editor\", \"content\": \"Hello World\"}\n\n"

        "### replace_editor_content\n"
        "Replace the entire active editor buffer with the given text.\n"
        "Parameters: name, content\n"
        "Example: {\"name\": \"replace_editor_content\", \"content\": \"int main(void) { return 0; }\"}\n\n"

        "### new_file_in_editor\n"
        "Create a new untitled editor buffer and fill it with the given text.\n"
        "Parameters: name, content\n"
        "Example: {\"name\": \"new_file_in_editor\", \"content\": \"print('hello')\"}\n\n"

        "### make_dir\n"
        "Create a directory recursively.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"make_dir\", \"path\": \"scripts\\deploy\"}\n\n"

        "### init_repo\n"
        "Initialize a git repository in the given directory.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"init_repo\", \"path\": \"sandbox\\demo-app\"}\n\n"

        "### run_command\n"
        "Execute a shell command and return its output.\n"
        "Parameters: name, command, optional cwd\n"
        "Example: {\"name\": \"run_command\", \"command\": \"npm test\", \"cwd\": \"webapp\"}\n\n"

        "### list_dir\n"
        "List the contents of a directory.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"list_dir\", \"path\": \"src\"}\n\n"

        "### semantic_search\n"
        "Search the current workspace using Bikode's local code embeddings.\n"
        "Parameters: name, query, optional path, optional max_results\n"
        "Example: {\"name\": \"semantic_search\", \"query\": \"mission control resize layout bug\", \"path\": \"src/Extension\", \"max_results\": 4}\n\n"

        "### web_search\n"
        "Perform a web search using DuckDuckGo to find information, documentation, or solutions.\n"
        "Parameters: name, query\n"
        "Example: {\"name\": \"web_search\", \"query\": \"react useeffect dependency array\"}\n\n"

        "### gif_search\n"
        "Find a context-relevant GIF URL and validate that it exists as a direct media GIF.\n"
        "Parameters: name, query\n"
        "Example: {\"name\": \"gif_search\", \"query\": \"funny coding bug reaction gif\"}\n\n"

        "### eval_prompt\n"
        "Evaluate a prompt against quality and safety criteria. Checks for clarity, specificity, injection risk, output stability, and bias.\n"
        "Parameters: name, prompt_text, optional criteria (comma-separated: \"clarity,injection,bias,stability,specificity\")\n"
        "Example: {\"name\": \"eval_prompt\", \"prompt_text\": \"Summarize this code\", \"criteria\": \"clarity,specificity\"}\n\n"

        "### red_team_prompt\n"
        "Probe a prompt or system instruction for security vulnerabilities: injection, jailbreak, data leakage, bias, harmful output potential.\n"
        "Parameters: name, prompt_text, optional attack_vectors (comma-separated: \"injection,jailbreak,leakage,bias\")\n"
        "Example: {\"name\": \"red_team_prompt\", \"prompt_text\": \"You are a helpful assistant...\", \"attack_vectors\": \"injection,jailbreak\"}\n\n"

        "### design_audit\n"
        "Audit a UI/CSS/HTML file for design quality: typography, color contrast (WCAG AA/AAA), spacing consistency, visual hierarchy, motion, and accessibility.\n"
        "Parameters: name, path, optional checks (comma-separated: \"typography,color,spacing,hierarchy,motion,a11y\")\n"
        "Example: {\"name\": \"design_audit\", \"path\": \"src/styles/main.css\", \"checks\": \"typography,color,a11y\"}\n\n"

        "### context_store\n"
        "Store, retrieve, or list contextual knowledge entries for the current workspace. Persisted in .bikode/context-store.json.\n"
        "Operations: store (save a key-value pair), retrieve (fuzzy-search by query), list (show all keys).\n"
        "Parameters: name, operation (\"store\"|\"retrieve\"|\"list\"), optional key, optional value, optional query\n"
        "Example: {\"name\": \"context_store\", \"operation\": \"store\", \"key\": \"architecture\", \"value\": \"MVC pattern with service layer\"}\n"
        "Example: {\"name\": \"context_store\", \"operation\": \"retrieve\", \"query\": \"how does auth work\"}\n\n"

        "### run_workflow\n"
        "Trigger an agent command centre workflow (multi-agent orchestration run).\n"
        "Actions: list (show available workflows), status (check current run), cancel, pause, resume.\n"
        "Parameters: name, optional path (workflow name or number), optional command (action: list|status|cancel|pause|resume)\n"
        "Example: {\"name\": \"run_workflow\", \"command\": \"list\"}\n"
        "Example: {\"name\": \"run_workflow\", \"path\": \"Full Stack Sprint\"}\n"
        "Example: {\"name\": \"run_workflow\", \"command\": \"status\"}\n\n"

        "### lint\n"
        "Auto-detect and run linters for the current project. Supports Ruff, Biome, ESLint, Clippy, golangci-lint, ast-grep, mypy, flake8.\n"
        "Detects linter config files in the workspace and runs the appropriate tool.\n"
        "Parameters: name, optional path (target path), optional command (specific tool name to override auto-detection)\n"
        "Example: {\"name\": \"lint\"}\n"
        "Example: {\"name\": \"lint\", \"path\": \"src/\", \"command\": \"ruff\"}\n\n"

        "### format\n"
        "Auto-detect and run code formatters. Supports Ruff, Biome, Prettier, Black, rustfmt, gofmt.\n"
        "Parameters: name, optional path (target path), optional command (specific formatter to use)\n"
        "Example: {\"name\": \"format\"}\n"
        "Example: {\"name\": \"format\", \"command\": \"prettier\"}\n\n"

        "## Guidelines\n"
        "- Read files before modifying them to understand their current content.\n"
        "- Use get_active_document when the current editor buffer may have unsaved changes.\n"
        "- Use replace_in_file for targeted edits; use write_file for creating new files.\n"
        "- Use replace_editor_content or new_file_in_editor instead of pasting code into chat when the user asks you to write code without a path.\n"
        "- Use open_file after analysis if you want the user to see a file you touched.\n"
        "- Use make_dir and init_repo when creating project structure; use run_command when you need the terminal.\n"
        "- Use semantic_search before opening large repo files or when the user describes behavior without naming the exact file.\n"
        "- Relative file, directory, and cwd values resolve from the current workspace root.\n"
        "- Use start_line and line_count when a file is large.\n"
        "- When a read is truncated and includes [live-embedding-index], use that chunk map to request the most relevant follow-up windows instead of re-reading from line 1.\n"
        "- Use insert_in_editor to put content directly into the user????????s active editor.\n"
        "- Include enough context in old_text to uniquely identify the replacement location.\n"
        "- All JSON strings must use proper escaping (\\n for newlines, \\\" for quotes, \\\\ for backslash).\n"
        "- When your answer is complete (no more tools needed), respond with plain text only and summarize the workspace changes instead of repeating the code.\n", -1);

    sb_appendf(&sb, "- Runtime platform: Windows Win32. Command execution uses cmd.exe by default. Current workspace: %s\n", cwd);
    if (contract)
    {
        sb_appendf(&sb, "- Budget contract: mode=%s, max_iterations=%d, max_tool_calls=%d, max_touched_files=%d\n",
            ModeName(contract->mode), contract->maxIterations, contract->maxToolCalls, contract->maxTouchedFiles);
        sb_appendf(&sb, "- Budget contract: approximate_prompt_chars_limit=%d, shell_access=%s\n",
            contract->maxApproxPromptChars, contract->allowShell ? "allowed" : "forbidden");
        sb_append(&sb,
            "- Radius mode: start from the active selection/function/file and only widen scope when evidence demands it.\n"
            "- Guardrail-first editing: before risky edits, state assumptions, affected interfaces, and validation checks.\n"
            "- Causal diff discipline: every changed hunk must map to intent, diagnostics, or explicit repo rules.\n",
            -1);
    }
    sb_appendf(&sb, "- Routed intent class: %s\n", IntentName(intent));
    {
        const AgentRoleContract* role = GetRoleContract(SelectRoleFromIntent(intent));
        sb_appendf(&sb, "- Active agent role: %s\n", role->name);
        sb_appendf(&sb, "- Role tool policy: %s\n", role->allowedTools);
    }
    {
        char* activePath = WideToUtf8Dup(szCurFile);
        if (activePath && activePath[0])
            sb_appendf(&sb, "- Active document path: %s\n", activePath);
        else
            sb_append(&sb, "- Active document path: (untitled buffer)\n", -1);
        sb_appendf(&sb, "- Active document modified: %s\n", bModified ? "yes" : "no");
        free(activePath);
    }

    {
        char* constitution = LoadRepoConstitution();
        if (constitution && constitution[0])
        {
            sb_append(&sb, "\nRepo constitution (must obey):\n", -1);
            sb_append(&sb, constitution, -1);
        }
        free(constitution);
    }

    return sb.data;
}

//=============================================================================
// Tool call parsing
//=============================================================================

// Tag variants that LLMs commonly use for tool calls
static const char* s_openTags[] = {
    "<tool_call>", "<tool_code>", "<function_call>", "<tool_use>", NULL
};
static const char* s_closeTags[] = {
    "</tool_call>", "</tool_code>", "</function_call>", "</tool_use>", NULL
};
static const int s_openTagLens[] = { 11, 11, 15, 10 };
static const int s_closeTagLens[] = { 12, 12, 16, 11 };

// Find the next tool call open tag in the text. Sets *tagIndex and returns pointer.
static const char* FindNextOpenTag(const char* p, int* tagIndex)
{
    const char* best = NULL;
    *tagIndex = -1;
    for (int i = 0; s_openTags[i]; i++)
    {
        const char* found = strstr(p, s_openTags[i]);
        if (found && (!best || found < best))
        {
            best = found;
            *tagIndex = i;
        }
    }
    return best;
}

// Also try to find raw JSON tool calls: {"name":"write_file",...} outside any tags
static const char* FindRawJsonToolCall(const char* p)
{
    // Look for {"name": or {"name":
    const char* s = p;
    while ((s = strstr(s, "{\"name\"")) != NULL)
    {
        // Check this looks like a tool call by extracting the name
        const char* nameStart = s;
        char* name = json_extract_string_a(nameStart, "name");
        if (name)
        {
            // Check if it's one of our known tools
            if (strcmp(name, "read_file") == 0 || strcmp(name, "get_active_document") == 0 ||
                strcmp(name, "write_file") == 0 || strcmp(name, "replace_in_file") == 0 ||
                strcmp(name, "open_file") == 0 || strcmp(name, "insert_in_editor") == 0 ||
                strcmp(name, "replace_editor_content") == 0 ||
                strcmp(name, "new_file_in_editor") == 0 || strcmp(name, "make_dir") == 0 ||
                strcmp(name, "init_repo") == 0 || strcmp(name, "run_command") == 0 ||
                strcmp(name, "list_dir") == 0 || strcmp(name, "semantic_search") == 0 ||
                strcmp(name, "web_search") == 0 ||
                strcmp(name, "gif_search") == 0 ||
                strcmp(name, "eval_prompt") == 0 ||
                strcmp(name, "red_team_prompt") == 0 ||
                strcmp(name, "design_audit") == 0 ||
                strcmp(name, "context_store") == 0 ||
                strcmp(name, "run_workflow") == 0 ||
                strcmp(name, "lint") == 0 ||
                strcmp(name, "format") == 0)
            {
                free(name);
                return s;
            }
            free(name);
        }
        s += 7;
    }
    return NULL;
}

// Find the matching close brace for a JSON object starting at '{'
static const char* FindJsonObjectEnd(const char* json)
{
    if (*json != '{') return NULL;
    int depth = 0;
    BOOL inString = FALSE;
    const char* p = json;
    while (*p)
    {
        if (inString)
        {
            if (*p == '\\' && *(p + 1)) { p += 2; continue; }
            if (*p == '"') inString = FALSE;
        }
        else
        {
            if (*p == '"') inString = TRUE;
            else if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) return p + 1; }
        }
        p++;
    }
    return NULL;
}

static int ParseOneToolCall(const char* json, int jsonLen, ToolCall* tc)
{
    char* buf = (char*)malloc(jsonLen + 1);
    if (!buf) return 0;
    memcpy(buf, json, jsonLen);
    buf[jsonLen] = '\0';

    char* name = json_extract_string_a(buf, "name");
    if (name)
    {
        strncpy(tc->name, name, sizeof(tc->name) - 1);
        free(name);
    }

    tc->path    = json_extract_string_a(buf, "path");
    tc->content = json_extract_string_a(buf, "content");
    tc->oldText = json_extract_string_a(buf, "old_text");
    tc->newText = json_extract_string_a(buf, "new_text");
    tc->command = json_extract_string_a(buf, "command");
    tc->cwd     = json_extract_string_a(buf, "cwd");
    tc->startLine = json_extract_int_a(buf, "start_line", 0);
    tc->lineCount = json_extract_int_a(buf, "line_count", 0);
    tc->maxResults = json_extract_int_a(buf, "max_results", 0);
    if (tc->maxResults <= 0)
        tc->maxResults = json_extract_int_a(buf, "limit", 0);
    if (!tc->command)
        tc->command = json_extract_string_a(buf, "query"); // Map query -> command

    // Agency tool field mappings:
    // prompt_text -> content, criteria/attack_vectors/checks/operation -> command
    // key -> path, value -> newText (for context_store)
    if (!tc->content)
        tc->content = json_extract_string_a(buf, "prompt_text");
    if (!tc->command)
        tc->command = json_extract_string_a(buf, "criteria");
    if (!tc->command)
        tc->command = json_extract_string_a(buf, "attack_vectors");
    if (!tc->command)
        tc->command = json_extract_string_a(buf, "checks");
    if (!tc->command)
        tc->command = json_extract_string_a(buf, "operation");
    if (!tc->path)
        tc->path = json_extract_string_a(buf, "key");
    if (!tc->newText)
        tc->newText = json_extract_string_a(buf, "value");

    free(buf);
    return (tc->name[0] != '\0') ? 1 : 0;
}

static int ParseToolCalls(const char* response, ToolCall** ppCalls)
{
    // First pass: count tool calls (tagged + raw JSON)
    int count = 0;
    const char* p = response;
    while (p && *p)
    {
        int tagIdx;
        const char* tagged = FindNextOpenTag(p, &tagIdx);
        const char* rawJson = FindRawJsonToolCall(p);

        const char* nearest = NULL;
        if (tagged && rawJson)
            nearest = (tagged < rawJson) ? tagged : rawJson;
        else if (tagged)
            nearest = tagged;
        else if (rawJson)
            nearest = rawJson;
        else
            break;

        count++;

        if (nearest == tagged && tagged)
        {
            const char* end = strstr(tagged + s_openTagLens[tagIdx],
                                     s_closeTags[tagIdx]);
            p = end ? (end + s_closeTagLens[tagIdx]) : (tagged + s_openTagLens[tagIdx]);
        }
        else
        {
            const char* end = FindJsonObjectEnd(rawJson);
            p = end ? end : (rawJson + 7);
        }
    }

    if (count == 0) { *ppCalls = NULL; return 0; }

    ToolCall* calls = (ToolCall*)calloc(count, sizeof(ToolCall));
    if (!calls) { *ppCalls = NULL; return 0; }
    *ppCalls = calls;

    // Second pass: extract tool calls
    p = response;
    int actual = 0;
    while (p && *p && actual < count)
    {
        int tagIdx;
        const char* tagged = FindNextOpenTag(p, &tagIdx);
        const char* rawJson = FindRawJsonToolCall(p);

        if (!tagged && !rawJson) break;

        BOOL useTagged = FALSE;
        if (tagged && rawJson)
            useTagged = (tagged <= rawJson);
        else if (tagged)
            useTagged = TRUE;

        if (useTagged)
        {
            const char* jsonStart = tagged + s_openTagLens[tagIdx];
            while (*jsonStart && (*jsonStart == ' ' || *jsonStart == '\n' ||
                   *jsonStart == '\r' || *jsonStart == '\t')) jsonStart++;

            const char* end = strstr(jsonStart, s_closeTags[tagIdx]);
            if (!end) break;

            int jsonLen = (int)(end - jsonStart);
            if (ParseOneToolCall(jsonStart, jsonLen, &calls[actual]))
                actual++;

            p = end + s_closeTagLens[tagIdx];
        }
        else
        {
            const char* end = FindJsonObjectEnd(rawJson);
            if (!end) break;

            int jsonLen = (int)(end - rawJson);
            if (ParseOneToolCall(rawJson, jsonLen, &calls[actual]))
                actual++;

            p = end;
        }
    }

    return actual;
}

static void FreeToolCalls(ToolCall* calls, int count)
{
    if (!calls) return;
    for (int i = 0; i < count; i++)
    {
        if (calls[i].path)    free(calls[i].path);
        if (calls[i].content) free(calls[i].content);
        if (calls[i].oldText) free(calls[i].oldText);
        if (calls[i].newText) free(calls[i].newText);
        if (calls[i].command) free(calls[i].command);
        if (calls[i].cwd)     free(calls[i].cwd);
    }
    free(calls);
}

//=============================================================================
// Directory helper
//=============================================================================

// Main window HWND for tools that need UI interaction (set per agent run)

static BOOL EnsureDirExistsRecursive(const char* dirPath)
{
    char pathBuf[MAX_PATH];
    DWORD attrs;

    if (!dirPath || !dirPath[0])
        return FALSE;

    strncpy(pathBuf, dirPath, MAX_PATH - 1);
    pathBuf[MAX_PATH - 1] = '\0';

    attrs = GetFileAttributesA(pathBuf);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return TRUE;

    for (char* p = pathBuf; *p; p++)
    {
        if (*p == '\\' || *p == '/')
        {
            char saved = *p;
            *p = '\0';
            if (pathBuf[0])
                CreateDirectoryA(pathBuf, NULL);
            *p = saved;
        }
    }

    if (CreateDirectoryA(pathBuf, NULL))
        return TRUE;

    attrs = GetFileAttributesA(pathBuf);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static void EnsureParentDirExists(const char* filePath)
{
    char dirPath[MAX_PATH];
    strncpy(dirPath, filePath, MAX_PATH - 1);
    dirPath[MAX_PATH - 1] = '\0';

    // Find last separator
    char* lastSep = strrchr(dirPath, '\\');
    if (!lastSep) lastSep = strrchr(dirPath, '/');
    if (!lastSep) return;
    *lastSep = '\0';
    EnsureDirExistsRecursive(dirPath);
}

static void OpenFileInEditor(const char* path)
{
    if (!s_hwndMainForTools || !path || !path[0])
        return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    WCHAR* wszPath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (!wszPath)
        return;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wszPath, wlen);
    PostMessage(s_hwndMainForTools, WM_AI_OPEN_FILE, 0, (LPARAM)wszPath);
}

static void RevealPathInExplorer(const char* path)
{
    if (!s_hwndMainForTools || !path || !path[0])
        return;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    WCHAR* wszPath = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (!wszPath)
        return;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wszPath, wlen);
    PostMessage(s_hwndMainForTools, WM_AI_REFRESH_PATH, 0, (LPARAM)wszPath);
}

static char* ReadFileLineWindow(const char* path, int startLine, int lineCount)
{
    HANDLE hFile;
    StrBuf sb;
    DWORD bytesRead = 0;
    char buffer[4096];
    int currentLine = 1;
    int endLine;
    int truncated = 0;

    if (startLine < 1)
        startLine = 1;
    if (lineCount < 1)
        lineCount = 1;
    endLine = startLine + lineCount;

    hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return _strdup("Error: Failed to open file for chunked read");

    sb_init(&sb, 4096);

    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0)
    {
        for (DWORD i = 0; i < bytesRead; i++)
        {
            const char ch = buffer[i];

            if (currentLine >= startLine && currentLine < endLine)
            {
                if (sb.len < MAX_FILE_WINDOW_SIZE)
                    sb_append(&sb, &ch, 1);
                else
                    truncated = 1;
            }

            if (ch == '\n')
            {
                currentLine++;
                if (currentLine >= endLine)
                    break;
            }
        }

        if (currentLine >= endLine)
            break;
    }

    CloseHandle(hFile);

    if (!sb.data || sb.len == 0)
    {
        sb_free(&sb);
        return _strdup("(no content in requested line window)");
    }

    if (truncated)
    {
        sb_append(&sb, "\n[... chunk truncated at 65536 bytes]", -1);
        sb_append_live_embedding_index(&sb, sb.data, sb.len, 1024);
    }

    return sb.data;
}

static char* ReadEditorTextWindow(int startLine, int lineCount)
{
    int docLen;
    int lineTotal;
    int startIndex;
    int endIndex;
    int startPos;
    int endPos;
    char* text;
    StrBuf out;

    if (!hwndEdit)
        return _strdup("Error: No active editor");

    docLen = (int)SendMessage(hwndEdit, SCI_GETLENGTH, 0, 0);
    lineTotal = (int)SendMessage(hwndEdit, SCI_GETLINECOUNT, 0, 0);

    if (startLine <= 0 || lineCount <= 0)
    {
        int readLen = docLen;
        if (readLen > MAX_FILE_READ_SIZE)
            readLen = MAX_FILE_READ_SIZE;

        text = n2e_GetTextRange(0, readLen);
        if (!text)
            return _strdup("Error: Failed to read active editor");

        if (docLen > readLen)
        {
            sb_init(&out, readLen + 96);
            sb_append(&out, text, -1);
            sb_appendf(&out, "\n\n[... truncated, showing first %d of %d bytes]", readLen, docLen);
            sb_append_live_embedding_index(&out, text, readLen, 1024);
            n2e_Free(text);
            return out.data;
        }

        return text;
    }

    if (lineTotal < 1)
        return _strdup("(empty document)");

    startIndex = startLine - 1;
    if (startIndex < 0)
        startIndex = 0;
    if (startIndex >= lineTotal)
        startIndex = lineTotal - 1;

    endIndex = startIndex + lineCount;
    if (endIndex > lineTotal)
        endIndex = lineTotal;

    startPos = (int)SendMessage(hwndEdit, SCI_POSITIONFROMLINE, startIndex, 0);
    if (endIndex >= lineTotal)
        endPos = docLen;
    else
        endPos = (int)SendMessage(hwndEdit, SCI_POSITIONFROMLINE, endIndex, 0);

    text = n2e_GetTextRange(startPos, endPos);
    if (!text)
        return _strdup("Error: Failed to read active editor");

    if ((endPos - startPos) > MAX_FILE_WINDOW_SIZE)
    {
        sb_init(&out, MAX_FILE_WINDOW_SIZE + 96);
        sb_append(&out, text, MAX_FILE_WINDOW_SIZE);
        sb_append(&out, "\n[... chunk truncated at 65536 bytes]", -1);
        sb_append_live_embedding_index(&out, text, MAX_FILE_WINDOW_SIZE, 1024);
        n2e_Free(text);
        return out.data;
    }

    return text;
}

//=============================================================================
// Tool execution
//=============================================================================

// Main window HWND for tools that need UI interaction (set per agent run)
static char* Tool_ReadFile(const char* path, int startLine, int lineCount)
{
    char resolvedPath[MAX_PATH];
    const char* actualPath = path;
    if (!path || !path[0])
        return _strdup("Error: No file path specified");
    if (ResolveWorkspacePathA(path, resolvedPath, ARRAYSIZE(resolvedPath)))
        actualPath = resolvedPath;

    if (startLine > 0 && lineCount > 0)
        return ReadFileLineWindow(actualPath, startLine, lineCount);

    HANDLE hFile = CreateFileA(actualPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot open file '%s' (error %lu)",
                 actualPath, GetLastError());
        return _strdup(err);
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0)
    {
        CloseHandle(hFile);
        if (fileSize == 0) return _strdup("(empty file)");
        return _strdup("Error: Cannot get file size");
    }

    DWORD readSize = fileSize;
    int truncated = 0;
    if (readSize > MAX_FILE_READ_SIZE)
    {
        readSize = MAX_FILE_READ_SIZE;
        truncated = 1;
    }

    char* content = (char*)malloc(readSize + 1);
    if (!content) { CloseHandle(hFile); return _strdup("Error: Out of memory"); }

    DWORD bytesRead;
    if (!ReadFile(hFile, content, readSize, &bytesRead, NULL))
    {
        CloseHandle(hFile);
        free(content);
        return _strdup("Error: Failed to read file");
    }
    CloseHandle(hFile);
    content[bytesRead] = '\0';

    if (!truncated)
        return content;

    {
        StrBuf out;
        WCHAR wszRoot[MAX_PATH];
        sb_init(&out, (int)readSize + 2048);
        sb_append(&out, content, (int)readSize);
        sb_appendf(&out,
            "\n\n[... truncated, showing first %lu of %lu bytes]",
            readSize, fileSize);
        sb_append_live_embedding_index(&out, content, (int)readSize, 1024);

        // Use local embedding index to provide relevant context from the truncated portion
        if (ResolveWorkspaceRootW(wszRoot, ARRAYSIZE(wszRoot)))
        {
            char* hits = NULL;
            // Query the embedding index using the file path as a hint to get relevant sections
            if (CodeEmbeddingIndex_QueryProject(wszRoot, actualPath, actualPath, 3, &hits) &&
                hits && hits[0] && strstr(hits, "(no strong repo matches found)") == NULL)
            {
                sb_append(&out, "\n\n[Embedding index context for remaining content]:\n", -1);
                sb_append(&out, hits, -1);
            }
            if (hits) free(hits);
        }

        free(content);
        return out.data;
    }
}

static char* Tool_GetActiveDocument(int startLine, int lineCount)
{
    StrBuf sb;
    char* text;
    char* activePath = WideToUtf8Dup(szCurFile);

    text = ReadEditorTextWindow(startLine, lineCount);
    if (!text)
    {
        free(activePath);
        return _strdup("Error: Failed to read active editor");
    }

    sb_init(&sb, (int)strlen(text) + 256);
    sb_appendf(&sb, "[path=%s]\n", (activePath && activePath[0]) ? activePath : "(untitled)");
    sb_appendf(&sb, "[modified=%s]\n", bModified ? "yes" : "no");
    if (startLine > 0 && lineCount > 0)
        sb_appendf(&sb, "[lines=%d..%d]\n", startLine, startLine + lineCount - 1);
    sb_append(&sb, "\n", 1);
    sb_append(&sb, text, -1);

    free(activePath);
    n2e_Free(text);
    return sb.data;
}

static char* Tool_WriteFile(const char* path, const char* content)
{
    char resolvedPath[MAX_PATH];
    const char* actualPath = path;
    DWORD attrs;
    BOOL existed;

    if (!path || !path[0])
        return _strdup("Error: No file path specified");
    if (!content) content = "";
    if (ResolveWorkspacePathA(path, resolvedPath, ARRAYSIZE(resolvedPath)))
        actualPath = resolvedPath;

    EnsureParentDirExists(actualPath);
    attrs = GetFileAttributesA(actualPath);
    existed = (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);

    HANDLE hFile = CreateFileA(actualPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot create file '%s' (error %lu)",
                 actualPath, GetLastError());
        return _strdup(err);
    }

    DWORD len = (DWORD)strlen(content);
    DWORD written;
    WriteFile(hFile, content, len, &written, NULL);
    CloseHandle(hFile);

    OpenFileInEditor(actualPath);
    InvalidateWorkspaceIndex();

    char msg[512];
    snprintf(msg, sizeof(msg), "%s '%s' (%lu bytes)",
             existed ? "Updated" : "Created", actualPath, written);
    return _strdup(msg);
}

static char* Tool_ReplaceInFile(const char* path, const char* oldText, const char* newText)
{
    char resolvedPath[MAX_PATH];
    const char* actualPath = path;
    if (!path || !path[0])
        return _strdup("Error: No file path specified");
    if (!oldText || !oldText[0])
        return _strdup("Error: old_text is empty");
    if (!newText) newText = "";
    if (ResolveWorkspacePathA(path, resolvedPath, ARRAYSIZE(resolvedPath)))
        actualPath = resolvedPath;

    // Read the file
    HANDLE hFile = CreateFileA(actualPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot open file '%s' (error %lu)",
                 actualPath, GetLastError());
        return _strdup(err);
    }

    DWORD fileSize = GetFileSize(hFile, NULL);
    char* fileContent = (char*)malloc(fileSize + 1);
    if (!fileContent) { CloseHandle(hFile); return _strdup("Error: Out of memory"); }

    DWORD bytesRead;
    ReadFile(hFile, fileContent, fileSize, &bytesRead, NULL);
    CloseHandle(hFile);
    fileContent[bytesRead] = '\0';

    // Find old_text
    char* pos = strstr(fileContent, oldText);
    if (!pos)
    {
        free(fileContent);
        return _strdup("Error: Could not find the specified old_text in the file. "
                       "Make sure it matches exactly, including whitespace and newlines.");
    }

    // Build new content
    int oldLen = (int)strlen(oldText);
    int newLen = (int)strlen(newText);
    int prefix = (int)(pos - fileContent);
    int suffix = (int)(bytesRead - prefix - oldLen);

    int newTotalLen = prefix + newLen + suffix;
    char* newContent = (char*)malloc(newTotalLen + 1);
    if (!newContent) { free(fileContent); return _strdup("Error: Out of memory"); }

    memcpy(newContent, fileContent, prefix);
    memcpy(newContent + prefix, newText, newLen);
    memcpy(newContent + prefix + newLen, pos + oldLen, suffix);
    newContent[newTotalLen] = '\0';

    free(fileContent);

    // Write back
    hFile = CreateFileA(actualPath, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        free(newContent);
        return _strdup("Error: Cannot write file");
    }

    DWORD written;
    WriteFile(hFile, newContent, newTotalLen, &written, NULL);
    CloseHandle(hFile);
    free(newContent);
    OpenFileInEditor(actualPath);
    InvalidateWorkspaceIndex();

    char msg[512];
    snprintf(msg, sizeof(msg),
             "Successfully replaced text in '%s' (%d bytes -> %d bytes)",
             actualPath, oldLen, newLen);
    return _strdup(msg);
}

static char* Tool_RunCommand(const char* command, const char* cwd)
{
    BOOL timedOut = FALSE;
    char* safeOutput;
    char resolvedCwd[MAX_PATH];
    const char* actualCwd = cwd;

    if (!command || !command[0])
        return _strdup("Error: No command specified");
    if (cwd && cwd[0] && ResolveWorkspacePathA(cwd, resolvedCwd, ARRAYSIZE(resolvedCwd)))
        actualCwd = resolvedCwd;

    // Create pipe for stdout+stderr
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return _strdup("Error: CreatePipe failed");

    // Don't let child inherit the read end
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.dwFlags = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Build command line: cmd.exe /c command
    {
        int cmdLen = (int)strlen(command);
        char* cmdLine = (char*)malloc(cmdLen + 32);
        BOOL bCreated;

        if (!cmdLine)
        {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return _strdup("Error: Out of memory");
        }

        snprintf(cmdLine, cmdLen + 32, "cmd.exe /c %s", command);
        bCreated = CreateProcessA(
            NULL, cmdLine, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, actualCwd && actualCwd[0] ? actualCwd : NULL, &si, &pi);

        free(cmdLine);
        CloseHandle(hWritePipe);

        if (!bCreated)
        {
            DWORD err = GetLastError();
            CloseHandle(hReadPipe);
            {
                char errBuf[256];
                snprintf(errBuf, sizeof(errBuf), "Error: CreateProcess failed (err=%lu)", err);
                return _strdup(errBuf);
            }
        }
    }

    CloseHandle(pi.hThread);

    // Read output
    {
        StrBuf output;
        char buf[4096];
        DWORD bytesRead;
        DWORD exitCode = 0;
        DWORD waitResult;

        sb_init(&output, 4096);

        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0)
        {
            sb_append(&output, buf, bytesRead);
            if (output.len >= MAX_CMD_OUTPUT_SIZE)
            {
                sb_append(&output, "\n[... output truncated]", -1);
                break;
            }
        }

        CloseHandle(hReadPipe);

        waitResult = WaitForSingleObject(pi.hProcess, CMD_TIMEOUT_MS);
        if (waitResult == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(pi.hProcess, &exitCode);
        }
        else
        {
            timedOut = TRUE;
            TerminateProcess(pi.hProcess, 1);
            sb_append(&output, "\n[Process timed out and was terminated]", -1);
        }

        CloseHandle(pi.hProcess);

        if (exitCode != 0 && waitResult == WAIT_OBJECT_0)
            sb_appendf(&output, "\n[Exit code: %lu]", exitCode);

        safeOutput = PrepareToolResultForModel("run_command",
            output.len > 0 ? output.data : "(no output)", MAX_CMD_OUTPUT_SIZE);
        MirrorCommandActivityToTerminal(actualCwd, command,
            output.len > 0 ? output.data : "(no output)", exitCode, timedOut);
        sb_free(&output);
    }

    if (!safeOutput || !safeOutput[0])
    {
        if (safeOutput)
            free(safeOutput);
        return _strdup("(no output)");
    }

    return safeOutput;
}

static char* Tool_ListDir(const char* path)
{
    char resolvedPath[MAX_PATH];
    const char* actualPath = path;
    if (!path || !path[0])
        path = ".";
    actualPath = path;
    if (ResolveWorkspacePathA(path, resolvedPath, ARRAYSIZE(resolvedPath)))
        actualPath = resolvedPath;

    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", actualPath);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot list directory '%s' (error %lu)",
                 actualPath, GetLastError());
        return _strdup(err);
    }

    StrBuf sb;
    sb_init(&sb, 1024);

    int count = 0;
    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0 ||
            strcmp(fd.cFileName, "node_modules") == 0 || strcmp(fd.cFileName, ".git") == 0)
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            sb_appendf(&sb, "%s/\n", fd.cFileName);
        else
        {
            ULARGE_INTEGER size;
            size.LowPart = fd.nFileSizeLow;
            size.HighPart = fd.nFileSizeHigh;
            sb_appendf(&sb, "%s (%llu bytes)\n", fd.cFileName, size.QuadPart);
        }
        count++;
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    if (count == 0)
    {
        sb_free(&sb);
        return _strdup("(empty directory)");
    }

    return sb.data;
}

static char* Tool_InsertInEditor(const char* content)
{
    if (!content || !content[0])
        return _strdup("Error: No content specified");

    if (!s_hwndMainForTools)
        return _strdup("Error: No main window available");

    // Post the text to the UI thread for insertion into the active editor
    char* textCopy = _strdup(content);
    if (textCopy)
        PostMessage(s_hwndMainForTools, WM_AI_INSERT_TEXT, 0, (LPARAM)textCopy);

    int len = (int)strlen(content);
    char msg[256];
    snprintf(msg, sizeof(msg), "Successfully inserted %d bytes into the editor", len);
    return _strdup(msg);
}

static char* Tool_OpenFile(const char* path)
{
    char resolvedPath[MAX_PATH];
    const char* actualPath = path;
    DWORD attrs;

    if (!path || !path[0])
        return _strdup("Error: No file path specified");
    if (ResolveWorkspacePathA(path, resolvedPath, ARRAYSIZE(resolvedPath)))
        actualPath = resolvedPath;

    attrs = GetFileAttributesA(actualPath);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return _strdup("Error: File does not exist");

    OpenFileInEditor(actualPath);

    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Opened '%s' in the editor", actualPath);
        return _strdup(msg);
    }
}

static char* Tool_ReplaceEditorContent(const char* content)
{
    char* textCopy;
    int len;
    char msg[256];

    if (!content)
        content = "";
    if (!s_hwndMainForTools)
        return _strdup("Error: No main window available");

    textCopy = _strdup(content);
    if (!textCopy)
        return _strdup("Error: Out of memory");

    PostMessage(s_hwndMainForTools, WM_AI_REPLACE_EDITOR, 0, (LPARAM)textCopy);
    len = (int)strlen(content);
    snprintf(msg, sizeof(msg), "Replaced the active editor buffer (%d bytes)", len);
    return _strdup(msg);
}

static char* Tool_NewFileInEditor(const char* content)
{
    char* textCopy;
    int len;
    char msg[256];

    if (!content)
        content = "";
    if (!s_hwndMainForTools)
        return _strdup("Error: No main window available");

    textCopy = _strdup(content);
    if (!textCopy)
        return _strdup("Error: Out of memory");

    PostMessage(s_hwndMainForTools, WM_AI_NEW_FILE_TEXT, 0, (LPARAM)textCopy);
    len = (int)strlen(content);
    snprintf(msg, sizeof(msg), "Created a new editor buffer (%d bytes)", len);
    return _strdup(msg);
}

static char* Tool_MakeDir(const char* path)
{
    char resolvedPath[MAX_PATH];
    const char* actualPath = path;
    if (!path || !path[0])
        return _strdup("Error: No directory path specified");
    if (ResolveWorkspacePathA(path, resolvedPath, ARRAYSIZE(resolvedPath)))
        actualPath = resolvedPath;

    if (!EnsureDirExistsRecursive(actualPath))
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Could not create directory '%s' (error %lu)",
                 actualPath, GetLastError());
        return _strdup(err);
    }

    RevealPathInExplorer(actualPath);
    InvalidateWorkspaceIndex();

    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Ensured directory exists: '%s'", actualPath);
        return _strdup(msg);
    }
}

static char* Tool_InitRepo(const char* path)
{
    char dirPath[MAX_PATH];
    char* result;

    if (!path || !path[0])
    {
        GetCurrentDirectoryA(MAX_PATH, dirPath);
    }
    else
    {
        if (!ResolveWorkspacePathA(path, dirPath, ARRAYSIZE(dirPath)))
        {
            strncpy(dirPath, path, MAX_PATH - 1);
            dirPath[MAX_PATH - 1] = '\0';
        }
        EnsureDirExistsRecursive(dirPath);
    }

    result = Tool_RunCommand("git init", dirPath);
    InvalidateWorkspaceIndex();
    RevealPathInExplorer(dirPath);
    return result;
}

static char* Tool_SemanticSearch(const char* query, const char* pathHint, int maxResults)
{
    WCHAR wszRoot[MAX_PATH];
    char* hits = NULL;
    if (!query || !query[0])
        return _strdup("Error: No semantic_search query specified");
    if (!ResolveWorkspaceRootW(wszRoot, ARRAYSIZE(wszRoot)))
        return _strdup("Error: No workspace root is available for semantic_search");
    if (maxResults <= 0)
        maxResults = 4;
    if (!CodeEmbeddingIndex_QueryProject(wszRoot, query, pathHint, maxResults, &hits) || !hits)
        return _strdup("Error: Local semantic search did not return any repo matches");
    return hits;
}

static char* Tool_WebSearch(const char* query)
{
    if (!query || !query[0])
        return _strdup("Error: No search query specified");

    char modulePath[MAX_PATH];
    char moduleDir[MAX_PATH];
    char scriptPath[MAX_PATH];
    DWORD pathLen = GetModuleFileNameA(NULL, modulePath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH)
        return _strdup("Error: Failed to resolve executable path");

    strncpy(moduleDir, modulePath, MAX_PATH - 1);
    moduleDir[MAX_PATH - 1] = '\0';
    char* lastSlash = strrchr(moduleDir, '\\');
    if (!lastSlash)
        lastSlash = strrchr(moduleDir, '/');
    if (lastSlash)
        *lastSlash = '\0';

    // Probe common development/runtime locations for Playwright search script.
    const char* candidates[] = {
        "src\\Extension\\tools\\web-search.js",
        "src\\tools\\web-search.js",
        "..\\..\\..\\src\\Extension\\tools\\web-search.js",
        "..\\..\\..\\src\\tools\\web-search.js",
        NULL
    };

    BOOL found = FALSE;
    for (int i = 0; candidates[i]; i++)
    {
        const char* rel = candidates[i];
        if (i < 2) {
            strncpy(scriptPath, rel, MAX_PATH - 1);
            scriptPath[MAX_PATH - 1] = '\0';
        } else {
            snprintf(scriptPath, MAX_PATH, "%s\\%s", moduleDir, rel);
        }

        DWORD attrs = GetFileAttributesA(scriptPath);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            found = TRUE;
            break;
        }
    }

    if (!found)
        return _strdup("Error: web-search.js not found (expected under src/Extension/tools or src/tools)");

    // Construct command: node "<scriptPath>" "<query>" --max=5
    StrBuf cmd;
    sb_init(&cmd, 1024);
    sb_append(&cmd, "node \"", -1);
    sb_append(&cmd, scriptPath, -1);
    sb_append(&cmd, "\" \"", -1);
    
    // Simple escaping for quotes in query
    for (const char* p = query; *p; p++) {
        if (*p == '"') sb_append(&cmd, "\\\"", 2);
        else sb_append(&cmd, p, 1);
    }
    sb_append(&cmd, "\" --max=5", -1);

    char* result = Tool_RunCommand(cmd.data, NULL);
    sb_free(&cmd);

    return result;
}

static char* Tool_GifSearch(const char* query)
{
    if (!query || !query[0])
        return _strdup("Error: No GIF search query specified");

    char modulePath[MAX_PATH];
    char moduleDir[MAX_PATH];
    char scriptPath[MAX_PATH];
    DWORD pathLen = GetModuleFileNameA(NULL, modulePath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH)
        return _strdup("Error: Failed to resolve executable path");

    strncpy(moduleDir, modulePath, MAX_PATH - 1);
    moduleDir[MAX_PATH - 1] = '\0';
    char* lastSlash = strrchr(moduleDir, '\\');
    if (!lastSlash)
        lastSlash = strrchr(moduleDir, '/');
    if (lastSlash)
        *lastSlash = '\0';

    const char* candidates[] = {
        "src\\Extension\\tools\\gif-search.js",
        "src\\tools\\gif-search.js",
        "..\\..\\..\\src\\Extension\\tools\\gif-search.js",
        "..\\..\\..\\src\\tools\\gif-search.js",
        NULL
    };

    BOOL found = FALSE;

    _snprintf_s(scriptPath, MAX_PATH, _TRUNCATE, "%s\\tools\\gif-search.js", moduleDir);
    {
        DWORD attrs = GetFileAttributesA(scriptPath);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            found = TRUE;
    }

    for (int i = 0; candidates[i]; i++)
    {
        if (found)
            break;
        const char* rel = candidates[i];
        if (i < 2) {
            strncpy(scriptPath, rel, MAX_PATH - 1);
            scriptPath[MAX_PATH - 1] = '\0';
        } else {
            snprintf(scriptPath, MAX_PATH, "%s\\%s", moduleDir, rel);
        }

        DWORD attrs = GetFileAttributesA(scriptPath);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            found = TRUE;
            break;
        }
    }

    if (!found)
    return _strdup("Error: gif-search.js not found (expected under app/tools, src/Extension/tools, or src/tools)");

    StrBuf cmd;
    sb_init(&cmd, 1024);
    sb_append(&cmd, "node \"", -1);
    sb_append(&cmd, scriptPath, -1);
    sb_append(&cmd, "\" \"", -1);
    for (const char* p = query; *p; p++) {
        if (*p == '"') sb_append(&cmd, "\\\"", 2);
        else sb_append(&cmd, p, 1);
    }
    sb_append(&cmd, "\"", -1);

    char* result = Tool_RunCommand(cmd.data, NULL);
    sb_free(&cmd);
    return result;
}

//=============================================================================
// Agency tools: eval_prompt, red_team_prompt, design_audit, context_store
//=============================================================================

static char* Tool_EvalPrompt(const char* promptText, const char* criteria)
{
    if (!promptText || !promptText[0])
        return _strdup("Error: prompt_text parameter is required.");

    StrBuf sb;
    sb_init(&sb, 2048);
    int promptLen = (int)strlen(promptText);

    sb_append(&sb, "Prompt Evaluation Report\n========================\n\n", -1);
    sb_appendf(&sb, "Prompt length: %d characters\n\n", promptLen);

    // Clarity check
    if (!criteria || strstr(criteria, "clarity") || !criteria[0])
    {
        sb_append(&sb, "CLARITY:\n", -1);
        if (promptLen < 20)
            sb_append(&sb, "  [WARN] Very short prompt - may lack sufficient context.\n", -1);
        else if (promptLen > 4000)
            sb_append(&sb, "  [WARN] Very long prompt - consider condensing.\n", -1);
        else
            sb_append(&sb, "  [OK] Prompt length is reasonable.\n", -1);

        if (ContainsSubstringCI(promptText, "do something") ||
            ContainsSubstringCI(promptText, "help me") ||
            ContainsSubstringCI(promptText, "figure out"))
            sb_append(&sb, "  [WARN] Vague phrasing detected. Be more specific about the task.\n", -1);
        else
            sb_append(&sb, "  [OK] No obvious vague phrasing.\n", -1);
    }

    // Injection check
    if (!criteria || strstr(criteria, "injection") || !criteria[0])
    {
        sb_append(&sb, "\nINJECTION RISK:\n", -1);
        int risks = 0;
        if (ContainsSubstringCI(promptText, "ignore previous") ||
            ContainsSubstringCI(promptText, "ignore above") ||
            ContainsSubstringCI(promptText, "disregard"))
        { sb_append(&sb, "  [HIGH] Contains override-style injection pattern.\n", -1); risks++; }
        if (ContainsSubstringCI(promptText, "system:") ||
            ContainsSubstringCI(promptText, "SYSTEM:") ||
            ContainsSubstringCI(promptText, "you are now"))
        { sb_append(&sb, "  [HIGH] Contains role reassignment pattern.\n", -1); risks++; }
        if (strstr(promptText, "{{") || strstr(promptText, "{%"))
        { sb_append(&sb, "  [MED] Contains template injection markers.\n", -1); risks++; }
        if (ContainsSubstringCI(promptText, "<script") || ContainsSubstringCI(promptText, "javascript:"))
        { sb_append(&sb, "  [MED] Contains XSS-style patterns.\n", -1); risks++; }
        if (risks == 0)
            sb_append(&sb, "  [OK] No injection patterns detected.\n", -1);
    }

    // Bias check
    if (criteria && strstr(criteria, "bias"))
    {
        sb_append(&sb, "\nBIAS:\n", -1);
        if (ContainsSubstringCI(promptText, "always") || ContainsSubstringCI(promptText, "never"))
            sb_append(&sb, "  [WARN] Contains absolute terms (always/never) which may introduce bias.\n", -1);
        else
            sb_append(&sb, "  [OK] No obvious bias indicators.\n", -1);
    }

    // Specificity
    if (!criteria || strstr(criteria, "specificity") || !criteria[0])
    {
        sb_append(&sb, "\nSPECIFICITY:\n", -1);
        if (ContainsSubstringCI(promptText, "example") || ContainsSubstringCI(promptText, "e.g.") ||
            ContainsSubstringCI(promptText, "for instance"))
            sb_append(&sb, "  [OK] Contains examples - good for specificity.\n", -1);
        else
            sb_append(&sb, "  [SUGGEST] Consider adding examples to improve output quality.\n", -1);
        if (ContainsSubstringCI(promptText, "format") || ContainsSubstringCI(promptText, "output") ||
            ContainsSubstringCI(promptText, "json") || ContainsSubstringCI(promptText, "markdown"))
            sb_append(&sb, "  [OK] Specifies output format.\n", -1);
        else
            sb_append(&sb, "  [SUGGEST] Consider specifying the desired output format.\n", -1);
    }

    return sb.data;
}

static char* Tool_RedTeamPrompt(const char* promptText, const char* attackVectors)
{
    if (!promptText || !promptText[0])
        return _strdup("Error: prompt_text parameter is required.");

    StrBuf sb;
    sb_init(&sb, 2048);

    sb_append(&sb, "Red Team Analysis\n==================\n\n", -1);

    // Injection vectors
    if (!attackVectors || strstr(attackVectors, "injection") || !attackVectors[0])
    {
        int found = 0;
        sb_append(&sb, "INJECTION VECTORS:\n", -1);
        if (ContainsSubstringCI(promptText, "ignore") && ContainsSubstringCI(promptText, "previous"))
        { sb_append(&sb, "  [VULN] Direct override: 'ignore previous' pattern found.\n", -1); found++; }
        if (ContainsSubstringCI(promptText, "forget") && (ContainsSubstringCI(promptText, "instruction") || ContainsSubstringCI(promptText, "rules")))
        { sb_append(&sb, "  [VULN] Memory wipe: 'forget instructions/rules' pattern.\n", -1); found++; }
        if (ContainsSubstringCI(promptText, "you are now") || ContainsSubstringCI(promptText, "act as") ||
            ContainsSubstringCI(promptText, "pretend to be"))
        { sb_append(&sb, "  [VULN] Role hijack: identity reassignment pattern.\n", -1); found++; }
        if (strstr(promptText, "{{") || strstr(promptText, "{%") || strstr(promptText, "${"))
        { sb_append(&sb, "  [VULN] Template injection: unescaped template markers.\n", -1); found++; }
        if (ContainsSubstringCI(promptText, "SELECT") && ContainsSubstringCI(promptText, "FROM"))
        { sb_append(&sb, "  [VULN] SQL-like patterns in prompt text.\n", -1); found++; }
        if (found == 0)
            sb_append(&sb, "  [PASS] No injection patterns detected.\n", -1);
    }

    // Jailbreak vectors
    if (!attackVectors || strstr(attackVectors, "jailbreak") || !attackVectors[0])
    {
        int found = 0;
        sb_append(&sb, "\nJAILBREAK VECTORS:\n", -1);
        if (ContainsSubstringCI(promptText, "DAN") || ContainsSubstringCI(promptText, "do anything now"))
        { sb_append(&sb, "  [VULN] Known DAN jailbreak pattern.\n", -1); found++; }
        if (ContainsSubstringCI(promptText, "developer mode") || ContainsSubstringCI(promptText, "no restrictions"))
        { sb_append(&sb, "  [VULN] Restriction bypass pattern.\n", -1); found++; }
        if (ContainsSubstringCI(promptText, "hypothetical") || ContainsSubstringCI(promptText, "fictional"))
        { sb_append(&sb, "  [WARN] Fictional framing - common jailbreak wrapper.\n", -1); found++; }
        if (found == 0)
            sb_append(&sb, "  [PASS] No jailbreak patterns detected.\n", -1);
    }

    // Data leakage
    if (!attackVectors || strstr(attackVectors, "leakage") || !attackVectors[0])
    {
        int found = 0;
        sb_append(&sb, "\nDATA LEAKAGE:\n", -1);
        if (ContainsSubstringCI(promptText, "repeat") && ContainsSubstringCI(promptText, "system"))
        { sb_append(&sb, "  [VULN] System prompt extraction: 'repeat system' pattern.\n", -1); found++; }
        if (ContainsSubstringCI(promptText, "show me your") && (ContainsSubstringCI(promptText, "instructions") || ContainsSubstringCI(promptText, "prompt")))
        { sb_append(&sb, "  [VULN] Direct prompt extraction attempt.\n", -1); found++; }
        if (strstr(promptText, "../") || strstr(promptText, "..\\"))
        { sb_append(&sb, "  [VULN] Path traversal pattern.\n", -1); found++; }
        if (found == 0)
            sb_append(&sb, "  [PASS] No data leakage patterns detected.\n", -1);
    }

    // Bias
    if (attackVectors && strstr(attackVectors, "bias"))
    {
        sb_append(&sb, "\nBIAS ANALYSIS:\n", -1);
        if (ContainsSubstringCI(promptText, "always") || ContainsSubstringCI(promptText, "never") ||
            ContainsSubstringCI(promptText, "must always"))
            sb_append(&sb, "  [WARN] Contains absolute directives that may introduce systematic bias.\n", -1);
        else
            sb_append(&sb, "  [PASS] No obvious bias-inducing patterns.\n", -1);
    }

    return sb.data;
}

static char* Tool_DesignAudit(const char* path, const char* checks)
{
    if (!path || !path[0])
        return _strdup("Error: path parameter is required.");

    // Read the file content
    char* content = Tool_ReadFile(path, 0, 0);
    if (!content || strncmp(content, "Error:", 6) == 0)
        return content ? content : _strdup("Error: Failed to read file.");

    StrBuf sb;
    sb_init(&sb, 4096);
    sb_appendf(&sb, "Design Audit: %s\n", path);
    sb_append(&sb, "============================\n\n", -1);

    // Typography
    if (!checks || strstr(checks, "typography") || !checks[0])
    {
        sb_append(&sb, "TYPOGRAPHY:\n", -1);
        if (strstr(content, "font-size"))
            sb_append(&sb, "  [FOUND] font-size declarations present.\n", -1);
        else
            sb_append(&sb, "  [MISSING] No font-size declarations found.\n", -1);

        if (strstr(content, "line-height"))
            sb_append(&sb, "  [FOUND] line-height declarations present.\n", -1);
        else
            sb_append(&sb, "  [SUGGEST] Consider adding line-height for readability (1.4-1.6 for body text).\n", -1);

        if (strstr(content, "font-family"))
            sb_append(&sb, "  [FOUND] font-family declarations present.\n", -1);
        else
            sb_append(&sb, "  [SUGGEST] Consider declaring font-family with a proper font stack.\n", -1);

        if (ContainsSubstringCI(content, "letter-spacing"))
            sb_append(&sb, "  [FOUND] letter-spacing used.\n", -1);
    }

    // Color
    if (!checks || strstr(checks, "color") || !checks[0])
    {
        sb_append(&sb, "\nCOLOR & CONTRAST:\n", -1);
        int hexCount = 0;
        const char* p = content;
        while ((p = strstr(p, "#")) != NULL) { hexCount++; p++; }
        sb_appendf(&sb, "  [INFO] Found %d hex color references.\n", hexCount);

        if (strstr(content, "oklch") || strstr(content, "OKLCH"))
            sb_append(&sb, "  [GOOD] OKLCH color space used (perceptually uniform).\n", -1);
        if (strstr(content, "rgb") || strstr(content, "rgba"))
            sb_append(&sb, "  [INFO] RGB/RGBA colors found.\n", -1);
        if (strstr(content, "hsl") || strstr(content, "hsla"))
            sb_append(&sb, "  [INFO] HSL/HSLA colors found.\n", -1);
        if (ContainsSubstringCI(content, "contrast") || ContainsSubstringCI(content, "WCAG"))
            sb_append(&sb, "  [GOOD] Contrast/WCAG references found.\n", -1);
        else
            sb_append(&sb, "  [SUGGEST] Consider verifying WCAG AA contrast ratios (4.5:1 for text, 3:1 for large text).\n", -1);
    }

    // Spacing
    if (!checks || strstr(checks, "spacing") || !checks[0])
    {
        sb_append(&sb, "\nSPACING:\n", -1);
        BOOL hasPx = strstr(content, "px") != NULL;
        BOOL hasRem = strstr(content, "rem") != NULL;
        BOOL hasEm = strstr(content, "em") != NULL;
        if (hasPx && (hasRem || hasEm))
            sb_append(&sb, "  [WARN] Mixed units (px + rem/em). Consider standardizing on rem for consistency.\n", -1);
        else if (hasRem)
            sb_append(&sb, "  [GOOD] Using rem units for consistent spacing.\n", -1);
        else if (hasPx)
            sb_append(&sb, "  [INFO] Using px units. Consider rem for responsive design.\n", -1);

        if (strstr(content, "gap") || strstr(content, "grid-gap"))
            sb_append(&sb, "  [GOOD] CSS gap property used for layout spacing.\n", -1);
    }

    // Hierarchy
    if (!checks || strstr(checks, "hierarchy") || !checks[0])
    {
        sb_append(&sb, "\nHIERARCHY:\n", -1);
        for (int h = 1; h <= 6; h++)
        {
            char tag[8];
            snprintf(tag, sizeof(tag), "<h%d", h);
            if (strstr(content, tag))
                sb_appendf(&sb, "  [FOUND] h%d heading tag used.\n", h);
        }
        if (strstr(content, "z-index"))
            sb_append(&sb, "  [INFO] z-index declarations found. Verify stacking order is intentional.\n", -1);
    }

    // Accessibility
    if (checks && strstr(checks, "a11y"))
    {
        sb_append(&sb, "\nACCESSIBILITY:\n", -1);
        if (ContainsSubstringCI(content, "aria-"))
            sb_append(&sb, "  [GOOD] ARIA attributes found.\n", -1);
        else
            sb_append(&sb, "  [SUGGEST] Consider adding ARIA attributes for screen readers.\n", -1);
        if (ContainsSubstringCI(content, "alt=") || ContainsSubstringCI(content, "alt ="))
            sb_append(&sb, "  [GOOD] alt attributes found on images.\n", -1);
        if (ContainsSubstringCI(content, "role="))
            sb_append(&sb, "  [GOOD] ARIA role attributes found.\n", -1);
        if (ContainsSubstringCI(content, ":focus") || ContainsSubstringCI(content, "focus-visible"))
            sb_append(&sb, "  [GOOD] Focus styles defined.\n", -1);
        else
            sb_append(&sb, "  [SUGGEST] Consider adding :focus-visible styles for keyboard navigation.\n", -1);
    }

    // Append truncated file content for AI to do deeper analysis
    sb_append(&sb, "\n--- File excerpt (first 200 lines) ---\n", -1);
    int lineCount = 0;
    const char* lp = content;
    while (*lp && lineCount < 200)
    {
        const char* eol = strchr(lp, '\n');
        if (!eol) eol = lp + strlen(lp);
        int lineLen = (int)(eol - lp);
        if (lineLen > 300) lineLen = 300;
        sb_appendf(&sb, "%4d| ", lineCount + 1);
        sb_append(&sb, lp, lineLen);
        sb_append(&sb, "\n", 1);
        lp = *eol ? eol + 1 : eol;
        lineCount++;
    }

    free(content);
    return sb.data;
}

static char* Tool_ContextStore(const char* operation, const char* key,
                                const char* value, const char* query)
{
    char storePath[MAX_PATH];
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    snprintf(storePath, sizeof(storePath), "%s\\.bikode\\context-store.json", cwd);

    if (!operation || !operation[0])
        return _strdup("Error: operation parameter is required (store, retrieve, or list).");

    // Ensure .bikode directory exists
    {
        char dirPath[MAX_PATH];
        snprintf(dirPath, sizeof(dirPath), "%s\\.bikode", cwd);
        CreateDirectoryA(dirPath, NULL);
    }

    if (_stricmp(operation, "store") == 0)
    {
        if (!key || !key[0])
            return _strdup("Error: key parameter is required for store operation.");
        if (!value || !value[0])
            return _strdup("Error: value parameter is required for store operation.");

        // Read existing store
        HANDLE hFile = CreateFileA(storePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        StrBuf existing;
        sb_init(&existing, 1024);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            char buf[4096];
            DWORD read;
            while (ReadFile(hFile, buf, sizeof(buf) - 1, &read, NULL) && read > 0)
            {
                buf[read] = '\0';
                sb_append(&existing, buf, (int)read);
            }
            CloseHandle(hFile);
        }

        // Build new JSON - simple approach: rebuild from scratch
        // Parse existing entries
        StrBuf out;
        sb_init(&out, 2048);
        sb_append(&out, "{\n", -1);

        // Add existing entries (except the key we're updating)
        if (existing.data && existing.len > 2)
        {
            // Simple key-value extraction from existing JSON
            const char* p = existing.data;
            while ((p = strstr(p, "\"")) != NULL)
            {
                p++; // skip opening quote
                const char* keyStart = p;
                const char* keyEnd = strchr(p, '"');
                if (!keyEnd) break;

                // Skip colon
                const char* afterKey = keyEnd + 1;
                while (*afterKey == ' ' || *afterKey == ':' || *afterKey == '\t' || *afterKey == '\n' || *afterKey == '\r') afterKey++;
                if (*afterKey != '"') { p = afterKey; continue; }

                int existingKeyLen = (int)(keyEnd - keyStart);
                char existingKey[256];
                if (existingKeyLen < (int)sizeof(existingKey))
                {
                    memcpy(existingKey, keyStart, existingKeyLen);
                    existingKey[existingKeyLen] = '\0';

                    // Skip if this is the key we're updating
                    if (strcmp(existingKey, key) != 0)
                    {
                        afterKey++; // skip value opening quote
                        const char* valEnd = afterKey;
                        while (*valEnd && (*valEnd != '"' || *(valEnd - 1) == '\\')) valEnd++;

                        if (out.len > 3) sb_append(&out, ",\n", -1);
                        sb_append(&out, "  \"", -1);
                        sb_append(&out, existingKey, existingKeyLen);
                        sb_append(&out, "\": \"", -1);
                        sb_append(&out, afterKey, (int)(valEnd - afterKey));
                        sb_append(&out, "\"", -1);
                    }
                }
                p = afterKey;
            }
        }

        // Add the new key-value
        if (out.len > 3) sb_append(&out, ",\n", -1);
        sb_append(&out, "  \"", -1);
        sb_append(&out, key, -1);
        sb_append(&out, "\": \"", -1);
        // Escape the value
        for (const char* v = value; *v; v++)
        {
            if (*v == '"') sb_append(&out, "\\\"", 2);
            else if (*v == '\\') sb_append(&out, "\\\\", 2);
            else if (*v == '\n') sb_append(&out, "\\n", 2);
            else sb_append(&out, v, 1);
        }
        sb_append(&out, "\"\n}\n", -1);

        sb_free(&existing);

        // Write back
        hFile = CreateFileA(storePath, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            DWORD written;
            WriteFile(hFile, out.data, out.len, &written, NULL);
            CloseHandle(hFile);
        }
        sb_free(&out);

        StrBuf result;
        sb_init(&result, 256);
        sb_appendf(&result, "Stored: %s = %s", key, value);
        return result.data;
    }
    else if (_stricmp(operation, "retrieve") == 0)
    {
        if (!query || !query[0])
            return _strdup("Error: query parameter is required for retrieve operation.");

        HANDLE hFile = CreateFileA(storePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return _strdup("Context store is empty. No entries stored yet.");

        char buf[16384];
        DWORD read;
        ReadFile(hFile, buf, sizeof(buf) - 1, &read, NULL);
        buf[read] = '\0';
        CloseHandle(hFile);

        // Fuzzy match: search keys and values for query terms
        StrBuf result;
        sb_init(&result, 1024);
        sb_appendf(&result, "Context store results for \"%s\":\n\n", query);

        int found = 0;
        const char* p = buf;
        while ((p = strstr(p, "\"")) != NULL)
        {
            p++;
            const char* keyStart = p;
            const char* keyEnd = strchr(p, '"');
            if (!keyEnd) break;

            const char* afterKey = keyEnd + 1;
            while (*afterKey == ' ' || *afterKey == ':' || *afterKey == '\t') afterKey++;
            if (*afterKey != '"') { p = afterKey; continue; }
            afterKey++;
            const char* valEnd = afterKey;
            while (*valEnd && (*valEnd != '"' || *(valEnd - 1) == '\\')) valEnd++;

            int klen = (int)(keyEnd - keyStart);
            int vlen = (int)(valEnd - afterKey);
            char k[256], v[4096];
            if (klen >= (int)sizeof(k)) klen = (int)sizeof(k) - 1;
            if (vlen >= (int)sizeof(v)) vlen = (int)sizeof(v) - 1;
            memcpy(k, keyStart, klen); k[klen] = '\0';
            memcpy(v, afterKey, vlen); v[vlen] = '\0';

            if (ContainsSubstringCI(k, query) || ContainsSubstringCI(v, query))
            {
                sb_appendf(&result, "  %s: %s\n", k, v);
                found++;
            }
            p = valEnd + 1;
        }
        if (found == 0)
            sb_append(&result, "  (no matching entries found)\n", -1);

        return result.data;
    }
    else if (_stricmp(operation, "list") == 0)
    {
        HANDLE hFile = CreateFileA(storePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return _strdup("Context store is empty. No entries stored yet.");

        char buf[16384];
        DWORD read;
        ReadFile(hFile, buf, sizeof(buf) - 1, &read, NULL);
        buf[read] = '\0';
        CloseHandle(hFile);

        StrBuf result;
        sb_init(&result, 1024);
        sb_append(&result, "Context store entries:\n\n", -1);

        const char* p = buf;
        int count = 0;
        while ((p = strstr(p, "\"")) != NULL)
        {
            p++;
            const char* keyEnd = strchr(p, '"');
            if (!keyEnd) break;

            const char* afterKey = keyEnd + 1;
            while (*afterKey == ' ' || *afterKey == ':' || *afterKey == '\t') afterKey++;
            if (*afterKey != '"') { p = afterKey; continue; }
            afterKey++;
            const char* valEnd = afterKey;
            while (*valEnd && (*valEnd != '"' || *(valEnd - 1) == '\\')) valEnd++;

            int klen = (int)(keyEnd - p);
            int vlen = (int)(valEnd - afterKey);
            char k[256];
            if (klen >= (int)sizeof(k)) klen = (int)sizeof(k) - 1;
            memcpy(k, p, klen); k[klen] = '\0';

            sb_appendf(&result, "  %s", k);
            if (vlen > 80)
                sb_appendf(&result, ": %.80s...\n", afterKey);
            else
            {
                char v[256];
                if (vlen >= (int)sizeof(v)) vlen = (int)sizeof(v) - 1;
                memcpy(v, afterKey, vlen); v[vlen] = '\0';
                sb_appendf(&result, ": %s\n", v);
            }
            count++;
            p = valEnd + 1;
        }
        if (count == 0)
            sb_append(&result, "  (empty)\n", -1);

        return result.data;
    }

    return _strdup("Error: operation must be 'store', 'retrieve', or 'list'.");
}

//=============================================================================
// Tool: run_workflow — trigger an AgentRuntime workflow from chat
//=============================================================================

static char* Tool_RunWorkflow(const char* workflowName, const char* action)
{
    // Handle cancel/pause/resume actions on a running workflow
    if (action && action[0])
    {
        if (_stricmp(action, "cancel") == 0 || _stricmp(action, "stop") == 0)
        {
            if (!AgentRuntime_IsRunning())
                return _strdup("No workflow is currently running.");
            AgentRuntime_Cancel();
            return _strdup("Workflow canceled.");
        }
        if (_stricmp(action, "pause") == 0)
        {
            if (!AgentRuntime_IsRunning())
                return _strdup("No workflow is currently running.");
            AgentRuntime_SetPaused(TRUE);
            return _strdup("Workflow paused.");
        }
        if (_stricmp(action, "resume") == 0 || _stricmp(action, "unpause") == 0)
        {
            AgentRuntime_SetPaused(FALSE);
            return _strdup("Workflow resumed.");
        }
        if (_stricmp(action, "status") == 0)
        {
            AgentRuntimeSnapshot snapshot;
            if (!AgentRuntime_GetSnapshot(&snapshot))
                return _strdup("No workflow has been run yet.");
            StrBuf sb;
            sb_init(&sb, 1024);
            sb_appendf(&sb, "Workflow: %s\nRunning: %s\nPaused: %s\nNodes: %d\n",
                snapshot.org.name,
                snapshot.isRunning ? "yes" : "no",
                snapshot.isPaused ? "yes" : "no",
                snapshot.nodeCount);
            for (int i = 0; i < snapshot.nodeCount; i++)
            {
                sb_appendf(&sb, "  [%s] %s — %s (tools: %d, files: %d)\n",
                    AgentRuntime_StateLabel(snapshot.nodes[i].state),
                    snapshot.nodes[i].title,
                    snapshot.nodes[i].lastAction[0] ? snapshot.nodes[i].lastAction : "idle",
                    snapshot.nodes[i].toolCount,
                    snapshot.nodes[i].fileCount);
            }
            return sb.data;
        }
        if (_stricmp(action, "list") == 0)
        {
            goto list_workflows;
        }
    }

    // If no workflow name given, list available workflows
    if (!workflowName || !workflowName[0])
    {
list_workflows:;
        OrgSpec orgs[32];
        int orgCount = 0;
        const WCHAR* wsRoot = AgentRuntime_GetWorkspaceRoot();
        if (!wsRoot || !wsRoot[0])
        {
            WCHAR cwd[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, cwd);
            wsRoot = cwd;
        }
        if (!AgentRuntime_LoadOrgSpecs(wsRoot, orgs, 32, &orgCount) || orgCount == 0)
            return _strdup("No workflows found. Create one in .bikode/orgs/ or use Mission Control.");

        StrBuf sb;
        sb_init(&sb, 512);
        sb_appendf(&sb, "Available workflows (%d):\n", orgCount);
        for (int i = 0; i < orgCount; i++)
            sb_appendf(&sb, "  %d. %s (%d agents)\n", i + 1, orgs[i].name, orgs[i].nodeCount);
        sb_append(&sb, "\nUse run_workflow with the workflow name to start one.", -1);
        return sb.data;
    }

    // Check if a workflow is already running
    if (AgentRuntime_IsRunning())
        return _strdup("Error: A workflow is already running. Cancel it first with action='cancel'.");

    // Find and start the named workflow
    {
        OrgSpec orgs[32];
        int orgCount = 0;
        const WCHAR* wsRoot = AgentRuntime_GetWorkspaceRoot();
        if (!wsRoot || !wsRoot[0])
        {
            WCHAR cwd[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, cwd);
            wsRoot = cwd;
        }
        if (!AgentRuntime_LoadOrgSpecs(wsRoot, orgs, 32, &orgCount) || orgCount == 0)
            return _strdup("Error: No workflows found in this workspace.");

        // Match by name (case-insensitive, partial match)
        int matchIdx = -1;
        for (int i = 0; i < orgCount; i++)
        {
            if (_stricmp(orgs[i].name, workflowName) == 0)
            {
                matchIdx = i;
                break;
            }
        }
        // Fallback: partial match
        if (matchIdx < 0)
        {
            for (int i = 0; i < orgCount; i++)
            {
                if (ContainsSubstringCI(orgs[i].name, workflowName))
                {
                    matchIdx = i;
                    break;
                }
            }
        }
        // Fallback: numeric index (1-based)
        if (matchIdx < 0)
        {
            int idx = atoi(workflowName);
            if (idx >= 1 && idx <= orgCount)
                matchIdx = idx - 1;
        }

        if (matchIdx < 0)
        {
            StrBuf sb;
            sb_init(&sb, 512);
            sb_appendf(&sb, "Error: No workflow matching '%s'. Available:\n", workflowName);
            for (int i = 0; i < orgCount; i++)
                sb_appendf(&sb, "  %d. %s\n", i + 1, orgs[i].name);
            return sb.data;
        }

        if (AgentRuntime_Start(&orgs[matchIdx]))
        {
            StrBuf sb;
            sb_init(&sb, 256);
            sb_appendf(&sb, "Started workflow '%s' with %d agent nodes.\n",
                orgs[matchIdx].name, orgs[matchIdx].nodeCount);
            for (int i = 0; i < orgs[matchIdx].nodeCount; i++)
                sb_appendf(&sb, "  - %s (%s)\n", orgs[matchIdx].nodes[i].title,
                    orgs[matchIdx].nodes[i].role);
            return sb.data;
        }
        return _strdup("Error: Failed to start workflow. Check Mission Control for details.");
    }
}

//=============================================================================
// Tool: lint — auto-detect and run available linters/formatters
//=============================================================================

// Check if a command is available on the system PATH
static BOOL IsCommandAvailable(const char* cmd)
{
    char check[512];
    _snprintf_s(check, sizeof(check), _TRUNCATE, "where %s >nul 2>nul", cmd);
    return (system(check) == 0);
}

static char* Tool_Lint(const char* path, const char* tool)
{
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);

    // If a specific tool is requested, run it directly
    if (tool && tool[0])
    {
        char cmd[1024];
        if (_stricmp(tool, "ruff") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "ruff check %s --output-format=concise",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "biome") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "npx @biomejs/biome check %s",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "eslint") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "npx eslint %s --format=compact",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "ast-grep") == 0 || _stricmp(tool, "sg") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "sg scan %s",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "clippy") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "cargo clippy --message-format=short 2>&1");
        }
        else if (_stricmp(tool, "golangci-lint") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "golangci-lint run %s",
                path && path[0] ? path : "./...");
        }
        else if (_stricmp(tool, "mypy") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "mypy %s --no-color-output",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "flake8") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "flake8 %s",
                path && path[0] ? path : ".");
        }
        else
        {
            // Unknown tool — run as custom command
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s",
                tool, path && path[0] ? path : ".");
        }
        return Tool_RunCommand(cmd, NULL);
    }

    // Auto-detect: check for config files and available tools
    StrBuf sb;
    sb_init(&sb, 2048);
    int toolsRun = 0;

    // Python: check for pyproject.toml or ruff.toml
    {
        BOOL hasRuff = PathFileExistsA("ruff.toml") || PathFileExistsA(".ruff.toml") ||
                       PathFileExistsA("pyproject.toml");
        if (hasRuff && IsCommandAvailable("ruff"))
        {
            char* result = Tool_RunCommand(
                path && path[0] ? "ruff check --output-format=concise" :
                "ruff check . --output-format=concise", NULL);
            if (result)
            {
                sb_append(&sb, "[Ruff]\n", -1);
                sb_append(&sb, result, -1);
                sb_append(&sb, "\n", 1);
                free(result);
            }
            toolsRun++;
        }
    }

    // JavaScript/TypeScript: check for biome.json or biome.jsonc
    {
        BOOL hasBiome = PathFileExistsA("biome.json") || PathFileExistsA("biome.jsonc");
        if (hasBiome)
        {
            char* result = Tool_RunCommand("npx @biomejs/biome check .", NULL);
            if (result)
            {
                sb_append(&sb, "[Biome]\n", -1);
                sb_append(&sb, result, -1);
                sb_append(&sb, "\n", 1);
                free(result);
            }
            toolsRun++;
        }
    }

    // JavaScript/TypeScript: check for .eslintrc or eslint.config
    if (toolsRun == 0)
    {
        BOOL hasEslint = PathFileExistsA(".eslintrc.json") || PathFileExistsA(".eslintrc.js") ||
                         PathFileExistsA(".eslintrc.yml") || PathFileExistsA("eslint.config.js") ||
                         PathFileExistsA("eslint.config.mjs");
        if (hasEslint)
        {
            char* result = Tool_RunCommand("npx eslint . --format=compact", NULL);
            if (result)
            {
                sb_append(&sb, "[ESLint]\n", -1);
                sb_append(&sb, result, -1);
                sb_append(&sb, "\n", 1);
                free(result);
            }
            toolsRun++;
        }
    }

    // Rust: check for Cargo.toml
    {
        if (PathFileExistsA("Cargo.toml") && IsCommandAvailable("cargo"))
        {
            char* result = Tool_RunCommand("cargo clippy --message-format=short 2>&1", NULL);
            if (result)
            {
                sb_append(&sb, "[Clippy]\n", -1);
                sb_append(&sb, result, -1);
                sb_append(&sb, "\n", 1);
                free(result);
            }
            toolsRun++;
        }
    }

    // Go: check for go.mod
    {
        if (PathFileExistsA("go.mod") && IsCommandAvailable("golangci-lint"))
        {
            char* result = Tool_RunCommand("golangci-lint run ./...", NULL);
            if (result)
            {
                sb_append(&sb, "[golangci-lint]\n", -1);
                sb_append(&sb, result, -1);
                sb_append(&sb, "\n", 1);
                free(result);
            }
            toolsRun++;
        }
    }

    // ast-grep: check for sgconfig.yml
    {
        if (PathFileExistsA("sgconfig.yml") && IsCommandAvailable("sg"))
        {
            char* result = Tool_RunCommand("sg scan .", NULL);
            if (result)
            {
                sb_append(&sb, "[ast-grep]\n", -1);
                sb_append(&sb, result, -1);
                sb_append(&sb, "\n", 1);
                free(result);
            }
            toolsRun++;
        }
    }

    if (toolsRun == 0)
    {
        sb_free(&sb);
        return _strdup(
            "No linter config files detected in the workspace.\n"
            "Supported tools (auto-detected by config):\n"
            "  - Ruff: ruff.toml, .ruff.toml, pyproject.toml\n"
            "  - Biome: biome.json, biome.jsonc\n"
            "  - ESLint: .eslintrc.*, eslint.config.*\n"
            "  - Clippy: Cargo.toml (Rust)\n"
            "  - golangci-lint: go.mod (Go)\n"
            "  - ast-grep: sgconfig.yml\n\n"
            "You can also run a specific tool: {\"name\": \"lint\", \"tool\": \"ruff\"}");
    }

    return sb.data;
}

//=============================================================================
// Tool: format — run code formatters
//=============================================================================

static char* Tool_Format(const char* path, const char* tool)
{
    // If a specific formatter is requested
    if (tool && tool[0])
    {
        char cmd[1024];
        if (_stricmp(tool, "ruff") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "ruff format %s",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "biome") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "npx @biomejs/biome format --write %s",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "prettier") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "npx prettier --write %s",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "black") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "black %s",
                path && path[0] ? path : ".");
        }
        else if (_stricmp(tool, "rustfmt") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "cargo fmt");
        }
        else if (_stricmp(tool, "gofmt") == 0)
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "gofmt -w %s",
                path && path[0] ? path : ".");
        }
        else
        {
            _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s",
                tool, path && path[0] ? path : ".");
        }
        return Tool_RunCommand(cmd, NULL);
    }

    // Auto-detect formatter
    if (PathFileExistsA("pyproject.toml") || PathFileExistsA("ruff.toml"))
    {
        if (IsCommandAvailable("ruff"))
            return Tool_RunCommand(path && path[0] ? "ruff format" : "ruff format .", NULL);
        if (IsCommandAvailable("black"))
            return Tool_RunCommand(path && path[0] ? "black" : "black .", NULL);
    }
    if (PathFileExistsA("biome.json") || PathFileExistsA("biome.jsonc"))
        return Tool_RunCommand("npx @biomejs/biome format --write .", NULL);
    if (PathFileExistsA(".prettierrc") || PathFileExistsA(".prettierrc.json") ||
        PathFileExistsA("prettier.config.js"))
        return Tool_RunCommand("npx prettier --write .", NULL);
    if (PathFileExistsA("Cargo.toml") && IsCommandAvailable("cargo"))
        return Tool_RunCommand("cargo fmt", NULL);
    if (PathFileExistsA("go.mod") && IsCommandAvailable("gofmt"))
        return Tool_RunCommand("gofmt -w .", NULL);

    return _strdup(
        "No formatter config detected.\n"
        "Supported: ruff, biome, prettier, black, rustfmt, gofmt.\n"
        "Specify one with: {\"name\": \"format\", \"tool\": \"prettier\"}");
}

// Dispatch a single tool call
static char* ExecuteTool(const ToolCall* tc)
{
    if (strcmp(tc->name, "read_file") == 0)
        return Tool_ReadFile(tc->path, tc->startLine, tc->lineCount);
    if (strcmp(tc->name, "get_active_document") == 0)
        return Tool_GetActiveDocument(tc->startLine, tc->lineCount);
    if (strcmp(tc->name, "write_file") == 0)
        return Tool_WriteFile(tc->path, tc->content);
    if (strcmp(tc->name, "replace_in_file") == 0)
        return Tool_ReplaceInFile(tc->path, tc->oldText, tc->newText);
    if (strcmp(tc->name, "open_file") == 0)
        return Tool_OpenFile(tc->path);
    if (strcmp(tc->name, "insert_in_editor") == 0)
        return Tool_InsertInEditor(tc->content);
    if (strcmp(tc->name, "replace_editor_content") == 0)
        return Tool_ReplaceEditorContent(tc->content);
    if (strcmp(tc->name, "new_file_in_editor") == 0)
        return Tool_NewFileInEditor(tc->content);
    if (strcmp(tc->name, "make_dir") == 0)
        return Tool_MakeDir(tc->path);
    if (strcmp(tc->name, "init_repo") == 0)
        return Tool_InitRepo(tc->path);
    if (strcmp(tc->name, "run_command") == 0)
        return Tool_RunCommand(tc->command, tc->cwd);
    if (strcmp(tc->name, "list_dir") == 0)
        return Tool_ListDir(tc->path);
    if (strcmp(tc->name, "semantic_search") == 0)
        return Tool_SemanticSearch(tc->command ? tc->command : tc->content, tc->path, tc->maxResults);
    if (strcmp(tc->name, "web_search") == 0)
        return Tool_WebSearch(tc->command ? tc->command : tc->content);
    if (strcmp(tc->name, "gif_search") == 0)
        return Tool_GifSearch(tc->command ? tc->command : tc->content);
    if (strcmp(tc->name, "eval_prompt") == 0)
        return Tool_EvalPrompt(tc->content, tc->cwd);
    if (strcmp(tc->name, "red_team_prompt") == 0)
        return Tool_RedTeamPrompt(tc->content, tc->cwd);
    if (strcmp(tc->name, "design_audit") == 0)
        return Tool_DesignAudit(tc->path, tc->cwd);
    if (strcmp(tc->name, "context_store") == 0)
        return Tool_ContextStore(tc->command, tc->path, tc->newText, tc->content);
    if (strcmp(tc->name, "run_workflow") == 0)
        return Tool_RunWorkflow(tc->path, tc->command);
    if (strcmp(tc->name, "lint") == 0)
        return Tool_Lint(tc->path, tc->command);
    if (strcmp(tc->name, "format") == 0)
        return Tool_Format(tc->path, tc->command);

    char err[128];
    snprintf(err, sizeof(err), "Error: Unknown tool '%s'", tc->name);
    return _strdup(err);
}

//=============================================================================
// Helper: post status to UI
//=============================================================================

static void PostStatusToUI(HWND hwnd, const char* fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    char* msg = _strdup(tmp);
    if (msg)
        PostMessage(hwnd, WM_AI_AGENT_STATUS, 0, (LPARAM)msg);
}

static void PostToolInfoToUI(HWND hwnd, const char* toolName, const char* detail)
{
    char tmp[1024];
    if (detail && detail[0])
        snprintf(tmp, sizeof(tmp), "[%s: %s]", toolName, detail);
    else
        snprintf(tmp, sizeof(tmp), "[%s]", toolName);

    char* msg = _strdup(tmp);
    if (msg)
        PostMessage(hwnd, WM_AI_AGENT_TOOL, 0, (LPARAM)msg);
}


static int BuildDefaultTaskGraph(AgentIntent intent, TaskNode nodes[8])
{
    int n = 0;
    nodes[n++] = (TaskNode){ "Plan", "Planner Agent", "ready" };
    if (intent == INTENT_REVIEW)
    {
        nodes[n++] = (TaskNode){ "Inspect code", "Reviewer Agent", "ready" };
    }
    else if (intent == INTENT_RUN || intent == INTENT_BUILD)
    {
        nodes[n++] = (TaskNode){ "Execute commands", "Setup Agent", "ready" };
        nodes[n++] = (TaskNode){ "Debug failures", "Debug Agent", "ready" };
    }
    else
    {
        nodes[n++] = (TaskNode){ "Implement edits", "Implementer Agent", "ready" };
        nodes[n++] = (TaskNode){ "Review patch", "Reviewer Agent", "ready" };
        nodes[n++] = (TaskNode){ "Run validation", "Test Agent", "ready" };
    }
    return n;
}

static void PostTaskGraphStatus(HWND hwnd, AgentIntent intent)
{
    TaskNode nodes[8];
    int count = BuildDefaultTaskGraph(intent, nodes);
    StrBuf sb;
    sb_init(&sb, 256);
    sb_append(&sb, "Task graph: ", -1);
    for (int i = 0; i < count; i++)
    {
        sb_appendf(&sb, "%s/%s", nodes[i].name, nodes[i].owner);
        if (i + 1 < count)
            sb_append(&sb, " -> ", -1);
    }
    PostStatusToUI(hwnd, "%s", sb.data ? sb.data : "Task graph initialized");
    sb_free(&sb);
}

// Strip any residual tool call XML tags from text shown to user
static char* StripToolCallTags(const char* text)
{
    if (!text || !text[0]) return _strdup("");

    StrBuf sb;
    sb_init(&sb, (int)strlen(text) + 1);
    const char* p = text;

    while (*p)
    {
        // Check for any open tag variant
        BOOL found = FALSE;
        for (int i = 0; s_openTags[i]; i++)
        {
            int openLen = s_openTagLens[i];
            if (strncmp(p, s_openTags[i], openLen) == 0)
            {
                // Find matching close tag
                const char* end = strstr(p + openLen, s_closeTags[i]);
                if (end)
                {
                    p = end + s_closeTagLens[i];
                    // Skip trailing whitespace/newlines
                    while (*p == '\n' || *p == '\r') p++;
                }
                else
                {
                    p += openLen; // malformed, skip open tag
                }
                found = TRUE;
                break;
            }
        }
        if (!found)
        {
            sb_append(&sb, p, 1);
            p++;
        }
    }

    // Trim leading/trailing whitespace
    if (sb.data)
    {
        char* start = sb.data;
        while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t') start++;
        char* end = sb.data + sb.len - 1;
        while (end > start && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t'))
            *end-- = '\0';
        if (start != sb.data)
        {
            char* trimmed = _strdup(start);
            sb_free(&sb);
            return trimmed;
        }
    }

    return sb.data;
}

static BOOL ContainsSubstringCI(const char* haystack, const char* needle)
{
    size_t needleLen;

    if (!haystack || !needle || !needle[0])
        return FALSE;

    needleLen = strlen(needle);
    for (const char* p = haystack; *p; p++)
    {
        if (_strnicmp(p, needle, needleLen) == 0)
            return TRUE;
    }

    return FALSE;
}

static BOOL IsPlaceholderResponseText(const char* text)
{
    char buffer[32];
    int len = 0;

    if (!text)
        return TRUE;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;

    while (*text && len < (int)sizeof(buffer) - 1)
    {
        if (*text != ' ' && *text != '\t' && *text != '\r' && *text != '\n')
            buffer[len++] = (char)tolower((unsigned char)*text);
        text++;
    }
    buffer[len] = '\0';

    return buffer[0] == '\0' ||
        strcmp(buffer, "done") == 0 ||
        strcmp(buffer, "done.") == 0;
}



static const char* IntentName(AgentIntent intent)
{
    switch (intent)
    {
    case INTENT_SEARCH: return "search";
    case INTENT_EXPLAIN: return "explain";
    case INTENT_PATCH: return "patch";
    case INTENT_REFACTOR: return "refactor";
    case INTENT_RUN: return "run";
    case INTENT_REVIEW: return "review";
    case INTENT_BUILD: return "build";
    case INTENT_DESIGN_CHANGE: return "design_change";
    case INTENT_SELF_MODIFY_IDE: return "self_modify_ide";
    default: return "ask";
    }
}

static AgentIntent DetectIntent(const char* msg)
{
    if (!msg || !msg[0]) return INTENT_ASK;
    if (ContainsSubstringCI(msg, "theme") || ContainsSubstringCI(msg, "layout") || ContainsSubstringCI(msg, "density")) return INTENT_DESIGN_CHANGE;
    if (ContainsSubstringCI(msg, "install support") || ContainsSubstringCI(msg, "provider") || ContainsSubstringCI(msg, "model ")) return INTENT_SELF_MODIFY_IDE;
    if (ContainsSubstringCI(msg, "refactor")) return INTENT_REFACTOR;
    /* Agency: security & hardening → review role */
    if (ContainsSubstringCI(msg, "security") || ContainsSubstringCI(msg, "vulnerab") || ContainsSubstringCI(msg, "harden") || ContainsSubstringCI(msg, "audit")) return INTENT_REVIEW;
    /* Agency: eval & red-team → run role */
    if (ContainsSubstringCI(msg, "evaluat") || ContainsSubstringCI(msg, "red team") || ContainsSubstringCI(msg, "benchmark") || ContainsSubstringCI(msg, "regression guard")) return INTENT_RUN;
    /* Agency: architecture & design quality → design */
    if (ContainsSubstringCI(msg, "architect") || ContainsSubstringCI(msg, "typography") || ContainsSubstringCI(msg, "color contrast") || ContainsSubstringCI(msg, "accessibility") || ContainsSubstringCI(msg, "a11y")) return INTENT_DESIGN_CHANGE;
    /* Agency: devops & SRE → build role */
    if (ContainsSubstringCI(msg, "deploy") || ContainsSubstringCI(msg, "pipeline") || ContainsSubstringCI(msg, "ci/cd") || ContainsSubstringCI(msg, "infra") || ContainsSubstringCI(msg, "incident")) return INTENT_BUILD;
    /* Agency: performance & optimization → refactor role */
    if (ContainsSubstringCI(msg, "optimi") || ContainsSubstringCI(msg, "perf") || ContainsSubstringCI(msg, "slow") || ContainsSubstringCI(msg, "latency")) return INTENT_REFACTOR;
    if (ContainsSubstringCI(msg, "fix") || ContainsSubstringCI(msg, "implement") || ContainsSubstringCI(msg, "patch") || ContainsSubstringCI(msg, "change ")) return INTENT_PATCH;
    if (ContainsSubstringCI(msg, "run ") || ContainsSubstringCI(msg, "test") || ContainsSubstringCI(msg, "execute")) return INTENT_RUN;
    if (ContainsSubstringCI(msg, "review")) return INTENT_REVIEW;
    if (ContainsSubstringCI(msg, "build")) return INTENT_BUILD;
    if (ContainsSubstringCI(msg, "find") || ContainsSubstringCI(msg, "search")) return INTENT_SEARCH;
    if (ContainsSubstringCI(msg, "explain") || ContainsSubstringCI(msg, "why")) return INTENT_EXPLAIN;
    return INTENT_ASK;
}

static void BuildBudgetContract(BudgetContract* c, const char* msg, AgentIntent intent)
{
    if (!c) return;
    c->mode = AGENT_MODE_BALANCED;
    c->maxIterations = 15;
    c->maxToolCalls = 40;
    c->maxTouchedFiles = 16;
    c->maxApproxPromptChars = 80000;
    c->allowShell = 1;

    if (intent == INTENT_ASK || intent == INTENT_EXPLAIN || intent == INTENT_SEARCH)
    {
        c->mode = AGENT_MODE_QUICK;
        c->maxIterations = 8;
        c->maxToolCalls = 12;
        c->maxTouchedFiles = 6;
        c->maxApproxPromptChars = 40000;
    }

    if (msg && (ContainsSubstringCI(msg, "economy") || ContainsSubstringCI(msg, "cheap") || ContainsSubstringCI(msg, "token-frugal")))
    {
        c->mode = AGENT_MODE_ECONOMY;
        c->maxIterations = 6;
        c->maxToolCalls = 10;
        c->maxTouchedFiles = 4;
        c->maxApproxPromptChars = 32000;
        c->allowShell = 0;
    }
    else if (msg && (ContainsSubstringCI(msg, "max quality") || ContainsSubstringCI(msg, "best quality") || ContainsSubstringCI(msg, "deep")))
    {
        c->mode = AGENT_MODE_MAX_QUALITY;
        c->maxIterations = 15;
        c->maxToolCalls = 50;
        c->maxTouchedFiles = 16;
        c->maxApproxPromptChars = 120000;
    }

    if (msg && (ContainsSubstringCI(msg, "do not run command") || ContainsSubstringCI(msg, "no shell")))
        c->allowShell = 0;

    if (msg && ContainsSubstringCI(msg, "do not touch more than"))
    {
        int n = ExtractFirstInteger(msg);
        if (n > 0 && n < MAX_TRACKED_FILES)
            c->maxTouchedFiles = n;
    }

    if (msg && ContainsSubstringCI(msg, "do not exceed") && ContainsSubstringCI(msg, "tool"))
    {
        int n = ExtractFirstInteger(msg);
        if (n > 0)
            c->maxToolCalls = n;
    }
}

static const char* ModeName(AgentExecutionMode mode)
{
    switch (mode)
    {
    case AGENT_MODE_QUICK: return "quick";
    case AGENT_MODE_MAX_QUALITY: return "max_quality";
    case AGENT_MODE_ECONOMY: return "economy";
    default: return "balanced";
    }
}

static AgentRole SelectRoleFromIntent(AgentIntent intent)
{
    switch (intent)
    {
    case INTENT_REVIEW: return AGENT_ROLE_REVIEWER;
    case INTENT_REFACTOR: return AGENT_ROLE_REFACTOR;
    case INTENT_RUN:
    case INTENT_BUILD: return AGENT_ROLE_SETUP;
    case INTENT_SEARCH:
    case INTENT_EXPLAIN: return AGENT_ROLE_RESEARCH;
    case INTENT_PATCH: return AGENT_ROLE_IMPLEMENTER;
    default: return AGENT_ROLE_PLANNER;
    }
}

static const AgentRoleContract* GetRoleContract(AgentRole role)
{
    static const AgentRoleContract contracts[] = {
        { AGENT_ROLE_PLANNER, "Planner Agent", "semantic_search,read_file,get_active_document,list_dir,open_file,context_store,eval_prompt,write_file,replace_in_file", 10, 0 },
        { AGENT_ROLE_IMPLEMENTER, "Implementer Agent", "semantic_search,read_file,get_active_document,write_file,replace_in_file,open_file,insert_in_editor,replace_editor_content,new_file_in_editor,list_dir,run_command,make_dir,design_audit,context_store", 15, 0 },
        { AGENT_ROLE_REVIEWER, "Reviewer Agent", "semantic_search,read_file,get_active_document,list_dir,open_file,run_command,write_file,replace_in_file,eval_prompt,red_team_prompt,design_audit", 10, 0 },
        { AGENT_ROLE_DEBUG, "Debug Agent", "semantic_search,read_file,get_active_document,run_command,list_dir,replace_in_file,write_file", 12, 0 },
        { AGENT_ROLE_TEST, "Test Agent", "semantic_search,read_file,get_active_document,run_command,list_dir,write_file,replace_in_file,eval_prompt,red_team_prompt", 10, 0 },
        { AGENT_ROLE_REFACTOR, "Refactor Agent", "semantic_search,read_file,get_active_document,replace_in_file,write_file,open_file,list_dir,run_command", 15, 0 },
        { AGENT_ROLE_RESEARCH, "Research Agent", "semantic_search,read_file,get_active_document,list_dir,web_search,context_store,write_file", 8, 0 },
        { AGENT_ROLE_SETUP, "Setup Agent", "semantic_search,read_file,get_active_document,list_dir,make_dir,init_repo,run_command,write_file,replace_in_file", 12, 0 }
    };

    for (int i = 0; i < (int)(sizeof(contracts) / sizeof(contracts[0])); i++)
    {
        if (contracts[i].role == role)
            return &contracts[i];
    }
    return &contracts[0];
}

static BOOL IsToolAllowedForRole(const AgentRoleContract* contract, const char* toolName)
{
    if (!contract || !contract->allowedTools || !toolName || !toolName[0])
        return FALSE;
    if (strcmp(toolName, "semantic_search") == 0)
        return TRUE;

    const char* pos = contract->allowedTools;
    size_t toolLen = strlen(toolName);
    while ((pos = strstr(pos, toolName)) != NULL)
    {
        BOOL leftOk = (pos == contract->allowedTools) || (*(pos - 1) == ',');
        BOOL rightOk = (pos[toolLen] == '\0') || (pos[toolLen] == ',');
        if (leftOk && rightOk)
            return TRUE;
        pos += toolLen;
    }
    return FALSE;
}

static int ExtractFirstInteger(const char* text)
{
    if (!text) return -1;
    for (const char* p = text; *p; p++)
    {
        if (*p >= '0' && *p <= '9')
            return atoi(p);
    }
    return -1;
}

static void ContextLedger_Persist(const ContextLedger* ledger, const char* mode, const char* intent,
                                  int touchedFiles, BOOL shadowMode, const char* topFile)
{
    if (!ledger) return;

    FILE* f = fopen("ai_context_ledger.log", "ab");
    if (!f) return;

    fprintf(f,
        "turn=%lu mode=%s intent=%s shadow=%d prompt_chars=%lu tool_calls=%lu useful=%lu blocked=%lu touched=%d top_file=%s\n",
        ledger->turnId,
        mode ? mode : "unknown",
        intent ? intent : "unknown",
        shadowMode ? 1 : 0,
        ledger->approxCharsSent,
        ledger->toolCalls,
        ledger->usefulToolCalls,
        ledger->blockedToolCalls,
        touchedFiles,
        topFile ? topFile : "-");
    fclose(f);
}

static void ContextLedger_RecordPrompt(ContextLedger* ledger, AIChatMessage* msgs, int count)
{
    if (!ledger || !msgs || count <= 0) return;
    for (int i = 0; i < count; i++)
    {
        if (msgs[i].content)
            ledger->approxCharsSent += (unsigned long)strlen(msgs[i].content);
    }
}

static BOOL TrackTouchedFile(const ToolCall* tc, char touched[MAX_TRACKED_FILES][260], int* n)
{
    if (!tc || !tc->path || !tc->path[0] || !touched || !n) return FALSE;
    for (int i = 0; i < *n; i++)
    {
        if (_stricmp(touched[i], tc->path) == 0)
            return TRUE;
    }
    if (*n < MAX_TRACKED_FILES)
    {
        _snprintf_s(touched[*n], 260, _TRUNCATE, "%s", tc->path);
        (*n)++;
    }
    return TRUE;
}

static int FindFailure(FailureEntry mem[MAX_FAILURE_MEMORY], int count, const char* tool, const char* target)
{
    for (int i = 0; i < count; i++)
    {
        if (_stricmp(mem[i].tool, tool ? tool : "") == 0 &&
            _stricmp(mem[i].target, target ? target : "") == 0)
            return i;
    }
    return -1;
}

static void FailureMemory_Add(FailureEntry mem[MAX_FAILURE_MEMORY], int* count, const char* tool, const char* target)
{
    int idx;
    if (!mem || !count) return;
    idx = FindFailure(mem, *count, tool, target);
    if (idx >= 0)
    {
        mem[idx].failures++;
        return;
    }
    if (*count >= MAX_FAILURE_MEMORY) return;
    _snprintf_s(mem[*count].tool, sizeof(mem[*count].tool), _TRUNCATE, "%s", tool ? tool : "");
    _snprintf_s(mem[*count].target, sizeof(mem[*count].target), _TRUNCATE, "%s", target ? target : "");
    mem[*count].failures = 1;
    (*count)++;
}

static BOOL FailureMemory_ShouldBlock(FailureEntry mem[MAX_FAILURE_MEMORY], int count, const ToolCall* tc)
{
    const char* target;
    int idx;
    if (!tc) return FALSE;
    target = tc->path ? tc->path : (tc->command ? tc->command : "");
    idx = FindFailure(mem, count, tc->name, target);
    return (idx >= 0 && mem[idx].failures >= 4);
}
static BOOL IsLikelyCodeWriteTask(const char* userMessage)
{
    static const char* positives[] = {
        "write ", "create ", "implement", "add ", "fix ", "edit ",
        "update ", "change ", "refactor", "build ", "make ", NULL
    };
    static const char* negatives[] = {
        "explain", "why ", "what does", "review", "summarize", NULL
    };

    if (!userMessage || !userMessage[0])
        return FALSE;

    for (int i = 0; negatives[i]; i++)
    {
        if (ContainsSubstringCI(userMessage, negatives[i]))
            return FALSE;
    }

    for (int i = 0; positives[i]; i++)
    {
        if (ContainsSubstringCI(userMessage, positives[i]))
            return TRUE;
    }

    return FALSE;
}

static BOOL ResponseLooksLikeCodeDump(const char* text)
{
    if (!text || !text[0])
        return FALSE;

    return strstr(text, "```") != NULL ||
        ContainsSubstringCI(text, "#include") ||
        ContainsSubstringCI(text, "function ") ||
        ContainsSubstringCI(text, "class ") ||
        ContainsSubstringCI(text, "def ") ||
        ContainsSubstringCI(text, "public static");
}

// Detect when the model says it will do something but never emits tool calls.
// Returns TRUE if the response contains planning language without actual action.
static BOOL ResponseLooksLikePlanningOnly(const char* text)
{
    static const char* planPhrases[] = {
        "I'll read ",   "I will read ",  "I'll check ",  "I will check ",
        "Let me read ", "Let me check ", "I'll examine ", "I'll look ",
        "I'll open ",   "I will open ",  "I'll inspect ", "I will inspect ",
        "I'll create ", "I will create ","I'll write ",  "I will write ",
        "I'll start ",  "I will start ", "Let me start ", "Let me examine ",
        "I'll understand ", "I will understand ",
        "First, I'll ", "First, I will ", "First, let me ",
        NULL
    };
    if (!text || !text[0]) return FALSE;
    for (int i = 0; planPhrases[i]; i++)
    {
        if (ContainsSubstringCI(text, planPhrases[i]))
            return TRUE;
    }
    return FALSE;
}

// Extract a plausible file path from the model's planning text.
// Looks for common file extensions. Returns a heap string or NULL.
static char* ExtractMentionedFilePath(const char* text)
{
    static const char* exts[] = {
        ".txt", ".c", ".h", ".py", ".js", ".ts", ".json", ".html", ".css",
        ".md", ".yml", ".yaml", ".toml", ".xml", ".java", ".go", ".rs",
        ".cpp", ".hpp", ".rb", ".php", ".sh", ".bat", ".ps1", ".cfg",
        ".ini", ".jsx", ".tsx", ".vue", ".svelte", NULL
    };
    if (!text) return NULL;

    for (int e = 0; exts[e]; e++)
    {
        const char* ext = strstr(text, exts[e]);
        if (!ext) continue;
        // Walk backwards from the extension to find the start of the path
        const char* start = ext;
        while (start > text && start[-1] != ' ' && start[-1] != '\n' &&
               start[-1] != '\t' && start[-1] != '"' && start[-1] != '\'' &&
               start[-1] != '(' && start[-1] != '[' && start[-1] != '`')
        {
            start--;
        }
        int pathLen = (int)(ext + (int)strlen(exts[e]) - start);
        if (pathLen > 1 && pathLen < 260)
        {
            char* path = (char*)malloc(pathLen + 1);
            if (path)
            {
                memcpy(path, start, pathLen);
                path[pathLen] = '\0';
                return path;
            }
        }
    }
    return NULL;
}

// Build a targeted tool-nudge correction message.
// Incorporates a concrete example if a file path was detected.
static char* BuildToolNudgeMessage(const char* response, int retryNumber)
{
    StrBuf sb;
    sb_init(&sb, 1024);

    sb_append(&sb,
        "CRITICAL: You are inside the Bikode tool execution engine. You MUST use tool calls to interact with the workspace. "
        "Do NOT describe what you plan to do. Actually DO it now by emitting tool call blocks.\n\n", -1);

    if (retryNumber > 1)
    {
        sb_append(&sb,
            "WARNING: This is your final retry. You MUST emit <tool_call>...</tool_call> blocks or your task will be marked as incomplete.\n\n", -1);
    }

    // Try to extract a mentioned file path for a concrete example
    {
        char* mentionedPath = ExtractMentionedFilePath(response);
        if (mentionedPath)
        {
            sb_append(&sb, "For example, to read the file you mentioned, emit exactly:\n\n", -1);
            sb_appendf(&sb,
                "<tool_call>\n"
                "{\"name\": \"read_file\", \"path\": \"%s\"}\n"
                "</tool_call>\n\n", mentionedPath);
            sb_append(&sb, "To create or write a file:\n\n", -1);
            sb_append(&sb,
                "<tool_call>\n"
                "{\"name\": \"write_file\", \"path\": \"path/to/file\", \"content\": \"file contents here\"}\n"
                "</tool_call>\n\n", -1);
            free(mentionedPath);
        }
        else
        {
            sb_append(&sb,
                "Start by listing the workspace directory to see what files exist:\n\n"
                "<tool_call>\n"
                "{\"name\": \"list_dir\", \"path\": \".\"}\n"
                "</tool_call>\n\n"
                "Then read files with read_file and write with write_file or replace_in_file.\n\n", -1);
        }
    }

    sb_append(&sb,
        "Respond ONLY with tool call blocks now. No planning text. Execute the tools.", -1);

    return sb.data;
}

static BOOL IsWorkspaceMutationTool(const char* toolName)
{
    return toolName &&
        (strcmp(toolName, "write_file") == 0 ||
         strcmp(toolName, "replace_in_file") == 0 ||
         strcmp(toolName, "replace_editor_content") == 0 ||
         strcmp(toolName, "new_file_in_editor") == 0 ||
         strcmp(toolName, "insert_in_editor") == 0 ||
         strcmp(toolName, "make_dir") == 0 ||
         strcmp(toolName, "init_repo") == 0);
}

static void AppendToolOperationSummary(StrBuf* sb, const ToolCall* tc)
{
    if (!sb || !tc)
        return;

    if (strcmp(tc->name, "write_file") == 0 && tc->path)
        sb_appendf(sb, "- Wrote %s\n", tc->path);
    else if (strcmp(tc->name, "replace_in_file") == 0 && tc->path)
        sb_appendf(sb, "- Updated %s\n", tc->path);
    else if (strcmp(tc->name, "open_file") == 0 && tc->path)
        sb_appendf(sb, "- Opened %s in the editor\n", tc->path);
    else if (strcmp(tc->name, "replace_editor_content") == 0)
        sb_append(sb, "- Replaced the active editor buffer\n", -1);
    else if (strcmp(tc->name, "new_file_in_editor") == 0)
        sb_append(sb, "- Created a new editor buffer\n", -1);
    else if (strcmp(tc->name, "insert_in_editor") == 0)
        sb_append(sb, "- Inserted text into the active editor\n", -1);
    else if (strcmp(tc->name, "make_dir") == 0 && tc->path)
        sb_appendf(sb, "- Ensured directory %s exists\n", tc->path);
    else if (strcmp(tc->name, "init_repo") == 0)
        sb_appendf(sb, "- Initialized a git repository%s%s\n",
                   tc->path && tc->path[0] ? " in " : "",
                   tc->path && tc->path[0] ? tc->path : "");
    else if (strcmp(tc->name, "run_command") == 0 && tc->command)
    {
        if (tc->cwd && tc->cwd[0])
            sb_appendf(sb, "- Ran command: %s (cwd: %s)\n", tc->command, tc->cwd);
        else
            sb_appendf(sb, "- Ran command: %s\n", tc->command);
    }
}

static char* BuildWorkspaceCompletionMessage(const StrBuf* ops, const char* cleanResponse,
                                             const ContextLedger* ledger, int touchedFiles, BOOL shadowMode)
{
    StrBuf sb;
    sb_init(&sb, 512);

    if (ops && ops->len > 0)
    {
        sb_append(&sb, "Applied the change directly in Bikode.\n", -1);
        sb_append(&sb, ops->data, ops->len);
    }
    else
    {
        sb_append(&sb, "Completed the workspace pass in Bikode.\n", -1);
    }

    sb_appendf(&sb,
        "\nDiff story:\n- mode: %s\n- files touched: %d\n- blocked actions: %lu\n- shadow mode: %s\n",
        shadowMode ? "preview" : "apply",
        touchedFiles,
        ledger ? ledger->blockedToolCalls : 0,
        shadowMode ? "yes" : "no");

    if (cleanResponse && cleanResponse[0] && !ResponseLooksLikeCodeDump(cleanResponse))
    {
        sb_append(&sb, "\n", 1);
        sb_append(&sb, cleanResponse, (int)strlen(cleanResponse) > 400 ? 400 : -1);
    }

    return sb.data;
}

//=============================================================================
// Agent thread
//=============================================================================

typedef struct {
    AIProviderConfig cfg;
    char*   szUserMessage;
    AIChatAttachment* attachments; // Copy of attachments
    int     attachmentCount;
    HWND    hwndTarget;
    HWND    hwndMainWnd;
} AgentParams;

static unsigned __stdcall AgentThreadProc(void* pArg)
{
    AgentParams* p = (AgentParams*)pArg;

    // Set main window handle for tools that need UI interaction
    s_hwndMainForTools = p->hwndMainWnd;

    AgentIntent intent = DetectIntent(p->szUserMessage);
    BudgetContract contract;
    BuildBudgetContract(&contract, p->szUserMessage, intent);
    AgentRole role = SelectRoleFromIntent(intent);
    const AgentRoleContract* roleContract = GetRoleContract(role);
    const BOOL shadowMode = ContainsSubstringCI(p->szUserMessage, "shadow mode") ||
                            ContainsSubstringCI(p->szUserMessage, "dry run");
    const BOOL wantsInternalTaskGraph = ContainsSubstringCI(p->szUserMessage, "task graph") ||
                                        ContainsSubstringCI(p->szUserMessage, "workflow graph");

    // Build system prompt
    char* systemPrompt = BuildSystemPrompt(&contract, intent);
    if (!systemPrompt)
    {
        char* err = _strdup("Error: Could not build system prompt");
        PostMessage(p->hwndTarget, WM_AI_DIRECT_RESPONSE, 0, (LPARAM)err);
        free(p->szUserMessage);
        if (p->attachments) free(p->attachments);
        free(p);
        InterlockedExchange(&s_bBusy, FALSE);
        return 1;
    }

    // Build message array: system + history + current user message
    AIChatMessage* msgs = (AIChatMessage*)calloc(MAX_MESSAGES_PER_CALL, sizeof(AIChatMessage));
    char** ownedStrings = (char**)calloc(MAX_MESSAGES_PER_CALL, sizeof(char*));
    int msgCount = 0;

    // System message
    msgs[msgCount].role = "system";
    msgs[msgCount].content = systemPrompt;
    ownedStrings[msgCount] = systemPrompt;
    msgCount++;

    if (intent != INTENT_ASK && msgCount < MAX_MESSAGES_PER_CALL - 4)
    {
        char* repoBrief = BuildLocalRepoBrief(p->szUserMessage);
        if (repoBrief && repoBrief[0])
        {
            msgs[msgCount].role = "system";
            msgs[msgCount].content = repoBrief;
            ownedStrings[msgCount] = repoBrief;
            msgCount++;
        }
        else if (repoBrief)
        {
            free(repoBrief);
        }
    }

    // Copy conversation history
    EnterCriticalSection(&s_csHistory);
    for (int i = 0; i < s_historyCount && msgCount < MAX_MESSAGES_PER_CALL - 10; i++)
    {
        msgs[msgCount].role = s_history[i].role;
        msgs[msgCount].content = s_history[i].content;
        // Don't mark as owned - history owns these strings
        msgCount++;
    }
    LeaveCriticalSection(&s_csHistory);

    // Current user message
    msgs[msgCount].role = "user";
    msgs[msgCount].content = p->szUserMessage;
    // Pass pointer to attachments (AIDirectCall will use them but not free them)
    if (p->attachmentCount > 0 && p->attachments) {
        msgs[msgCount].attachments = p->attachments;
        msgs[msgCount].attachmentCount = p->attachmentCount;
    }
    msgCount++;

    // Agent loop
    int iteration = 0;
    char* finalResponse = NULL;
    int toolNudgeRetries = 0;         // number of tool-nudge retries attempted (max 3)
    BOOL compactToolRetry = FALSE;
    BOOL workspaceMutated = FALSE;
    const BOOL codeWriteTask = IsLikelyCodeWriteTask(p->szUserMessage);
    StrBuf operationSummary;
    sb_init(&operationSummary, 512);
    ContextLedger ledger = { 0 };
    ledger.turnId = GetTickCount();
    FailureEntry failureMemory[MAX_FAILURE_MEMORY] = { 0 };
    int failureCount = 0;
    int totalToolCalls = 0;
    char touchedFiles[MAX_TRACKED_FILES][260] = { 0 };
    int touchedFileCount = 0;

    if (wantsInternalTaskGraph)
    {
        PostStatusToUI(p->hwndTarget, "Intent=%s, role=%s, mode=%s, budget: iterations<=%d, tool_calls<=%d",
            IntentName(intent), roleContract->name, ModeName(contract.mode), contract.maxIterations, contract.maxToolCalls);
        PostTaskGraphStatus(p->hwndTarget, intent);
    }

    if (shadowMode)
        PostStatusToUI(p->hwndTarget, "Shadow mode enabled: mutation tools will be previewed, not executed.");

    while (iteration < contract.maxIterations && iteration < MAX_AGENT_ITERATIONS)
    {
        iteration++;

        if (iteration > 1)
            PostStatusToUI(p->hwndTarget, "Thinking... (step %d)", iteration);

        // Call LLM
        ContextLedger_RecordPrompt(&ledger, msgs, msgCount);
        char* response = AIDirectCall_ChatMulti(&p->cfg, msgs, msgCount);
        if (!response)
        {
            finalResponse = _strdup("Error: No response from AI");
            break;
        }

        // Check for errors
        if (strncmp(response, "Error:", 6) == 0 ||
            strncmp(response, "API Error", 9) == 0)
        {
            if (iteration > 1 && !compactToolRetry && ContainsSubstringCI(response, "parsing the body") &&
                msgCount > 0 && msgs[msgCount - 1].role &&
                strcmp(msgs[msgCount - 1].role, "user") == 0)
            {
                char* compact = BuildCompactRetryMessage(msgs[msgCount - 1].content);
                if (compact)
                {
                    if (ownedStrings[msgCount - 1])
                        free(ownedStrings[msgCount - 1]);
                    msgs[msgCount - 1].content = compact;
                    ownedStrings[msgCount - 1] = compact;
                    compactToolRetry = TRUE;
                    free(response);
                    continue;
                }
            }

            finalResponse = response;
            break;
        }

        // Parse tool calls
        ToolCall* toolCalls = NULL;
        int toolCount = ParseToolCalls(response, &toolCalls);

        if ((totalToolCalls + toolCount) > contract.maxToolCalls)
        {
            ledger.blockedToolCalls += (unsigned long)toolCount;
            free(response);
            FreeToolCalls(toolCalls, toolCount);
            finalResponse = _strdup("Stopped before exceeding your tool-call budget. Ask for max quality mode if you want wider execution.");
            break;
        }

        if (toolCount == 0)
        {
            // Nudge the model to use tools: allow up to 3 retries with increasingly
            // forceful correction messages that include concrete tool call examples
            BOOL shouldNudge = (codeWriteTask || ResponseLooksLikePlanningOnly(response)) &&
                               !workspaceMutated &&
                               toolNudgeRetries < 3 &&
                               msgCount < MAX_MESSAGES_PER_CALL - 2;

            if (shouldNudge)
            {
                char* respCopy = _strdup(response);
                char* correction = BuildToolNudgeMessage(response, toolNudgeRetries + 1);

                if (respCopy && correction)
                {
                    msgs[msgCount].role = "assistant";
                    msgs[msgCount].content = respCopy;
                    ownedStrings[msgCount] = respCopy;
                    msgCount++;

                    msgs[msgCount].role = "user";
                    msgs[msgCount].content = correction;
                    ownedStrings[msgCount] = correction;
                    msgCount++;

                    toolNudgeRetries++;
                    free(response);
                    continue;
                }

                if (respCopy) free(respCopy);
                if (correction) free(correction);
            }

            if (codeWriteTask && !workspaceMutated && ResponseLooksLikeCodeDump(response))
            {
                free(response);
                finalResponse = _strdup(
                    "I wasn't able to apply that code directly in Bikode this time. "
                    "Please retry or specify a target file, and I'll write it into the workspace/editor instead of the chat."
                );
                break;
            }

            // No tool calls - this is the final answer
            finalResponse = response;
            break;
        }

        // Add assistant response to messages
        if (msgCount < MAX_MESSAGES_PER_CALL - 2)
        {
            char* respCopy = _strdup(response);
            msgs[msgCount].role = "assistant";
            msgs[msgCount].content = respCopy;
            ownedStrings[msgCount] = respCopy;
            msgCount++;
        }
        free(response);

        // Execute each tool call
        StrBuf toolResults;
        BOOL appendMoreToolResults = TRUE;
        sb_init(&toolResults, 4096);

        for (int i = 0; i < toolCount; i++)
        {
            char* result;
            char* safeResult = NULL;
            BOOL isMutationTool;

            // Post tool info to UI
            const char* detail = toolCalls[i].path ? toolCalls[i].path :
                                 toolCalls[i].command ? toolCalls[i].command : "";
            PostToolInfoToUI(p->hwndTarget, toolCalls[i].name, detail);

            // Execute
            if (!IsToolAllowedForRole(roleContract, toolCalls[i].name))
            {
                result = _strdup("Skipped: tool not allowed for active agent role contract");
                ledger.blockedToolCalls++;
            }
            else if (shadowMode && IsWorkspaceMutationTool(toolCalls[i].name))
            {
                result = _strdup("Shadow mode preview: mutation tool would execute here after approval");
                ledger.blockedToolCalls++;
            }
            else if (!contract.allowShell && strcmp(toolCalls[i].name, "run_command") == 0)
            {
                result = _strdup("Skipped: run_command disabled by budget contract");
                ledger.blockedToolCalls++;
            }
            else if (FailureMemory_ShouldBlock(failureMemory, failureCount, &toolCalls[i]))
            {
                result = _strdup("Skipped: similar tool attempt failed repeatedly in this session");
                ledger.blockedToolCalls++;
            }
            else
            {
                result = ExecuteTool(&toolCalls[i]);
                if (result && strncmp(result, "Error:", 6) == 0)
                {
                    FailureMemory_Add(failureMemory, &failureCount, toolCalls[i].name,
                        toolCalls[i].path ? toolCalls[i].path : toolCalls[i].command);
                }
                else
                {
                    ledger.usefulToolCalls++;
                }
            }
            totalToolCalls++;
            ledger.toolCalls++;
            TrackTouchedFile(&toolCalls[i], touchedFiles, &touchedFileCount);

            if (touchedFileCount > contract.maxTouchedFiles)
            {
                if (result) free(result);
                result = _strdup("Stopped: touched-file radius exceeded budget contract");
                ledger.blockedToolCalls++;
            }

            AppendToolOperationSummary(&operationSummary, &toolCalls[i]);
            isMutationTool = IsWorkspaceMutationTool(toolCalls[i].name);
            if (isMutationTool && !shadowMode)
                workspaceMutated = TRUE;

            if (isMutationTool && workspaceMutated && roleContract->stopOnFirstMutation)
            {
                if (result) free(result);
                result = _strdup("Role stop condition reached after first mutation");
                ledger.blockedToolCalls++;
                appendMoreToolResults = FALSE;
            }

            if (appendMoreToolResults)
            {
                sb_appendf(&toolResults, "[Tool result: %s", toolCalls[i].name);
                if (toolCalls[i].path)
                    sb_appendf(&toolResults, " - %s", toolCalls[i].path);
                sb_append(&toolResults, "]\n", -1);

                safeResult = PrepareToolResultForModel(toolCalls[i].name,
                    result ? result : "(no output)", MAX_TOOL_RESULT_SNIPPET);
                if (safeResult && safeResult[0])
                    sb_append(&toolResults, safeResult, -1);
                else
                    sb_append(&toolResults, "(no output)", -1);
                sb_append(&toolResults, "\n", 1);

                if (toolResults.len >= MAX_TOOL_FEEDBACK_SIZE)
                {
                    sb_append(&toolResults,
                        "\n[Additional tool output omitted to keep the request compact.]\n",
                        -1);
                    appendMoreToolResults = FALSE;
                }
            }

            if (safeResult)
                free(safeResult);
            if (result)
                free(result);
        }

        FreeToolCalls(toolCalls, toolCount);

        // Add tool results as user message
        if (msgCount < MAX_MESSAGES_PER_CALL)
        {
            msgs[msgCount].role = "user";
            msgs[msgCount].content = toolResults.data;
            ownedStrings[msgCount] = toolResults.data;
            msgCount++;
        }
        else
        {
            sb_free(&toolResults);
        }
    }

    if (!finalResponse)
    {
        finalResponse = _strdup(
            "I stopped at the current budget limit before getting to a usable final answer. "
            "Ask for balanced or max quality mode if you want a wider pass.");
    }

    // Save to history: user message + final response (not intermediate tool calls)
    AddToHistory("user", p->szUserMessage);
    AddToHistory("assistant", finalResponse);

    // Strip any residual tool call XML from the displayed response
    char* cleanResponse = StripToolCallTags(finalResponse);
    if (!cleanResponse || IsPlaceholderResponseText(cleanResponse))
    {
        if (cleanResponse) free(cleanResponse);
        cleanResponse = _strdup(
            "I finished the run, but the agent did not return a user-facing answer. "
            "Please retry if you want a clean summary.");
    }

    if (codeWriteTask && (workspaceMutated || operationSummary.len > 0))
    {
        char* summaryResponse = BuildWorkspaceCompletionMessage(&operationSummary, cleanResponse,
            &ledger, touchedFileCount, shadowMode);
        if (summaryResponse)
        {
            free(cleanResponse);
            cleanResponse = summaryResponse;
        }
    }

    {
        char ledgerMsg[512];
        _snprintf_s(ledgerMsg, sizeof(ledgerMsg), _TRUNCATE,
            "Context ledger: prompt_chars~%lu, tool_calls=%lu, useful=%lu, blocked=%lu, touched_files=%d",
            ledger.approxCharsSent, ledger.toolCalls, ledger.usefulToolCalls, ledger.blockedToolCalls, touchedFileCount);
        if (wantsInternalTaskGraph || ContainsSubstringCI(p->szUserMessage, "context ledger"))
            PostStatusToUI(p->hwndTarget, "%s", ledgerMsg);

        ContextLedger_Persist(&ledger,
            ModeName(contract.mode),
            IntentName(intent),
            touchedFileCount,
            shadowMode,
            touchedFileCount > 0 ? touchedFiles[0] : NULL);
    }

    // Post final response to UI
    PostMessage(p->hwndTarget, WM_AI_DIRECT_RESPONSE, 0, (LPARAM)cleanResponse);

    // Cleanup
    free(finalResponse);
    for (int i = 0; i < msgCount; i++)
    {
        if (ownedStrings[i]) free(ownedStrings[i]);
    }
    free(ownedStrings);
    free(msgs);
    sb_free(&operationSummary);
    free(p->szUserMessage);
    if (p->attachments) free(p->attachments);
    free(p);

    InterlockedExchange(&s_bBusy, FALSE);
    return 0;
}

//=============================================================================
// Public API
//=============================================================================

void AIAgent_Init(void)
{
    if (s_bInitialized) return;
    InitializeCriticalSection(&s_csHistory);
    s_historyCount = 0;
    s_bBusy = FALSE;
    s_bInitialized = TRUE;
}

void AIAgent_Shutdown(void)
{
    if (!s_bInitialized) return;

    AIAgent_ClearHistory();
    DeleteCriticalSection(&s_csHistory);
    s_bInitialized = FALSE;
}

BOOL AIAgent_ChatAsync(const AIProviderConfig* pCfg,
                       const char* szUserMessage,
                       const AIChatAttachment* pAttachments,
                       int cAttachments,
                       HWND hwndTarget,
                       HWND hwndMainWnd)
{
    if (!s_bInitialized) AIAgent_Init();

    // Prevent concurrent requests
    if (InterlockedCompareExchange(&s_bBusy, TRUE, FALSE) != FALSE)
        return FALSE;

    AgentParams* p = (AgentParams*)malloc(sizeof(AgentParams));
    if (!p) { InterlockedExchange(&s_bBusy, FALSE); return FALSE; }

    memcpy(&p->cfg, pCfg, sizeof(AIProviderConfig));
    p->szUserMessage = _strdup(szUserMessage ? szUserMessage : "");
    p->hwndTarget = hwndTarget;
    p->hwndMainWnd = hwndMainWnd;
    
    // Copy attachments
    if (pAttachments && cAttachments > 0) {
        p->attachmentCount = cAttachments;
        p->attachments = (AIChatAttachment*)calloc(cAttachments, sizeof(AIChatAttachment));
        if (p->attachments) {
            memcpy(p->attachments, pAttachments, cAttachments * sizeof(AIChatAttachment));
        } else {
            p->attachmentCount = 0; // fallback
        }
    } else {
        p->attachments = NULL;
        p->attachmentCount = 0;
    }

    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, AgentThreadProc, p, 0, NULL);
    if (!hThread)
    {
        free(p->szUserMessage);
        if (p->attachments) free(p->attachments);
        free(p);
        InterlockedExchange(&s_bBusy, FALSE);
        return FALSE;
    }

    CloseHandle(hThread);
    return TRUE;
}

void AIAgent_ClearHistory(void)
{
    EnterCriticalSection(&s_csHistory);
    for (int i = 0; i < s_historyCount; i++)
    {
        if (s_history[i].role)    free(s_history[i].role);
        if (s_history[i].content) free(s_history[i].content);
    }
    s_historyCount = 0;
    LeaveCriticalSection(&s_csHistory);
}

//=============================================================================
// Synchronous tool loop for AgentRuntime / Mission Control nodes
//=============================================================================

#define TOOLLOOP_MAX_ITER       12
#define TOOLLOOP_MAX_TOOLS      30
#define TOOLLOOP_MAX_MSGS       64
#define TOOLLOOP_MAX_RESULT     4096
#define TOOLLOOP_MAX_FEEDBACK   (12 * 1024)

void AIAgent_RunToolLoop(const AIProviderConfig* pCfg,
                         const char* systemPrompt,
                         const char* userPrompt,
                         int maxIter,
                         int maxTools,
                         AIAgentLoopCallback callback,
                         int cbNodeIndex,
                         AIAgentLoopResult* pResult)
{
    AIChatMessage* msgs;
    char** owned;
    int msgCount = 0;
    int iteration = 0;
    int totalToolCalls = 0;
    int filesChanged = 0;
    int totalInput = 0;
    int totalOutput = 0;
    char* finalResponse = NULL;
    int toolNudgeRetries = 0;         // allow up to 3 nudge retries
    int effectiveMaxIter = maxIter > 0 ? maxIter : TOOLLOOP_MAX_ITER;
    int effectiveMaxTools = maxTools > 0 ? maxTools : TOOLLOOP_MAX_TOOLS;

    if (!pResult) return;
    memset(pResult, 0, sizeof(*pResult));

    msgs = (AIChatMessage*)calloc(TOOLLOOP_MAX_MSGS, sizeof(AIChatMessage));
    owned = (char**)calloc(TOOLLOOP_MAX_MSGS, sizeof(char*));
    if (!msgs || !owned)
    {
        pResult->pszResult = _strdup("Error: Out of memory for agent loop");
        if (msgs) free(msgs);
        if (owned) free(owned);
        return;
    }

    // System prompt
    msgs[msgCount].role = "system";
    msgs[msgCount].content = systemPrompt;
    msgCount++;

    // User prompt
    msgs[msgCount].role = "user";
    msgs[msgCount].content = userPrompt;
    msgCount++;

    while (iteration < effectiveMaxIter)
    {
        AITokenUsage usage = { 0, 0 };
        char* response;
        ToolCall* toolCalls = NULL;
        int toolCount;

        iteration++;

        if (callback && iteration > 1)
        {
            char buf[128];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Thinking... (step %d/%d)", iteration, effectiveMaxIter);
            callback(cbNodeIndex, 0, buf);
        }

        response = AIDirectCall_ChatMultiEx(pCfg, msgs, msgCount, &usage);
        totalInput += usage.inputTokens;
        totalOutput += usage.outputTokens;

        if (!response)
        {
            finalResponse = _strdup("Error: No response from AI provider");
            break;
        }

        if (strncmp(response, "Error:", 6) == 0 ||
            strncmp(response, "API Error", 9) == 0)
        {
            finalResponse = response;
            break;
        }

        toolCount = ParseToolCalls(response, &toolCalls);

        if ((totalToolCalls + toolCount) > effectiveMaxTools)
        {
            free(response);
            FreeToolCalls(toolCalls, toolCount);
            finalResponse = _strdup("Stopped: tool-call budget exhausted.");
            break;
        }

        if (toolCount == 0)
        {
            // Model didn't use tools - nudge it with up to 3 increasingly
            // forceful retries that include concrete tool call examples
            if (filesChanged == 0 && toolNudgeRetries < 3 &&
                msgCount < TOOLLOOP_MAX_MSGS - 2)
            {
                char* respCopy = _strdup(response);
                char* correction = BuildToolNudgeMessage(response, toolNudgeRetries + 1);
                if (respCopy && correction)
                {
                    msgs[msgCount].role = "assistant";
                    msgs[msgCount].content = respCopy;
                    owned[msgCount] = respCopy;
                    msgCount++;

                    msgs[msgCount].role = "user";
                    msgs[msgCount].content = correction;
                    owned[msgCount] = correction;
                    msgCount++;

                    toolNudgeRetries++;
                    free(response);
                    continue;
                }
                if (respCopy) free(respCopy);
                if (correction) free(correction);
            }

            // No tool calls -- this is the final answer
            finalResponse = response;
            break;
        }

        // Add assistant response to history
        if (msgCount < TOOLLOOP_MAX_MSGS - 2)
        {
            char* respCopy = _strdup(response);
            msgs[msgCount].role = "assistant";
            msgs[msgCount].content = respCopy;
            owned[msgCount] = respCopy;
            msgCount++;
        }
        free(response);

        // Execute tool calls and build results
        {
            StrBuf toolResults;
            int i;
            sb_init(&toolResults, 4096);

            for (i = 0; i < toolCount; i++)
            {
                char* result;
                char* safeResult;

                if (callback)
                {
                    char buf[512];
                    const char* detail = toolCalls[i].path ? toolCalls[i].path :
                                         toolCalls[i].command ? toolCalls[i].command : "";
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%s: %s]", toolCalls[i].name, detail);
                    callback(cbNodeIndex, 1, buf);
                }

                result = ExecuteTool(&toolCalls[i]);
                totalToolCalls++;

                if (IsWorkspaceMutationTool(toolCalls[i].name) &&
                    result && strncmp(result, "Error:", 6) != 0)
                {
                    filesChanged++;
                }

                sb_appendf(&toolResults, "[Tool result: %s", toolCalls[i].name);
                if (toolCalls[i].path)
                    sb_appendf(&toolResults, " - %s", toolCalls[i].path);
                sb_append(&toolResults, "]\n", -1);

                safeResult = PrepareToolResultForModel(toolCalls[i].name,
                    result ? result : "(no output)", TOOLLOOP_MAX_RESULT);
                if (safeResult && safeResult[0])
                    sb_append(&toolResults, safeResult, -1);
                else
                    sb_append(&toolResults, "(no output)", -1);
                sb_append(&toolResults, "\n", 1);

                if (safeResult) free(safeResult);
                if (result) free(result);

                if (toolResults.len >= TOOLLOOP_MAX_FEEDBACK)
                {
                    sb_append(&toolResults,
                        "\n[Additional tool output omitted to keep the request compact.]\n", -1);
                    break;
                }
            }

            FreeToolCalls(toolCalls, toolCount);

            // Add tool results as user message
            if (msgCount < TOOLLOOP_MAX_MSGS)
            {
                msgs[msgCount].role = "user";
                msgs[msgCount].content = toolResults.data;
                owned[msgCount] = toolResults.data;
                msgCount++;
            }
            else
            {
                sb_free(&toolResults);
            }
        }
    }

    if (!finalResponse)
    {
        finalResponse = _strdup(
            "Stopped at the iteration limit before getting a final answer.");
    }

    pResult->pszResult = finalResponse;
    pResult->inputTokens = totalInput;
    pResult->outputTokens = totalOutput;
    pResult->toolCalls = totalToolCalls;
    pResult->filesChanged = filesChanged;

    // Cleanup owned strings (skip index 0 and 1 which are systemPrompt/userPrompt not owned)
    {
        int i;
        for (i = 0; i < msgCount; i++)
        {
            if (owned[i]) free(owned[i]);
        }
    }
    free(owned);
    free(msgs);
}
