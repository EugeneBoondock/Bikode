/******************************************************************************
*
* Biko — Embedded Terminal  (complete rewrite, Feb 2026)
*
* Terminal.c
*
*   Architecture (simple and correct):
*   ==================================
*   Panel   — "BikoTermPanel" child of main window. Header bar + splitter.
*   View    — "BikoTermView" child of panel. Custom-drawn char grid.
*             Owns its WndProc. Keyboard goes WM_CHAR → UTF-8 → pipe → shell.
*   ConPTY  — Win10 1809+ Pseudo Console. Falls back to raw pipes.
*   Reader  — background thread reads shell stdout, posts to panel.
*   VT      — minimal ANSI parser updates character grid.
*
* IMPORTANT BUILD MARKER: 2026-02-08-v5-CLEAN-REWRITE
*
******************************************************************************/

#include "Terminal.h"
#include "ui/theme/BikodeTheme.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include <uxtheme.h>
#include <shlwapi.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")

/* ═══════════════════════════════════════════════════════════════════
 * ConPTY function pointers (Win10 1809+)
 * ═══════════════════════════════════════════════════════════════════ */
typedef VOID* HPCON;
typedef HRESULT(WINAPI* PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT(WINAPI* PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void   (WINAPI* PFN_ClosePseudoConsole)(HPCON);

static PFN_CreatePseudoConsole pfnCreatePC  = NULL;
static PFN_ResizePseudoConsole pfnResizePC  = NULL;
static PFN_ClosePseudoConsole  pfnClosePC   = NULL;
static BOOL g_havePTY = FALSE;

/* ═══════════════════════════════════════════════════════════════════
 * Constants
 * ═══════════════════════════════════════════════════════════════════ */
#define PANEL_HEIGHT_DEFAULT  180
#define SPLITTER_H            3
#define HEADER_H              34
#define TIMER_HEALTH          1
#define TIMER_BLINK           2
#define WM_TERMDATA           (WM_USER + 200)

/* colours */
#define C_DKBG     RGB(8,11,17)
#define C_DKFG     RGB(243,245,247)
#define C_DKHDR    RGB(23,28,36)
#define C_DKSPLIT  RGB(42,49,64)
#define C_DKTXT    RGB(243,245,247)
#define C_DKDIM    RGB(168,179,194)
#define C_DKROWALT RGB(10,14,20)
#define C_DKSEL    RGB(22,47,63)
#define C_DKCARET  RGB(255,212,0)
#define C_DKBTN    RGB(29,36,48)
#define C_DKBORD   RGB(42,49,64)
#define C_DKDROP   RGB(17,21,28)

#define C_LTBG     RGB(255,255,255)
#define C_LTFG     RGB(17,24,39)
#define C_LTHDR    RGB(243,244,246)
#define C_LTSPLIT  RGB(229,231,235)
#define C_LTTXT    RGB(17,24,39)
#define C_LTDIM    RGB(107,114,128)
#define C_LTBTN    RGB(229,231,235)
#define C_LTBORD   RGB(209,213,219)
#define C_LTDROP   RGB(255,255,255)

/* ANSI 16-colour palette (dark theme) */
static COLORREF g_ansi[16] = {
    RGB(12,12,12),    RGB(197,15,31),   RGB(19,161,14),  RGB(193,156,0),
    RGB(0,55,218),    RGB(136,23,152),  RGB(58,150,221), RGB(204,204,204),
    RGB(118,118,118), RGB(231,72,86),   RGB(22,198,12),  RGB(249,241,165),
    RGB(59,120,255),  RGB(180,0,158),   RGB(97,214,214), RGB(242,242,242)
};

/* ═══════════════════════════════════════════════════════════════════
 * Shell table
 * ═══════════════════════════════════════════════════════════════════ */
typedef enum { SH_PS=0, SH_CMD, SH_GIT, SH_WSL, SH_COUNT } ShellKind;

static struct {
    const WCHAR *label;
    WCHAR path[MAX_PATH];
    BOOL  found;
} g_shells[SH_COUNT] = {
    { L"PowerShell", {0}, FALSE },
    { L"CMD",        {0}, FALSE },
    { L"Git Bash",   {0}, FALSE },
    { L"Bash (WSL)", {0}, FALSE },
};
static ShellKind g_curShell = SH_PS;

/* ═══════════════════════════════════════════════════════════════════
 * Character Grid
 * ═══════════════════════════════════════════════════════════════════ */
#define GRID_MAXLINES 4000

typedef struct {
    WCHAR ch;
    BYTE  fg, bg, attr;
} Cell;

typedef struct {
    Cell *buf;
    int   cols, rows;
    int   maxLines;
    int   used;          /* total lines allocated */
    int   curR, curC;    /* cursor position (screen-relative) */
    int   scrollOff;     /* scrollback offset (0=bottom) */
    /* VT parser */
    BOOL  inEsc;
    char  escBuf[256];
    int   escLen;
    BYTE  fg, bg;
    BOOL  bold;
    int   savedR, savedC;
} Grid;

static int GridBase(Grid *g) {
    int b = g->used - g->rows;
    return b < 0 ? 0 : b;
}

static Cell* GridCell(Grid *g, int screenRow, int col) {
    int line = GridBase(g) + screenRow;
    if (line < 0) line = 0;
    if (line >= g->used) line = g->used - 1;
    if (col < 0) col = 0;
    if (col >= g->cols) col = g->cols - 1;
    return &g->buf[line * g->cols + col];
}

static void Grid_ClampCursor(Grid *g) {
    if (g->curR < 0) g->curR = 0;
    if (g->curR >= g->rows) g->curR = g->rows - 1;
    if (g->curC < 0) g->curC = 0;
    if (g->curC >= g->cols) g->curC = g->cols - 1;
}

static void Grid_ClearRow(Grid *g, int screenRow) {
    int line = GridBase(g) + screenRow;
    if (line < 0 || line >= g->used) return;
    Cell *r = &g->buf[line * g->cols];
    for (int c = 0; c < g->cols; c++) {
        r[c].ch = L' '; r[c].fg = 7; r[c].bg = 0; r[c].attr = 0;
    }
}

static void Grid_Compact(Grid *g) {
    if (g->used < g->maxLines) return;
    int drop = g->maxLines / 2;
    memmove(g->buf, g->buf + drop * g->cols,
            (size_t)(g->used - drop) * g->cols * sizeof(Cell));
    g->used -= drop;
    g->scrollOff -= drop;
    if (g->scrollOff < 0) g->scrollOff = 0;
}

static void Grid_ScrollUp(Grid *g) {
    Grid_Compact(g);
    if (g->used < g->maxLines) {
        int nl = g->used++;
        Cell *r = &g->buf[nl * g->cols];
        for (int c = 0; c < g->cols; c++) {
            r[c].ch = L' '; r[c].fg = g->fg; r[c].bg = g->bg; r[c].attr = 0;
        }
    }
}

static Grid* Grid_Create(int cols, int rows) {
    Grid *g = (Grid*)n2e_Alloc(sizeof(Grid));
    if (!g) return NULL;
    ZeroMemory(g, sizeof(Grid));
    g->maxLines = GRID_MAXLINES;
    g->cols = cols > 0 ? cols : 80;
    g->rows = rows > 0 ? rows : 24;
    g->buf = (Cell*)n2e_Alloc((size_t)g->maxLines * g->cols * sizeof(Cell));
    if (!g->buf) { n2e_Free(g); return NULL; }
    g->used = g->rows;
    g->fg = 7; g->bg = 0;
    for (int i = 0; i < g->used * g->cols; i++) {
        g->buf[i].ch = L' '; g->buf[i].fg = 7; g->buf[i].bg = 0; g->buf[i].attr = 0;
    }
    return g;
}

static void Grid_Free(Grid *g) {
    if (!g) return;
    if (g->buf) n2e_Free(g->buf);
    n2e_Free(g);
}

static void Grid_Resize(Grid *g, int newCols, int newRows) {
    if (newCols <= 0 || newRows <= 0) return;
    if (newCols > 500) newCols = 500;
    if (newRows > 200) newRows = 200;
    if (newCols == g->cols && newRows == g->rows) return;

    Cell *nb = (Cell*)n2e_Alloc((size_t)g->maxLines * newCols * sizeof(Cell));
    if (!nb) return;
    for (int i = 0; i < g->maxLines * newCols; i++) {
        nb[i].ch = L' '; nb[i].fg = 7; nb[i].bg = 0; nb[i].attr = 0;
    }
    int mc = g->cols < newCols ? g->cols : newCols;
    int dl = 0;
    for (int sl = 0; sl < g->used && dl < g->maxLines; sl++, dl++) {
        for (int c = 0; c < mc; c++)
            nb[dl * newCols + c] = g->buf[sl * g->cols + c];
    }
    n2e_Free(g->buf);
    g->buf = nb;
    g->cols = newCols;
    g->rows = newRows;
    g->used = dl < newRows ? newRows : dl;
    Grid_ClampCursor(g);
    g->scrollOff = 0;
}

static void Grid_PutChar(Grid *g, WCHAR ch) {
    Cell *c = GridCell(g, g->curR, g->curC);
    c->ch = ch; c->fg = g->fg; c->bg = g->bg;
    c->attr = g->bold ? 1 : 0;
    g->curC++;
    if (g->curC >= g->cols) {
        g->curC = 0;
        if (g->curR >= g->rows - 1)
            Grid_ScrollUp(g);
        else
            g->curR++;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * VT/ANSI Parser
 * ═══════════════════════════════════════════════════════════════════ */

static void ParseCSIParams(const char *s, int len, int *params, int *np, int maxP) {
    *np = 0;
    int cur = 0; BOOL got = FALSE;
    for (int i = 0; i < len && *np < maxP; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            cur = cur * 10 + (s[i] - '0');
            got = TRUE;
        } else if (s[i] == ';') {
            params[(*np)++] = got ? cur : 0;
            cur = 0; got = FALSE;
        }
    }
    if (got || *np > 0) params[(*np)++] = cur;
}

static void HandleCSI(Grid *g, char final, const char *body, int bodyLen) {
    int p[16] = {0}; int np = 0;
    ParseCSIParams(body, bodyLen, p, &np, 16);

    switch (final) {
    case 'm': /* SGR */
        if (np == 0) { g->fg = 7; g->bg = 0; g->bold = FALSE; break; }
        for (int i = 0; i < np; i++) {
            int v = p[i];
            if (v == 0) { g->fg = 7; g->bg = 0; g->bold = FALSE; }
            else if (v == 1) g->bold = TRUE;
            else if (v == 22) g->bold = FALSE;
            else if (v >= 30 && v <= 37) g->fg = (BYTE)(v - 30);
            else if (v == 39) g->fg = 7;
            else if (v >= 40 && v <= 47) g->bg = (BYTE)(v - 40);
            else if (v == 49) g->bg = 0;
            else if (v >= 90 && v <= 97) g->fg = (BYTE)(v - 90 + 8);
            else if (v >= 100 && v <= 107) g->bg = (BYTE)(v - 100 + 8);
        }
        break;

    case 'A': g->curR -= np > 0 && p[0] > 0 ? p[0] : 1; Grid_ClampCursor(g); break;
    case 'B': g->curR += np > 0 && p[0] > 0 ? p[0] : 1; Grid_ClampCursor(g); break;
    case 'C': g->curC += np > 0 && p[0] > 0 ? p[0] : 1; Grid_ClampCursor(g); break;
    case 'D': g->curC -= np > 0 && p[0] > 0 ? p[0] : 1; Grid_ClampCursor(g); break;
    case 'H': case 'f':
        g->curR = (np > 0 && p[0] > 0 ? p[0] : 1) - 1;
        g->curC = (np > 1 && p[1] > 0 ? p[1] : 1) - 1;
        Grid_ClampCursor(g); break;
    case 'G':
        g->curC = (np > 0 && p[0] > 0 ? p[0] : 1) - 1;
        Grid_ClampCursor(g); break;
    case 'd':
        g->curR = (np > 0 && p[0] > 0 ? p[0] : 1) - 1;
        Grid_ClampCursor(g); break;

    case 'J': {
        int mode = np > 0 ? p[0] : 0;
        if (mode == 0) {
            for (int c = g->curC; c < g->cols; c++) GridCell(g, g->curR, c)->ch = L' ';
            for (int r = g->curR + 1; r < g->rows; r++) Grid_ClearRow(g, r);
        } else if (mode == 1) {
            for (int r = 0; r < g->curR; r++) Grid_ClearRow(g, r);
            for (int c = 0; c <= g->curC; c++) GridCell(g, g->curR, c)->ch = L' ';
        } else if (mode == 2 || mode == 3) {
            for (int r = 0; r < g->rows; r++) Grid_ClearRow(g, r);
            g->curR = 0; g->curC = 0;
        }
        break;
    }

    case 'K': {
        int mode = np > 0 ? p[0] : 0;
        int base = GridBase(g) + g->curR;
        if (base < 0 || base >= g->used) break;
        Cell *row = &g->buf[base * g->cols];
        if (mode == 0) { for (int c = g->curC; c < g->cols; c++) row[c].ch = L' '; }
        else if (mode == 1) { for (int c = 0; c <= g->curC; c++) row[c].ch = L' '; }
        else if (mode == 2) { for (int c = 0; c < g->cols; c++) row[c].ch = L' '; }
        break;
    }

    case 'S': { int n = np > 0 && p[0] > 0 ? p[0] : 1; for (int i = 0; i < n; i++) Grid_ScrollUp(g); break; }
    case 'T': break; /* scroll down — ignore */

    case 'L': { /* insert lines */
        int n = np > 0 && p[0] > 0 ? p[0] : 1;
        for (int i = 0; i < n && g->curR < g->rows; i++) Grid_ScrollUp(g);
        break;
    }
    case 'M': break; /* delete lines — ignore */
    case 'P': { /* delete chars */
        int n = np > 0 && p[0] > 0 ? p[0] : 1;
        int base = GridBase(g) + g->curR;
        if (base < 0 || base >= g->used) break;
        Cell *row = &g->buf[base * g->cols];
        for (int c = g->curC; c < g->cols - n; c++) row[c] = row[c + n];
        for (int c = g->cols - n; c < g->cols; c++) { row[c].ch = L' '; row[c].fg = g->fg; row[c].bg = g->bg; }
        break;
    }
    case '@': { /* insert chars */
        int n = np > 0 && p[0] > 0 ? p[0] : 1;
        int base = GridBase(g) + g->curR;
        if (base < 0 || base >= g->used) break;
        Cell *row = &g->buf[base * g->cols];
        for (int c = g->cols - 1; c >= g->curC + n; c--) row[c] = row[c - n];
        for (int c = g->curC; c < g->curC + n && c < g->cols; c++) { row[c].ch = L' '; row[c].fg = g->fg; row[c].bg = g->bg; }
        break;
    }
    case 'X': { /* erase chars */
        int n = np > 0 && p[0] > 0 ? p[0] : 1;
        for (int c = g->curC; c < g->curC + n && c < g->cols; c++) GridCell(g, g->curR, c)->ch = L' ';
        break;
    }

    case 'n': /* device status report */
        break; /* ignore for now */
    case 's': g->savedR = g->curR; g->savedC = g->curC; break;
    case 'u': g->curR = g->savedR; g->curC = g->savedC; Grid_ClampCursor(g); break;
    case 'r': break; /* scroll region — ignore */
    case 'h': case 'l': break; /* mode set/reset — ignore */
    }
}

static void Grid_ProcessVT(Grid *g, const char *data, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];

        if (g->inEsc) {
            g->escBuf[g->escLen++] = (char)ch;
            if (g->escLen >= 255) { g->inEsc = FALSE; g->escLen = 0; continue; }

            if (g->escLen == 1) {
                if (ch == '[') continue;       /* CSI start */
                if (ch == ']') continue;       /* OSC start */
                if (ch == '(' || ch == ')' || ch == '*' || ch == '+') continue;
                if (ch == '7') { g->savedR = g->curR; g->savedC = g->curC; }
                if (ch == '8') { g->curR = g->savedR; g->curC = g->savedC; Grid_ClampCursor(g); }
                if (ch == 'M') { if (g->curR > 0) g->curR--; }
                g->inEsc = FALSE; g->escLen = 0;
                continue;
            }

            /* CSI sequence: first byte was '[' */
            if (g->escBuf[0] == '[') {
                if ((ch >= 0x40 && ch <= 0x7E) && ch != '[') {
                    /* Final byte */
                    if (ch == '?') continue; /* private mode prefix, keep going */
                    HandleCSI(g, (char)ch, g->escBuf + 1, g->escLen - 1);
                    g->inEsc = FALSE; g->escLen = 0;
                }
                continue;
            }

            /* OSC sequence: ends with BEL or ST */
            if (g->escBuf[0] == ']') {
                if (ch == 0x07 || (ch == '\\' && g->escLen >= 2 && g->escBuf[g->escLen-2] == 0x1B)) {
                    g->inEsc = FALSE; g->escLen = 0;
                }
                continue;
            }

            /* Charset selection: consume one more byte */
            if (g->escBuf[0] == '(' || g->escBuf[0] == ')') {
                g->inEsc = FALSE; g->escLen = 0;
                continue;
            }

            g->inEsc = FALSE; g->escLen = 0;
            continue;
        }

        /* Not in escape */
        if (ch == 0x1B) { g->inEsc = TRUE; g->escLen = 0; continue; }
        if (ch == '\r')  { g->curC = 0; continue; }
        if (ch == '\n')  {
            if (g->curR >= g->rows - 1) Grid_ScrollUp(g);
            else g->curR++;
            continue;
        }
        if (ch == '\b')  { if (g->curC > 0) g->curC--; continue; }
        if (ch == '\t')  { g->curC = (g->curC + 8) & ~7; if (g->curC >= g->cols) g->curC = g->cols - 1; continue; }
        if (ch == '\a')  { continue; } /* bell */
        if (ch < 0x20)  { continue; } /* other control chars */

        /* Printable character — handle UTF-8 → wide */
        WCHAR wch;
        if (ch < 0x80) {
            wch = (WCHAR)ch;
        } else {
            /* Simple UTF-8 decode */
            char mb[4]; int mbLen = 0;
            mb[mbLen++] = (char)ch;
            if ((ch & 0xE0) == 0xC0) {
                if (i + 1 < len) mb[mbLen++] = data[++i];
            } else if ((ch & 0xF0) == 0xE0) {
                if (i + 1 < len) mb[mbLen++] = data[++i];
                if (i + 1 < len) mb[mbLen++] = data[++i];
            } else if ((ch & 0xF8) == 0xF0) {
                if (i + 1 < len) mb[mbLen++] = data[++i];
                if (i + 1 < len) mb[mbLen++] = data[++i];
                if (i + 1 < len) mb[mbLen++] = data[++i];
            }
            WCHAR wbuf[2] = {0};
            int r = MultiByteToWideChar(CP_UTF8, 0, mb, mbLen, wbuf, 2);
            wch = r > 0 ? wbuf[0] : L'?';
        }
        Grid_PutChar(g, wch);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Per-terminal state
 * ═══════════════════════════════════════════════════════════════════ */
typedef struct {
    HPCON          hPC;
    HANDLE         hProc, hThread;
    HANDLE         hPipeWr;     /* app writes here → shell stdin */
    HANDLE         hPipeRd;     /* app reads here  ← shell stdout */
    HANDLE         hReader;
    HWND           hwndView;
    volatile LONG  alive;
    ShellKind      shell;
    Grid          *grid;
} Term;

/* ═══════════════════════════════════════════════════════════════════
 * Global state
 * ═══════════════════════════════════════════════════════════════════ */
static HWND   g_hwndMain    = NULL;
static HWND   g_hwndPanel   = NULL;
static Term  *g_term        = NULL;
static BOOL   g_visible     = FALSE;
static BOOL   g_wantFocus   = FALSE;
static int    g_panelH      = PANEL_HEIGHT_DEFAULT;
static HFONT  g_fontHdr     = NULL;
static HFONT  g_fontDrop    = NULL;
static HFONT  g_fontGrid    = NULL;
static BOOL   g_caretOn     = TRUE;
static int    g_cellW       = 8;
static int    g_cellH       = 16;
static UINT   g_sessionCounter = 0;
static UINT   g_activeSessionId = 1;

static const WCHAR *CLS_PANEL = L"BikoTermPanel";
static const WCHAR *CLS_VIEW  = L"BikoTermView";
static BOOL g_regPanel = FALSE;
static BOOL g_regView  = FALSE;

/* Header button rects */
static RECT g_rcDrop = {0}, g_rcNew = {0}, g_rcClose = {0};
static BOOL g_hoverClose = FALSE, g_hoverNew = FALSE, g_hoverDrop = FALSE;

/* Selection state */
static BOOL g_selecting = FALSE, g_hasSel = FALSE;
static int  g_selSR, g_selSC, g_selER, g_selEC;

/* Forward declarations */
static LRESULT CALLBACK PanelProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
static DWORD   WINAPI   ReaderThread(LPVOID);
static BOOL    ShellCreate(Term*, COORD, ShellKind);
static void    ShellKill(Term*);
static void    ShellDetect(void);
static void    ShowShellMenu(HWND);

/* ═══════════════════════════════════════════════════════════════════
 * Window class registration
 * ═══════════════════════════════════════════════════════════════════ */
static void EnsureClasses(HINSTANCE hi) {
    if (!g_regPanel) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = CLS_PANEL;
        RegisterClassExW(&wc);
        g_regPanel = TRUE;
    }
    if (!g_regView) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
        wc.lpszClassName = CLS_VIEW;
        RegisterClassExW(&wc);
        g_regView = TRUE;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Font setup
 * ═══════════════════════════════════════════════════════════════════ */
static void EnsureFonts(void) {
    if (!g_fontHdr)
        g_fontHdr = CreateFontW(-13,0,0,0,FW_SEMIBOLD,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    if (!g_fontDrop)
        g_fontDrop = CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,
            DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    if (!g_fontGrid) {
        const WCHAR *try_fonts[] = { L"Cascadia Mono", L"Cascadia Code", L"Consolas", NULL };
        HDC hdc = GetDC(NULL);
        for (int i = 0; try_fonts[i]; i++) {
            g_fontGrid = CreateFontW(-14,0,0,0,FW_NORMAL,0,0,0,
                DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN, try_fonts[i]);
            if (hdc && g_fontGrid) {
                HFONT old = SelectObject(hdc, g_fontGrid);
                WCHAR face[64] = {0};
                GetTextFaceW(hdc, 64, face);
                SelectObject(hdc, old);
                if (_wcsicmp(face, try_fonts[i]) == 0) break;
                DeleteObject(g_fontGrid); g_fontGrid = NULL;
            }
        }
        if (!g_fontGrid)
            g_fontGrid = CreateFontW(-14,0,0,0,FW_NORMAL,0,0,0,
                DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,L"Consolas");
        if (hdc && g_fontGrid) {
            HFONT old = SelectObject(hdc, g_fontGrid);
            TEXTMETRICW tm;
            GetTextMetricsW(hdc, &tm);
            g_cellW = tm.tmAveCharWidth;
            g_cellH = tm.tmHeight + tm.tmExternalLeading;
            if (g_cellW < 4) g_cellW = 8;
            if (g_cellH < 6) g_cellH = 16;
            SelectObject(hdc, old);
        }
        if (hdc) ReleaseDC(NULL, hdc);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Shell detection
 * ═══════════════════════════════════════════════════════════════════ */
static void ShellDetect(void) {
    WCHAR p[MAX_PATH];
    /* PowerShell */
    if (SearchPathW(NULL,L"pwsh.exe",NULL,MAX_PATH,p,NULL))
        { wcscpy_s(g_shells[SH_PS].path,MAX_PATH,p); g_shells[SH_PS].found=TRUE; }
    else if (SearchPathW(NULL,L"powershell.exe",NULL,MAX_PATH,p,NULL))
        { wcscpy_s(g_shells[SH_PS].path,MAX_PATH,p); g_shells[SH_PS].found=TRUE; }
    /* CMD */
    { DWORD d=GetEnvironmentVariableW(L"COMSPEC",p,MAX_PATH);
      if (d>0&&d<MAX_PATH) { wcscpy_s(g_shells[SH_CMD].path,MAX_PATH,p); g_shells[SH_CMD].found=TRUE; }
      else if (SearchPathW(NULL,L"cmd.exe",NULL,MAX_PATH,p,NULL))
          { wcscpy_s(g_shells[SH_CMD].path,MAX_PATH,p); g_shells[SH_CMD].found=TRUE; }
    }
    /* Git Bash */
    { const WCHAR *gp[]={L"C:\\Program Files\\Git\\bin\\bash.exe",
                         L"C:\\Program Files (x86)\\Git\\bin\\bash.exe",NULL};
      BOOL f=FALSE;
      if (SearchPathW(NULL,L"bash.exe",NULL,MAX_PATH,p,NULL) && wcsstr(p,L"Git"))
          { wcscpy_s(g_shells[SH_GIT].path,MAX_PATH,p); g_shells[SH_GIT].found=TRUE; f=TRUE; }
      if (!f) for(int i=0;gp[i];i++)
          if (GetFileAttributesW(gp[i])!=INVALID_FILE_ATTRIBUTES)
              { wcscpy_s(g_shells[SH_GIT].path,MAX_PATH,gp[i]); g_shells[SH_GIT].found=TRUE; break; }
    }
    /* WSL */
    if (SearchPathW(NULL,L"wsl.exe",NULL,MAX_PATH,p,NULL))
        { wcscpy_s(g_shells[SH_WSL].path,MAX_PATH,p); g_shells[SH_WSL].found=TRUE; }
}

static ShellKind BestShell(void) {
    if (g_shells[SH_PS].found)  return SH_PS;
    if (g_shells[SH_CMD].found) return SH_CMD;
    if (g_shells[SH_GIT].found) return SH_GIT;
    if (g_shells[SH_WSL].found) return SH_WSL;
    return SH_CMD;
}

/* ═══════════════════════════════════════════════════════════════════
 * ConPTY init
 * ═══════════════════════════════════════════════════════════════════ */
static BOOL InitConPTY(void) {
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (!h) return FALSE;
    pfnCreatePC = (PFN_CreatePseudoConsole)GetProcAddress(h, "CreatePseudoConsole");
    pfnResizePC = (PFN_ResizePseudoConsole)GetProcAddress(h, "ResizePseudoConsole");
    pfnClosePC  = (PFN_ClosePseudoConsole) GetProcAddress(h, "ClosePseudoConsole");
    return (pfnCreatePC && pfnResizePC && pfnClosePC);
}

/* ═══════════════════════════════════════════════════════════════════
 * Shell process creation
 *
 * Pipe wiring for ConPTY:
 *   hPipeIn_R  ──→ ConPTY stdin  ──→ shell stdin
 *   hPipeIn_W  ← app writes here (this is t->hPipeWr)
 *   hPipeOut_R ← app reads here  (this is t->hPipeRd)
 *   hPipeOut_W ──→ ConPTY stdout ──→ shell stdout
 *
 * After CreatePseudoConsole, ConPTY dups the PTY-side handles,
 * so we close hPipeIn_R and hPipeOut_W immediately.
 * ═══════════════════════════════════════════════════════════════════ */
static BOOL ShellCreate(Term *t, COORD sz, ShellKind sh) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hPipeIn_R = NULL, hPipeIn_W = NULL;
    HANDLE hPipeOut_R = NULL, hPipeOut_W = NULL;

    if (!CreatePipe(&hPipeIn_R, &hPipeIn_W, &sa, 0))
        return FALSE;
    if (!CreatePipe(&hPipeOut_R, &hPipeOut_W, &sa, 0)) {
        CloseHandle(hPipeIn_R); CloseHandle(hPipeIn_W);
        return FALSE;
    }

    /* Save app-side handles */
    t->hPipeWr = hPipeIn_W;   /* app writes → shell stdin */
    t->hPipeRd = hPipeOut_R;  /* app reads  ← shell stdout */

    BOOL usedPTY = FALSE;
    if (g_havePTY) {
        HRESULT hr = pfnCreatePC(sz, hPipeIn_R, hPipeOut_W, 0, &t->hPC);
        if (SUCCEEDED(hr)) usedPTY = TRUE;
    }

    /* Build command line */
    WCHAR cmd[MAX_PATH*2] = {0};
    if (g_shells[sh].found && g_shells[sh].path[0]) {
        if (sh == SH_GIT) swprintf_s(cmd, MAX_PATH*2, L"\"%s\" --login -i", g_shells[sh].path);
        else wcscpy_s(cmd, MAX_PATH*2, g_shells[sh].path);
    } else {
        DWORD d = GetEnvironmentVariableW(L"COMSPEC", cmd, MAX_PATH);
        if (!d) wcscpy_s(cmd, MAX_PATH*2, L"cmd.exe");
    }

    WCHAR cwd[MAX_PATH] = {0};
    GetCurrentDirectoryW(MAX_PATH, cwd);

    STARTUPINFOEXW si; ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    BOOL ok = FALSE;

    if (usedPTY && t->hPC) {
        SIZE_T asz = 0;
        InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)n2e_Alloc(asz);
        if (si.lpAttributeList &&
            InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz)) {
            UpdateProcThreadAttribute(si.lpAttributeList, 0,
                0x00020016 /*PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE*/,
                t->hPC, sizeof(HPCON), NULL, NULL);
            ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                EXTENDED_STARTUPINFO_PRESENT, NULL, cwd[0] ? cwd : NULL,
                &si.StartupInfo, &pi);
        }
        if (si.lpAttributeList) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            n2e_Free(si.lpAttributeList);
        }
    }

    if (!ok && !usedPTY) {
        /* Fallback: raw pipe, no ConPTY */
        si.StartupInfo.cb       = sizeof(STARTUPINFOW);
        si.StartupInfo.dwFlags  = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.StartupInfo.hStdInput  = hPipeIn_R;
        si.StartupInfo.hStdOutput = hPipeOut_W;
        si.StartupInfo.hStdError  = hPipeOut_W;
        si.StartupInfo.wShowWindow = SW_HIDE;
        ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, cwd[0] ? cwd : NULL, &si.StartupInfo, &pi);
    }

    if (ok) {
        t->hProc = pi.hProcess;
        t->hThread = pi.hThread;
    }

    /* Close PTY-side pipe ends (ConPTY has duped them) */
    CloseHandle(hPipeIn_R);
    CloseHandle(hPipeOut_W);

    if (!ok) {
        CloseHandle(hPipeIn_W);
        CloseHandle(hPipeOut_R);
        t->hPipeWr = NULL;
        t->hPipeRd = NULL;
    }

    return ok;
}

