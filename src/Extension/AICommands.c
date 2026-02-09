/******************************************************************************
*
* Biko
*
* AICommands.c
*   Command handlers for AI features, menu creation, and status bar updates.
*
******************************************************************************/

#include "AICommands.h"
#include "AIBridge.h"
#include "AIProvider.h"
#include "AIContext.h"
#include "DiffParse.h"
#include "DiffPreview.h"
#include "PatchUndo.h"
#include "ChatPanel.h"
#include "DarkMode.h"
#include "GitUI.h"
#include "Terminal.h"
#include "MarkdownPreview.h"
#include "FileManager.h"
#include "CommonUtils.h"
#include "SciCall.h"
#include <string.h>
#include <stdio.h>

//=============================================================================
// External references
//=============================================================================

extern WCHAR szIniFile[MAX_PATH];
extern WCHAR szCurFile[MAX_PATH];
extern HWND  hwndMain;
extern HWND  hwndToolbar;
extern HWND  hwndStatus;
extern HWND  hwndReBar;

//=============================================================================
// Internal state
//=============================================================================

static HWND  s_hwndMain = NULL;
static HWND  s_hwndEdit = NULL;
static HMENU s_hAIMenu = NULL;
static AIConfig s_aiConfig;

// Current pending response data
static AIPatch*  s_pCurrentPatches = NULL;
static int       s_iCurrentPatchCount = 0;

//=============================================================================
// Forward declarations
//=============================================================================

static void AICmd_DoTransform(HWND hwndParent);
static void AICmd_DoRefactor(void);
static void AICmd_DoExplain(void);
static void AICmd_DoFix(void);
static void AICmd_HandleResponse(UINT uRequestId);
static void AICmd_HandlePatchResponse(AIResponse* pResp);
static void AICmd_HandleExplainResponse(AIResponse* pResp);
static void AICmd_HandleChatResponse(AIResponse* pResp);
static void AICmd_ShowSettingsDialog(HWND hwndParent);

//=============================================================================
// Initialization
//=============================================================================

void AICommands_Init(HWND hwnd, HWND hwndEd)
{
    s_hwndMain = hwnd;
    s_hwndEdit = hwndEd;

    // Load AI config
    AIBridge_LoadConfig(&s_aiConfig, szIniFile);

    // Initialize subsystems
    PatchUndo_Init();
    AIBridge_Init(hwnd, &s_aiConfig);
    DarkMode_Init(hwnd);
    GitUI_Init(hwnd);
    Terminal_Init(hwnd);
    FileManager_Init(hwnd);
}

void AICommands_Shutdown(void)
{
    // Free current patches
    if (s_pCurrentPatches)
    {
        DiffParse_FreePatches(s_pCurrentPatches, s_iCurrentPatchCount);
        s_pCurrentPatches = NULL;
        s_iCurrentPatchCount = 0;
    }

    FileManager_Shutdown();
    Terminal_Shutdown();
    GitUI_Shutdown();
    DarkMode_Shutdown();
    AIBridge_Shutdown();
}

//=============================================================================
// Command dispatch
//=============================================================================

