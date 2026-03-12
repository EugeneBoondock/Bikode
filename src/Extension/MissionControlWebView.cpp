#include "MissionControlWebView.h"
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl/client.h>
#include <wrl/event.h>

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static bool g_ready = false;

extern "C" BOOL MissionControlWebView_Initialize(HWND hwndParent) {
    if (g_controller != nullptr || !hwndParent) {
        return g_controller != nullptr;
    }
    g_ready = false;
    CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwndParent](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || env == nullptr) {
                    return result;
                }
                return env->CreateCoreWebView2Controller(hwndParent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwndParent](HRESULT result2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result2) || controller == nullptr) {
                                return result2;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            if (g_webview != nullptr) {
                                ComPtr<ICoreWebView2Settings> settings;
                                g_webview->get_Settings(&settings);
                                if (settings != nullptr) {
                                    settings->put_IsScriptEnabled(TRUE);
                                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                                    settings->put_IsZoomControlEnabled(FALSE);
                                }
                                RECT rc;
                                GetClientRect(hwndParent, &rc);
                                g_controller->put_Bounds(rc);
                                g_webview->NavigateToString(L"<html><body></body></html>");
                                g_ready = true;
                            }
                            return S_OK;
                        }).Get());
            }).Get());
    return TRUE;
}

extern "C" void MissionControlWebView_Destroy(void) {
    if (g_controller != nullptr) {
        g_controller->Close();
    }
    g_controller = nullptr;
    g_webview = nullptr;
    g_ready = false;
}

extern "C" void MissionControlWebView_Resize(const RECT* prc) {
    if (g_controller != nullptr && prc != nullptr) {
        g_controller->put_Bounds(*prc);
    }
}

extern "C" void MissionControlWebView_SetHtml(LPCWSTR wszHtml) {
    if (g_webview != nullptr && wszHtml != nullptr) {
        g_webview->NavigateToString(wszHtml);
    }
}

extern "C" BOOL MissionControlWebView_IsReady(void) {
    return g_ready ? TRUE : FALSE;
}
