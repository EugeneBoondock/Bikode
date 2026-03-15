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

/* Detect the dev server port from package.json scripts */
static int DetectDevServerPort(const WCHAR* root)
{
    WCHAR packageJson[MAX_PATH];
    HANDLE hFile;
    char buf[8192];
    DWORD dwRead = 0;

    wcscpy_s(packageJson, MAX_PATH, root);
    PathAppendW(packageJson, L"package.json");

    hFile = CreateFileW(packageJson, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    ReadFile(hFile, buf, sizeof(buf) - 1, &dwRead, NULL);
    CloseHandle(hFile);
    buf[dwRead] = '\0';

    /* Look for common dev server patterns in scripts */
    if (strstr(buf, "\"dev\"") || strstr(buf, "\"start\"") ||
        strstr(buf, "\"serve\""))
    {
        /* Check for common ports */
        if (strstr(buf, "vite") || strstr(buf, "svelte"))
            return 5173;
        if (strstr(buf, "next") || strstr(buf, "react-scripts") || strstr(buf, "nuxt"))
            return 3000;
        if (strstr(buf, "angular") || strstr(buf, "ng serve"))
            return 4200;
        if (strstr(buf, "vue") || strstr(buf, "webpack-dev-server"))
            return 8080;
        /* Default for generic npm dev scripts */
        return 3000;
    }
    return 0;
}

/* Check if the current file is a previewable frontend file */
static BOOL IsPreviewableFile(const WCHAR* path)
{
    const WCHAR* ext;
    if (!path || !path[0]) return FALSE;
    ext = PathFindExtensionW(path);
    if (!ext) return FALSE;
    return (lstrcmpiW(ext, L".html") == 0 ||
            lstrcmpiW(ext, L".htm") == 0 ||
            lstrcmpiW(ext, L".svg") == 0 ||
            lstrcmpiW(ext, L".xml") == 0);
}

/* Find any index.html in the project */
static BOOL FindIndexHtml(const WCHAR* root, WCHAR* outPath, int cchOutPath)
{
    /* Check common locations for index.html */
    const WCHAR* candidates[] = {
        L"index.html",
        L"public\\index.html",
        L"src\\index.html",
        L"dist\\index.html",
        L"build\\index.html",
        L"www\\index.html"
    };
    int i;
    WCHAR test[MAX_PATH];
    for (i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++)
    {
        wcscpy_s(test, MAX_PATH, root);
        PathAppendW(test, candidates[i]);
        if (PathFileExistsW(test))
        {
            wcscpy_s(outPath, cchOutPath, test);
            return TRUE;
        }
    }
    return FALSE;
}

static void BuildFileUri(const WCHAR* filePath, WCHAR* outUri, int cchUri)
{
    int i;
    wcscpy_s(outUri, cchUri, L"file:///");
    wcscat_s(outUri, cchUri, filePath);
    for (i = 0; outUri[i]; i++)
    {
        if (outUri[i] == L'\\') outUri[i] = L'/';
    }
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
    if (!hwndEditActive || !IsWindow(hwndEditActive))
    {
        /* No active editor — can't split, just show standalone */
        hwndEditActive = NULL;
    }

    HWND hwndParent = hwndEditActive ? GetParent(hwndEditActive) : NULL;

    s_hwndPreview = CreateWindowExW(0, L"BikoPreviewTab", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwndMain, NULL, GetModuleHandle(NULL), NULL);

    if (!s_hwndPreview)
        return;

    if (hwndEditActive)
    {
        HWND hwndSplitter = NULL;
        if (hwndParent && IsSplitterWnd(hwndParent)) {
            hwndSplitter = AddSplitterChild(hwndParent, hwndEditActive, s_hwndPreview, TRUE);
        } else {
            hwndSplitter = CreateSplitterWnd(hwndParent ? hwndParent : hwndMain,
                hwndEditActive, s_hwndPreview, TRUE);
        }

        if (hwndSplitter && (hwndEditParent == hwndParent || hwndEditParent == hwndEditActive)) {
            hwndEditParent = hwndSplitter;
        }
    }

    {
        RECT rc;
        GetClientRect(hwndMain, &rc);
        SendMessage(hwndMain, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
    }

    WebViewUI_Initialize(s_hwndPreview);

    /* Determine what to preview */
    {
        const WCHAR* root = FileManager_GetRootPath();
        int devPort = 0;

        if (root && root[0])
            devPort = DetectDevServerPort(root);

        if (devPort > 0)
        {
            /* Node.js project with dev scripts — start dev server and navigate */
            WCHAR url[128];
            Terminal_SendCommand("npm run dev\n");
            StringCchPrintfW(url, ARRAYSIZE(url), L"http://localhost:%d", devPort);
            WebViewUI_NavigateTo(url);
        }
        else if (IsPreviewableFile(szCurFile))
        {
            /* Current file is a previewable HTML/SVG file */
            WCHAR fileUri[MAX_PATH + 64];
            BuildFileUri(szCurFile, fileUri, ARRAYSIZE(fileUri));
            WebViewUI_NavigateTo(fileUri);
        }
        else if (root && root[0])
        {
            /* Try to find an index.html in the project */
            WCHAR indexPath[MAX_PATH];
            if (FindIndexHtml(root, indexPath, ARRAYSIZE(indexPath)))
            {
                WCHAR fileUri[MAX_PATH + 64];
                BuildFileUri(indexPath, fileUri, ARRAYSIZE(fileUri));
                WebViewUI_NavigateTo(fileUri);
            }
            else
            {
                /* No frontend files found — show a helpful message */
                WebViewUI_NavigateTo(L"about:blank");
            }
        }
        else
        {
            /* No project open, no file — show blank */
            WebViewUI_NavigateTo(L"about:blank");
        }
    }
}
