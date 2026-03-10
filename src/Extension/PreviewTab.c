#include "PreviewTab.h"
#include "WebViewUI.h"
#include "FileManager.h"
#include "Terminal.h"
#include "ViewHelper.h"
#include "SplitterWnd.h"
#include "CommonUtils.h"
#include <shlwapi.h>
#include <stdio.h>

extern WCHAR szCurFile[MAX_PATH + 40];
extern HWND hwndMain;
extern HWND hwndEditParent;

static HWND s_hwndPreview = NULL;
static BOOL s_registered = FALSE;

static LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SIZE:
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        WebViewUI_Resize(&rc);
        return 0;
    }
    case WM_DESTROY:
        WebViewUI_Destroy();
        s_hwndPreview = NULL;
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void PreviewTab_Toggle(void)
{
    if (s_hwndPreview) {
        HWND hwndSplitter = GetParent(s_hwndPreview);
        if (IsSplitterWnd(hwndSplitter)) {
            HWND hwndTargetParent = NULL;
            DeleteSplitterChild(s_hwndPreview, hwndMain, &hwndTargetParent);
            if (hwndTargetParent) {
                hwndEditParent = hwndTargetParent;
            }
        } else {
            DestroyWindow(s_hwndPreview);
        }
        s_hwndPreview = NULL;
        RECT rc;
        GetClientRect(hwndMain, &rc);
        SendMessage(hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        return;
    }

    if (!s_registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = PreviewWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = L"BikoPreviewTab";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc);
        s_registered = TRUE;
    }

    HWND hwndEditActive = n2e_GetActiveEditCheckFocus();
    HWND hwndParent = GetParent(hwndEditActive);

    s_hwndPreview = CreateWindowExW(0, L"BikoPreviewTab", L"", 
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwndMain, NULL, GetModuleHandle(NULL), NULL);

    HWND hwndSplitter = NULL;
    if (IsSplitterWnd(hwndParent)) {
        hwndSplitter = AddSplitterChild(hwndParent, hwndEditActive, s_hwndPreview, TRUE);
    } else {
        hwndSplitter = CreateSplitterWnd(hwndParent, hwndEditActive, s_hwndPreview, TRUE);
    }
    
    if (hwndEditParent == hwndParent || hwndEditParent == hwndEditActive) {
        hwndEditParent = hwndSplitter;
    }
    
    RECT rc;
    GetClientRect(hwndMain, &rc);
    SendMessage(hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    
    WebViewUI_Initialize(s_hwndPreview);
    
    WCHAR packageJson[MAX_PATH];
    const WCHAR* root = FileManager_GetRootPath();
    BOOL hasNode = FALSE;
    if (root && root[0]) {
        wcscpy_s(packageJson, MAX_PATH, root);
        PathAppendW(packageJson, L"package.json");
        if (PathFileExistsW(packageJson)) {
            hasNode = TRUE;
        }
    }
    
    if (hasNode) {
        Terminal_SendCommand("npm run dev\n");
        // For Next.js/etc., it starts in a sec. Just navigating will show white page then load if refreshed,
        // or edge browser auto-retries on connection refused. We'll add a small JS reload if we want, but local WebView2 handles it okay.
        WebViewUI_NavigateTo(L"http://localhost:3000");
    } else {
        WCHAR fileUri[MAX_PATH + 10] = L"file:///";
        wcscat_s(fileUri, MAX_PATH + 10, szCurFile);
        for (int i = 0; fileUri[i]; i++) {
            if (fileUri[i] == L'\\') fileUri[i] = L'/';
        }
        WebViewUI_NavigateTo(fileUri);
    }
}
