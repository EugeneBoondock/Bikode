/******************************************************************************
*
* Biko
*
* PluginManager.c
*   Lightweight plugin manager implementation.
*   - Lazy loading: no work if plugins/ folder doesn't exist
*   - Minimal memory: only loads plugin info until needed
*   - Fast startup: scans directory once, loads DLLs on demand
*
******************************************************************************/

#include "PluginManager.h"
#include "BikoPluginAPI.h"
#include "DarkMode.h"
#include "mono_json.h"
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include "SciCall.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <urlmon.h>
#include <process.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "urlmon.lib")

// ViewHelper.h defines hwndEdit as a macro; it breaks BikoHostServices.hwndEdit.
#ifdef hwndEdit
#undef hwndEdit
#endif

//=============================================================================
// Constants
//=============================================================================

#define MAX_PLUGINS         32
#define MAX_MENU_ITEMS      16   // Per plugin
#define PLUGINS_FOLDER      L"plugins"
#define MAX_AVAILABLE_PLUGINS 384
#define PLUGIN_MGR_CLASS    L"BikoPluginManagerWnd"
#define VSIX_CONTENT_FOLDER L"vscode-extensions"
#define OPENVSX_POPULAR_URL L"https://open-vsx.org/api/-/search?size=80&offset=0&sortBy=downloadCount&sortOrder=desc"

// Plugin manager window controls
#define IDC_PM_SEARCH       5010
#define IDC_PM_LIST         5011
#define IDC_PM_ADD          5012
#define IDC_PM_INSTALL      5013
#define IDC_PM_TOGGLE       5014
#define IDC_PM_UNINSTALL    5015
#define IDC_PM_RELOAD       5016
#define IDC_PM_CLOSE        5017
#define IDC_PM_ADDURL       5018
#define IDC_PM_STATUS       5019

#define PM_MSG_FETCH_COMPLETE   (WM_APP + 181)

#define PM_MIN_W            620
#define PM_MIN_H            500

// Plugin table columns
#define PM_COL_NAME         0
#define PM_COL_AUTHOR       1
#define PM_COL_INSTALLS     2
#define PM_COL_RATING       3
#define PM_COL_STATUS       4
#define PM_COL_ACTION       5
#define PM_COL_COUNT        6

//=============================================================================
// Plugin Entry Structure
//=============================================================================

typedef struct {
    WCHAR           dllPath[MAX_PATH];
    WCHAR           name[64];
    HMODULE         hModule;
    BOOL            loaded;
    BOOL            enabled;
    
    // Function pointers (resolved on load)
    PFN_BikoPlugin_GetInfo          pfnGetInfo;
    PFN_BikoPlugin_Init             pfnInit;
    PFN_BikoPlugin_Shutdown         pfnShutdown;
    PFN_BikoPlugin_GetMenuItemCount pfnGetMenuCount;
    PFN_BikoPlugin_GetMenuItem      pfnGetMenuItem;
    PFN_BikoPlugin_OnCommand        pfnOnCommand;
    PFN_BikoPlugin_OnDocumentChanged pfnOnDocChanged;
    
    // Menu items
    int             menuItemCount;
    UINT            menuCmdBase;        // First command ID for this plugin
    
} PluginEntry;

//=============================================================================
// Module State
//=============================================================================

static HWND             g_hwndMain = NULL;
static HWND             g_hwndEdit = NULL;
static PluginEntry      g_plugins[MAX_PLUGINS];
static int              g_pluginCount = 0;
static UINT             g_nextCmdId = IDM_PLUGIN_CMD_BASE;
static HMENU            g_hPluginMenu = NULL;
static BikoHostServices g_hostServices;
static BOOL             g_initialized = FALSE;
static BOOL             g_uiClassRegistered = FALSE;
static HWND             g_hwndPluginMgr = NULL;

typedef struct {
    WCHAR name[128];
    WCHAR source[1024];      // local path or URL
    WCHAR description[256];
    WCHAR author[96];
    int   popularity;
    int   ratingPermille;
    BOOL  installed;
    BOOL  installable;
    BOOL  referenceOnly;
    BOOL  onlineEntry;
} AvailablePlugin;

typedef enum {
    PM_ROW_INSTALLED = 1,
    PM_ROW_AVAILABLE = 2
} PluginMgrRowType;

typedef struct {
    PluginMgrRowType type;
    int index;
} PluginMgrRow;

typedef struct {
    LONG requestId;
} PlatformFetchRequest;

typedef struct {
    LONG requestId;
    BOOL usedFallback;
    int count;
    AvailablePlugin items[MAX_AVAILABLE_PLUGINS];
} PlatformFetchResult;

static AvailablePlugin g_availablePlugins[MAX_AVAILABLE_PLUGINS];
static int             g_availableCount = 0;
static PluginMgrRow    g_uiRows[MAX_PLUGINS + MAX_AVAILABLE_PLUGINS];
static int             g_uiRowCount = 0;
static volatile LONG   g_platformFetchRequestId = 0;
static BOOL            g_platformFetchInProgress = FALSE;
static WCHAR           g_platformStatusText[160] = L"";

//=============================================================================
// Forward Declarations
//=============================================================================

static void InitHostServices(void);
static BOOL LoadPlugin(PluginEntry* pEntry);
static void UnloadPlugin(PluginEntry* pEntry);
static void ScanPluginsFolder(void);
static void GetPluginsPath(LPWSTR path, int maxLen);
static void RemovePluginMenuFromMain(void);
static void RebuildPluginMenu(void);
static BOOL EnsurePluginsDirPath(LPWSTR outPath, int cchOut);
static BOOL IsHttpUrlW(const WCHAR* s);
static void LoadAvailableCatalog(void);
static void SortAvailablePlugins(void);
static void RemoveOnlineAvailableEntries(void);
static void RefreshAvailableInstalledFlags(void);
static void RefreshPluginManagerList(void);
static void UpdatePluginManagerButtons(void);
static BOOL AddAvailablePluginEntryEx(const WCHAR* name, const WCHAR* source, const WCHAR* description,
                                      BOOL installable, BOOL referenceOnly, const WCHAR* author,
                                      int popularity, int ratingPermille, BOOL onlineEntry);
static void LoadCatalogFile(const WCHAR* path);
static void StartPlatformFetchAsync(void);
static unsigned __stdcall PlatformFetchThreadProc(void* arg);
static void SetPluginManagerStatusText(const WCHAR* text);
static BOOL AddResultEntry(PlatformFetchResult* out, const WCHAR* name, const WCHAR* source, const WCHAR* description,
                           BOOL installable, BOOL referenceOnly, const WCHAR* author,
                           int popularity, int ratingPermille);
static BOOL IsStaleFetchRequest(LONG requestId);
static BOOL LoadUrlToBufferA(const WCHAR* url, char** outData, int* outLen);
static void ParseNotepadPluginListJson(const char* json, int len, PlatformFetchResult* out);
static void ParseOpenVsxSearchJson(const char* json, int len, PlatformFetchResult* out);
static int GetNotepadPopularityBoost(const WCHAR* name);
static void AddFallbackPopularNotepadPlugins(PlatformFetchResult* out);
static void CompactDescriptionW(WCHAR* text, int cchText);
static void Utf8ToWideSafe(const char* src, WCHAR* out, int cchOut);
static BOOL IsDirectPluginBinaryUrlW(const WCHAR* url);
static BOOL IsPluginArchiveUrlW(const WCHAR* url);
static BOOL IsVsixPackageUrlW(const WCHAR* url);
static BOOL CopyPluginFileWithPrompt(const WCHAR* srcPath, const WCHAR* dstPath);
static void NormalizePathForTar(const WCHAR* srcPath, WCHAR* outPath, int cchOut);
static BOOL ExtractArchiveToDirectory(const WCHAR* archivePath, const WCHAR* extractDir);
static BOOL FindFirstDllRecursive(const WCHAR* dir, WCHAR* outPath, int cchOut);
static void DeleteDirectoryTree(const WCHAR* dir);
static BOOL InstallArchiveToPluginsFolder(const WCHAR* archivePath, const WCHAR* pluginsDir, WCHAR* outInstalledPath, int cchOut);
static BOOL BuildPackageStemFromSource(const WCHAR* sourceOrPath, const WCHAR* fallbackName, WCHAR* outStem, int cchOut);
static BOOL InstallVsixContentPackage(const WCHAR* archivePath, const WCHAR* pluginsDir,
                                      const WCHAR* sourceOrPath, const WCHAR* fallbackName,
                                      WCHAR* outInstalledPath, int cchOut);
static BOOL IsVsixContentInstalled(const WCHAR* pluginsDir, const WCHAR* sourceOrPath, const WCHAR* fallbackName);
static BOOL DownloadUrlToPath(const WCHAR* url, const WCHAR* outPath);
static BOOL CreateTempFilePath(const WCHAR* extension, WCHAR* outPath, int cchOut);
static void FormatCountW(int value, WCHAR* out, int cchOut);
static void LayoutPluginManagerControls(HWND hwnd);
static void SetupPluginManagerListColumns(HWND hList);
static void ResizePluginManagerListColumns(HWND hList);
static void ApplyPluginManagerTheme(HWND hwnd);
static HBRUSH GetPluginMgrBackgroundBrush(void);
static LRESULT HandlePluginListCustomDraw(NMLVCUSTOMDRAW* pcd);
static void ShowPluginManagerWindow(void);
static LRESULT CALLBACK PluginManagerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

//=============================================================================
// Host Service Implementations
//=============================================================================

static int Host_GetText(LPWSTR buffer, int maxLen)
{
    if (!g_hwndEdit || !buffer || maxLen <= 0) return 0;
    
    int len = (int)SendMessage(g_hwndEdit, SCI_GETLENGTH, 0, 0);
    if (len <= 0) { buffer[0] = 0; return 0; }
    
    // Get UTF-8 text from Scintilla
    int utf8Len = len + 1;
    char* utf8 = (char*)LocalAlloc(LPTR, utf8Len);
    if (!utf8) { buffer[0] = 0; return 0; }
    
    SendMessage(g_hwndEdit, SCI_GETTEXT, utf8Len, (LPARAM)utf8);
    
    // Convert to UTF-16
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buffer, maxLen);
    LocalFree(utf8);
    
    return wideLen > 0 ? wideLen - 1 : 0;
}

static int Host_GetSelection(LPWSTR buffer, int maxLen)
{
    if (!g_hwndEdit || !buffer || maxLen <= 0) return 0;
    
    int start = (int)SendMessage(g_hwndEdit, SCI_GETSELECTIONSTART, 0, 0);
    int end = (int)SendMessage(g_hwndEdit, SCI_GETSELECTIONEND, 0, 0);
    if (start >= end) { buffer[0] = 0; return 0; }
    
    int len = end - start;
    char* utf8 = (char*)LocalAlloc(LPTR, len + 1);
    if (!utf8) { buffer[0] = 0; return 0; }
    
    SendMessage(g_hwndEdit, SCI_GETSELTEXT, 0, (LPARAM)utf8);
    
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buffer, maxLen);
    LocalFree(utf8);
    
    return wideLen > 0 ? wideLen - 1 : 0;
}

static BOOL Host_SetText(LPCWSTR text)
{
    if (!g_hwndEdit || !text) return FALSE;
    
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)LocalAlloc(LPTR, utf8Len);
    if (!utf8) return FALSE;
    
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);
    SendMessage(g_hwndEdit, SCI_SETTEXT, 0, (LPARAM)utf8);
    LocalFree(utf8);
    
    return TRUE;
}

static BOOL Host_ReplaceSelection(LPCWSTR text)
{
    if (!g_hwndEdit || !text) return FALSE;
    
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)LocalAlloc(LPTR, utf8Len);
    if (!utf8) return FALSE;
    
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);
    SendMessage(g_hwndEdit, SCI_REPLACESEL, 0, (LPARAM)utf8);
    LocalFree(utf8);
    
    return TRUE;
}

static BOOL Host_InsertText(int pos, LPCWSTR text)
{
    if (!g_hwndEdit || !text) return FALSE;
    
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)LocalAlloc(LPTR, utf8Len);
    if (!utf8) return FALSE;
    
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, utf8Len, NULL, NULL);
    SendMessage(g_hwndEdit, SCI_INSERTTEXT, pos, (LPARAM)utf8);
    LocalFree(utf8);
    
    return TRUE;
}

static int Host_GetCursorPos(void)
{
    return g_hwndEdit ? (int)SendMessage(g_hwndEdit, SCI_GETCURRENTPOS, 0, 0) : 0;
}

static void Host_SetCursorPos(int pos)
{
    if (g_hwndEdit) SendMessage(g_hwndEdit, SCI_GOTOPOS, pos, 0);
}

static void Host_GetSelectionRange(int* start, int* end)
{
    if (!g_hwndEdit) { *start = *end = 0; return; }
    *start = (int)SendMessage(g_hwndEdit, SCI_GETSELECTIONSTART, 0, 0);
    *end = (int)SendMessage(g_hwndEdit, SCI_GETSELECTIONEND, 0, 0);
}

static void Host_SetSelectionRange(int start, int end)
{
    if (g_hwndEdit) SendMessage(g_hwndEdit, SCI_SETSEL, start, end);
}

static int Host_GetTextLength(void)
{
    return g_hwndEdit ? (int)SendMessage(g_hwndEdit, SCI_GETLENGTH, 0, 0) : 0;
}

static int Host_GetLineCount(void)
{
    return g_hwndEdit ? (int)SendMessage(g_hwndEdit, SCI_GETLINECOUNT, 0, 0) : 0;
}