BOOL AICommands_HandleCommand(HWND hwnd, UINT cmd)
{
    switch (cmd)
    {
    // AI commands
    case IDM_AI_TRANSFORM:
        AICmd_DoTransform(hwnd);
        return TRUE;

    case IDM_AI_REFACTOR:
        AICmd_DoRefactor();
        return TRUE;

    case IDM_AI_EXPLAIN:
        AICmd_DoExplain();
        return TRUE;

    case IDM_AI_FIX:
        AICmd_DoFix();
        return TRUE;

    case IDM_AI_CHAT:
    case IDM_AI_TOGGLE_CHAT:
        ChatPanel_Toggle(hwnd);
        return TRUE;

    case IDM_AI_CANCEL:
        AIBridge_CancelAll();
        if (DiffPreview_IsActive())
            DiffPreview_RejectAll(s_hwndEdit);
        return TRUE;

    // Patch preview commands
    case IDM_AI_APPLY_PATCH:
        if (DiffPreview_IsActive())
            DiffPreview_ApplyAll(s_hwndEdit);
        return TRUE;

    case IDM_AI_REJECT_PATCH:
        if (DiffPreview_IsActive())
            DiffPreview_RejectAll(s_hwndEdit);
        return TRUE;

    case IDM_AI_NEXT_HUNK:
        if (DiffPreview_IsActive())
            DiffPreview_NextHunk(s_hwndEdit);
        return TRUE;

    case IDM_AI_PREV_HUNK:
        if (DiffPreview_IsActive())
            DiffPreview_PrevHunk(s_hwndEdit);
        return TRUE;

    case IDM_AI_TOGGLE_HUNK:
        if (DiffPreview_IsActive())
            DiffPreview_ToggleHunk(s_hwndEdit);
        return TRUE;

    // Engine management
    case IDM_AI_SETTINGS:
        AICmd_ShowSettingsDialog(hwnd);
        return TRUE;

    case IDM_AI_RESTART_ENGINE:
        AIBridge_RestartEngine();
        return TRUE;

    // Dark mode
    case IDM_VIEW_DARKMODE:
        DarkMode_Toggle();
        DarkMode_ApplyAll(hwnd, s_hwndEdit, hwndToolbar, hwndStatus, hwndReBar);
        return TRUE;

    // Git commands
    case IDM_GIT_STATUS:
        GitUI_ShowStatus(hwnd);
        return TRUE;

    case IDM_GIT_DIFF:
        GitUI_ShowDiff(hwnd);
        return TRUE;

    case IDM_GIT_COMMIT:
        GitUI_ShowCommitDialog(hwnd);
        return TRUE;

    case IDM_GIT_LOG:
        GitUI_ShowLog(hwnd);
        return TRUE;

    case IDM_GIT_TOGGLE_PANEL:
        GitUI_TogglePanel(hwnd);
        return TRUE;

    case IDM_GIT_PULL:
        GitUI_PullWithUI(hwnd);
        return TRUE;

    case IDM_GIT_PUSH:
        GitUI_PushWithUI(hwnd);
        return TRUE;

    case IDM_GIT_BLAME:
        GitUI_ShowBlame(hwnd);
        return TRUE;

    case IDM_GIT_BRANCHES:
        GitUI_ShowBranches(hwnd);
        return TRUE;

    case IDM_GIT_STASH:
        GitUI_ShowStash(hwnd);
        return TRUE;

    // Terminal
    case IDM_TERMINAL_TOGGLE:
        Terminal_Toggle(hwnd);
        return TRUE;

    case IDM_TERMINAL_NEW:
        Terminal_New(hwnd);
        return TRUE;

    // Markdown preview
    case IDM_MARKDOWN_PREVIEW:
        MarkdownPreview_Toggle(hwnd);
        return TRUE;

    // File manager
    case IDM_FILEMGR_TOGGLE:
        FileManager_Toggle(hwnd);
        return TRUE;

    case IDM_FILEMGR_OPENFOLDER:
        FileManager_BrowseForFolder(hwnd);
        return TRUE;

    default:
        return FALSE;
    }
}

//=============================================================================
// Message handlers
//=============================================================================

BOOL AICommands_HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
    case WM_AI_RESPONSE:
        AICmd_HandleResponse((UINT)wParam);
        return TRUE;

    case WM_AI_STATUS:
        // Status changed â€” update status bar (trigger repaint)
        InvalidateRect(hwnd, NULL, FALSE);
        return TRUE;

    case WM_AI_CONNECTED:
        // Engine connected
        return TRUE;

    case WM_AI_DISCONNECTED:
        // Engine disconnected
        if (DiffPreview_IsActive())
            DiffPreview_Exit(s_hwndEdit);
        return TRUE;

    case WM_AI_CHUNK:
        // Streaming chunk â€” could update status bar
        return TRUE;

    default:
        return FALSE;
    }
}

//=============================================================================
// Menu
//=============================================================================

