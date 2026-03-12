#pragma once
/******************************************************************************
*
* Biko
*
* MissionControl.h
*   Dedicated Mission Control workspace for supervising multi-agent runs.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL MissionControl_Init(HWND hwndMain);
void MissionControl_Shutdown(void);

void MissionControl_Toggle(HWND hwndParent);
void MissionControl_Show(HWND hwndParent);
void MissionControl_Hide(void);
BOOL MissionControl_IsVisible(void);

void MissionControl_Layout(HWND hwndParent, int x, int y, int cx, int cy);
void MissionControl_ApplyTheme(void);
BOOL MissionControl_HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
HWND MissionControl_GetHwnd(void);

#ifdef __cplusplus
}
#endif