/* ═══════════════════════════════════════════════════════════════════
 * Destroy terminal
 * ═══════════════════════════════════════════════════════════════════ */
static void ShellKill(Term *t) {
    if (!t) return;
    InterlockedExchange(&t->alive, FALSE);
    if (t->hPC && pfnClosePC) { pfnClosePC(t->hPC); t->hPC = NULL; }
    if (t->hPipeWr) { CloseHandle(t->hPipeWr); t->hPipeWr = NULL; }
    if (t->hPipeRd) { CloseHandle(t->hPipeRd); t->hPipeRd = NULL; }
    if (t->hReader) {
        WaitForSingleObject(t->hReader, 2000);
        CloseHandle(t->hReader); t->hReader = NULL;
    }
    if (t->hProc) { TerminateProcess(t->hProc, 0); CloseHandle(t->hProc); t->hProc = NULL; }
    if (t->hThread) { CloseHandle(t->hThread); t->hThread = NULL; }
    if (t->hwndView) { DestroyWindow(t->hwndView); t->hwndView = NULL; }
    if (t->grid) { Grid_Free(t->grid); t->grid = NULL; }
}

/* ═══════════════════════════════════════════════════════════════════
 * Reader thread — reads shell stdout, posts to panel
 * ═══════════════════════════════════════════════════════════════════ */
