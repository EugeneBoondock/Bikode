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

#pragma comment(lib, "shlwapi.lib")

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

//=============================================================================
// Forward Declarations
//=============================================================================

static void InitHostServices(void);
static BOOL LoadPlugin(PluginEntry* pEntry);
static void UnloadPlugin(PluginEntry* pEntry);
static void ScanPluginsFolder(void);
static void GetPluginsPath(LPWSTR path, int maxLen);

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
    
    // Unload all plugins
    for (int i = 0; i < g_pluginCount; i++)
    {
        UnloadPlugin(&g_plugins[i]);
    }
    
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
        // TODO: Show settings dialog
        MessageBoxW(g_hwndMain, L"Plugin settings dialog not yet implemented.",
                    L"Plugins", MB_OK | MB_ICONINFORMATION);
        return TRUE;
    }
    
    if (cmd == IDM_PLUGIN_RELOAD)
    {
        PluginManager_ReloadAll();
        // Rebuild menu
        if (g_hPluginMenu)
        {
            // Clear and rebuild - let CreateMenu handle it
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
