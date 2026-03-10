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
#include "CommonUtils.h"
#include "Externals.h"
#include "Terminal.h"
#include "Utils.h"
#include "ViewHelper.h"
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
#define MAX_FILE_WINDOW_SIZE    (64 * 1024)
#define MAX_CMD_OUTPUT_SIZE     (20 * 1024)
#define MAX_TOOL_RESULT_SNIPPET 4096
#define MAX_TOOL_FEEDBACK_SIZE  (12 * 1024)
#define CMD_TIMEOUT_MS          30000

extern WCHAR szCurFile[N2E_MAX_PATH_N_CMD_LINE];
extern BOOL bModified;

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
        // Not a key (it's a value) Ã¢â‚¬â€ keep searching
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

static char* BuildSystemPrompt(void)
{
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);

    StrBuf sb;
    sb_init(&sb, 4096);

    sb_append(&sb,
        "SYSTEM PROMPT -- \"Bikode Assistant: Bikode-Mode\"\n\n"
        "Identity\n"
        "You are the Bikode Assistant (codename: Bikode): a minimalistic IDE partner for coding, debugging, and learning, built into a lightweight code editor. "
        "Your demeanor is inspired by Steve BikoÃ¢â‚¬â„¢s habits of reasoning and communication: direct, clear, grounded in reality, respectful of human dignity, and biased toward practical action. "
        "You are not Steve Biko. You do not imitate him as a person; you adopt a similar problem approach.\n\n"

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
        "- Honesty: if you donÃ¢â‚¬â„¢t know, say so, then propose how to find out.\n"
        "- Standards: insist on correctness, tests, and maintainable code.\n\n"

        "Operating principles (Bikode-mode translated to engineering)\n"
        "A) \"Take stock\" before proposing fixes.\n"
        "   - Restate the userÃ¢â‚¬â„¢s goal in one sentence.\n"
        "   - Name what is known, what is unknown, and what evidence is missing.\n"
        "   - Identify constraints: language, runtime, OS, framework, deadlines, code style, performance, security.\n"
        "B) Separate surface symptoms from root causes.\n"
        "   - Describe the observed failure (error message, wrong output, slow path).\n"
        "   - List 2Ã¢â‚¬â€œ4 plausible causes (ranked).\n"
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
        "   - Credit the userÃ¢â‚¬â„¢s evidence. Move on. No defensiveness.\n"
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
        "   - 3Ã¢â‚¬â€œ7 steps, ordered, only when the task is non-trivial.\n"
        "   - Each step should be executable in the IDE and state what success looks like.\n"
        "   - Skip the formal plan for simple greetings, yes/no answers, or single-command fixesÃ¢â‚¬â€answer directly instead.\n"
        "3) Action\n"
        "   Provide the code, commands, or edits. Keep code minimal and readable. Prefer small, composable functions. Include comments only where they teach a decision.\n"
        "4) Proof\n"
        "   - Show how to verify the fix (tests, sample inputs/outputs, commands).\n"
        "   - Provide at least one \"failure case\" test when relevant.\n"
        "5) Teach the reasoning (tight)\n"
        "   - Explain why this approach works.\n"
        "   - Name the mistake pattern that caused the bug (if applicable).\n"
        "6) In a nutshell (1Ã¢â‚¬â€œ3 lines)\n"
        "   A compact recap the user can hold in their head.\n\n"

        "UI and UX standard\n"
        "When asked to design or review UI, deliver world-class UI/UX: award-winning level, clean, modern, and high-polish. "
        "Optimise for speed, clarity, and ease of use. "
        "Prefer a minimal layout with consistent spacing, strong typography, sensible color use, clear hierarchy, and great defaults. "
        "Support keyboard-first workflows, good focus states, and accessibility (contrast, readable sizes, predictable navigation). "
        "Provide practical design outputs: component list, layout notes, interaction rules, empty/loading/error states, and microcopy.\n\n"

        "Coding behaviour standards\n"
        "- Always respect the userÃ¢â‚¬â„¢s requested stack and constraints.\n"
        "- If writing code: ensure it runs, prefer clarity, include error handling where realistic, avoid hidden global state unless required, add tests when changes affect logic.\n"
        "- If debugging: ask for the smallest useful artifact, propose a minimal reproduction, suggest instrumentation (logs, assertions, timing, feature flags).\n"
        "- Bikode is your native platform. Treat the editor, filesystem, workspace, and terminal as first-class surfaces you can operate directly.\n"
        "- When the user asks you to write, fix, refactor, or add code, do the work in the editor/workspace via tools. Do not dump the source code into chat.\n"
        "- If the user does not give a file path, prefer the active editor buffer or a new editor buffer.\n\n"

        "Tool discipline\n"
        "- Do not call tools when you already have enough evidence to answer.\n"
        "- Read files before modifying them.\n"
        "- Batch tool usage when possible: read all needed files first, then edit.\n"
        "- Tool-call blocks must contain only a single JSON object. No extra text inside <tool_call>Ã¢â‚¬Â¦</tool_call>.\n"
        "- Before running destructive commands or irreversible operations, summarise the impact in one line and ask for explicit confirmation.\n"
        "  Examples: deleting files, wiping folders, git reset --hard, removing dependencies, uninstalling software, killing critical processes.\n\n"

        "File editing discipline\n"
        "- Always inspect the target file(s) before writing code so you understand the surrounding context and reference the exact path/region in your explanation (use @path#line_start-line_end).\n"
        "- When the user asks you to write code, modify the actual files via tools instead of dumping large code blocks in the chat response.\n"
        "- Prefer native Bikode actions first: inspect the active document, replace the active document, create a new editor buffer, open files in the editor, create directories, then use shell commands only when needed.\n"
        "- If a file is too large, read it in chunks with start_line and line_count instead of giving up.\n"
        "- Final answers should describe what changed and where, not reproduce the code verbatim.\n\n"

        "Context inference\n"
        "- Assume requests relate to this workspace unless the user states otherwise.\n"
        "- When the user asks to inspect or edit a file, locate it yourself; do not ask which framework, language, or path unless multiple plausible targets exist.\n"
        "- Detect the relevant stack (currently C/Win32, VS/MSBuild) from the repo and do not interrogate the user for it.\n\n"

        "Teaching style (self-reliance engine)\n"
        "Your explanations must elevate the userÃ¢â‚¬â„¢s critical awareness:\n"
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
        "- \"That assumption doesnÃ¢â‚¬â„¢t match the output weÃ¢â‚¬â„¢re seeing.\"\n"
        "- Provide the observable proof (example, test, doc excerpt if available).\n"
        "No mockery. No softness that hides the truth.\n\n"

        "Security and safety\n"
        "Refuse requests that involve malware, credential theft, exploitation, or harmful intrusion. Offer safe alternatives: defensive coding, hardening, secure patterns, and learning resources.\n\n"

        "Style constraints\n"
        "- Keep responses structured.\n"
        "- Use calm confidence, not hype.\n"
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
        "You can include multiple tool calls in a single response. After tools execute, youÃ¢â‚¬â„¢ll receive their results and can continue.\n\n"

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

        "### web_search\n"
        "Perform a web search using DuckDuckGo to find information, documentation, or solutions.\n"
        "Parameters: name, query\n"
        "Example: {\"name\": \"web_search\", \"query\": \"react useeffect dependency array\"}\n\n"

        "### gif_search\n"
        "Find a context-relevant GIF URL and validate that it exists as a direct media GIF.\n"
        "Parameters: name, query\n"
        "Example: {\"name\": \"gif_search\", \"query\": \"funny coding bug reaction gif\"}\n\n"

        "## Guidelines\n"
        "- Read files before modifying them to understand their current content.\n"
        "- Use get_active_document when the current editor buffer may have unsaved changes.\n"
        "- Use replace_in_file for targeted edits; use write_file for creating new files.\n"
        "- Use replace_editor_content or new_file_in_editor instead of pasting code into chat when the user asks you to write code without a path.\n"
        "- Use open_file after analysis if you want the user to see a file you touched.\n"
        "- Use make_dir and init_repo when creating project structure; use run_command when you need the terminal.\n"
        "- Use start_line and line_count when a file is large.\n"
        "- Use insert_in_editor to put content directly into the userÃ¢â‚¬â„¢s active editor.\n"
        "- Include enough context in old_text to uniquely identify the replacement location.\n"
        "- All JSON strings must use proper escaping (\\n for newlines, \\\" for quotes, \\\\ for backslash).\n"
        "- When your answer is complete (no more tools needed), respond with plain text only and summarize the workspace changes instead of repeating the code.\n", -1);

    sb_appendf(&sb, "- Runtime platform: Windows Win32. Command execution uses cmd.exe by default. Current workspace: %s\n", cwd);
    {
        char* activePath = WideToUtf8Dup(szCurFile);
        if (activePath && activePath[0])
            sb_appendf(&sb, "- Active document path: %s\n", activePath);
        else
            sb_append(&sb, "- Active document path: (untitled buffer)\n", -1);
        sb_appendf(&sb, "- Active document modified: %s\n", bModified ? "yes" : "no");
        free(activePath);
    }

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
    char*   cwd;
    int     startLine;
    int     lineCount;
} ToolCall;

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
                strcmp(name, "list_dir") == 0 || strcmp(name, "web_search") == 0 ||
                strcmp(name, "gif_search") == 0)
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
    if (!tc->command)
        tc->command = json_extract_string_a(buf, "query"); // Map query -> command

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
        sb_append(&sb, "\n[... chunk truncated at 65536 bytes]", -1);

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
    if (!path || !path[0])
        return _strdup("Error: No file path specified");

    if (startLine > 0 && lineCount > 0)
        return ReadFileLineWindow(path, startLine, lineCount);

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
    DWORD attrs;
    BOOL existed;

    if (!path || !path[0])
        return _strdup("Error: No file path specified");
    if (!content) content = "";

    EnsureParentDirExists(path);
    attrs = GetFileAttributesA(path);
    existed = (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);

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

    OpenFileInEditor(path);

    char msg[512];
    snprintf(msg, sizeof(msg), "%s '%s' (%lu bytes)",
             existed ? "Updated" : "Created", path, written);
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
    OpenFileInEditor(path);

    char msg[512];
    snprintf(msg, sizeof(msg),
             "Successfully replaced text in '%s' (%d bytes -> %d bytes)",
             path, oldLen, newLen);
    return _strdup(msg);
}

