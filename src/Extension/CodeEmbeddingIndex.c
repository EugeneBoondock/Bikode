/******************************************************************************
*
* Biko
*
* CodeEmbeddingIndex.c
*   Local code-search index built from hashed lexical embeddings.
*
******************************************************************************/

#include "CodeEmbeddingIndex.h"
#include "mono_json.h"
#include <windows.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Shlwapi.lib")

#define CEI_VECTOR_DIM          64
#define CEI_MAX_FILE_BYTES      (384 * 1024)
#define CEI_MAX_CHUNK_LINES     72
#define CEI_MAX_CHUNK_BYTES     8192
#define CEI_MAX_SNIPPET_CHARS   220
#define CEI_MAX_RESULTS         8

typedef struct CeiStrBuf {
    char* data;
    int len;
    int cap;
} CeiStrBuf;

typedef struct CodeChunk {
    char* relativePath;
    int startLine;
    int endLine;
    int vector[CEI_VECTOR_DIM];
    double norm;
    char* snippet;
} CodeChunk;

typedef struct CodeIndexCache {
    CRITICAL_SECTION cs;
    BOOL csInit;
    WCHAR root[MAX_PATH];
    WCHAR artifactPath[MAX_PATH];
    CodeChunk* chunks;
    int chunkCount;
    int chunkCap;
    int fileCount;
    DWORD lastBuildTick;
    BOOL dirty;
} CodeIndexCache;

static CodeIndexCache s_cache;

static void sb_init(CeiStrBuf* sb, int cap)
{
    if (!sb)
        return;
    sb->cap = cap > 256 ? cap : 256;
    sb->len = 0;
    sb->data = (char*)malloc((size_t)sb->cap);
    if (sb->data)
        sb->data[0] = '\0';
}

static void sb_append(CeiStrBuf* sb, const char* text, int len)
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
        grown = (char*)realloc(sb->data, (size_t)nextCap);
        if (!grown)
            return;
        sb->data = grown;
        sb->cap = nextCap;
    }
    memcpy(sb->data + sb->len, text, (size_t)len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

static void sb_appendf(CeiStrBuf* sb, const char* fmt, ...)
{
    char buffer[2048];
    va_list ap;
    int written;
    va_start(ap, fmt);
    written = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (written > 0)
        sb_append(sb, buffer, written);
}

static void sb_free(CeiStrBuf* sb)
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

static void CopyWideSafe(WCHAR* dst, int cchDst, LPCWSTR src)
{
    if (!dst || cchDst <= 0)
        return;
    dst[0] = L'\0';
    if (src)
        lstrcpynW(dst, src, cchDst);
}

static void EnsureCacheInit(void)
{
    if (s_cache.csInit)
        return;
    InitializeCriticalSection(&s_cache.cs);
    s_cache.csInit = TRUE;
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
    out = (char*)malloc((size_t)needed);
    if (!out)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wszText, -1, out, needed, NULL, NULL);
    return out;
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
    CopyWideSafe(wszParent, ARRAYSIZE(wszParent), wszPath);
    PathRemoveFileSpecW(wszParent);
    if (wszParent[0] && lstrcmpiW(wszParent, wszPath) != 0)
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
    CopyWideSafe(wszDir, ARRAYSIZE(wszDir), wszPath);
    PathRemoveFileSpecW(wszDir);
    return EnsureDirExists(wszDir);
}