void AICommands_CreateMenu(HMENU hMainMenu)
{
    if (!hMainMenu) return;

    // ── Get existing top-level submenus by position ──
    // File=0, Edit=1, View=2, Settings=3, ?=4
    int menuCount = GetMenuItemCount(hMainMenu);
    HMENU hFile     = (menuCount > 0) ? GetSubMenu(hMainMenu, 0) : NULL;
    HMENU hEdit     = (menuCount > 1) ? GetSubMenu(hMainMenu, 1) : NULL;
    HMENU hView     = (menuCount > 2) ? GetSubMenu(hMainMenu, 2) : NULL;
    HMENU hSettings = (menuCount > 3) ? GetSubMenu(hMainMenu, 3) : NULL;

    // ── File menu: Open Folder ──
    if (hFile)
    {
        AppendMenuW(hFile, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hFile, MF_STRING, IDM_FILEMGR_OPENFOLDER,
                    L"Open Folder...");
    }

    // ── Edit menu: AI actions on selection ──
    if (hEdit)
    {
        AppendMenuW(hEdit, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hEdit, MF_STRING, IDM_AI_TRANSFORM,
                    L"AI Transform...\tCtrl+Shift+T");
        AppendMenuW(hEdit, MF_STRING, IDM_AI_REFACTOR,
                    L"AI Refactor\tCtrl+Shift+R");
        AppendMenuW(hEdit, MF_STRING, IDM_AI_EXPLAIN,
                    L"AI Explain\tCtrl+Shift+E");
        AppendMenuW(hEdit, MF_STRING, IDM_AI_FIX,
                    L"AI Fix\tCtrl+Shift+F");
    }

    // ── View menu: panels & visual toggles ──
    if (hView)
    {
        AppendMenuW(hView, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hView, MF_STRING, IDM_FILEMGR_TOGGLE,
                    L"File Explorer\tCtrl+\\");
        AppendMenuW(hView, MF_STRING, IDM_FILEMGR_OPENFOLDER,
                    L"Open Folder...");
        AppendMenuW(hView, MF_STRING, IDM_AI_TOGGLE_CHAT,
                    L"Chat Panel\tCtrl+Shift+C");
        AppendMenuW(hView, MF_STRING, IDM_TERMINAL_TOGGLE,
                    L"Terminal\tCtrl+`");
        AppendMenuW(hView, MF_STRING, IDM_MARKDOWN_PREVIEW,
                    L"Markdown Preview\tCtrl+Shift+M");
        AppendMenuW(hView, MF_STRING, IDM_VIEW_DARKMODE,
                    L"Dark Mode\tCtrl+Shift+D");

        // Git submenu under View
        HMENU hGitMenu = CreatePopupMenu();
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_STATUS,  L"Status");
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_DIFF,    L"Diff Current File");
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_LOG,     L"Log");
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_BLAME,   L"Blame Current File");
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_BRANCHES, L"Branches");
        AppendMenuW(hGitMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_COMMIT,  L"Commit...");
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_PULL,    L"Pull");
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_PUSH,    L"Push");
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_STASH,   L"Stash List");
        AppendMenuW(hGitMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hGitMenu, MF_STRING, IDM_GIT_TOGGLE_PANEL, L"Toggle Git Panel");
        AppendMenuW(hView, MF_POPUP, (UINT_PTR)hGitMenu, L"Git");
    }

    // ── Settings menu: AI configuration ──
    if (hSettings)
    {
        AppendMenuW(hSettings, MF_SEPARATOR, 0, NULL);
        AppendMenuW(hSettings, MF_STRING, IDM_AI_SETTINGS, L"AI Settings...");
        AppendMenuW(hSettings, MF_STRING, IDM_AI_RESTART_ENGINE, L"Restart AI Engine");
    }

    // Keep a reference for UpdateMenu (use the View menu for dark mode checkmark)
    s_hAIMenu = hView;

    DrawMenuBar(s_hwndMain);
}

void AICommands_UpdateMenu(HMENU hMainMenu)
{
    if (!hMainMenu) return;

    BOOL bConnected = AIBridge_IsConnected();
    BOOL bPreviewing = DiffPreview_IsActive();

    UINT aiState = bConnected ? MF_ENABLED : MF_GRAYED;

    // AI actions live in Edit menu now — use EnableMenuItem on the main menu
    EnableMenuItem(hMainMenu, IDM_AI_TRANSFORM, MF_BYCOMMAND | aiState);
    EnableMenuItem(hMainMenu, IDM_AI_REFACTOR, MF_BYCOMMAND | aiState);
    EnableMenuItem(hMainMenu, IDM_AI_EXPLAIN, MF_BYCOMMAND | aiState);
    EnableMenuItem(hMainMenu, IDM_AI_FIX, MF_BYCOMMAND | aiState);

    UINT patchState = bPreviewing ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(hMainMenu, IDM_AI_APPLY_PATCH, MF_BYCOMMAND | patchState);
    EnableMenuItem(hMainMenu, IDM_AI_REJECT_PATCH, MF_BYCOMMAND | patchState);

    // Dark mode checkmark (lives in View menu)
    CheckMenuItem(hMainMenu, IDM_VIEW_DARKMODE,
                  MF_BYCOMMAND | (DarkMode_IsEnabled() ? MF_CHECKED : MF_UNCHECKED));
}

//=============================================================================
// Status bar
//=============================================================================

void AICommands_GetStatusText(WCHAR* wszBuf, int cchBuf)
{
    if (DiffPreview_IsActive())
    {
        WCHAR wszSummary[64];
        DiffPreview_GetSummary(wszSummary, 64);
        swprintf(wszBuf, cchBuf, L"AI: patch %s [%d/%d]",
                 wszSummary,
                 DiffPreview_GetCurrentHunk() + 1,
                 DiffPreview_GetHunkCount());
    }
    else
    {
        lstrcpynW(wszBuf, AIBridge_GetStatusText(), cchBuf);
    }
}

