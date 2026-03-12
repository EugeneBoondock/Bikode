/******************************************************************************
*
* Biko
*
* AISubscriptionAgent.c
*   Chat-panel backend for locally installed subscription-based coding agents.
*
******************************************************************************/

#include "AISubscriptionAgent.h"
#include "AIAgent.h"
#include "AIDirectCall.h"
#include "AIContext.h"
#include "FileManager.h"
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <process.h>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")

extern WCHAR szCurFile[MAX_PATH + 40];

typedef struct {
    char* data;
    int len;
    int cap;
} StrBuf;

typedef struct {
    AIConfig config;
    char* message;
    AIChatAttachment attachments[AI_MAX_CHAT_ATTACHMENTS];
    int attachmentCount;
    HWND hwndTarget;
} SubscriptionJob;

static volatile LONG s_busy = FALSE;
static char s_codexSession[128] = "";
static char s_claudeSession[128] = "";
typedef struct {
    WCHAR path[MAX_PATH];
    FILETIME writeTime;
    DWORD attributes;
} WorkspaceEntry;

typedef struct {
    WorkspaceEntry* entries;
    int count;
    int cap;
    ULONGLONG lastScanTick;
    WCHAR lastOpenedFile[MAX_PATH];
} WorkspaceTracker;

static char* DupString(const char* text);
static char* NormalizeCommandExecutionText(const char* text);
static const char* SkipWhitespace(const char* p);

static void WorkspaceTracker_Init(WorkspaceTracker* tracker)
{
    if (!tracker)
        return;
    ZeroMemory(tracker, sizeof(*tracker));
}

static void WorkspaceTracker_Free(WorkspaceTracker* tracker)
{
    if (!tracker)
        return;
    if (tracker->entries)
        free(tracker->entries);
    ZeroMemory(tracker, sizeof(*tracker));
}

static BOOL ShouldTrackWorkspaceChild(const WIN32_FIND_DATAW* pfd)
{
    const WCHAR* name;
    DWORD attrs;

    if (!pfd)
        return FALSE;
    name = pfd->cFileName;
    attrs = pfd->dwFileAttributes;
    if (!name[0])
        return FALSE;
    if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0)
        return FALSE;
    if (_wcsicmp(name, L".git") == 0 ||
        _wcsicmp(name, L".vs") == 0 ||
        _wcsicmp(name, L"node_modules") == 0 ||
        _wcsicmp(name, L"__pycache__") == 0)
        return FALSE;
    if (attrs & FILE_ATTRIBUTE_HIDDEN)
        return FALSE;
    if (attrs & FILE_ATTRIBUTE_SYSTEM)
        return FALSE;
    return TRUE;
}

static BOOL WorkspaceTracker_Add(WorkspaceTracker* tracker, LPCWSTR wszPath,
                                 DWORD attrs, const FILETIME* pWriteTime)
{
    WorkspaceEntry* grown;
    WorkspaceEntry* entry;
    int newCap;

    if (!tracker || !wszPath || !wszPath[0] || !pWriteTime)
        return FALSE;
    if (tracker->count >= tracker->cap)
    {
        newCap = tracker->cap > 0 ? tracker->cap * 2 : 128;
        grown = (WorkspaceEntry*)realloc(tracker->entries, sizeof(WorkspaceEntry) * newCap);
        if (!grown)
            return FALSE;
        tracker->entries = grown;
        tracker->cap = newCap;
    }

    entry = &tracker->entries[tracker->count++];
    lstrcpynW(entry->path, wszPath, ARRAYSIZE(entry->path));
    entry->writeTime = *pWriteTime;
    entry->attributes = attrs;
    return TRUE;
}

