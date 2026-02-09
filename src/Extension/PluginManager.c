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
#include <commctrl.h>
#include "SciCall.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <urlmon.h>
#include <stdio.h>

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
#define MAX_AVAILABLE_PLUGINS 128
#define PLUGIN_MGR_CLASS    L"BikoPluginManagerWnd"

// Plugin manager window controls
#define IDC_PM_SEARCH       5010
#define IDC_PM_LIST         5011
#define IDC_PM_ADD          5012
#define IDC_PM_INSTALL      5013
#define IDC_PM_TOGGLE       5014
#define IDC_PM_UNINSTALL    5015
#define IDC_PM_RELOAD       5016
#define IDC_PM_CLOSE        5017

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
    BOOL  installed;
} AvailablePlugin;

typedef enum {
    PM_ROW_INSTALLED = 1,
    PM_ROW_AVAILABLE = 2
} PluginMgrRowType;

typedef struct {
    PluginMgrRowType type;
    int index;
} PluginMgrRow;

static AvailablePlugin g_availablePlugins[MAX_AVAILABLE_PLUGINS];
static int             g_availableCount = 0;
static PluginMgrRow    g_uiRows[MAX_PLUGINS + MAX_AVAILABLE_PLUGINS];
static int             g_uiRowCount = 0;

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
static void RefreshAvailableInstalledFlags(void);
static void RefreshPluginManagerList(void);
static void UpdatePluginManagerButtons(void);
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

static void LoadAvailableCatalog(void)
{
    g_availableCount = 0;

    WCHAR pluginsPath[MAX_PATH];
    if (!EnsurePluginsDirPath(pluginsPath, MAX_PATH))
        return;

    WCHAR catalogPath[MAX_PATH];
    StringCchPrintfW(catalogPath, MAX_PATH, L"%s\\catalog.txt", pluginsPath);
    if (!PathFileExistsW(catalogPath))
        return;

    FILE* f = _wfopen(catalogPath, L"rt, ccs=UTF-8");
    if (!f) {
        f = _wfopen(catalogPath, L"rt");
        if (!f) return;
    }

    WCHAR line[2048];
    while (fgetws(line, (int)(sizeof(line) / sizeof(line[0])), f))
    {
        if (g_availableCount >= MAX_AVAILABLE_PLUGINS)
            break;

        // trim trailing newline
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

        AvailablePlugin* ap = &g_availablePlugins[g_availableCount++];
        ZeroMemory(ap, sizeof(*ap));
        StringCchCopyW(ap->name, ARRAYSIZE(ap->name), name);
        StringCchCopyW(ap->source, ARRAYSIZE(ap->source), source);
        StringCchCopyW(ap->description, ARRAYSIZE(ap->description), desc);
        ap->installed = FALSE;
    }

    fclose(f);
}

static void RefreshAvailableInstalledFlags(void)
{
    for (int i = 0; i < g_availableCount; i++)
    {
        g_availablePlugins[i].installed = FALSE;
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
    if (!hList) return LB_ERR;
    return (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
}

static void UpdatePluginManagerButtons(void)
{
    if (!g_hwndPluginMgr) return;
    int sel = GetSelectedListIndex(g_hwndPluginMgr);
    BOOL canInstall = FALSE, canToggle = FALSE, canUninstall = FALSE;

    if (sel != LB_ERR && sel >= 0 && sel < g_uiRowCount)
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
                !g_availablePlugins[row.index].installed)
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

static void RefreshPluginManagerList(void)
{
    if (!g_hwndPluginMgr) return;

    HWND hList = GetDlgItem(g_hwndPluginMgr, IDC_PM_LIST);
    HWND hSearch = GetDlgItem(g_hwndPluginMgr, IDC_PM_SEARCH);
    if (!hList || !hSearch) return;

    WCHAR filter[256];
    GetWindowTextW(hSearch, filter, ARRAYSIZE(filter));

    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_uiRowCount = 0;
    RefreshAvailableInstalledFlags();

    WCHAR line[1024];
    for (int i = 0; i < g_pluginCount; i++)
    {
        PluginEntry* p = &g_plugins[i];
        StringCchPrintfW(line, ARRAYSIZE(line),
            L"[Installed] %s  (%s%s)",
            p->name[0] ? p->name : L"(unnamed)",
            p->enabled ? L"Enabled" : L"Disabled",
            p->loaded ? L", Loaded" : L", Not loaded");

        if (!MatchSearch(line, filter))
            continue;

        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)line);
        if (idx >= 0 && g_uiRowCount < (int)ARRAYSIZE(g_uiRows))
        {
            g_uiRows[idx].type = PM_ROW_INSTALLED;
            g_uiRows[idx].index = i;
            g_uiRowCount = max(g_uiRowCount, idx + 1);
        }
    }

    for (int i = 0; i < g_availableCount; i++)
    {
        AvailablePlugin* ap = &g_availablePlugins[i];
        StringCchPrintfW(line, ARRAYSIZE(line),
            L"[Available] %s%s",
            ap->name[0] ? ap->name : L"(unnamed)",
            ap->installed ? L"  (Installed)" : L"");

        if (!MatchSearch(line, filter) && !MatchSearch(ap->description, filter))
            continue;

        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)line);
        if (idx >= 0 && g_uiRowCount < (int)ARRAYSIZE(g_uiRows))
        {
            g_uiRows[idx].type = PM_ROW_AVAILABLE;
            g_uiRows[idx].index = i;
            g_uiRowCount = max(g_uiRowCount, idx + 1);
        }
    }

    if (SendMessageW(hList, LB_GETCOUNT, 0, 0) > 0)
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
    UpdatePluginManagerButtons();
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
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = PLUGIN_MGR_CLASS;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);
    g_uiClassRegistered = TRUE;
}