static DWORD WINAPI ReaderThread(LPVOID lp) {
    Term *t = (Term*)lp;
    char buf[4096];
    while (InterlockedCompareExchange(&t->alive, 0, 0) && t->hPipeRd) {
        DWORD nr = 0;
        if (!ReadFile(t->hPipeRd, buf, sizeof(buf)-1, &nr, NULL) || !nr)
            break;
        buf[nr] = '\0';
        HWND pan = g_hwndPanel;
        if (pan) {
            char *cp = (char*)n2e_Alloc(nr + 1);
            if (cp) {
                memcpy(cp, buf, nr + 1);
                PostMessage(pan, WM_TERMDATA, (WPARAM)cp, (LPARAM)nr);
            }
        }
    }
    InterlockedExchange(&t->alive, FALSE);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * Spawn — create panel + view + grid + shell
 * ═══════════════════════════════════════════════════════════════════ */
static BOOL Spawn(HWND hwndParent, ShellKind sh) {
    HINSTANCE hi = (HINSTANCE)GetWindowLongPtr(hwndParent, GWLP_HINSTANCE);
    EnsureClasses(hi);
    EnsureFonts();

    /* Create panel window if needed */
    if (!g_hwndPanel) {
        g_hwndPanel = CreateWindowExW(0, CLS_PANEL, L"",
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0, hwndParent,
            (HMENU)(UINT_PTR)IDC_TERMINAL_PANEL, hi, NULL);
        if (!g_hwndPanel) return FALSE;
    }

    /* Tear down old terminal */
    if (g_term) {
        KillTimer(g_hwndPanel, TIMER_HEALTH);
        KillTimer(g_hwndPanel, TIMER_BLINK);
        ShellKill(g_term);
        n2e_Free(g_term);
        g_term = NULL;
    }

    g_term = (Term*)n2e_Alloc(sizeof(Term));
    if (!g_term) return FALSE;
    ZeroMemory(g_term, sizeof(Term));
    g_term->shell = sh;

    /* Create view window */
    g_term->hwndView = CreateWindowExW(0, CLS_VIEW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0, g_hwndPanel,
        (HMENU)(UINT_PTR)IDC_TERMINAL_VIEW, hi, NULL);
    if (!g_term->hwndView) {
        n2e_Free(g_term); g_term = NULL; return FALSE;
    }

    /* Determine grid dimensions */
    RECT rc;
    GetClientRect(g_hwndPanel, &rc);
    int vw = rc.right > 80 ? rc.right : 640;
    int vh = rc.bottom - SPLITTER_H - HEADER_H;
    if (vh < 40) vh = 300;
    int cols = vw / g_cellW;
    int rows = vh / g_cellH;
    if (cols < 20) cols = 80;
    if (rows < 5) rows = 24;

    g_term->grid = Grid_Create(cols, rows);
    if (!g_term->grid) {
        DestroyWindow(g_term->hwndView);
        n2e_Free(g_term); g_term = NULL; return FALSE;
    }

    /* Launch shell */
    COORD sz = { (SHORT)cols, (SHORT)rows };
    if (!ShellCreate(g_term, sz, sh)) {
        ShellKill(g_term); n2e_Free(g_term); g_term = NULL; return FALSE;
    }

    InterlockedExchange(&g_term->alive, TRUE);
    g_term->hReader = CreateThread(NULL, 0, ReaderThread, g_term, 0, NULL);
    SetTimer(g_hwndPanel, TIMER_HEALTH, 500, NULL);
    SetTimer(g_hwndPanel, TIMER_BLINK, 530, NULL);

    g_curShell = sh;
    g_activeSessionId = ++g_sessionCounter;
    InvalidateRect(g_hwndPanel, NULL, TRUE);
    return TRUE;
}

/* ═══════════════════════════════════════════════════════════════════
 * PUBLIC API — Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

BOOL Terminal_Init(HWND hwnd) {
    g_hwndMain = hwnd;
    g_havePTY = InitConPTY();
    ShellDetect();
    g_curShell = BestShell();
    return TRUE;
}

void Terminal_Shutdown(void) {
    if (g_term) { ShellKill(g_term); n2e_Free(g_term); g_term = NULL; }
    if (g_hwndPanel) {
        KillTimer(g_hwndPanel, TIMER_HEALTH);
        KillTimer(g_hwndPanel, TIMER_BLINK);
        DestroyWindow(g_hwndPanel); g_hwndPanel = NULL;
    }
    if (g_fontHdr)  { DeleteObject(g_fontHdr);  g_fontHdr = NULL; }
    if (g_fontDrop) { DeleteObject(g_fontDrop); g_fontDrop = NULL; }
    if (g_fontGrid) { DeleteObject(g_fontGrid); g_fontGrid = NULL; }
    g_visible = FALSE;
}

BOOL Terminal_New(HWND hwndP) {
    BOOL r = Spawn(hwndP, g_curShell);
    if (r) Terminal_Show(hwndP);
    return r;
}

BOOL Terminal_NewShell(HWND hwndP, int t) {
    if (t < 0 || t >= SH_COUNT || !g_shells[t].found) return FALSE;
    BOOL r = Spawn(hwndP, (ShellKind)t);
    if (r) Terminal_Show(hwndP);
    return r;
}

void Terminal_Toggle(HWND hwndP) {
    if (g_visible) Terminal_Hide();
    else { if (!g_term) Terminal_New(hwndP); else Terminal_Show(hwndP); }
}

void Terminal_Show(HWND hwndP) {
    if (!g_hwndPanel) Terminal_New(hwndP);
    if (g_hwndPanel) {
        ShowWindow(g_hwndPanel, SW_SHOW);
        g_visible = TRUE;
        g_wantFocus = TRUE;
        RECT rc; GetClientRect(hwndP, &rc);
        SendMessage(hwndP, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        Terminal_Focus();
    }
}

void Terminal_Hide(void) {
    if (g_hwndPanel) {
        ShowWindow(g_hwndPanel, SW_HIDE);
        g_visible = FALSE;
        g_wantFocus = FALSE;
        HWND p = GetParent(g_hwndPanel);
        if (p) { RECT rc; GetClientRect(p, &rc);
            SendMessage(p, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom)); }
    }
}

BOOL Terminal_IsVisible(void) { return g_visible; }

int Terminal_Layout(HWND hwndP, int pw, int pb) {
    (void)hwndP;
    if (!g_visible || !g_hwndPanel) return 0;
    int top = pb - g_panelH;
    if (top < 100) top = 100;
    MoveWindow(g_hwndPanel, 0, top, pw, g_panelH, TRUE);
    int vy = SPLITTER_H + HEADER_H;
    int vh = g_panelH - vy;
    if (vh < 40) vh = 40;
    if (g_term && g_term->hwndView)
        MoveWindow(g_term->hwndView, 0, vy, pw, vh, TRUE);
    return g_panelH;
}

void Terminal_Write(const char *d, int n) {
    if (!g_term || !g_term->hPipeWr) return;
    if (!InterlockedCompareExchange(&g_term->alive, 0, 0)) return;
    DWORD w = 0;
    WriteFile(g_term->hPipeWr, d, (DWORD)n, &w, NULL);
}

void Terminal_SendCommand(const char *c) {
    if (!c) return;
    Terminal_Write(c, (int)strlen(c));
    Terminal_Write("\r\n", 2);
}

void Terminal_Focus(void) {
    if (g_term && g_term->hwndView && IsWindow(g_term->hwndView)) {
        g_wantFocus = TRUE;
        SetFocus(g_term->hwndView);
    } else if (g_hwndPanel && IsWindow(g_hwndPanel)) {
        g_wantFocus = TRUE;
        SetFocus(g_hwndPanel);
    }
}

HWND Terminal_GetPanelHwnd(void) { return g_hwndPanel; }

BOOL Terminal_WantsFocus(void) { return g_visible && g_wantFocus; }

void Terminal_RelinquishFocus(void) { g_wantFocus = FALSE; }

void Terminal_ApplyDarkMode(void) {
    if (g_hwndPanel) InvalidateRect(g_hwndPanel, NULL, TRUE);
    if (g_term && g_term->hwndView) InvalidateRect(g_term->hwndView, NULL, TRUE);
}

void Terminal_RunCommand(HWND hwndP, const char *cmd) {
    if (!g_term) Terminal_New(hwndP);
    if (!g_visible) Terminal_Show(hwndP);
    Terminal_SendCommand(cmd);
}

/* ═══════════════════════════════════════════════════════════════════
 * Colour helpers
 * ═══════════════════════════════════════════════════════════════════ */
static COLORREF GetFG(BYTE idx) {
    BOOL dk = DarkMode_IsEnabled();
    if (idx < 16) return g_ansi[idx];
    return dk ? C_DKFG : C_LTFG;
}
static COLORREF GetBG(BYTE idx) {
    BOOL dk = DarkMode_IsEnabled();
    if (idx == 0) return dk ? C_DKBG : C_LTBG;
    if (idx < 16) return g_ansi[idx];
    return dk ? C_DKBG : C_LTBG;
}

typedef enum {
    ROW_NORMAL = 0,
    ROW_PROMPT,
    ROW_SUCCESS,
    ROW_WARNING,
    ROW_ERROR
} RowTone;

static RowTone ClassifyRowTone(const Cell *row, int cols) {
    WCHAR text[512];
    int n = 0;
    int end = cols - 1;
    while (end >= 0 && row[end].ch == L' ') end--;
    if (end < 0) return ROW_NORMAL;
    for (int i = 0; i <= end && n < (int)ARRAYSIZE(text) - 1; i++) {
        text[n++] = row[i].ch ? row[i].ch : L' ';
    }
    text[n] = 0;

    if (StrStrIW(text, L"error") || StrStrIW(text, L"failed") || StrStrIW(text, L"exception") ||
        StrStrIW(text, L"fatal") || StrStrIW(text, L"cannot"))
        return ROW_ERROR;
    if (StrStrIW(text, L"warning") || StrStrIW(text, L"warn") || StrStrIW(text, L"deprecated"))
        return ROW_WARNING;
    if (StrStrIW(text, L"success") || StrStrIW(text, L"done") || StrStrIW(text, L"ready") ||
        StrStrIW(text, L"completed"))
        return ROW_SUCCESS;
    if ((n >= 3 && text[0] == L'P' && text[1] == L'S' && text[2] == L' ') ||
        (n >= 2 && text[n - 1] == L'>' && text[0] != L'[') ||
        (n >= 1 && text[0] == L'$'))
        return ROW_PROMPT;

    return ROW_NORMAL;
}

static COLORREF ToneRowBackground(RowTone tone, COLORREF base) {
    switch (tone) {
    case ROW_PROMPT:
        return BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW), base, 26);
    case ROW_SUCCESS:
        return BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN), base, 24);
    case ROW_WARNING:
        return BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_WARNING_ORANGE), base, 26);
    case ROW_ERROR:
        return BikodeTheme_Mix(BikodeTheme_GetColor(BKCLR_DANGER_RED), base, 28);
    default:
        return base;
    }
}

