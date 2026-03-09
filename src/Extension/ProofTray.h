#pragma once
/******************************************************************************
*
* Biko
*
* ProofTray.h
*   Delta Mesh proof tray and mission queue surface.
*
******************************************************************************/

#include <wtypes.h>
#include "AIBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDC_PROOFTRAY_PANEL     0xFB50

BOOL    ProofTray_Init(HWND hwndMain);
void    ProofTray_Shutdown(void);

void    ProofTray_Toggle(HWND hwndParent);
void    ProofTray_Show(HWND hwndParent);
void    ProofTray_Hide(void);
BOOL    ProofTray_IsVisible(void);

int     ProofTray_Layout(HWND hwndParent, int parentWidth, int parentBottom);
HWND    ProofTray_GetPanelHwnd(void);
void    ProofTray_ApplyDarkMode(void);

void    ProofTray_Publish(const AIResponse* pResp,
                          const AIPatch* pPatches,
                          int iPatchCount,
                          BOOL bPreviewActive);
void    ProofTray_Clear(void);
void    ProofTray_SetMissionStatus(LPCWSTR wszStatus);

#ifdef __cplusplus
}
#endif