//=============================================================================
// Internal: AI actions
//=============================================================================

static void AICmd_SendAction(EAIRequestType eType, EAIAction eAction,
                             const char* pszInstruction)
{
    if (!AIBridge_IsConnected()) return;

    AIRequest* pReq = AIBridge_CreateRequest(eType, eAction);
    if (!pReq) return;

    AIContext_FillRequest(pReq, s_hwndEdit);

    if (pszInstruction)
    {
        int len = (int)strlen(pszInstruction);
        pReq->pszInstruction = (char*)n2e_Alloc(len + 1);
        if (pReq->pszInstruction)
            memcpy(pReq->pszInstruction, pszInstruction, len + 1);
    }

    AIBridge_SendRequest(pReq);
    AIBridge_FreeRequest(pReq);
}

static void AICmd_DoTransform(HWND hwndParent)
{
    WCHAR wszInstruction[1024];
    if (!AICommands_ShowTransformDialog(hwndParent, wszInstruction, 1024))
        return;

    // Convert to UTF-8
    int needed = WideCharToMultiByte(CP_UTF8, 0, wszInstruction, -1, NULL, 0, NULL, NULL);
    char* utf8 = (char*)n2e_Alloc(needed);
    if (!utf8) return;
    WideCharToMultiByte(CP_UTF8, 0, wszInstruction, -1, utf8, needed, NULL, NULL);

    AICmd_SendAction(AI_REQ_PATCH, AI_ACTION_TRANSFORM, utf8);
    n2e_Free(utf8);
}

static void AICmd_DoRefactor(void)
{
    AICmd_SendAction(AI_REQ_PATCH, AI_ACTION_REFACTOR, NULL);
}

static void AICmd_DoExplain(void)
{
    AICmd_SendAction(AI_REQ_EXPLAIN, AI_ACTION_EXPLAIN, NULL);
}

static void AICmd_DoFix(void)
{
    AICmd_SendAction(AI_REQ_PATCH, AI_ACTION_FIX, NULL);
}

//=============================================================================
// Internal: Response handling
//=============================================================================

static void AICmd_HandleResponse(UINT uRequestId)
{
    AIResponse* pResp = AIBridge_GetResponse(uRequestId);
    if (!pResp) return;

    if (!pResp->bSuccess && pResp->pszErrorMessage)
    {
        // Show error in status bar (no modal dialog)
        // The WM_AI_STATUS handler already updates the UI
        AIBridge_FreeResponse(pResp);
        return;
    }

    // Route by content type
    if (pResp->pPatches && pResp->iPatchCount > 0)
    {
        AICmd_HandlePatchResponse(pResp);
    }
    else if (pResp->pszExplanation)
    {
        AICmd_HandleExplainResponse(pResp);
    }
    else if (pResp->pszChatResponse)
    {
        AICmd_HandleChatResponse(pResp);
    }

    AIBridge_FreeResponse(pResp);
}

static void AICmd_HandlePatchResponse(AIResponse* pResp)
{
    // Free any existing patches
    if (s_pCurrentPatches)
    {
        DiffParse_FreePatches(s_pCurrentPatches, s_iCurrentPatchCount);
    }

    // Parse the raw diffs into structured hunks
    s_iCurrentPatchCount = pResp->iPatchCount;
    s_pCurrentPatches = (AIPatch*)n2e_Alloc(sizeof(AIPatch) * s_iCurrentPatchCount);
    if (!s_pCurrentPatches) return;

    for (int i = 0; i < s_iCurrentPatchCount; i++)
    {
        if (pResp->pPatches[i].pszRawDiff)
        {
            DiffParse_Parse(pResp->pPatches[i].pszRawDiff,
                           (int)strlen(pResp->pPatches[i].pszRawDiff),
                           &s_pCurrentPatches[i]);
        }
        else
        {
            ZeroMemory(&s_pCurrentPatches[i], sizeof(AIPatch));
        }
    }

    // Validate hunks against current buffer
    if (s_iCurrentPatchCount > 0 && s_pCurrentPatches[0].iHunkCount > 0)
    {
        int bufLen = 0;
        char* pBuf = AIContext_GetFileContent(s_hwndEdit, &bufLen);
        if (pBuf)
        {
            DiffParse_ValidateHunks(&s_pCurrentPatches[0], pBuf, bufLen);
            n2e_Free(pBuf);
        }
    }

    // Enter preview mode
    DiffPreview_Enter(s_hwndEdit, s_pCurrentPatches, s_iCurrentPatchCount);
}

