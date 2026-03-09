/******************************************************************************
*
* Biko
*
* AIContext.c
*   Extract editor context for AI requests using Scintilla API.
*
******************************************************************************/

#include "AIContext.h"
#include "CommonUtils.h"
#include "FileManager.h"
#include "GitUI.h"
#include "SciCall.h"
#include "Scintilla.h"
#include <Shlwapi.h>
#include <string.h>
#include <stdio.h>

// External references from Notepad2.c
extern WCHAR szCurFile[MAX_PATH];

//=============================================================================
// Language ID mapping from Scintilla lexer IDs
//=============================================================================

typedef struct
{
    int     iLexer;
    const char* pszId;
} LexerLanguageMap;

static const LexerLanguageMap s_languageMap[] = {
    { 3 /* SCLEX_CPP */,        "cpp" },
    { 5 /* SCLEX_PYTHON */,     "python" },
    { 6 /* SCLEX_HTML */,       "html" },
    { 7 /* SCLEX_XML */,        "xml" },
    { 11 /* SCLEX_JAVA */,      "java" },    // CPP-based
    { 14 /* SCLEX_SQL */,       "sql" },
    { 15 /* SCLEX_VB */,        "vb" },
    { 18 /* SCLEX_LUA */,       "lua" },
    { 22 /* SCLEX_BASH */,      "bash" },
    { 27 /* SCLEX_BATCH */,     "batch" },
    { 28 /* SCLEX_MAKEFILE */,  "makefile" },
    { 34 /* SCLEX_PERL */,      "perl" },
    { 38 /* SCLEX_CSS */,       "css" },
    { 47 /* SCLEX_FORTRAN */,   "fortran" },
    { 62 /* SCLEX_JSON */,      "json" },
    { 63 /* SCLEX_YAML */,      "yaml" },
    { 65 /* SCLEX_RUST */,      "rust" },
    { 100 /* SCLEX_D */,        "d" },
    { 0, NULL } // sentinel
};

static const char* GetLanguageFromLexer(int iLexer)
{
    for (int i = 0; s_languageMap[i].pszId; i++)
    {
        if (s_languageMap[i].iLexer == iLexer)
            return s_languageMap[i].pszId;
    }
    return "text";
}

static const char* GetLanguageFromExtension(LPCWSTR wszPath)
{
    LPCWSTR ext = PathFindExtensionW(wszPath);
    if (!ext || !*ext) return "text";
    ext++; // skip dot

    if (_wcsicmp(ext, L"c") == 0) return "c";
    if (_wcsicmp(ext, L"h") == 0) return "c";
    if (_wcsicmp(ext, L"cpp") == 0 || _wcsicmp(ext, L"cxx") == 0 ||
        _wcsicmp(ext, L"cc") == 0) return "cpp";
    if (_wcsicmp(ext, L"hpp") == 0 || _wcsicmp(ext, L"hxx") == 0) return "cpp";
    if (_wcsicmp(ext, L"py") == 0) return "python";
    if (_wcsicmp(ext, L"js") == 0) return "javascript";
    if (_wcsicmp(ext, L"ts") == 0) return "typescript";
    if (_wcsicmp(ext, L"java") == 0) return "java";
    if (_wcsicmp(ext, L"cs") == 0) return "csharp";
    if (_wcsicmp(ext, L"rs") == 0) return "rust";
    if (_wcsicmp(ext, L"go") == 0) return "go";
    if (_wcsicmp(ext, L"rb") == 0) return "ruby";
    if (_wcsicmp(ext, L"php") == 0) return "php";
    if (_wcsicmp(ext, L"html") == 0 || _wcsicmp(ext, L"htm") == 0) return "html";
    if (_wcsicmp(ext, L"css") == 0) return "css";
    if (_wcsicmp(ext, L"json") == 0) return "json";
    if (_wcsicmp(ext, L"xml") == 0) return "xml";
    if (_wcsicmp(ext, L"yaml") == 0 || _wcsicmp(ext, L"yml") == 0) return "yaml";
    if (_wcsicmp(ext, L"md") == 0) return "markdown";
    if (_wcsicmp(ext, L"sh") == 0 || _wcsicmp(ext, L"bash") == 0) return "bash";
    if (_wcsicmp(ext, L"bat") == 0 || _wcsicmp(ext, L"cmd") == 0) return "batch";
    if (_wcsicmp(ext, L"ps1") == 0) return "powershell";
    if (_wcsicmp(ext, L"sql") == 0) return "sql";
    if (_wcsicmp(ext, L"lua") == 0) return "lua";
    if (_wcsicmp(ext, L"swift") == 0) return "swift";
    if (_wcsicmp(ext, L"kt") == 0) return "kotlin";
    if (_wcsicmp(ext, L"r") == 0) return "r";
    if (_wcsicmp(ext, L"toml") == 0) return "toml";
    if (_wcsicmp(ext, L"ini") == 0) return "ini";
    if (_wcsicmp(ext, L"makefile") == 0) return "makefile";

    return "text";
}

