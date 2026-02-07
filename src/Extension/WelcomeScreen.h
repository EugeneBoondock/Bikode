
#pragma once
/******************************************************************************
*
* Biko
*
* WelcomeScreen.h
*   Welcome Screen overlay on startup when no files are open.
*
******************************************************************************/

#include <windows.h>

//=============================================================================
// Lifecycle
//=============================================================================

BOOL    WelcomeScreen_Init(HWND hwndMain);
void    WelcomeScreen_Shutdown(void);

//=============================================================================
// Visibility
//=============================================================================

void    WelcomeScreen_Show(HWND hwndParent);
void    WelcomeScreen_Hide(void);
BOOL    WelcomeScreen_IsVisible(void);

//=============================================================================
// Events
//=============================================================================

void    WelcomeScreen_OnResize(HWND hwndParent, int cx, int cy);