static void AICmd_HandleExplainResponse(AIResponse* pResp)
{
    // Show explanation as an annotation on the current line
    if (!pResp->pszExplanation) return;

    int curLine = 0, curCol = 0;
    AIContext_GetCursorPos(s_hwndEdit, &curLine, &curCol);

    SendMessage(s_hwndEdit, SCI_ANNOTATIONSETTEXT,
                (WPARAM)curLine, (LPARAM)pResp->pszExplanation);
    SendMessage(s_hwndEdit, SCI_ANNOTATIONSETVISIBLE, 2 /* ANNOTATION_BOXED */, 0);
}

static void AICmd_HandleChatResponse(AIResponse* pResp)
{
    if (!pResp->pszChatResponse) return;
    ChatPanel_AppendResponse(pResp->pszChatResponse);
}

//=============================================================================
// Transform dialog
//=============================================================================

static WCHAR s_wszTransformResult[1024];

static INT_PTR CALLBACK TransformDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetWindowTextW(hDlg, L"AI Transform");
        // Center on parent
        RECT rcParent, rcDlg;
        GetWindowRect(GetParent(hDlg), &rcParent);
        GetWindowRect(hDlg, &rcDlg);
        int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
        int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        SetFocus(GetDlgItem(hDlg, 100)); // Edit control
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDOK:
            GetDlgItemTextW(hDlg, 100, s_wszTransformResult, 1024);
            EndDialog(hDlg, IDOK);
            return TRUE;

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL AICommands_ShowTransformDialog(HWND hwndParent,
                                    WCHAR* wszInstruction, int cchInstruction)
{
    // Build a simple dialog template in memory (no .rc dependency for this dialog)
    // This creates: a static label, an edit control, OK and Cancel buttons

    #pragma pack(push, 4)
    struct
    {
        DLGTEMPLATE dlg;
        WORD menu;
        WORD wndClass;
        WCHAR title[32];
        // Controls follow, aligned to DWORD
        // 1. Static label "Describe the transformation:"
        // 2. Edit control
        // 3. OK button
        // 4. Cancel button
    } tmpl;
    #pragma pack(pop)

    // For simplicity, use a resource-less approach with CreateDialogIndirect
    // Actually, let's just use a simple MessageBox-style approach with a hook,
    // or better yet, use GetOpenFileName-style dialog template.

    // Simplest approach: build DLGTEMPLATE in memory
    BYTE buffer[1024];
    ZeroMemory(buffer, sizeof(buffer));

    DLGTEMPLATE* pDlg = (DLGTEMPLATE*)buffer;
    pDlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    pDlg->cdit = 4; // 4 controls
    pDlg->cx = 260;
    pDlg->cy = 70;

    WORD* p = (WORD*)(pDlg + 1);
    *p++ = 0; // no menu
    *p++ = 0; // default window class

    // Title
    LPCWSTR title = L"AI Transform";
    while (*title) *p++ = *title++;
    *p++ = 0;

    // Align to DWORD
    if ((ULONG_PTR)p % 4) p++;

    // Control 1: Static label
    DLGITEMTEMPLATE* pItem = (DLGITEMTEMPLATE*)p;
    pItem->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    pItem->x = 7; pItem->y = 7; pItem->cx = 246; pItem->cy = 10;
    pItem->id = 99;
    p = (WORD*)(pItem + 1);
    *p++ = 0xFFFF; *p++ = 0x0082; // Static
    LPCWSTR label = L"Describe the transformation:";
    while (*label) *p++ = *label++;
    *p++ = 0;
    *p++ = 0; // no creation data
    if ((ULONG_PTR)p % 4) p++;

    // Control 2: Edit
    pItem = (DLGITEMTEMPLATE*)p;
    pItem->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    pItem->x = 7; pItem->y = 20; pItem->cx = 246; pItem->cy = 14;
    pItem->id = 100;
    p = (WORD*)(pItem + 1);
    *p++ = 0xFFFF; *p++ = 0x0081; // Edit
    *p++ = 0; // no text
    *p++ = 0;
    if ((ULONG_PTR)p % 4) p++;

    // Control 3: OK button
    pItem = (DLGITEMTEMPLATE*)p;
    pItem->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP;
    pItem->x = 145; pItem->y = 42; pItem->cx = 50; pItem->cy = 14;
    pItem->id = IDOK;
    p = (WORD*)(pItem + 1);
    *p++ = 0xFFFF; *p++ = 0x0080; // Button
    LPCWSTR ok = L"OK";
    while (*ok) *p++ = *ok++;
    *p++ = 0;
    *p++ = 0;
    if ((ULONG_PTR)p % 4) p++;

    // Control 4: Cancel button
    pItem = (DLGITEMTEMPLATE*)p;
    pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
    pItem->x = 200; pItem->y = 42; pItem->cx = 50; pItem->cy = 14;
    pItem->id = IDCANCEL;
    p = (WORD*)(pItem + 1);
    *p++ = 0xFFFF; *p++ = 0x0080; // Button
    LPCWSTR cancel = L"Cancel";
    while (*cancel) *p++ = *cancel++;
    *p++ = 0;
    *p++ = 0;

    s_wszTransformResult[0] = L'\0';
    INT_PTR result = DialogBoxIndirectW(GetModuleHandle(NULL),
                                        (DLGTEMPLATE*)buffer,
                                        hwndParent,
                                        TransformDlgProc);

    if (result == IDOK && s_wszTransformResult[0])
    {
        lstrcpynW(wszInstruction, s_wszTransformResult, cchInstruction);
        return TRUE;
    }
    return FALSE;
}

