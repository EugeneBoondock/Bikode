#include "WebViewUI.h"
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <string>

using namespace Microsoft::WRL;

// Global WebView2 pointers
static ComPtr<ICoreWebView2Controller> g_webviewController;
static ComPtr<ICoreWebView2> g_webview;
static HWND g_hwndParent = NULL;
static BOOL g_initComplete = FALSE;
static LPCWSTR g_pendingUrl = NULL;
static WCHAR g_pendingUrlBuf[MAX_PATH + 64];

extern "C" BOOL WebViewUI_IsReady(void) {
    return g_initComplete && g_webview != nullptr;
}

extern "C" void WebViewUI_Resize(RECT* prect) {
    if (g_webviewController != nullptr && prect != nullptr) {
        g_webviewController->put_Bounds(*prect);
    }
}

extern "C" void WebViewUI_NavigateTo(LPCWSTR url) {
    if (url == NULL) return;
    if (g_webview != nullptr && g_initComplete) {
        g_webview->Navigate(url);
    } else {
        /* WebView2 init is async; stash URL for when it completes */
        wcsncpy_s(g_pendingUrlBuf, ARRAYSIZE(g_pendingUrlBuf), url, _TRUNCATE);
        g_pendingUrl = g_pendingUrlBuf;
    }
}

extern "C" void WebViewUI_Destroy() {
    g_pendingUrl = NULL;
    g_initComplete = FALSE;
    if (g_webviewController != nullptr) {
        g_webviewController->Close();
        g_webviewController = nullptr;
        g_webview = nullptr;
    }
    g_hwndParent = NULL;
}

extern "C" void WebViewUI_Initialize(HWND hwndParent) {
    if (hwndParent == NULL || !IsWindow(hwndParent))
        return;

    g_hwndParent = hwndParent;
    g_initComplete = FALSE;
    g_pendingUrl = NULL;

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || env == nullptr) return E_FAIL;
                if (g_hwndParent == NULL || !IsWindow(g_hwndParent)) return E_FAIL;

                env->CreateCoreWebView2Controller(g_hwndParent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || controller == nullptr) return S_OK;
                            if (g_hwndParent == NULL || !IsWindow(g_hwndParent)) return S_OK;

                            g_webviewController = controller;
                            g_webviewController->get_CoreWebView2(&g_webview);

                            if (g_webview == nullptr) {
                                g_webviewController = nullptr;
                                return S_OK;
                            }

                            // Add a few settings for the local UI
                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            // Resize to parent bounds
                            RECT bounds;
                            GetClientRect(g_hwndParent, &bounds);
                            g_webviewController->put_Bounds(bounds);

                            g_initComplete = TRUE;

                            // Navigate to pending URL if one was queued
                            if (g_pendingUrl != NULL && g_pendingUrl[0] != L'\0') {
                                g_webview->Navigate(g_pendingUrl);
                                g_pendingUrl = NULL;
                            } else {
                                g_webview->Navigate(L"about:blank");
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}