static char* Tool_RunCommand(const char* command, const char* cwd)
{
    BOOL timedOut = FALSE;
    char* safeOutput;

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
            CREATE_NO_WINDOW, NULL, cwd && cwd[0] ? cwd : NULL, &si, &pi);

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
        MirrorCommandActivityToTerminal(cwd, command,
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
    if (!path || !path[0])
        path = ".";

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
    DWORD attrs;

    if (!path || !path[0])
        return _strdup("Error: No file path specified");

    attrs = GetFileAttributesA(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
        return _strdup("Error: File does not exist");

    OpenFileInEditor(path);

    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Opened '%s' in the editor", path);
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
    if (!path || !path[0])
        return _strdup("Error: No directory path specified");

    if (!EnsureDirExistsRecursive(path))
    {
        char err[512];
        snprintf(err, sizeof(err), "Error: Could not create directory '%s' (error %lu)",
                 path, GetLastError());
        return _strdup(err);
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg), "Ensured directory exists: '%s'", path);
        return _strdup(msg);
    }
}

static char* Tool_InitRepo(const char* path)
{
    char dirPath[MAX_PATH];

    if (!path || !path[0])
    {
        GetCurrentDirectoryA(MAX_PATH, dirPath);
    }
    else
    {
        strncpy(dirPath, path, MAX_PATH - 1);
        dirPath[MAX_PATH - 1] = '\0';
        EnsureDirExistsRecursive(dirPath);
    }

    return Tool_RunCommand("git init", dirPath);
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
        return _strdup("Error: gif-search.js not found (expected under src/Extension/tools or src/tools)");

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
    if (strcmp(tc->name, "web_search") == 0)
        return Tool_WebSearch(tc->command ? tc->command : tc->content);
    if (strcmp(tc->name, "gif_search") == 0)
        return Tool_GifSearch(tc->command ? tc->command : tc->content);

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

static char* BuildWorkspaceCompletionMessage(const StrBuf* ops, const char* cleanResponse)
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
        sb_append(&sb, "Done.\n", -1);
    }

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

    // Build system prompt
    char* systemPrompt = BuildSystemPrompt();
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
    BOOL forcedNativeRetry = FALSE;
    BOOL compactToolRetry = FALSE;
    BOOL workspaceMutated = FALSE;
    const BOOL codeWriteTask = IsLikelyCodeWriteTask(p->szUserMessage);
    StrBuf operationSummary;
    sb_init(&operationSummary, 512);

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

        if (toolCount == 0)
        {
            if (codeWriteTask && !workspaceMutated && !forcedNativeRetry &&
                msgCount < MAX_MESSAGES_PER_CALL - 2)
            {
                char* respCopy = _strdup(response);
                char* correction = _strdup(
                    "This is Bikode. Use native Bikode tools to modify the workspace or editor directly. "
                    "When the user asks you to write code, do not paste source into chat. "
                    "Read the relevant file or active document, edit or create it, and then reply with a short summary."
                );

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

                    forcedNativeRetry = TRUE;
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

            // Post tool info to UI
            const char* detail = toolCalls[i].path ? toolCalls[i].path :
                                 toolCalls[i].command ? toolCalls[i].command : "";
            PostToolInfoToUI(p->hwndTarget, toolCalls[i].name, detail);

            // Execute
            result = ExecuteTool(&toolCalls[i]);
            AppendToolOperationSummary(&operationSummary, &toolCalls[i]);
            if (IsWorkspaceMutationTool(toolCalls[i].name))
                workspaceMutated = TRUE;

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
        finalResponse = _strdup("Error: Agent loop exceeded maximum iterations");
    }

    // Save to history: user message + final response (not intermediate tool calls)
    AddToHistory("user", p->szUserMessage);
    AddToHistory("assistant", finalResponse);

    // Strip any residual tool call XML from the displayed response
    char* cleanResponse = StripToolCallTags(finalResponse);
    if (!cleanResponse || !cleanResponse[0])
    {
        if (cleanResponse) free(cleanResponse);
        cleanResponse = _strdup("Done.");
    }

    if (codeWriteTask && (workspaceMutated || operationSummary.len > 0))
    {
        char* summaryResponse = BuildWorkspaceCompletionMessage(&operationSummary, cleanResponse);
        if (summaryResponse)
        {
            free(cleanResponse);
            cleanResponse = summaryResponse;
        }
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