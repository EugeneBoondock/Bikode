#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void WebViewUI_Initialize(HWND hwndParent);
void WebViewUI_NavigateTo(LPCWSTR url);
void WebViewUI_Resize(RECT* prect);
void WebViewUI_Destroy();
BOOL WebViewUI_IsReady(void);

#ifdef __cplusplus
}
#endif