static int Host_GetCurrentLine(void)
{
    if (!g_hwndEdit) return 0;
    int pos = (int)SendMessage(g_hwndEdit, SCI_GETCURRENTPOS, 0, 0);
    return (int)SendMessage(g_hwndEdit, SCI_LINEFROMPOSITION, pos, 0);
}

static void Host_SetStatusText(LPCWSTR text)
{
    // Use existing status bar mechanism
    extern HWND hwndStatus;
    if (hwndStatus && text)
    {
        SendMessage(hwndStatus, SB_SETTEXT, 255, (LPARAM)text);
    }
}

static int Host_ShowMessageBox(LPCWSTR text, LPCWSTR title, UINT type)
{
    return MessageBoxW(g_hwndMain, text, title, type);
}

static void Host_BeginUndoAction(void)
{
    if (g_hwndEdit) SendMessage(g_hwndEdit, SCI_BEGINUNDOACTION, 0, 0);
}

static void Host_EndUndoAction(void)
{
    if (g_hwndEdit) SendMessage(g_hwndEdit, SCI_ENDUNDOACTION, 0, 0);
}

static void InitHostServices(void)
{
    g_hostServices.apiVersion = BIKO_PLUGIN_API_VERSION;
    g_hostServices.hwndMain = g_hwndMain;
    g_hostServices.hwndEdit = g_hwndEdit;
    
    g_hostServices.GetText = Host_GetText;
    g_hostServices.GetSelection = Host_GetSelection;
    g_hostServices.SetText = Host_SetText;
    g_hostServices.ReplaceSelection = Host_ReplaceSelection;
    g_hostServices.InsertText = Host_InsertText;
    
    g_hostServices.GetCursorPos = Host_GetCursorPos;
    g_hostServices.SetCursorPos = Host_SetCursorPos;
    g_hostServices.GetSelectionRange = Host_GetSelectionRange;
    g_hostServices.SetSelectionRange = Host_SetSelectionRange;
    
    g_hostServices.GetTextLength = Host_GetTextLength;
    g_hostServices.GetLineCount = Host_GetLineCount;
    g_hostServices.GetCurrentLine = Host_GetCurrentLine;
    
    g_hostServices.SetStatusText = Host_SetStatusText;
    g_hostServices.ShowMessageBox = Host_ShowMessageBox;
    
    g_hostServices.BeginUndoAction = Host_BeginUndoAction;
    g_hostServices.EndUndoAction = Host_EndUndoAction;
}

//=============================================================================
// Plugin Loading
//=============================================================================

static void GetPluginsPath(LPWSTR path, int maxLen)
{
    WCHAR exeDir[MAX_PATH];
    WCHAR candidates[4][MAX_PATH];
    ZeroMemory(candidates, sizeof(candidates));

    GetModuleFileNameW(NULL, exeDir, MAX_PATH);
    PathRemoveFileSpecW(exeDir);

    // 1) Beside executable (release layout)
    StringCchPrintfW(candidates[0], MAX_PATH, L"%s\\%s", exeDir, PLUGINS_FOLDER);

    // 2) Repository root when running from bin/x64/{Debug,Release}
    StringCchPrintfW(candidates[1], MAX_PATH, L"%s\\..\\..\\..\\%s", exeDir, PLUGINS_FOLDER);
    PathCanonicalizeW(candidates[1], candidates[1]);

    // 3) Current working directory (developer launch)
    StringCchCopyW(candidates[2], MAX_PATH, PLUGINS_FOLDER);

    for (int i = 0; i < 3; i++)
    {
        if (PathIsDirectoryW(candidates[i]))
        {
            StringCchCopyW(path, maxLen, candidates[i]);
            return;
        }
    }

    // Keep a sensible default path for diagnostics if folder does not exist.
    StringCchCopyW(path, maxLen, candidates[0]);
}

static void ScanPluginsFolder(void)
{
    WCHAR pluginPath[MAX_PATH];
    WCHAR searchPath[MAX_PATH];
    WIN32_FIND_DATAW fd;
    
    GetPluginsPath(pluginPath, MAX_PATH);
    
    // Check if folder exists - if not, skip (zero overhead)
    if (!PathIsDirectoryW(pluginPath))
        return;
    
    StringCchPrintfW(searchPath, MAX_PATH, L"%s\\*.dll", pluginPath);
    
    HANDLE hFind = FindFirstFileW(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;
    
    do
    {
        if (g_pluginCount >= MAX_PLUGINS)
            break;
        
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        
        PluginEntry* pEntry = &g_plugins[g_pluginCount];
        ZeroMemory(pEntry, sizeof(PluginEntry));
        
        StringCchPrintfW(pEntry->dllPath, MAX_PATH, L"%s\\%s", pluginPath, fd.cFileName);
        
        // Get name without extension
        StringCchCopyW(pEntry->name, 64, fd.cFileName);
        PathRemoveExtensionW(pEntry->name);
        
        pEntry->enabled = TRUE;  // TODO: Read from INI
        pEntry->loaded = FALSE;
        
        g_pluginCount++;
        
    } while (FindNextFileW(hFind, &fd));
    
    FindClose(hFind);
}

static BOOL EnsurePluginsDirPath(LPWSTR outPath, int cchOut)
{
    if (!outPath || cchOut <= 0) return FALSE;
    GetPluginsPath(outPath, cchOut);
    if (PathIsDirectoryW(outPath))
        return TRUE;
    if (CreateDirectoryW(outPath, NULL))
        return TRUE;
    return (GetLastError() == ERROR_ALREADY_EXISTS);
}

static BOOL IsHttpUrlW(const WCHAR* s)
{
    if (!s) return FALSE;
    return (StrCmpNIW(s, L"http://", 7) == 0 || StrCmpNIW(s, L"https://", 8) == 0);
}

static BOOL AddAvailablePluginEntryEx(const WCHAR* name, const WCHAR* source, const WCHAR* description,
                                      BOOL installable, BOOL referenceOnly, const WCHAR* author,
                                      int popularity, int ratingPermille, BOOL onlineEntry)
{
    if (!name || !name[0]) return FALSE;

    // Merge duplicate rows by name+source.
    for (int i = 0; i < g_availableCount; i++)
    {
        BOOL sameName = (StrCmpIW(g_availablePlugins[i].name, name) == 0);
        BOOL sameSource = (StrCmpIW(g_availablePlugins[i].source, source ? source : L"") == 0);
        BOOL sameAuthor = (author && author[0] && g_availablePlugins[i].author[0] &&
                           StrCmpIW(g_availablePlugins[i].author, author) == 0);
        if (sameName && (sameSource || sameAuthor))
        {
            AvailablePlugin* ap = &g_availablePlugins[i];
            if (!ap->description[0] && description) StringCchCopyW(ap->description, ARRAYSIZE(ap->description), description);
            if (!ap->author[0] && author) StringCchCopyW(ap->author, ARRAYSIZE(ap->author), author);
            if (popularity > ap->popularity) ap->popularity = popularity;
            if (ratingPermille > ap->ratingPermille) ap->ratingPermille = ratingPermille;
            if (installable) ap->installable = TRUE;
            if (!referenceOnly) ap->referenceOnly = FALSE;
            if (onlineEntry) ap->onlineEntry = TRUE;
            return FALSE;
        }
    }

    if (g_availableCount >= MAX_AVAILABLE_PLUGINS) return FALSE;

    AvailablePlugin* ap = &g_availablePlugins[g_availableCount++];
    ZeroMemory(ap, sizeof(*ap));
    StringCchCopyW(ap->name, ARRAYSIZE(ap->name), name);
    StringCchCopyW(ap->source, ARRAYSIZE(ap->source), source ? source : L"");
    StringCchCopyW(ap->description, ARRAYSIZE(ap->description), description ? description : L"");
    StringCchCopyW(ap->author, ARRAYSIZE(ap->author), author ? author : L"");
    ap->popularity = popularity;
    ap->ratingPermille = ratingPermille;
    ap->installable = installable;
    ap->referenceOnly = referenceOnly;
    ap->installed = FALSE;
    ap->onlineEntry = onlineEntry;
    return TRUE;
}

static void LoadCatalogFile(const WCHAR* path)
{
    if (!path || !path[0] || !PathFileExistsW(path))
        return;

    FILE* f = _wfopen(path, L"rt, ccs=UTF-8");
    if (!f) {
        f = _wfopen(path, L"rt");
        if (!f) return;
    }

    WCHAR line[2048];
    while (fgetws(line, (int)ARRAYSIZE(line), f))
    {
        if (g_availableCount >= MAX_AVAILABLE_PLUGINS)
            break;

        size_t ln = wcslen(line);
        while (ln > 0 && (line[ln - 1] == L'\n' || line[ln - 1] == L'\r'))
            line[--ln] = L'\0';
        if (line[0] == L'\0' || line[0] == L'#')
            continue;

        WCHAR* name = line;
        WCHAR* sep1 = wcschr(name, L'|');
        if (!sep1) continue;
        *sep1 = L'\0';
        WCHAR* source = sep1 + 1;
        WCHAR* sep2 = wcschr(source, L'|');
        WCHAR* desc = L"";
        if (sep2) {
            *sep2 = L'\0';
            desc = sep2 + 1;
        }
        BOOL installable = IsDirectPluginBinaryUrlW(source) || IsPluginArchiveUrlW(source) || PathFileExistsW(source);
        AddAvailablePluginEntryEx(name, source, desc, installable, !installable, L"", 0, 0, FALSE);
    }

    fclose(f);
}

static BOOL IsDirectPluginBinaryUrlW(const WCHAR* url)
{
    if (!url || !url[0]) return FALSE;
    WCHAR clean[1024];
    StringCchCopyW(clean, ARRAYSIZE(clean), url);
    WCHAR* cut = wcspbrk(clean, L"?&#");
    if (cut) *cut = L'\0';
    const WCHAR* ext = PathFindExtensionW(clean);
    return ext && _wcsicmp(ext, L".dll") == 0;
}

static BOOL IsPluginArchiveUrlW(const WCHAR* url)
{
    if (!url || !url[0]) return FALSE;
    WCHAR clean[1024];
    StringCchCopyW(clean, ARRAYSIZE(clean), url);
    WCHAR* cut = wcspbrk(clean, L"?&#");
    if (cut) *cut = L'\0';
    const WCHAR* ext = PathFindExtensionW(clean);
    return ext && (_wcsicmp(ext, L".zip") == 0 || _wcsicmp(ext, L".vsix") == 0);
}

static BOOL IsVsixPackageUrlW(const WCHAR* url)
{
    if (!url || !url[0]) return FALSE;
    WCHAR clean[1024];
    StringCchCopyW(clean, ARRAYSIZE(clean), url);
    WCHAR* cut = wcspbrk(clean, L"?&#");
    if (cut) *cut = L'\0';
    const WCHAR* ext = PathFindExtensionW(clean);
    return ext && _wcsicmp(ext, L".vsix") == 0;
}

static void Utf8ToWideSafe(const char* src, WCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = '\0';
    if (!src || !src[0]) return;

    if (!MultiByteToWideChar(CP_UTF8, 0, src, -1, out, cchOut))
    {
        MultiByteToWideChar(CP_ACP, 0, src, -1, out, cchOut);
    }
}

static void CompactDescriptionW(WCHAR* text, int cchText)
{
    if (!text || cchText <= 1) return;

    int w = 0;
    BOOL previousSpace = TRUE;
    for (int r = 0; text[r] && w < cchText - 1; r++)
    {
        WCHAR ch = text[r];
        if (ch == L'\r' || ch == L'\n' || ch == L'\t')
            ch = L' ';
        if (ch == L' ')
        {
            if (previousSpace)
                continue;
            previousSpace = TRUE;
        }
        else
        {
            previousSpace = FALSE;
        }
        text[w++] = ch;
    }
    while (w > 0 && text[w - 1] == L' ')
        w--;
    text[w] = L'\0';
}

static BOOL AddResultEntry(PlatformFetchResult* out, const WCHAR* name, const WCHAR* source, const WCHAR* description,
                           BOOL installable, BOOL referenceOnly, const WCHAR* author,
                           int popularity, int ratingPermille)
{
    if (!out || !name || !name[0] || out->count >= MAX_AVAILABLE_PLUGINS)
        return FALSE;

    for (int i = 0; i < out->count; i++)
    {
        BOOL sameName = (StrCmpIW(out->items[i].name, name) == 0);
        BOOL sameSource = (StrCmpIW(out->items[i].source, source ? source : L"") == 0);
        BOOL sameAuthor = (author && author[0] && out->items[i].author[0] &&
                           StrCmpIW(out->items[i].author, author) == 0);
        if (sameName && (sameSource || sameAuthor))
        {
            AvailablePlugin* ap = &out->items[i];
            if (!ap->description[0] && description) StringCchCopyW(ap->description, ARRAYSIZE(ap->description), description);
            if (!ap->author[0] && author) StringCchCopyW(ap->author, ARRAYSIZE(ap->author), author);
            if (popularity > ap->popularity) ap->popularity = popularity;
            if (ratingPermille > ap->ratingPermille) ap->ratingPermille = ratingPermille;
            if (installable) ap->installable = TRUE;
            if (!referenceOnly) ap->referenceOnly = FALSE;
            return FALSE;
        }
    }

    AvailablePlugin* ap = &out->items[out->count++];
    ZeroMemory(ap, sizeof(*ap));
    StringCchCopyW(ap->name, ARRAYSIZE(ap->name), name);
    StringCchCopyW(ap->source, ARRAYSIZE(ap->source), source ? source : L"");
    StringCchCopyW(ap->description, ARRAYSIZE(ap->description), description ? description : L"");
    StringCchCopyW(ap->author, ARRAYSIZE(ap->author), author ? author : L"");
    ap->popularity = popularity;
    ap->ratingPermille = ratingPermille;
    ap->installable = installable;
    ap->referenceOnly = referenceOnly;
    ap->onlineEntry = TRUE;
    return TRUE;
}

static BOOL IsStaleFetchRequest(LONG requestId)
{
    LONG active = InterlockedCompareExchange(&g_platformFetchRequestId, 0, 0);
    return requestId != active;
}

static BOOL LoadUrlToBufferA(const WCHAR* url, char** outData, int* outLen)
{
    if (!url || !outData || !outLen) return FALSE;
    *outData = NULL;
    *outLen = 0;

    WCHAR tempDir[MAX_PATH];
    WCHAR tempFile[MAX_PATH];
    if (!GetTempPathW(ARRAYSIZE(tempDir), tempDir))
        return FALSE;
    if (!GetTempFileNameW(tempDir, L"bko", 0, tempFile))
        return FALSE;

    HRESULT hr = URLDownloadToFileW(NULL, url, tempFile, 0, NULL);
    if (FAILED(hr))
    {
        DeleteFileW(tempFile);
        return FALSE;
    }

    FILE* f = _wfopen(tempFile, L"rb");
    if (!f)
    {
        DeleteFileW(tempFile);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8 * 1024 * 1024)
    {
        fclose(f);
        DeleteFileW(tempFile);
        return FALSE;
    }

    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf)
    {
        fclose(f);
        DeleteFileW(tempFile);
        return FALSE;
    }

    size_t readBytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    DeleteFileW(tempFile);
    if ((long)readBytes != sz)
    {
        free(buf);
        return FALSE;
    }

    buf[sz] = '\0';
    *outData = buf;
    *outLen = (int)sz;
    return TRUE;
}

