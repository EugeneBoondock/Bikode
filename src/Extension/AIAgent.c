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
#include "AIDirectCall.h"
#include "AIBridge.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>

//=============================================================================
// Constants
//=============================================================================

#define MAX_HISTORY_TURNS       20
#define MAX_HISTORY_MESSAGES    (MAX_HISTORY_TURNS * 2)
#define MAX_AGENT_ITERATIONS    15
#define MAX_MESSAGES_PER_CALL   128
#define MAX_FILE_READ_SIZE      (50 * 1024)
#define MAX_CMD_OUTPUT_SIZE     (20 * 1024)
#define CMD_TIMEOUT_MS          30000

//=============================================================================
// Growable string buffer (local copy)
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

//=============================================================================
// Minimal JSON parsing (for tool call extraction)
//=============================================================================

static const char* json_find_value_a(const char* json, const char* key)
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

//=============================================================================
// System prompt
//=============================================================================

static char* BuildSystemPrompt(void)
{
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);

    StrBuf sb;
    sb_init(&sb, 4096);

    sb_append(&sb,
        "You are Biko, an AI coding assistant built into a lightweight code editor. "
        "You have access to tools that let you read files, write code, execute commands, "
        "and explore the filesystem.\n\n"
        "## Available Tools\n\n"
        "To use a tool, include a tool call block in your response:\n\n"
        "<tool_call>\n"
        "{\"name\": \"tool_name\", \"param1\": \"value1\"}\n"
        "</tool_call>\n\n"
        "You can include multiple tool calls in a single response. "
        "After tools execute, you'll receive their results and can continue.\n\n"
        "### read_file\n"
        "Read the contents of a file.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"read_file\", \"path\": \"src/main.c\"}\n\n"
        "### write_file\n"
        "Create or overwrite a file with the given content.\n"
        "Parameters: name, path, content\n"
        "Example: {\"name\": \"write_file\", \"path\": \"hello.c\", "
        "\"content\": \"#include <stdio.h>\\nint main() { return 0; }\"}\n\n"
        "### replace_in_file\n"
        "Find and replace text in an existing file. Replaces the first occurrence.\n"
        "Parameters: name, path, old_text, new_text\n"
        "Example: {\"name\": \"replace_in_file\", \"path\": \"main.c\", "
        "\"old_text\": \"return 0;\", \"new_text\": \"return EXIT_SUCCESS;\"}\n\n"
        "### run_command\n"
        "Execute a shell command and return its output.\n"
        "Parameters: name, command\n"
        "Example: {\"name\": \"run_command\", \"command\": \"dir /b src\"}\n\n"
        "### list_dir\n"
        "List the contents of a directory.\n"
        "Parameters: name, path\n"
        "Example: {\"name\": \"list_dir\", \"path\": \"src\"}\n\n"
        "## Guidelines\n"
        "- Read files before modifying them to understand their current content.\n"
        "- Use replace_in_file for targeted edits; use write_file for creating new files.\n"
        "- Include enough context in old_text to uniquely identify the replacement location.\n"
        "- All JSON strings must use proper escaping (\\n for newlines, \\\" for quotes, \\\\ for backslash).\n"
        "- When your answer is complete (no more tools needed), respond with plain text only.\n", -1);

    sb_appendf(&sb, "- The workspace is on Windows. Current directory: %s\n", cwd);

    return sb.data;
}

//=============================================================================
// Tool call parsing
//=============================================================================

typedef struct {
    char    name[64];
    char*   path;
    char*   content;
    char*   oldText;
    char*   newText;
    char*   command;
} ToolCall;

static int ParseToolCalls(const char* response, ToolCall** ppCalls)
{
    // Count <tool_call> occurrences
    int count = 0;
    const char* p = response;
    while ((p = strstr(p, "<tool_call>")) != NULL)
    {
        count++;
        p += 11;
    }

    if (count == 0) { *ppCalls = NULL; return 0; }

    ToolCall* calls = (ToolCall*)calloc(count, sizeof(ToolCall));
    if (!calls) { *ppCalls = NULL; return 0; }
    *ppCalls = calls;

    p = response;
    int actual = 0;
    for (int i = 0; i < count; i++)
    {
        p = strstr(p, "<tool_call>");
        if (!p) break;
        p += 11;

        // Skip whitespace
        while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;

        const char* end = strstr(p, "</tool_call>");
        if (!end) break;

        int jsonLen = (int)(end - p);
        char* json = (char*)malloc(jsonLen + 1);
        if (!json) break;
        memcpy(json, p, jsonLen);
        json[jsonLen] = '\0';

        // Extract tool name
        char* name = json_extract_string_a(json, "name");
        if (name)
        {
            strncpy(calls[actual].name, name, sizeof(calls[actual].name) - 1);
            free(name);
        }

        // Extract parameters
        calls[actual].path    = json_extract_string_a(json, "path");
        calls[actual].content = json_extract_string_a(json, "content");
        calls[actual].oldText = json_extract_string_a(json, "old_text");
        calls[actual].newText = json_extract_string_a(json, "new_text");
        calls[actual].command = json_extract_string_a(json, "command");

        free(json);
        actual++;
        p = end + 12;
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
    }
    free(calls);
}

