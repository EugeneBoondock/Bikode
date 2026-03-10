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

extern "C" void WebViewUI_Resize(RECT* prect) {
    if (g_webviewController != nullptr && prect != nullptr) {
        g_webviewController->put_Bounds(*prect);
    }
}

extern "C" void WebViewUI_NavigateTo(LPCWSTR url) {
    if (g_webview != nullptr) {
        g_webview->Navigate(url);
    }
}

extern "C" void WebViewUI_Destroy() {
    if (g_webviewController != nullptr) {
        g_webviewController->Close();
        g_webviewController = nullptr;
        g_webview = nullptr;
    }
}

extern "C" void WebViewUI_Initialize(HWND hwndParent) {
    g_hwndParent = hwndParent;

    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwndParent](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (env == nullptr) return E_FAIL;
                
                env->CreateCoreWebView2Controller(hwndParent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwndParent](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (controller != nullptr) {
                                g_webviewController = controller;
                                g_webviewController->get_CoreWebView2(&g_webview);

                                // Add a few settings for the local UI
                                ComPtr<ICoreWebView2Settings> settings;
                                g_webview->get_Settings(&settings);
                                settings->put_IsScriptEnabled(TRUE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);

                                // Resize to parent bounds
                                RECT bounds;
                                GetClientRect(hwndParent, &bounds);
                                g_webviewController->put_Bounds(bounds);

                                // For now, load a local dummy HTML or navigate to a dev server
                                g_webview->Navigate(L"about:blank");
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}
