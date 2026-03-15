/******************************************************************************
*
* Biko
*
* AgentRuntime.c
*   Mission Control runtime and org-spec orchestration.
*
******************************************************************************/

#include "AgentRuntime.h"
#include "AIDirectCall.h"
#include "AISubscriptionAgent.h"
#include "AIAgent.h"
#include "AIBridge.h"
#include "CodeEmbeddingIndex.h"
#include "FileManager.h"
#include "ProofTray.h"
#include "Terminal.h"
#include "Externals.h"
#include "mono_json.h"
#include "CommonUtils.h"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <process.h>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

typedef struct StrBuf {
    char* data;
    int len;
    int cap;
} StrBuf;

typedef struct FileSnapshotEntry {
    WCHAR relativePath[MAX_PATH];
    FILETIME writeTime;
    DWORD sizeHigh;
    DWORD sizeLow;
} FileSnapshotEntry;

typedef struct NodeThreadCtx {
    int nodeIndex;
} NodeThreadCtx;

typedef struct RuntimeContext {
    HWND hwndMain;
    CRITICAL_SECTION cs;
    BOOL initialized;
    HANDLE schedulerThread;
    HANDLE nodeThreads[AGENT_RUNTIME_MAX_NODES];
    volatile LONG isRunning;
    volatile LONG isPaused;
    volatile LONG isCanceled;
    int nextEventSeq;
    OrgSpec org;
    AgentNodeSnapshot nodes[AGENT_RUNTIME_MAX_NODES];
    AgentEvent events[AGENT_RUNTIME_MAX_EVENTS];
    int eventCount;
    WCHAR workspaceRoot[MAX_PATH];
    WCHAR runRoot[MAX_PATH];
    char runId[AGENT_RUNTIME_TEXT_SMALL];
    WCHAR lastSelectedOrg[MAX_PATH];
} RuntimeContext;

static RuntimeContext s_runtime;
static WCHAR s_workspaceRootBuf[MAX_PATH];
extern WCHAR szCurFile[MAX_PATH + 40];

static unsigned __stdcall RuntimeSchedulerThreadProc(void* pParam);
static unsigned __stdcall RuntimeNodeThreadProc(void* pParam);
static const char* SkipWhitespace(const char* p);
static char* JsonExtractString(const char* json, const char* key);
static char* NormalizeCommandExecutionText(const char* text);
static char* WideToUtf8Dup(LPCWSTR wszText);

static void sb_init(StrBuf* sb, int cap)
{
    if (!sb) return;
    sb->cap = cap > 256 ? cap : 256;
    sb->len = 0;
    sb->data = (char*)malloc(sb->cap);
    if (sb->data)
        sb->data[0] = '\0';
}

static void sb_append(StrBuf* sb, const char* text, int len)
{
    char* grown;
    int need;
    if (!sb || !sb->data || !text)
        return;
    if (len < 0)
        len = (int)strlen(text);
    need = sb->len + len + 1;
    if (need > sb->cap)
    {
        int nextCap = sb->cap * 2;
        if (nextCap < need)
            nextCap = need + 256;
        grown = (char*)realloc(sb->data, nextCap);
        if (!grown)
            return;
        sb->data = grown;
        sb->cap = nextCap;
    }
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(StrBuf* sb, const char* fmt, ...)
{
    char buffer[4096];
    va_list args;
    int written;
    va_start(args, fmt);
    written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written > 0)
        sb_append(sb, buffer, written);
}

static void sb_free(StrBuf* sb)
{
    if (!sb)
        return;
    if (sb->data)
        free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static void CopyStringSafe(char* dst, int cchDst, const char* src)
{
    if (!dst || cchDst <= 0)
        return;
    dst[0] = '\0';
    if (!src)
        return;
    strncpy(dst, src, cchDst - 1);
    dst[cchDst - 1] = '\0';
}

static void CopyWideSafe(WCHAR* dst, int cchDst, LPCWSTR src)
{
    if (!dst || cchDst <= 0)
        return;
    dst[0] = L'\0';
    if (!src)
        return;
    lstrcpynW(dst, src, cchDst);
}

static char* DupString(const char* text)
{
    size_t len;
    char* out;
    if (!text)
        return NULL;
    len = strlen(text);
    out = (char*)malloc(len + 1);
    if (out)
        memcpy(out, text, len + 1);
    return out;
}

static char* NormalizeQuotedShellPayload(const char* text)
{
    StrBuf sb;
    char quote;

    if (!text || (*text != '"' && *text != '\''))
        return DupString(text ? text : "");

    quote = *text++;
    sb_init(&sb, 128);
    while (*text && *text != quote)
    {
        if (*text == '\\' && *(text + 1))
        {
            text++;
            switch (*text)
            {
            case 'n': sb_append(&sb, "\n", 1); break;
            case 'r': sb_append(&sb, "\r", 1); break;
            case 't': sb_append(&sb, "\t", 1); break;
            case '\\': sb_append(&sb, "\\", 1); break;
            case '"': sb_append(&sb, "\"", 1); break;
            case '\'': sb_append(&sb, "\'", 1); break;
            default: sb_append(&sb, text, 1); break;
            }
        }
        else
        {
            sb_append(&sb, text, 1);
        }
        text++;
    }
    return sb.data ? sb.data : DupString("");
}

static char* NormalizeCommandExecutionText(const char* text)
{
    const char* commandArg;

    if (!text || !text[0])
        return DupString("");

    commandArg = StrStrIA(text, " -Command ");
    if (commandArg)
    {
        commandArg = SkipWhitespace(commandArg + 10);
        return NormalizeQuotedShellPayload(commandArg);
    }

    commandArg = StrStrIA(text, " /c ");
    if (commandArg)
    {
        commandArg = SkipWhitespace(commandArg + 4);
        return NormalizeQuotedShellPayload(commandArg);
    }

    return DupString(text);
}

static AgentBackend BackendForChatAccessMode(EAIChatAccessMode mode)
{
    switch (mode)
    {
    case AI_CHAT_ACCESS_CODEX:
        return AGENT_BACKEND_CODEX;
    case AI_CHAT_ACCESS_CLAUDE:
        return AGENT_BACKEND_CLAUDE;
    case AI_CHAT_ACCESS_CODEX_CLAUDE:
        return AGENT_BACKEND_RELAY;
    case AI_CHAT_ACCESS_LOCAL:
        return AGENT_BACKEND_LOCAL;
    case AI_CHAT_ACCESS_API_PROVIDER:
    default:
        return AGENT_BACKEND_API;
    }
}

static AgentBackend ResolveRuntimeBackend(AgentBackend requested, EAIChatAccessMode mode)
{
    AgentBackend selected = BackendForChatAccessMode(mode);

    if (mode == AI_CHAT_ACCESS_API_PROVIDER)
        return AGENT_BACKEND_API;
    if (mode == AI_CHAT_ACCESS_LOCAL)
        return AGENT_BACKEND_LOCAL;
    if (requested == AGENT_BACKEND_API)
        return AGENT_BACKEND_API;
    if (requested == AGENT_BACKEND_LOCAL)
        return AGENT_BACKEND_LOCAL;
    return selected;
}

static void ApplyConfiguredRuntimeModel(OrgNodeSpec* node, const AIConfig* pCfg)
{
    char* modelUtf8;

    if (!node || !pCfg)
        return;

    node->model[0] = '\0';
    if (node->backend == AGENT_BACKEND_API)
    {
        if (pCfg->providerCfg.szModel[0])
            CopyStringSafe(node->model, COUNTOF(node->model), pCfg->providerCfg.szModel);
        return;
    }

    if (!pCfg->wszChatDriverModel[0])
        return;

    modelUtf8 = WideToUtf8Dup(pCfg->wszChatDriverModel);
    if (modelUtf8)
    {
        CopyStringSafe(node->model, COUNTOF(node->model), modelUtf8);
        free(modelUtf8);
    }
}

static BOOL IsPlaceholderAgentResult(const char* text)
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
            buffer[len++] = *text;
        text++;
    }
    buffer[len] = '\0';

    return buffer[0] == '\0' ||
        _stricmp(buffer, "done") == 0 ||
        _stricmp(buffer, "done.") == 0;
}

static BOOL IsMissingFinalAnswerError(const char* text)
{
    if (!text)
        return FALSE;

    return strcmp(text, "Error: The embedded agent finished without returning a user-facing answer.") == 0 ||
        strcmp(text, "Error: The embedded agent finished without returning a usable final answer.") == 0 ||
        strcmp(text, "Error: Agent exited without a final response.") == 0;
}

static char* BuildRelayHandoffSummary(const char* codexResult)
{
    if (codexResult &&
        codexResult[0] &&
        strncmp(codexResult, "Error:", 6) != 0 &&
        !IsPlaceholderAgentResult(codexResult))
    {
        return DupString(codexResult);
    }

    return DupString(
        "Codex completed an implementation pass in the workspace but did not leave a polished textual handoff. "
        "Review the current files on disk, verify the actual edits, and write the final user-facing summary from that workspace state.");
}

static char* BuildWorkspaceChangeFallbackResult(const char* nodeTitle, int changedCount, LPCWSTR wszFirstChanged)
{
    StrBuf sb;
    char* firstChangedUtf8 = NULL;

    sb_init(&sb, 512);
    sb_appendf(&sb,
        "%s changed %d file%s but exited before leaving a polished final handoff. "
        "Continue from the saved workspace diff and transcript instead of treating the implementation pass as lost.",
        (nodeTitle && nodeTitle[0]) ? nodeTitle : "This agent",
        changedCount,
        changedCount == 1 ? "" : "s");

    if (wszFirstChanged && wszFirstChanged[0])
    {
        firstChangedUtf8 = WideToUtf8Dup(wszFirstChanged);
        if (firstChangedUtf8 && firstChangedUtf8[0])
            sb_appendf(&sb, " First changed file: %s.", firstChangedUtf8);
    }

    if (firstChangedUtf8)
        free(firstChangedUtf8);
    return sb.data;
}

static char* WideToUtf8Dup(LPCWSTR wszText)
{
    int needed;
    char* out;
    if (!wszText || !wszText[0])
        return DupString("");
    needed = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
    if (needed <= 0)
        return DupString("");
    out = (char*)malloc(needed);
    if (!out)
        return DupString("");
    WideCharToMultiByte(CP_UTF8, 0, wszText, -1, out, needed, NULL, NULL);
    return out;
}

static void Utf8ToWide(const char* text, WCHAR* wszOut, int cchOut)
{
    if (!wszOut || cchOut <= 0)
        return;
    wszOut[0] = L'\0';
    if (text && text[0])
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wszOut, cchOut);
}