static void ParseNotepadPluginListJson(const char* json, int len, PlatformFetchResult* out)
{
    if (!json || len <= 0 || !out) return;

    JsonReader r;
    if (!JsonReader_Init(&r, json, len))
        return;
    if (JsonReader_Next(&r) != JSON_OBJECT_START)
        return;

    EJsonToken tok;
    while ((tok = JsonReader_Next(&r)) == JSON_KEY)
    {
        const char* key = JsonReader_GetString(&r);
        if (strcmp(key, "npp-plugins") == 0)
        {
            tok = JsonReader_Next(&r);
            if (tok != JSON_ARRAY_START)
            {
                JsonReader_SkipValue(&r);
                continue;
            }

            int rank = 0;
            while ((tok = JsonReader_Next(&r)) != JSON_ARRAY_END && tok != JSON_ERROR && tok != JSON_NONE)
            {
                if (tok != JSON_OBJECT_START)
                {
                    JsonReader_SkipValue(&r);
                    continue;
                }

                char nameA[256] = "";
                char sourceA[1024] = "";
                char descA[2048] = "";
                char authorA[256] = "";

                while ((tok = JsonReader_Next(&r)) == JSON_KEY)
                {
                    const char* pkey = JsonReader_GetString(&r);
                    tok = JsonReader_Next(&r);
                    if (strcmp(pkey, "display-name") == 0)
                        StringCchCopyA(nameA, ARRAYSIZE(nameA), JsonReader_GetString(&r));
                    else if (strcmp(pkey, "repository") == 0)
                        StringCchCopyA(sourceA, ARRAYSIZE(sourceA), JsonReader_GetString(&r));
                    else if (strcmp(pkey, "description") == 0)
                        StringCchCopyA(descA, ARRAYSIZE(descA), JsonReader_GetString(&r));
                    else if (strcmp(pkey, "author") == 0)
                        StringCchCopyA(authorA, ARRAYSIZE(authorA), JsonReader_GetString(&r));
                    else
                        JsonReader_SkipValue(&r);
                }

                if (!nameA[0] || !sourceA[0] || out->count >= MAX_AVAILABLE_PLUGINS)
                    continue;

                WCHAR nameW[128];
                WCHAR sourceW[1024];
                WCHAR descW[256];
                WCHAR authorW[96];
                Utf8ToWideSafe(nameA, nameW, ARRAYSIZE(nameW));
                Utf8ToWideSafe(sourceA, sourceW, ARRAYSIZE(sourceW));
                Utf8ToWideSafe(descA, descW, ARRAYSIZE(descW));
                Utf8ToWideSafe(authorA, authorW, ARRAYSIZE(authorW));
                CompactDescriptionW(descW, ARRAYSIZE(descW));

                BOOL installable = IsDirectPluginBinaryUrlW(sourceW) || IsPluginArchiveUrlW(sourceW);
                int popularity = (500 - rank) + GetNotepadPopularityBoost(nameW);
                AddResultEntry(out, nameW, sourceW, descW, installable, !installable, authorW, popularity, 0);
                rank++;
            }
        }
        else
        {
            JsonReader_Next(&r);
            JsonReader_SkipValue(&r);
        }
    }
}

static void ParseOpenVsxSearchJson(const char* json, int len, PlatformFetchResult* out)
{
    if (!json || len <= 0 || !out) return;

    JsonReader r;
    if (!JsonReader_Init(&r, json, len))
        return;
    if (JsonReader_Next(&r) != JSON_OBJECT_START)
        return;

    EJsonToken tok;
    while ((tok = JsonReader_Next(&r)) == JSON_KEY)
    {
        const char* key = JsonReader_GetString(&r);
        tok = JsonReader_Next(&r);

        if (strcmp(key, "extensions") == 0 && tok == JSON_ARRAY_START)
        {
            int rank = 0;
            while ((tok = JsonReader_Next(&r)) != JSON_ARRAY_END && tok != JSON_ERROR && tok != JSON_NONE)
            {
                if (tok != JSON_OBJECT_START)
                {
                    JsonReader_SkipValue(&r);
                    continue;
                }

                char namespaceA[128] = "";
                char nameA[128] = "";
                char displayNameA[256] = "";
                char descriptionA[2048] = "";
                char downloadA[1024] = "";
                int downloadCount = 0;
                int ratingPermille = 0;
                BOOL verified = FALSE;
                BOOL deprecated = FALSE;

                while ((tok = JsonReader_Next(&r)) == JSON_KEY)
                {
                    const char* ekey = JsonReader_GetString(&r);
                    tok = JsonReader_Next(&r);

                    if (strcmp(ekey, "namespace") == 0 && tok == JSON_STRING)
                    {
                        StringCchCopyA(namespaceA, ARRAYSIZE(namespaceA), JsonReader_GetString(&r));
                    }
                    else if (strcmp(ekey, "name") == 0 && tok == JSON_STRING)
                    {
                        StringCchCopyA(nameA, ARRAYSIZE(nameA), JsonReader_GetString(&r));
                    }
                    else if (strcmp(ekey, "displayName") == 0 && tok == JSON_STRING)
                    {
                        StringCchCopyA(displayNameA, ARRAYSIZE(displayNameA), JsonReader_GetString(&r));
                    }
                    else if (strcmp(ekey, "description") == 0 && tok == JSON_STRING)
                    {
                        StringCchCopyA(descriptionA, ARRAYSIZE(descriptionA), JsonReader_GetString(&r));
                    }
                    else if (strcmp(ekey, "downloadCount") == 0 && tok == JSON_NUMBER)
                    {
                        downloadCount = JsonReader_GetInt(&r);
                        if (downloadCount < 0) downloadCount = 0;
                    }
                    else if (strcmp(ekey, "averageRating") == 0 && tok == JSON_NUMBER)
                    {
                        double d = JsonReader_GetDouble(&r);
                        if (d < 0.0) d = 0.0;
                        if (d > 5.0) d = 5.0;
                        ratingPermille = (int)(d * 1000.0 + 0.5);
                    }
                    else if (strcmp(ekey, "verified") == 0 && tok == JSON_BOOL)
                    {
                        verified = JsonReader_GetBool(&r);
                    }
                    else if (strcmp(ekey, "deprecated") == 0 && tok == JSON_BOOL)
                    {
                        deprecated = JsonReader_GetBool(&r);
                    }
                    else if (strcmp(ekey, "files") == 0 && tok == JSON_OBJECT_START)
                    {
                        EJsonToken t2;
                        while ((t2 = JsonReader_Next(&r)) == JSON_KEY)
                        {
                            const char* fkey = JsonReader_GetString(&r);
                            t2 = JsonReader_Next(&r);
                            if (strcmp(fkey, "download") == 0 && t2 == JSON_STRING)
                            {
                                StringCchCopyA(downloadA, ARRAYSIZE(downloadA), JsonReader_GetString(&r));
                            }
                            else
                            {
                                JsonReader_SkipValue(&r);
                            }
                        }
                    }
                    else
                    {
                        JsonReader_SkipValue(&r);
                    }
                }

                if (deprecated || !downloadA[0] || out->count >= MAX_AVAILABLE_PLUGINS)
                    continue;

                char titleA[256] = "";
                if (displayNameA[0])
                {
                    StringCchCopyA(titleA, ARRAYSIZE(titleA), displayNameA);
                }
                else if (namespaceA[0] && nameA[0])
                {
                    StringCchPrintfA(titleA, ARRAYSIZE(titleA), "%s.%s", namespaceA, nameA);
                }
                else if (nameA[0])
                {
                    StringCchCopyA(titleA, ARRAYSIZE(titleA), nameA);
                }
                if (!titleA[0])
                    continue;

                WCHAR nameW[128];
                WCHAR sourceW[1024];
                WCHAR descW[256];
                WCHAR authorW[96];
                Utf8ToWideSafe(titleA, nameW, ARRAYSIZE(nameW));
                Utf8ToWideSafe(downloadA, sourceW, ARRAYSIZE(sourceW));
                Utf8ToWideSafe(descriptionA, descW, ARRAYSIZE(descW));
                Utf8ToWideSafe(namespaceA, authorW, ARRAYSIZE(authorW));
                if (!descW[0])
                    StringCchCopyW(descW, ARRAYSIZE(descW), L"VS Code extension package (content only).");
                CompactDescriptionW(descW, ARRAYSIZE(descW));

                int rankBoost = 1000 - rank;
                if (rankBoost < 0) rankBoost = 0;
                int sizeBoost = (downloadCount > 1000000) ? 200 : (downloadCount > 100000 ? 100 : 0);
                int popularity = 12000 + (verified ? 1000 : 0) + rankBoost + sizeBoost;
                BOOL installable = IsPluginArchiveUrlW(sourceW);
                AddResultEntry(out, nameW, sourceW, descW, installable, !installable, authorW, popularity, ratingPermille);
                rank++;
            }
        }
        else
        {
            JsonReader_SkipValue(&r);
        }
    }
}

static int GetNotepadPopularityBoost(const WCHAR* name)
{
    if (!name || !name[0]) return 0;
    if (StrCmpIW(name, L"NppExec") == 0) return 50000;
    if (StrCmpIW(name, L"Compare") == 0) return 49000;
    if (StrCmpIW(name, L"ComparePlus") == 0) return 48500;
    if (StrCmpIW(name, L"PythonScript") == 0) return 48000;
    if (StrCmpIW(name, L"JSON Viewer") == 0) return 47500;
    if (StrCmpIW(name, L"JSON Tools") == 0) return 47000;
    if (StrCmpIW(name, L"Explorer") == 0) return 46500;
    if (StrCmpIW(name, L"XML Tools") == 0) return 46000;
    if (StrCmpIW(name, L"HEX-Editor") == 0) return 45500;
    if (StrCmpIW(name, L"JSTool") == 0) return 45000;
    if (StrCmpIW(name, L"MarkdownViewer++") == 0) return 44500;
    if (StrCmpIW(name, L"Npp Converter") == 0) return 44000;
    if (StrCmpIW(name, L"CSV Lint") == 0) return 43500;
    if (StrCmpIW(name, L"AutoSave") == 0) return 43000;
    return 0;
}

static void AddFallbackPopularNotepadPlugins(PlatformFetchResult* out)
{
    if (!out) return;

    AddResultEntry(out,
        L"NppExec",
        L"https://github.com/d0vgan/nppexec/releases/download/NppExec_v0810/NppExec_0810_dll_x64.zip",
        L"Execute scripts and commands from Notepad++.",
        TRUE, FALSE, L"Vitaliy Dovgan", 50000, 0);
    AddResultEntry(out,
        L"Compare",
        L"https://github.com/pnedev/compare-plugin/releases/download/v2.0.2/ComparePlugin_v2.0.2_X64.zip",
        L"Diff and compare files inside Notepad++.",
        TRUE, FALSE, L"Pavel Nedev", 49000, 0);
    AddResultEntry(out,
        L"PythonScript",
        L"https://github.com/bruderstein/PythonScript/releases/download/v2.1.0/PythonScript_Full_2.1.0.0_x64_PluginAdmin.zip",
        L"Run Python automation scripts in Notepad++.",
        TRUE, FALSE, L"Dave Brotherstone", 48000, 0);
    AddResultEntry(out,
        L"JSON Viewer",
        L"https://github.com/NPP-JSONViewer/JSON-Viewer/releases/download/v2.1.1.0/NppJSONViewer_x64_Release.zip",
        L"JSON formatting and viewing tools.",
        TRUE, FALSE, L"Kapil Ratnani", 47500, 0);
    AddResultEntry(out,
        L"Explorer",
        L"https://github.com/oviradoi/npp-explorer-plugin/releases/download/v1.9.9/Explorer_x64.zip",
        L"File explorer panel for Notepad++.",
        TRUE, FALSE, L"Jens Lorenz", 46500, 0);
    AddResultEntry(out,
        L"XML Tools",
        L"https://github.com/morbac/xmltools/releases/download/3.1.1.13/XMLTools-3.1.1.13-x64.zip",
        L"XML pretty-printing and validation.",
        TRUE, FALSE, L"Nicolas Crittin", 46000, 0);
    AddResultEntry(out,
        L"HEX-Editor",
        L"https://github.com/chcg/NPP_HexEdit/releases/download/0.9.14/HexEditor_0.9.14_x64.zip",
        L"Hex editing features in Notepad++.",
        TRUE, FALSE, L"Jens Lorenz", 45500, 0);
    AddResultEntry(out,
        L"JSTool",
        L"https://sourceforge.net/projects/jsminnpp/files/Uni/JSToolNPP.25.11.16.uni.64.zip",
        L"JavaScript formatting and utility tools.",
        TRUE, FALSE, L"Sun Junwen", 45000, 0);
    AddResultEntry(out,
        L"MarkdownViewer++",
        L"https://github.com/nea/MarkdownViewerPlusPlus/releases/download/0.8.2/MarkdownViewerPlusPlus-0.8.2-x64.zip",
        L"Markdown preview support.",
        TRUE, FALSE, L"nea", 44500, 0);
    AddResultEntry(out,
        L"Npp Converter",
        L"https://github.com/npp-plugins/converter/releases/download/v4.7/nppConvert.v4.7.x64.zip",
        L"Character conversion utilities.",
        TRUE, FALSE, L"Don HO", 44000, 0);
}