static void WorkspaceTracker_EnumerateRecursive(WorkspaceTracker* tracker, LPCWSTR wszDir)
{
    WCHAR wszPattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;

    if (!tracker || !wszDir || !wszDir[0])
        return;
    if (FAILED(StringCchPrintfW(wszPattern, ARRAYSIZE(wszPattern), L"%s\\*", wszDir)))
        return;

    hFind = FindFirstFileW(wszPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        WCHAR wszChild[MAX_PATH];
        if (!ShouldTrackWorkspaceChild(&fd))
            continue;
        if (FAILED(StringCchPrintfW(wszChild, ARRAYSIZE(wszChild), L"%s\\%s", wszDir, fd.cFileName)))
            continue;
        WorkspaceTracker_Add(tracker, wszChild, fd.dwFileAttributes, &fd.ftLastWriteTime);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            WorkspaceTracker_EnumerateRecursive(tracker, wszChild);
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static int __cdecl WorkspaceEntryCompare(const void* a, const void* b)
{
    const WorkspaceEntry* left = (const WorkspaceEntry*)a;
    const WorkspaceEntry* right = (const WorkspaceEntry*)b;
    return _wcsicmp(left->path, right->path);
}

static BOOL FileTimeIsNewer(const FILETIME* candidate, const FILETIME* current)
{
    if (!candidate)
        return FALSE;
    if (!current)
        return TRUE;
    return CompareFileTime(candidate, current) > 0;
}

static void SelectNewestPath(WCHAR* wszBest, int cchBest, FILETIME* pBestTime,
                             LPCWSTR wszPath, const FILETIME* pWriteTime)
{
    if (!wszBest || cchBest <= 0 || !pBestTime || !wszPath || !wszPath[0] || !pWriteTime)
        return;
    if (!wszBest[0] || FileTimeIsNewer(pWriteTime, pBestTime))
    {
        lstrcpynW(wszBest, wszPath, cchBest);
        *pBestTime = *pWriteTime;
    }
}

static BOOL WorkspaceTracker_BuildSnapshot(WorkspaceTracker* tracker, LPCWSTR wszRoot)
{
    DWORD attrs;

    if (!tracker || !wszRoot || !wszRoot[0])
        return FALSE;
    attrs = GetFileAttributesW(wszRoot);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return FALSE;

    WorkspaceTracker_EnumerateRecursive(tracker, wszRoot);
    if (tracker->count > 1)
        qsort(tracker->entries, tracker->count, sizeof(WorkspaceEntry), WorkspaceEntryCompare);
    return TRUE;
}

static void PostPathMessageToMain(HWND hwndTarget, UINT msg, LPCWSTR wszPath)
{
    HWND hwndMain;
    WCHAR* wszCopy;
    size_t cbBytes;

    if (!hwndTarget || !wszPath || !wszPath[0])
        return;
    hwndMain = GetParent(hwndTarget);
    if (!hwndMain)
        return;

    cbBytes = (size_t)(lstrlenW(wszPath) + 1) * sizeof(WCHAR);
    wszCopy = (WCHAR*)malloc(cbBytes);
    if (!wszCopy)
        return;
    memcpy(wszCopy, wszPath, cbBytes);
    PostMessage(hwndMain, msg, 0, (LPARAM)wszCopy);
}

static void WorkspaceTracker_Baseline(WorkspaceTracker* tracker, LPCWSTR wszRoot)
{
    WorkspaceTracker fresh;

    if (!tracker)
        return;
    WorkspaceTracker_Init(&fresh);
    if (!WorkspaceTracker_BuildSnapshot(&fresh, wszRoot))
    {
        WorkspaceTracker_Free(&fresh);
        return;
    }

    WorkspaceTracker_Free(tracker);
    *tracker = fresh;
    tracker->lastScanTick = GetTickCount64();
}

static void WorkspaceTracker_Poll(WorkspaceTracker* tracker, LPCWSTR wszRoot,
                                  HWND hwndTarget, BOOL bForce)
{
    WorkspaceTracker fresh;
    WCHAR wszLastOpened[MAX_PATH];
    WCHAR wszNewFile[MAX_PATH] = L"";
    WCHAR wszReloadFile[MAX_PATH] = L"";
    WCHAR wszChangedFile[MAX_PATH] = L"";
    WCHAR wszDir[MAX_PATH] = L"";
    FILETIME ftNewFile = {0};
    FILETIME ftReloadFile = {0};
    FILETIME ftChangedFile = {0};
    FILETIME ftDir = {0};
    ULONGLONG now;
    int i = 0;
    int j = 0;
    BOOL bAnyChange = FALSE;

    if (!tracker || !wszRoot || !wszRoot[0])
        return;
    now = GetTickCount64();
    if (!bForce && tracker->lastScanTick != 0 && (now - tracker->lastScanTick) < 500)
        return;

    WorkspaceTracker_Init(&fresh);
    if (!WorkspaceTracker_BuildSnapshot(&fresh, wszRoot))
    {
        WorkspaceTracker_Free(&fresh);
        tracker->lastScanTick = now;
        return;
    }

    lstrcpynW(wszLastOpened, tracker->lastOpenedFile, ARRAYSIZE(wszLastOpened));

    while (i < tracker->count || j < fresh.count)
    {
        int cmp;

        if (i >= tracker->count)
            cmp = 1;
        else if (j >= fresh.count)
            cmp = -1;
        else
            cmp = _wcsicmp(tracker->entries[i].path, fresh.entries[j].path);

        if (cmp < 0)
        {
            bAnyChange = TRUE;
            i++;
            continue;
        }
        if (cmp > 0)
        {
            bAnyChange = TRUE;
            if (fresh.entries[j].attributes & FILE_ATTRIBUTE_DIRECTORY)
                SelectNewestPath(wszDir, ARRAYSIZE(wszDir), &ftDir, fresh.entries[j].path, &fresh.entries[j].writeTime);
            else
                SelectNewestPath(wszNewFile, ARRAYSIZE(wszNewFile), &ftNewFile, fresh.entries[j].path, &fresh.entries[j].writeTime);
            j++;
            continue;
        }

        if (CompareFileTime(&tracker->entries[i].writeTime, &fresh.entries[j].writeTime) != 0)
        {
            bAnyChange = TRUE;
            if (!(fresh.entries[j].attributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                SelectNewestPath(wszChangedFile, ARRAYSIZE(wszChangedFile), &ftChangedFile,
                                 fresh.entries[j].path, &fresh.entries[j].writeTime);
                if (wszLastOpened[0] && _wcsicmp(wszLastOpened, fresh.entries[j].path) == 0)
                    SelectNewestPath(wszReloadFile, ARRAYSIZE(wszReloadFile), &ftReloadFile,
                                     fresh.entries[j].path, &fresh.entries[j].writeTime);
            }
        }

        i++;
        j++;
    }

    if (bAnyChange)
    {
        if (wszNewFile[0])
        {
            PostPathMessageToMain(hwndTarget, WM_AI_OPEN_FILE, wszNewFile);
            lstrcpynW(wszLastOpened, wszNewFile, ARRAYSIZE(wszLastOpened));
        }
        else if (wszReloadFile[0])
        {
            PostPathMessageToMain(hwndTarget, WM_AI_OPEN_FILE, wszReloadFile);
        }
        else if (!wszLastOpened[0] && wszChangedFile[0])
        {
            PostPathMessageToMain(hwndTarget, WM_AI_OPEN_FILE, wszChangedFile);
            lstrcpynW(wszLastOpened, wszChangedFile, ARRAYSIZE(wszLastOpened));
        }
        else if (wszDir[0])
        {
            PostPathMessageToMain(hwndTarget, WM_AI_REFRESH_PATH, wszDir);
        }
        else
        {
            PostPathMessageToMain(hwndTarget, WM_AI_REFRESH_PATH, wszRoot);
        }
    }

    WorkspaceTracker_Free(tracker);
    *tracker = fresh;
    tracker->lastScanTick = now;
    lstrcpynW(tracker->lastOpenedFile, wszLastOpened, ARRAYSIZE(tracker->lastOpenedFile));
}

static BOOL ModeUsesCodex(EAIChatAccessMode mode)
{
    return mode == AI_CHAT_ACCESS_CODEX || mode == AI_CHAT_ACCESS_CODEX_CLAUDE;
}

static BOOL ModeUsesClaude(EAIChatAccessMode mode)
{
    return mode == AI_CHAT_ACCESS_CLAUDE || mode == AI_CHAT_ACCESS_CODEX_CLAUDE;
}

static BOOL ModeIsCollaborative(EAIChatAccessMode mode)
{
    return mode == AI_CHAT_ACCESS_CODEX_CLAUDE;
}

static BOOL ModeIsSupported(EAIChatAccessMode mode)
{
    return ModeUsesCodex(mode) || ModeUsesClaude(mode);
}

static const char* GetAgentName(EAIChatAccessMode mode)
{
    switch (mode)
    {
    case AI_CHAT_ACCESS_CODEX:
        return "Codex (Embedded)";
    case AI_CHAT_ACCESS_CLAUDE:
        return "Claude Code (Embedded)";
    case AI_CHAT_ACCESS_CODEX_CLAUDE:
        return "Codex + Claude (Embedded)";
    default:
        return "Subscription agent";
    }
}

static const char* GetDrivingStatus(EAIChatAccessMode mode)
{
    if (mode == AI_CHAT_ACCESS_CODEX)
        return "Codex embedded session is driving the workspace...";
    if (mode == AI_CHAT_ACCESS_CLAUDE)
        return "Claude embedded session is driving the workspace...";
    return "Codex and Claude embedded sessions are coordinating the workspace...";
}

static BOOL IsErrorResult(const char* text)
{
    return text && strncmp(text, "Error:", 6) == 0;
}

static BOOL IsPlaceholderFinalText(const char* text)
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

static BOOL IsMissingFinalAnswerError(const char* text)
{
    if (!text)
        return FALSE;

    return strcmp(text, "Error: The embedded agent finished without returning a user-facing answer.") == 0 ||
        strcmp(text, "Error: The embedded agent finished without returning a usable final answer.") == 0;
}

static char* BuildRelayHandoffSummary(const char* codexResult)
{
    if (codexResult &&
        codexResult[0] &&
        strncmp(codexResult, "Error:", 6) != 0 &&
        !IsPlaceholderFinalText(codexResult))
    {
        return DupString(codexResult);
    }

    return DupString(
        "Codex completed an implementation pass in the shared workspace but did not leave a polished textual handoff. "
        "Inspect the files on disk, verify the actual edits, and write the final user-facing summary from that workspace state.");
}

static void sb_init(StrBuf* sb, int cap)
{
    if (!sb)
        return;
    sb->cap = cap > 128 ? cap : 128;
    sb->len = 0;
    sb->data = (char*)malloc(sb->cap);
    if (sb->data)
        sb->data[0] = '\0';
}

static void sb_append(StrBuf* sb, const char* text, int len)
{
    char* next;
    int need;
    if (!sb || !sb->data || !text)
        return;
    if (len < 0)
        len = (int)strlen(text);
    need = sb->len + len + 1;
    if (need > sb->cap)
    {
        int newCap = sb->cap * 2;
        if (newCap < need)
            newCap = need + 256;
        next = (char*)realloc(sb->data, newCap);
        if (!next)
            return;
        sb->data = next;
        sb->cap = newCap;
    }
    memcpy(sb->data + sb->len, text, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(StrBuf* sb, const char* fmt, ...)
{
    char buffer[2048];
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

static void CopyStringSafe(char* dst, size_t cchDst, const char* src)
{
    if (!dst || cchDst == 0)
        return;
    dst[0] = '\0';
    if (src)
        strncpy(dst, src, cchDst - 1);
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

static void PostHeapString(HWND hwndTarget, UINT msg, const char* text)
{
    char* copy;
    if (!hwndTarget || !text || !text[0])
        return;
    copy = DupString(text);
    if (copy)
        PostMessage(hwndTarget, msg, 0, (LPARAM)copy);
}

static void PostShortStatus(HWND hwndTarget, const char* text)
{
    char buffer[320];
    int i = 0;
    if (!text || !text[0])
        return;
    while (*text == ' ' || *text == '\n' || *text == '\r')
        text++;
    while (text[i] && text[i] != '\n' && text[i] != '\r' && i < (int)sizeof(buffer) - 1)
    {
        buffer[i] = text[i];
        i++;
    }
    buffer[i] = '\0';
    if (buffer[0])
        PostHeapString(hwndTarget, WM_AI_AGENT_STATUS, buffer);
}

static void ReplaceFinal(char** ppszFinal, const char* text)
{
    if (!ppszFinal || !text || !text[0])
        return;
    if (*ppszFinal)
        free(*ppszFinal);
    *ppszFinal = DupString(text);
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
    if (cchSuffix <= 0 || cchPath < cchSuffix)
        return FALSE;
    return _wcsicmp(wszPath + cchPath - cchSuffix, wszSuffix) == 0;
}

static BOOL ResolvePreferredCommandPath(const LPCWSTR* pCandidates, int cCandidates,
                                        WCHAR* wszOut, DWORD cchOut, BOOL* pbUseCmd)
{
    int i;

    if (!wszOut || cchOut == 0)
        return FALSE;
    wszOut[0] = L'\0';
    if (pbUseCmd)
        *pbUseCmd = FALSE;

    for (i = 0; i < cCandidates; i++)
    {
        if (ResolveCommandPath(pCandidates[i], wszOut, cchOut))
        {
            if (pbUseCmd)
                *pbUseCmd = PathEndsWithInsensitive(wszOut, L".cmd") ||
                            PathEndsWithInsensitive(wszOut, L".bat");
            return TRUE;
        }
    }

    return FALSE;
}

static BOOL ResolveExecutable(EAIChatAccessMode mode, WCHAR* wszOut, DWORD cchOut, BOOL* pbUseCmd)
{
    static const LPCWSTR kCodexCandidates[] = { L"codex.cmd", L"codex.exe", L"codex" };
    static const LPCWSTR kClaudeCandidates[] = { L"claude.cmd", L"claude.exe", L"claude" };

    if (mode == AI_CHAT_ACCESS_CODEX)
        return ResolvePreferredCommandPath(kCodexCandidates, ARRAYSIZE(kCodexCandidates),
                                           wszOut, cchOut, pbUseCmd);
    else if (mode == AI_CHAT_ACCESS_CLAUDE)
        return ResolvePreferredCommandPath(kClaudeCandidates, ARRAYSIZE(kClaudeCandidates),
                                           wszOut, cchOut, pbUseCmd);

    if (wszOut && cchOut > 0)
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

static BOOL GetUserProfilePath(WCHAR* wszOut, DWORD cchOut)
{
    DWORD dw;
    if (!wszOut || cchOut == 0)
        return FALSE;
    wszOut[0] = L'\0';
    dw = GetEnvironmentVariableW(L"USERPROFILE", wszOut, cchOut);
    return dw > 0 && dw < cchOut;
}

static BOOL BuildUserStatePath(LPCWSTR wszRelative, WCHAR* wszOut, DWORD cchOut)
{
    WCHAR wszHome[MAX_PATH];
    if (!wszRelative || !wszOut || cchOut == 0)
        return FALSE;
    if (!GetUserProfilePath(wszHome, ARRAYSIZE(wszHome)))
        return FALSE;
    return PathCombineW(wszOut, wszHome, wszRelative) != NULL;
}

static BOOL FileExistsAndHasData(LPCWSTR wszPath)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!wszPath || !wszPath[0])
        return FALSE;
    if (!GetFileAttributesExW(wszPath, GetFileExInfoStandard, &data))
        return FALSE;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return FALSE;
    return data.nFileSizeHigh != 0 || data.nFileSizeLow != 0;
}

static BOOL DeleteFileIfPresent(LPCWSTR wszPath)
{
    DWORD attrs;
    if (!wszPath || !wszPath[0])
        return FALSE;
    attrs = GetFileAttributesW(wszPath);
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        DWORD dwErr = GetLastError();
        return dwErr == ERROR_FILE_NOT_FOUND || dwErr == ERROR_PATH_NOT_FOUND;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        return FALSE;
    return DeleteFileW(wszPath);
}

static BOOL IsCodexAuthenticated(void)
{
    WCHAR wszPath[MAX_PATH];
    return BuildUserStatePath(L".codex\\auth.json", wszPath, ARRAYSIZE(wszPath)) &&
           FileExistsAndHasData(wszPath);
}

static BOOL IsClaudeAuthenticated(void)
{
    WCHAR wszPath[MAX_PATH];
    return BuildUserStatePath(L".claude\\.credentials.json", wszPath, ARRAYSIZE(wszPath)) &&
           FileExistsAndHasData(wszPath);
}

static void GetWorkspaceRoot(WCHAR* wszOut, int cchOut)
{
    if (!wszOut || cchOut <= 0)
        return;
    wszOut[0] = L'\0';

    if (szCurFile[0] && AIContext_GetProjectRoot(szCurFile, wszOut, cchOut))
        return;
    if (FileManager_GetRootPath()[0])
    {
        lstrcpynW(wszOut, FileManager_GetRootPath(), cchOut);
        return;
    }
    if (szCurFile[0])
    {
        lstrcpynW(wszOut, szCurFile, cchOut);
        PathRemoveFileSpecW(wszOut);
        return;
    }
    GetCurrentDirectoryW(cchOut, wszOut);
}

static BOOL PathIsSameOrChild(LPCWSTR wszRoot, LPCWSTR wszCandidate)
{
    int len;
    if (!wszRoot || !wszRoot[0] || !wszCandidate || !wszCandidate[0])
        return FALSE;
    len = lstrlenW(wszRoot);
    if (_wcsnicmp(wszRoot, wszCandidate, len) != 0)
        return FALSE;
    return wszCandidate[len] == L'\0' || wszCandidate[len] == L'\\' || wszCandidate[len] == L'/';
}

static void CollectExtraDirs(const AIChatAttachment* attachments, int count,
                             LPCWSTR wszWorkspaceRoot,
                             WCHAR wszDirs[AI_MAX_CHAT_ATTACHMENTS][MAX_PATH],
                             int* pnDirs)
{
    int i;
    int added = 0;
    if (!pnDirs)
        return;
    *pnDirs = 0;

    for (i = 0; i < count && i < AI_MAX_CHAT_ATTACHMENTS; i++)
    {
        WCHAR wszPath[MAX_PATH];
        WCHAR wszDir[MAX_PATH];
        int j;

        Utf8ToWide(attachments[i].path, wszPath, ARRAYSIZE(wszPath));
        if (!wszPath[0])
            continue;
        lstrcpynW(wszDir, wszPath, ARRAYSIZE(wszDir));
        PathRemoveFileSpecW(wszDir);
        if (!wszDir[0])
            continue;
        if (wszWorkspaceRoot[0] && PathIsSameOrChild(wszWorkspaceRoot, wszDir))
            continue;

        for (j = 0; j < added; j++)
        {
            if (_wcsicmp(wszDirs[j], wszDir) == 0)
                break;
        }
        if (j < added)
            continue;

        lstrcpynW(wszDirs[added], wszDir, MAX_PATH);
        added++;
    }

    *pnDirs = added;
}

static void AppendWorkspaceContext(StrBuf* sb, const SubscriptionJob* job, LPCWSTR wszWorkspaceRoot)
{
    char* workspace = WideToUtf8Dup(wszWorkspaceRoot);
    char* active = WideToUtf8Dup(szCurFile);
    int i;

    sb_appendf(sb, "Workspace root: %s\n", workspace && workspace[0] ? workspace : "(unknown)");
    sb_appendf(sb, "Active file: %s\n", active && active[0] ? active : "(none)");

    if (job->attachmentCount > 0)
    {
        sb_append(sb, "Attachment paths:\n", -1);
        for (i = 0; i < job->attachmentCount; i++)
            sb_appendf(sb, "- %s\n", job->attachments[i].path);
    }

    sb_append(sb, "\nUser request:\n", -1);
    sb_append(sb, job->message ? job->message : "", -1);
    sb_append(sb, "\n", 1);

    if (workspace)
        free(workspace);
    if (active)
        free(active);
}

static const char* GetModeNameForPrompt(EAIChatAccessMode mode)
{
    switch (mode)
    {
    case AI_CHAT_ACCESS_CODEX:
        return "Codex embedded chat";
    case AI_CHAT_ACCESS_CLAUDE:
        return "Claude embedded chat";
    case AI_CHAT_ACCESS_CODEX_CLAUDE:
        return "Codex + Claude embedded relay";
    default:
        return "Built-in provider chat";
    }
}

static void AppendEmbeddedHouseRules(StrBuf* sb)
{
    sb_append(sb,
        "Bikode house rules:\n"
        "- Bikode is an AI-first IDE. Its motto is \"I write what I like.\"\n"
        "- Strengthen the user's agency, preserve their voice, and keep the machine in service of the author's intent.\n"
        "- Be concise, practical, and reality-based.\n"
        "- Use calm confidence and avoid sycophantic language.\n"
        "- Prefer direct workspace edits over long pasted code blocks.\n"
        "- Read current files before changing them and keep changes reviewable.\n"
        "- You have direct access to the current workspace. Do not claim you lack filesystem access when Bikode already gave you a workspace root.\n"
        "- Only use a what-changed / what-checked / remaining-risk wrap-up after you actually inspected files, ran checks, or changed the workspace.\n"
        "- In prose, avoid em dashes when a plain alternative works.\n", -1);
}

static void AppendModeMetadata(StrBuf* sb, const SubscriptionJob* job, EAIChatAccessMode agentMode)
{
    char* model = NULL;

    sb_appendf(sb, "Bikode chat mode: %s\n", GetModeNameForPrompt(job->config.eChatAccessMode));
    sb_appendf(sb, "Active embedded agent for this pass: %s\n", GetAgentName(agentMode));
    if (job->config.wszChatDriverModel[0])
    {
        model = WideToUtf8Dup(job->config.wszChatDriverModel);
        if (model && model[0])
            sb_appendf(sb, "Requested chat model: %s\n", model);
    }

    if (ModeIsCollaborative(job->config.eChatAccessMode))
    {
        if (agentMode == AI_CHAT_ACCESS_CODEX)
        {
            sb_append(sb,
                "Relay topology: Codex is the implementation lead for this pass; Claude Code will review the same workspace after your handoff.\n"
                "Second embedded agent configured for this request: Claude Code.\n", -1);
        }
        else
        {
            sb_append(sb,
                "Relay topology: Claude Code is the review and finalization pass after Codex worked in this same workspace.\n"
                "Second embedded agent configured for this request: Codex. Use the handoff summary below as context, then verify against the files on disk.\n", -1);
        }
    }
    else
    {
        sb_append(sb,
            "No second embedded agent is participating in this request. Only the selected Bikode chat mode is active here.\n", -1);
    }

    if (model)
        free(model);
}

static char* BuildPrompt(const SubscriptionJob* job, LPCWSTR wszWorkspaceRoot,
                         EAIChatAccessMode agentMode,
                         const char* szPriorAgentResult)
{
    StrBuf sb;

    sb_init(&sb, 6144);
    sb_append(&sb, "SYSTEM PROMPT -- Bikode Embedded Agent\n\n", -1);
    sb_appendf(&sb, "You are %s operating inside Bikode's embedded chat panel.\n", GetAgentName(agentMode));
    sb_append(&sb, "The workspace runs on Windows and the on-disk workspace is the source of truth.\n", -1);
    sb_append(&sb, "The current working directory is already the workspace root, so inspect files on disk directly instead of asking the user to paste them back to you.\n", -1);
    sb_append(&sb, "Prefer making file changes directly in the workspace over dumping long code blocks into chat.\n\n", -1);

    AppendEmbeddedHouseRules(&sb);
    sb_append(&sb, "\nMode metadata:\n", -1);
    AppendModeMetadata(&sb, job, agentMode);

    if (ModeIsCollaborative(job->config.eChatAccessMode))
    {
        sb_append(&sb, "\nRelay instructions:\n", -1);
        if (agentMode == AI_CHAT_ACCESS_CODEX)
        {
            sb_append(&sb,
                "- You are the implementation lead for the first pass.\n"
                "- Make the workspace changes directly, keep them reviewable, and avoid unnecessary churn.\n"
                "- End with a short handoff for Claude Code listing changed files, checks run, and remaining risks.\n", -1);
        }
        else
        {
            sb_append(&sb,
                "- Codex has already worked in this same workspace. Review the current files, improve them if needed, and finalize the result.\n"
                "- Keep good changes, tighten correctness, and call out any residual risk.\n"
                "- End with a concise final summary for the user covering what changed, what was checked, and any remaining caveats.\n", -1);
        }
    }
    else
    {
        sb_append(&sb, "\nExecution instructions:\n", -1);
        sb_appendf(&sb, "- Act like %s driving the workspace when code or files need to change.\n",
                   GetAgentName(agentMode));
        sb_append(&sb,
            "- Answer the user directly. Only add a what-changed / what-checked / remaining-risk wrap-up when you actually inspected files, ran commands, or changed the workspace.\n",
            -1);
    }

    sb_append(&sb, "\nWorkspace context:\n", -1);
    AppendWorkspaceContext(&sb, job, wszWorkspaceRoot);

    if (szPriorAgentResult && szPriorAgentResult[0])
    {
        sb_appendf(&sb, "\n%s handoff summary:\n", GetAgentName(AI_CHAT_ACCESS_CODEX));
        sb_append(&sb, szPriorAgentResult, -1);
        sb_append(&sb, "\n", 1);
    }

    return sb.data;
}

static void AppendQuoted(WCHAR* wszCmd, size_t cchCmd, LPCWSTR wszValue)
{
    StringCchCatW(wszCmd, cchCmd, L" \"");
    StringCchCatW(wszCmd, cchCmd, wszValue);
    StringCchCatW(wszCmd, cchCmd, L"\"");
}

static void BuildArguments(const SubscriptionJob* job, EAIChatAccessMode agentMode,
                           LPCWSTR wszWorkspaceRoot,
                           const WCHAR wszExtraDirs[AI_MAX_CHAT_ATTACHMENTS][MAX_PATH],
                           int cExtraDirs,
                           WCHAR* wszArgs, size_t cchArgs)
{
    int i;
    wszArgs[0] = L'\0';

    if (agentMode == AI_CHAT_ACCESS_CODEX)
    {
        BOOL bResumeCodex = s_codexSession[0] != '\0';
        if (bResumeCodex)
        {
            StringCchCatW(wszArgs, cchArgs, L" exec resume");
            StringCchCatW(wszArgs, cchArgs, L" --json --dangerously-bypass-approvals-and-sandbox --skip-git-repo-check");
            if (job->config.wszChatDriverModel[0])
            {
                StringCchCatW(wszArgs, cchArgs, L" -m");
                AppendQuoted(wszArgs, cchArgs, job->config.wszChatDriverModel);
            }

            {
                WCHAR wszSession[128];
                Utf8ToWide(s_codexSession, wszSession, ARRAYSIZE(wszSession));
                AppendQuoted(wszArgs, cchArgs, wszSession);
            }
        }
        else
        {
            StringCchCatW(wszArgs, cchArgs, L" exec");
            StringCchCatW(wszArgs, cchArgs, L" --json --dangerously-bypass-approvals-and-sandbox --skip-git-repo-check");
            if (wszWorkspaceRoot && wszWorkspaceRoot[0])
            {
                StringCchCatW(wszArgs, cchArgs, L" -C");
                AppendQuoted(wszArgs, cchArgs, wszWorkspaceRoot);
            }
            if (job->config.wszChatDriverModel[0])
            {
                StringCchCatW(wszArgs, cchArgs, L" -m");
                AppendQuoted(wszArgs, cchArgs, job->config.wszChatDriverModel);
            }
        }
    }
    else
    {
        StringCchCatW(wszArgs, cchArgs, L" -p --output-format stream-json --verbose --permission-mode bypassPermissions");
        if (s_claudeSession[0])
        {
            WCHAR wszSession[128];
            Utf8ToWide(s_claudeSession, wszSession, ARRAYSIZE(wszSession));
            StringCchCatW(wszArgs, cchArgs, L" -r");
            AppendQuoted(wszArgs, cchArgs, wszSession);
        }
        if (job->config.wszChatDriverModel[0])
        {
            StringCchCatW(wszArgs, cchArgs, L" --model");
            AppendQuoted(wszArgs, cchArgs, job->config.wszChatDriverModel);
        }
    }

    if (!(agentMode == AI_CHAT_ACCESS_CODEX && s_codexSession[0]))
    {
        for (i = 0; i < cExtraDirs; i++)
        {
            StringCchCatW(wszArgs, cchArgs, L" --add-dir");
            AppendQuoted(wszArgs, cchArgs, wszExtraDirs[i]);
        }
    }
}

static BOOL BuildCommandLine(LPCWSTR wszExecutable, LPCWSTR wszArgs, BOOL bUseCmd, WCHAR* wszCmd, size_t cchCmd)
{
    WCHAR wszCmdExe[MAX_PATH];
    if (!wszExecutable || !wszExecutable[0] || !wszCmd || cchCmd == 0)
        return FALSE;
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
    StringCchCatW(wszCmd, cchCmd, L"\"");
    StringCchCatW(wszCmd, cchCmd, L" /d /s /c \"\"");
    StringCchCatW(wszCmd, cchCmd, wszExecutable);
    StringCchCatW(wszCmd, cchCmd, L"\"");
    if (wszArgs && wszArgs[0])
        StringCchCatW(wszCmd, cchCmd, wszArgs);
    StringCchCatW(wszCmd, cchCmd, L"\"");
    return TRUE;
}

static BOOL RunResolvedCommand(EAIChatAccessMode eMode, LPCWSTR wszArgs, DWORD* pdwExitCode)
{
    WCHAR wszExe[MAX_PATH];
    WCHAR wszCmd[8192];
    BOOL bUseCmd = FALSE;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = (DWORD)-1;

    if (pdwExitCode)
        *pdwExitCode = (DWORD)-1;
    if (!ResolveExecutable(eMode, wszExe, ARRAYSIZE(wszExe), &bUseCmd))
        return FALSE;
    if (!BuildCommandLine(wszExe, wszArgs, bUseCmd, wszCmd, ARRAYSIZE(wszCmd)))
        return FALSE;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, wszCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (pdwExitCode)
        *pdwExitCode = exitCode;
    return exitCode == 0;
}

static BOOL LogoutCodex(void)
{
    WCHAR wszAuthPath[MAX_PATH];
    DWORD exitCode = 0;

    s_codexSession[0] = '\0';
    if (!IsCodexAuthenticated())
        return TRUE;
    if (RunResolvedCommand(AI_CHAT_ACCESS_CODEX, L" logout", &exitCode))
        return TRUE;
    if (!BuildUserStatePath(L".codex\\auth.json", wszAuthPath, ARRAYSIZE(wszAuthPath)))
        return FALSE;
    return DeleteFileIfPresent(wszAuthPath);
}

static BOOL LogoutClaude(void)
{
    WCHAR wszCredPath[MAX_PATH];

    s_claudeSession[0] = '\0';
    if (!IsClaudeAuthenticated())
        return TRUE;
    if (!BuildUserStatePath(L".claude\\.credentials.json", wszCredPath, ARRAYSIZE(wszCredPath)))
        return FALSE;
    return DeleteFileIfPresent(wszCredPath);
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

static void RememberSession(EAIChatAccessMode mode, const char* session)
{
    if (!session || !session[0])
        return;
    if (mode == AI_CHAT_ACCESS_CODEX)
        CopyStringSafe(s_codexSession, sizeof(s_codexSession), session);
    else if (mode == AI_CHAT_ACCESS_CLAUDE)
        CopyStringSafe(s_claudeSession, sizeof(s_claudeSession), session);
}

static char* BuildLaunchErrorResult(DWORD errorCode)
{
    WCHAR wszMessage[512];
    DWORD cchMessage;
    char* utf8Message;
    char buffer[768];

    wszMessage[0] = L'\0';
    cchMessage = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                NULL, errorCode, 0, wszMessage, ARRAYSIZE(wszMessage), NULL);
    while (cchMessage > 0 &&
           (wszMessage[cchMessage - 1] == L'\r' ||
            wszMessage[cchMessage - 1] == L'\n' ||
            wszMessage[cchMessage - 1] == L' ' ||
            wszMessage[cchMessage - 1] == L'\t'))
    {
        wszMessage[cchMessage - 1] = L'\0';
        cchMessage--;
    }

    utf8Message = cchMessage > 0 ? WideToUtf8Dup(wszMessage) : NULL;
    if (utf8Message && utf8Message[0])
    {
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                    "Error: Failed to launch the selected subscription agent (Windows error %lu: %s).",
                    (unsigned long)errorCode, utf8Message);
    }
    else
    {
        _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                    "Error: Failed to launch the selected subscription agent (Windows error %lu).",
                    (unsigned long)errorCode);
    }

    if (utf8Message)
        free(utf8Message);
    return DupString(buffer);
}

static void HandleCodexLine(HWND hwndTarget, const char* line, char** ppszFinal)
{
    char* value;
    if (!line || line[0] != '{')
        return;

    if (strstr(line, "\"type\":\"thread.started\""))
    {
        value = JsonExtractString(line, "thread_id");
        if (value)
        {
            RememberSession(AI_CHAT_ACCESS_CODEX, value);
            free(value);
        }
        return;
    }

    if (strstr(line, "\"type\":\"reasoning\""))
    {
        value = JsonExtractString(line, "text");
        if (value)
        {
            PostShortStatus(hwndTarget, value);
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
            PostHeapString(hwndTarget, WM_AI_AGENT_TOOL, normalized ? normalized : value);
            if (normalized)
                free(normalized);
            free(value);
        }
        return;
    }

    if (strstr(line, "\"type\":\"agent_message\""))
    {
        value = JsonExtractString(line, "text");
        if (value)
        {
            ReplaceFinal(ppszFinal, value);
            free(value);
        }
        return;
    }
}

static void HandleClaudeLine(HWND hwndTarget, const char* line, char** ppszFinal)
{
    char* value;
    char* tool;
    char* path;
    char* command;
    char buffer[512];

    if (!line || line[0] != '{')
        return;

    if (strstr(line, "\"type\":\"system\"") && strstr(line, "\"subtype\":\"init\""))
    {
        value = JsonExtractString(line, "session_id");
        if (value)
        {
            RememberSession(AI_CHAT_ACCESS_CLAUDE, value);
            free(value);
        }
        return;
    }

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
            PostHeapString(hwndTarget, WM_AI_AGENT_TOOL, buffer);
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
            ReplaceFinal(ppszFinal, value);
            free(value);
        }
        return;
    }

    if (strstr(line, "\"type\":\"result\""))
    {
        value = JsonExtractString(line, "session_id");
        if (value)
        {
            RememberSession(AI_CHAT_ACCESS_CLAUDE, value);
            free(value);
        }
        value = JsonExtractString(line, "result");
        if (value)
        {
            ReplaceFinal(ppszFinal, value);
            free(value);
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

static char* RunAgent(const SubscriptionJob* job, EAIChatAccessMode agentMode,
                      LPCWSTR wszWorkspaceRoot,
                      const WCHAR wszExtraDirs[AI_MAX_CHAT_ATTACHMENTS][MAX_PATH], int cExtraDirs,
                      const char* prompt)
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
    StrBuf line;
    StrBuf full;
    char* final = NULL;
    DWORD exitCode = 0;
    WorkspaceTracker tracker;
    DWORD cbAvailable = 0;
    DWORD waitResult = WAIT_TIMEOUT;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    WorkspaceTracker_Init(&tracker);

    if (!ResolveExecutable(agentMode, wszExe, ARRAYSIZE(wszExe), &bUseCmd))
    {
        return DupString(agentMode == AI_CHAT_ACCESS_CODEX
            ? "Error: Codex CLI was not found on PATH."
            : "Error: Claude Code CLI was not found on PATH.");
    }

    BuildArguments(job, agentMode, wszWorkspaceRoot, wszExtraDirs, cExtraDirs, wszArgs, ARRAYSIZE(wszArgs));
    if (!BuildCommandLine(wszExe, wszArgs, bUseCmd, wszCmd, ARRAYSIZE(wszCmd)))
        return DupString("Error: Failed to prepare the agent command line.");

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0))
        return DupString("Error: Failed to create the agent output pipe.");
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&hStdInRead, &hStdInWrite, &sa, 0))
        goto cleanup;
    SetHandleInformation(hStdInWrite, HANDLE_FLAG_INHERIT, 0);

    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdInRead;
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdOutWrite;
    PostShortStatus(job->hwndTarget, GetDrivingStatus(agentMode));

    if (!CreateProcessW(NULL, wszCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                        wszWorkspaceRoot && wszWorkspaceRoot[0] ? wszWorkspaceRoot : NULL,
                        &si, &pi))
    {
        final = BuildLaunchErrorResult(GetLastError());
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
    sb_init(&full, 4096);
    WorkspaceTracker_Baseline(&tracker, wszWorkspaceRoot);

    for (;;)
    {
        if (!PeekNamedPipe(hStdOutRead, NULL, 0, NULL, &cbAvailable, NULL))
            break;

        if (cbAvailable == 0)
        {
            WorkspaceTracker_Poll(&tracker, wszWorkspaceRoot, job->hwndTarget, FALSE);
            waitResult = WaitForSingleObject(pi.hProcess, 150);
            if (waitResult == WAIT_TIMEOUT)
                continue;
            if (!PeekNamedPipe(hStdOutRead, NULL, 0, NULL, &cbAvailable, NULL) || cbAvailable == 0)
                break;
        }

        if (!ReadFile(hStdOutRead, chunk, min((DWORD)(sizeof(chunk) - 1), cbAvailable), &cbRead, NULL) || cbRead == 0)
            break;

        {
            DWORD i;
            chunk[cbRead] = '\0';
            sb_append(&full, chunk, (int)cbRead);

            for (i = 0; i < cbRead; i++)
            {
                char ch = chunk[i];
                if (ch == '\r')
                    continue;
                if (ch == '\n')
                {
                    if (line.len > 0)
                    {
                        if (agentMode == AI_CHAT_ACCESS_CODEX)
                            HandleCodexLine(job->hwndTarget, line.data, &final);
                        else
                            HandleClaudeLine(job->hwndTarget, line.data, &final);
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

        WorkspaceTracker_Poll(&tracker, wszWorkspaceRoot, job->hwndTarget, FALSE);
    }

    WorkspaceTracker_Poll(&tracker, wszWorkspaceRoot, job->hwndTarget, TRUE);
    if (line.len > 0)
    {
        if (agentMode == AI_CHAT_ACCESS_CODEX)
            HandleCodexLine(job->hwndTarget, line.data, &final);
        else
            HandleClaudeLine(job->hwndTarget, line.data, &final);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);

    if (!final)
    {
        if (full.data && full.data[0])
            final = DupString(full.data);
        else
            final = DupString(exitCode == 0
                ? "Error: The embedded agent finished without returning a user-facing answer."
                : "Error: The subscription agent did not return a response.");
    }
    else if (exitCode != 0 && strncmp(final, "Error:", 6) != 0)
    {
        char errorBuf[512];
        _snprintf_s(errorBuf, sizeof(errorBuf), _TRUNCATE, "Error: Agent exited with code %lu.\n%s",
                    (unsigned long)exitCode, final);
        free(final);
        final = DupString(errorBuf);
    }
    else if (exitCode == 0 && IsPlaceholderFinalText(final))
    {
        free(final);
        final = DupString("Error: The embedded agent finished without returning a usable final answer.");
    }

    sb_free(&line);
    sb_free(&full);

cleanup:
    WorkspaceTracker_Free(&tracker);
    if (hStdOutRead) CloseHandle(hStdOutRead);
    if (hStdOutWrite) CloseHandle(hStdOutWrite);
    if (hStdInRead) CloseHandle(hStdInRead);
    if (hStdInWrite) CloseHandle(hStdInWrite);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    return final ? final : DupString("Error: The subscription agent failed to start.");
}

static char* BuildRelayFallbackResult(const char* codexResult, const char* claudeError)
{
    StrBuf sb;
    sb_init(&sb, 1024);
    sb_append(&sb,
        "Codex completed the shared-workspace pass, but Claude Code did not finish the review pass.\n\n"
        "Codex summary:\n", -1);
    sb_append(&sb, codexResult ? codexResult : "(no summary)", -1);
    if (claudeError && claudeError[0])
    {
        sb_append(&sb, "\n\nClaude Code issue:\n", -1);
        sb_append(&sb, claudeError, -1);
    }
    return sb.data;
}

static char* RunCollaborativeFlow(const SubscriptionJob* job, LPCWSTR wszWorkspaceRoot,
                                  const WCHAR wszExtraDirs[AI_MAX_CHAT_ATTACHMENTS][MAX_PATH],
                                  int cExtraDirs)
{
    char* codexPrompt = NULL;
    char* codexResult = NULL;
    char* codexHandoff = NULL;
    char* claudePrompt = NULL;
    char* final = NULL;

    PostShortStatus(job->hwndTarget, "Shared workspace relay starting...");
    PostHeapString(job->hwndTarget, WM_AI_AGENT_TOOL, "Shared workspace relay: Codex implementation pass");

    codexPrompt = BuildPrompt(job, wszWorkspaceRoot, AI_CHAT_ACCESS_CODEX, NULL);
    codexResult = RunAgent(job, AI_CHAT_ACCESS_CODEX, wszWorkspaceRoot, wszExtraDirs, cExtraDirs, codexPrompt);
    if (!codexResult)
        codexResult = DupString("Error: Codex did not return a response.");
    if (IsErrorResult(codexResult) && !IsMissingFinalAnswerError(codexResult))
    {
        final = codexResult;
        codexResult = NULL;
        goto cleanup;
    }
    codexHandoff = BuildRelayHandoffSummary(codexResult);

    PostShortStatus(job->hwndTarget, "Codex handoff ready. Claude Code is reviewing the shared workspace...");
    PostHeapString(job->hwndTarget, WM_AI_AGENT_TOOL, "Shared workspace relay: Claude review and refinement pass");

    claudePrompt = BuildPrompt(job, wszWorkspaceRoot, AI_CHAT_ACCESS_CLAUDE, codexHandoff ? codexHandoff : codexResult);
    final = RunAgent(job, AI_CHAT_ACCESS_CLAUDE, wszWorkspaceRoot, wszExtraDirs, cExtraDirs, claudePrompt);
    if (!final)
    {
        final = BuildRelayFallbackResult(codexHandoff ? codexHandoff : codexResult, "Error: Claude Code did not return a response.");
    }
    else if (IsErrorResult(final))
    {
        char* relayFallback = BuildRelayFallbackResult(codexHandoff ? codexHandoff : codexResult, final);
        free(final);
        final = relayFallback;
    }

cleanup:
    if (codexPrompt)
        free(codexPrompt);
    if (codexResult)
        free(codexResult);
    if (codexHandoff)
        free(codexHandoff);
    if (claudePrompt)
        free(claudePrompt);
    return final ? final : DupString("Error: Failed to coordinate the shared workspace relay.");
}

static unsigned __stdcall SubscriptionThreadProc(void* pParam)
{
    SubscriptionJob* job = (SubscriptionJob*)pParam;
    WCHAR wszWorkspaceRoot[MAX_PATH];
    WCHAR wszExtraDirs[AI_MAX_CHAT_ATTACHMENTS][MAX_PATH];
    int cExtraDirs = 0;
    char* prompt = NULL;
    char* final = NULL;

    if (!job)
        return 0;

    GetWorkspaceRoot(wszWorkspaceRoot, ARRAYSIZE(wszWorkspaceRoot));
    CollectExtraDirs(job->attachments, job->attachmentCount, wszWorkspaceRoot, wszExtraDirs, &cExtraDirs);
    if (ModeIsCollaborative(job->config.eChatAccessMode))
    {
        final = RunCollaborativeFlow(job, wszWorkspaceRoot, wszExtraDirs, cExtraDirs);
    }
    else
    {
        prompt = BuildPrompt(job, wszWorkspaceRoot, job->config.eChatAccessMode, NULL);
        final = RunAgent(job, job->config.eChatAccessMode, wszWorkspaceRoot, wszExtraDirs, cExtraDirs, prompt);
    }

    if (final)
        PostMessage(job->hwndTarget, WM_AI_DIRECT_RESPONSE, 0, (LPARAM)final);
    if (prompt)
        free(prompt);
    if (job->message)
        free(job->message);
    free(job);
    InterlockedExchange(&s_busy, FALSE);
    return 0;
}

BOOL AISubscriptionAgent_ChatAsync(const AIConfig* pConfig,
                                   const char* szUserMessage,
                                   const AIChatAttachment* pAttachments,
                                   int cAttachments,
                                   HWND hwndTarget)
{
    SubscriptionJob* job;
    uintptr_t hThread;
    int copyCount;

    if (!pConfig || !szUserMessage || !szUserMessage[0] || !hwndTarget)
        return FALSE;
    if (!ModeIsSupported(pConfig->eChatAccessMode))
        return FALSE;
    if (InterlockedCompareExchange(&s_busy, TRUE, FALSE) != FALSE)
        return FALSE;

    job = (SubscriptionJob*)calloc(1, sizeof(SubscriptionJob));
    if (!job)
    {
        InterlockedExchange(&s_busy, FALSE);
        return FALSE;
    }

    memcpy(&job->config, pConfig, sizeof(AIConfig));
    job->message = DupString(szUserMessage);
    job->hwndTarget = hwndTarget;
    copyCount = cAttachments > AI_MAX_CHAT_ATTACHMENTS ? AI_MAX_CHAT_ATTACHMENTS : cAttachments;
    if (copyCount > 0 && pAttachments)
    {
        memcpy(job->attachments, pAttachments, sizeof(AIChatAttachment) * copyCount);
        job->attachmentCount = copyCount;
    }

    hThread = _beginthreadex(NULL, 0, SubscriptionThreadProc, job, 0, NULL);
    if (!hThread)
    {
        if (job->message)
            free(job->message);
        free(job);
        InterlockedExchange(&s_busy, FALSE);
        return FALSE;
    }

    CloseHandle((HANDLE)hThread);
    return TRUE;
}

BOOL AISubscriptionAgent_IsBusy(void)
{
    return InterlockedCompareExchange(&s_busy, FALSE, FALSE) != FALSE;
}

void AISubscriptionAgent_ResetSessions(void)
{
    s_codexSession[0] = '\0';
    s_claudeSession[0] = '\0';
}

static LPCWSTR GetLoginArguments(EAIChatAccessMode eMode)
{
    if (eMode == AI_CHAT_ACCESS_CODEX)
        return L" login";
    if (eMode == AI_CHAT_ACCESS_CLAUDE)
        return L" setup-token";
    return NULL;
}

static BOOL OpenResolvedLoginFlow(EAIChatAccessMode eMode, HWND hwndOwner)
{
    WCHAR wszExe[MAX_PATH];
    WCHAR wszParams[4096];
    BOOL bUseCmd = FALSE;
    LPCWSTR wszArgs = GetLoginArguments(eMode);
    HINSTANCE hRes;

    if (!wszArgs)
        return FALSE;
    if (!ResolveExecutable(eMode, wszExe, ARRAYSIZE(wszExe), &bUseCmd))
        return FALSE;

    UNREFERENCED_PARAMETER(bUseCmd);
    wszParams[0] = L'\0';
    StringCchCatW(wszParams, ARRAYSIZE(wszParams), L"/k \"\"");
    StringCchCatW(wszParams, ARRAYSIZE(wszParams), wszExe);
    StringCchCatW(wszParams, ARRAYSIZE(wszParams), L"\"");
    StringCchCatW(wszParams, ARRAYSIZE(wszParams), wszArgs);
    StringCchCatW(wszParams, ARRAYSIZE(wszParams), L"\"");

    hRes = ShellExecuteW(hwndOwner, L"open", L"cmd.exe", wszParams, NULL, SW_SHOWNORMAL);
    return ((INT_PTR)hRes) > 32;
}

BOOL AISubscriptionAgent_IsAuthenticated(EAIChatAccessMode eMode)
{
    if (eMode == AI_CHAT_ACCESS_CODEX)
        return IsCodexAuthenticated();
    if (eMode == AI_CHAT_ACCESS_CLAUDE)
        return IsClaudeAuthenticated();
    if (eMode == AI_CHAT_ACCESS_CODEX_CLAUDE)
        return IsCodexAuthenticated() && IsClaudeAuthenticated();
    return FALSE;
}

BOOL AISubscriptionAgent_Logout(EAIChatAccessMode eMode)
{
    if (eMode == AI_CHAT_ACCESS_CODEX)
        return LogoutCodex();
    if (eMode == AI_CHAT_ACCESS_CLAUDE)
        return LogoutClaude();
    if (eMode == AI_CHAT_ACCESS_CODEX_CLAUDE)
    {
        BOOL bCodex = LogoutCodex();
        BOOL bClaude = LogoutClaude();
        return bCodex && bClaude;
    }
    return FALSE;
}

BOOL AISubscriptionAgent_OpenLoginFlow(EAIChatAccessMode eMode, HWND hwndOwner)
{
    if (eMode == AI_CHAT_ACCESS_CODEX)
        return IsCodexAuthenticated() ? TRUE : OpenResolvedLoginFlow(AI_CHAT_ACCESS_CODEX, hwndOwner);
    if (eMode == AI_CHAT_ACCESS_CLAUDE)
        return IsClaudeAuthenticated() ? TRUE : OpenResolvedLoginFlow(AI_CHAT_ACCESS_CLAUDE, hwndOwner);
    if (eMode == AI_CHAT_ACCESS_CODEX_CLAUDE)
    {
        BOOL bCodex = TRUE;
        BOOL bClaude = TRUE;
        if (!IsCodexAuthenticated())
            bCodex = OpenResolvedLoginFlow(AI_CHAT_ACCESS_CODEX, hwndOwner);
        if (!IsClaudeAuthenticated())
            bClaude = OpenResolvedLoginFlow(AI_CHAT_ACCESS_CLAUDE, hwndOwner);
        return bCodex && bClaude;
    }
    return FALSE;
}

void AISubscriptionAgent_GetModeDisplayName(EAIChatAccessMode eMode, WCHAR* wszOut, int cchOut)
{
    if (!wszOut || cchOut <= 0)
        return;
    wszOut[0] = L'\0';

    switch (eMode)
    {
    case AI_CHAT_ACCESS_CODEX:
        lstrcpynW(wszOut, L"Codex Embedded", cchOut);
        break;
    case AI_CHAT_ACCESS_CLAUDE:
        lstrcpynW(wszOut, L"Claude Embedded", cchOut);
        break;
    case AI_CHAT_ACCESS_CODEX_CLAUDE:
        lstrcpynW(wszOut, L"Codex + Claude Embedded", cchOut);
        break;
    case AI_CHAT_ACCESS_API_PROVIDER:
    default:
        lstrcpynW(wszOut, L"Built-in AI", cchOut);
        break;
    }
}
