/******************************************************************************
*
* Biko
*
* DarkMode.c
*   Windows dark mode implementation.
*   Uses undocumented uxtheme APIs for Win10 1809+ dark mode,
*   DwmSetWindowAttribute for dark title bar.
*
******************************************************************************/

#include "DarkMode.h"
#include "ChatPanel.h"
#include "Terminal.h"
#include "BikoToolbar.h"
#include "CommonUtils.h"
#include "ComicTheme.h"
#include "Externals.h"
#include "ui/theme/BikodeTheme.h"
#include "Scintilla.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include "resource.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

//=============================================================================
// Dark mode color scheme
//=============================================================================

static const DarkModeColors s_darkColors = {
    .clrBackground  = RGB(24, 24, 24),       // biko-bg #181818
    .clrSurface     = RGB(36, 36, 36),       // biko-surface #242424
    .clrText        = RGB(230, 230, 230),    // biko-text1 #E6E6E6
    .clrTextDim     = RGB(80, 80, 80),       // biko-muted #505050
    .clrBorder      = RGB(55, 55, 55),       // biko-border #373737
    .clrAccent      = RGB(75, 139, 245),     // biko-accent #4B8BF5
    .clrSelection   = RGB(30, 55, 97),       // accent at ~20%
    .clrCaretLine   = RGB(30, 30, 30),       // subtle lift
    .clrLineNumber  = RGB(80, 80, 80),       // biko-muted #505050
    .clrIndentGuide = RGB(50, 50, 50),       // biko-divider #323232
    .clrComment     = RGB(150, 150, 150),    // biko-text2 #969696
    .clrKeyword     = RGB(75, 139, 245),     // biko-accent #4B8BF5
    .clrString      = RGB(255, 155, 48),
    .clrNumber      = RGB(31, 227, 138),
    .clrOperator    = RGB(230, 230, 230),    // biko-text1
    .clrType        = RGB(100, 160, 255),    // biko-accent2 #64A0FF
    .clrFunction    = RGB(255, 212, 0),
    .clrPreprocessor = RGB(255, 91, 91),
};

static const DarkModeColors s_lightColors = {
    .clrBackground  = RGB(248, 244, 236),     // warm Claude-like cream
    .clrSurface     = RGB(240, 234, 223),     // lifted parchment
    .clrText        = RGB(54, 46, 36),        // warm ink
    .clrTextDim     = RGB(159, 148, 135),     // muted taupe
    .clrBorder      = RGB(201, 191, 176),     // soft paper edge
    .clrAccent      = RGB(79, 122, 214),      // cobalt accent
    .clrSelection   = RGB(206, 223, 246),     // cool selection wash
    .clrCaretLine   = RGB(244, 239, 229),     // subtle highlight
    .clrLineNumber  = RGB(140, 128, 113),
    .clrIndentGuide = RGB(214, 205, 191),
    .clrComment     = RGB(106, 130, 97),
    .clrKeyword     = RGB(79, 122, 214),
    .clrString      = RGB(176, 103, 67),
    .clrNumber      = RGB(53, 150, 111),
    .clrOperator    = RGB(54, 46, 36),
    .clrType        = RGB(97, 120, 171),
    .clrFunction    = RGB(181, 140, 41),
    .clrPreprocessor = RGB(179, 92, 132),
};

//=============================================================================
// DWM dark mode â€” documented since Windows 10 20H1 (build 18985)
//=============================================================================

// DWMWA_USE_IMMERSIVE_DARK_MODE â€” attribute 20 (19 on older builds)
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#endif

//=============================================================================
// Undocumented uxtheme ordinals for AllowDarkModeForWindow etc.
//=============================================================================

typedef enum _PREFERRED_APP_MODE {
    PAM_DEFAULT = 0,
    PAM_ALLOW_DARK = 1,
    PAM_FORCE_DARK = 2,
    PAM_FORCE_LIGHT = 3,
    PAM_MAX = 4
} PREFERRED_APP_MODE;

// Function pointer types for undocumented uxtheme APIs
typedef BOOL (WINAPI *pfnAllowDarkModeForWindow)(HWND, BOOL);
typedef PREFERRED_APP_MODE (WINAPI *pfnSetPreferredAppMode)(PREFERRED_APP_MODE);
typedef void (WINAPI *pfnRefreshImmersiveColorPolicyState)(void);
typedef BOOL (WINAPI *pfnShouldAppsUseDarkMode)(void);

