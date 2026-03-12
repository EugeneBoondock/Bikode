/******************************************************************************
*
* Biko
*
* ProofTray.c
*   Delta Mesh proof tray and mission queue UI.
*
******************************************************************************/

#include "ProofTray.h"
#include "AICommands.h"
#include "DiffPreview.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include <uxtheme.h>
#include <stdio.h>
#include <string.h>

#define PT_HEIGHT_DEFAULT   196
#define PT_HEADER_H          28
#define PT_PAD                8
#define PT_BUTTON_W          86
#define PT_BUTTON_H          26
#define PT_LIST_W           220
#define PT_MAX_ITEMS         12

#define IDC_PT_HEADER      0xFB51
#define IDC_PT_LIST        0xFB52
#define IDC_PT_DETAILS     0xFB53
#define IDC_PT_APPLY       0xFB54
#define IDC_PT_REJECT      0xFB55
#define IDC_PT_REFINE      0xFB56
#define IDC_PT_NEXT        0xFB57

typedef struct ProofItem {
    WCHAR wszTitle[256];
    WCHAR wszDetails[4096];
} ProofItem;

static HWND   s_hwndMain = NULL;
static HWND   s_hwndPanel = NULL;
static HWND   s_hwndHeader = NULL;
static HWND   s_hwndList = NULL;
static HWND   s_hwndDetails = NULL;
static HWND   s_hwndApply = NULL;
static HWND   s_hwndReject = NULL;
static HWND   s_hwndRefine = NULL;
static HWND   s_hwndNext = NULL;
static HFONT  s_hHeaderFont = NULL;
static BOOL   s_bVisible = FALSE;
static int    s_iHeight = PT_HEIGHT_DEFAULT;
static ProofItem s_items[PT_MAX_ITEMS];
static int    s_iItemCount = 0;
static int    s_iSelected = -1;
static WCHAR  s_wszMissionStatus[256] = L"Mission idle";

static LRESULT CALLBACK ProofTray_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static void Utf8ToWideTrunc(const char* pszUtf8, WCHAR* wszOut, int cchOut)
{
    if (!wszOut || cchOut <= 0) return;
    wszOut[0] = L'\0';
    if (!pszUtf8 || !pszUtf8[0]) return;
    MultiByteToWideChar(CP_UTF8, 0, pszUtf8, -1, wszOut, cchOut);
    wszOut[cchOut - 1] = L'\0';
}

