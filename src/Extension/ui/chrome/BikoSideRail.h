#pragma once
/******************************************************************************
*
* Biko
*
* BikoSideRail.h
*   Slim utility rail for explorer, search, symbols, git, agents, plugins,
*   and settings actions.
*
******************************************************************************/

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL BikoSideRail_Init(HWND hwndMain);
void BikoSideRail_Shutdown(void);
int  BikoSideRail_Layout(HWND hwndParent, int* px, int* pcx, int y, int cy);
void BikoSideRail_ApplyTheme(void);
HWND BikoSideRail_GetHwnd(void);

#ifdef __cplusplus
}
#endif