static void InstallFromAvailable(int availableIndex)
{
    if (availableIndex < 0 || availableIndex >= g_availableCount) return;
    AvailablePlugin* ap = &g_availablePlugins[availableIndex];
    if (ap->installed) return;

    WCHAR pluginsDir[MAX_PATH];
    if (!EnsurePluginsDirPath(pluginsDir, MAX_PATH))
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to resolve plugins folder.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    WCHAR destPath[MAX_PATH];
    const WCHAR* destName = PathFindFileNameW(ap->source);
    if (!destName || !destName[0] || wcscmp(destName, ap->source) == 0)
    {
        StringCchPrintfW(destPath, MAX_PATH, L"%s\\%s.dll", pluginsDir, ap->name);
    }
    else
    {
        StringCchPrintfW(destPath, MAX_PATH, L"%s\\%s", pluginsDir, destName);
    }

    HRESULT hr = E_FAIL;
    if (IsHttpUrlW(ap->source))
    {
        hr = URLDownloadToFileW(NULL, ap->source, destPath, 0, NULL);
    }
    else
    {
        hr = CopyFileW(ap->source, destPath, FALSE) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    if (FAILED(hr))
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to install selected plugin.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    PluginManager_ReloadAll();
    RebuildPluginMenu();
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
    ofn.lpstrFilter = L"Plugin DLL (*.dll)\0*.dll\0All Files\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return;

    WCHAR destPath[MAX_PATH];
    StringCchPrintfW(destPath, MAX_PATH, L"%s\\%s", pluginsDir, PathFindFileNameW(srcPath));

    if (PathFileExistsW(destPath))
    {
        int res = MessageBoxW(g_hwndPluginMgr,
            L"A plugin with this filename already exists. Overwrite?",
            L"Plugins", MB_YESNO | MB_ICONQUESTION);
        if (res != IDYES)
            return;
    }

    if (!CopyFileW(srcPath, destPath, FALSE))
    {
        MessageBoxW(g_hwndPluginMgr, L"Failed to copy plugin DLL.", L"Plugins", MB_OK | MB_ICONERROR);
        return;
    }

    PluginManager_ReloadAll();
    RebuildPluginMenu();
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
    case WM_CREATE:
    {
        CreateWindowExW(0, L"STATIC", L"Search:",
            WS_CHILD | WS_VISIBLE, 12, 12, 50, 20, hwnd, NULL, NULL, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            64, 10, 460, 24, hwnd, (HMENU)(UINT_PTR)IDC_PM_SEARCH, NULL, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            12, 42, 512, 288, hwnd, (HMENU)(UINT_PTR)IDC_PM_LIST, NULL, NULL);

        CreateWindowExW(0, L"BUTTON", L"Add...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 340, 78, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_ADD, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Install",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 96, 340, 78, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_INSTALL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Enable/Disable",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 180, 340, 110, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_TOGGLE, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Uninstall",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 296, 340, 84, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_UNINSTALL, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Reload",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 386, 340, 66, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_RELOAD, NULL, NULL);
        CreateWindowExW(0, L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 458, 340, 66, 28, hwnd, (HMENU)(UINT_PTR)IDC_PM_CLOSE, NULL, NULL);

        LoadAvailableCatalog();
        RefreshPluginManagerList();
        return 0;
    }

    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD code = HIWORD(wParam);

        if (id == IDC_PM_SEARCH && code == EN_CHANGE) {
            RefreshPluginManagerList();
            return 0;
        }
        if (id == IDC_PM_LIST && code == LBN_SELCHANGE) {
            UpdatePluginManagerButtons();
            return 0;
        }
        if (id == IDC_PM_CLOSE && code == BN_CLICKED) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_PM_RELOAD && code == BN_CLICKED) {
            PluginManager_ReloadAll();
            LoadAvailableCatalog();
            RebuildPluginMenu();
            RefreshPluginManagerList();
            return 0;
        }
        if (id == IDC_PM_ADD && code == BN_CLICKED) {
            AddPluginFromFile();
            return 0;
        }

        int sel = GetSelectedListIndex(hwnd);
        if (sel == LB_ERR || sel < 0 || sel >= g_uiRowCount)
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
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 548, 420,
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
        LoadAvailableCatalog();
        ShowPluginManagerWindow();
        return TRUE;
    }
    
    if (cmd == IDM_PLUGIN_RELOAD)
    {
        PluginManager_ReloadAll();
        RebuildPluginMenu();
        if (g_hwndPluginMgr && IsWindow(g_hwndPluginMgr))
            RefreshPluginManagerList();
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
