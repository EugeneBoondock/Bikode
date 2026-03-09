#pragma once
/******************************************************************************
*
* Biko
*
* BikoCommandPalette.h
*   Lightweight command palette overlay for Bikode shell actions.
*
******************************************************************************/

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void BikoCommandPalette_Show(HWND hwndParent);
void BikoCommandPalette_Hide(void);
void BikoCommandPalette_ApplyTheme(void);
BOOL BikoCommandPalette_IsVisible(void);

#ifdef __cplusplus
}
#endif