//=============================================================================
// AI Settings dialog (Provider / Model / Key selection)
//=============================================================================

// Control IDs for settings dialog
#define IDC_SETTINGS_PROVIDER   200
#define IDC_SETTINGS_MODEL      201
#define IDC_SETTINGS_KEY        202
#define IDC_SETTINGS_HOST       203
#define IDC_SETTINGS_PORT       204
#define IDC_SETTINGS_TEMP       205
#define IDC_SETTINGS_MAXTOK     206

static AIConfig s_settingsDlgConfig;

// Helper: populate model combo from a provider's semicolon-separated szModels string
static void PopulateModelCombo(HWND hDlg, const AIProviderDef* pDef, const char* szCurrentModel)
{
    HWND hModelCombo = GetDlgItem(hDlg, IDC_SETTINGS_MODEL);
    SendMessageW(hModelCombo, CB_RESETCONTENT, 0, 0);

    if (!pDef || !pDef->szModels) return;

    // Parse semicolon-separated model list
    char szBuf[1024];
    strncpy(szBuf, pDef->szModels, sizeof(szBuf) - 1);
    szBuf[sizeof(szBuf) - 1] = '\0';

    int selIdx = 0;
    int count = 0;
    char* ctx = NULL;
    char* tok = strtok_s(szBuf, ";", &ctx);
    while (tok)
    {
        WCHAR wszModel[256];
        MultiByteToWideChar(CP_UTF8, 0, tok, -1, wszModel, 256);
        SendMessageW(hModelCombo, CB_ADDSTRING, 0, (LPARAM)wszModel);
        if (szCurrentModel && szCurrentModel[0] && strcmp(tok, szCurrentModel) == 0)
            selIdx = count;
        count++;
        tok = strtok_s(NULL, ";", &ctx);
    }

    SendMessageW(hModelCombo, CB_SETCURSEL, selIdx, 0);
}