static BOOL WriteUtf8File(LPCWSTR wszPath, const char* text)
{
    HANDLE hFile;
    DWORD len;
    DWORD written = 0;
    if (!wszPath || !text)
        return FALSE;
    EnsureParentDirExists(wszPath);
    hFile = CreateFileW(wszPath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
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

static void FreeChunksLocked(void)
{
    int i;
    for (i = 0; i < s_cache.chunkCount; i++)
    {
        if (s_cache.chunks[i].relativePath)
            free(s_cache.chunks[i].relativePath);
        if (s_cache.chunks[i].snippet)
            free(s_cache.chunks[i].snippet);
    }
    free(s_cache.chunks);
    s_cache.chunks = NULL;
    s_cache.chunkCount = 0;
    s_cache.chunkCap = 0;
    s_cache.fileCount = 0;
}

static BOOL CacheMatchesRootLocked(LPCWSTR wszProjectRoot)
{
    return (wszProjectRoot && wszProjectRoot[0] && s_cache.root[0] &&
        lstrcmpiW(s_cache.root, wszProjectRoot) == 0);
}

static BOOL IsAbsolutePathUtf8(const char* path)
{
    if (!path || !path[0])
        return FALSE;
    if ((isalpha((unsigned char)path[0]) && path[1] == ':' &&
         (path[2] == '\\' || path[2] == '/')) ||
        (path[0] == '\\' && path[1] == '\\'))
        return TRUE;
    return FALSE;
}

static char* NormalizeHintLeaf(const char* pathHint)
{
    const char* leaf;
    char* out;
    char* p;
    if (!pathHint || !pathHint[0])
        return NULL;
    leaf = pathHint;
    if (IsAbsolutePathUtf8(pathHint))
    {
        const char* pSlash = strrchr(pathHint, '/');
        const char* pBack = strrchr(pathHint, '\\');
        const char* last = pSlash > pBack ? pSlash : pBack;
        if (last && *(last + 1))
            leaf = last + 1;
    }
    out = DupString(leaf);
    if (!out)
        return NULL;
    for (p = out; *p; p++)
    {
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p - 'A' + 'a');
        else if (*p == '\\')
            *p = '/';
    }
    return out;
}

static BOOL ContainsCi(const char* haystack, const char* needle)
{
    size_t nlen;
    const char* p;
    if (!haystack || !needle || !needle[0])
        return FALSE;
    nlen = strlen(needle);
    for (p = haystack; *p; p++)
    {
        size_t i;
        for (i = 0; i < nlen; i++)
        {
            unsigned char hc = (unsigned char)p[i];
            unsigned char nc = (unsigned char)needle[i];
            if (!hc)
                return FALSE;
            if (hc >= 'A' && hc <= 'Z')
                hc = (unsigned char)(hc - 'A' + 'a');
            if (nc >= 'A' && nc <= 'Z')
                nc = (unsigned char)(nc - 'A' + 'a');
            if (hc != nc)
                break;
        }
        if (i == nlen)
            return TRUE;
    }
    return FALSE;
}

static unsigned HashToken(const char* token, int len)
{
    unsigned hash = 2166136261u;
    int i;
    for (i = 0; i < len; i++)
    {
        unsigned char ch = (unsigned char)token[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch - 'A' + 'a');
        hash ^= ch;
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

static void EmbeddingAddToken(int* vec, const char* token, int len, int weight)
{
    unsigned hash;
    int idx;
    int idx2;
    int mag;
    if (!vec || !token || len <= 1)
        return;
    hash = HashToken(token, len);
    mag = weight + min(len, 20) * 4;
    idx = (int)(hash % CEI_VECTOR_DIM);
    idx2 = (int)(((hash >> 11) ^ hash) % CEI_VECTOR_DIM);
    vec[idx] += ((hash >> 7) & 1) ? mag : -mag;
    vec[idx2] += ((hash >> 19) & 1) ? (mag / 2) : -(mag / 2);
}

static void EmbeddingAddText(int* vec, const char* text, int weight)
{
    char token[128];
    int tokenLen = 0;
    const unsigned char* p = (const unsigned char*)text;
    if (!vec || !text)
        return;
    for (; *p; p++)
    {
        unsigned char ch = *p;
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.')
        {
            if (tokenLen < (int)sizeof(token) - 1)
                token[tokenLen++] = (char)tolower(ch);
        }
        else if (tokenLen > 0)
        {
            token[tokenLen] = '\0';
            EmbeddingAddToken(vec, token, tokenLen, weight);
            tokenLen = 0;
        }
    }
    if (tokenLen > 0)
    {
        token[tokenLen] = '\0';
        EmbeddingAddToken(vec, token, tokenLen, weight);
    }
}

static double EmbeddingNorm(const int* vec)
{
    double sum = 0.0;
    int i;
    if (!vec)
        return 1.0;
    for (i = 0; i < CEI_VECTOR_DIM; i++)
        sum += (double)vec[i] * (double)vec[i];
    return sum > 0.0 ? sqrt(sum) : 1.0;
}

static double EmbeddingCosine(const int* lhs, double lhsNorm, const int* rhs, double rhsNorm)
{
    double dot = 0.0;
    int i;
    if (!lhs || !rhs || lhsNorm <= 0.0 || rhsNorm <= 0.0)
        return 0.0;
    for (i = 0; i < CEI_VECTOR_DIM; i++)
        dot += (double)lhs[i] * (double)rhs[i];
    return dot / (lhsNorm * rhsNorm);
}

static char* MakeSnippet(const char* text, int len)
{
    char* out;
    int i;
    int j = 0;
    int inSpace = 0;
    if (!text || len <= 0)
        return DupString("");
    out = (char*)malloc(CEI_MAX_SNIPPET_CHARS + 1);
    if (!out)
        return NULL;
    for (i = 0; i < len && j < CEI_MAX_SNIPPET_CHARS; i++)
    {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ')
        {
            if (!inSpace && j > 0)
            {
                out[j++] = ' ';
                inSpace = 1;
            }
            continue;
        }
        inSpace = 0;
        if (isprint(ch))
            out[j++] = (char)ch;
    }
    while (j > 0 && out[j - 1] == ' ')
        j--;
    out[j] = '\0';
    return out;
}

static BOOL LooksLikeText(const char* bytes, DWORD len)
{
    DWORD i;
    DWORD zeroCount = 0;
    if (!bytes || len == 0)
        return FALSE;
    for (i = 0; i < len && i < 4096; i++)
    {
        if (bytes[i] == '\0')
            zeroCount++;
    }
    return zeroCount == 0;
}

static BOOL ShouldIndexFile(LPCWSTR wszName)
{
    LPCWSTR ext;
    if (!wszName || !wszName[0])
        return FALSE;
    if (lstrcmpiW(wszName, L"AGENTS.md") == 0 ||
        lstrcmpiW(wszName, L"README.md") == 0 ||
        lstrcmpiW(wszName, L"CMakeLists.txt") == 0 ||
        lstrcmpiW(wszName, L"Makefile") == 0 ||
        lstrcmpiW(wszName, L"Dockerfile") == 0)
        return TRUE;
    ext = PathFindExtensionW(wszName);
    if (!ext || !ext[0])
        return FALSE;
    return lstrcmpiW(ext, L".c") == 0 || lstrcmpiW(ext, L".h") == 0 ||
        lstrcmpiW(ext, L".cpp") == 0 || lstrcmpiW(ext, L".hpp") == 0 ||
        lstrcmpiW(ext, L".cc") == 0 || lstrcmpiW(ext, L".hh") == 0 ||
        lstrcmpiW(ext, L".js") == 0 || lstrcmpiW(ext, L".ts") == 0 ||
        lstrcmpiW(ext, L".tsx") == 0 || lstrcmpiW(ext, L".jsx") == 0 ||
        lstrcmpiW(ext, L".json") == 0 || lstrcmpiW(ext, L".md") == 0 ||
        lstrcmpiW(ext, L".txt") == 0 || lstrcmpiW(ext, L".ini") == 0 ||
        lstrcmpiW(ext, L".toml") == 0 || lstrcmpiW(ext, L".yml") == 0 ||
        lstrcmpiW(ext, L".yaml") == 0 || lstrcmpiW(ext, L".py") == 0 ||
        lstrcmpiW(ext, L".ps1") == 0 || lstrcmpiW(ext, L".java") == 0 ||
        lstrcmpiW(ext, L".go") == 0 || lstrcmpiW(ext, L".rs") == 0 ||
        lstrcmpiW(ext, L".vcxproj") == 0 || lstrcmpiW(ext, L".filters") == 0 ||
        lstrcmpiW(ext, L".cs") == 0 || lstrcmpiW(ext, L".lua") == 0;
}

static BOOL ShouldSkipDirectory(LPCWSTR wszName)
{
    if (!wszName || !wszName[0])
        return TRUE;
    return lstrcmpiW(wszName, L".git") == 0 ||
        lstrcmpiW(wszName, L".vs") == 0 ||
        lstrcmpiW(wszName, L".idea") == 0 ||
        lstrcmpiW(wszName, L".bikode") == 0 ||
        lstrcmpiW(wszName, L"node_modules") == 0 ||
        lstrcmpiW(wszName, L"bin") == 0 ||
        lstrcmpiW(wszName, L"obj") == 0 ||
        lstrcmpiW(wszName, L"dist") == 0 ||
        lstrcmpiW(wszName, L"build") == 0;
}

static char* ReadUtf8FileLimited(LPCWSTR wszPath, DWORD* pcbOut)
{
    HANDLE hFile;
    LARGE_INTEGER size;
    DWORD read = 0;
    DWORD toRead;
    char* buffer;
    if (pcbOut)
        *pcbOut = 0;
    hFile = CreateFileW(wszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0)
    {
        CloseHandle(hFile);
        return NULL;
    }
    toRead = (DWORD)min((LONGLONG)CEI_MAX_FILE_BYTES, size.QuadPart);
    buffer = (char*)malloc((size_t)toRead + 1);
    if (!buffer)
    {
        CloseHandle(hFile);
        return NULL;
    }
    if (!ReadFile(hFile, buffer, toRead, &read, NULL))
    {
        CloseHandle(hFile);
        free(buffer);
        return NULL;
    }
    CloseHandle(hFile);
    buffer[read] = '\0';
    if (!LooksLikeText(buffer, read))
    {
        free(buffer);
        return NULL;
    }
    if (pcbOut)
        *pcbOut = read;
    return buffer;
}

static char* MakeRelativeUtf8(LPCWSTR wszRoot, LPCWSTR wszAbsPath)
{
    const WCHAR* wszRel;
    char* utf8;
    char* p;
    int rootLen;
    if (!wszAbsPath || !wszAbsPath[0])
        return NULL;
    wszRel = wszAbsPath;
    rootLen = wszRoot ? lstrlenW(wszRoot) : 0;
    if (rootLen > 0 && _wcsnicmp(wszAbsPath, wszRoot, rootLen) == 0)
    {
        wszRel = wszAbsPath + rootLen;
        while (*wszRel == L'\\' || *wszRel == L'/')
            wszRel++;
    }
    utf8 = WideToUtf8Dup(wszRel);
    if (!utf8)
        return NULL;
    for (p = utf8; *p; p++)
    {
        if (*p == '\\')
            *p = '/';
    }
    return utf8;
}

static BOOL PushChunkLocked(const char* relativePath, int startLine, int endLine,
                            const int* vec, double norm, const char* snippet)
{
    CodeChunk* grown;
    CodeChunk* chunk;
    if (!relativePath || !relativePath[0] || !vec)
        return FALSE;
    if (s_cache.chunkCount >= s_cache.chunkCap)
    {
        int nextCap = s_cache.chunkCap > 0 ? s_cache.chunkCap * 2 : 128;
        grown = (CodeChunk*)realloc(s_cache.chunks, sizeof(CodeChunk) * (size_t)nextCap);
        if (!grown)
            return FALSE;
        s_cache.chunks = grown;
        s_cache.chunkCap = nextCap;
    }
    chunk = &s_cache.chunks[s_cache.chunkCount++];
    ZeroMemory(chunk, sizeof(*chunk));
    chunk->relativePath = DupString(relativePath);
    chunk->snippet = DupString(snippet ? snippet : "");
    memcpy(chunk->vector, vec, sizeof(int) * CEI_VECTOR_DIM);
    chunk->norm = norm;
    chunk->startLine = startLine;
    chunk->endLine = endLine;
    return chunk->relativePath != NULL;
}

static BOOL AddChunkFromTextLocked(const char* relativePath, const char* text, int len,
                                   int startLine, int endLine)
{
    int vec[CEI_VECTOR_DIM];
    char* snippet;
    double norm;
    if (!relativePath || !relativePath[0] || !text || len <= 0)
        return FALSE;
    ZeroMemory(vec, sizeof(vec));
    EmbeddingAddText(vec, relativePath, 140);
    {
        char* window = (char*)malloc((size_t)len + 1);
        BOOL pushed = FALSE;
        if (!window)
            return FALSE;
        memcpy(window, text, (size_t)len);
        window[len] = '\0';
        EmbeddingAddText(vec, window, 100);
        snippet = MakeSnippet(window, len);
        norm = EmbeddingNorm(vec);
        if (snippet && snippet[0] && norm > 0.0)
            pushed = PushChunkLocked(relativePath, startLine, endLine, vec, norm, snippet);
        free(snippet);
        free(window);
        return pushed;
    }
}

static BOOL IndexFileLocked(LPCWSTR wszRoot, LPCWSTR wszFilePath)
{
    DWORD len = 0;
    char* content;
    char* relativeUtf8;
    int chunkStart = 0;
    int chunkStartLine = 1;
    int lineNo = 1;
    DWORD i;
    BOOL addedAny = FALSE;

    content = ReadUtf8FileLimited(wszFilePath, &len);
    if (!content || len == 0)
    {
        free(content);
        return FALSE;
    }
    relativeUtf8 = MakeRelativeUtf8(wszRoot, wszFilePath);
    if (!relativeUtf8)
    {
        free(content);
        return FALSE;
    }

    for (i = 0; i < len; i++)
    {
        BOOL boundary = FALSE;
        if (content[i] == '\n')
        {
            if ((lineNo - chunkStartLine + 1) >= CEI_MAX_CHUNK_LINES)
                boundary = TRUE;
            lineNo++;
        }
        if (!boundary && ((int)i - chunkStart + 1) >= CEI_MAX_CHUNK_BYTES &&
            (content[i] == '\n' || content[i] == ' ' || content[i] == '\t' || content[i] == ';' || content[i] == '}'))
        {
            boundary = TRUE;
        }
        if (boundary)
        {
            if (AddChunkFromTextLocked(relativeUtf8, content + chunkStart, (int)i - chunkStart + 1,
                chunkStartLine, max(chunkStartLine, lineNo - 1)))
            {
                addedAny = TRUE;
            }
            chunkStart = (int)i + 1;
            chunkStartLine = lineNo;
        }
    }

    if (chunkStart < (int)len)
    {
        if (AddChunkFromTextLocked(relativeUtf8, content + chunkStart, (int)len - chunkStart,
            chunkStartLine, max(chunkStartLine, lineNo)))
        {
            addedAny = TRUE;
        }
    }

    if (addedAny)
        s_cache.fileCount++;
    free(relativeUtf8);
    free(content);
    return addedAny;
}

static void ScanDirectoryLocked(LPCWSTR wszRoot, LPCWSTR wszDir)
{
    WCHAR wszPattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hFind;
    PathCombineW(wszPattern, wszDir, L"*");
    hFind = FindFirstFileW(wszPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        WCHAR wszChild[MAX_PATH];
        if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0)
            continue;
        PathCombineW(wszChild, wszDir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (!ShouldSkipDirectory(fd.cFileName))
                ScanDirectoryLocked(wszRoot, wszChild);
        }
        else if (ShouldIndexFile(fd.cFileName))
        {
            IndexFileLocked(wszRoot, wszChild);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

static BOOL PersistIndexLocked(void)
{
    JsonWriter w;
    SYSTEMTIME st;
    char* rootUtf8;
    char ts[64];
    int i;

    if (!s_cache.root[0] || s_cache.chunkCount <= 0)
        return FALSE;

    PathCombineW(s_cache.artifactPath, s_cache.root, L".bikode\\index\\code-embeddings.json");
    EnsureParentDirExists(s_cache.artifactPath);

    if (!JsonWriter_Init(&w, max(8192, s_cache.chunkCount * 256)))
        return FALSE;

    GetLocalTime(&st);
    rootUtf8 = WideToUtf8Dup(s_cache.root);
    _snprintf_s(ts, sizeof(ts), _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    JsonWriter_BeginObject(&w);
    JsonWriter_Int(&w, "version", 1);
    JsonWriter_String(&w, "generatedAt", ts);
    JsonWriter_Int(&w, "dimension", CEI_VECTOR_DIM);
    JsonWriter_Int(&w, "fileCount", s_cache.fileCount);
    JsonWriter_Int(&w, "chunkCount", s_cache.chunkCount);
    JsonWriter_String(&w, "root", rootUtf8 ? rootUtf8 : "");
    JsonWriter_Key(&w, "chunks");
    JsonWriter_BeginArray(&w);
    for (i = 0; i < s_cache.chunkCount; i++)
    {
        int j;
        JsonWriter_BeginObject(&w);
        JsonWriter_String(&w, "path", s_cache.chunks[i].relativePath);
        JsonWriter_Int(&w, "startLine", s_cache.chunks[i].startLine);
        JsonWriter_Int(&w, "endLine", s_cache.chunks[i].endLine);
        JsonWriter_String(&w, "snippet", s_cache.chunks[i].snippet ? s_cache.chunks[i].snippet : "");
        JsonWriter_Key(&w, "vector");
        JsonWriter_BeginArray(&w);
        for (j = 0; j < CEI_VECTOR_DIM; j++)
            JsonWriter_IntValue(&w, s_cache.chunks[i].vector[j]);
        JsonWriter_EndArray(&w);
        JsonWriter_EndObject(&w);
    }
    JsonWriter_EndArray(&w);
    JsonWriter_EndObject(&w);

    free(rootUtf8);
    i = WriteUtf8File(s_cache.artifactPath, JsonWriter_GetBuffer(&w));
    JsonWriter_Free(&w);
    return i ? TRUE : FALSE;
}

static BOOL RebuildCacheLocked(LPCWSTR wszProjectRoot)
{
    if (!wszProjectRoot || !wszProjectRoot[0])
        return FALSE;
    FreeChunksLocked();
    CopyWideSafe(s_cache.root, ARRAYSIZE(s_cache.root), wszProjectRoot);
    s_cache.artifactPath[0] = L'\0';
    s_cache.lastBuildTick = 0;
    s_cache.dirty = FALSE;
    ScanDirectoryLocked(wszProjectRoot, wszProjectRoot);
    s_cache.lastBuildTick = GetTickCount();
    if (s_cache.chunkCount > 0)
        PersistIndexLocked();
    return s_cache.chunkCount > 0;
}

typedef struct QueryResult {
    int index;
    double score;
} QueryResult;

static void InsertTopResult(QueryResult* top, int maxResults, int index, double score)
{
    int i;
    if (!top || maxResults <= 0)
        return;
    for (i = 0; i < maxResults; i++)
    {
        if (score > top[i].score)
        {
            int j;
            for (j = maxResults - 1; j > i; j--)
                top[j] = top[j - 1];
            top[i].index = index;
            top[i].score = score;
            return;
        }
    }
}

BOOL CodeEmbeddingIndex_QueryProject(const WCHAR* wszProjectRoot,
                                     const char* query,
                                     const char* pathHint,
                                     int maxResults,
                                     char** ppszResult)
{
    int queryVec[CEI_VECTOR_DIM];
    double queryNorm;
    QueryResult top[CEI_MAX_RESULTS];
    char* hintLeaf = NULL;
    CeiStrBuf out;
    int i;
    BOOL ok = FALSE;

    if (ppszResult)
        *ppszResult = NULL;
    if (!wszProjectRoot || !wszProjectRoot[0] || !query || !query[0] || !ppszResult)
        return FALSE;

    EnsureCacheInit();
    EnterCriticalSection(&s_cache.cs);

    if (!CacheMatchesRootLocked(wszProjectRoot) || s_cache.dirty || s_cache.chunkCount == 0)
    {
        if (!RebuildCacheLocked(wszProjectRoot))
        {
            LeaveCriticalSection(&s_cache.cs);
            return FALSE;
        }
    }

    ZeroMemory(queryVec, sizeof(queryVec));
    EmbeddingAddText(queryVec, query, 120);
    if (pathHint && pathHint[0])
        EmbeddingAddText(queryVec, pathHint, 150);
    queryNorm = EmbeddingNorm(queryVec);
    if (queryNorm <= 0.0)
    {
        LeaveCriticalSection(&s_cache.cs);
        return FALSE;
    }

    if (maxResults <= 0)
        maxResults = 4;
    if (maxResults > CEI_MAX_RESULTS)
        maxResults = CEI_MAX_RESULTS;
    for (i = 0; i < CEI_MAX_RESULTS; i++)
    {
        top[i].index = -1;
        top[i].score = -1000.0;
    }

    hintLeaf = NormalizeHintLeaf(pathHint);

    for (i = 0; i < s_cache.chunkCount; i++)
    {
        double score = EmbeddingCosine(queryVec, queryNorm, s_cache.chunks[i].vector, s_cache.chunks[i].norm);
        if (hintLeaf && hintLeaf[0] && ContainsCi(s_cache.chunks[i].relativePath, hintLeaf))
            score += 0.14;
        InsertTopResult(top, maxResults, i, score);
    }

    sb_init(&out, 2048);
    for (i = 0; i < maxResults; i++)
    {
        if (top[i].index < 0)
            continue;
        sb_appendf(&out, "%d. %s:%d-%d\n",
            i + 1,
            s_cache.chunks[top[i].index].relativePath,
            s_cache.chunks[top[i].index].startLine,
            s_cache.chunks[top[i].index].endLine);
        if (s_cache.chunks[top[i].index].snippet && s_cache.chunks[top[i].index].snippet[0])
            sb_appendf(&out, "   %s\n", s_cache.chunks[top[i].index].snippet);
    }
    if (out.len == 0)
        sb_append(&out, "(no strong repo matches found)", -1);

    *ppszResult = out.data;
    out.data = NULL;
    ok = TRUE;

    free(hintLeaf);
    LeaveCriticalSection(&s_cache.cs);
    return ok;
}

void CodeEmbeddingIndex_InvalidateProject(const WCHAR* wszProjectRoot)
{
    EnsureCacheInit();
    EnterCriticalSection(&s_cache.cs);
    if (!wszProjectRoot || !wszProjectRoot[0] || CacheMatchesRootLocked(wszProjectRoot))
        s_cache.dirty = TRUE;
    LeaveCriticalSection(&s_cache.cs);
}