//=============================================================================
// Directory helper
//=============================================================================

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

    // Iteratively create directories
    for (char* p = dirPath; *p; p++)
    {
        if (*p == '\\' || *p == '/')
        {
            char saved = *p;
            *p = '\0';
            if (dirPath[0]) CreateDirectoryA(dirPath, NULL);
            *p = saved;
        }
    }
    CreateDirectoryA(dirPath, NULL);
}

//=============================================================================
// Tool execution
//=============================================================================

static char* Tool_ReadFile(const char* path)
{
    if (!path || !path[0])
        return _strdup("Error: No file path specified");

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot open file '%s' (error %lu)",
                 path, GetLastError());
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

    char* content = (char*)malloc(readSize + 128);
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

    if (truncated)
    {
        char notice[128];
        snprintf(notice, sizeof(notice),
                 "\n\n[... truncated, showing first %lu of %lu bytes]",
                 readSize, fileSize);
        strcat(content, notice);
    }

    return content;
}

static char* Tool_WriteFile(const char* path, const char* content)
{
    if (!path || !path[0])
        return _strdup("Error: No file path specified");
    if (!content) content = "";

    EnsureParentDirExists(path);

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot create file '%s' (error %lu)",
                 path, GetLastError());
        return _strdup(err);
    }

    DWORD len = (DWORD)strlen(content);
    DWORD written;
    WriteFile(hFile, content, len, &written, NULL);
    CloseHandle(hFile);

    char msg[512];
    snprintf(msg, sizeof(msg), "Successfully wrote %lu bytes to '%s'", written, path);
    return _strdup(msg);
}

static char* Tool_ReplaceInFile(const char* path, const char* oldText, const char* newText)
{
    if (!path || !path[0])
        return _strdup("Error: No file path specified");
    if (!oldText || !oldText[0])
        return _strdup("Error: old_text is empty");
    if (!newText) newText = "";

    // Read the file
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot open file '%s' (error %lu)",
                 path, GetLastError());
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
    hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
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

    char msg[512];
    snprintf(msg, sizeof(msg),
             "Successfully replaced text in '%s' (%d bytes -> %d bytes)",
             path, oldLen, newLen);
    return _strdup(msg);
}

static char* Tool_RunCommand(const char* command)
{
    if (!command || !command[0])
        return _strdup("Error: No command specified");

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

    // Build command line: cmd.exe /c "command"
    int cmdLen = (int)strlen(command);
    char* cmdLine = (char*)malloc(cmdLen + 32);
    if (!cmdLine)
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return _strdup("Error: Out of memory");
    }
    snprintf(cmdLine, cmdLen + 32, "cmd.exe /c %s", command);

    BOOL bCreated = CreateProcessA(
        NULL, cmdLine, NULL, NULL, TRUE,
        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    free(cmdLine);
    CloseHandle(hWritePipe); // close write end in parent

    if (!bCreated)
    {
        DWORD err = GetLastError();
        CloseHandle(hReadPipe);
        char errBuf[256];
        snprintf(errBuf, sizeof(errBuf), "Error: CreateProcess failed (err=%lu)", err);
        return _strdup(errBuf);
    }

    CloseHandle(pi.hThread);

    // Read output
    StrBuf output;
    sb_init(&output, 4096);

    char buf[4096];
    DWORD bytesRead;
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

    // Wait for process with timeout
    DWORD waitResult = WaitForSingleObject(pi.hProcess, CMD_TIMEOUT_MS);

    DWORD exitCode = 0;
    if (waitResult == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    }
    else
    {
        TerminateProcess(pi.hProcess, 1);
        sb_append(&output, "\n[Process timed out and was terminated]", -1);
    }

    CloseHandle(pi.hProcess);

    // Format result
    if (exitCode != 0 && waitResult == WAIT_OBJECT_0)
    {
        sb_appendf(&output, "\n[Exit code: %lu]", exitCode);
    }

    if (output.len == 0)
    {
        sb_free(&output);
        return _strdup("(no output)");
    }

    return output.data; // caller frees
}