static void SetPluginManagerStatusText(const WCHAR* text)
{
    StringCchCopyW(g_platformStatusText, ARRAYSIZE(g_platformStatusText), text ? text : L"");
    if (!g_hwndPluginMgr || !IsWindow(g_hwndPluginMgr))
        return;
    HWND hStatus = GetDlgItem(g_hwndPluginMgr, IDC_PM_STATUS);
    if (hStatus)
        SetWindowTextW(hStatus, g_platformStatusText);
}

static void StartPlatformFetchAsync(void)
{
    LONG requestId = InterlockedIncrement(&g_platformFetchRequestId);
    PlatformFetchRequest* req = (PlatformFetchRequest*)malloc(sizeof(PlatformFetchRequest));
    if (!req)
    {
        SetPluginManagerStatusText(L"Unable to start plugin catalog fetch.");
        return;
    }
    req->requestId = requestId;

    uintptr_t th = _beginthreadex(NULL, 0, PlatformFetchThreadProc, req, 0, NULL);
    if (!th)
    {
        free(req);
        SetPluginManagerStatusText(L"Unable to start plugin catalog fetch.");
        return;
    }
    CloseHandle((HANDLE)th);
    g_platformFetchInProgress = TRUE;
    SetPluginManagerStatusText(L"Loading popular plugins from official catalogs...");
}

static unsigned __stdcall PlatformFetchThreadProc(void* arg)
{
    PlatformFetchRequest* req = (PlatformFetchRequest*)arg;
    LONG requestId = req ? req->requestId : 0;
    if (req) free(req);
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOL comInitialized = SUCCEEDED(hrCo) || hrCo == S_FALSE;

    PlatformFetchResult* result = (PlatformFetchResult*)malloc(sizeof(PlatformFetchResult));
    if (!result)
    {
        if (comInitialized) CoUninitialize();
        return 0;
    }
    ZeroMemory(result, sizeof(*result));
    result->requestId = requestId;

    char* json = NULL;
    int len = 0;
    BOOL downloadedNpp = FALSE;
    BOOL downloadedOvsx = FALSE;

    if (!IsStaleFetchRequest(requestId) &&
        LoadUrlToBufferA(L"https://raw.githubusercontent.com/notepad-plus-plus/nppPluginList/master/src/pl.x64.json", &json, &len))
    {
        downloadedNpp = TRUE;
        ParseNotepadPluginListJson(json, len, result);
    }
    if (json) { free(json); json = NULL; }

    if (!IsStaleFetchRequest(requestId) &&
        LoadUrlToBufferA(OPENVSX_POPULAR_URL, &json, &len))
    {
        downloadedOvsx = TRUE;
        ParseOpenVsxSearchJson(json, len, result);
    }
    if (json) { free(json); json = NULL; }

    if (result->count == 0)
    {
        result->usedFallback = TRUE;
        AddFallbackPopularNotepadPlugins(result);
    }

    if (result->count > 0)
    {
        WCHAR desc[160];
        if (!downloadedNpp && !downloadedOvsx)
            StringCchCopyW(desc, ARRAYSIZE(desc), L"Loaded fallback popular Notepad plugins.");
        else if (downloadedNpp && downloadedOvsx)
            StringCchPrintfW(desc, ARRAYSIZE(desc), L"Fetched %d plugins from Notepad and Open VSX.", result->count);
        else
            StringCchPrintfW(desc, ARRAYSIZE(desc), L"Fetched %d online plugin entries.", result->count);
        OutputDebugStringW(desc);
    }

    if (comInitialized) CoUninitialize();

    if (IsStaleFetchRequest(requestId) ||
        !g_hwndPluginMgr || !IsWindow(g_hwndPluginMgr) ||
        !PostMessageW(g_hwndPluginMgr, PM_MSG_FETCH_COMPLETE, 0, (LPARAM)result))
    {
        free(result);
    }
    return 0;
}

static void LoadAvailableCatalog(void)
{
    g_availableCount = 0;
    g_platformFetchInProgress = FALSE;

    WCHAR pluginsPath[MAX_PATH];
    if (!EnsurePluginsDirPath(pluginsPath, MAX_PATH))
        return;

    WCHAR catalogPath[MAX_PATH];
    StringCchPrintfW(catalogPath, MAX_PATH, L"%s\\catalog.txt", pluginsPath);
    LoadCatalogFile(catalogPath);
    SortAvailablePlugins();
    SetPluginManagerStatusText(L"Ready.");
}

static int __cdecl CompareAvailablePlugins(const void* a, const void* b)
{
    const AvailablePlugin* pa = (const AvailablePlugin*)a;
    const AvailablePlugin* pb = (const AvailablePlugin*)b;
    if (pa->popularity != pb->popularity)
        return (pb->popularity > pa->popularity) ? 1 : -1;
    if (pa->installable != pb->installable)
        return pb->installable - pa->installable;
    return StrCmpIW(pa->name, pb->name);
}

static void SortAvailablePlugins(void)
{
    if (g_availableCount > 1)
        qsort(g_availablePlugins, (size_t)g_availableCount, sizeof(g_availablePlugins[0]), CompareAvailablePlugins);
}

static void RemoveOnlineAvailableEntries(void)
{
    int dst = 0;
    for (int i = 0; i < g_availableCount; i++)
    {
        if (!g_availablePlugins[i].onlineEntry)
        {
            if (dst != i)
                g_availablePlugins[dst] = g_availablePlugins[i];
            dst++;
        }
    }
    g_availableCount = dst;
}

static void RefreshAvailableInstalledFlags(void)
{
    WCHAR pluginsDir[MAX_PATH] = L"";
    BOOL hasPluginsDir = EnsurePluginsDirPath(pluginsDir, ARRAYSIZE(pluginsDir));

    for (int i = 0; i < g_availableCount; i++)
    {
        g_availablePlugins[i].installed = FALSE;

        if (hasPluginsDir && IsVsixPackageUrlW(g_availablePlugins[i].source))
        {
            g_availablePlugins[i].installed = IsVsixContentInstalled(
                pluginsDir, g_availablePlugins[i].source, g_availablePlugins[i].name);
            if (g_availablePlugins[i].installed)
                continue;
        }

        for (int j = 0; j < g_pluginCount; j++)
        {
            if (StrCmpIW(g_availablePlugins[i].name, g_plugins[j].name) == 0)
            {
                g_availablePlugins[i].installed = TRUE;
                break;
            }
        }
    }
}

static void RemovePluginMenuFromMain(void)
{
    if (!g_hwndMain || !g_hPluginMenu) return;
    HMENU hMain = GetMenu(g_hwndMain);
    if (!hMain) return;

    int count = GetMenuItemCount(hMain);
    for (int i = 0; i < count; i++)
    {
        if (GetSubMenu(hMain, i) == g_hPluginMenu)
        {
            RemoveMenu(hMain, i, MF_BYPOSITION);
            break;
        }
    }
    DestroyMenu(g_hPluginMenu);
    g_hPluginMenu = NULL;
}

static void RebuildPluginMenu(void)
{
    if (!g_hwndMain) return;
    HMENU hMain = GetMenu(g_hwndMain);
    if (!hMain) return;

    RemovePluginMenuFromMain();
    PluginManager_CreateMenu(hMain);
    DrawMenuBar(g_hwndMain);
}

static int GetSelectedListIndex(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_PM_LIST);
    if (!hList) return -1;
    return ListView_GetNextItem(hList, -1, LVNI_SELECTED);
}

static void UpdatePluginManagerButtons(void)
{
    if (!g_hwndPluginMgr) return;
    int sel = GetSelectedListIndex(g_hwndPluginMgr);
    BOOL canInstall = FALSE, canToggle = FALSE, canUninstall = FALSE;

    if (sel >= 0 && sel < g_uiRowCount)
    {
        PluginMgrRow row = g_uiRows[sel];
        if (row.type == PM_ROW_INSTALLED)
        {
            canToggle = TRUE;
            canUninstall = TRUE;
        }
        else if (row.type == PM_ROW_AVAILABLE)
        {
            if (row.index >= 0 && row.index < g_availableCount &&
                !g_availablePlugins[row.index].installed &&
                g_availablePlugins[row.index].installable)
            {
                canInstall = TRUE;
            }
        }
    }

    EnableWindow(GetDlgItem(g_hwndPluginMgr, IDC_PM_INSTALL), canInstall);
    EnableWindow(GetDlgItem(g_hwndPluginMgr, IDC_PM_TOGGLE), canToggle);
    EnableWindow(GetDlgItem(g_hwndPluginMgr, IDC_PM_UNINSTALL), canUninstall);
}

static BOOL MatchSearch(const WCHAR* haystack, const WCHAR* needle)
{
    if (!needle || !needle[0]) return TRUE;
    if (!haystack) return FALSE;
    return StrStrIW(haystack, needle) != NULL;
}

static void FormatCountW(int value, WCHAR* out, int cchOut)
{
    if (!out || cchOut <= 0) return;
    out[0] = L'\0';
    WCHAR raw[32];
    StringCchPrintfW(raw, ARRAYSIZE(raw), L"%d", value);

    int rawLen = (int)wcslen(raw);
    int commaCount = (rawLen > 0) ? (rawLen - 1) / 3 : 0;
    int outLen = rawLen + commaCount;
    if (outLen >= cchOut) outLen = cchOut - 1;
    out[outLen] = L'\0';

    int ri = rawLen - 1;
    int oi = outLen - 1;
    int digits = 0;
    while (ri >= 0 && oi >= 0)
    {
        out[oi--] = raw[ri--];
        digits++;
        if (digits == 3 && ri >= 0 && oi >= 0)
        {
            out[oi--] = L',';
            digits = 0;
        }
    }
}