static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Copy current config for editing
        memcpy(&s_settingsDlgConfig, &s_aiConfig, sizeof(AIConfig));

        // Populate provider combo
        HWND hCombo = GetDlgItem(hDlg, IDC_SETTINGS_PROVIDER);
        int selIdx = 0;
        for (int i = 0; i < AI_PROVIDER_COUNT; i++)
        {
            const AIProviderDef* pDef = AIProvider_Get((EAIProvider)i);
            if (pDef)
            {
                WCHAR wszName[128];
                MultiByteToWideChar(CP_UTF8, 0, pDef->szName, -1, wszName, 128);
                int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)wszName);
                SendMessageW(hCombo, CB_SETITEMDATA, idx, (LPARAM)i);
                if ((EAIProvider)i == s_settingsDlgConfig.providerCfg.eProvider)
                    selIdx = idx;
            }
        }
        SendMessageW(hCombo, CB_SETCURSEL, selIdx, 0);

        // Populate model dropdown from current provider
        {
            const AIProviderDef* pDef = AIProvider_Get(s_settingsDlgConfig.providerCfg.eProvider);
            PopulateModelCombo(hDlg, pDef, s_settingsDlgConfig.providerCfg.szModel);
        }

        // API key (masked)
        if (s_settingsDlgConfig.providerCfg.szApiKey[0])
        {
            WCHAR wszKey[512];
            MultiByteToWideChar(CP_UTF8, 0, s_settingsDlgConfig.providerCfg.szApiKey,
                                -1, wszKey, 512);
            SetDlgItemTextW(hDlg, IDC_SETTINGS_KEY, wszKey);
        }
        SendMessageW(GetDlgItem(hDlg, IDC_SETTINGS_KEY), EM_SETPASSWORDCHAR, L'*', 0);

        // Max tokens
        SetDlgItemInt(hDlg, IDC_SETTINGS_MAXTOK,
                      s_settingsDlgConfig.providerCfg.iMaxTokens, FALSE);

        // Center dialog
        RECT rcParent, rcDlg;
        GetWindowRect(GetParent(hDlg), &rcParent);
        GetWindowRect(hDlg, &rcDlg);
        int x = rcParent.left + ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
        int y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SETTINGS_PROVIDER:
            if (HIWORD(wParam) == CBN_SELCHANGE)
            {
                HWND hCombo = GetDlgItem(hDlg, IDC_SETTINGS_PROVIDER);
                int idx = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
                if (idx >= 0)
                {
                    EAIProvider eNew = (EAIProvider)SendMessageW(hCombo, CB_GETITEMDATA, idx, 0);
                    const AIProviderDef* pDef = AIProvider_Get(eNew);
                    if (pDef)
                    {
                        s_settingsDlgConfig.providerCfg.eProvider = eNew;
                        // Repopulate model dropdown with this provider's models
                        PopulateModelCombo(hDlg, pDef, pDef->szDefaultModel);

                        // Try to find env-based API key for this provider
                        if (pDef->szEnvVarKey)
                        {
                            char keyBuf[512];
                            DWORD len = GetEnvironmentVariableA(pDef->szEnvVarKey,
                                                                 keyBuf, sizeof(keyBuf));
                            if (len > 0 && len < sizeof(keyBuf))
                            {
                                WCHAR wszKey[512];
                                MultiByteToWideChar(CP_UTF8, 0, keyBuf, -1, wszKey, 512);
                                SetDlgItemTextW(hDlg, IDC_SETTINGS_KEY, wszKey);
                            }
                            else if (!pDef->bRequiresKey)
                            {
                                SetDlgItemTextW(hDlg, IDC_SETTINGS_KEY, L"");
                            }
                        }
                        else if (!pDef->bRequiresKey)
                        {
                            SetDlgItemTextW(hDlg, IDC_SETTINGS_KEY, L"");
                        }
                    }
                }
            }
            break;

        case IDOK:
        {
            // Read all fields into config
            WCHAR buf[512];

            // Model (from combo)
            {
                HWND hModelCombo = GetDlgItem(hDlg, IDC_SETTINGS_MODEL);
                int idx = (int)SendMessageW(hModelCombo, CB_GETCURSEL, 0, 0);
                if (idx >= 0)
                {
                    SendMessageW(hModelCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
                }
                else
                {
                    GetWindowTextW(hModelCombo, buf, 128);
                }
                WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                    s_settingsDlgConfig.providerCfg.szModel,
                                    sizeof(s_settingsDlgConfig.providerCfg.szModel), NULL, NULL);
            }

            // API key
            GetDlgItemTextW(hDlg, IDC_SETTINGS_KEY, buf, 512);
            WideCharToMultiByte(CP_UTF8, 0, buf, -1,
                                s_settingsDlgConfig.providerCfg.szApiKey,
                                sizeof(s_settingsDlgConfig.providerCfg.szApiKey), NULL, NULL);

            // Max tokens
            s_settingsDlgConfig.providerCfg.iMaxTokens =
                GetDlgItemInt(hDlg, IDC_SETTINGS_MAXTOK, NULL, FALSE);

            // Provider (from combo)
            {
                HWND hCombo = GetDlgItem(hDlg, IDC_SETTINGS_PROVIDER);
                int idx = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
                if (idx >= 0)
                    s_settingsDlgConfig.providerCfg.eProvider =
                        (EAIProvider)SendMessageW(hCombo, CB_GETITEMDATA, idx, 0);
            }

            // Apply to global config
            memcpy(&s_aiConfig, &s_settingsDlgConfig, sizeof(AIConfig));
            AIBridge_ApplyConfig(&s_aiConfig);
            AIBridge_SaveConfig(&s_aiConfig, szIniFile);

            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void AICmd_ShowSettingsDialog(HWND hwndParent)
{
    // Build dialog template in memory
    // Layout:
    //   Provider:    [ComboBox___________]
    //   Model:       [Edit_______________]
    //   API Key:     [Edit_______________]
    //   Max Tokens:  [Edit_______________]
    //   [OK] [Cancel]

    BYTE buffer[2048];
    ZeroMemory(buffer, sizeof(buffer));

    DLGTEMPLATE* pDlg = (DLGTEMPLATE*)buffer;
    pDlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    pDlg->cdit = 10;  // 4 labels + 2 edits + 2 combos + 2 buttons
    pDlg->cx = 280;
    pDlg->cy = 120;

    WORD* p = (WORD*)(pDlg + 1);
    *p++ = 0; // no menu
    *p++ = 0; // default class

    LPCWSTR title = L"Bikode \x2014 AI Settings";
    while (*title) *p++ = *title++;
    *p++ = 0;

    // Helper macros for building controls
    #define ALIGN_DWORD() if ((ULONG_PTR)p % 4) p++

    #define ADD_STATIC(_x, _y, _cx, _cy, _id, _text) \
    { \
        ALIGN_DWORD(); \
        DLGITEMTEMPLATE* pi = (DLGITEMTEMPLATE*)p; \
        pi->style = WS_CHILD | WS_VISIBLE | SS_LEFT; \
        pi->x = (_x); pi->y = (_y); pi->cx = (_cx); pi->cy = (_cy); pi->id = (_id); \
        p = (WORD*)(pi + 1); \
        *p++ = 0xFFFF; *p++ = 0x0082; \
        { LPCWSTR _t = (_text); while (*_t) *p++ = *_t++; *p++ = 0; } \
        *p++ = 0; \
    }

    #define ADD_EDIT(_x, _y, _cx, _cy, _id, _flags) \
    { \
        ALIGN_DWORD(); \
        DLGITEMTEMPLATE* pi = (DLGITEMTEMPLATE*)p; \
        pi->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (_flags); \
        pi->x = (_x); pi->y = (_y); pi->cx = (_cx); pi->cy = (_cy); pi->id = (_id); \
        p = (WORD*)(pi + 1); \
        *p++ = 0xFFFF; *p++ = 0x0081; \
        *p++ = 0; *p++ = 0; \
    }

    #define ADD_COMBO(_x, _y, _cx, _cy, _id) \
    { \
        ALIGN_DWORD(); \
        DLGITEMTEMPLATE* pi = (DLGITEMTEMPLATE*)p; \
        pi->style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWNLIST; \
        pi->x = (_x); pi->y = (_y); pi->cx = (_cx); pi->cy = (_cy); pi->id = (_id); \
        p = (WORD*)(pi + 1); \
        *p++ = 0xFFFF; *p++ = 0x0085; \
        *p++ = 0; *p++ = 0; \
    }

    #define ADD_BUTTON(_x, _y, _cx, _cy, _id, _text, _style) \
    { \
        ALIGN_DWORD(); \
        DLGITEMTEMPLATE* pi = (DLGITEMTEMPLATE*)p; \
        pi->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | (_style); \
        pi->x = (_x); pi->y = (_y); pi->cx = (_cx); pi->cy = (_cy); pi->id = (_id); \
        p = (WORD*)(pi + 1); \
        *p++ = 0xFFFF; *p++ = 0x0080; \
        { LPCWSTR _bt = (_text); while (*_bt) *p++ = *_bt++; *p++ = 0; } \
        *p++ = 0; \
    }

    int yRow = 10;
    int labelW = 60;
    int ctrlX = 70;
    int ctrlW = 200;
    int rowH = 16;

    // Row 1: Provider
    ADD_STATIC(7, yRow + 2, labelW, 10, 99, L"Provider:");
    ADD_COMBO(ctrlX, yRow, ctrlW, 120, IDC_SETTINGS_PROVIDER);
    yRow += rowH;

    // Row 2: Model
    ADD_STATIC(7, yRow + 2, labelW, 10, 98, L"Model:");
    ADD_COMBO(ctrlX, yRow, ctrlW, 200, IDC_SETTINGS_MODEL);
    yRow += rowH;

    // Row 3: API Key
    ADD_STATIC(7, yRow + 2, labelW, 10, 97, L"API Key:");
    ADD_EDIT(ctrlX, yRow, ctrlW, 13, IDC_SETTINGS_KEY, ES_PASSWORD);
    yRow += rowH;

    // Row 4: Max Tokens
    ADD_STATIC(7, yRow + 2, labelW, 10, 96, L"Max Tokens:");
    ADD_EDIT(ctrlX, yRow, ctrlW, 13, IDC_SETTINGS_MAXTOK, 0);
    yRow += rowH + 8;

    // Buttons
    ADD_BUTTON(165, yRow, 50, 14, IDOK, L"OK", BS_DEFPUSHBUTTON);
    ADD_BUTTON(220, yRow, 50, 14, IDCANCEL, L"Cancel", 0);

    yRow += 22;

    // Adjust dialog height
    pDlg->cy = (short)yRow;

    #undef ALIGN_DWORD
    #undef ADD_STATIC
    #undef ADD_EDIT
    #undef ADD_COMBO
    #undef ADD_BUTTON

    DialogBoxIndirectW(GetModuleHandle(NULL),
                       (DLGTEMPLATE*)buffer,
                       hwndParent,
                       SettingsDlgProc);
}