static char* ai_ctx_strdup(const char* s)
{
    if (!s) return NULL;
    int len = (int)strlen(s);
    char* p = (char*)n2e_Alloc(len + 1);
    if (p)
        memcpy(p, s, len + 1);
    return p;
}

static char* AIContext_Utf8FromWide(LPCWSTR wszText)
{
    if (!wszText || !wszText[0]) return NULL;
    int needed = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) return NULL;
    char* p = (char*)n2e_Alloc(needed);
    if (p)
        WideCharToMultiByte(CP_UTF8, 0, wszText, -1, p, needed, NULL, NULL);
    return p;
}

static unsigned __int64 AIContext_Fnv1a64(const char* s)
{
    const unsigned char* p = (const unsigned char*)(s ? s : "");
    unsigned __int64 h = 1469598103934665603ULL;
    while (*p)
    {
        h ^= (unsigned __int64)(*p++);
        h *= 1099511628211ULL;
    }
    return h;
}

static char* AIContext_BuildBufferHash(const char* pszContent)
{
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%016llx", AIContext_Fnv1a64(pszContent));
    return ai_ctx_strdup(buf);
}

static BOOL AIContext_PathHasLeaf(LPCWSTR wszDir, LPCWSTR wszLeaf)
{
    if (!wszDir || !wszDir[0] || !wszLeaf || !wszLeaf[0]) return FALSE;
    WCHAR wszPath[MAX_PATH];
    _snwprintf_s(wszPath, _countof(wszPath), _TRUNCATE, L"%s\\%s", wszDir, wszLeaf);
    return PathFileExistsW(wszPath);
}

static char* AIContext_BuildActiveFilesList(void)
{
    if (!szCurFile[0]) return NULL;
    return AIContext_Utf8FromWide(szCurFile);
}

static char* AIContext_BuildHotZones(HWND hwndScintilla, const char* pszSelection)
{
    char buf[512];
    buf[0] = '\0';

    if (pszSelection && pszSelection[0])
    {
        const char* end = pszSelection;
        while (*end && *end != '\r' && *end != '\n')
            end++;
        int len = (int)(end - pszSelection);
        if (len > 180) len = 180;
        if (len > 0)
        {
            memcpy(buf, pszSelection, len);
            buf[len] = '\0';
            return ai_ctx_strdup(buf);
        }
    }

    int pos = (int)SendMessage(hwndScintilla, SCI_GETCURRENTPOS, 0, 0);
    int line = (int)SendMessage(hwndScintilla, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s:%d", szCurFile[0] ? "(current file)" : "(buffer)", line + 1);
    return ai_ctx_strdup(buf);
}

static char* AIContext_BuildDiagnosticsSummary(void)
{
    return ai_ctx_strdup("No structured editor diagnostics captured for this mission.");
}

static char* AIContext_BuildAtlasSummary(const char* pszLanguage,
                                         const char* pszProjectRoot,
                                         const char* pszGitSummary,
                                         const char* pszHotZones)
{
    char buf[1024];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "file=%s | lang=%s | project=%s | git=%s | hot=%s",
                szCurFile[0] ? "<current>" : "<untitled>",
                pszLanguage ? pszLanguage : "text",
                pszProjectRoot ? pszProjectRoot : "(none)",
                pszGitSummary ? pszGitSummary : "(no git summary)",
                pszHotZones ? pszHotZones : "(none)");
    return ai_ctx_strdup(buf);
}

static char* AIContext_InferBuildCommandUtf8(LPCWSTR wszProjectRoot)
{
    if (!wszProjectRoot || !wszProjectRoot[0]) return NULL;
    if (AIContext_PathHasLeaf(wszProjectRoot, L"do_build.cmd"))
        return ai_ctx_strdup("cmd /c do_build.cmd");
    if (AIContext_PathHasLeaf(wszProjectRoot, L"Notepad2e.vcxproj"))
        return ai_ctx_strdup("msbuild Notepad2e.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64");
    return NULL;
}