static void RefreshPluginManagerList(void)
{
    if (!g_hwndPluginMgr) return;

    HWND hList = GetDlgItem(g_hwndPluginMgr, IDC_PM_LIST);
    HWND hSearch = GetDlgItem(g_hwndPluginMgr, IDC_PM_SEARCH);
    if (!hList || !hSearch) return;

    WCHAR filter[256];
    GetWindowTextW(hSearch, filter, ARRAYSIZE(filter));

    ListView_DeleteAllItems(hList);
    g_uiRowCount = 0;
    RefreshAvailableInstalledFlags();

    for (int i = 0; i < g_pluginCount; i++)
    {
        PluginEntry* p = &g_plugins[i];
        WCHAR name[256];
        WCHAR status[96];
        WCHAR searchable[512];
        StringCchCopyW(name, ARRAYSIZE(name), p->name[0] ? p->name : L"(unnamed)");
        StringCchPrintfW(status, ARRAYSIZE(status), L"%s%s",
            p->enabled ? L"Enabled" : L"Disabled",
            p->loaded ? L", Loaded" : L", Not loaded");
        StringCchPrintfW(searchable, ARRAYSIZE(searchable), L"%s %s installed", name, status);
        if (!MatchSearch(searchable, filter))
            continue;

        WCHAR author[] = L"-";
        WCHAR installs[] = L"-";
        WCHAR rating[] = L"-";
        WCHAR action[] = L"";

        LVITEMW lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = ListView_GetItemCount(hList);
        lvi.iSubItem = PM_COL_NAME;
        lvi.pszText = name;
        int idx = ListView_InsertItem(hList, &lvi);
        if (idx >= 0 && g_uiRowCount < (int)ARRAYSIZE(g_uiRows))
        {
            ListView_SetItemText(hList, idx, PM_COL_AUTHOR, author);
            ListView_SetItemText(hList, idx, PM_COL_INSTALLS, installs);
            ListView_SetItemText(hList, idx, PM_COL_RATING, rating);
            ListView_SetItemText(hList, idx, PM_COL_STATUS, status);
            ListView_SetItemText(hList, idx, PM_COL_ACTION, action);

            g_uiRows[idx].type = PM_ROW_INSTALLED;
            g_uiRows[idx].index = i;
            g_uiRowCount = max(g_uiRowCount, idx + 1);
        }
    }

    for (int i = 0; i < g_availableCount; i++)
    {
        AvailablePlugin* ap = &g_availablePlugins[i];
        BOOL matches = MatchSearch(ap->name, filter) ||
                       MatchSearch(ap->author, filter) ||
                       MatchSearch(ap->description, filter);
        if (!matches)
            continue;

        WCHAR name[256];
        WCHAR author[128];
        WCHAR installs[32];
        WCHAR rating[32];
        WCHAR status[96];
        WCHAR action[32];
        StringCchCopyW(name, ARRAYSIZE(name), ap->name[0] ? ap->name : L"(unnamed)");
        StringCchCopyW(author, ARRAYSIZE(author), ap->author[0] ? ap->author : L"-");
        if (ap->popularity > 0)
            FormatCountW(ap->popularity, installs, ARRAYSIZE(installs));
        else
            StringCchCopyW(installs, ARRAYSIZE(installs), L"-");

        if (ap->ratingPermille > 0)
        {
            int whole = ap->ratingPermille / 1000;
            int frac = (ap->ratingPermille % 1000) / 100;
            StringCchPrintfW(rating, ARRAYSIZE(rating), L"%d.%d", whole, frac);
        }
        else
        {
            StringCchCopyW(rating, ARRAYSIZE(rating), L"-");
        }

        if (ap->installed)
        {
            if (IsVsixPackageUrlW(ap->source))
                StringCchCopyW(status, ARRAYSIZE(status), L"Installed (content)");
            else
                StringCchCopyW(status, ARRAYSIZE(status), L"Installed");
            StringCchCopyW(action, ARRAYSIZE(action), L"");
        }
        else if (ap->installable)
        {
            if (IsVsixPackageUrlW(ap->source))
                StringCchCopyW(status, ARRAYSIZE(status), L"Available (content)");
            else
                StringCchCopyW(status, ARRAYSIZE(status), L"Available");
            StringCchCopyW(action, ARRAYSIZE(action), L"Install");
        }
        else if (ap->referenceOnly)
        {
            StringCchCopyW(status, ARRAYSIZE(status), L"Reference");
            StringCchCopyW(action, ARRAYSIZE(action), L"");
        }
        else
        {
            StringCchCopyW(status, ARRAYSIZE(status), L"-");
            StringCchCopyW(action, ARRAYSIZE(action), L"");
        }

        LVITEMW lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = ListView_GetItemCount(hList);
        lvi.iSubItem = PM_COL_NAME;
        lvi.pszText = name;
        int idx = ListView_InsertItem(hList, &lvi);
        if (idx >= 0 && g_uiRowCount < (int)ARRAYSIZE(g_uiRows))
        {
            ListView_SetItemText(hList, idx, PM_COL_AUTHOR, author);
            ListView_SetItemText(hList, idx, PM_COL_INSTALLS, installs);
            ListView_SetItemText(hList, idx, PM_COL_RATING, rating);
            ListView_SetItemText(hList, idx, PM_COL_STATUS, status);
            ListView_SetItemText(hList, idx, PM_COL_ACTION, action);

            g_uiRows[idx].type = PM_ROW_AVAILABLE;
            g_uiRows[idx].index = i;
            g_uiRowCount = max(g_uiRowCount, idx + 1);
        }
    }

    if (ListView_GetItemCount(hList) > 0)
    {
        ListView_SetItemState(hList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    else
    {
        LVITEMW lvi;
        WCHAR message[160];
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = 0;
        lvi.iSubItem = PM_COL_NAME;
        if (g_platformFetchInProgress)
            StringCchCopyW(message, ARRAYSIZE(message), L"Loading plugins from official catalogs...");
        else
            StringCchCopyW(message, ARRAYSIZE(message), L"No plugins match the current filter.");
        lvi.pszText = message;
        ListView_InsertItem(hList, &lvi);
    }
    UpdatePluginManagerButtons();
}

static void SetupPluginManagerListColumns(HWND hList)
{
    if (!hList) return;

    while (ListView_DeleteColumn(hList, 0)) {}

    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;

    col.fmt = LVCFMT_LEFT; col.iSubItem = PM_COL_NAME; col.pszText = L"Plugin"; col.cx = 200;
    ListView_InsertColumn(hList, PM_COL_NAME, &col);
    col.fmt = LVCFMT_LEFT; col.iSubItem = PM_COL_AUTHOR; col.pszText = L"Author"; col.cx = 150;
    ListView_InsertColumn(hList, PM_COL_AUTHOR, &col);
    col.fmt = LVCFMT_RIGHT; col.iSubItem = PM_COL_INSTALLS; col.pszText = L"Installs"; col.cx = 100;
    ListView_InsertColumn(hList, PM_COL_INSTALLS, &col);
    col.fmt = LVCFMT_RIGHT; col.iSubItem = PM_COL_RATING; col.pszText = L"Rating"; col.cx = 70;
    ListView_InsertColumn(hList, PM_COL_RATING, &col);
    col.fmt = LVCFMT_LEFT; col.iSubItem = PM_COL_STATUS; col.pszText = L"Status"; col.cx = 140;
    ListView_InsertColumn(hList, PM_COL_STATUS, &col);
    col.fmt = LVCFMT_CENTER; col.iSubItem = PM_COL_ACTION; col.pszText = L"Action"; col.cx = 96;
    ListView_InsertColumn(hList, PM_COL_ACTION, &col);
}

static void ResizePluginManagerListColumns(HWND hList)
{
    if (!hList) return;
    RECT rc;
    GetClientRect(hList, &rc);
    int totalW = max(220, rc.right - rc.left - 4);

    int actionW = 96;
    int ratingW = 70;
    int installsW = 100;
    int statusW = 140;
    int authorW = 150;
    int nameW = totalW - (actionW + ratingW + installsW + statusW + authorW);

    if (nameW < 140)
    {
        int need = 140 - nameW;
        int shrink = min(need, max(0, authorW - 100));
        authorW -= shrink; need -= shrink;
        shrink = min(need, max(0, statusW - 100));
        statusW -= shrink; need -= shrink;
        shrink = min(need, max(0, installsW - 80));
        installsW -= shrink; need -= shrink;
        nameW = totalW - (actionW + ratingW + installsW + statusW + authorW);
        if (nameW < 100) nameW = 100;
    }

    ListView_SetColumnWidth(hList, PM_COL_NAME, nameW);
    ListView_SetColumnWidth(hList, PM_COL_AUTHOR, authorW);
    ListView_SetColumnWidth(hList, PM_COL_INSTALLS, installsW);
    ListView_SetColumnWidth(hList, PM_COL_RATING, ratingW);
    ListView_SetColumnWidth(hList, PM_COL_STATUS, statusW);
    ListView_SetColumnWidth(hList, PM_COL_ACTION, actionW);
}

static LRESULT HandlePluginListCustomDraw(NMLVCUSTOMDRAW* pcd)
{
    if (!pcd) return CDRF_DODEFAULT;

    switch (pcd->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case (CDDS_ITEMPREPAINT | CDDS_SUBITEM):
    {
        if (pcd->iSubItem != PM_COL_ACTION)
            return CDRF_DODEFAULT;

        HWND hList = (HWND)pcd->nmcd.hdr.hwndFrom;
        int row = (int)pcd->nmcd.dwItemSpec;
        if (!hList || row < 0)
            return CDRF_DODEFAULT;

        WCHAR action[32];
        action[0] = L'\0';
        ListView_GetItemText(hList, row, PM_COL_ACTION, action, ARRAYSIZE(action));
        if (!action[0])
            return CDRF_DODEFAULT;

        BOOL selected = (ListView_GetItemState(hList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
        RECT cell = pcd->nmcd.rc;
        RECT buttonRc = cell;
        buttonRc.left += 6;
        buttonRc.right -= 6;
        buttonRc.top += 3;
        buttonRc.bottom -= 3;

        const DarkModeColors* colors = DarkMode_GetColors();
        COLORREF rowBg = GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
        COLORREF btnFill = RGB(228, 239, 252);
        COLORREF btnBorder = RGB(126, 170, 220);
        COLORREF btnText = RGB(9, 79, 142);
        if (DarkMode_IsEnabled() && colors)
        {
            rowBg = selected ? colors->clrSelection : colors->clrSurface;
            btnFill = selected ? RGB(53, 94, 141) : RGB(41, 73, 111);
            btnBorder = selected ? RGB(103, 154, 212) : RGB(82, 130, 184);
            btnText = RGB(235, 243, 252);
        }

        HBRUSH hRow = CreateSolidBrush(rowBg);
        FillRect(pcd->nmcd.hdc, &cell, hRow);
        DeleteObject(hRow);

        if (buttonRc.right > buttonRc.left + 10 && buttonRc.bottom > buttonRc.top + 8)
        {
            HBRUSH hBtn = CreateSolidBrush(btnFill);
            HPEN hPen = CreatePen(PS_SOLID, 1, btnBorder);
            HGDIOBJ oldPen = SelectObject(pcd->nmcd.hdc, hPen);
            HGDIOBJ oldBrush = SelectObject(pcd->nmcd.hdc, hBtn);
            RoundRect(pcd->nmcd.hdc, buttonRc.left, buttonRc.top, buttonRc.right, buttonRc.bottom, 10, 10);
            SelectObject(pcd->nmcd.hdc, oldPen);
            SelectObject(pcd->nmcd.hdc, oldBrush);
            DeleteObject(hPen);
            DeleteObject(hBtn);
        }

        SetBkMode(pcd->nmcd.hdc, TRANSPARENT);
        SetTextColor(pcd->nmcd.hdc, btnText);
        DrawTextW(pcd->nmcd.hdc, action, -1, &buttonRc,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

static void LayoutPluginManagerControls(HWND hwnd)
{
    if (!hwnd) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const int m = 12;
    const int gap = 6;
    const int labelW = 50;
    const int editH = 24;
    const int btnH = 28;
    const int rowGap = 8;

    HWND hSearch = GetDlgItem(hwnd, IDC_PM_SEARCH);
    HWND hList = GetDlgItem(hwnd, IDC_PM_LIST);
    HWND hAdd = GetDlgItem(hwnd, IDC_PM_ADD);
    HWND hAddUrl = GetDlgItem(hwnd, IDC_PM_ADDURL);
    HWND hInstall = GetDlgItem(hwnd, IDC_PM_INSTALL);
    HWND hToggle = GetDlgItem(hwnd, IDC_PM_TOGGLE);
    HWND hUninstall = GetDlgItem(hwnd, IDC_PM_UNINSTALL);
    HWND hReload = GetDlgItem(hwnd, IDC_PM_RELOAD);
    HWND hStatus = GetDlgItem(hwnd, IDC_PM_STATUS);
    HWND hClose = GetDlgItem(hwnd, IDC_PM_CLOSE);

    int searchY = m;
    int searchX = m + labelW + gap;
    int searchW = max(140, w - searchX - m);
    MoveWindow(hSearch, searchX, searchY - 2, searchW, editH, TRUE);

    int secondRowY = h - m - btnH;
    int firstRowY = secondRowY - rowGap - btnH;
    int statusY = searchY + editH + 4;
    MoveWindow(hStatus, m, statusY, max(140, w - 2 * m), 20, TRUE);

    int listY = statusY + 22;
    int listH = max(120, firstRowY - listY - 8);
    MoveWindow(hList, m, listY, max(200, w - 2 * m), listH, TRUE);
    ResizePluginManagerListColumns(hList);

    int availableW = w - 2 * m;
    int colGap = 6;
    int colW = (availableW - 5 * colGap) / 6;
    if (colW < 72) colW = 72;

    int x = m;
    MoveWindow(hAdd, x, firstRowY, colW, btnH, TRUE); x += colW + colGap;
    MoveWindow(hAddUrl, x, firstRowY, colW, btnH, TRUE); x += colW + colGap;
    MoveWindow(hInstall, x, firstRowY, colW, btnH, TRUE); x += colW + colGap;
    MoveWindow(hToggle, x, firstRowY, colW, btnH, TRUE); x += colW + colGap;
    MoveWindow(hUninstall, x, firstRowY, colW, btnH, TRUE);

    int closeW = 90;
    int reloadW = 80;
    int closeX = max(m, w - m - closeW);
    int reloadX = max(m, closeX - colGap - reloadW);
    MoveWindow(hReload, reloadX, secondRowY, reloadW, btnH, TRUE);
    MoveWindow(hClose, closeX, secondRowY, closeW, btnH, TRUE);
}

static void ApplyPluginManagerTheme(HWND hwnd)
{
    if (!hwnd) return;
    DarkMode_ApplyToDialog(hwnd);
    SetWindowTheme(hwnd, DarkMode_IsEnabled() ? L"DarkMode_Explorer" : NULL, NULL);

    if (DarkMode_IsEnabled())
    {
        HWND hSearch = GetDlgItem(hwnd, IDC_PM_SEARCH);
        HWND hList = GetDlgItem(hwnd, IDC_PM_LIST);
        if (hSearch) SetWindowTheme(hSearch, L"DarkMode_Explorer", NULL);
        if (hList) SetWindowTheme(hList, L"DarkMode_Explorer", NULL);

        const int ids[] = {
            IDC_PM_ADD, IDC_PM_ADDURL, IDC_PM_INSTALL, IDC_PM_TOGGLE,
            IDC_PM_UNINSTALL, IDC_PM_RELOAD, IDC_PM_CLOSE
        };
        for (int i = 0; i < (int)ARRAYSIZE(ids); i++) {
            HWND hBtn = GetDlgItem(hwnd, ids[i]);
            if (hBtn) SetWindowTheme(hBtn, L"DarkMode_Explorer", NULL);
        }
    }
}

static HBRUSH GetPluginMgrBackgroundBrush(void)
{
    if (DarkMode_IsEnabled())
    {
        HBRUSH h = DarkMode_GetBackgroundBrush();
        if (h) return h;
    }
    return (HBRUSH)(COLOR_WINDOW + 1);
}

static void RegisterPluginMgrClass(void)
{
    if (g_uiClassRegistered) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PluginManagerWndProc;
    wc.hInstance = (HINSTANCE)GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = PLUGIN_MGR_CLASS;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);
    g_uiClassRegistered = TRUE;
}

static BOOL CreateTempFilePath(const WCHAR* extension, WCHAR* outPath, int cchOut)
{
    if (!outPath || cchOut <= 0) return FALSE;

    WCHAR tempDir[MAX_PATH];
    WCHAR tempFile[MAX_PATH];
    if (!GetTempPathW(ARRAYSIZE(tempDir), tempDir))
        return FALSE;
    if (!GetTempFileNameW(tempDir, L"bko", 0, tempFile))
        return FALSE;

    DeleteFileW(tempFile);
    if (extension && extension[0])
        StringCchPrintfW(outPath, cchOut, L"%s%s", tempFile, extension);
    else
        StringCchCopyW(outPath, cchOut, tempFile);
    return TRUE;
}

static BOOL DownloadUrlToPath(const WCHAR* url, const WCHAR* outPath)
{
    if (!url || !outPath) return FALSE;
    return SUCCEEDED(URLDownloadToFileW(NULL, url, outPath, 0, NULL));
}

static BOOL CopyPluginFileWithPrompt(const WCHAR* srcPath, const WCHAR* dstPath)
{
    if (!srcPath || !dstPath) return FALSE;

    if (PathFileExistsW(dstPath))
    {
        int res = MessageBoxW(g_hwndPluginMgr,
            L"A plugin with this filename already exists. Overwrite?",
            L"Plugins", MB_YESNO | MB_ICONQUESTION);
        if (res != IDYES)
            return FALSE;
        SetFileAttributesW(dstPath, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(dstPath);
    }

    return CopyFileW(srcPath, dstPath, FALSE);
}

static void NormalizePathForTar(const WCHAR* srcPath, WCHAR* outPath, int cchOut)
{
    if (!outPath || cchOut <= 0) return;
    outPath[0] = L'\0';
    if (!srcPath || !srcPath[0]) return;

    StringCchCopyW(outPath, cchOut, srcPath);
    for (int i = 0; outPath[i]; i++)
    {
        if (outPath[i] == L'\\')
            outPath[i] = L'/';
    }
}

static BOOL ExtractArchiveToDirectory(const WCHAR* archivePath, const WCHAR* extractDir)
{
    if (!archivePath || !archivePath[0] || !extractDir || !extractDir[0])
        return FALSE;
    if (!PathFileExistsW(archivePath))
        return FALSE;
    if (!PathIsDirectoryW(extractDir))
        return FALSE;

    WCHAR tarExe[MAX_PATH];
    UINT sysLen = GetSystemDirectoryW(tarExe, ARRAYSIZE(tarExe));
    if (sysLen == 0 || sysLen > ARRAYSIZE(tarExe) - 10)
        return FALSE;
    StringCchCatW(tarExe, ARRAYSIZE(tarExe), L"\\tar.exe");
    if (!PathFileExistsW(tarExe))
        return FALSE;

    WCHAR archiveArg[MAX_PATH];
    WCHAR extractArg[MAX_PATH];
    NormalizePathForTar(archivePath, archiveArg, ARRAYSIZE(archiveArg));
    NormalizePathForTar(extractDir, extractArg, ARRAYSIZE(extractArg));

    WCHAR cmd[4096];
    StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"tar.exe -xf \"%s\" -C \"%s\"", archiveArg, extractArg);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(tarExe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;

    DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return FALSE;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

static BOOL BuildPackageStemFromSource(const WCHAR* sourceOrPath, const WCHAR* fallbackName, WCHAR* outStem, int cchOut)
{
    if (!outStem || cchOut <= 0)
        return FALSE;
    outStem[0] = L'\0';

    WCHAR clean[1024] = L"";
    if (sourceOrPath && sourceOrPath[0])
    {
        StringCchCopyW(clean, ARRAYSIZE(clean), sourceOrPath);
        WCHAR* cut = wcspbrk(clean, L"?&#");
        if (cut) *cut = L'\0';
    }

    WCHAR stemRaw[256] = L"";
    const WCHAR* fileName = clean[0] ? PathFindFileNameW(clean) : L"";
    if (fileName && fileName[0] && wcscmp(fileName, clean) != 0)
    {
        StringCchCopyW(stemRaw, ARRAYSIZE(stemRaw), fileName);
    }
    else if (fallbackName && fallbackName[0])
    {
        StringCchCopyW(stemRaw, ARRAYSIZE(stemRaw), fallbackName);
    }
    else
    {
        StringCchCopyW(stemRaw, ARRAYSIZE(stemRaw), L"extension");
    }

    WCHAR* ext = (WCHAR*)PathFindExtensionW(stemRaw);
    if (ext && ext != stemRaw)
        *ext = L'\0';

    int w = 0;
    BOOL prevUnderscore = FALSE;
    for (int i = 0; stemRaw[i] && w < cchOut - 1; i++)
    {
        WCHAR ch = stemRaw[i];
        BOOL keep = iswalnum((wint_t)ch) || ch == L'.' || ch == L'-' || ch == L'_';
        if (!keep)
            ch = L'_';
        if (ch == L'_')
        {
            if (prevUnderscore)
                continue;
            prevUnderscore = TRUE;
        }
        else
        {
            prevUnderscore = FALSE;
        }
        outStem[w++] = ch;
    }
    outStem[w] = L'\0';

    if (!outStem[0])
        StringCchCopyW(outStem, cchOut, L"extension");
    return TRUE;
}

static BOOL InstallVsixContentPackage(const WCHAR* archivePath, const WCHAR* pluginsDir,
                                      const WCHAR* sourceOrPath, const WCHAR* fallbackName,
                                      WCHAR* outInstalledPath, int cchOut)
{
    if (!archivePath || !archivePath[0] || !pluginsDir || !pluginsDir[0])
        return FALSE;

    WCHAR rootDir[MAX_PATH];
    StringCchPrintfW(rootDir, ARRAYSIZE(rootDir), L"%s\\%s", pluginsDir, VSIX_CONTENT_FOLDER);
    if (!PathIsDirectoryW(rootDir) && !CreateDirectoryW(rootDir, NULL))
        return FALSE;

    WCHAR stem[160];
    BuildPackageStemFromSource(sourceOrPath, fallbackName, stem, ARRAYSIZE(stem));

    WCHAR targetDir[MAX_PATH];
    StringCchPrintfW(targetDir, ARRAYSIZE(targetDir), L"%s\\%s", rootDir, stem);

    if (PathIsDirectoryW(targetDir))
    {
        int res = MessageBoxW(g_hwndPluginMgr,
            L"This VS Code extension package is already installed. Overwrite?",
            L"Plugins", MB_YESNO | MB_ICONQUESTION);
        if (res != IDYES)
            return FALSE;
        DeleteDirectoryTree(targetDir);
    }

    if (!CreateDirectoryW(targetDir, NULL))
        return FALSE;

    if (!ExtractArchiveToDirectory(archivePath, targetDir))
    {
        DeleteDirectoryTree(targetDir);
        return FALSE;
    }

    WCHAR pkg1[MAX_PATH];
    WCHAR pkg2[MAX_PATH];
    StringCchPrintfW(pkg1, ARRAYSIZE(pkg1), L"%s\\extension\\package.json", targetDir);
    StringCchPrintfW(pkg2, ARRAYSIZE(pkg2), L"%s\\package.json", targetDir);
    if (!PathFileExistsW(pkg1) && !PathFileExistsW(pkg2))
    {
        DeleteDirectoryTree(targetDir);
        return FALSE;
    }

    if (outInstalledPath && cchOut > 0)
        StringCchCopyW(outInstalledPath, cchOut, targetDir);
    return TRUE;
}

static BOOL IsVsixContentInstalled(const WCHAR* pluginsDir, const WCHAR* sourceOrPath, const WCHAR* fallbackName)
{
    if (!pluginsDir || !pluginsDir[0])
        return FALSE;

    WCHAR stem[160];
    BuildPackageStemFromSource(sourceOrPath, fallbackName, stem, ARRAYSIZE(stem));
    WCHAR dir[MAX_PATH];
    StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s\\%s", pluginsDir, VSIX_CONTENT_FOLDER, stem);
    return PathIsDirectoryW(dir);
}

static BOOL FindFirstDllRecursive(const WCHAR* dir, WCHAR* outPath, int cchOut)
{
    if (!dir || !outPath || cchOut <= 0) return FALSE;

    WCHAR search[MAX_PATH];
    StringCchPrintfW(search, ARRAYSIZE(search), L"%s\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return FALSE;

    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        WCHAR child[MAX_PATH];
        StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (FindFirstDllRecursive(child, outPath, cchOut))
            {
                FindClose(hFind);
                return TRUE;
            }
        }
        else
        {
            const WCHAR* ext = PathFindExtensionW(fd.cFileName);
            if (ext && _wcsicmp(ext, L".dll") == 0)
            {
                StringCchCopyW(outPath, cchOut, child);
                FindClose(hFind);
                return TRUE;
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return FALSE;
}

static void DeleteDirectoryTree(const WCHAR* dir)
{
    if (!dir || !PathIsDirectoryW(dir))
        return;

    WCHAR search[MAX_PATH];
    StringCchPrintfW(search, ARRAYSIZE(search), L"%s\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        WCHAR child[MAX_PATH];
        StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            DeleteDirectoryTree(child);
            RemoveDirectoryW(child);
        }
        else
        {
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(child);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    RemoveDirectoryW(dir);
}

static BOOL InstallArchiveToPluginsFolder(const WCHAR* archivePath, const WCHAR* pluginsDir, WCHAR* outInstalledPath, int cchOut)
{
    if (!archivePath || !pluginsDir) return FALSE;

    WCHAR tempBase[MAX_PATH];
    if (!GetTempPathW(ARRAYSIZE(tempBase), tempBase))
        return FALSE;

    WCHAR extractDir[MAX_PATH];
    StringCchPrintfW(extractDir, ARRAYSIZE(extractDir), L"%sBikodePlugin_%lu_%lu",
                     tempBase, (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    if (!CreateDirectoryW(extractDir, NULL))
        return FALSE;

    if (!ExtractArchiveToDirectory(archivePath, extractDir))
    {
        DeleteDirectoryTree(extractDir);
        return FALSE;
    }

    WCHAR dllPath[MAX_PATH];
    if (!FindFirstDllRecursive(extractDir, dllPath, ARRAYSIZE(dllPath)))
    {
        DeleteDirectoryTree(extractDir);
        return FALSE;
    }

    WCHAR destPath[MAX_PATH];
    StringCchPrintfW(destPath, ARRAYSIZE(destPath), L"%s\\%s", pluginsDir, PathFindFileNameW(dllPath));
    if (!CopyPluginFileWithPrompt(dllPath, destPath))
    {
        DeleteDirectoryTree(extractDir);
        return FALSE;
    }

    if (outInstalledPath && cchOut > 0)
        StringCchCopyW(outInstalledPath, cchOut, destPath);

    DeleteDirectoryTree(extractDir);
    return TRUE;
}

static void InstallFromAvailable(int availableIndex)
{
    if (availableIndex < 0 || availableIndex >= g_availableCount) return;
    AvailablePlugin* ap = &g_availablePlugins[availableIndex];
    if (ap->installed) return;
    if (!ap->installable)
    {
        MessageBoxW(g_hwndPluginMgr,
            L"This entry is reference-only.",
            L"Plugins", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WCHAR pluginsDir[MAX_PATH];
    if (!EnsurePluginsDirPath(pluginsDir, MAX_PATH))
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to resolve plugins folder.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    BOOL ok = FALSE;
    WCHAR cleanedSource[1024];
    StringCchCopyW(cleanedSource, ARRAYSIZE(cleanedSource), ap->source);
    WCHAR* cut = wcspbrk(cleanedSource, L"?&#");
    if (cut) *cut = L'\0';

    if (IsDirectPluginBinaryUrlW(cleanedSource))
    {
        WCHAR destName[MAX_PATH];
        StringCchCopyW(destName, ARRAYSIZE(destName), PathFindFileNameW(cleanedSource));
        if (!destName[0] || wcscmp(destName, cleanedSource) == 0)
            StringCchPrintfW(destName, ARRAYSIZE(destName), L"%s.dll", ap->name);
        if (_wcsicmp(PathFindExtensionW(destName), L".dll") != 0)
            StringCchCatW(destName, ARRAYSIZE(destName), L".dll");

        WCHAR destPath[MAX_PATH];
        StringCchPrintfW(destPath, ARRAYSIZE(destPath), L"%s\\%s", pluginsDir, destName);

        WCHAR tempDownload[MAX_PATH];
        const WCHAR* sourcePath = ap->source;
        BOOL isTemp = FALSE;
        if (IsHttpUrlW(ap->source))
        {
            if (!CreateTempFilePath(L".dll", tempDownload, ARRAYSIZE(tempDownload)) ||
                !DownloadUrlToPath(ap->source, tempDownload))
            {
                MessageBoxW(g_hwndPluginMgr, L"Failed to download plugin package.", L"Plugins", MB_OK | MB_ICONERROR);
                return;
            }
            sourcePath = tempDownload;
            isTemp = TRUE;
        }

        ok = CopyPluginFileWithPrompt(sourcePath, destPath);
        if (isTemp) DeleteFileW(tempDownload);
    }
    else if (IsPluginArchiveUrlW(cleanedSource))
    {
        WCHAR archivePath[MAX_PATH];
        const WCHAR* sourcePath = ap->source;
        BOOL isTemp = FALSE;

        if (IsHttpUrlW(ap->source))
        {
            WCHAR ext[16] = L".zip";
            const WCHAR* srcExt = PathFindExtensionW(cleanedSource);
            if (srcExt && srcExt[0])
                StringCchCopyW(ext, ARRAYSIZE(ext), srcExt);

            if (!CreateTempFilePath(ext, archivePath, ARRAYSIZE(archivePath)) ||
                !DownloadUrlToPath(ap->source, archivePath))
            {
                MessageBoxW(g_hwndPluginMgr, L"Failed to download plugin package.", L"Plugins", MB_OK | MB_ICONERROR);
                return;
            }
            sourcePath = archivePath;
            isTemp = TRUE;
        }

        WCHAR installedPath[MAX_PATH];
        if (IsVsixPackageUrlW(cleanedSource))
        {
            ok = InstallVsixContentPackage(sourcePath, pluginsDir, ap->source, ap->name,
                                           installedPath, ARRAYSIZE(installedPath));
        }
        else
        {
            ok = InstallArchiveToPluginsFolder(sourcePath, pluginsDir, installedPath, ARRAYSIZE(installedPath));
        }
        if (isTemp) DeleteFileW(archivePath);
    }
    else
    {
        MessageBoxW(g_hwndPluginMgr,
            L"Unsupported package type. Only .dll, .zip and .vsix packages are installable.",
            L"Plugins", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!ok)
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to install selected plugin.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    PluginManager_ReloadAll();
    RebuildPluginMenu();
    if (IsVsixPackageUrlW(cleanedSource))
    {
        MessageBoxW(g_hwndPluginMgr,
            L"VS Code extension package installed as a lightweight content plugin.",
            L"Plugins", MB_OK | MB_ICONINFORMATION);
    }
    RefreshPluginManagerList();
}

static void AddPluginFromFile(void)
{
    WCHAR pluginsDir[MAX_PATH];
    if (!EnsurePluginsDirPath(pluginsDir, MAX_PATH))
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to resolve plugins folder.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    WCHAR srcPath[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndPluginMgr;
    ofn.lpstrFile = srcPath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Plugin Packages (*.dll;*.zip;*.vsix)\0*.dll;*.zip;*.vsix\0All Files\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return;

    BOOL ok = FALSE;
    const WCHAR* ext = PathFindExtensionW(srcPath);
    if (ext && _wcsicmp(ext, L".dll") == 0)
    {
        WCHAR destPath[MAX_PATH];
        StringCchPrintfW(destPath, MAX_PATH, L"%s\\%s", pluginsDir, PathFindFileNameW(srcPath));
        ok = CopyPluginFileWithPrompt(srcPath, destPath);
        if (!ok)
        {
            MessageBoxW(g_hwndPluginMgr, L"Failed to copy plugin DLL.", L"Plugins", MB_OK | MB_ICONERROR);
            return;
        }
    }
    else if (ext && (_wcsicmp(ext, L".zip") == 0 || _wcsicmp(ext, L".vsix") == 0))
    {
        WCHAR installedPath[MAX_PATH];
        if (_wcsicmp(ext, L".vsix") == 0)
        {
            ok = InstallVsixContentPackage(srcPath, pluginsDir, srcPath, PathFindFileNameW(srcPath),
                                           installedPath, ARRAYSIZE(installedPath));
            if (!ok)
            {
                MessageBoxW(g_hwndPluginMgr, L"Failed to install VS Code extension package.", L"Plugins", MB_OK | MB_ICONERROR);
                return;
            }
            MessageBoxW(g_hwndPluginMgr,
                L"VS Code extension package installed as a lightweight content plugin.",
                L"Plugins", MB_OK | MB_ICONINFORMATION);
        }
        else
        {
            ok = InstallArchiveToPluginsFolder(srcPath, pluginsDir, installedPath, ARRAYSIZE(installedPath));
            if (!ok)
            {
                MessageBoxW(g_hwndPluginMgr, L"Failed to install plugin package.", L"Plugins", MB_OK | MB_ICONERROR);
                return;
            }
        }
    }
    else
    {
        MessageBoxW(g_hwndPluginMgr,
            L"Unsupported package type. Use .dll, .zip, or .vsix.",
            L"Plugins", MB_OK | MB_ICONWARNING);
        return;
    }

    PluginManager_ReloadAll();
    RebuildPluginMenu();
    RefreshPluginManagerList();
}

static void AddPluginFromUrl(void)
{
    WCHAR url[1024] = L"";
    // Lightweight input approach: read URL from clipboard.
    if (OpenClipboard(g_hwndPluginMgr))
    {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData)
        {
            LPCWSTR clip = (LPCWSTR)GlobalLock(hData);
            if (clip && IsHttpUrlW(clip))
                StringCchCopyW(url, ARRAYSIZE(url), clip);
            if (clip) GlobalUnlock(hData);
        }
        CloseClipboard();
    }

    if (!url[0])
    {
        MessageBoxW(g_hwndPluginMgr,
            L"Copy a plugin package URL (.dll, .zip or .vsix) to clipboard, then click 'Add URL...' again.",
            L"Plugins", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WCHAR pluginsDir[MAX_PATH];
    if (!EnsurePluginsDirPath(pluginsDir, MAX_PATH))
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to resolve plugins folder.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    WCHAR cleaned[1024];
    StringCchCopyW(cleaned, ARRAYSIZE(cleaned), url);
    WCHAR* cut = wcspbrk(cleaned, L"?&#");
    if (cut) *cut = L'\0';

    BOOL ok = FALSE;
    if (IsDirectPluginBinaryUrlW(cleaned))
    {
        WCHAR fileName[MAX_PATH];
        StringCchCopyW(fileName, ARRAYSIZE(fileName), PathFindFileNameW(cleaned));
        if (!fileName[0])
            StringCchCopyW(fileName, ARRAYSIZE(fileName), L"downloaded_plugin.dll");

        WCHAR tempDll[MAX_PATH];
        if (!CreateTempFilePath(L".dll", tempDll, ARRAYSIZE(tempDll)) ||
            !DownloadUrlToPath(url, tempDll))
        {
            MessageBoxW(g_hwndPluginMgr, L"Failed to download plugin package.", L"Plugins", MB_OK | MB_ICONERROR);
            return;
        }

        WCHAR destPath[MAX_PATH];
        StringCchPrintfW(destPath, ARRAYSIZE(destPath), L"%s\\%s", pluginsDir, fileName);
        ok = CopyPluginFileWithPrompt(tempDll, destPath);
        DeleteFileW(tempDll);
    }
    else if (IsPluginArchiveUrlW(cleaned))
    {
        WCHAR ext[16] = L".zip";
        const WCHAR* srcExt = PathFindExtensionW(cleaned);
        if (srcExt && srcExt[0])
            StringCchCopyW(ext, ARRAYSIZE(ext), srcExt);

        WCHAR tempArchive[MAX_PATH];
        if (!CreateTempFilePath(ext, tempArchive, ARRAYSIZE(tempArchive)) ||
            !DownloadUrlToPath(url, tempArchive))
        {
            MessageBoxW(g_hwndPluginMgr, L"Failed to download plugin package.", L"Plugins", MB_OK | MB_ICONERROR);
            return;
        }

        WCHAR installedPath[MAX_PATH];
        if (IsVsixPackageUrlW(cleaned))
        {
            ok = InstallVsixContentPackage(tempArchive, pluginsDir, url, PathFindFileNameW(cleaned),
                                           installedPath, ARRAYSIZE(installedPath));
        }
        else
        {
            ok = InstallArchiveToPluginsFolder(tempArchive, pluginsDir, installedPath, ARRAYSIZE(installedPath));
        }
        DeleteFileW(tempArchive);
    }
    else
    {
        MessageBoxW(g_hwndPluginMgr,
            L"Unsupported URL type. Use a .dll, .zip or .vsix package URL.",
            L"Plugins", MB_OK | MB_ICONWARNING);
        return;
    }

    if (!ok)
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to install plugin package from URL.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    PluginManager_ReloadAll();
    RebuildPluginMenu();
    if (IsVsixPackageUrlW(cleaned))
    {
        MessageBoxW(g_hwndPluginMgr,
            L"VS Code extension package installed as a lightweight content plugin.",
            L"Plugins", MB_OK | MB_ICONINFORMATION);
    }
    RefreshPluginManagerList();
}

static void ToggleInstalledPlugin(int pluginIndex)
{
    if (pluginIndex < 0 || pluginIndex >= g_pluginCount) return;
    PluginEntry* p = &g_plugins[pluginIndex];
    if (p->enabled)
    {
        UnloadPlugin(p);
        p->enabled = FALSE;
    }
    else
    {
        p->enabled = TRUE;
        if (!LoadPlugin(p))
        {
            p->enabled = FALSE;
            MessageBoxW(g_hwndPluginMgr, L"Failed to enable/load plugin.", L"Plugins", MB_OK | MB_ICONERROR);
        }
    }

    RebuildPluginMenu();
    RefreshPluginManagerList();
}

static void UninstallInstalledPlugin(int pluginIndex)
{
    if (pluginIndex < 0 || pluginIndex >= g_pluginCount) return;
    PluginEntry* p = &g_plugins[pluginIndex];
    WCHAR pathToDelete[MAX_PATH];
    StringCchCopyW(pathToDelete, MAX_PATH, p->dllPath);

    int res = MessageBoxW(g_hwndPluginMgr, L"Uninstall selected plugin DLL?", L"Plugins",
                          MB_YESNO | MB_ICONWARNING);
    if (res != IDYES) return;

    UnloadPlugin(p);

    if (!DeleteFileW(pathToDelete))
    {
        MessageBoxW(g_hwndPluginMgr,
                    L"Plugin was removed from runtime list but DLL file could not be deleted.",
                    L"Plugins", MB_OK | MB_ICONWARNING);
    }

    PluginManager_ReloadAll();
    RebuildPluginMenu();
    RefreshPluginManagerList();
}

static LRESULT CALLBACK PluginManagerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, GetPluginMgrBackgroundBrush());
        return 1;
    }

    case WM_CREATE:
    {
        g_hwndPluginMgr = hwnd;
        INITCOMMONCONTROLSEX icc;
        ZeroMemory(&icc, sizeof(icc));
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_LISTVIEW_CLASSES;
        InitCommonControlsEx(&icc);

        CreateWindowExW(0, L"STATIC", L"Search:",
            WS_CHILD | WS_VISIBLE, 12, 12, 50, 20, hwnd, NULL, NULL, NULL);
        CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, 12, 36, 500, 20, hwnd, (HMENU)(UINT_PTR)IDC_PM_STATUS, NULL, NULL);
        CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER,
            64, 10, 460, 24, hwnd, (HMENU)(UINT_PTR)IDC_PM_SEARCH, NULL, NULL);
        HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            12, 42, 512, 288, hwnd, (HMENU)(UINT_PTR)IDC_PM_LIST, NULL, NULL);
        if (hList)
        {
            ListView_SetExtendedListViewStyleEx(hList, 0,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
            SetupPluginManagerListColumns(hList);
            ResizePluginManagerListColumns(hList);
        }

        CreateWindowExW(0, L"BUTTON", L"Add...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 340, 78, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_ADD, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Add URL...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 96, 340, 84, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_ADDURL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Install",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 186, 340, 74, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_INSTALL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Enable/Disable",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 266, 340, 108, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_TOGGLE, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Uninstall",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 380, 340, 78, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_UNINSTALL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Reload",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 374, 70, 26, hwnd, (HMENU)(UINT_PTR)IDC_PM_RELOAD, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 458, 374, 66, 26, hwnd, (HMENU)(UINT_PTR)IDC_PM_CLOSE, NULL, NULL);

        ApplyPluginManagerTheme(hwnd);
        LayoutPluginManagerControls(hwnd);
        LoadAvailableCatalog();
        StartPlatformFetchAsync();
        RefreshPluginManagerList();
        return 0;
    }

    case WM_SIZE:
        LayoutPluginManagerControls(hwnd);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* p = (MINMAXINFO*)lParam;
        if (p)
        {
            p->ptMinTrackSize.x = PM_MIN_W;
            p->ptMinTrackSize.y = PM_MIN_H;
        }
        return 0;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
    {
        HBRUSH hBr = DarkMode_HandleCtlColor((HDC)wParam);
        if (hBr) return (LRESULT)hBr;
        break;
    }

    case PM_MSG_FETCH_COMPLETE:
    {
        PlatformFetchResult* result = (PlatformFetchResult*)lParam;
        if (!result)
            return 0;

        LONG activeRequestId = InterlockedCompareExchange(&g_platformFetchRequestId, 0, 0);
        if (result->requestId == activeRequestId)
        {
            RemoveOnlineAvailableEntries();
            int added = 0;
            for (int i = 0; i < result->count && g_availableCount < MAX_AVAILABLE_PLUGINS; i++)
            {
                AvailablePlugin* it = &result->items[i];
                if (AddAvailablePluginEntryEx(it->name, it->source, it->description,
                                              it->installable, it->referenceOnly, it->author,
                                              it->popularity, it->ratingPermille, TRUE))
                {
                    added++;
                }
            }
            SortAvailablePlugins();
            g_platformFetchInProgress = FALSE;

            WCHAR status[160];
            if (added > 0 && result->usedFallback)
                StringCchPrintfW(status, ARRAYSIZE(status), L"Loaded %d fallback popular Notepad plugins.", added);
            else if (added > 0)
                StringCchPrintfW(status, ARRAYSIZE(status), L"Loaded %d plugins from official catalogs.", added);
            else
                StringCchCopyW(status, ARRAYSIZE(status), L"No online plugins were loaded.");
            SetPluginManagerStatusText(status);
            RefreshPluginManagerList();
        }

        free(result);
        return 0;
    }

    case WM_NOTIFY:
    {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (!pnmh || pnmh->idFrom != IDC_PM_LIST)
            break;

        if (pnmh->code == NM_CUSTOMDRAW)
            return HandlePluginListCustomDraw((NMLVCUSTOMDRAW*)lParam);

        if (pnmh->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* pnm = (NMLISTVIEW*)lParam;
            if (pnm && (pnm->uChanged & LVIF_STATE) &&
                ((pnm->uOldState ^ pnm->uNewState) & LVIS_SELECTED))
            {
                UpdatePluginManagerButtons();
            }
            return 0;
        }

        if (pnmh->code == NM_CLICK || pnmh->code == NM_DBLCLK)
        {
            LPNMITEMACTIVATE pia = (LPNMITEMACTIVATE)lParam;
            if (!pia)
                return 0;

            if (pia->iItem >= 0 && pia->iItem < g_uiRowCount && pia->iSubItem == PM_COL_ACTION)
            {
                PluginMgrRow row = g_uiRows[pia->iItem];
                if (row.type == PM_ROW_AVAILABLE && row.index >= 0 && row.index < g_availableCount)
                {
                    AvailablePlugin* ap = &g_availablePlugins[row.index];
                    if (!ap->installed && ap->installable)
                    {
                        InstallFromAvailable(row.index);
                        return 0;
                    }
                }
                else if (row.type == PM_ROW_INSTALLED && row.index >= 0 && row.index < g_pluginCount)
                {
                    ToggleInstalledPlugin(row.index);
                    return 0;
                }
            }
            UpdatePluginManagerButtons();
            return 0;
        }
        break;
    }

    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (id == IDC_PM_SEARCH && code == EN_CHANGE) {
            RefreshPluginManagerList();
            return 0;
        }
        if (id == IDC_PM_CLOSE && code == BN_CLICKED) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_PM_RELOAD && code == BN_CLICKED) {
            PluginManager_ReloadAll();
            LoadAvailableCatalog();
            StartPlatformFetchAsync();
            RebuildPluginMenu();
            RefreshPluginManagerList();
            return 0;
        }
        if (id == IDC_PM_ADD && code == BN_CLICKED) {
            AddPluginFromFile();
            return 0;
        }
        if (id == IDC_PM_ADDURL && code == BN_CLICKED) {
            AddPluginFromUrl();
            return 0;
        }

        int sel = GetSelectedListIndex(hwnd);
        if (sel < 0 || sel >= g_uiRowCount)
            return 0;

        PluginMgrRow row = g_uiRows[sel];
        if (id == IDC_PM_INSTALL && code == BN_CLICKED && row.type == PM_ROW_AVAILABLE) {
            InstallFromAvailable(row.index);
            return 0;
        }
        if (id == IDC_PM_TOGGLE && code == BN_CLICKED && row.type == PM_ROW_INSTALLED) {
            ToggleInstalledPlugin(row.index);
            return 0;
        }
        if (id == IDC_PM_UNINSTALL && code == BN_CLICKED && row.type == PM_ROW_INSTALLED) {
            UninstallInstalledPlugin(row.index);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        InterlockedIncrement(&g_platformFetchRequestId);
        g_platformFetchInProgress = FALSE;
        g_hwndPluginMgr = NULL;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowPluginManagerWindow(void)
{
    if (g_hwndPluginMgr && IsWindow(g_hwndPluginMgr))
    {
        ShowWindow(g_hwndPluginMgr, SW_SHOW);
        SetForegroundWindow(g_hwndPluginMgr);
        return;
    }

    RegisterPluginMgrClass();
    g_hwndPluginMgr = CreateWindowExW(
        WS_EX_DLGMODALFRAME, PLUGIN_MGR_CLASS, L"Plugin Manager",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 560,
        g_hwndMain, NULL, (HINSTANCE)GetModuleHandle(NULL), NULL);
    if (g_hwndPluginMgr)
    {
        ShowWindow(g_hwndPluginMgr, SW_SHOW);
        UpdateWindow(g_hwndPluginMgr);
    }
}

static BOOL LoadPlugin(PluginEntry* pEntry)
{
    if (pEntry->loaded)
        return TRUE;
    
    pEntry->hModule = LoadLibraryW(pEntry->dllPath);
    if (!pEntry->hModule)
        return FALSE;
    
    // Resolve required exports
    pEntry->pfnGetInfo = (PFN_BikoPlugin_GetInfo)
        GetProcAddress(pEntry->hModule, BIKO_EXPORT_GETINFO);
    pEntry->pfnInit = (PFN_BikoPlugin_Init)
        GetProcAddress(pEntry->hModule, BIKO_EXPORT_INIT);
    pEntry->pfnShutdown = (PFN_BikoPlugin_Shutdown)
        GetProcAddress(pEntry->hModule, BIKO_EXPORT_SHUTDOWN);
    
    if (!pEntry->pfnGetInfo || !pEntry->pfnInit || !pEntry->pfnShutdown)
    {
        FreeLibrary(pEntry->hModule);
        pEntry->hModule = NULL;
        return FALSE;
    }
    
    // Verify API version
    BikoPluginInfo* pInfo = pEntry->pfnGetInfo();
    if (!pInfo || pInfo->apiVersion != BIKO_PLUGIN_API_VERSION)
    {
        FreeLibrary(pEntry->hModule);
        pEntry->hModule = NULL;
        return FALSE;
    }
    
    // Update name from plugin info
    if (pInfo->name)
        StringCchCopyW(pEntry->name, 64, pInfo->name);
    
    // Resolve optional exports
    pEntry->pfnGetMenuCount = (PFN_BikoPlugin_GetMenuItemCount)
        GetProcAddress(pEntry->hModule, BIKO_EXPORT_GETMENUCOUNT);
    pEntry->pfnGetMenuItem = (PFN_BikoPlugin_GetMenuItem)
        GetProcAddress(pEntry->hModule, BIKO_EXPORT_GETMENUITEM);
    pEntry->pfnOnCommand = (PFN_BikoPlugin_OnCommand)
        GetProcAddress(pEntry->hModule, BIKO_EXPORT_ONCOMMAND);
    pEntry->pfnOnDocChanged = (PFN_BikoPlugin_OnDocumentChanged)
        GetProcAddress(pEntry->hModule, BIKO_EXPORT_ONDOCCHANGED);
    
    // Initialize plugin
    if (!pEntry->pfnInit(&g_hostServices))
    {
        FreeLibrary(pEntry->hModule);
        pEntry->hModule = NULL;
        return FALSE;
    }
    
    // Assign command IDs
    pEntry->menuCmdBase = g_nextCmdId;
    pEntry->menuItemCount = 0;
    
    if (pEntry->pfnGetMenuCount)
    {
        pEntry->menuItemCount = pEntry->pfnGetMenuCount();
        if (pEntry->menuItemCount > MAX_MENU_ITEMS)
            pEntry->menuItemCount = MAX_MENU_ITEMS;
        g_nextCmdId += pEntry->menuItemCount;
    }
    
    pEntry->loaded = TRUE;
    return TRUE;
}

static void UnloadPlugin(PluginEntry* pEntry)
{
    if (!pEntry->loaded)
        return;
    
    if (pEntry->pfnShutdown)
        pEntry->pfnShutdown();
    
    if (pEntry->hModule)
    {
        FreeLibrary(pEntry->hModule);
        pEntry->hModule = NULL;
    }
    
    pEntry->loaded = FALSE;
}

//=============================================================================
// Public API
//=============================================================================

void PluginManager_Init(HWND hwndMain, HWND hwndEdit)
{
    if (g_initialized)
        return;
    
    g_hwndMain = hwndMain;
    g_hwndEdit = hwndEdit;
    g_pluginCount = 0;
    g_nextCmdId = IDM_PLUGIN_CMD_BASE;
    g_hPluginMenu = NULL;
    
    InitHostServices();
    
    g_initialized = TRUE;
}

void PluginManager_Shutdown(void)
{
    if (!g_initialized)
        return;

    if (g_hwndPluginMgr && IsWindow(g_hwndPluginMgr))
    {
        DestroyWindow(g_hwndPluginMgr);
        g_hwndPluginMgr = NULL;
    }
    
    // Unload all plugins
    for (int i = 0; i < g_pluginCount; i++)
    {
        UnloadPlugin(&g_plugins[i]);
    }

    RemovePluginMenuFromMain();
    g_pluginCount = 0;
    g_initialized = FALSE;
}

void PluginManager_LoadAll(void)
{
    if (!g_initialized)
        return;
    
    // Scan for plugins
    ScanPluginsFolder();
    
    // Load enabled plugins
    for (int i = 0; i < g_pluginCount; i++)
    {
        if (g_plugins[i].enabled)
        {
            LoadPlugin(&g_plugins[i]);
        }
    }
}

void PluginManager_ReloadAll(void)
{
    // Unload
    for (int i = 0; i < g_pluginCount; i++)
    {
        UnloadPlugin(&g_plugins[i]);
    }
    g_pluginCount = 0;
    g_nextCmdId = IDM_PLUGIN_CMD_BASE;
    
    // Reload
    PluginManager_LoadAll();
}

int PluginManager_GetPluginCount(void)
{
    return g_pluginCount;
}

BOOL PluginManager_HandleCommand(HWND hwnd, UINT cmd)
{
    (void)hwnd;
    
    if (cmd == IDM_PLUGIN_SETTINGS)
    {
        if (g_hwndPluginMgr && IsWindow(g_hwndPluginMgr))
        {
            LoadAvailableCatalog();
            StartPlatformFetchAsync();
            RefreshPluginManagerList();
        }
        ShowPluginManagerWindow();
        return TRUE;
    }
    
    if (cmd == IDM_PLUGIN_RELOAD)
    {
        PluginManager_ReloadAll();
        RebuildPluginMenu();
        if (g_hwndPluginMgr && IsWindow(g_hwndPluginMgr))
        {
            LoadAvailableCatalog();
            StartPlatformFetchAsync();
            RefreshPluginManagerList();
        }
        return TRUE;
    }
    
    // Route to plugin
    if (cmd >= IDM_PLUGIN_CMD_BASE && cmd <= IDM_PLUGIN_CMD_MAX)
    {
        for (int i = 0; i < g_pluginCount; i++)
        {
            PluginEntry* pEntry = &g_plugins[i];
            if (!pEntry->loaded || !pEntry->pfnOnCommand)
                continue;
            
            if (cmd >= pEntry->menuCmdBase && 
                cmd < pEntry->menuCmdBase + (UINT)pEntry->menuItemCount)
            {
                return pEntry->pfnOnCommand(cmd);
            }
        }
    }
    
    return FALSE;
}

void PluginManager_CreateMenu(HMENU hMainMenu)
{
    if (!hMainMenu)
        return;
    
    // Create Plugins submenu
    g_hPluginMenu = CreatePopupMenu();
    if (!g_hPluginMenu)
        return;
    
    // Add plugin menu items
    BOOL hasItems = FALSE;
    
    for (int i = 0; i < g_pluginCount; i++)
    {
        PluginEntry* pEntry = &g_plugins[i];
        if (!pEntry->loaded)
            continue;
        
        if (pEntry->menuItemCount > 0 && pEntry->pfnGetMenuItem)
        {
            // Add separator between plugins
            if (hasItems)
                AppendMenuW(g_hPluginMenu, MF_SEPARATOR, 0, NULL);
            
            for (int j = 0; j < pEntry->menuItemCount; j++)
            {
                BikoMenuItem item;
                ZeroMemory(&item, sizeof(item));
                
                if (pEntry->pfnGetMenuItem(j, &item))
                {
                    UINT cmdId = pEntry->menuCmdBase + j;
                    
                    if (item.separator)
                        AppendMenuW(g_hPluginMenu, MF_SEPARATOR, 0, NULL);
                    
                    WCHAR menuText[128];
                    if (item.shortcut[0])
                        StringCchPrintfW(menuText, 128, L"%s\t%s", item.name, item.shortcut);
                    else
                        StringCchCopyW(menuText, 128, item.name);
                    
                    AppendMenuW(g_hPluginMenu, MF_STRING, cmdId, menuText);
                    hasItems = TRUE;
                }
            }
        }
    }
    
    // Add separator and built-in items
    if (hasItems)
        AppendMenuW(g_hPluginMenu, MF_SEPARATOR, 0, NULL);
    
    AppendMenuW(g_hPluginMenu, MF_STRING, IDM_PLUGIN_RELOAD, L"&Reload Plugins");
    AppendMenuW(g_hPluginMenu, MF_STRING, IDM_PLUGIN_SETTINGS, L"&Settings...");
    
    // Insert into main menu before Help
    int menuCount = GetMenuItemCount(hMainMenu);
    InsertMenuW(hMainMenu, menuCount - 1, MF_BYPOSITION | MF_POPUP,
                (UINT_PTR)g_hPluginMenu, L"&Plugins");
}

void PluginManager_UpdateMenu(HMENU hMainMenu)
{
    (void)hMainMenu;
    // Enable/disable based on state - currently all enabled
}

void PluginManager_NotifyDocumentChanged(void)
{
    for (int i = 0; i < g_pluginCount; i++)
    {
        PluginEntry* pEntry = &g_plugins[i];
        if (pEntry->loaded && pEntry->pfnOnDocChanged)
        {
            pEntry->pfnOnDocChanged();
        }
    }
}