static COLORREF ToneAccent(RowTone tone) {
    switch (tone) {
    case ROW_PROMPT:
        return BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW);
    case ROW_SUCCESS:
        return BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN);
    case ROW_WARNING:
        return BikodeTheme_GetColor(BKCLR_WARNING_ORANGE);
    case ROW_ERROR:
        return BikodeTheme_GetColor(BKCLR_DANGER_RED);
    default:
        return RGB(0, 0, 0);
    }
}

/* Selection helper */
static BOOL InSel(int r, int c) {
    if (!g_hasSel) return FALSE;
    int sr = g_selSR, sc = g_selSC, er = g_selER, ec = g_selEC;
    if (sr > er || (sr == er && sc > ec)) { int t; t=sr;sr=er;er=t; t=sc;sc=ec;ec=t; }
    if (r < sr || r > er) return FALSE;
    if (r == sr && r == er) return c >= sc && c <= ec;
    if (r == sr) return c >= sc;
    if (r == er) return c <= ec;
    return TRUE;
}

/* Copy selection to clipboard */
static void CopySel(HWND hwnd) {
    if (!g_hasSel || !g_term || !g_term->grid) return;
    Grid *g = g_term->grid;
    int sr = g_selSR, sc = g_selSC, er = g_selER, ec = g_selEC;
    if (sr > er || (sr == er && sc > ec)) { int t; t=sr;sr=er;er=t; t=sc;sc=ec;ec=t; }
    int sz = (er - sr + 1) * (g->cols + 2) + 1;
    WCHAR *buf = (WCHAR*)n2e_Alloc(sz * sizeof(WCHAR));
    if (!buf) return;
    int pos = 0;
    for (int r = sr; r <= er; r++) {
        int cs = (r == sr) ? sc : 0;
        int ce = (r == er) ? ec : g->cols - 1;
        for (int c = cs; c <= ce && c < g->cols; c++)
            buf[pos++] = GridCell(g, r, c)->ch;
        if (r < er) { buf[pos++] = L'\r'; buf[pos++] = L'\n'; }
    }
    buf[pos] = 0;
    /* Trim trailing spaces per line */
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (pos+1)*sizeof(WCHAR));
        if (hg) {
            WCHAR *p = (WCHAR*)GlobalLock(hg);
            memcpy(p, buf, (pos+1)*sizeof(WCHAR));
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
        }
        CloseClipboard();
    }
    n2e_Free(buf);
}