static char* Tool_ListDir(const char* path)
{
    if (!path || !path[0])
        return _strdup("Error: No directory path specified");

    char searchPath[MAX_PATH];
    snprintf(searchPath, sizeof(searchPath), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Cannot list directory '%s' (error %lu)",
                 path, GetLastError());
        return _strdup(err);
    }

    StrBuf sb;
    sb_init(&sb, 1024);

    int count = 0;
    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
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

// Dispatch a single tool call
static char* ExecuteTool(const ToolCall* tc)
{
    if (strcmp(tc->name, "read_file") == 0)
        return Tool_ReadFile(tc->path);
    if (strcmp(tc->name, "write_file") == 0)
        return Tool_WriteFile(tc->path, tc->content);
    if (strcmp(tc->name, "replace_in_file") == 0)
        return Tool_ReplaceInFile(tc->path, tc->oldText, tc->newText);
    if (strcmp(tc->name, "run_command") == 0)
        return Tool_RunCommand(tc->command);
    if (strcmp(tc->name, "list_dir") == 0)
        return Tool_ListDir(tc->path);

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

//=============================================================================
// Agent thread
//=============================================================================

typedef struct {
    AIProviderConfig cfg;
    char*   szUserMessage;
    HWND    hwndTarget;
} AgentParams;

static unsigned __stdcall AgentThreadProc(void* pArg)
{
    AgentParams* p = (AgentParams*)pArg;

    // Build system prompt
    char* systemPrompt = BuildSystemPrompt();
    if (!systemPrompt)
    {
        char* err = _strdup("Error: Could not build system prompt");
        PostMessage(p->hwndTarget, WM_AI_DIRECT_RESPONSE, 0, (LPARAM)err);
        free(p->szUserMessage);
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
    msgCount++;

    // Agent loop
    int iteration = 0;
    char* finalResponse = NULL;

    while (iteration < MAX_AGENT_ITERATIONS)
    {
        iteration++;

        if (iteration > 1)
            PostStatusToUI(p->hwndTarget, "Thinking... (step %d)", iteration);

        // Call LLM
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
            finalResponse = response;
            break;
        }

        // Parse tool calls
        ToolCall* toolCalls = NULL;
        int toolCount = ParseToolCalls(response, &toolCalls);

        if (toolCount == 0)
        {
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
        sb_init(&toolResults, 4096);

        for (int i = 0; i < toolCount; i++)
        {
            // Post tool info to UI
            const char* detail = toolCalls[i].path ? toolCalls[i].path :
                                 toolCalls[i].command ? toolCalls[i].command : "";
            PostToolInfoToUI(p->hwndTarget, toolCalls[i].name, detail);

            // Execute
            char* result = ExecuteTool(&toolCalls[i]);

            // Append to results message
            sb_appendf(&toolResults, "[Tool result: %s", toolCalls[i].name);
            if (toolCalls[i].path)
                sb_appendf(&toolResults, " - %s", toolCalls[i].path);
            sb_append(&toolResults, "]\n", -1);
            if (result)
            {
                // Truncate very long results
                int rlen = (int)strlen(result);
                if (rlen > MAX_FILE_READ_SIZE)
                {
                    result[MAX_FILE_READ_SIZE] = '\0';
                    sb_append(&toolResults, result, MAX_FILE_READ_SIZE);
                    sb_append(&toolResults, "\n[... truncated]\n", -1);
                }
                else
                {
                    sb_append(&toolResults, result, rlen);
                }
                free(result);
            }
            sb_append(&toolResults, "\n", 1);
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
        finalResponse = _strdup("Error: Agent loop exceeded maximum iterations");
    }

    // Save to history: user message + final response (not intermediate tool calls)
    AddToHistory("user", p->szUserMessage);
    AddToHistory("assistant", finalResponse);

    // Post final response to UI
    PostMessage(p->hwndTarget, WM_AI_DIRECT_RESPONSE, 0, (LPARAM)_strdup(finalResponse));

    // Cleanup
    free(finalResponse);
    for (int i = 0; i < msgCount; i++)
    {
        if (ownedStrings[i]) free(ownedStrings[i]);
    }
    free(ownedStrings);
    free(msgs);
    free(p->szUserMessage);
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
                       HWND hwndTarget)
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

    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, AgentThreadProc, p, 0, NULL);
    if (!hThread)
    {
        free(p->szUserMessage);
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