static BOOL EnsureDirExists(LPCWSTR wszPath)
{
    DWORD attrs;
    WCHAR wszParent[MAX_PATH];
    if (!wszPath || !wszPath[0])
        return FALSE;
    attrs = GetFileAttributesW(wszPath);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return TRUE;
    lstrcpynW(wszParent, wszPath, ARRAYSIZE(wszParent));
    PathRemoveFileSpecW(wszParent);
    if (wszParent[0] && _wcsicmp(wszParent, wszPath) != 0)
        EnsureDirExists(wszParent);
    if (CreateDirectoryW(wszPath, NULL))
        return TRUE;
    attrs = GetFileAttributesW(wszPath);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL EnsureParentDirExists(LPCWSTR wszPath)
{
    WCHAR wszDir[MAX_PATH];
    if (!wszPath || !wszPath[0])
        return FALSE;
    lstrcpynW(wszDir, wszPath, ARRAYSIZE(wszDir));
    PathRemoveFileSpecW(wszDir);
    return EnsureDirExists(wszDir);
}

static BOOL WriteUtf8File(LPCWSTR wszPath, const char* text)
{
    HANDLE hFile;
    DWORD written = 0;
    DWORD len;
    if (!wszPath || !text)
        return FALSE;
    EnsureParentDirExists(wszPath);
    hFile = CreateFileW(wszPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    len = (DWORD)strlen(text);
    if (len > 0 && !WriteFile(hFile, text, len, &written, NULL))
    {
        CloseHandle(hFile);
        return FALSE;
    }
    CloseHandle(hFile);
    return TRUE;
}

static BOOL AppendUtf8FileLine(LPCWSTR wszPath, const char* text)
{
    HANDLE hFile;
    DWORD written = 0;
    DWORD len;
    if (!wszPath || !text)
        return FALSE;
    EnsureParentDirExists(wszPath);
    hFile = CreateFileW(wszPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;
    SetFilePointer(hFile, 0, NULL, FILE_END);
    len = (DWORD)strlen(text);
    if (len > 0 && !WriteFile(hFile, text, len, &written, NULL))
    {
        CloseHandle(hFile);
        return FALSE;
    }
    if (!WriteFile(hFile, "\r\n", 2, &written, NULL))
    {
        CloseHandle(hFile);
        return FALSE;
    }
    CloseHandle(hFile);
    return TRUE;
}

static char* ReadUtf8File(LPCWSTR wszPath, DWORD* pcbOut)
{
    HANDLE hFile;
    LARGE_INTEGER size;
    DWORD read = 0;
    char* buffer;
    if (pcbOut)
        *pcbOut = 0;
    hFile = CreateFileW(wszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(hFile, &size) || size.HighPart != 0 || size.LowPart > (32 * 1024 * 1024))
    {
        CloseHandle(hFile);
        return NULL;
    }
    buffer = (char*)malloc(size.LowPart + 1);
    if (!buffer)
    {
        CloseHandle(hFile);
        return NULL;
    }
    if (size.LowPart > 0 && !ReadFile(hFile, buffer, size.LowPart, &read, NULL))
    {
        free(buffer);
        CloseHandle(hFile);
        return NULL;
    }
    buffer[read] = '\0';
    if (pcbOut)
        *pcbOut = read;
    CloseHandle(hFile);
    return buffer;
}

static BOOL ShouldSkipWorkspaceChild(const WIN32_FIND_DATAW* pfd)
{
    const WCHAR* name;
    if (!pfd)
        return TRUE;
    name = pfd->cFileName;
    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
        return TRUE;
    if (_wcsicmp(name, L".git") == 0 ||
        _wcsicmp(name, L".bikode") == 0 ||
        _wcsicmp(name, L"node_modules") == 0 ||
        _wcsicmp(name, L"__pycache__") == 0 ||
        _wcsicmp(name, L".vs") == 0)
        return TRUE;
    return FALSE;
}

static BOOL CopyTreeRecursive(LPCWSTR wszSrc, LPCWSTR wszDst)
{
    WCHAR wszPattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;
    if (!EnsureDirExists(wszDst))
        return FALSE;
    if (FAILED(StringCchPrintfW(wszPattern, ARRAYSIZE(wszPattern), L"%s\\*", wszSrc)))
        return FALSE;
    hFind = FindFirstFileW(wszPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return TRUE;
    do
    {
        WCHAR wszChildSrc[MAX_PATH];
        WCHAR wszChildDst[MAX_PATH];
        if (ShouldSkipWorkspaceChild(&fd))
            continue;
        if (FAILED(StringCchPrintfW(wszChildSrc, ARRAYSIZE(wszChildSrc), L"%s\\%s", wszSrc, fd.cFileName)) ||
            FAILED(StringCchPrintfW(wszChildDst, ARRAYSIZE(wszChildDst), L"%s\\%s", wszDst, fd.cFileName)))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!CopyTreeRecursive(wszChildSrc, wszChildDst))
            {
                FindClose(hFind);
                return FALSE;
            }
        }
        else
        {
            EnsureParentDirExists(wszChildDst);
            if (!CopyFileW(wszChildSrc, wszChildDst, FALSE))
            {
                FindClose(hFind);
                return FALSE;
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return TRUE;
}

static int __cdecl CompareFileSnapshotEntry(const void* a, const void* b)
{
    const FileSnapshotEntry* left = (const FileSnapshotEntry*)a;
    const FileSnapshotEntry* right = (const FileSnapshotEntry*)b;
    return _wcsicmp(left->relativePath, right->relativePath);
}

static void CaptureWorkspaceSnapshotRecursive(LPCWSTR wszRoot, LPCWSTR wszDir,
                                              FileSnapshotEntry* entries, int maxEntries, int* pCount)
{
    WCHAR wszPattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;
    if (!wszRoot || !wszDir || !entries || !pCount || *pCount >= maxEntries)
        return;
    if (FAILED(StringCchPrintfW(wszPattern, ARRAYSIZE(wszPattern), L"%s\\*", wszDir)))
        return;
    hFind = FindFirstFileW(wszPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;
    do
    {
        WCHAR wszChild[MAX_PATH];
        const WCHAR* wszRelative;
        if (ShouldSkipWorkspaceChild(&fd))
            continue;
        if (FAILED(StringCchPrintfW(wszChild, ARRAYSIZE(wszChild), L"%s\\%s", wszDir, fd.cFileName)))
            continue;
        wszRelative = wszChild + lstrlenW(wszRoot);
        if (*wszRelative == L'\\' || *wszRelative == L'/')
            wszRelative++;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            CaptureWorkspaceSnapshotRecursive(wszRoot, wszChild, entries, maxEntries, pCount);
        }
        else if (*pCount < maxEntries)
        {
            FileSnapshotEntry* entry = &entries[*pCount];
            lstrcpynW(entry->relativePath, wszRelative, ARRAYSIZE(entry->relativePath));
            entry->writeTime = fd.ftLastWriteTime;
            entry->sizeHigh = fd.nFileSizeHigh;
            entry->sizeLow = fd.nFileSizeLow;
            (*pCount)++;
        }
    } while (*pCount < maxEntries && FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

static int CaptureWorkspaceSnapshot(LPCWSTR wszRoot, FileSnapshotEntry* entries, int maxEntries)
{
    int count = 0;
    if (!wszRoot || !wszRoot[0] || !entries || maxEntries <= 0)
        return 0;
    CaptureWorkspaceSnapshotRecursive(wszRoot, wszRoot, entries, maxEntries, &count);
    if (count > 1)
        qsort(entries, count, sizeof(FileSnapshotEntry), CompareFileSnapshotEntry);
    return count;
}

static void AppendWorkspacePromptSnapshot(StrBuf* sb, LPCWSTR wszRoot)
{
    FileSnapshotEntry entries[160];
    BOOL selected[160] = { FALSE };
    int count;
    int listed = 0;
    WCHAR wszPattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    if (!sb || !wszRoot || !wszRoot[0])
        return;

    count = CaptureWorkspaceSnapshot(wszRoot, entries, ARRAYSIZE(entries));
    sb_appendf(sb, "Workspace snapshot: tracked files=%d\n", count);

    if (FAILED(StringCchPrintfW(wszPattern, ARRAYSIZE(wszPattern), L"%s\\*", wszRoot)))
        return;

    hFind = FindFirstFileW(wszPattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        sb_append(sb, "Top-level entries:\n", -1);
        do
        {
            char* entryUtf8;
            if (ShouldSkipWorkspaceChild(&fd))
                continue;
            entryUtf8 = WideToUtf8Dup(fd.cFileName);
            if (entryUtf8)
            {
                sb_appendf(sb, "- %s%s\n",
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "[dir] " : "",
                    entryUtf8);
                free(entryUtf8);
            }
            listed++;
        } while (listed < 12 && FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (count > 0)
    {
        sb_append(sb, "Recent files in snapshot:\n", -1);
        for (int sample = 0; sample < min(count, 8); sample++)
        {
            int best = -1;
            for (int i = 0; i < count; i++)
            {
                if (selected[i])
                    continue;
                if (best < 0 || CompareFileTime(&entries[i].writeTime, &entries[best].writeTime) > 0)
                    best = i;
            }
            if (best < 0)
                break;
            selected[best] = TRUE;
            {
                char* relUtf8 = WideToUtf8Dup(entries[best].relativePath);
                if (relUtf8)
                {
                    sb_appendf(sb, "- %s\n", relUtf8);
                    free(relUtf8);
                }
            }
        }
    }
}

static int CollectChangedFiles(LPCWSTR wszRoot,
                               const FileSnapshotEntry* beforeEntries, int beforeCount,
                               AgentNodeSnapshot* pNode)
{
    FileSnapshotEntry afterEntries[512];
    int afterCount;
    int i = 0;
    int j = 0;
    int changed = 0;
    if (!wszRoot || !pNode)
        return 0;
    afterCount = CaptureWorkspaceSnapshot(wszRoot, afterEntries, ARRAYSIZE(afterEntries));
    while (i < beforeCount || j < afterCount)
    {
        int cmp;
        if (i >= beforeCount)
            cmp = 1;
        else if (j >= afterCount)
            cmp = -1;
        else
            cmp = _wcsicmp(beforeEntries[i].relativePath, afterEntries[j].relativePath);
        if (cmp < 0)
        {
            i++;
            continue;
        }
        if (cmp > 0)
        {
            if (changed < AGENT_RUNTIME_MAX_CHANGED_FILES)
                PathCombineW(pNode->changedFiles[changed], wszRoot, afterEntries[j].relativePath);
            changed++;
            j++;
            continue;
        }
        if (CompareFileTime(&beforeEntries[i].writeTime, &afterEntries[j].writeTime) != 0 ||
            beforeEntries[i].sizeHigh != afterEntries[j].sizeHigh ||
            beforeEntries[i].sizeLow != afterEntries[j].sizeLow)
        {
            if (changed < AGENT_RUNTIME_MAX_CHANGED_FILES)
                PathCombineW(pNode->changedFiles[changed], wszRoot, afterEntries[j].relativePath);
            changed++;
        }
        i++;
        j++;
    }
    pNode->fileCount = changed;
    return changed;
}

static AgentBackend ParseBackend(const char* text)
{
    if (!text)
        return AGENT_BACKEND_API;
    if (_stricmp(text, "codex") == 0)
        return AGENT_BACKEND_CODEX;
    if (_stricmp(text, "claude") == 0)
        return AGENT_BACKEND_CLAUDE;
    if (_stricmp(text, "relay") == 0)
        return AGENT_BACKEND_RELAY;
    if (_stricmp(text, "local") == 0)
        return AGENT_BACKEND_LOCAL;
    return AGENT_BACKEND_API;
}

static const char* BackendSpecName(AgentBackend backend)
{
    switch (backend)
    {
    case AGENT_BACKEND_CODEX:  return "codex";
    case AGENT_BACKEND_CLAUDE: return "claude";
    case AGENT_BACKEND_RELAY:  return "relay";
    case AGENT_BACKEND_LOCAL:  return "local";
    default:                   return "api";
    }
}

const char* AgentRuntime_BackendLabel(AgentBackend backend)
{
    switch (backend)
    {
    case AGENT_BACKEND_CODEX:  return "Codex";
    case AGENT_BACKEND_CLAUDE: return "Claude";
    case AGENT_BACKEND_RELAY:  return "Relay";
    case AGENT_BACKEND_LOCAL:  return "Local";
    default:                   return "API";
    }
}

static AgentWorkspacePolicy ParseWorkspacePolicyText(const char* text)
{
    if (!text)
        return AGENT_WORKSPACE_ISOLATED;
    if (_stricmp(text, "shared-read") == 0 || _stricmp(text, "shared-readonly") == 0)
        return AGENT_WORKSPACE_SHARED_READONLY;
    if (_stricmp(text, "shared-write") == 0 || _stricmp(text, "shared") == 0 || _stricmp(text, "shared-mutating") == 0)
        return AGENT_WORKSPACE_SHARED_MUTATING;
    return AGENT_WORKSPACE_ISOLATED;
}

static const char* WorkspacePolicySpecName(AgentWorkspacePolicy policy)
{
    switch (policy)
    {
    case AGENT_WORKSPACE_SHARED_READONLY: return "shared-read";
    case AGENT_WORKSPACE_SHARED_MUTATING: return "shared-write";
    default:                              return "isolated";
    }
}

const char* AgentRuntime_WorkspaceLabel(AgentWorkspacePolicy policy)
{
    switch (policy)
    {
    case AGENT_WORKSPACE_SHARED_READONLY: return "Shared read";
    case AGENT_WORKSPACE_SHARED_MUTATING: return "Shared write";
    default:                              return "Isolated";
    }
}

const char* AgentRuntime_StateLabel(AgentNodeState state)
{
    switch (state)
    {
    case AGENT_NODE_QUEUED:   return "Queued";
    case AGENT_NODE_BLOCKED:  return "Blocked";
    case AGENT_NODE_RUNNING:  return "Running";
    case AGENT_NODE_DONE:     return "Done";
    case AGENT_NODE_ERROR:    return "Error";
    case AGENT_NODE_CANCELED: return "Canceled";
    case AGENT_NODE_PAUSED:   return "Paused";
    default:                  return "Idle";
    }
}

static void MakeSlugFromUtf8(const char* text, WCHAR* wszOut, int cchOut)
{
    WCHAR wszWide[256];
    int outPos = 0;
    int i;
    if (!wszOut || cchOut <= 0)
        return;
    wszOut[0] = L'\0';
    Utf8ToWide(text, wszWide, ARRAYSIZE(wszWide));
    for (i = 0; wszWide[i] && outPos < cchOut - 1; i++)
    {
        WCHAR ch = wszWide[i];
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9'))
            wszOut[outPos++] = ch;
        else if (ch >= L'A' && ch <= L'Z')
            wszOut[outPos++] = (WCHAR)(ch - L'A' + L'a');
        else if (outPos > 0 && wszOut[outPos - 1] != L'-')
            wszOut[outPos++] = L'-';
    }
    wszOut[outPos] = L'\0';
    if (!wszOut[0])
        lstrcpynW(wszOut, L"org", cchOut);
}

static const WCHAR* RuntimeWorkspaceRoot(void)
{
    if (s_runtime.workspaceRoot[0])
        return s_runtime.workspaceRoot;
    if (FileManager_GetRootPath()[0])
    {
        lstrcpynW(s_workspaceRootBuf, FileManager_GetRootPath(), ARRAYSIZE(s_workspaceRootBuf));
        return s_workspaceRootBuf;
    }
    if (szCurFile[0])
    {
        lstrcpynW(s_workspaceRootBuf, szCurFile, ARRAYSIZE(s_workspaceRootBuf));
        PathRemoveFileSpecW(s_workspaceRootBuf);
        return s_workspaceRootBuf;
    }
    GetCurrentDirectoryW(ARRAYSIZE(s_workspaceRootBuf), s_workspaceRootBuf);
    return s_workspaceRootBuf;
}

const WCHAR* AgentRuntime_GetWorkspaceRoot(void)
{
    return RuntimeWorkspaceRoot();
}

static void AppendOrgNode(OrgSpec* pSpec, const char* id, const char* title, const char* role,
                          AgentBackend backend, AgentWorkspacePolicy workspace,
                          const char* prompt, const char* group)
{
    OrgNodeSpec* node;
    if (!pSpec || pSpec->nodeCount >= AGENT_RUNTIME_MAX_NODES)
        return;
    node = &pSpec->nodes[pSpec->nodeCount++];
    ZeroMemory(node, sizeof(*node));
    CopyStringSafe(node->id, COUNTOF(node->id), id);
    CopyStringSafe(node->title, COUNTOF(node->title), title);
    CopyStringSafe(node->role, COUNTOF(node->role), role);
    node->backend = backend;
    node->workspacePolicy = workspace;
    CopyStringSafe(node->prompt, COUNTOF(node->prompt), prompt);
    CopyStringSafe(node->group, COUNTOF(node->group), group);
}

static void AddDependency(OrgNodeSpec* node, const char* dependency)
{
    if (!node || !dependency || !dependency[0] || node->dependsOnCount >= AGENT_RUNTIME_MAX_DEPENDS)
        return;
    CopyStringSafe(node->dependsOn[node->dependsOnCount++], AGENT_RUNTIME_TEXT_SMALL, dependency);
}

static void InitTemplateSpec(OrgSpec* pSpec, LPCWSTR wszRoot, LPCWSTR wszPath,
                             const char* name, const char* layout)
{
    ZeroMemory(pSpec, sizeof(*pSpec));
    pSpec->version = 1;
    CopyWideSafe(pSpec->root, COUNTOF(pSpec->root), wszRoot);
    CopyWideSafe(pSpec->path, COUNTOF(pSpec->path), wszPath);
    CopyStringSafe(pSpec->name, COUNTOF(pSpec->name), name);
    CopyStringSafe(pSpec->layout, COUNTOF(pSpec->layout), layout);
    pSpec->defaultWorkspacePolicy = AGENT_WORKSPACE_ISOLATED;
}

static BOOL SaveSpecIfMissing(const OrgSpec* pSpec)
{
    if (!pSpec || !pSpec->path[0])
        return FALSE;
    if (PathFileExistsW(pSpec->path))
        return TRUE;
    return AgentRuntime_SaveOrgSpec(pSpec);
}

static BOOL JsonWriteStringArray(JsonWriter* pW, char values[][AGENT_RUNTIME_TEXT_SMALL], int count)
{
    int i;
    JsonWriter_BeginArray(pW);
    for (i = 0; i < count; i++)
        JsonWriter_StringValue(pW, values[i]);
    JsonWriter_EndArray(pW);
    return TRUE;
}

BOOL AgentRuntime_SaveOrgSpec(const OrgSpec* pSpec)
{
    JsonWriter w;
    int i;
    BOOL ok;
    if (!pSpec || !pSpec->path[0])
        return FALSE;
    if (!JsonWriter_Init(&w, 4096))
        return FALSE;
    JsonWriter_BeginObject(&w);
    JsonWriter_Int(&w, "version", pSpec->version > 0 ? pSpec->version : 1);
    JsonWriter_String(&w, "name", pSpec->name);
    JsonWriter_WString(&w, "root", pSpec->root);
    JsonWriter_String(&w, "defaultWorkspacePolicy", WorkspacePolicySpecName(pSpec->defaultWorkspacePolicy));
    JsonWriter_String(&w, "layout", pSpec->layout[0] ? pSpec->layout : "line");
    JsonWriter_Key(&w, "nodes");
    JsonWriter_BeginArray(&w);
    for (i = 0; i < pSpec->nodeCount; i++)
    {
        const OrgNodeSpec* node = &pSpec->nodes[i];
        JsonWriter_BeginObject(&w);
        JsonWriter_String(&w, "id", node->id);
        JsonWriter_String(&w, "title", node->title);
        JsonWriter_String(&w, "role", node->role);
        JsonWriter_String(&w, "backend", BackendSpecName(node->backend));
        if (node->model[0])
            JsonWriter_String(&w, "model", node->model);
        JsonWriter_String(&w, "prompt", node->prompt);
        JsonWriter_String(&w, "workspace", WorkspacePolicySpecName(node->workspacePolicy));
        JsonWriter_Key(&w, "dependsOn");
        JsonWriteStringArray(&w, (char (*)[AGENT_RUNTIME_TEXT_SMALL])node->dependsOn, node->dependsOnCount);
        JsonWriter_Key(&w, "tools");
        JsonWriteStringArray(&w, (char (*)[AGENT_RUNTIME_TEXT_SMALL])node->tools, node->toolCount);
        if (node->group[0])
            JsonWriter_String(&w, "group", node->group);
        JsonWriter_EndObject(&w);
    }
    JsonWriter_EndArray(&w);
    JsonWriter_EndObject(&w);
    ok = WriteUtf8File(pSpec->path, JsonWriter_GetBuffer(&w));
    JsonWriter_Free(&w);
    return ok;
}

static BOOL ParseStringArray(JsonReader* pR, char values[][AGENT_RUNTIME_TEXT_SMALL], int maxCount, int* pCount)
{
    EJsonToken token;
    int count = 0;
    if (!pCount)
        return FALSE;
    *pCount = 0;
    while ((token = JsonReader_Next(pR)) != JSON_ARRAY_END && token != JSON_ERROR)
    {
        if (token == JSON_STRING && count < maxCount)
            CopyStringSafe(values[count++], AGENT_RUNTIME_TEXT_SMALL, JsonReader_GetString(pR));
        else if (token == JSON_OBJECT_START || token == JSON_ARRAY_START)
            JsonReader_SkipValue(pR);
    }
    *pCount = count;
    return token != JSON_ERROR;
}

static BOOL ParseNodeObject(JsonReader* pR, OrgNodeSpec* pNode)
{
    EJsonToken token;
    ZeroMemory(pNode, sizeof(*pNode));
    pNode->workspacePolicy = AGENT_WORKSPACE_ISOLATED;
    while ((token = JsonReader_Next(pR)) != JSON_OBJECT_END && token != JSON_ERROR)
    {
        const char* key;
        if (token != JSON_KEY)
            continue;
        key = JsonReader_GetString(pR);
        token = JsonReader_Next(pR);
        if (strcmp(key, "id") == 0 && token == JSON_STRING)
            CopyStringSafe(pNode->id, COUNTOF(pNode->id), JsonReader_GetString(pR));
        else if (strcmp(key, "title") == 0 && token == JSON_STRING)
            CopyStringSafe(pNode->title, COUNTOF(pNode->title), JsonReader_GetString(pR));
        else if (strcmp(key, "role") == 0 && token == JSON_STRING)
            CopyStringSafe(pNode->role, COUNTOF(pNode->role), JsonReader_GetString(pR));
        else if (strcmp(key, "backend") == 0 && token == JSON_STRING)
            pNode->backend = ParseBackend(JsonReader_GetString(pR));
        else if (strcmp(key, "model") == 0 && token == JSON_STRING)
            CopyStringSafe(pNode->model, COUNTOF(pNode->model), JsonReader_GetString(pR));
        else if (strcmp(key, "prompt") == 0 && token == JSON_STRING)
            CopyStringSafe(pNode->prompt, COUNTOF(pNode->prompt), JsonReader_GetString(pR));
        else if (strcmp(key, "workspace") == 0 && token == JSON_STRING)
            pNode->workspacePolicy = ParseWorkspacePolicyText(JsonReader_GetString(pR));
        else if (strcmp(key, "dependsOn") == 0 && token == JSON_ARRAY_START)
            ParseStringArray(pR, (char (*)[AGENT_RUNTIME_TEXT_SMALL])pNode->dependsOn, AGENT_RUNTIME_MAX_DEPENDS, &pNode->dependsOnCount);
        else if (strcmp(key, "tools") == 0 && token == JSON_ARRAY_START)
            ParseStringArray(pR, (char (*)[AGENT_RUNTIME_TEXT_SMALL])pNode->tools, AGENT_RUNTIME_MAX_TOOLS, &pNode->toolCount);
        else if (strcmp(key, "group") == 0 && token == JSON_STRING)
            CopyStringSafe(pNode->group, COUNTOF(pNode->group), JsonReader_GetString(pR));
        else if (token == JSON_OBJECT_START || token == JSON_ARRAY_START)
            JsonReader_SkipValue(pR);
    }
    return pNode->id[0] && pNode->title[0] && token != JSON_ERROR;
}

static BOOL ParseOrgSpecJson(const char* json, int jsonLen, LPCWSTR wszPath, LPCWSTR wszWorkspaceRoot, OrgSpec* pSpec)
{
    JsonReader r;
    EJsonToken token;
    if (!json || !pSpec)
        return FALSE;
    ZeroMemory(pSpec, sizeof(*pSpec));
    pSpec->version = 1;
    pSpec->defaultWorkspacePolicy = AGENT_WORKSPACE_ISOLATED;
    CopyWideSafe(pSpec->path, COUNTOF(pSpec->path), wszPath);
    CopyWideSafe(pSpec->root, COUNTOF(pSpec->root), wszWorkspaceRoot);
    if (!JsonReader_Init(&r, json, jsonLen))
        return FALSE;
    if (JsonReader_Next(&r) != JSON_OBJECT_START)
        return FALSE;
    while ((token = JsonReader_Next(&r)) != JSON_OBJECT_END && token != JSON_ERROR)
    {
        const char* key;
        if (token != JSON_KEY)
            continue;
        key = JsonReader_GetString(&r);
        token = JsonReader_Next(&r);
        if (strcmp(key, "version") == 0 && token == JSON_NUMBER)
            pSpec->version = JsonReader_GetInt(&r);
        else if (strcmp(key, "name") == 0 && token == JSON_STRING)
            CopyStringSafe(pSpec->name, COUNTOF(pSpec->name), JsonReader_GetString(&r));
        else if (strcmp(key, "root") == 0 && token == JSON_STRING)
            Utf8ToWide(JsonReader_GetString(&r), pSpec->root, COUNTOF(pSpec->root));
        else if (strcmp(key, "defaultWorkspacePolicy") == 0 && token == JSON_STRING)
            pSpec->defaultWorkspacePolicy = ParseWorkspacePolicyText(JsonReader_GetString(&r));
        else if (strcmp(key, "layout") == 0 && token == JSON_STRING)
            CopyStringSafe(pSpec->layout, COUNTOF(pSpec->layout), JsonReader_GetString(&r));
        else if (strcmp(key, "nodes") == 0 && token == JSON_ARRAY_START)
        {
            while ((token = JsonReader_Next(&r)) != JSON_ARRAY_END && token != JSON_ERROR)
            {
                if (token == JSON_OBJECT_START && pSpec->nodeCount < AGENT_RUNTIME_MAX_NODES)
                {
                    if (ParseNodeObject(&r, &pSpec->nodes[pSpec->nodeCount]))
                        pSpec->nodeCount++;
                }
                else if (token == JSON_OBJECT_START || token == JSON_ARRAY_START)
                {
                    JsonReader_SkipValue(&r);
                }
            }
        }
        else if (token == JSON_OBJECT_START || token == JSON_ARRAY_START)
        {
            JsonReader_SkipValue(&r);
        }
    }
    if (!pSpec->name[0])
        CopyStringSafe(pSpec->name, COUNTOF(pSpec->name), "Mission Org");
    if (!pSpec->layout[0])
        CopyStringSafe(pSpec->layout, COUNTOF(pSpec->layout), "line");
    return pSpec->nodeCount > 0;
}

static const char* FindJsonValueStart(const char* json, const char* key)
{
    char needle[128];
    const char* p;
    if (!json || !key)
        return NULL;
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    p = strstr(json, needle);
    if (!p)
        return NULL;
    p = strchr(p + (int)strlen(needle), ':');
    if (!p)
        return NULL;
    return SkipWhitespace(p + 1);
}

static const char* ParseJsonStringValue(const char* p, char* dst, int cchDst)
{
    if (!p || *p != '"' || !dst || cchDst <= 0)
        return NULL;
    p++;
    dst[0] = '\0';
    while (*p && *p != '"')
    {
        char ch = *p++;
        if (ch == '\\' && *p)
        {
            switch (*p)
            {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case '\\': ch = '\\'; break;
            case '"': ch = '"'; break;
            default: ch = *p; break;
            }
            p++;
        }
        if (cchDst > 1)
        {
            *dst++ = ch;
            cchDst--;
        }
    }
    *dst = '\0';
    return (*p == '"') ? (p + 1) : NULL;
}

static BOOL ExtractJsonArrayRange(const char* json, const char* key, const char** ppStart, const char** ppEnd)
{
    const char* p = FindJsonValueStart(json, key);
    BOOL inString = FALSE;
    int depth = 0;
    if (ppStart)
        *ppStart = NULL;
    if (ppEnd)
        *ppEnd = NULL;
    if (!p || *p != '[')
        return FALSE;
    if (ppStart)
        *ppStart = p + 1;
    for (; *p; p++)
    {
        if (inString)
        {
            if (*p == '\\' && *(p + 1))
                p++;
            else if (*p == '"')
                inString = FALSE;
            continue;
        }
        if (*p == '"')
        {
            inString = TRUE;
            continue;
        }
        if (*p == '[')
            depth++;
        else if (*p == ']')
        {
            depth--;
            if (depth == 0)
            {
                if (ppEnd)
                    *ppEnd = p;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL JsonExtractStringArrayFallback(const char* json, const char* key,
                                           char values[][AGENT_RUNTIME_TEXT_SMALL], int maxCount, int* pCount)
{
    const char* p;
    const char* end;
    int count = 0;
    if (pCount)
        *pCount = 0;
    if (!ExtractJsonArrayRange(json, key, &p, &end))
        return FALSE;
    while (p && p < end)
    {
        char value[AGENT_RUNTIME_TEXT_SMALL];
        p = SkipWhitespace(p);
        if (!p || p >= end)
            break;
        if (*p == ',')
        {
            p++;
            continue;
        }
        if (*p == '"')
        {
            p = ParseJsonStringValue(p, value, ARRAYSIZE(value));
            if (!p)
                return FALSE;
            if (count < maxCount)
                CopyStringSafe(values[count++], AGENT_RUNTIME_TEXT_SMALL, value);
            continue;
        }
        p++;
    }
    if (pCount)
        *pCount = count;
    return TRUE;
}

static BOOL ExtractNextJsonObjectCopy(const char** ppCursor, const char* end, char** ppszObject)
{
    const char* p;
    const char* start;
    BOOL inString = FALSE;
    int depth = 0;
    int len;
    char* out;
    if (ppszObject)
        *ppszObject = NULL;
    if (!ppCursor || !*ppCursor || !end)
        return FALSE;
    p = *ppCursor;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ','))
        p++;
    if (p >= end || *p != '{')
    {
        *ppCursor = end;
        return FALSE;
    }
    start = p;
    for (; p < end; p++)
    {
        if (inString)
        {
            if (*p == '\\' && (p + 1) < end)
                p++;
            else if (*p == '"')
                inString = FALSE;
            continue;
        }
        if (*p == '"')
        {
            inString = TRUE;
            continue;
        }
        if (*p == '{')
            depth++;
        else if (*p == '}')
        {
            depth--;
            if (depth == 0)
            {
                len = (int)(p - start + 1);
                out = (char*)malloc((size_t)len + 1);
                if (!out)
                {
                    *ppCursor = p + 1;
                    return FALSE;
                }
                memcpy(out, start, (size_t)len);
                out[len] = '\0';
                if (ppszObject)
                    *ppszObject = out;
                *ppCursor = p + 1;
                return TRUE;
            }
        }
    }
    *ppCursor = end;
    return FALSE;
}

static BOOL ParseNodeObjectFallback(const char* json, OrgNodeSpec* pNode)
{
    char* value;
    if (!json || !pNode)
        return FALSE;
    ZeroMemory(pNode, sizeof(*pNode));
    pNode->workspacePolicy = AGENT_WORKSPACE_ISOLATED;

    value = JsonExtractString(json, "id");
    if (value)
    {
        CopyStringSafe(pNode->id, COUNTOF(pNode->id), value);
        free(value);
    }
    value = JsonExtractString(json, "title");
    if (value)
    {
        CopyStringSafe(pNode->title, COUNTOF(pNode->title), value);
        free(value);
    }
    value = JsonExtractString(json, "role");
    if (value)
    {
        CopyStringSafe(pNode->role, COUNTOF(pNode->role), value);
        free(value);
    }
    value = JsonExtractString(json, "backend");
    if (value)
    {
        pNode->backend = ParseBackend(value);
        free(value);
    }
    value = JsonExtractString(json, "model");
    if (value)
    {
        CopyStringSafe(pNode->model, COUNTOF(pNode->model), value);
        free(value);
    }
    value = JsonExtractString(json, "prompt");
    if (value)
    {
        CopyStringSafe(pNode->prompt, COUNTOF(pNode->prompt), value);
        free(value);
    }
    value = JsonExtractString(json, "workspace");
    if (value)
    {
        pNode->workspacePolicy = ParseWorkspacePolicyText(value);
        free(value);
    }
    value = JsonExtractString(json, "group");
    if (value)
    {
        CopyStringSafe(pNode->group, COUNTOF(pNode->group), value);
        free(value);
    }
    JsonExtractStringArrayFallback(json, "dependsOn",
        (char (*)[AGENT_RUNTIME_TEXT_SMALL])pNode->dependsOn, AGENT_RUNTIME_MAX_DEPENDS, &pNode->dependsOnCount);
    JsonExtractStringArrayFallback(json, "tools",
        (char (*)[AGENT_RUNTIME_TEXT_SMALL])pNode->tools, AGENT_RUNTIME_MAX_TOOLS, &pNode->toolCount);
    return pNode->id[0] && pNode->title[0];
}

static BOOL ParseOrgSpecJsonFallback(const char* json, LPCWSTR wszPath, LPCWSTR wszWorkspaceRoot, OrgSpec* pSpec)
{
    char* value;
    const char* cursor;
    const char* end;
    char* nodeJson;
    if (!json || !pSpec)
        return FALSE;
    ZeroMemory(pSpec, sizeof(*pSpec));
    pSpec->version = 1;
    pSpec->defaultWorkspacePolicy = AGENT_WORKSPACE_ISOLATED;
    CopyWideSafe(pSpec->path, COUNTOF(pSpec->path), wszPath);
    CopyWideSafe(pSpec->root, COUNTOF(pSpec->root), wszWorkspaceRoot);

    value = JsonExtractString(json, "name");
    if (value)
    {
        CopyStringSafe(pSpec->name, COUNTOF(pSpec->name), value);
        free(value);
    }
    value = JsonExtractString(json, "layout");
    if (value)
    {
        CopyStringSafe(pSpec->layout, COUNTOF(pSpec->layout), value);
        free(value);
    }
    value = JsonExtractString(json, "root");
    if (value)
    {
        Utf8ToWide(value, pSpec->root, COUNTOF(pSpec->root));
        free(value);
    }
    value = JsonExtractString(json, "defaultWorkspacePolicy");
    if (value)
    {
        pSpec->defaultWorkspacePolicy = ParseWorkspacePolicyText(value);
        free(value);
    }

    if (ExtractJsonArrayRange(json, "nodes", &cursor, &end))
    {
        while (pSpec->nodeCount < AGENT_RUNTIME_MAX_NODES &&
               ExtractNextJsonObjectCopy(&cursor, end, &nodeJson))
        {
            if (nodeJson)
            {
                if (ParseNodeObjectFallback(nodeJson, &pSpec->nodes[pSpec->nodeCount]))
                    pSpec->nodeCount++;
                free(nodeJson);
            }
        }
    }

    if (!pSpec->name[0])
        CopyStringSafe(pSpec->name, COUNTOF(pSpec->name), "Mission Org");
    if (!pSpec->layout[0])
        CopyStringSafe(pSpec->layout, COUNTOF(pSpec->layout), "line");
    return pSpec->nodeCount > 0;
}

BOOL AgentRuntime_EnsureWorkspaceAssets(const WCHAR* wszWorkspaceRoot)
{
    WCHAR wszBikode[MAX_PATH];
    WCHAR wszOrgs[MAX_PATH];
    WCHAR wszRuns[MAX_PATH];
    WCHAR wszWorktrees[MAX_PATH];
    WCHAR wszSandboxes[MAX_PATH];
    OrgSpec spec;
    WCHAR wszPath[MAX_PATH];
    BOOL ok = TRUE;

    if (!wszWorkspaceRoot || !wszWorkspaceRoot[0])
        return FALSE;

    PathCombineW(wszBikode, wszWorkspaceRoot, L".bikode");
    PathCombineW(wszOrgs, wszBikode, L"orgs");
    PathCombineW(wszRuns, wszBikode, L"runs");
    PathCombineW(wszWorktrees, wszBikode, L"worktrees");
    PathCombineW(wszSandboxes, wszBikode, L"sandboxes");

    if (!EnsureDirExists(wszBikode) || !EnsureDirExists(wszOrgs) || !EnsureDirExists(wszRuns) ||
        !EnsureDirExists(wszWorktrees) || !EnsureDirExists(wszSandboxes))
        return FALSE;

    PathCombineW(wszPath, wszOrgs, L"delivery-line.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Delivery Line", "line");
    AppendOrgNode(&spec, "planner", "Planner", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_ISOLATED,
                  "Plan the work in this repository, identify milestones, and hand a concrete implementation brief to the implementer.", "delivery");
    AppendOrgNode(&spec, "implementer", "Implementer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
                  "Implement the plan in the isolated workspace, make code changes directly, and leave a concise build and risk note.", "delivery");
    AddDependency(&spec.nodes[1], "planner");
    AppendOrgNode(&spec, "reviewer", "Reviewer", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_ISOLATED,
                  "Review the implementer output for correctness, readability, and regressions. Tighten the work if needed.", "delivery");
    AddDependency(&spec.nodes[2], "implementer");
    AppendOrgNode(&spec, "tester", "Tester", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
                  "Summarize likely checks to run, note validation gaps, and call out residual risk from the changed files.", "delivery");
    AddDependency(&spec.nodes[3], "reviewer");
    ok = SaveSpecIfMissing(&spec) && ok;

    PathCombineW(wszPath, wszOrgs, L"bug-hunt.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Bug Hunt", "tree");
    AppendOrgNode(&spec, "repro", "Reproduce", "debug", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
                  "Analyze the likely bug surface, restate the suspected failure, and identify the smallest high-confidence fix zone.", "bugs");
    AppendOrgNode(&spec, "fix", "Fix", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
                  "Patch the issue in the isolated workspace and keep the fix as focused as possible.", "bugs");
    AddDependency(&spec.nodes[1], "repro");
    AppendOrgNode(&spec, "review", "Review", "reviewer", AGENT_BACKEND_CODEX, AGENT_WORKSPACE_ISOLATED,
                  "Review the patch, tighten edge cases, and summarize what still needs validation.", "bugs");
    AddDependency(&spec.nodes[2], "fix");
    AppendOrgNode(&spec, "validate", "Validate", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
                  "Generate a concise validation checklist for the fix, including regression concerns.", "bugs");
    AddDependency(&spec.nodes[3], "review");
    ok = SaveSpecIfMissing(&spec) && ok;

    PathCombineW(wszPath, wszOrgs, L"research-swarm.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Research Swarm", "mesh");
    AppendOrgNode(&spec, "research-a", "Research A", "research", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
                  "Inspect the codebase and extract architecture insights related to the user goal.", "research");
    AppendOrgNode(&spec, "research-b", "Research B", "research", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
                  "Inspect likely integration points, interfaces, and hidden risks related to the same goal.", "research");
    AppendOrgNode(&spec, "synth", "Synthesizer", "planner", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
                  "Merge the research threads into one practical execution brief with sharp tradeoffs.", "research");
    AddDependency(&spec.nodes[2], "research-a");
    AddDependency(&spec.nodes[2], "research-b");
    ok = SaveSpecIfMissing(&spec) && ok;

    PathCombineW(wszPath, wszOrgs, L"refactor-design.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Refactor Design", "hub");
    AppendOrgNode(&spec, "design", "Design", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
                  "Design a refactor plan that improves maintainability without unnecessary churn.", "refactor");
    AppendOrgNode(&spec, "refactor", "Refactor", "refactor", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
                  "Carry out the approved refactor plan and keep behavior stable.", "refactor");
    AddDependency(&spec.nodes[1], "design");
    AppendOrgNode(&spec, "proof", "Proof", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_ISOLATED,
                  "Review the refactor for clarity, regressions, and incomplete migrations.", "refactor");
    AddDependency(&spec.nodes[2], "refactor");
    ok = SaveSpecIfMissing(&spec) && ok;

    PathCombineW(wszPath, wszOrgs, L"paired-review.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Codex Claude Pair", "line");
    AppendOrgNode(&spec, "codex-pass", "Codex Pass", "implementer", AGENT_BACKEND_CODEX, AGENT_WORKSPACE_ISOLATED,
                  "Make the first implementation pass and leave a clear review handoff.", "pair");
    AppendOrgNode(&spec, "claude-pass", "Claude Pass", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_ISOLATED,
                  "Review and refine the Codex pass, preserving good changes and tightening correctness.", "pair");
    AddDependency(&spec.nodes[1], "codex-pass");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- The Agency: Startup MVP Pipeline ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-startup-mvp.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Startup MVP", "tree");
    AppendOrgNode(&spec, "architect", "Software Architect", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Software Architect. Design the system architecture for this MVP. Identify bounded contexts, "
        "select appropriate patterns (modular monolith vs microservices), and produce an ADR covering key trade-offs. "
        "Domain first, technology second. Prefer reversible decisions. Hand off a concrete implementation brief.", "engineering");
    AppendOrgNode(&spec, "backend", "Backend Architect", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Backend Architect. Implement the API layer and data model from the architecture brief. "
        "Design schemas optimized for performance and consistency. Include proper error handling, input validation, "
        "and security measures. Leave a clear handoff describing endpoints, models, and integration points.", "engineering");
    AddDependency(&spec.nodes[1], "architect");
    AppendOrgNode(&spec, "frontend", "Frontend Developer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Frontend Developer. Build the UI layer integrating with the backend API. "
        "Focus on core user flows, responsive design, and accessibility. Use component architecture for reusability. "
        "Optimize for Core Web Vitals. Include mobile-first responsive patterns.", "engineering");
    AddDependency(&spec.nodes[2], "architect");
    AppendOrgNode(&spec, "reviewer", "Code Reviewer", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Code Reviewer. Review all implementation work for correctness, security, and maintainability. "
        "Prioritize findings as blockers, suggestions, and nits. Check for injection, XSS, auth issues, and race conditions. "
        "Provide specific, actionable feedback with line references. Praise good patterns.", "engineering");
    AddDependency(&spec.nodes[3], "backend");
    AddDependency(&spec.nodes[3], "frontend");
    AppendOrgNode(&spec, "checker", "Reality Checker", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Reality Checker. Verify production readiness across reliability, security, performance, and UX. "
        "Require evidence for every claim. Check monitoring, alerting, and rollback procedures. "
        "Provide a clear pass/fail verdict with specific blockers if any.", "testing");
    AddDependency(&spec.nodes[4], "reviewer");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- The Agency: Security Hardening Pipeline ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-security-hardening.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Security Hardening", "line");
    AppendOrgNode(&spec, "threat-model", "Security Engineer", "reviewer", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Security Engineer. Perform STRIDE threat modeling on this codebase. Identify attack surfaces, "
        "trust boundaries, and data classification. Review for OWASP Top 10 and CWE Top 25 vulnerabilities. "
        "Assume all user input is malicious. Classify findings by risk level. "
        "Hand off a prioritized list of vulnerabilities with remediation guidance.", "engineering");
    AppendOrgNode(&spec, "fix", "Security Implementer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "Apply the security remediations from the threat model. Fix critical and high-severity findings first. "
        "Use parameterized queries, proper input validation, secure authentication patterns, and secrets management. "
        "Never disable security controls as a fix. Prefer well-tested libraries over custom crypto.", "engineering");
    AddDependency(&spec.nodes[1], "threat-model");
    AppendOrgNode(&spec, "audit", "Code Reviewer", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Code Reviewer. Audit the security fixes for correctness and completeness. "
        "Verify that each vulnerability from the threat model has been properly addressed. "
        "Check for regressions or newly introduced issues. Confirm no hardcoded secrets remain.", "engineering");
    AddDependency(&spec.nodes[2], "fix");
    AppendOrgNode(&spec, "sre-check", "SRE", "reviewer", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are SRE. Verify that security changes maintain system reliability. Check that SLOs are preserved, "
        "monitoring covers new security controls, and graceful degradation paths exist. "
        "Validate that deployment can be rolled back safely if issues arise.", "engineering");
    AddDependency(&spec.nodes[3], "audit");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- The Agency: Full-Stack Feature ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-fullstack-feature.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Full-Stack Feature", "tree");
    AppendOrgNode(&spec, "prioritizer", "Sprint Prioritizer", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Sprint Prioritizer. Analyze the feature request using RICE or value-vs-effort frameworks. "
        "Identify dependencies, estimate scope, and define clear acceptance criteria. "
        "Break down into implementation tasks with priorities. Hand off a structured brief.", "product");
    AppendOrgNode(&spec, "ux", "UX Architect", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are UX Architect. Design the technical UI foundation for this feature. "
        "Define component architecture, layout patterns, and interaction flows. "
        "Include responsive breakpoints and accessibility considerations. Provide a developer-ready spec.", "design");
    AddDependency(&spec.nodes[1], "prioritizer");
    AppendOrgNode(&spec, "impl", "Frontend Developer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Frontend Developer. Implement the feature following the UX spec and sprint brief. "
        "Build responsive, accessible components with proper state management. "
        "Optimize for performance. Leave clear integration notes.", "engineering");
    AddDependency(&spec.nodes[2], "ux");
    AppendOrgNode(&spec, "db", "Database Optimizer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Database Optimizer. Design or update the data layer for this feature. "
        "Create efficient schemas with proper indexes. Ensure migrations are reversible. "
        "Optimize queries for performance. Validate foreign key constraints.", "engineering");
    AddDependency(&spec.nodes[3], "prioritizer");
    AppendOrgNode(&spec, "review", "Code Reviewer", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Code Reviewer. Review all implementation work across frontend and data layers. "
        "Focus on correctness, security, maintainability, and performance. "
        "Ensure the feature meets the acceptance criteria from the sprint brief.", "engineering");
    AddDependency(&spec.nodes[4], "impl");
    AddDependency(&spec.nodes[4], "db");
    AppendOrgNode(&spec, "a11y", "Accessibility Auditor", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Accessibility Auditor. Audit the feature against WCAG 2.2 AA. "
        "Check keyboard navigation, screen reader compatibility, focus management, and color contrast. "
        "Catch what automated tools miss. Provide specific findings with remediation steps.", "testing");
    AddDependency(&spec.nodes[5], "review");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- The Agency: Performance Optimization ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-perf-optimization.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Perf Optimization", "mesh");
    AppendOrgNode(&spec, "bench", "Performance Benchmarker", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Performance Benchmarker. Profile this codebase and establish performance baselines. "
        "Identify bottlenecks through systematic analysis. Measure Core Web Vitals, API response times, "
        "and resource utilization. Produce a ranked list of optimization opportunities with expected impact.", "testing");
    AppendOrgNode(&spec, "db-opt", "Database Optimizer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Database Optimizer. Optimize the data layer based on performance findings. "
        "Add missing indexes, fix N+1 queries, optimize slow queries using EXPLAIN ANALYZE. "
        "Implement connection pooling improvements. Ensure all changes are backwards-compatible.", "engineering");
    AddDependency(&spec.nodes[1], "bench");
    AppendOrgNode(&spec, "frontend-opt", "Frontend Developer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Frontend Developer. Optimize the frontend based on performance findings. "
        "Implement code splitting, lazy loading, and bundle size reduction. "
        "Optimize images and assets. Improve Core Web Vitals scores.", "engineering");
    AddDependency(&spec.nodes[2], "bench");
    AppendOrgNode(&spec, "verify", "Performance Benchmarker", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Performance Benchmarker. Re-run benchmarks after optimizations. "
        "Compare against baselines and quantify improvements. Verify SLAs are met with 95% confidence. "
        "Report remaining opportunities and diminishing-returns threshold.", "testing");
    AddDependency(&spec.nodes[3], "db-opt");
    AddDependency(&spec.nodes[3], "frontend-opt");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- The Agency: Incident Response ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-incident-response.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Incident Response", "line");
    AppendOrgNode(&spec, "triage", "Incident Commander", "debug", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Incident Response Commander. Triage this incident by user impact and severity. "
        "Identify the blast radius, establish a timeline of events, and coordinate the response. "
        "Determine root cause candidates and assign investigation paths.", "engineering");
    AppendOrgNode(&spec, "fix", "Rapid Prototyper", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Rapid Prototyper in incident mode. Implement the fastest safe fix for the identified root cause. "
        "Focus on stopping the bleeding, not perfection. Keep the patch minimal and reversible. "
        "Document what was changed and why.", "engineering");
    AddDependency(&spec.nodes[1], "triage");
    AppendOrgNode(&spec, "verify", "SRE", "reviewer", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are SRE. Verify the incident fix is safe to deploy. Check that SLOs are restored, "
        "monitoring confirms resolution, and no new issues were introduced. "
        "Validate rollback procedures are ready. Give a deploy/no-deploy verdict.", "engineering");
    AddDependency(&spec.nodes[2], "fix");
    AppendOrgNode(&spec, "postmortem", "Technical Writer", "research", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Technical Writer producing the blameless post-mortem. "
        "Document the incident timeline, root cause, impact, resolution, and follow-up action items. "
        "Focus on systemic improvements, not individual blame. Make action items specific and assigned.", "engineering");
    AddDependency(&spec.nodes[3], "verify");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- Promptfoo: LLM Eval Pipeline ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-llm-eval.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: LLM Eval Pipeline", "line");
    AppendOrgNode(&spec, "evaluator", "Prompt Evaluator", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Prompt Evaluator. Design test cases for the prompts in this workspace. "
        "Create evaluation matrices: expected vs actual, correctness, relevance, safety. "
        "Use assertion-based grading. Track metrics across prompt iterations. Report pass/fail with confidence.", "eval");
    AppendOrgNode(&spec, "redteam", "Red Teamer", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Red Teamer. Probe the prompts for vulnerabilities: injection, jailbreaks, data leakage, bias, harmful outputs. "
        "Generate adversarial test cases systematically. Classify findings by severity. "
        "Provide remediation strategies for each vulnerability. Focus on making AI outputs safer.", "eval");
    AddDependency(&spec.nodes[1], "evaluator");
    AppendOrgNode(&spec, "comparator", "Model Comparator", "research", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Model Comparator. Compare outputs across model configurations. "
        "Evaluate cost, latency, quality, and safety trade-offs. Produce a comparison matrix with clear recommendations. "
        "Recommend the optimal model for each use case identified.", "eval");
    AddDependency(&spec.nodes[2], "redteam");
    AppendOrgNode(&spec, "guard", "Regression Guard", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Regression Guard. Define the golden test set from evaluation and red-team results. "
        "Set minimum pass rates for safety, correctness, and relevance. "
        "Produce a final go/no-go verdict for the prompt configuration.", "eval");
    AddDependency(&spec.nodes[3], "comparator");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- Impeccable: Design Quality Pipeline ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-design-quality.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Design Quality", "tree");
    AppendOrgNode(&spec, "audit", "Design Auditor", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Design Auditor. Run technical quality checks on the frontend: accessibility (WCAG AA), "
        "Core Web Vitals, responsive behavior. Flag anti-patterns: nested cards, gray-on-color text, "
        "pure black/white, overused fonts, identical grids, glassmorphism overuse. Report issues with severity.", "design");
    AppendOrgNode(&spec, "critique", "Design Critic", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Design Critic. Review for hierarchy clarity, emotional resonance, and intentional aesthetics. "
        "Does the design have a bold direction or fall into generic AI slop? "
        "Push for distinctive design over safe defaults. Identify what makes it memorable or forgettable.", "design");
    AddDependency(&spec.nodes[1], "audit");
    AppendOrgNode(&spec, "typo", "Typography Expert", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Typography Expert. Fix type system issues from the audit. "
        "Implement modular type scales with fluid sizing. Replace generic fonts with distinctive choices. "
        "Establish vertical rhythm and proper measure (65ch). Optimize font loading.", "design");
    AddDependency(&spec.nodes[2], "critique");
    AppendOrgNode(&spec, "color", "Color Specialist", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Color Specialist. Fix color system issues from the audit. "
        "Switch to OKLCH. Tint neutrals toward brand hue. Apply 60-30-10 rule. "
        "Ensure WCAG AA contrast. Build functional palette with proper semantic colors.", "design");
    AddDependency(&spec.nodes[3], "critique");
    AppendOrgNode(&spec, "polish", "Design Polisher", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Design Polisher. Final production pass: micro-interactions, hover states, focus indicators, "
        "loading states. Normalize to design system. Strip unnecessary complexity. "
        "Every decorative element must serve a purpose. Make every pixel intentional.", "design");
    AddDependency(&spec.nodes[4], "typo");
    AddDependency(&spec.nodes[4], "color");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- OpenViking: Context Architecture Pipeline ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-context-arch.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Context Architecture", "line");
    AppendOrgNode(&spec, "architect", "Context Architect", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Context Architect. Design the context structure for agents in this workspace. "
        "Organize using filesystem paradigm: unify memories, resources, skills. "
        "Define L0/L1/L2 tiered loading strategy. Create context schemas for automatic session management.", "context");
    AppendOrgNode(&spec, "curator", "Memory Curator", "research", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Memory Curator. Extract and organize long-term memories from existing sessions and codebase. "
        "Separate task memory from user memory. Prune outdated or conflicting entries. "
        "Structure memories hierarchically for efficient retrieval.", "context");
    AddDependency(&spec.nodes[1], "architect");
    AppendOrgNode(&spec, "retrieval", "Retrieval Optimizer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Retrieval Optimizer. Implement the context retrieval strategy from the architecture plan. "
        "Combine directory positioning with semantic search. Optimize for token budget. "
        "Make retrieval paths observable and debuggable. Verify relevance and completeness.", "context");
    AddDependency(&spec.nodes[2], "curator");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- Composite: Ship-Ready Feature (all tools) ---- */
    PathCombineW(wszPath, wszOrgs, L"agency-ship-ready.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Agency: Ship-Ready Feature", "tree");
    AppendOrgNode(&spec, "plan", "Software Architect", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Software Architect. Design the architecture for this feature. "
        "Identify bounded contexts and trade-offs. Produce an ADR and implementation brief. "
        "Domain first, technology second. Prefer reversible decisions.", "engineering");
    AppendOrgNode(&spec, "impl", "Frontend Developer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Frontend Developer. Implement the feature from the architecture brief. "
        "Build responsive, accessible components. Use distinctive design choices, not generic templates. "
        "Follow OKLCH color, modular type scales, and spatial rhythm principles.", "engineering");
    AddDependency(&spec.nodes[1], "plan");
    AppendOrgNode(&spec, "review", "Code Reviewer", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Code Reviewer. Review for correctness, security, and maintainability. "
        "Prioritize as blockers, suggestions, nits. Check for OWASP Top 10 issues.", "engineering");
    AddDependency(&spec.nodes[2], "impl");
    AppendOrgNode(&spec, "design-qa", "Design Auditor", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Design Auditor. Audit the implementation for frontend quality: "
        "accessibility, responsive behavior, anti-patterns, type consistency, color contrast, spatial rhythm.", "design");
    AddDependency(&spec.nodes[3], "review");
    AppendOrgNode(&spec, "eval", "Prompt Evaluator", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Prompt Evaluator. If this feature includes AI-facing prompts or LLM integrations, "
        "evaluate prompt quality for correctness, safety, and consistency. "
        "If no prompts exist, verify that any AI-generated content meets quality standards.", "eval");
    AddDependency(&spec.nodes[4], "review");
    AppendOrgNode(&spec, "ship", "Reality Checker", "tester", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Reality Checker. Final ship/no-ship verdict. Verify all prior findings are resolved. "
        "Check monitoring, rollback, and deployment readiness. Require evidence for every claim. "
        "Produce a clear verdict with any remaining blockers.", "testing");
    AddDependency(&spec.nodes[5], "design-qa");
    AddDependency(&spec.nodes[5], "eval");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- SWE-Bench: Automated Issue Resolution ---- */
    PathCombineW(wszPath, wszOrgs, L"swebench-issue-fix.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "SWE-Bench: Issue Fix", "line");
    AppendOrgNode(&spec, "triager", "Issue Triager", "debug", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Issue Triager, inspired by SWE-Agent (Princeton/Stanford). "
        "Parse the issue, identify the failing behavior, locate the relevant source files, "
        "and produce a minimal reproduction plan. Classify severity and narrow the search space "
        "to the smallest set of files that could contain the bug.", "swebench");
    AppendOrgNode(&spec, "reproducer", "Bug Reproducer", "debug", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Bug Reproducer. Read the identified source files, trace the execution path, "
        "and confirm the root cause. Write a minimal test case or command that demonstrates the failure. "
        "Document the exact conditions under which the bug manifests.", "swebench");
    AddDependency(&spec.nodes[1], "triager");
    AppendOrgNode(&spec, "patcher", "Patch Writer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Patch Writer. Write the minimal, focused patch that fixes the confirmed bug. "
        "Keep changes as small as possible. Follow the existing code style exactly. "
        "Add or update tests to cover the fixed behavior. Document what was changed and why.", "swebench");
    AddDependency(&spec.nodes[2], "reproducer");
    AppendOrgNode(&spec, "regression", "Regression Tester", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Regression Tester. Verify the patch fixes the original issue without breaking existing tests. "
        "Check edge cases around the fix boundary. Confirm the reproduction case now passes. "
        "Provide a clear pass/fail verdict with evidence.", "swebench");
    AddDependency(&spec.nodes[3], "patcher");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- Semgrep: Security Scanning Pipeline ---- */
    PathCombineW(wszPath, wszOrgs, L"semgrep-security-scan.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Semgrep: Security Scan", "line");
    AppendOrgNode(&spec, "scanner", "SAST Scanner", "reviewer", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are SAST Scanner, inspired by Semgrep. Scan the codebase for security vulnerabilities "
        "using pattern-based analysis. Check for OWASP Top 10 and CWE Top 25. "
        "Report findings with severity, CWE ID, file location, and remediation guidance. "
        "Minimize false positives by understanding data flow context.", "security");
    AppendOrgNode(&spec, "deps", "Dependency Auditor", "reviewer", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Dependency Auditor, inspired by Semgrep Supply Chain. "
        "Audit project dependencies for known CVEs, license issues, and supply chain risks. "
        "Check package manifests for outdated or vulnerable packages. "
        "Recommend version upgrades with breaking change analysis.", "security");
    AppendOrgNode(&spec, "fixer", "Vulnerability Fixer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Vulnerability Fixer. Apply security patches for critical and high severity findings. "
        "Use parameterized queries, sanitize user input, replace custom crypto with tested libraries. "
        "Remove hardcoded secrets. Each fix should be minimal and well-documented.", "security");
    AddDependency(&spec.nodes[2], "scanner");
    AddDependency(&spec.nodes[2], "deps");
    AppendOrgNode(&spec, "compliance", "Compliance Checker", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Compliance Checker. Verify fixes are complete and code meets compliance standards. "
        "Check data handling, encryption, access controls, and audit logging. "
        "Produce a compliance report with pass/fail status and remaining remediation steps.", "security");
    AddDependency(&spec.nodes[3], "fixer");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- MetaGPT: Software Project Team ---- */
    PathCombineW(wszPath, wszOrgs, L"metagpt-project-team.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "MetaGPT: Project Team", "tree");
    AppendOrgNode(&spec, "pm", "Product Manager", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Product Manager, inspired by MetaGPT. "
        "Translate user requirements into a detailed PRD with user stories, acceptance criteria, "
        "competitive analysis, and MVP scope. Hand off a structured brief with success metrics.", "metagpt");
    AppendOrgNode(&spec, "techlead", "Tech Lead", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Tech Lead, inspired by MetaGPT. Translate the PRD into a technical design document "
        "with architecture, data models, API specs, and technology stack. "
        "Define module boundaries, create a task breakdown with dependencies and effort estimates.", "metagpt");
    AddDependency(&spec.nodes[1], "pm");
    AppendOrgNode(&spec, "dev", "Senior Developer", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Senior Developer, inspired by MetaGPT. Implement the technical design with "
        "clean, production-quality code. Proper error handling, logging, and input validation. "
        "Write unit tests alongside implementation. Follow SOLID principles.", "metagpt");
    AddDependency(&spec.nodes[2], "techlead");
    AppendOrgNode(&spec, "qa", "QA Engineer", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are QA Engineer, inspired by MetaGPT. Design comprehensive test plans from the PRD and design. "
        "Write integration and edge case tests. Verify acceptance criteria. "
        "Report bugs with reproduction steps, expected vs actual, and severity.", "metagpt");
    AddDependency(&spec.nodes[3], "dev");
    AppendOrgNode(&spec, "scrum", "Scrum Master", "reviewer", AGENT_BACKEND_CLAUDE, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Scrum Master, inspired by MetaGPT. Review the full pipeline for process quality. "
        "Check requirements traceability from PRD through design to implementation to tests. "
        "Produce a sprint retrospective and provide a ship/no-ship recommendation.", "metagpt");
    AddDependency(&spec.nodes[4], "qa");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- Aider: AI Pair Programming ---- */
    PathCombineW(wszPath, wszOrgs, L"aider-pair-programming.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "Aider: Pair Programming", "line");
    AppendOrgNode(&spec, "navigator", "Repo Navigator", "research", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Repo Navigator, inspired by Aider's repo-map capability. "
        "Map the repository structure, identify key modules and dependencies. "
        "Determine which files are relevant to the current task. "
        "Hand off a focused context brief with exact files and functions that need attention.", "aider");
    AppendOrgNode(&spec, "editor", "Code Editor", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Code Editor, inspired by Aider's edit-focused approach. "
        "Make targeted, surgical code changes based on the navigator's brief. "
        "Follow existing conventions exactly. Make the smallest change that achieves the goal. "
        "Explain each change clearly. No leftover debug code.", "aider");
    AddDependency(&spec.nodes[1], "navigator");
    AppendOrgNode(&spec, "committer", "Git Committer", "reviewer", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Git Committer, inspired by Aider's auto-commit workflow. "
        "Review changes for correctness and completeness. Verify intent matches the task. "
        "Suggest clear conventional commit messages. Flag changes that need splitting.", "aider");
    AddDependency(&spec.nodes[2], "editor");
    AppendOrgNode(&spec, "tester", "Test Runner", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Test Runner. Run existing tests to verify the changes. "
        "Analyze any failures and identify which changes caused regressions. "
        "Suggest new test cases. Provide a clear test report with pass/fail counts.", "aider");
    AddDependency(&spec.nodes[3], "committer");
    ok = SaveSpecIfMissing(&spec) && ok;

    /* ---- CrewAI: Collaborative Task Completion ---- */
    PathCombineW(wszPath, wszOrgs, L"crewai-collaborative.json");
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszPath, "CrewAI: Collaborative Task", "tree");
    AppendOrgNode(&spec, "coordinator", "Task Coordinator", "planner", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Task Coordinator, inspired by CrewAI's role-based orchestration. "
        "Break the complex task into sub-tasks with clear ownership, dependencies, and success criteria. "
        "Create a structured execution plan with parallel tracks where possible. "
        "Ensure each handoff includes complete context.", "crewai");
    AppendOrgNode(&spec, "expert", "Domain Expert", "research", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Domain Expert, inspired by CrewAI's specialist pattern. "
        "Research the codebase to understand patterns, conventions, and architectural decisions. "
        "Identify domain-specific constraints, edge cases, and best practices. "
        "Document key insights that should inform all implementation decisions.", "crewai");
    AddDependency(&spec.nodes[1], "coordinator");
    AppendOrgNode(&spec, "coder-a", "Code Specialist A", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Code Specialist A, inspired by CrewAI's collaborative execution. "
        "Implement your assigned sub-task with domain expert guidance. "
        "Write production-quality code that integrates cleanly. Handle errors gracefully. "
        "Leave integration notes for other agents.", "crewai");
    AddDependency(&spec.nodes[2], "expert");
    AppendOrgNode(&spec, "coder-b", "Code Specialist B", "implementer", AGENT_BACKEND_RELAY, AGENT_WORKSPACE_ISOLATED,
        "You are Code Specialist B, inspired by CrewAI's collaborative execution. "
        "Implement your assigned sub-task independently from Specialist A. "
        "Write production-quality code following established patterns. "
        "Ensure your changes don't conflict with parallel work.", "crewai");
    AddDependency(&spec.nodes[3], "expert");
    AppendOrgNode(&spec, "integrator", "Integration Tester", "tester", AGENT_BACKEND_API, AGENT_WORKSPACE_SHARED_READONLY,
        "You are Integration Tester. Verify all sub-tasks integrate correctly when combined. "
        "Test interfaces between components. Check data consistency and API contract compliance. "
        "Provide a final integration verdict with pass/fail status.", "crewai");
    AddDependency(&spec.nodes[4], "coder-a");
    AddDependency(&spec.nodes[4], "coder-b");
    ok = SaveSpecIfMissing(&spec) && ok;

    return ok;
}

BOOL AgentRuntime_LoadOrgSpecs(const WCHAR* wszWorkspaceRoot, OrgSpec* pSpecs, int cMaxSpecs, int* pnSpecs)
{
    WCHAR wszOrgs[MAX_PATH];
    WCHAR wszPattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;
    int count = 0;
    if (pnSpecs)
        *pnSpecs = 0;
    if (!wszWorkspaceRoot || !pSpecs || cMaxSpecs <= 0)
        return FALSE;
    if (!AgentRuntime_EnsureWorkspaceAssets(wszWorkspaceRoot))
        return FALSE;
    PathCombineW(wszOrgs, wszWorkspaceRoot, L".bikode\\orgs");
    PathCombineW(wszPattern, wszOrgs, L"*.json");
    hFind = FindFirstFileW(wszPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return FALSE;
    do
    {
        WCHAR wszPath[MAX_PATH];
        char* json;
        DWORD cb;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        PathCombineW(wszPath, wszOrgs, fd.cFileName);
        json = ReadUtf8File(wszPath, &cb);
        if (!json)
            continue;
        if (ParseOrgSpecJson(json, (int)cb, wszPath, wszWorkspaceRoot, &pSpecs[count]) ||
            ParseOrgSpecJsonFallback(json, wszPath, wszWorkspaceRoot, &pSpecs[count]))
        {
            count++;
            if (count >= cMaxSpecs)
            {
                free(json);
                break;
            }
        }
        free(json);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    if (pnSpecs)
        *pnSpecs = count;
    return count > 0;
}

BOOL AgentRuntime_CreateDraftOrgSpec(const WCHAR* wszWorkspaceRoot, WCHAR* wszOutPath, int cchOutPath)
{
    OrgSpec spec;
    WCHAR wszDir[MAX_PATH];
    WCHAR wszSlug[128];
    WCHAR wszCandidate[MAX_PATH];
    int suffix = 2;
    const char* name = "Custom Workflow";
    if (!wszWorkspaceRoot || !wszWorkspaceRoot[0])
        return FALSE;
    if (!AgentRuntime_EnsureWorkspaceAssets(wszWorkspaceRoot))
        return FALSE;
    MakeSlugFromUtf8(name, wszSlug, COUNTOF(wszSlug));
    PathCombineW(wszDir, wszWorkspaceRoot, L".bikode\\orgs");
    swprintf_s(wszCandidate, COUNTOF(wszCandidate), L"%s\\%s.json", wszDir, wszSlug);
    while (PathFileExistsW(wszCandidate))
        swprintf_s(wszCandidate, COUNTOF(wszCandidate), L"%s\\custom-workflow-%d.json", wszDir, suffix++);
    InitTemplateSpec(&spec, wszWorkspaceRoot, wszCandidate, name, "line");
    AppendOrgNode(&spec, "lead-agent", "Lead Agent", "research", AGENT_BACKEND_API, AGENT_WORKSPACE_ISOLATED,
                  "Inspect the workspace, decide the next useful step, and leave a concise handoff for any agent added after you.", "custom");
    if (!AgentRuntime_SaveOrgSpec(&spec))
        return FALSE;
    AgentRuntime_SetLastSelectedOrg(spec.path);
    if (wszOutPath)
        CopyWideSafe(wszOutPath, cchOutPath, spec.path);
    return TRUE;
}

BOOL AgentRuntime_DuplicateOrgSpec(const OrgSpec* pSpec, WCHAR* wszOutPath, int cchOutPath)
{
    OrgSpec copy;
    WCHAR wszDir[MAX_PATH];
    WCHAR wszSlug[128];
    WCHAR wszCandidate[MAX_PATH];
    int suffix = 2;
    if (!pSpec)
        return FALSE;
    copy = *pSpec;
    CopyStringSafe(copy.name, COUNTOF(copy.name), pSpec->name);
    strncat(copy.name, " Copy", COUNTOF(copy.name) - strlen(copy.name) - 1);
    MakeSlugFromUtf8(copy.name, wszSlug, COUNTOF(wszSlug));
    PathCombineW(wszDir, pSpec->root, L".bikode\\orgs");
    swprintf_s(wszCandidate, COUNTOF(wszCandidate), L"%s\\%s.json", wszDir, wszSlug);
    while (PathFileExistsW(wszCandidate))
    {
        swprintf_s(wszCandidate, COUNTOF(wszCandidate), L"%s\\org-copy-%d.json", wszDir, suffix++);
    }
    CopyWideSafe(copy.path, COUNTOF(copy.path), wszCandidate);
    if (wszOutPath)
        CopyWideSafe(wszOutPath, cchOutPath, wszCandidate);
    return AgentRuntime_SaveOrgSpec(&copy);
}

BOOL AgentRuntime_AddDefaultNode(OrgSpec* pSpec)
{
    OrgNodeSpec* node;
    char id[AGENT_RUNTIME_TEXT_SMALL];
    char title[AGENT_RUNTIME_TEXT_MEDIUM];
    if (!pSpec || pSpec->nodeCount >= AGENT_RUNTIME_MAX_NODES)
        return FALSE;
    _snprintf_s(id, sizeof(id), _TRUNCATE, "node-%d", pSpec->nodeCount + 1);
    _snprintf_s(title, sizeof(title), _TRUNCATE, "Node %d", pSpec->nodeCount + 1);
    node = &pSpec->nodes[pSpec->nodeCount++];
    ZeroMemory(node, sizeof(*node));
    CopyStringSafe(node->id, COUNTOF(node->id), id);
    CopyStringSafe(node->title, COUNTOF(node->title), title);
    CopyStringSafe(node->role, COUNTOF(node->role), "research");
    node->backend = AGENT_BACKEND_API;
    node->workspacePolicy = pSpec->defaultWorkspacePolicy;
    CopyStringSafe(node->prompt, COUNTOF(node->prompt), "Inspect the workspace, contribute one focused finding, and hand off a concise summary.");
    CopyStringSafe(node->group, COUNTOF(node->group), "custom");
    return TRUE;
}

static int FindNodeIndexByIdLocked(const char* nodeId)
{
    int i;
    if (!nodeId)
        return -1;
    for (i = 0; i < s_runtime.org.nodeCount; i++)
    {
        if (_stricmp(s_runtime.nodes[i].id, nodeId) == 0)
            return i;
    }
    return -1;
}

static void RuntimeWriteManifest(void)
{
    JsonWriter w;
    WCHAR wszManifest[MAX_PATH];
    int i;
    if (!s_runtime.runRoot[0])
        return;
    PathCombineW(wszManifest, s_runtime.runRoot, L"manifest.json");
    if (!JsonWriter_Init(&w, 8192))
        return;
    EnterCriticalSection(&s_runtime.cs);
    JsonWriter_BeginObject(&w);
    JsonWriter_String(&w, "runId", s_runtime.runId);
    JsonWriter_String(&w, "orgName", s_runtime.org.name);
    JsonWriter_WString(&w, "workspaceRoot", s_runtime.workspaceRoot);
    JsonWriter_WString(&w, "runRoot", s_runtime.runRoot);
    JsonWriter_Bool(&w, "isRunning", s_runtime.isRunning != FALSE);
    JsonWriter_Bool(&w, "isPaused", s_runtime.isPaused != FALSE);
    JsonWriter_Bool(&w, "isCanceled", s_runtime.isCanceled != FALSE);
    JsonWriter_Key(&w, "nodes");
    JsonWriter_BeginArray(&w);
    for (i = 0; i < s_runtime.org.nodeCount; i++)
    {
        AgentNodeSnapshot* node = &s_runtime.nodes[i];
        int j;
        JsonWriter_BeginObject(&w);
        JsonWriter_String(&w, "id", node->id);
        JsonWriter_String(&w, "title", node->title);
        JsonWriter_String(&w, "role", node->role);
        JsonWriter_String(&w, "backend", BackendSpecName(node->backend));
        JsonWriter_String(&w, "workspace", WorkspacePolicySpecName(node->workspacePolicy));
        JsonWriter_String(&w, "state", AgentRuntime_StateLabel(node->state));
        JsonWriter_Int(&w, "toolCount", node->toolCount);
        JsonWriter_Int(&w, "fileCount", node->fileCount);
        JsonWriter_Int(&w, "inputTokens", node->inputTokens);
        JsonWriter_Int(&w, "outputTokens", node->outputTokens);
        JsonWriter_Int(&w, "startTick", (int)node->startTick);
        JsonWriter_Int(&w, "endTick", (int)node->endTick);
        JsonWriter_String(&w, "lastAction", node->lastAction);
        JsonWriter_String(&w, "summary", node->summary);
        JsonWriter_WString(&w, "workspacePath", node->workspacePath);
        JsonWriter_WString(&w, "transcriptPath", node->transcriptPath);
        JsonWriter_WString(&w, "summaryPath", node->summaryPath);
        JsonWriter_Key(&w, "changedFiles");
        JsonWriter_BeginArray(&w);
        for (j = 0; j < min(node->fileCount, AGENT_RUNTIME_MAX_CHANGED_FILES); j++)
            JsonWriter_WStringValue(&w, node->changedFiles[j]);
        JsonWriter_EndArray(&w);
        JsonWriter_EndObject(&w);
    }
    JsonWriter_EndArray(&w);
    JsonWriter_EndObject(&w);
    LeaveCriticalSection(&s_runtime.cs);
    WriteUtf8File(wszManifest, JsonWriter_GetBuffer(&w));
    JsonWriter_Free(&w);
}

static void RuntimeAppendEventFile(const AgentEvent* pEvent)
{
    JsonWriter w;
    WCHAR wszEvents[MAX_PATH];
    if (!pEvent || !s_runtime.runRoot[0])
        return;
    PathCombineW(wszEvents, s_runtime.runRoot, L"events.jsonl");
    if (!JsonWriter_Init(&w, 1024))
        return;
    JsonWriter_BeginObject(&w);
    JsonWriter_Int(&w, "sequence", pEvent->sequence);
    JsonWriter_Int(&w, "type", (int)pEvent->type);
    JsonWriter_Int(&w, "tick", (int)pEvent->tick);
    JsonWriter_String(&w, "nodeId", pEvent->nodeId);
    JsonWriter_String(&w, "nodeTitle", pEvent->nodeTitle);
    JsonWriter_String(&w, "message", pEvent->message);
    JsonWriter_EndObject(&w);
    AppendUtf8FileLine(wszEvents, JsonWriter_GetBuffer(&w));
    JsonWriter_Free(&w);
}

static void RuntimeAddEvent(int nodeIndex, AgentEventType type, const char* message)
{
    AgentEvent ev;
    AgentEvent* heapEv;
    ZeroMemory(&ev, sizeof(ev));
    EnterCriticalSection(&s_runtime.cs);
    ev.sequence = ++s_runtime.nextEventSeq;
    ev.type = type;
    ev.tick = GetTickCount();
    CopyStringSafe(ev.message, COUNTOF(ev.message), message ? message : "");
    if (nodeIndex >= 0 && nodeIndex < s_runtime.org.nodeCount)
    {
        CopyStringSafe(ev.nodeId, COUNTOF(ev.nodeId), s_runtime.nodes[nodeIndex].id);
        CopyStringSafe(ev.nodeTitle, COUNTOF(ev.nodeTitle), s_runtime.nodes[nodeIndex].title);
        CopyStringSafe(s_runtime.nodes[nodeIndex].lastAction, COUNTOF(s_runtime.nodes[nodeIndex].lastAction), ev.message);
        s_runtime.nodes[nodeIndex].lastUpdateTick = ev.tick;
        if (type == AGENT_EVENT_TOOL)
            s_runtime.nodes[nodeIndex].toolCount++;
    }
    if (s_runtime.eventCount >= AGENT_RUNTIME_MAX_EVENTS)
    {
        memmove(&s_runtime.events[0], &s_runtime.events[1], sizeof(AgentEvent) * (AGENT_RUNTIME_MAX_EVENTS - 1));
        s_runtime.eventCount = AGENT_RUNTIME_MAX_EVENTS - 1;
    }
    s_runtime.events[s_runtime.eventCount++] = ev;
    LeaveCriticalSection(&s_runtime.cs);
    RuntimeAppendEventFile(&ev);
    RuntimeWriteManifest();
    heapEv = (AgentEvent*)malloc(sizeof(AgentEvent));
    if (heapEv)
    {
        *heapEv = ev;
        PostMessage(s_runtime.hwndMain, WM_AGENT_RUNTIME_EVENT, 0, (LPARAM)heapEv);
    }
}

static void RuntimeSetNodeState(int nodeIndex, AgentNodeState state, const char* action)
{
    if (nodeIndex < 0 || nodeIndex >= s_runtime.org.nodeCount)
        return;
    EnterCriticalSection(&s_runtime.cs);
    s_runtime.nodes[nodeIndex].state = state;
    if (state == AGENT_NODE_RUNNING)
        s_runtime.nodes[nodeIndex].startTick = GetTickCount();
    if (state == AGENT_NODE_DONE || state == AGENT_NODE_BLOCKED || state == AGENT_NODE_ERROR || state == AGENT_NODE_CANCELED)
        s_runtime.nodes[nodeIndex].endTick = GetTickCount();
    if (action)
        CopyStringSafe(s_runtime.nodes[nodeIndex].lastAction, COUNTOF(s_runtime.nodes[nodeIndex].lastAction), action);
    LeaveCriticalSection(&s_runtime.cs);
    RuntimeWriteManifest();
}

static BOOL IsPendingNodeState(AgentNodeState state)
{
    return state == AGENT_NODE_IDLE || state == AGENT_NODE_QUEUED;
}

static BOOL IsDependencyFailureState(AgentNodeState state)
{
    return state == AGENT_NODE_BLOCKED || state == AGENT_NODE_ERROR || state == AGENT_NODE_CANCELED;
}

static BOOL NodeHasFailedDependencyLocked(int nodeIndex, char* pszReason, int cchReason)
{
    int i;
    const OrgNodeSpec* spec = &s_runtime.org.nodes[nodeIndex];

    if (pszReason && cchReason > 0)
        pszReason[0] = '\0';

    for (i = 0; i < spec->dependsOnCount; i++)
    {
        int depIndex = FindNodeIndexByIdLocked(spec->dependsOn[i]);
        AgentNodeState depState;

        if (depIndex < 0)
        {
            if (pszReason && cchReason > 0)
                _snprintf_s(pszReason, cchReason, _TRUNCATE, "Blocked: missing dependency '%s'.", spec->dependsOn[i]);
            return TRUE;
        }

        depState = s_runtime.nodes[depIndex].state;
        if (IsDependencyFailureState(depState))
        {
            if (pszReason && cchReason > 0)
            {
                _snprintf_s(pszReason, cchReason, _TRUNCATE,
                    "Blocked by %s (%s).",
                    s_runtime.nodes[depIndex].title[0] ? s_runtime.nodes[depIndex].title : s_runtime.nodes[depIndex].id,
                    AgentRuntime_StateLabel(depState));
            }
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL NodeDepsSatisfiedLocked(int nodeIndex)
{
    int i;
    const OrgNodeSpec* spec = &s_runtime.org.nodes[nodeIndex];
    for (i = 0; i < spec->dependsOnCount; i++)
    {
        int depIndex = FindNodeIndexByIdLocked(spec->dependsOn[i]);
        if (depIndex < 0)
            return FALSE;
        if (s_runtime.nodes[depIndex].state != AGENT_NODE_DONE)
            return FALSE;
    }
    return TRUE;
}

static BOOL SharedMutatorRunningLocked(void)
{
    int i;
    for (i = 0; i < s_runtime.org.nodeCount; i++)
    {
        if (s_runtime.nodes[i].state == AGENT_NODE_RUNNING &&
            s_runtime.nodes[i].workspacePolicy == AGENT_WORKSPACE_SHARED_MUTATING)
            return TRUE;
    }
    return FALSE;
}

static BOOL ResolveCommandPath(LPCWSTR wszName, WCHAR* wszOut, DWORD cchOut)
{
    DWORD dw;
    if (!wszName || !wszOut || cchOut == 0)
        return FALSE;
    wszOut[0] = L'\0';
    dw = SearchPathW(NULL, wszName, NULL, cchOut, wszOut, NULL);
    return dw > 0 && dw < cchOut;
}

static BOOL PathEndsWithInsensitive(LPCWSTR wszPath, LPCWSTR wszSuffix)
{
    int cchPath;
    int cchSuffix;
    if (!wszPath || !wszSuffix)
        return FALSE;
    cchPath = lstrlenW(wszPath);
    cchSuffix = lstrlenW(wszSuffix);
    if (cchPath < cchSuffix)
        return FALSE;
    return _wcsicmp(wszPath + cchPath - cchSuffix, wszSuffix) == 0;
}

static BOOL ResolvePreferredCommandPath(const LPCWSTR* pCandidates, int cCandidates,
                                        WCHAR* wszOut, DWORD cchOut, BOOL* pbUseCmd)
{
    int i;
    if (pbUseCmd)
        *pbUseCmd = FALSE;
    for (i = 0; i < cCandidates; i++)
    {
        if (ResolveCommandPath(pCandidates[i], wszOut, cchOut))
        {
            if (pbUseCmd)
                *pbUseCmd = PathEndsWithInsensitive(wszOut, L".cmd") || PathEndsWithInsensitive(wszOut, L".bat");
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL ResolveExecutable(AgentBackend backend, WCHAR* wszOut, DWORD cchOut, BOOL* pbUseCmd)
{
    static const LPCWSTR kCodexCandidates[] = { L"codex.cmd", L"codex.exe", L"codex" };
    static const LPCWSTR kClaudeCandidates[] = { L"claude.cmd", L"claude.exe", L"claude" };
    if (backend == AGENT_BACKEND_CODEX)
        return ResolvePreferredCommandPath(kCodexCandidates, ARRAYSIZE(kCodexCandidates), wszOut, cchOut, pbUseCmd);
    if (backend == AGENT_BACKEND_CLAUDE)
        return ResolvePreferredCommandPath(kClaudeCandidates, ARRAYSIZE(kClaudeCandidates), wszOut, cchOut, pbUseCmd);
    wszOut[0] = L'\0';
    if (pbUseCmd)
        *pbUseCmd = FALSE;
    return FALSE;
}

static BOOL ResolveCmdExe(WCHAR* wszOut, DWORD cchOut)
{
    DWORD dw;
    if (!wszOut || cchOut == 0)
        return FALSE;
    wszOut[0] = L'\0';
    dw = GetEnvironmentVariableW(L"ComSpec", wszOut, cchOut);
    if (dw > 0 && dw < cchOut)
        return TRUE;
    return ResolveCommandPath(L"cmd.exe", wszOut, cchOut);
}

static void AppendQuoted(WCHAR* wszCmd, size_t cchCmd, LPCWSTR wszValue)
{
    StringCchCatW(wszCmd, cchCmd, L" \"");
    StringCchCatW(wszCmd, cchCmd, wszValue);
    StringCchCatW(wszCmd, cchCmd, L"\"");
}

static BOOL BuildCommandLine(LPCWSTR wszExecutable, LPCWSTR wszArgs, BOOL bUseCmd, WCHAR* wszCmd, size_t cchCmd)
{
    WCHAR wszCmdExe[MAX_PATH];
    wszCmd[0] = L'\0';
    if (!bUseCmd)
    {
        StringCchCatW(wszCmd, cchCmd, L"\"");
        StringCchCatW(wszCmd, cchCmd, wszExecutable);
        StringCchCatW(wszCmd, cchCmd, L"\"");
        if (wszArgs && wszArgs[0])
            StringCchCatW(wszCmd, cchCmd, wszArgs);
        return TRUE;
    }
    if (!ResolveCmdExe(wszCmdExe, ARRAYSIZE(wszCmdExe)))
        return FALSE;
    StringCchCatW(wszCmd, cchCmd, L"\"");
    StringCchCatW(wszCmd, cchCmd, wszCmdExe);
    StringCchCatW(wszCmd, cchCmd, L"\" /d /s /c \"\"");
    StringCchCatW(wszCmd, cchCmd, wszExecutable);
    StringCchCatW(wszCmd, cchCmd, L"\"");
    if (wszArgs && wszArgs[0])
        StringCchCatW(wszCmd, cchCmd, wszArgs);
    StringCchCatW(wszCmd, cchCmd, L"\"");
    return TRUE;
}

static const char* SkipWhitespace(const char* p)
{
    while (p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
        p++;
    return p;
}

static char* JsonExtractString(const char* json, const char* key)
{
    char needle[128];
    const char* p;
    StrBuf sb;
    if (!json || !key)
        return NULL;
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    p = strstr(json, needle);
    if (!p)
        return NULL;
    p = strchr(p + (int)strlen(needle), ':');
    if (!p)
        return NULL;
    p = SkipWhitespace(p + 1);
    if (!p || *p != '"')
        return NULL;
    p++;
    sb_init(&sb, 128);
    while (*p && *p != '"')
    {
        if (*p == '\\' && *(p + 1))
        {
            p++;
            switch (*p)
            {
            case 'n': sb_append(&sb, "\n", 1); break;
            case 'r': sb_append(&sb, "\r", 1); break;
            case 't': sb_append(&sb, "\t", 1); break;
            case '\\': sb_append(&sb, "\\", 1); break;
            case '"': sb_append(&sb, "\"", 1); break;
            default: sb_append(&sb, p, 1); break;
            }
        }
        else
        {
            sb_append(&sb, p, 1);
        }
        p++;
    }
    return sb.data;
}

static char* JsonExtractAfter(const char* json, const char* marker, const char* key)
{
    const char* scope = strstr(json, marker);
    if (!scope)
        return NULL;
    return JsonExtractString(scope, key);
}

// Extract an integer value from a JSON object.  Returns defaultValue on miss.
static int JsonExtractInt(const char* json, const char* key, int defaultValue)
{
    char needle[128];
    const char* p;
    char* endPtr;
    long val;
    if (!json || !key)
        return defaultValue;
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    p = strstr(json, needle);
    if (!p)
        return defaultValue;
    p = strchr(p + (int)strlen(needle), ':');
    if (!p)
        return defaultValue;
    p = SkipWhitespace(p + 1);
    if (!p)
        return defaultValue;
    val = strtol(p, &endPtr, 10);
    if (endPtr == p)
        return defaultValue;
    return (int)val;
}

static void HandleCodexLine(int nodeIndex, const char* line, char** ppszFinal)
{
    char* value;
    if (!line || line[0] != '{')
        return;
    if (strstr(line, "\"type\":\"reasoning\""))
    {
        value = JsonExtractString(line, "text");
        if (value)
        {
            RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, value);
            free(value);
        }
        return;
    }
    if (strstr(line, "\"type\":\"command_execution\""))
    {
        value = JsonExtractString(line, "command");
        if (value)
        {
            char* normalized = NormalizeCommandExecutionText(value);
            RuntimeAddEvent(nodeIndex, AGENT_EVENT_TOOL, normalized ? normalized : value);
            if (normalized)
                free(normalized);
            free(value);
        }
        return;
    }
    // Extract token usage from Codex JSON output ({"type":"usage", "input_tokens":N, "output_tokens":N})
    if (strstr(line, "\"type\":\"usage\"") || strstr(line, "\"input_tokens\""))
    {
        int inTok = JsonExtractInt(line, "input_tokens", 0);
        int outTok = JsonExtractInt(line, "output_tokens", 0);
        if (inTok > 0 || outTok > 0)
        {
            EnterCriticalSection(&s_runtime.cs);
            s_runtime.nodes[nodeIndex].inputTokens += inTok;
            s_runtime.nodes[nodeIndex].outputTokens += outTok;
            LeaveCriticalSection(&s_runtime.cs);
        }
        // Don't return — could carry other fields too
    }
    if (strstr(line, "\"type\":\"agent_message\""))
    {
        value = JsonExtractString(line, "text");
        if (value)
        {
            if (*ppszFinal)
                free(*ppszFinal);
            *ppszFinal = value;
        }
    }
}

static void HandleClaudeLine(int nodeIndex, const char* line, char** ppszFinal)
{
    char* value;
    char* tool;
    char* path;
    char* command;
    char buffer[512];
    if (!line || line[0] != '{')
        return;
    if (strstr(line, "\"type\":\"assistant\"") && strstr(line, "\"type\":\"tool_use\""))
    {
        tool = JsonExtractAfter(line, "\"type\":\"tool_use\"", "name");
        path = JsonExtractAfter(line, "\"type\":\"tool_use\"", "file_path");
        command = JsonExtractAfter(line, "\"type\":\"tool_use\"", "command");
        buffer[0] = '\0';
        if (command && command[0])
            _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%s: %s", tool ? tool : "Tool", command);
        else if (path && path[0])
            _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%s: %s", tool ? tool : "Tool", path);
        else if (tool && tool[0])
            _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%s", tool);
        if (buffer[0])
            RuntimeAddEvent(nodeIndex, AGENT_EVENT_TOOL, buffer);
        if (tool) free(tool);
        if (path) free(path);
        if (command) free(command);
        return;
    }
    if (strstr(line, "\"type\":\"assistant\"") && strstr(line, "\"type\":\"text\""))
    {
        value = JsonExtractAfter(line, "\"type\":\"text\"", "text");
        if (value)
        {
            if (*ppszFinal)
                free(*ppszFinal);
            *ppszFinal = value;
        }
        return;
    }
    // Extract token usage from Claude JSON output
    if (strstr(line, "\"type\":\"usage\"") || strstr(line, "\"input_tokens\""))
    {
        int inTok = JsonExtractInt(line, "input_tokens", 0);
        int outTok = JsonExtractInt(line, "output_tokens", 0);
        if (inTok > 0 || outTok > 0)
        {
            EnterCriticalSection(&s_runtime.cs);
            s_runtime.nodes[nodeIndex].inputTokens += inTok;
            s_runtime.nodes[nodeIndex].outputTokens += outTok;
            LeaveCriticalSection(&s_runtime.cs);
        }
    }
    if (strstr(line, "\"type\":\"result\""))
    {
        value = JsonExtractString(line, "result");
        if (value)
        {
            if (*ppszFinal)
                free(*ppszFinal);
            *ppszFinal = value;
        }
    }
}

static void BuildSubscriptionArgs(AgentBackend backend, LPCWSTR wszWorkspaceRoot, LPCWSTR wszModel,
                                  WCHAR* wszArgs, size_t cchArgs)
{
    wszArgs[0] = L'\0';
    if (backend == AGENT_BACKEND_CODEX)
    {
        StringCchCatW(wszArgs, cchArgs, L" exec --json --dangerously-bypass-approvals-and-sandbox --skip-git-repo-check");
        if (wszWorkspaceRoot && wszWorkspaceRoot[0])
        {
            StringCchCatW(wszArgs, cchArgs, L" -C");
            AppendQuoted(wszArgs, cchArgs, wszWorkspaceRoot);
        }
        if (wszModel && wszModel[0])
        {
            StringCchCatW(wszArgs, cchArgs, L" -m");
            AppendQuoted(wszArgs, cchArgs, wszModel);
        }
    }
    else
    {
        StringCchCatW(wszArgs, cchArgs, L" -p --output-format stream-json --verbose --permission-mode bypassPermissions");
        if (wszModel && wszModel[0])
        {
            StringCchCatW(wszArgs, cchArgs, L" --model");
            AppendQuoted(wszArgs, cchArgs, wszModel);
        }
    }
}

static BOOL WriteAll(HANDLE hPipe, const char* text)
{
    DWORD total = 0;
    DWORD len;
    if (!text)
        return TRUE;
    len = (DWORD)strlen(text);
    while (total < len)
    {
        DWORD written = 0;
        if (!WriteFile(hPipe, text + total, len - total, &written, NULL))
            return FALSE;
        if (written == 0)
            return FALSE;
        total += written;
    }
    return TRUE;
}

static void AppendRepoIndexHitsToPrompt(int nodeIndex, StrBuf* sb)
{
    const OrgNodeSpec* spec = &s_runtime.org.nodes[nodeIndex];
    WCHAR wszRoot[MAX_PATH];
    char query[AGENT_RUNTIME_TEXT_HUGE + 160];
    char* pathHint = NULL;
    char* hits = NULL;
    int i;

    CopyWideSafe(wszRoot, ARRAYSIZE(wszRoot),
        s_runtime.nodes[nodeIndex].workspacePath[0] ? s_runtime.nodes[nodeIndex].workspacePath : s_runtime.workspaceRoot);
    if (!wszRoot[0])
        return;

    for (i = 0; i < spec->dependsOnCount; i++)
    {
        int depIndex = FindNodeIndexByIdLocked(spec->dependsOn[i]);
        if (depIndex >= 0 && s_runtime.nodes[depIndex].fileCount > 0 &&
            s_runtime.nodes[depIndex].changedFiles[0][0])
        {
            pathHint = WideToUtf8Dup(s_runtime.nodes[depIndex].changedFiles[0]);
            break;
        }
    }

    _snprintf_s(query, sizeof(query), _TRUNCATE, "%s %s %s",
        spec->title, spec->role, spec->prompt);

    if (CodeEmbeddingIndex_QueryProject(wszRoot, query, pathHint, 4, &hits) &&
        hits && hits[0] && strstr(hits, "(no strong repo matches found)") == NULL)
    {
        sb_append(sb, "\nLocal repo index hits:\n", -1);
        sb_append(sb, hits, -1);
        sb_append(sb, "Verify exact lines in the workspace before making precise edits.\n", -1);
    }

    if (pathHint)
        free(pathHint);
    if (hits)
        free(hits);
}

static char* BuildNodePrompt(int nodeIndex)
{
    StrBuf sb;
    const OrgNodeSpec* spec = &s_runtime.org.nodes[nodeIndex];
    int i;
    char* workspaceUtf8 = WideToUtf8Dup(s_runtime.nodes[nodeIndex].workspacePath);
    sb_init(&sb, 4096);
    sb_appendf(&sb, "Mission Control org: %s\n", s_runtime.org.name);
    sb_appendf(&sb, "Node: %s (%s)\n", spec->title, spec->role);
    sb_appendf(&sb, "Backend: %s\n", AgentRuntime_BackendLabel(spec->backend));
    sb_appendf(&sb, "Workspace policy: %s\n", AgentRuntime_WorkspaceLabel(spec->workspacePolicy));
    sb_appendf(&sb, "Workspace path: %s\n", workspaceUtf8 && workspaceUtf8[0] ? workspaceUtf8 : "(unknown)");
    if (spec->workspacePolicy == AGENT_WORKSPACE_SHARED_READONLY)
        sb_append(&sb, "Do not mutate files in this node. Inspect and report only.\n", -1);
    else
        sb_append(&sb, "You may change files directly in the workspace if needed. Keep changes reviewable.\n", -1);
    // Give this agent context about ALL completed prior agents so it can
    // build on their work, coordinate, and avoid duplicating effort.
    EnterCriticalSection(&s_runtime.cs);
    {
        BOOL hasPriorWork = FALSE;
        // First pass: check if any prior nodes have completed
        for (i = 0; i < s_runtime.org.nodeCount; i++)
        {
            if (i == nodeIndex) continue;
            if (s_runtime.nodes[i].state == AGENT_NODE_DONE && s_runtime.nodes[i].summary[0])
            {
                hasPriorWork = TRUE;
                break;
            }
        }

        if (hasPriorWork)
        {
            BOOL isDep;
            sb_append(&sb, "\n## Prior agent work\n"
                "The following agents have already completed their tasks. "
                "Build on their output \xe2\x80\x94 do not redo work they already did.\n\n", -1);

            for (i = 0; i < s_runtime.org.nodeCount; i++)
            {
                if (i == nodeIndex) continue;
                if (s_runtime.nodes[i].state != AGENT_NODE_DONE) continue;
                if (!s_runtime.nodes[i].summary[0]) continue;

                // Check if this is an explicit dependency
                isDep = FALSE;
                {
                    int d;
                    for (d = 0; d < spec->dependsOnCount; d++)
                    {
                        int depIdx = FindNodeIndexByIdLocked(spec->dependsOn[d]);
                        if (depIdx == i) { isDep = TRUE; break; }
                    }
                }

                sb_appendf(&sb, "### %s (%s)%s\n",
                    s_runtime.nodes[i].title,
                    s_runtime.org.nodes[i].role,
                    isDep ? " [DEPENDENCY]" : "");
                sb_appendf(&sb, "**Task:** %.*s\n",
                    (int)(strlen(s_runtime.org.nodes[i].prompt) < 300 ?
                          strlen(s_runtime.org.nodes[i].prompt) : 300),
                    s_runtime.org.nodes[i].prompt);
                if (strlen(s_runtime.org.nodes[i].prompt) > 300)
                    sb_append(&sb, "...", 3);
                sb_appendf(&sb, "\n**Result:** %s\n\n", s_runtime.nodes[i].summary);
            }
        }
    }
    LeaveCriticalSection(&s_runtime.cs);
    if (spec->toolCount > 0)
    {
        sb_append(&sb, "Preferred tools:\n", -1);
        for (i = 0; i < spec->toolCount; i++)
            sb_appendf(&sb, "- %s\n", spec->tools[i]);
    }
    sb_append(&sb,
        "Bikode principles:\n"
        "- Keep the work grounded in the current workspace.\n"
        "- End with what changed, what you checked, and remaining risk.\n"
        "- Prefer concise, technical language over fluff.\n\n", -1);
    if (spec->backend == AGENT_BACKEND_API || spec->backend == AGENT_BACKEND_LOCAL)
    {
        AppendWorkspacePromptSnapshot(&sb, s_runtime.nodes[nodeIndex].workspacePath);
        sb_append(&sb, "\n", 1);
    }
    AppendRepoIndexHitsToPrompt(nodeIndex, &sb);
    sb_append(&sb, "\n", 1);
    if (s_runtime.org.userPrompt[0])
    {
        sb_append(&sb, "## Project Brief (from user)\n", -1);
        sb_append(&sb, s_runtime.org.userPrompt, -1);
        sb_append(&sb, "\n\n", 2);
        sb_append(&sb,
            "IMPORTANT: The user wants actual working code, not documentation.\n"
            "- Write real source files (.html, .css, .js, .py, .c, etc.) that implement the project.\n"
            "- Do NOT write markdown reports, specifications, or documentation files.\n"
            "- The handoff summary you return is sufficient for the next agent \xe2\x80\x94 no .md files needed.\n"
            "- Focus your tool calls on creating and editing code files in the workspace.\n\n", -1);
    }
    sb_append(&sb, "Task:\n", -1);
    sb_append(&sb, spec->prompt, -1);
    sb_append(&sb, "\n", 1);
    if (workspaceUtf8)
        free(workspaceUtf8);
    return sb.data;
}

static char* RunSingleSubscriptionAgent(int nodeIndex, AgentBackend backend, LPCWSTR wszWorkspaceRoot,
                                        LPCWSTR wszModel, const char* prompt, StrBuf* pTranscript)
{
    WCHAR wszExe[MAX_PATH];
    WCHAR wszArgs[4096];
    WCHAR wszCmd[8192];
    BOOL bUseCmd = FALSE;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE hStdOutRead = NULL;
    HANDLE hStdOutWrite = NULL;
    HANDLE hStdInRead = NULL;
    HANDLE hStdInWrite = NULL;
    char chunk[1025];
    DWORD cbRead = 0;
    DWORD cbAvailable = 0;
    DWORD exitCode = 0;
    StrBuf line;
    char* final = NULL;

    if (!ResolveExecutable(backend, wszExe, ARRAYSIZE(wszExe), &bUseCmd))
        return DupString(backend == AGENT_BACKEND_CODEX ? "Error: Codex CLI was not found on PATH." : "Error: Claude CLI was not found on PATH.");
    BuildSubscriptionArgs(backend, wszWorkspaceRoot, wszModel, wszArgs, ARRAYSIZE(wszArgs));
    if (!BuildCommandLine(wszExe, wszArgs, bUseCmd, wszCmd, ARRAYSIZE(wszCmd)))
        return DupString("Error: Failed to prepare the agent command line.");

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0))
        return DupString("Error: Failed to create the output pipe.");
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0))
        goto cleanup;
    SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdInRead;
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdOutWrite;

    RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, backend == AGENT_BACKEND_CODEX ? "Codex is driving this node." : "Claude is driving this node.");

    if (!CreateProcessW(NULL, wszCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                        wszWorkspaceRoot && wszWorkspaceRoot[0] ? wszWorkspaceRoot : NULL,
                        &si, &pi))
    {
        final = DupString("Error: Failed to launch the selected agent.");
        goto cleanup;
    }

    CloseHandle(hStdInRead);
    hStdInRead = NULL;
    CloseHandle(hStdOutWrite);
    hStdOutWrite = NULL;

    WriteAll(hStdInWrite, prompt ? prompt : "");
    CloseHandle(hStdInWrite);
    hStdInWrite = NULL;

    sb_init(&line, 256);
    for (;;)
    {
        if (!PeekNamedPipe(hStdOutRead, NULL, 0, NULL, &cbAvailable, NULL))
            break;
        if (cbAvailable == 0)
        {
            if (WaitForSingleObject(pi.hProcess, 120) != WAIT_TIMEOUT)
                break;
            continue;
        }
        if (!ReadFile(hStdOutRead, chunk, min((DWORD)(sizeof(chunk) - 1), cbAvailable), &cbRead, NULL) || cbRead == 0)
            break;
        chunk[cbRead] = '\0';
        sb_append(pTranscript, chunk, (int)cbRead);
        {
            DWORD i;
            for (i = 0; i < cbRead; i++)
            {
                char ch = chunk[i];
                if (ch == '\r')
                    continue;
                if (ch == '\n')
                {
                    if (line.len > 0)
                    {
                        if (backend == AGENT_BACKEND_CODEX)
                            HandleCodexLine(nodeIndex, line.data, &final);
                        else
                            HandleClaudeLine(nodeIndex, line.data, &final);
                        line.len = 0;
                        line.data[0] = '\0';
                    }
                }
                else
                {
                    sb_append(&line, &ch, 1);
                }
            }
        }
        if (s_runtime.isCanceled)
            break;
    }

    if (line.data && line.len > 0)
    {
        if (backend == AGENT_BACKEND_CODEX)
            HandleCodexLine(nodeIndex, line.data, &final);
        else
            HandleClaudeLine(nodeIndex, line.data, &final);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    if (!final)
        final = DupString(exitCode == 0
            ? "Error: The embedded agent finished without returning a user-facing answer."
            : "Error: Agent exited without a final response.");
    else if (exitCode != 0 && strncmp(final, "Error:", 6) != 0)
    {
        char buffer[512];
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "Error: Agent exited with code %lu.\n%s", (unsigned long)exitCode, final);
        free(final);
        final = DupString(buffer);
    }
    else if (exitCode == 0 && IsPlaceholderAgentResult(final))
    {
        free(final);
        final = DupString("Error: The embedded agent finished without returning a usable final answer.");
    }

cleanup:
    if (hStdOutRead) CloseHandle(hStdOutRead);
    if (hStdOutWrite) CloseHandle(hStdOutWrite);
    if (hStdInRead) CloseHandle(hStdInRead);
    if (hStdInWrite) CloseHandle(hStdInWrite);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (line.data) sb_free(&line);
    return final ? final : DupString("Error: Failed to execute the subscription agent.");
}

// Callback adapter: forwards AIAgent_RunToolLoop events into the runtime event log
static void RuntimeToolLoopCallback(int nodeIndex, int eventType, const char* message)
{
    if (eventType == 1)
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_TOOL, message ? message : "tool call");
    else
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, message ? message : "");
}

static char* BuildApiNodeSystemPrompt(const OrgNodeSpec* spec, const char* workspaceUtf8)
{
    StrBuf sb;
    sb_init(&sb, 8192);

    sb_append(&sb,
        "You are a Bikode Mission Control node. Stay concise, repository-aware, and reality-based.\n\n"
        "You have access to tools that let you operate the workspace natively: "
        "read and write files, execute commands, and explore the filesystem.\n\n"
        "## Available Tools\n\n"
        "To use a tool, include a tool call block in your response:\n\n"
        "<tool_call>\n"
        "{\"name\": \"tool_name\", \"param1\": \"value1\"}\n"
        "</tool_call>\n\n"
        "You can include multiple tool calls in a single response. After tools execute, you will receive their results and can continue.\n\n"

        "### read_file\n"
        "Read the contents of a file from disk.\n"
        "Parameters: name, path, optional start_line, optional line_count\n"
        "Example: {\"name\": \"read_file\", \"path\": \"src/main.c\"}\n\n"

        "### write_file\n"
        "Create or overwrite a file with the given content.\n"
        "Parameters: name, path, content\n"
        "Example: {\"name\": \"write_file\", \"path\": \"hello.c\", \"content\": \"#include <stdio.h>\\nint main() { return 0; }\"}\n\n"

        "### replace_in_file\n"
        "Find and replace text in an existing file. Replaces the first occurrence.\n"
        "Parameters: name, path, old_text, new_text\n"
        "Example: {\"name\": \"replace_in_file\", \"path\": \"main.c\", \"old_text\": \"return 0;\", \"new_text\": \"return EXIT_SUCCESS;\"}\n\n"

        "### run_command\n"
        "Execute a shell command and return its output.\n"
        "Parameters: name, command, optional cwd\n"
        "Example: {\"name\": \"run_command\", \"command\": \"npm test\", \"cwd\": \"webapp\"}\n\n"

        "### list_dir\n"
        "List the contents of a directory.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"list_dir\", \"path\": \"src\"}\n\n"

        "### make_dir\n"
        "Create a directory recursively.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"make_dir\", \"path\": \"scripts/deploy\"}\n\n"

        "### semantic_search\n"
        "Search the workspace using local code embeddings.\n"
        "Parameters: name, query, optional path, optional max_results\n"
        "Example: {\"name\": \"semantic_search\", \"query\": \"database connection\", \"max_results\": 4}\n\n"

        "### lint\n"
        "Auto-detect and run linters (Ruff, Biome, ESLint, Clippy, golangci-lint, ast-grep). "
        "Detects config files in workspace or specify a tool.\n"
        "Parameters: name, optional path, optional command (tool name)\n"
        "Example: {\"name\": \"lint\"}\n"
        "Example: {\"name\": \"lint\", \"command\": \"ruff\"}\n\n"

        "### format\n"
        "Auto-detect and run code formatters (Ruff, Biome, Prettier, Black, rustfmt, gofmt).\n"
        "Parameters: name, optional path, optional command (formatter name)\n"
        "Example: {\"name\": \"format\"}\n\n"

        "## Guidelines\n"
        "- Read files before modifying them.\n"
        "- Use replace_in_file for targeted edits; write_file for new files.\n"
        "- Relative paths resolve from the workspace root.\n"
        "- All JSON strings must use proper escaping (\\n for newlines, \\\" for quotes).\n"
        "- When your work is complete, stop calling tools and return a final summary.\n"
        "- Your final message becomes the handoff to the next agent. Keep it under 3000 chars.\n"
        "  Structure it as: what you did, key files created/changed, decisions made,\n"
        "  and anything the next agent needs to know.\n"
        "- Do NOT write markdown reports, specification docs, or .md files as deliverables.\n"
        "  Your handoff summary IS the report. Write actual source code files instead.\n"
        "- Prioritize creating working code files over documentation.\n", -1);

    if (spec->workspacePolicy == AGENT_WORKSPACE_SHARED_READONLY)
        sb_append(&sb, "- IMPORTANT: This node is READ-ONLY. Do NOT write or modify any files.\n", -1);

    sb_appendf(&sb, "- Workspace root: %s\n",
        workspaceUtf8 && workspaceUtf8[0] ? workspaceUtf8 : ".");
    sb_appendf(&sb, "- Runtime platform: Windows Win32. Commands use cmd.exe.\n");

    return sb.data;
}

static char* RunApiNode(int nodeIndex, const OrgNodeSpec* spec, StrBuf* pTranscript)
{
    AIProviderConfig cfg;
    const AIConfig* pBridgeCfg = AIBridge_GetConfig();
    char* prompt;
    char* systemPrompt;
    char* workspaceUtf8;
    AIAgentLoopResult loopResult;

    if (!pBridgeCfg)
        return DupString("Error: No AI provider configuration is available.");

    if (spec->backend == AGENT_BACKEND_LOCAL)
    {
        EAIProvider detected = AIProvider_DetectLocal();
        if (detected >= AI_PROVIDER_COUNT)
            return DupString("Error: No local model server detected (Ollama, LM Studio, llama.cpp, vLLM, LocalAI).");
        AIProviderConfig_InitDefaults(&cfg, detected);
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, "Using local model server.");
    }
    else
    {
        cfg = pBridgeCfg->providerCfg;
    }
    if (spec->model[0])
        CopyStringSafe(cfg.szModel, COUNTOF(cfg.szModel), spec->model);

    workspaceUtf8 = WideToUtf8Dup(s_runtime.nodes[nodeIndex].workspacePath);
    systemPrompt = BuildApiNodeSystemPrompt(spec, workspaceUtf8);
    prompt = BuildNodePrompt(nodeIndex);

    sb_append(pTranscript, "System:\n", -1);
    sb_append(pTranscript, systemPrompt, -1);
    sb_append(pTranscript, "\n\n---\n\nUser:\n", -1);
    sb_append(pTranscript, prompt, -1);
    sb_append(pTranscript, "\n\n---\n\n", -1);

    RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, "Running agentic tool loop.");

    memset(&loopResult, 0, sizeof(loopResult));
    AIAgent_RunToolLoop(&cfg, systemPrompt, prompt, 0, 0,
                        RuntimeToolLoopCallback, nodeIndex, &loopResult);

    // Record token counts and tool/file metrics
    EnterCriticalSection(&s_runtime.cs);
    s_runtime.nodes[nodeIndex].inputTokens += loopResult.inputTokens;
    s_runtime.nodes[nodeIndex].outputTokens += loopResult.outputTokens;
    s_runtime.nodes[nodeIndex].toolCount += loopResult.toolCalls;
    LeaveCriticalSection(&s_runtime.cs);

    if (loopResult.pszResult)
        sb_append(pTranscript, loopResult.pszResult, -1);

    {
        char metricMsg[256];
        _snprintf_s(metricMsg, sizeof(metricMsg), _TRUNCATE,
            "Tokens: %d in / %d out, tools: %d, file mutations: %d",
            loopResult.inputTokens, loopResult.outputTokens,
            loopResult.toolCalls, loopResult.filesChanged);
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_METRIC, metricMsg);
    }

    free(systemPrompt);
    free(prompt);
    if (workspaceUtf8) free(workspaceUtf8);

    return loopResult.pszResult ? loopResult.pszResult : DupString("Error: The provider did not return a response.");
}

static char* BuildRelayFallbackResult(const char* codexSummary, const char* claudeIssue)
{
    StrBuf sb;
    sb_init(&sb, 1024);
    sb_append(&sb,
        "Codex completed the implementation pass, but Claude did not finish the review pass.\n\n"
        "Codex handoff:\n", -1);
    sb_append(&sb, codexSummary && codexSummary[0] ? codexSummary : "(no handoff summary)", -1);
    if (claudeIssue && claudeIssue[0])
    {
        sb_append(&sb, "\n\nClaude issue:\n", -1);
        sb_append(&sb, claudeIssue, -1);
    }
    return sb.data;
}

static char* RunSubscriptionNode(int nodeIndex, const OrgNodeSpec* spec, StrBuf* pTranscript)
{
    char* prompt;
    char* first = NULL;
    char* second = NULL;
    char* handoffSummary = NULL;
    WCHAR wszModel[128] = L"";
    Utf8ToWide(spec->model, wszModel, ARRAYSIZE(wszModel));
    if (spec->backend == AGENT_BACKEND_CODEX && !AISubscriptionAgent_IsAuthenticated(AI_CHAT_ACCESS_CODEX))
        return DupString("Error: Codex is not authenticated.");
    if (spec->backend == AGENT_BACKEND_CLAUDE && !AISubscriptionAgent_IsAuthenticated(AI_CHAT_ACCESS_CLAUDE))
        return DupString("Error: Claude is not authenticated.");
    if (spec->backend == AGENT_BACKEND_RELAY && !AISubscriptionAgent_IsAuthenticated(AI_CHAT_ACCESS_CODEX_CLAUDE))
        return DupString("Error: Relay mode requires both Codex and Claude to be authenticated.");

    prompt = BuildNodePrompt(nodeIndex);
    if (spec->backend == AGENT_BACKEND_RELAY)
    {
        StrBuf handoff;
        char* mergedPrompt;
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, "Relay flow starting with Codex.");
        first = RunSingleSubscriptionAgent(nodeIndex, AGENT_BACKEND_CODEX, s_runtime.nodes[nodeIndex].workspacePath, wszModel, prompt, pTranscript);
        if (first && strncmp(first, "Error:", 6) == 0 && !IsMissingFinalAnswerError(first))
        {
            free(prompt);
            return first;
        }
        handoffSummary = BuildRelayHandoffSummary(first);
        sb_append(pTranscript, "\n\n--- Relay Handoff ---\n\n", -1);
        if (handoffSummary)
            sb_append(pTranscript, handoffSummary, -1);
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_HANDOFF, "Codex handoff ready. Claude is reviewing the same workspace.");
        sb_init(&handoff, 4096);
        sb_append(&handoff, prompt, -1);
        sb_append(&handoff, "\n\nCodex handoff summary:\n", -1);
        sb_append(&handoff, handoffSummary ? handoffSummary : "(no handoff)", -1);
        mergedPrompt = handoff.data;
        second = RunSingleSubscriptionAgent(nodeIndex, AGENT_BACKEND_CLAUDE, s_runtime.nodes[nodeIndex].workspacePath, wszModel, mergedPrompt, pTranscript);
        sb_free(&handoff);
        free(prompt);
        if (first)
            free(first);
        if (!second)
            second = BuildRelayFallbackResult(handoffSummary, "Error: Relay review did not produce a final response.");
        else if (strncmp(second, "Error:", 6) == 0)
        {
            char* relayFallback = BuildRelayFallbackResult(handoffSummary, second);
            free(second);
            second = relayFallback;
        }
        if (handoffSummary)
            free(handoffSummary);
        return second;
    }
    first = RunSingleSubscriptionAgent(nodeIndex, spec->backend, s_runtime.nodes[nodeIndex].workspacePath, wszModel, prompt, pTranscript);
    free(prompt);
    return first;
}

static BOOL PrepareWorkspaceForNode(int nodeIndex, WCHAR* wszOutPath, int cchOutPath)
{
    const OrgNodeSpec* spec = &s_runtime.org.nodes[nodeIndex];
    WCHAR wszBase[MAX_PATH];
    WCHAR wszRunBase[MAX_PATH];
    WCHAR wszNodeDir[MAX_PATH];
    WCHAR wszGitMarker[MAX_PATH];
    WCHAR wszRunIdW[AGENT_RUNTIME_TEXT_SMALL];
    WCHAR wszNodeIdW[AGENT_RUNTIME_TEXT_SMALL];
    BOOL bIsGit = FALSE;
    if (!wszOutPath || cchOutPath <= 0)
        return FALSE;
    wszOutPath[0] = L'\0';
    if (spec->workspacePolicy != AGENT_WORKSPACE_ISOLATED)
    {
        CopyWideSafe(wszOutPath, cchOutPath, s_runtime.workspaceRoot);
        return TRUE;
    }

    PathCombineW(wszGitMarker, s_runtime.workspaceRoot, L".git");
    bIsGit = PathFileExistsW(wszGitMarker);
    PathCombineW(wszBase, s_runtime.workspaceRoot, bIsGit ? L".bikode\\worktrees" : L".bikode\\sandboxes");
    Utf8ToWide(s_runtime.runId, wszRunIdW, COUNTOF(wszRunIdW));
    Utf8ToWide(spec->id, wszNodeIdW, COUNTOF(wszNodeIdW));
    PathCombineW(wszRunBase, wszBase, wszRunIdW);
    if (!EnsureDirExists(wszRunBase))
        return FALSE;
    PathCombineW(wszNodeDir, wszRunBase, wszNodeIdW);

    if (bIsGit)
    {
        WCHAR wszCmd[2048];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD exitCode = (DWORD)-1;
        swprintf_s(wszCmd, COUNTOF(wszCmd), L"git -C \"%s\" worktree add --force --detach \"%s\" HEAD", s_runtime.workspaceRoot, wszNodeDir);
        ZeroMemory(&si, sizeof(si));
        ZeroMemory(&pi, sizeof(pi));
        si.cb = sizeof(si);
        EnsureDirExists(wszNodeDir);
        if (CreateProcessW(NULL, wszCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, s_runtime.workspaceRoot, &si, &pi))
        {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &exitCode);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (exitCode == 0)
        {
            CopyWideSafe(wszOutPath, cchOutPath, wszNodeDir);
            return TRUE;
        }
    }
    if (CopyTreeRecursive(s_runtime.workspaceRoot, wszNodeDir))
    {
        CopyWideSafe(wszOutPath, cchOutPath, wszNodeDir);
        return TRUE;
    }
    return FALSE;
}

static void RuntimePersistNodeFiles(int nodeIndex, const char* transcript, const char* summary)
{
    WCHAR wszNodeDir[MAX_PATH];
    WCHAR wszTranscript[MAX_PATH];
    WCHAR wszSummary[MAX_PATH];
    WCHAR wszNodeIdW[AGENT_RUNTIME_TEXT_SMALL];
    Utf8ToWide(s_runtime.nodes[nodeIndex].id, wszNodeIdW, COUNTOF(wszNodeIdW));
    PathCombineW(wszNodeDir, s_runtime.runRoot, wszNodeIdW);
    EnsureDirExists(wszNodeDir);
    PathCombineW(wszTranscript, wszNodeDir, L"transcript.txt");
    PathCombineW(wszSummary, wszNodeDir, L"summary.txt");
    WriteUtf8File(wszTranscript, transcript ? transcript : "");
    WriteUtf8File(wszSummary, summary ? summary : "");
    EnterCriticalSection(&s_runtime.cs);
    CopyWideSafe(s_runtime.nodes[nodeIndex].transcriptPath, COUNTOF(s_runtime.nodes[nodeIndex].transcriptPath), wszTranscript);
    CopyWideSafe(s_runtime.nodes[nodeIndex].summaryPath, COUNTOF(s_runtime.nodes[nodeIndex].summaryPath), wszSummary);
    LeaveCriticalSection(&s_runtime.cs);
}

static unsigned __stdcall RuntimeNodeThreadProc(void* pParam)
{
    NodeThreadCtx* ctx = (NodeThreadCtx*)pParam;
    int nodeIndex;
    const OrgNodeSpec* spec;
    FileSnapshotEntry baseline[512];
    int baselineCount;
    int changedCount = 0;
    StrBuf transcript;
    char* result = NULL;
    char changeMessage[1024];
    char* firstChangedUtf8 = NULL;
    BOOL missingFinalAnswer = FALSE;
    WCHAR wszFirstChanged[MAX_PATH];
    if (!ctx)
        return 0;
    nodeIndex = ctx->nodeIndex;
    free(ctx);
    spec = &s_runtime.org.nodes[nodeIndex];
    sb_init(&transcript, 8192);
    wszFirstChanged[0] = L'\0';
    changeMessage[0] = '\0';

    if (!PrepareWorkspaceForNode(nodeIndex, s_runtime.nodes[nodeIndex].workspacePath, COUNTOF(s_runtime.nodes[nodeIndex].workspacePath)))
    {
        RuntimeSetNodeState(nodeIndex, AGENT_NODE_ERROR, "Failed to prepare workspace");
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_ERROR, "Failed to prepare the workspace for this node.");
        goto finish;
    }

    baselineCount = CaptureWorkspaceSnapshot(s_runtime.nodes[nodeIndex].workspacePath, baseline, ARRAYSIZE(baseline));
    RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, "Workspace ready. Node execution started.");

    if (spec->backend == AGENT_BACKEND_API || spec->backend == AGENT_BACKEND_LOCAL)
        result = RunApiNode(nodeIndex, spec, &transcript);
    else
        result = RunSubscriptionNode(nodeIndex, spec, &transcript);

    missingFinalAnswer = IsMissingFinalAnswerError(result);

    EnterCriticalSection(&s_runtime.cs);
    changedCount = CollectChangedFiles(s_runtime.nodes[nodeIndex].workspacePath, baseline, baselineCount, &s_runtime.nodes[nodeIndex]);
    if (changedCount > 0 && s_runtime.nodes[nodeIndex].changedFiles[0][0])
        CopyWideSafe(wszFirstChanged, COUNTOF(wszFirstChanged), s_runtime.nodes[nodeIndex].changedFiles[0]);
    LeaveCriticalSection(&s_runtime.cs);

    if (missingFinalAnswer && changedCount > 0)
    {
        char* fallback = BuildWorkspaceChangeFallbackResult(spec->title, changedCount, wszFirstChanged);
        if (result)
            free(result);
        result = fallback;
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, "Changes were captured even though the agent skipped the final handoff.");
    }

    if (result && strncmp(result, "Error:", 6) == 0)
    {
        RuntimeSetNodeState(nodeIndex, AGENT_NODE_ERROR, result);
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_ERROR, result);
    }
    else
    {
        RuntimeSetNodeState(nodeIndex, AGENT_NODE_DONE, "Node completed");
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_STATUS, "Node completed successfully.");
    }

    EnterCriticalSection(&s_runtime.cs);
    CopyStringSafe(s_runtime.nodes[nodeIndex].summary, COUNTOF(s_runtime.nodes[nodeIndex].summary), result ? result : "");
    LeaveCriticalSection(&s_runtime.cs);

    if (changedCount > 0)
    {
        firstChangedUtf8 = WideToUtf8Dup(wszFirstChanged);
        _snprintf_s(changeMessage, sizeof(changeMessage), _TRUNCATE,
            "Changed %d file%s.%s%s",
            changedCount,
            changedCount == 1 ? "" : "s",
            firstChangedUtf8 && firstChangedUtf8[0] ? " First: " : "",
            firstChangedUtf8 && firstChangedUtf8[0] ? firstChangedUtf8 : "");
        RuntimeAddEvent(nodeIndex, AGENT_EVENT_FILE, changeMessage);
        if (firstChangedUtf8)
        {
            free(firstChangedUtf8);
            firstChangedUtf8 = NULL;
        }
    }
    RuntimeWriteManifest();

finish:
    RuntimePersistNodeFiles(nodeIndex, transcript.data ? transcript.data : "", result ? result : "");
    if (result)
        free(result);
    if (transcript.data)
        sb_free(&transcript);
    return 0;
}

static unsigned __stdcall RuntimeSchedulerThreadProc(void* pParam)
{
    UNREFERENCED_PARAMETER(pParam);
    for (;;)
    {
        BOOL anyRunning = FALSE;
        BOOL anyPending = FALSE;
        int launchIndex = -1;
        int i;

        if (s_runtime.isCanceled)
        {
            EnterCriticalSection(&s_runtime.cs);
            for (i = 0; i < s_runtime.org.nodeCount; i++)
            {
                if (IsPendingNodeState(s_runtime.nodes[i].state))
                    s_runtime.nodes[i].state = AGENT_NODE_CANCELED;
            }
            LeaveCriticalSection(&s_runtime.cs);
        }

        if (s_runtime.isPaused)
        {
            Sleep(200);
            continue;
        }

        EnterCriticalSection(&s_runtime.cs);
        for (i = 0; i < s_runtime.org.nodeCount; i++)
        {
            AgentNodeState state = s_runtime.nodes[i].state;
            char reason[256];
            if (state == AGENT_NODE_RUNNING)
                anyRunning = TRUE;
            if (IsPendingNodeState(state))
            {
                if (NodeHasFailedDependencyLocked(i, reason, ARRAYSIZE(reason)))
                {
                    s_runtime.nodes[i].state = AGENT_NODE_BLOCKED;
                    s_runtime.nodes[i].endTick = GetTickCount();
                    CopyStringSafe(s_runtime.nodes[i].lastAction, COUNTOF(s_runtime.nodes[i].lastAction), reason);
                }
                else
                {
                    anyPending = TRUE;
                    if (NodeDepsSatisfiedLocked(i))
                    {
                        if (s_runtime.org.nodes[i].workspacePolicy == AGENT_WORKSPACE_SHARED_MUTATING && SharedMutatorRunningLocked())
                        {
                            s_runtime.nodes[i].state = AGENT_NODE_QUEUED;
                            CopyStringSafe(s_runtime.nodes[i].lastAction, COUNTOF(s_runtime.nodes[i].lastAction), "Queued behind another mutating node.");
                        }
                        else if (launchIndex < 0)
                        {
                            launchIndex = i;
                            s_runtime.nodes[i].state = AGENT_NODE_RUNNING;
                            s_runtime.nodes[i].startTick = GetTickCount();
                            CopyStringSafe(s_runtime.nodes[i].lastAction, COUNTOF(s_runtime.nodes[i].lastAction), "Queued node is starting.");
                        }
                    }
                    else
                    {
                        s_runtime.nodes[i].state = AGENT_NODE_QUEUED;
                        CopyStringSafe(s_runtime.nodes[i].lastAction, COUNTOF(s_runtime.nodes[i].lastAction), "Queued behind unfinished dependencies.");
                    }
                }
            }
        }
        LeaveCriticalSection(&s_runtime.cs);

        if (launchIndex >= 0)
        {
            NodeThreadCtx* ctx = (NodeThreadCtx*)malloc(sizeof(NodeThreadCtx));
            uintptr_t hThread;
            if (ctx)
            {
                ctx->nodeIndex = launchIndex;
                RuntimeAddEvent(launchIndex, AGENT_EVENT_STATUS, "Queued node is starting.");
                hThread = _beginthreadex(NULL, 0, RuntimeNodeThreadProc, ctx, 0, NULL);
                if (hThread)
                    s_runtime.nodeThreads[launchIndex] = (HANDLE)hThread;
                else
                {
                    free(ctx);
                    RuntimeSetNodeState(launchIndex, AGENT_NODE_ERROR, "Failed to start node thread");
                    RuntimeAddEvent(launchIndex, AGENT_EVENT_ERROR, "Failed to create the node worker thread.");
                }
            }
        }

        if (!anyPending && !anyRunning && launchIndex < 0)
            break;
        Sleep(150);
    }

    for (int i = 0; i < s_runtime.org.nodeCount; i++)
    {
        if (s_runtime.nodeThreads[i])
        {
            WaitForSingleObject(s_runtime.nodeThreads[i], INFINITE);
            CloseHandle(s_runtime.nodeThreads[i]);
            s_runtime.nodeThreads[i] = NULL;
        }
    }

    InterlockedExchange(&s_runtime.isRunning, FALSE);
    RuntimeAddEvent(-1, AGENT_EVENT_SYSTEM, s_runtime.isCanceled ? "Mission Control run finished after cancellation." : "Mission Control run finished.");
    RuntimeWriteManifest();
    return 0;
}

BOOL AgentRuntime_Init(HWND hwndMain)
{
    ZeroMemory(&s_runtime, sizeof(s_runtime));
    s_runtime.hwndMain = hwndMain;
    InitializeCriticalSection(&s_runtime.cs);
    s_runtime.initialized = TRUE;
    if (szIniFile[0])
        GetPrivateProfileStringW(L"MissionControl", L"LastOrg", L"", s_runtime.lastSelectedOrg, COUNTOF(s_runtime.lastSelectedOrg), szIniFile);
    return TRUE;
}

void AgentRuntime_Shutdown(void)
{
    if (!s_runtime.initialized)
        return;
    AgentRuntime_Cancel();
    if (s_runtime.schedulerThread)
    {
        WaitForSingleObject(s_runtime.schedulerThread, 5000);
        CloseHandle(s_runtime.schedulerThread);
        s_runtime.schedulerThread = NULL;
    }
    DeleteCriticalSection(&s_runtime.cs);
    ZeroMemory(&s_runtime, sizeof(s_runtime));
}

BOOL AgentRuntime_GetLastSelectedOrg(WCHAR* wszPath, int cchPath)
{
    if (!wszPath || cchPath <= 0)
        return FALSE;
    wszPath[0] = L'\0';
    if (!s_runtime.lastSelectedOrg[0])
        return FALSE;
    lstrcpynW(wszPath, s_runtime.lastSelectedOrg, cchPath);
    return TRUE;
}

void AgentRuntime_SetLastSelectedOrg(LPCWSTR wszPath)
{
    CopyWideSafe(s_runtime.lastSelectedOrg, COUNTOF(s_runtime.lastSelectedOrg), wszPath);
    if (szIniFile[0])
        WritePrivateProfileStringW(L"MissionControl", L"LastOrg", wszPath ? wszPath : L"", szIniFile);
}

BOOL AgentRuntime_Start(const OrgSpec* pSpec)
{
    SYSTEMTIME st;
    WCHAR wszRuns[MAX_PATH];
    WCHAR wszRunIdW[AGENT_RUNTIME_TEXT_SMALL];
    uintptr_t hThread;
    const AIConfig* pCfg;
    EAIChatAccessMode chatMode;
    int i;
    if (!pSpec)
        return FALSE;
    if (s_runtime.isRunning)
        return FALSE;
    ZeroMemory(s_runtime.nodeThreads, sizeof(s_runtime.nodeThreads));
    EnterCriticalSection(&s_runtime.cs);
    s_runtime.org = *pSpec;
    s_runtime.eventCount = 0;
    s_runtime.nextEventSeq = 0;
    pCfg = AIBridge_GetConfig();
    chatMode = pCfg ? pCfg->eChatAccessMode : AI_CHAT_ACCESS_API_PROVIDER;
    CopyWideSafe(s_runtime.workspaceRoot, COUNTOF(s_runtime.workspaceRoot), pSpec->root[0] ? pSpec->root : RuntimeWorkspaceRoot());
    GetLocalTime(&st);
    _snprintf_s(s_runtime.runId, sizeof(s_runtime.runId), _TRUNCATE, "%04d%02d%02d-%02d%02d%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Utf8ToWide(s_runtime.runId, wszRunIdW, COUNTOF(wszRunIdW));
    PathCombineW(wszRuns, s_runtime.workspaceRoot, L".bikode\\runs");
    EnsureDirExists(wszRuns);
    PathCombineW(s_runtime.runRoot, wszRuns, wszRunIdW);
    EnsureDirExists(s_runtime.runRoot);
    for (i = 0; i < s_runtime.org.nodeCount; i++)
    {
        OrgNodeSpec* spec = &s_runtime.org.nodes[i];
        AgentNodeSnapshot* node = &s_runtime.nodes[i];
        ZeroMemory(node, sizeof(*node));
        spec->backend = ResolveRuntimeBackend(spec->backend, chatMode);
        ApplyConfiguredRuntimeModel(spec, pCfg);
        CopyStringSafe(node->id, COUNTOF(node->id), spec->id);
        CopyStringSafe(node->title, COUNTOF(node->title), spec->title);
        CopyStringSafe(node->role, COUNTOF(node->role), spec->role);
        CopyStringSafe(node->group, COUNTOF(node->group), spec->group);
        node->backend = spec->backend;
        node->workspacePolicy = spec->workspacePolicy;
        node->state = spec->dependsOnCount > 0 ? AGENT_NODE_QUEUED : AGENT_NODE_IDLE;
        CopyStringSafe(node->lastAction, COUNTOF(node->lastAction),
            spec->dependsOnCount > 0 ? "Queued behind unfinished dependencies." : "Waiting to be scheduled.");
    }
    LeaveCriticalSection(&s_runtime.cs);
    InterlockedExchange(&s_runtime.isCanceled, FALSE);
    InterlockedExchange(&s_runtime.isPaused, FALSE);
    InterlockedExchange(&s_runtime.isRunning, TRUE);
    AgentRuntime_SetLastSelectedOrg(pSpec->path);
    RuntimeAddEvent(-1, AGENT_EVENT_SYSTEM, "Mission Control run started.");
    RuntimeWriteManifest();
    hThread = _beginthreadex(NULL, 0, RuntimeSchedulerThreadProc, NULL, 0, NULL);
    if (!hThread)
    {
        InterlockedExchange(&s_runtime.isRunning, FALSE);
        RuntimeAddEvent(-1, AGENT_EVENT_ERROR, "Failed to create the Mission Control scheduler thread.");
        return FALSE;
    }
    s_runtime.schedulerThread = (HANDLE)hThread;
    return TRUE;
}

void AgentRuntime_Cancel(void)
{
    if (!AgentRuntime_IsRunning())
        return;
    InterlockedExchange(&s_runtime.isPaused, FALSE);
    InterlockedExchange(&s_runtime.isCanceled, TRUE);
    RuntimeAddEvent(-1, AGENT_EVENT_SYSTEM, "Mission Control cancellation requested.");
    RuntimeWriteManifest();
}

void AgentRuntime_SetPaused(BOOL bPaused)
{
    InterlockedExchange(&s_runtime.isPaused, bPaused ? TRUE : FALSE);
    RuntimeAddEvent(-1, AGENT_EVENT_SYSTEM, bPaused ? "Mission Control paused." : "Mission Control resumed.");
}

BOOL AgentRuntime_IsRunning(void)
{
    return InterlockedCompareExchange(&s_runtime.isRunning, FALSE, FALSE) != FALSE;
}

BOOL AgentRuntime_GetSnapshot(AgentRuntimeSnapshot* pSnapshot)
{
    int i;
    if (!pSnapshot)
        return FALSE;
    ZeroMemory(pSnapshot, sizeof(*pSnapshot));
    EnterCriticalSection(&s_runtime.cs);
    pSnapshot->hasRun = s_runtime.runId[0] != '\0';
    pSnapshot->isRunning = s_runtime.isRunning != FALSE;
    pSnapshot->isPaused = s_runtime.isPaused != FALSE;
    pSnapshot->isCanceled = s_runtime.isCanceled != FALSE;
    CopyWideSafe(pSnapshot->workspaceRoot, COUNTOF(pSnapshot->workspaceRoot), s_runtime.workspaceRoot);
    CopyWideSafe(pSnapshot->runRoot, COUNTOF(pSnapshot->runRoot), s_runtime.runRoot);
    CopyStringSafe(pSnapshot->runId, COUNTOF(pSnapshot->runId), s_runtime.runId);
    pSnapshot->org = s_runtime.org;
    pSnapshot->nodeCount = s_runtime.org.nodeCount;
    for (i = 0; i < pSnapshot->nodeCount; i++)
        pSnapshot->nodes[i] = s_runtime.nodes[i];
    pSnapshot->eventCount = s_runtime.eventCount;
    for (i = 0; i < s_runtime.eventCount; i++)
        pSnapshot->events[i] = s_runtime.events[i];
    LeaveCriticalSection(&s_runtime.cs);
    return TRUE;
}

void AgentRuntime_OpenPathInExplorer(LPCWSTR wszPath)
{
    if (!wszPath || !wszPath[0])
        return;
    ShellExecuteW(NULL, L"open", wszPath, NULL, NULL, SW_SHOWNORMAL);
}