static void BuildProofDetails(const AIResponse* pResp, const AIPatch* pPatch, WCHAR* wszOut, int cchOut)
{
    WCHAR wszTmp[1024];
    WCHAR* p = wszOut;
    int remaining = cchOut;
    *p = L'\0';

    #define APPEND_TEXT(s) \
        do { \
            if ((s) && *(s) && remaining > 1) { \
                int n = _snwprintf_s(p, remaining, _TRUNCATE, L"%s", (s)); \
                if (n > 0) { p += n; remaining -= n; } \
            } \
        } while (0)
    #define APPEND_LINE(label, value) \
        do { \
            if ((value) && *(value) && remaining > 1) { \
                int n = _snwprintf_s(p, remaining, _TRUNCATE, L"%s%s\r\n", (label), (value)); \
                if (n > 0) { p += n; remaining -= n; } \
            } \
        } while (0)

    APPEND_LINE(L"Mission: ", pResp ? s_wszMissionStatus : L"");
    if (pResp && pResp->pszMissionSummary)
    {
        Utf8ToWideTrunc(pResp->pszMissionSummary, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Summary: ", wszTmp);
    }
    if (pPatch && pPatch->pszProofSummary)
    {
        Utf8ToWideTrunc(pPatch->pszProofSummary, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Proof: ", wszTmp);
    }
    if (pPatch && pPatch->pszTouchedSymbols)
    {
        Utf8ToWideTrunc(pPatch->pszTouchedSymbols, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Touched: ", wszTmp);
    }
    if (pPatch && pPatch->pszValidations)
    {
        Utf8ToWideTrunc(pPatch->pszValidations, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Validations: ", wszTmp);
    }
    if (pPatch && pPatch->pszReviewerVotes)
    {
        Utf8ToWideTrunc(pPatch->pszReviewerVotes, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Reviewers: ", wszTmp);
    }
    if (pPatch && pPatch->pszResidualRisk)
    {
        Utf8ToWideTrunc(pPatch->pszResidualRisk, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Residual risk: ", wszTmp);
    }
    if (pPatch && pPatch->pszRollbackFingerprint)
    {
        Utf8ToWideTrunc(pPatch->pszRollbackFingerprint, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Rollback: ", wszTmp);
    }
    if (pResp && pResp->pszBuildCommand)
    {
        Utf8ToWideTrunc(pResp->pszBuildCommand, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Build memory: ", wszTmp);
    }
    if (pResp && pResp->pszTestCommand)
    {
        Utf8ToWideTrunc(pResp->pszTestCommand, wszTmp, COUNTOF(wszTmp));
        APPEND_LINE(L"Test memory: ", wszTmp);
    }
    if (pPatch && pPatch->bStale)
        APPEND_TEXT(L"\r\nThis ghost layer is stale and must be regenerated before apply.\r\n");

    #undef APPEND_TEXT
    #undef APPEND_LINE
}

static void ApplyThemes(void)
{
    if (!s_hwndPanel) return;
    if (DarkMode_IsEnabled())
    {
        DarkMode_ApplyToDialog(s_hwndPanel);
        SetWindowTheme(s_hwndList, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_hwndDetails, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_hwndApply, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_hwndReject, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_hwndRefine, L"DarkMode_Explorer", NULL);
        SetWindowTheme(s_hwndNext, L"DarkMode_Explorer", NULL);
    }
    else
    {
        SetWindowTheme(s_hwndList, L"", NULL);
        SetWindowTheme(s_hwndDetails, L"", NULL);
        SetWindowTheme(s_hwndApply, L"", NULL);
        SetWindowTheme(s_hwndReject, L"", NULL);
        SetWindowTheme(s_hwndRefine, L"", NULL);
        SetWindowTheme(s_hwndNext, L"", NULL);
    }
    InvalidateRect(s_hwndPanel, NULL, TRUE);
}

static void UpdateSelection(void)
{
    if (!s_hwndDetails) return;
    if (s_iSelected >= 0 && s_iSelected < s_iItemCount)
        SetWindowTextW(s_hwndDetails, s_items[s_iSelected].wszDetails);
    else
        SetWindowTextW(s_hwndDetails, L"No proof selected.");
}

static void UpdateButtons(void)
{
    BOOL bHasPreview = DiffPreview_IsActive();
    EnableWindow(s_hwndApply, bHasPreview);
    EnableWindow(s_hwndReject, bHasPreview);
    EnableWindow(s_hwndRefine, bHasPreview);
    EnableWindow(s_hwndNext, bHasPreview);
}

static void LayoutControls(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    int listW = PT_LIST_W;
    int detailsX = PT_PAD + listW + PT_PAD;
    int detailsW = width - detailsX - PT_PAD;
    int buttonsY = height - PT_PAD - PT_BUTTON_H;
    int detailsH = buttonsY - (PT_HEADER_H + PT_PAD * 2);
    if (detailsH < 60) detailsH = 60;

    MoveWindow(s_hwndHeader, PT_PAD, 4, width - PT_PAD * 2, PT_HEADER_H - 4, TRUE);
    MoveWindow(s_hwndList, PT_PAD, PT_HEADER_H + PT_PAD, listW, height - PT_HEADER_H - PT_PAD * 2, TRUE);
    MoveWindow(s_hwndDetails, detailsX, PT_HEADER_H + PT_PAD, detailsW, detailsH, TRUE);

    MoveWindow(s_hwndApply, width - PT_PAD - PT_BUTTON_W * 4 - PT_PAD * 3, buttonsY, PT_BUTTON_W, PT_BUTTON_H, TRUE);
    MoveWindow(s_hwndReject, width - PT_PAD - PT_BUTTON_W * 3 - PT_PAD * 2, buttonsY, PT_BUTTON_W, PT_BUTTON_H, TRUE);
    MoveWindow(s_hwndRefine, width - PT_PAD - PT_BUTTON_W * 2 - PT_PAD, buttonsY, PT_BUTTON_W, PT_BUTTON_H, TRUE);
    MoveWindow(s_hwndNext, width - PT_PAD - PT_BUTTON_W, buttonsY, PT_BUTTON_W, PT_BUTTON_H, TRUE);
}

static BOOL EnsureWindow(void)
{
    if (s_hwndPanel) return TRUE;

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = ProofTray_WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"BikoProofTray";
    RegisterClassW(&wc);

    s_hwndPanel = CreateWindowExW(
        0, L"BikoProofTray", L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        s_hwndMain, (HMENU)(UINT_PTR)IDC_PROOFTRAY_PANEL, GetModuleHandleW(NULL), NULL);
    return s_hwndPanel != NULL;
}

static LRESULT CALLBACK ProofTray_WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        s_hwndHeader = CreateWindowExW(0, L"STATIC", s_wszMissionStatus,
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_PT_HEADER, NULL, NULL);
        s_hwndList = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_PT_LIST, NULL, NULL);
        s_hwndDetails = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_PT_DETAILS, NULL, NULL);
        s_hwndApply = CreateWindowExW(0, L"BUTTON", L"Apply",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_PT_APPLY, NULL, NULL);
        s_hwndReject = CreateWindowExW(0, L"BUTTON", L"Reject",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_PT_REJECT, NULL, NULL);
        s_hwndRefine = CreateWindowExW(0, L"BUTTON", L"Refine",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_PT_REFINE, NULL, NULL);
        s_hwndNext = CreateWindowExW(0, L"BUTTON", L"Next Hunk",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)(UINT_PTR)IDC_PT_NEXT, NULL, NULL);

        s_hHeaderFont = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        if (s_hHeaderFont)
            SendMessageW(s_hwndHeader, WM_SETFONT, (WPARAM)s_hHeaderFont, TRUE);

        ApplyThemes();
        LayoutControls(hwnd);
        UpdateSelection();
        UpdateButtons();
        return 0;
    }

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wParam, &rc, DarkMode_IsEnabled() ? DarkMode_GetBackgroundBrush() : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
        HBRUSH hbr = DarkMode_HandleCtlColor((HDC)wParam);
        if (hbr) return (LRESULT)hbr;
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_PT_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE)
            {
                s_iSelected = (int)SendMessageW(s_hwndList, LB_GETCURSEL, 0, 0);
                UpdateSelection();
                UpdateButtons();
            }
            return 0;
        case IDC_PT_APPLY:
            SendMessageW(s_hwndMain, WM_COMMAND, IDM_AI_APPLY_PATCH, 0);
            return 0;
        case IDC_PT_REJECT:
            SendMessageW(s_hwndMain, WM_COMMAND, IDM_AI_REJECT_PATCH, 0);
            return 0;
        case IDC_PT_REFINE:
            SendMessageW(s_hwndMain, WM_COMMAND, IDM_AI_REFINE_PATCH, 0);
            return 0;
        case IDC_PT_NEXT:
            SendMessageW(s_hwndMain, WM_COMMAND, IDM_AI_NEXT_HUNK, 0);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

BOOL ProofTray_Init(HWND hwndMain)
{
    s_hwndMain = hwndMain;
    return EnsureWindow();
}

void ProofTray_Shutdown(void)
{
    if (s_hHeaderFont)
    {
        DeleteObject(s_hHeaderFont);
        s_hHeaderFont = NULL;
    }
    if (s_hwndPanel)
    {
        DestroyWindow(s_hwndPanel);
        s_hwndPanel = NULL;
    }
    s_hwndHeader = s_hwndList = s_hwndDetails = NULL;
    s_hwndApply = s_hwndReject = s_hwndRefine = s_hwndNext = NULL;
    s_iItemCount = 0;
    s_iSelected = -1;
    s_bVisible = FALSE;
}

void ProofTray_Show(HWND hwndParent)
{
    if (!EnsureWindow()) return;
    ShowWindow(s_hwndPanel, SW_SHOW);
    s_bVisible = TRUE;
    if (hwndParent)
    {
        RECT rc;
        GetClientRect(hwndParent, &rc);
        SendMessageW(hwndParent, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    }
}

void ProofTray_Hide(void)
{
    if (!s_hwndPanel) return;
    ShowWindow(s_hwndPanel, SW_HIDE);
    s_bVisible = FALSE;
    if (s_hwndMain)
    {
        RECT rc;
        GetClientRect(s_hwndMain, &rc);
        SendMessageW(s_hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    }
}

void ProofTray_Toggle(HWND hwndParent)
{
    if (s_bVisible) ProofTray_Hide();
    else ProofTray_Show(hwndParent);
}

BOOL ProofTray_IsVisible(void)
{
    return s_bVisible;
}

int ProofTray_Layout(HWND hwndParent, int parentWidth, int parentBottom)
{
    if (!s_bVisible || !s_hwndPanel) return 0;
    int top = parentBottom - s_iHeight;
    if (top < 80) top = 80;
    MoveWindow(s_hwndPanel, 0, top, parentWidth, s_iHeight, TRUE);
    return s_iHeight;
}

HWND ProofTray_GetPanelHwnd(void)
{
    return s_hwndPanel;
}

void ProofTray_ApplyDarkMode(void)
{
    ApplyThemes();
}

void ProofTray_Clear(void)
{
    s_iItemCount = 0;
    s_iSelected = -1;
    if (s_hwndList)
        SendMessageW(s_hwndList, LB_RESETCONTENT, 0, 0);
    UpdateSelection();
    UpdateButtons();
}

void ProofTray_SetMissionStatus(LPCWSTR wszStatus)
{
    lstrcpynW(s_wszMissionStatus, wszStatus ? wszStatus : L"Mission idle", COUNTOF(s_wszMissionStatus));
    if (s_hwndHeader)
        SetWindowTextW(s_hwndHeader, s_wszMissionStatus);
}

void ProofTray_Publish(const AIResponse* pResp, const AIPatch* pPatches, int iPatchCount, BOOL bPreviewActive)
{
    UNREFERENCED_PARAMETER(bPreviewActive);

    if (!pResp) return;
    if (!EnsureWindow()) return;

    if (s_iItemCount >= PT_MAX_ITEMS)
    {
        memmove(&s_items[0], &s_items[1], sizeof(ProofItem) * (PT_MAX_ITEMS - 1));
        s_iItemCount = PT_MAX_ITEMS - 1;
    }

    ProofItem* item = &s_items[s_iItemCount];
    ZeroMemory(item, sizeof(*item));

    {
        WCHAR wszMissionId[128] = L"mission";
        WCHAR wszPhase[128] = L"ready";
        WCHAR wszSummary[256] = L"Proof ready";
        if (pResp->pszMissionId) Utf8ToWideTrunc(pResp->pszMissionId, wszMissionId, COUNTOF(wszMissionId));
        if (pResp->pszMissionPhase) Utf8ToWideTrunc(pResp->pszMissionPhase, wszPhase, COUNTOF(wszPhase));
        if (pResp->pszMissionSummary) Utf8ToWideTrunc(pResp->pszMissionSummary, wszSummary, COUNTOF(wszSummary));
        _snwprintf_s(item->wszTitle, COUNTOF(item->wszTitle), _TRUNCATE, L"%s  [%s]", wszMissionId, wszPhase);
        _snwprintf_s(s_wszMissionStatus, COUNTOF(s_wszMissionStatus), _TRUNCATE, L"%s", wszSummary);
    }

    BuildProofDetails(pResp, (iPatchCount > 0) ? &pPatches[0] : NULL, item->wszDetails, COUNTOF(item->wszDetails));
    s_iItemCount++;

    SendMessageW(s_hwndList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < s_iItemCount; i++)
        SendMessageW(s_hwndList, LB_ADDSTRING, 0, (LPARAM)s_items[i].wszTitle);

    s_iSelected = s_iItemCount - 1;
    SendMessageW(s_hwndList, LB_SETCURSEL, s_iSelected, 0);
    UpdateSelection();
    UpdateButtons();
    ProofTray_SetMissionStatus(s_wszMissionStatus);
    ProofTray_Show(s_hwndMain);
}

void ProofTray_PublishMissionNote(LPCWSTR wszTitle, LPCWSTR wszDetails)
{
    ProofItem* item;
    if (!EnsureWindow()) return;
    if (s_iItemCount >= PT_MAX_ITEMS)
    {
        memmove(&s_items[0], &s_items[1], sizeof(ProofItem) * (PT_MAX_ITEMS - 1));
        s_iItemCount = PT_MAX_ITEMS - 1;
    }
    item = &s_items[s_iItemCount];
    ZeroMemory(item, sizeof(*item));
    lstrcpynW(item->wszTitle, wszTitle ? wszTitle : L"Mission note", COUNTOF(item->wszTitle));
    lstrcpynW(item->wszDetails, wszDetails ? wszDetails : L"", COUNTOF(item->wszDetails));
    s_iItemCount++;

    SendMessageW(s_hwndList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < s_iItemCount; i++)
        SendMessageW(s_hwndList, LB_ADDSTRING, 0, (LPARAM)s_items[i].wszTitle);

    s_iSelected = s_iItemCount - 1;
    SendMessageW(s_hwndList, LB_SETCURSEL, s_iSelected, 0);
    UpdateSelection();
    UpdateButtons();
    ProofTray_Show(s_hwndMain);
}