static char* AIContext_InferTestCommandUtf8(LPCWSTR wszProjectRoot)
{
    if (!wszProjectRoot || !wszProjectRoot[0]) return NULL;
    if (AIContext_PathHasLeaf(wszProjectRoot, L"test\\Extension\\Notepad2eTests.vcxproj"))
        return ai_ctx_strdup("msbuild test\\Extension\\Notepad2eTests.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64");
    return NULL;
}

//=============================================================================
// Public functions
//=============================================================================

const char* AIContext_GetLanguageId(void)
{
    // Try extension first (more accurate)
    if (szCurFile[0])
    {
        const char* lang = GetLanguageFromExtension(szCurFile);
        if (strcmp(lang, "text") != 0)
            return lang;
    }

    // Fall back to lexer ID
    int lexer = (int)SciCall(SCI_GETLEXER, 0, 0);
    return GetLanguageFromLexer(lexer);
}

char* AIContext_GetFileContent(HWND hwndScintilla, int* pLength)
{
    int len = (int)SendMessage(hwndScintilla, SCI_GETLENGTH, 0, 0);
    if (len <= 0)
    {
        if (pLength) *pLength = 0;
        return NULL;
    }

    char* pBuf = (char*)n2e_Alloc(len + 1);
    if (!pBuf) return NULL;

    SendMessage(hwndScintilla, SCI_GETTEXT, (WPARAM)(len + 1), (LPARAM)pBuf);
    pBuf[len] = '\0';

    if (pLength) *pLength = len;
    return pBuf;
}