/* ═══════════════════════════════════════════════════════════════════
 * VIEW WNDPROC — the terminal character grid
 *
 * This is a plain custom HWND. Its WndProc is the ONLY handler.
 * No Scintilla. No subclassing. Keyboard arrives directly.
 *
 * BUILD MARKER: 2026-02-08-v5
 * ═══════════════════════════════════════════════════════════════════ */
static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    /* ── Tell Windows we handle ALL keyboard input ────────── */
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;

    /* ── Paint ────────────────────────────────────────────── */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!g_term || !g_term->grid) { EndPaint(hwnd, &ps); return 0; }
        Grid *g = g_term->grid;
        BOOL dk = DarkMode_IsEnabled();
        COLORREF defBg = dk ? C_DKBG : C_LTBG;

        RECT rc; GetClientRect(hwnd, &rc);

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP obmp = SelectObject(mem, bmp);

        HBRUSH bgBr = CreateSolidBrush(defBg);
        FillRect(mem, &rc, bgBr); DeleteObject(bgBr);

        HFONT ofont = NULL;
        if (g_fontGrid) ofont = SelectObject(mem, g_fontGrid);
        SetBkMode(mem, OPAQUE);

        int viewBase = GridBase(g) - g->scrollOff;
        if (viewBase < 0) viewBase = 0;

        for (int r = 0; r < g->rows; r++) {
            int absLine = viewBase + r;
            if (absLine < 0 || absLine >= g->used) continue;
            Cell *row = &g->buf[absLine * g->cols];
            int y = r * g_cellH;
            RowTone tone = ClassifyRowTone(row, g->cols);
            COLORREF rowBaseBg = (dk && (absLine & 1)) ? C_DKROWALT : defBg;
            COLORREF accent = ToneAccent(tone);
            rowBaseBg = ToneRowBackground(tone, rowBaseBg);
            if (dk && tone != ROW_NORMAL) {
                RECT accentRc = { 0, y, 3, y + g_cellH };
                HBRUSH hAcc = CreateSolidBrush(accent);
                FillRect(mem, &accentRc, hAcc);
                DeleteObject(hAcc);
            }
            int c = 0;
            while (c < g->cols) {
                BOOL sel = InSel(r, c);
                COLORREF fg = sel ? (dk ? RGB(255,255,255) : RGB(0,0,0)) : GetFG(row[c].fg);
                COLORREF bg = sel ? (dk ? C_DKSEL : RGB(180,215,255))
                                  : (row[c].bg == 0 ? rowBaseBg : GetBG(row[c].bg));
                if (!sel && row[c].fg == 7 && dk && tone != ROW_NORMAL) {
                    fg = BikodeTheme_Mix(accent, C_DKFG, 54);
                }
                WCHAR run[512]; int rl = 0;
                int cs = c;
                while (c < g->cols && rl < 511) {
                    BOOL cs2 = InSel(r, c);
                    COLORREF f2 = cs2 ? (dk?RGB(255,255,255):RGB(0,0,0)) : GetFG(row[c].fg);
                    COLORREF b2 = cs2 ? (dk ? C_DKSEL : RGB(180,215,255))
                                      : (row[c].bg == 0 ? rowBaseBg : GetBG(row[c].bg));
                    if (!cs2 && row[c].fg == 7 && dk && tone != ROW_NORMAL) {
                        f2 = BikodeTheme_Mix(accent, C_DKFG, 54);
                    }
                    if (f2 != fg || b2 != bg) break;
                    run[rl++] = row[c].ch;
                    c++;
                }
                SetTextColor(mem, fg);
                SetBkColor(mem, bg);
                RECT cr = { cs * g_cellW, y, (cs + rl) * g_cellW, y + g_cellH };
                ExtTextOutW(mem, cs * g_cellW, y, ETO_OPAQUE|ETO_CLIPPED, &cr, run, rl, NULL);
            }
            int rx = g->cols * g_cellW;
            if (rx < rc.right) {
                RECT rr = { rx, y, rc.right, y + g_cellH };
                HBRUSH hb = CreateSolidBrush(defBg); FillRect(mem, &rr, hb); DeleteObject(hb);
            }
        }
        int by = g->rows * g_cellH;
        if (by < rc.bottom) {
            RECT rr = { 0, by, rc.right, rc.bottom };
            HBRUSH hb = CreateSolidBrush(defBg); FillRect(mem, &rr, hb); DeleteObject(hb);
        }

        /* Cursor */
        if (g_caretOn && g->scrollOff == 0) {
            int cx = g->curC * g_cellW;
            int cy = g->curR * g_cellH;
            RECT cr2 = { cx, cy, cx + 2, cy + g_cellH };
            HBRUSH cb = CreateSolidBrush(dk ? C_DKCARET : RGB(0,0,0));
            FillRect(mem, &cr2, cb); DeleteObject(cb);
        }

        if (dk) {
            HPEN hOuter = CreatePen(PS_SOLID, 1, BikodeTheme_GetColor(BKCLR_STROKE_DARK));
            HPEN hInner = CreatePen(PS_SOLID, 1, BikodeTheme_GetColor(BKCLR_STROKE_SOFT));
            HPEN hOld = (HPEN)SelectObject(mem, hOuter);
            MoveToEx(mem, 0, 0, NULL); LineTo(mem, rc.right - 1, 0);
            MoveToEx(mem, 0, 0, NULL); LineTo(mem, 0, rc.bottom - 1);
            SelectObject(mem, hInner);
            MoveToEx(mem, rc.right - 1, 0, NULL); LineTo(mem, rc.right - 1, rc.bottom - 1);
            MoveToEx(mem, 0, rc.bottom - 1, NULL); LineTo(mem, rc.right - 1, rc.bottom - 1);
            SelectObject(mem, hOld);
            DeleteObject(hOuter);
            DeleteObject(hInner);
        }

        if (ofont) SelectObject(mem, ofont);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, obmp); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE: {
        if (!g_term || !g_term->grid) break;
        RECT rc; GetClientRect(hwnd, &rc);
        int nc = rc.right / g_cellW;
        int nr = rc.bottom / g_cellH;
        if (nc < 10) nc = 10;
        if (nr < 3) nr = 3;
        Grid *g = g_term->grid;
        if (nc != g->cols || nr != g->rows) {
            Grid_Resize(g, nc, nr);
            if (g_term->hPC && pfnResizePC) {
                COORD sz = { (SHORT)nc, (SHORT)nr };
                pfnResizePC(g_term->hPC, sz);
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    /* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
     * KEYBOARD INPUT
     *
     * WM_CHAR: printable chars, Enter, Tab, Backspace, Ctrl+letter
     * WM_KEYDOWN: special keys (arrow, F1-F12, etc), Ctrl+V paste
     *
     * BUILD MARKER: 2026-02-08-v5
     * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */

    case WM_CHAR: {
        WCHAR wch = (WCHAR)wParam;

        /* Ctrl+C — copy selection or send interrupt */
        if (wch == 0x03) {
            if (g_hasSel) CopySel(hwnd);
            else Terminal_Write("\x03", 1);
            return 0;
        }

        /* Skip chars already handled as complex operations in WM_KEYDOWN */
        if (wch == 0x16) return 0;  /* Ctrl+V → paste handled in WM_KEYDOWN */
        if (wch == 0x0C) return 0;  /* Ctrl+L → clear handled in WM_KEYDOWN */

        /* Convert to UTF-8 and write to shell pipe */
        char u8[8];
        int n = WideCharToMultiByte(CP_UTF8, 0, &wch, 1, u8, sizeof(u8), NULL, NULL);
        if (n > 0) Terminal_Write(u8, n);
        g_hasSel = FALSE;
        return 0;
    }

    case WM_KEYDOWN: {
        BOOL ctrl = !!(GetKeyState(VK_CONTROL) & 0x8000);

        /* Ctrl+V: paste */
        if (wParam == 'V' && ctrl) {
            if (OpenClipboard(hwnd)) {
                HANDLE hd = GetClipboardData(CF_UNICODETEXT);
                if (hd) {
                    const WCHAR *wz = (const WCHAR*)GlobalLock(hd);
                    if (wz) {
                        int n = WideCharToMultiByte(CP_UTF8, 0, wz, -1, NULL, 0, NULL, NULL);
                        if (n > 1) {
                            char *b = (char*)n2e_Alloc(n);
                            if (b) {
                                WideCharToMultiByte(CP_UTF8, 0, wz, -1, b, n, NULL, NULL);
                                Terminal_Write(b, n-1);
                                n2e_Free(b);
                            }
                        }
                        GlobalUnlock(hd);
                    }
                }
                CloseClipboard();
            }
            return 0;
        }

        /* Ctrl+L: clear screen */
        if (wParam == 'L' && ctrl) {
            if (g_term && g_term->grid) {
                for (int r = 0; r < g_term->grid->rows; r++)
                    Grid_ClearRow(g_term->grid, r);
                g_term->grid->curR = 0; g_term->grid->curC = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            Terminal_Write("\x0C", 1);
            return 0;
        }

        if (wParam == 'C' && ctrl) return 0; /* handled in WM_CHAR */

        /* Special keys that do NOT generate WM_CHAR → send VT escape */
        switch (wParam) {
        case VK_UP:     Terminal_Write("\033[A", 3); return 0;
        case VK_DOWN:   Terminal_Write("\033[B", 3); return 0;
        case VK_RIGHT:  Terminal_Write("\033[C", 3); return 0;
        case VK_LEFT:   Terminal_Write("\033[D", 3); return 0;
        case VK_DELETE: Terminal_Write("\033[3~", 4); return 0;
        case VK_HOME:   Terminal_Write("\033[H", 3); return 0;
        case VK_END:    Terminal_Write("\033[F", 3); return 0;
        case VK_PRIOR:  Terminal_Write("\033[5~", 4); return 0;
        case VK_NEXT:   Terminal_Write("\033[6~", 4); return 0;
        case VK_INSERT: Terminal_Write("\033[2~", 4); return 0;
        case VK_F1:  Terminal_Write("\033OP", 3); return 0;
        case VK_F2:  Terminal_Write("\033OQ", 3); return 0;
        case VK_F3:  Terminal_Write("\033OR", 3); return 0;
        case VK_F4:  Terminal_Write("\033OS", 3); return 0;
        case VK_F5:  Terminal_Write("\033[15~", 5); return 0;
        case VK_F6:  Terminal_Write("\033[17~", 5); return 0;
        case VK_F7:  Terminal_Write("\033[18~", 5); return 0;
        case VK_F8:  Terminal_Write("\033[19~", 5); return 0;
        case VK_F9:  Terminal_Write("\033[20~", 5); return 0;
        case VK_F10: Terminal_Write("\033[21~", 5); return 0;
        case VK_F11: Terminal_Write("\033[23~", 5); return 0;
        case VK_F12: Terminal_Write("\033[24~", 5); return 0;
        case VK_ESCAPE: Terminal_Hide(); return 0;
        }
        break;  /* let DefWindowProc + TranslateMessage generate WM_CHAR */
    }

    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        return 0;

    /* ── Focus ────────────────────────────────────────────── */
    case WM_SETFOCUS:
        g_wantFocus = TRUE;
        g_caretOn = TRUE;
        if (g_term && g_term->hwndView)
            InvalidateRect(g_term->hwndView, NULL, FALSE);
        return 0;

    case WM_KILLFOCUS: {
        HWND hNew = (HWND)wParam;
        if (hNew && g_hwndPanel) {
            if (hNew != g_hwndPanel && !IsChild(g_hwndPanel, hNew))
                g_wantFocus = FALSE;
        }
        return 0;
    }

    /* ── Mouse — selection ────────────────────────────────── */
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        SetCapture(hwnd);
        g_selecting = TRUE;
        g_selSR = (short)HIWORD(lParam) / g_cellH;
        g_selSC = (short)LOWORD(lParam) / g_cellW;
        g_selER = g_selSR; g_selEC = g_selSC;
        g_hasSel = FALSE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        if (g_selecting) {
            g_selER = (short)HIWORD(lParam) / g_cellH;
            g_selEC = (short)LOWORD(lParam) / g_cellW;
            g_hasSel = (g_selSR != g_selER || g_selSC != g_selEC);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_selecting) {
            g_selecting = FALSE;
            ReleaseCapture();
            g_selER = (short)HIWORD(lParam) / g_cellH;
            g_selEC = (short)LOWORD(lParam) / g_cellW;
            g_hasSel = (g_selSR != g_selER || g_selSC != g_selEC);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        SetFocus(hwnd);
        if (g_term && g_term->grid) {
            int r = (short)HIWORD(lParam) / g_cellH;
            int c = (short)LOWORD(lParam) / g_cellW;
            Grid *g = g_term->grid;
            if (r >= 0 && r < g->rows && c >= 0 && c < g->cols) {
                Cell *cl = GridCell(g, r, c);
                if (cl->ch != L' ') {
                    int s = c, e = c;
                    while (s > 0 && GridCell(g, r, s-1)->ch != L' ') s--;
                    while (e < g->cols-1 && GridCell(g, r, e+1)->ch != L' ') e++;
                    g_selSR = r; g_selSC = s; g_selER = r; g_selEC = e;
                    g_hasSel = TRUE;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;
    }

    /* ── Scroll ───────────────────────────────────────────── */
    case WM_MOUSEWHEEL: {
        if (!g_term || !g_term->grid) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        int lines = delta / WHEEL_DELTA * 3;
        Grid *g = g_term->grid;
        g->scrollOff += lines;
        int maxOff = g->used - g->rows;
        if (maxOff < 0) maxOff = 0;
        if (g->scrollOff < 0) g->scrollOff = 0;
        if (g->scrollOff > maxOff) g->scrollOff = maxOff;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    /* ── Context menu ─────────────────────────────────────── */
    case WM_RBUTTONUP: {
        HMENU hm = CreatePopupMenu();
        if (hm) {
            AppendMenuW(hm, MF_STRING | (g_hasSel ? 0 : MF_GRAYED), 1, L"  Copy\tCtrl+C");
            AppendMenuW(hm, MF_STRING, 2, L"  Paste\tCtrl+V");
            AppendMenuW(hm, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hm, MF_STRING, 3, L"  Clear\tCtrl+L");
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            ClientToScreen(hwnd, &pt);
            int cmd = TrackPopupMenu(hm, TPM_RETURNCMD|TPM_LEFTBUTTON|TPM_NONOTIFY,
                pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hm);
            if (cmd == 1) CopySel(hwnd);
            else if (cmd == 2) PostMessage(hwnd, WM_KEYDOWN, 'V', 0); /* trigger paste logic */
            else if (cmd == 3) {
                if (g_term && g_term->grid) {
                    for (int r = 0; r < g_term->grid->rows; r++)
                        Grid_ClearRow(g_term->grid, r);
                    g_term->grid->curR = 0; g_term->grid->curC = 0;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                Terminal_Write("\x0C", 1);
            }
        }
        return 0;
    }

    } /* end switch */

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/* ═══════════════════════════════════════════════════════════════════
 * Shell dropdown menu
 * ═══════════════════════════════════════════════════════════════════ */
static void ShowShellMenu(HWND hwnd) {
    HMENU hm = CreatePopupMenu();
    if (!hm) return;
    for (int i = 0; i < SH_COUNT; i++) {
        if (!g_shells[i].found) continue;
        WCHAR it[128];
        swprintf_s(it, 128, L"  %s", g_shells[i].label);
        UINT fl = MF_STRING;
        if ((ShellKind)i == g_curShell && g_term && InterlockedCompareExchange(&g_term->alive,0,0))
            fl |= MF_CHECKED;
        AppendMenuW(hm, fl, 5000+i, it);
    }
    POINT pt = { g_rcDrop.left, g_rcDrop.bottom };
    ClientToScreen(hwnd, &pt);
    int cmd = TrackPopupMenu(hm, TPM_RETURNCMD|TPM_LEFTBUTTON|TPM_NONOTIFY,
        pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hm);
    if (cmd >= 5000 && cmd < 5000+SH_COUNT)
        Terminal_NewShell(GetParent(hwnd), cmd - 5000);
}

/* ═══════════════════════════════════════════════════════════════════
 * Header painting
 * ═══════════════════════════════════════════════════════════════════ */
static void CalcBtns(int w) {
    int bs = 20, by = SPLITTER_H + (HEADER_H - 20) / 2 + 3;
    g_rcClose.left = w - bs - 8; g_rcClose.top = by;
    g_rcClose.right = g_rcClose.left + bs; g_rcClose.bottom = by + bs;
    g_rcNew.left = g_rcClose.left - bs - 4; g_rcNew.top = by;
    g_rcNew.right = g_rcNew.left + bs; g_rcNew.bottom = by + bs;
    g_rcDrop.left = 104; g_rcDrop.top = SPLITTER_H + 12;
    g_rcDrop.right = g_rcDrop.left + 110; g_rcDrop.bottom = g_rcDrop.top + 18;
}

static void PaintHeader(HWND hwnd, HDC dc, RECT *rc) {
    WCHAR cwd[MAX_PATH] = L"";
    WCHAR tabText[64];
    RECT deck, rail, deckLabel, tabChip, shellChip, cwdChip, stateChip;
    WCHAR runState[32] = L"IDLE";
    COLORREF stateAccent = BikodeTheme_GetColor(BKCLR_TEXT_MUTED);
    int W = rc->right;
    CalcBtns(W);
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (g_term && InterlockedCompareExchange(&g_term->alive, 0, 0)) {
        lstrcpyW(runState, L"RUNNING");
        stateAccent = BikodeTheme_GetColor(BKCLR_SUCCESS_GREEN);
    }

    deck.left = 0;
    deck.top = SPLITTER_H;
    deck.right = W;
    deck.bottom = SPLITTER_H + HEADER_H;
    BikodeTheme_DrawCutCornerPanel(dc, &deck,
        BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        8, TRUE);

    rail = deck;
    rail.left += 8;
    rail.top += 6;
    rail.right = rail.left + 3;
    rail.bottom -= 6;
    {
        HBRUSH hRail = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
        FillRect(dc, &rail, hRail);
        DeleteObject(hRail);
    }

    SetBkMode(dc, TRANSPARENT);
    swprintf_s(tabText, ARRAYSIZE(tabText), L"TERM %02u", g_activeSessionId ? g_activeSessionId : 1);
    deckLabel.left = 18;
    deckLabel.top = deck.top + 1;
    deckLabel.right = 132;
    deckLabel.bottom = deckLabel.top + 12;
    SetTextColor(dc, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
    if (g_fontDrop) SelectObject(dc, g_fontDrop);
    DrawTextW(dc, L"CONSOLE DECK", -1, &deckLabel, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    tabChip.left = 18;
    tabChip.top = deckLabel.bottom + 1;
    tabChip.right = tabChip.left + 78;
    tabChip.bottom = tabChip.top + 18;
    BikodeTheme_DrawChip(dc, &tabChip, tabText,
        BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
        BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE, BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));

    shellChip = g_rcDrop;
    shellChip.left = tabChip.right + 8;
    shellChip.right = shellChip.left + 110;
    stateChip.right = g_rcNew.left - 10;
    stateChip.left = stateChip.right - 86;
    stateChip.top = g_rcDrop.top;
    stateChip.bottom = g_rcDrop.bottom;
    cwdChip.left = shellChip.right + 8;
    cwdChip.top = shellChip.top;
    cwdChip.right = stateChip.left - 8;
    cwdChip.bottom = shellChip.bottom;
    BikodeTheme_DrawChip(dc, &shellChip, g_shells[g_curShell].label,
        g_hoverDrop ? BikodeTheme_GetColor(BKCLR_SURFACE_RAISED) : BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        g_hoverDrop ? BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN) : BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
        BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE, BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
    {
        RECT rcDropArrow = shellChip;
        rcDropArrow.left = rcDropArrow.right - 12;
        SetTextColor(dc, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
        DrawTextW(dc, L"v", 1, &rcDropArrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (cwdChip.right > cwdChip.left + 40) {
        BikodeTheme_DrawChip(dc, &cwdChip, cwd,
            BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
            BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
            BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY),
            BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE, BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));
    }
    BikodeTheme_DrawChip(dc, &stateChip, runState,
        BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
        BikodeTheme_GetFont(BKFONT_MONO_SMALL), TRUE, stateAccent);

    /* Close button */
    BikodeTheme_DrawButton(dc, &g_rcClose, L"", BKGLYPH_NONE, g_hoverClose, FALSE, FALSE, FALSE);
    SetTextColor(dc, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
    DrawTextW(dc, L"\x2715", 1, &g_rcClose, DT_SINGLELINE|DT_VCENTER|DT_CENTER);

    /* New button */
    BikodeTheme_DrawButton(dc, &g_rcNew, L"", BKGLYPH_TERMINAL, g_hoverNew, FALSE, FALSE, FALSE);
    SetTextColor(dc, BikodeTheme_GetColor(BKCLR_TEXT_MUTED));
    DrawTextW(dc, L"+", 1, &g_rcNew, DT_SINGLELINE|DT_VCENTER|DT_CENTER);
}

/* ═══════════════════════════════════════════════════════════════════
 * PANEL WNDPROC
 * ═══════════════════════════════════════════════════════════════════ */
static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        BOOL dk = DarkMode_IsEnabled();
        HBRUSH hb = CreateSolidBrush(dk ? C_DKBG : C_LTBG);
        FillRect(dc, &rc, hb); DeleteObject(hb);
        RECT rs = rc; rs.bottom = SPLITTER_H;
        hb = CreateSolidBrush(dk ? C_DKSPLIT : C_LTSPLIT);
        FillRect(dc, &rs, hb); DeleteObject(hb);
        PaintHeader(hwnd, dc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC dc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH h = CreateSolidBrush(DarkMode_IsEnabled() ? C_DKBG : C_LTBG);
        FillRect(dc, &rc, h); DeleteObject(h);
        return 1;
    }

    case WM_SIZE: {
        int cx = LOWORD(lParam);
        if (cx > 0) CalcBtns(cx);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        if (pt.y < SPLITTER_H) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); return TRUE; }
        if (PtInRect(&g_rcDrop, pt) || PtInRect(&g_rcNew, pt) || PtInRect(&g_rcClose, pt)) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lParam), y = (short)HIWORD(lParam);
        if (y < SPLITTER_H) { SetCapture(hwnd); return 0; }
        POINT pt = { x, y };
        if (PtInRect(&g_rcDrop, pt))  { ShowShellMenu(hwnd); return 0; }
        if (PtInRect(&g_rcNew, pt))   { Terminal_New(GetParent(hwnd)); return 0; }
        if (PtInRect(&g_rcClose, pt)) { Terminal_Hide(); return 0; }
        /* Click below header → focus view */
        if (g_term && g_term->hwndView) {
            g_wantFocus = TRUE;
            SetFocus(g_term->hwndView);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (GetCapture() == hwnd) {
            POINT pt; GetCursorPos(&pt);
            HWND p = GetParent(hwnd); ScreenToClient(p, &pt);
            RECT rp; GetClientRect(p, &rp);
            int nh = rp.bottom - pt.y;
            if (nh < 120) nh = 120;
            if (nh > rp.bottom - 100) nh = rp.bottom - 100;
            g_panelH = nh;
            SendMessage(p, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rp.right, rp.bottom));
        } else {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            BOOL c2 = PtInRect(&g_rcClose, pt), n2 = PtInRect(&g_rcNew, pt), d2 = PtInRect(&g_rcDrop, pt);
            if (c2 != g_hoverClose || n2 != g_hoverNew || d2 != g_hoverDrop) {
                g_hoverClose = c2; g_hoverNew = n2; g_hoverDrop = d2;
                RECT rh = { 0, SPLITTER_H, 9999, SPLITTER_H + HEADER_H };
                InvalidateRect(hwnd, &rh, FALSE);
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    }

    case WM_MOUSELEAVE:
        if (g_hoverClose || g_hoverNew || g_hoverDrop) {
            g_hoverClose = FALSE; g_hoverNew = FALSE; g_hoverDrop = FALSE;
            RECT rh = { 0, SPLITTER_H, 9999, SPLITTER_H + HEADER_H };
            InvalidateRect(hwnd, &rh, FALSE);
        }
        break;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        break;

    /* ── Terminal output from reader thread ────────────────── */
    case WM_TERMDATA: {
        char *d = (char*)wParam;
        int n = (int)lParam;
        if (g_term && g_term->grid && d && n > 0) {
            Grid_ProcessVT(g_term->grid, d, n);
            if (g_term->hwndView)
                InvalidateRect(g_term->hwndView, NULL, FALSE);
        }
        if (d) n2e_Free(d);
        return 0;
    }

    /* ── Timers ───────────────────────────────────────────── */
    case WM_TIMER:
        if (wParam == TIMER_HEALTH && g_term && g_term->hProc) {
            DWORD ec = 0;
            if (GetExitCodeProcess(g_term->hProc, &ec) && ec != STILL_ACTIVE) {
                InterlockedExchange(&g_term->alive, FALSE);
                if (g_term->grid) {
                    char m[128];
                    int ml = sprintf_s(m, 128, "\r\n\r\n[Process exited with code %lu]\r\n", ec);
                    Grid_ProcessVT(g_term->grid, m, ml);
                    if (g_term->hwndView)
                        InvalidateRect(g_term->hwndView, NULL, FALSE);
                }
                KillTimer(hwnd, TIMER_HEALTH);
            }
        }
        if (wParam == TIMER_BLINK) {
            g_caretOn = !g_caretOn;
            if (g_term && g_term->hwndView && GetFocus() == g_term->hwndView)
                InvalidateRect(g_term->hwndView, NULL, FALSE);
        }
        return 0;

    /* Forward keyboard to view if panel somehow gets it */
    case WM_CHAR:
    case WM_KEYDOWN:
        if (g_term && g_term->hwndView)
            return SendMessage(g_term->hwndView, msg, wParam, lParam);
        return 0;

    /* Redirect focus to view */
    case WM_SETFOCUS:
        if (g_term && g_term->hwndView) {
            SetFocus(g_term->hwndView);
            return 0;
        }
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
