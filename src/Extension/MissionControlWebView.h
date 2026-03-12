#pragma once

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL MissionControlWebView_Initialize(HWND hwndParent);
void MissionControlWebView_Destroy(void);
void MissionControlWebView_Resize(const RECT* prc);
void MissionControlWebView_SetHtml(LPCWSTR wszHtml);
BOOL MissionControlWebView_IsReady(void);

#ifdef __cplusplus
}
#endif