char* AIContext_GetSelection(HWND hwndScintilla, int* pLength)
{
    int selStart = (int)SendMessage(hwndScintilla, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessage(hwndScintilla, SCI_GETSELECTIONEND, 0, 0);

    if (selStart == selEnd)
    {
        if (pLength) *pLength = 0;
        return NULL;
    }

    int len = selEnd - selStart;
    char* pBuf = (char*)n2e_Alloc(len + 1);
    if (!pBuf) return NULL;

    struct Sci_TextRange tr;
    tr.chrg.cpMin = selStart;
    tr.chrg.cpMax = selEnd;
    tr.lpstrText = pBuf;
    SendMessage(hwndScintilla, SCI_GETTEXTRANGE, 0, (LPARAM)&tr);
    pBuf[len] = '\0';

    if (pLength) *pLength = len;
    return pBuf;
}

void AIContext_GetCursorPos(HWND hwndScintilla, int* pLine, int* pCol)
{
    int pos = (int)SendMessage(hwndScintilla, SCI_GETCURRENTPOS, 0, 0);
    *pLine = (int)SendMessage(hwndScintilla, SCI_LINEFROMPOSITION, (WPARAM)pos, 0);
    *pCol = (int)SendMessage(hwndScintilla, SCI_GETCOLUMN, (WPARAM)pos, 0);
}

BOOL AIContext_GetSelectionRange(HWND hwndScintilla,
                                int* pStartLine, int* pStartCol,
                                int* pEndLine, int* pEndCol)
{
    int selStart = (int)SendMessage(hwndScintilla, SCI_GETSELECTIONSTART, 0, 0);
    int selEnd = (int)SendMessage(hwndScintilla, SCI_GETSELECTIONEND, 0, 0);

    if (selStart == selEnd) return FALSE;

    *pStartLine = (int)SendMessage(hwndScintilla, SCI_LINEFROMPOSITION, (WPARAM)selStart, 0);
    *pStartCol = (int)SendMessage(hwndScintilla, SCI_GETCOLUMN, (WPARAM)selStart, 0);
    *pEndLine = (int)SendMessage(hwndScintilla, SCI_LINEFROMPOSITION, (WPARAM)selEnd, 0);
    *pEndCol = (int)SendMessage(hwndScintilla, SCI_GETCOLUMN, (WPARAM)selEnd, 0);

    return TRUE;
}

void AIContext_GetViewport(HWND hwndScintilla, int* pFirstLine, int* pLastLine)
{
    *pFirstLine = (int)SendMessage(hwndScintilla, SCI_GETFIRSTVISIBLELINE, 0, 0);
    int linesOnScreen = (int)SendMessage(hwndScintilla, SCI_LINESONSCREEN, 0, 0);
    *pLastLine = *pFirstLine + linesOnScreen;
}

BOOL AIContext_GetProjectRoot(LPCWSTR wszFilePath, WCHAR* wszRoot, int cchRoot)
{
    if (!wszFilePath || !wszFilePath[0]) return FALSE;

    WCHAR wszDir[MAX_PATH];
    lstrcpynW(wszDir, wszFilePath, MAX_PATH);
    PathRemoveFileSpecW(wszDir);

    // Walk up looking for project markers
    static const WCHAR* markers[] = {
        L".git", L".hg", L".svn", L"Makefile", L"CMakeLists.txt",
        L"package.json", L"Cargo.toml", L"go.mod", L".biko-project",
        NULL
    };

    while (wszDir[0] && lstrcmpiW(wszDir, L"\\") != 0)
    {
        for (int i = 0; markers[i]; i++)
        {
            WCHAR wszTest[MAX_PATH];
            PathCombineW(wszTest, wszDir, markers[i]);
            if (PathFileExistsW(wszTest))
            {
                lstrcpynW(wszRoot, wszDir, cchRoot);
                return TRUE;
            }
        }

        // Go up one level
        if (!PathRemoveFileSpecW(wszDir)) break;
    }

    // No marker found â€” use the file's directory
    lstrcpynW(wszRoot, wszDir, cchRoot);
    return FALSE;
}

BOOL AIContext_FillRequest(AIRequest* pReq, HWND hwndScintilla)
{
    if (!pReq || !hwndScintilla) return FALSE;

    // File path (convert WCHAR to UTF-8)
    if (szCurFile[0])
        pReq->pszFilePath = AIContext_Utf8FromWide(szCurFile);

    // Language
    const char* lang = AIContext_GetLanguageId();
    int langLen = (int)strlen(lang);
    pReq->pszLanguage = (char*)n2e_Alloc(langLen + 1);
    if (pReq->pszLanguage)
        memcpy(pReq->pszLanguage, lang, langLen + 1);

    // File content
    int contentLen = 0;
    pReq->pszFileContent = AIContext_GetFileContent(hwndScintilla, &contentLen);
    pReq->uBufferVersion = pReq->uRequestId;
    pReq->pszBufferHash = AIContext_BuildBufferHash(pReq->pszFileContent);

    // Selection
    int selLen = 0;
    pReq->pszSelectedText = AIContext_GetSelection(hwndScintilla, &selLen);

    // Cursor
    AIContext_GetCursorPos(hwndScintilla, &pReq->iCursorLine, &pReq->iCursorCol);

    // Selection range
    if (pReq->pszSelectedText)
    {
        AIContext_GetSelectionRange(hwndScintilla,
            &pReq->iSelStartLine, &pReq->iSelStartCol,
            &pReq->iSelEndLine, &pReq->iSelEndCol);
    }

    // Viewport
    AIContext_GetViewport(hwndScintilla,
        &pReq->iFirstVisibleLine, &pReq->iLastVisibleLine);

    // Project root / atlas-lite
    WCHAR wszProjectRoot[MAX_PATH] = L"";
    BOOL bHasProjectRoot = FALSE;
    if (szCurFile[0])
        bHasProjectRoot = AIContext_GetProjectRoot(szCurFile, wszProjectRoot, COUNTOF(wszProjectRoot));
    if ((!bHasProjectRoot || !wszProjectRoot[0]) && FileManager_GetRootPath()[0])
        lstrcpynW(wszProjectRoot, FileManager_GetRootPath(), COUNTOF(wszProjectRoot));

    if (wszProjectRoot[0])
        pReq->pszProjectRoot = AIContext_Utf8FromWide(wszProjectRoot);

    pReq->pszActiveFiles = AIContext_BuildActiveFilesList();
    pReq->pszDiagnostics = AIContext_BuildDiagnosticsSummary();
    pReq->pszHotZones = AIContext_BuildHotZones(hwndScintilla, pReq->pszSelectedText);
    pReq->pszBuildCommand = AIContext_InferBuildCommandUtf8(wszProjectRoot);
    pReq->pszTestCommand = AIContext_InferTestCommandUtf8(wszProjectRoot);

    {
        const WCHAR* wszGitSummary = GitUI_GetStatusSummary();
        if (wszGitSummary && wszGitSummary[0])
            pReq->pszGitSummary = AIContext_Utf8FromWide(wszGitSummary);
    }

    pReq->pszAtlasSummary = AIContext_BuildAtlasSummary(
        pReq->pszLanguage,
        pReq->pszProjectRoot,
        pReq->pszGitSummary,
        pReq->pszHotZones);

    return TRUE;
}