//=============================================================================
// Internal state
//=============================================================================

static BOOL s_bInitialized = FALSE;
static BOOL s_bEnabled = TRUE;
static BOOL s_bSupported = FALSE;
static HWND s_hwndMain = NULL;
static HBRUSH s_hBackgroundBrush = NULL;
static HICON s_hIconDark = NULL;   // biko_white.ico (IDR_MAINWND)
static HICON s_hIconLight = NULL;  // biko_black.ico (IDI_BIKO_LIGHT)

// Resolved undocumented functions
static pfnAllowDarkModeForWindow    s_pfnAllowDark = NULL;
static pfnSetPreferredAppMode       s_pfnSetAppMode = NULL;
static pfnRefreshImmersiveColorPolicyState s_pfnRefreshPolicy = NULL;
static pfnShouldAppsUseDarkMode     s_pfnShouldUseDark = NULL;
static HMODULE                      s_hUxTheme = NULL;

// Forward declarations
static void DarkMode_UpdateIcon(void);
static void DarkMode_SavePreference(void);

#define DARKMODE_INI_SECTION L"Settings2"
#define DARKMODE_INI_KEY     L"DarkMode"

//=============================================================================
// Internal helpers
//=============================================================================

static BOOL IsWindows10BuildOrGreater(DWORD build)
{
    OSVERSIONINFOEXW osvi;
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    osvi.dwMajorVersion = 10;
    osvi.dwMinorVersion = 0;
    osvi.dwBuildNumber = build;

    DWORDLONG condMask = 0;
    VER_SET_CONDITION(condMask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    VER_SET_CONDITION(condMask, VER_MINORVERSION, VER_GREATER_EQUAL);
    VER_SET_CONDITION(condMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

    return VerifyVersionInfoW(&osvi,
        VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER,
        condMask);
}

static void ResolveUxThemeAPIs(void)
{
    s_hUxTheme = GetModuleHandleW(L"uxtheme.dll");
    if (!s_hUxTheme)
        s_hUxTheme = LoadLibraryW(L"uxtheme.dll");

    if (!s_hUxTheme) return;

    // Ordinal 133: AllowDarkModeForWindow (Win10 1809+)
    s_pfnAllowDark = (pfnAllowDarkModeForWindow)
        GetProcAddress(s_hUxTheme, MAKEINTRESOURCEA(133));

    // Ordinal 135: SetPreferredAppMode (Win10 1903+) or AllowDarkModeForApp (1809)
    s_pfnSetAppMode = (pfnSetPreferredAppMode)
        GetProcAddress(s_hUxTheme, MAKEINTRESOURCEA(135));

    // Ordinal 104: RefreshImmersiveColorPolicyState
    s_pfnRefreshPolicy = (pfnRefreshImmersiveColorPolicyState)
        GetProcAddress(s_hUxTheme, MAKEINTRESOURCEA(104));

    // Ordinal 132: ShouldAppsUseDarkMode
    s_pfnShouldUseDark = (pfnShouldAppsUseDarkMode)
        GetProcAddress(s_hUxTheme, MAKEINTRESOURCEA(132));
}

static BOOL LoadDarkModePreference(void)
{
    if (szIniFile[0])
    {
        return GetPrivateProfileIntW(DARKMODE_INI_SECTION, DARKMODE_INI_KEY, 1, szIniFile) != 0;
    }
    return TRUE;
}

static void ApplyPreferredAppMode(void)
{
    if (s_bSupported && s_pfnSetAppMode)
    {
        s_pfnSetAppMode(s_bEnabled ? PAM_FORCE_DARK : PAM_FORCE_LIGHT);
    }
}

static void ApplyDarkTitleBar(HWND hwnd, BOOL bDark)
{
    BOOL useDark = bDark;

    // Try the documented attribute first (20H1+)
    HRESULT hr = DwmSetWindowAttribute(hwnd,
        DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));

    // Fallback to older attribute for 1809/1903
    if (FAILED(hr))
    {
        DwmSetWindowAttribute(hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &useDark, sizeof(useDark));
    }
}

static void UpdateBackgroundBrush(void)
{
    if (s_hBackgroundBrush)
        DeleteObject(s_hBackgroundBrush);

    const DarkModeColors* pColors = s_bEnabled ? &s_darkColors : &s_lightColors;
    s_hBackgroundBrush = CreateSolidBrush(pColors->clrSurface);
}

//=============================================================================
// Public: Lifecycle
//=============================================================================

void DarkMode_Init(HWND hwndMain)
{
    if (s_bInitialized) return;

    s_hwndMain = hwndMain;
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtr(hwndMain, GWLP_HINSTANCE);

    // Load both icon variants
    s_hIconDark  = LoadIcon(hInst, MAKEINTRESOURCE(IDR_MAINWND));     // white logo
    s_hIconLight = LoadIcon(hInst, MAKEINTRESOURCE(IDI_BIKO_LIGHT));  // black logo

    // Check Windows 10 1809+ (build 17763)
    s_bSupported = IsWindows10BuildOrGreater(17763);

    if (s_bSupported)
    {
        ResolveUxThemeAPIs();
    }

    s_bEnabled = LoadDarkModePreference();
    ApplyPreferredAppMode();

    UpdateBackgroundBrush();
    s_bInitialized = TRUE;

    // [biko]: Initialize the Comic Theme
    BikodeTheme_Init();
    ComicTheme_Init();
    DarkMode_UpdateIcon();
}

static void DarkMode_UpdateIcon(void)
{
    if (!s_hwndMain) return;
    HICON hIcon = s_bEnabled ? s_hIconDark : s_hIconLight;
    if (hIcon)
    {
        SendMessage(s_hwndMain, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(s_hwndMain, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
}

void DarkMode_Shutdown(void)
{
    if (s_hBackgroundBrush)
    {
        DeleteObject(s_hBackgroundBrush);
        s_hBackgroundBrush = NULL;
    }
    BikodeTheme_Shutdown();
    s_bInitialized = FALSE;
}

//=============================================================================
// Public: State
//=============================================================================

void DarkMode_Toggle(void)
{
    DarkMode_SetEnabled(!s_bEnabled);
}

BOOL DarkMode_IsEnabled(void)
{
    return s_bEnabled;
}

BOOL DarkMode_IsSupported(void)
{
    return s_bSupported;
}

void DarkMode_SetEnabled(BOOL bEnable)
{
    s_bEnabled = bEnable ? TRUE : FALSE;
    ApplyPreferredAppMode();
    UpdateBackgroundBrush();
    DarkMode_UpdateIcon();
    DarkMode_SavePreference();

    if (s_hwndMain)
    {
        DarkMode_ApplyToWindow(s_hwndMain);

        // Force repaint of entire window
        RedrawWindow(s_hwndMain, NULL, NULL,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        UpdateWindow(s_hwndMain);
    }
}

void DarkMode_ApplyAll(HWND hwndMain, HWND hwndEdit,
                       HWND hwndToolbar, HWND hwndStatus,
                       HWND hwndReBar)
{
    if (hwndMain)  DarkMode_ApplyToWindow(hwndMain);
    if (hwndEdit)  DarkMode_ApplyToEditor(hwndEdit);
    if (hwndToolbar) DarkMode_ApplyToToolbar(hwndToolbar);
    if (hwndStatus)  DarkMode_ApplyToStatusBar(hwndStatus);

    // ReBar (toolbar container)
    if (hwndReBar && s_bSupported)
    {
        if (s_pfnAllowDark)
            s_pfnAllowDark(hwndReBar, s_bEnabled);
        SetWindowTheme(hwndReBar, s_bEnabled ? L"DarkMode_Explorer" : NULL, NULL);
        InvalidateRect(hwndReBar, NULL, TRUE);
    }

    // [biko]: Apply Comic Theme to editor, toolbar, status
    ComicTheme_ApplyAll(hwndMain, hwndEdit, hwndToolbar, hwndStatus);

    // Refresh Chat Panel dark mode
    ChatPanel_ApplyDarkMode();

    // Refresh Terminal panel dark mode
    Terminal_ApplyDarkMode();

    // Refresh custom toolbar
    BikoToolbar_ApplyDarkMode();

    // Re-apply Scintilla styles after dark mode change so lexer knows
    if (hwndEdit)
        SendMessage(hwndEdit, SCI_COLOURISE, 0, -1);

    if (hwndMain)
    {
        RedrawWindow(hwndMain, NULL, NULL,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        UpdateWindow(hwndMain);
    }
}

//=============================================================================
// Public: Theme application
//=============================================================================

void DarkMode_ApplyToWindow(HWND hwnd)
{
    if (!s_bSupported) return;

    // Dark title bar
    ApplyDarkTitleBar(hwnd, s_bEnabled);

    // Allow dark mode for the window
    if (s_pfnAllowDark)
        s_pfnAllowDark(hwnd, s_bEnabled);

    // Set window theme to dark explorer
    if (s_bEnabled)
        SetWindowTheme(hwnd, L"DarkMode_Explorer", NULL);
    else
        SetWindowTheme(hwnd, NULL, NULL);

    // Refresh
    if (s_pfnRefreshPolicy)
        s_pfnRefreshPolicy();
}

void DarkMode_ApplyToEditor(HWND hwndEdit)
{
    if (!hwndEdit) return;

    // [biko]: Delegate entirely to the Comic Theme
    ComicTheme_ApplyToEditor(hwndEdit);

    // Remove any Win32 theme border on the editor
    SetWindowLongPtr(hwndEdit, GWL_EXSTYLE,
        GetWindowLongPtr(hwndEdit, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowLongPtr(hwndEdit, GWL_STYLE,
        GetWindowLongPtr(hwndEdit, GWL_STYLE) & ~(WS_HSCROLL | WS_VSCROLL | WS_BORDER));
    SetWindowPos(hwndEdit, NULL, 0, 0, 0, 0,
        SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
}

void DarkMode_ApplyToToolbar(HWND hwndToolbar)
{
    if (!hwndToolbar || !s_bSupported) return;

    if (s_bEnabled)
    {
        SetWindowTheme(hwndToolbar, L"DarkMode_Explorer", NULL);
        if (s_pfnAllowDark)
            s_pfnAllowDark(hwndToolbar, TRUE);
    }
    else
    {
        SetWindowTheme(hwndToolbar, NULL, NULL);
        if (s_pfnAllowDark)
            s_pfnAllowDark(hwndToolbar, FALSE);
    }
    InvalidateRect(hwndToolbar, NULL, TRUE);
}

void DarkMode_ApplyToStatusBar(HWND hwndStatus)
{
    if (!hwndStatus || !s_bSupported) return;

    if (s_bEnabled)
    {
        SetWindowTheme(hwndStatus, L"DarkMode_Explorer", NULL);
        if (s_pfnAllowDark)
            s_pfnAllowDark(hwndStatus, TRUE);
    }
    else
    {
        SetWindowTheme(hwndStatus, NULL, NULL);
        if (s_pfnAllowDark)
            s_pfnAllowDark(hwndStatus, FALSE);
    }
    InvalidateRect(hwndStatus, NULL, TRUE);
}

void DarkMode_ApplyToDialog(HWND hwndDlg)
{
    if (!hwndDlg || !s_bSupported) return;

    if (s_bEnabled)
    {
        ApplyDarkTitleBar(hwndDlg, TRUE);
        if (s_pfnAllowDark)
            s_pfnAllowDark(hwndDlg, TRUE);
        SetWindowTheme(hwndDlg, L"DarkMode_Explorer", NULL);
    }
    else
    {
        ApplyDarkTitleBar(hwndDlg, FALSE);
        if (s_pfnAllowDark)
            s_pfnAllowDark(hwndDlg, FALSE);
        SetWindowTheme(hwndDlg, NULL, NULL);
    }
}

//=============================================================================
// Public: Drawing helpers
//=============================================================================

COLORREF DarkMode_GetBackgroundColor(void)
{
    return s_bEnabled ? s_darkColors.clrBackground : s_lightColors.clrBackground;
}

COLORREF DarkMode_GetTextColor(void)
{
    return s_bEnabled ? s_darkColors.clrText : s_lightColors.clrText;
}

COLORREF DarkMode_GetAccentColor(void)
{
    return s_bEnabled ? s_darkColors.clrAccent : s_lightColors.clrAccent;
}

HBRUSH DarkMode_GetBackgroundBrush(void)
{
    return s_hBackgroundBrush;
}

const DarkModeColors* DarkMode_GetColors(void)
{
    return s_bEnabled ? &s_darkColors : &s_lightColors;
}

HBRUSH DarkMode_HandleCtlColor(HDC hdc)
{
    const DarkModeColors* pColors = DarkMode_GetColors();
    SetTextColor(hdc, pColors->clrText);
    SetBkColor(hdc, pColors->clrSurface);
    return s_hBackgroundBrush;
}

static void DarkMode_SavePreference(void)
{
    if (szIniFile[0])
    {
        WritePrivateProfileStringW(DARKMODE_INI_SECTION, DARKMODE_INI_KEY,
            s_bEnabled ? L"1" : L"0", szIniFile);
    }
}
