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
#include "AIBridge.h"
#include "AIDirectCall.h"
#include "mono_json.h"
#include "ui/theme/BikodeTheme.h"
#include "DarkMode.h"
#include "CommonUtils.h"
#include "FileManager.h"
#include <uxtheme.h>
#include <shlwapi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
#define TERM_INPUT_MAX        512
#define TERM_CMD_MAX          1024
#define TERM_AI_PROMPT_MAX    4096
#define TERM_AI_OUTPUT_MAX    4096

/* colours */
#define C_DKBG     RGB(24,24,24)
#define C_DKFG     RGB(230,230,230)
#define C_DKHDR    RGB(48,50,58)
#define C_DKSPLIT  RGB(50,50,50)
#define C_DKTXT    RGB(230,230,230)
#define C_DKDIM    RGB(150,150,150)
#define C_DKROWALT RGB(30,30,30)
#define C_DKSEL    RGB(48,50,58)
#define C_DKCARET  RGB(255,212,0)
#define C_DKBTN    RGB(36,36,36)
#define C_DKBORD   RGB(55,55,55)
#define C_DKDROP   RGB(36,36,36)

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
static COLORREF g_ansiDark[16] = {
    RGB(12,12,12),    RGB(197,15,31),   RGB(19,161,14),  RGB(193,156,0),
    RGB(0,55,218),    RGB(136,23,152),  RGB(58,150,221), RGB(204,204,204),
    RGB(118,118,118), RGB(231,72,86),   RGB(22,198,12),  RGB(249,241,165),
    RGB(59,120,255),  RGB(180,0,158),   RGB(97,214,214), RGB(242,242,242)
};
/* ANSI 16-colour palette (light theme — darker tones for white backgrounds) */
static COLORREF g_ansiLight[16] = {
    RGB(0,0,0),       RGB(175,0,0),     RGB(0,135,0),    RGB(135,100,0),
    RGB(0,40,180),    RGB(120,0,140),   RGB(0,120,180),  RGB(80,80,80),
    RGB(100,100,100), RGB(200,30,30),   RGB(0,155,0),    RGB(160,120,0),
    RGB(40,80,220),   RGB(160,0,140),   RGB(0,140,160),  RGB(30,30,30)
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

typedef enum {
    TERM_AI_IDLE = 0,
    TERM_AI_TRANSLATE,
    TERM_AI_CORRECT
} TermAIRequestMode;

typedef struct {
    BOOL              bPending;
    BOOL              bCommandFailureSuggested;
    TermAIRequestMode mode;
    WCHAR             wszInputLine[TERM_INPUT_MAX];
    int               cchInputLine;
    WCHAR             wszLastSubmitted[TERM_INPUT_MAX];
    char              szLastSubmitted[TERM_CMD_MAX];
    char              szRecentOutput[TERM_AI_OUTPUT_MAX];
} TerminalAIState;

typedef struct {
    WCHAR             wszCommand[TERM_CMD_MAX];
    TermAIRequestMode mode;
    BOOL              accepted;
} TerminalAISuggestionDialogData;

static TerminalAIState g_termAI = {0};

/* Forward declarations */
static LRESULT CALLBACK PanelProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
static DWORD   WINAPI   ReaderThread(LPVOID);
static BOOL    ShellCreate(Term*, COORD, ShellKind);
static void    ShellKill(Term*);
static void    ShellDetect(void);
static void    ShowShellMenu(HWND);
static void    TerminalAI_ResetInputLine(void);
static void    TerminalAI_RecordSubmittedUtf8(const char* cmd);
static void    TerminalAI_UnwrapCommand(char* szCommand);
static BOOL    TerminalAI_StartSuggestion(HWND hwnd, TermAIRequestMode mode, const WCHAR* wszInput, const char* pszFailure);
static void    TerminalAI_HandleSuggestionResponse(HWND hwnd, char* pszResponse);
static void    TerminalAI_ExecuteSuggestedCommand(const char* cmd, TermAIRequestMode mode);

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
    {
        const WCHAR* projRoot = FileManager_GetRootPath();
        if (projRoot && projRoot[0])
            lstrcpynW(cwd, projRoot, MAX_PATH);
        else
            GetCurrentDirectoryW(MAX_PATH, cwd);
    }

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
    ZeroMemory(&g_termAI, sizeof(g_termAI));
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
    ZeroMemory(&g_termAI, sizeof(g_termAI));
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
    TerminalAI_RecordSubmittedUtf8(c);
    Terminal_Write(c, (int)strlen(c));
    Terminal_Write("\r\n", 2);
}

static void TerminalAI_ExecuteSuggestedCommand(const char* cmd, TermAIRequestMode mode) {
    if (!cmd || !cmd[0])
        return;

    if (mode == TERM_AI_TRANSLATE) {
        TerminalAI_ResetInputLine();
        Terminal_Write("\x03", 1);
        Sleep(80);
    }

    Terminal_SendCommand(cmd);
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

void Terminal_AppendTranscript(HWND hwndP, const char *text) {
    if (!text || !text[0]) return;

    if (!g_term && !Spawn(hwndP, g_curShell)) return;

    if (!g_visible && g_hwndPanel) {
        ShowWindow(g_hwndPanel, SW_SHOW);
        g_visible = TRUE;
        g_wantFocus = FALSE;
        if (hwndP) {
            RECT rc;
            GetClientRect(hwndP, &rc);
            SendMessage(hwndP, WM_SIZE, SIZE_RESTORED, MAKELPARAM(rc.right, rc.bottom));
        }
    }

    if (g_term && g_term->grid) {
        Grid_ProcessVT(g_term->grid, text, (int)strlen(text));
        if (g_term->hwndView)
            InvalidateRect(g_term->hwndView, NULL, FALSE);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * Colour helpers
 * ═══════════════════════════════════════════════════════════════════ */
static COLORREF GetFG(BYTE idx) {
    BOOL dk = DarkMode_IsEnabled();
    if (idx < 16) return dk ? g_ansiDark[idx] : g_ansiLight[idx];
    return dk ? C_DKFG : C_LTFG;
}
static COLORREF GetBG(BYTE idx) {
    BOOL dk = DarkMode_IsEnabled();
    if (idx == 0) return dk ? C_DKBG : C_LTBG;
    if (idx < 16) return dk ? g_ansiDark[idx] : g_ansiLight[idx];
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

static BOOL TerminalAI_IsSpaceW(WCHAR ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

static BOOL TerminalAI_IsSpaceA(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void TerminalAI_TrimWideInPlace(WCHAR* wszText) {
    int start = 0;
    int end;
    if (!wszText) return;

    while (wszText[start] && TerminalAI_IsSpaceW(wszText[start]))
        start++;
    if (start > 0)
        MoveMemory(wszText, wszText + start, (lstrlenW(wszText + start) + 1) * sizeof(WCHAR));

    end = lstrlenW(wszText);
    while (end > 0 && TerminalAI_IsSpaceW(wszText[end - 1]))
        wszText[--end] = 0;
}

static void TerminalAI_TrimAnsiInPlace(char* szText) {
    int start = 0;
    int end;
    if (!szText) return;

    while (szText[start] && TerminalAI_IsSpaceA(szText[start]))
        start++;
    if (start > 0)
        MoveMemory(szText, szText + start, strlen(szText + start) + 1);

    end = (int)strlen(szText);
    while (end > 0 && TerminalAI_IsSpaceA(szText[end - 1]))
        szText[--end] = 0;
}

static void TerminalAI_ResetInputLine(void) {
    g_termAI.wszInputLine[0] = 0;
    g_termAI.cchInputLine = 0;
}

static void TerminalAI_AppendInputChar(WCHAR wch) {
    if (g_termAI.cchInputLine >= TERM_INPUT_MAX - 1)
        return;
    g_termAI.wszInputLine[g_termAI.cchInputLine++] = wch;
    g_termAI.wszInputLine[g_termAI.cchInputLine] = 0;
}

static void TerminalAI_BackspaceInputChar(void) {
    if (g_termAI.cchInputLine <= 0)
        return;
    g_termAI.wszInputLine[--g_termAI.cchInputLine] = 0;
}

static void TerminalAI_AppendInputText(const WCHAR* wszText) {
    if (!wszText) return;
    while (*wszText && g_termAI.cchInputLine < TERM_INPUT_MAX - 1) {
        if (*wszText == L'\r' || *wszText == L'\n') {
            TerminalAI_ResetInputLine();
            return;
        }
        g_termAI.wszInputLine[g_termAI.cchInputLine++] = *wszText++;
    }
    g_termAI.wszInputLine[g_termAI.cchInputLine] = 0;
}

static void TerminalAI_CopyGridRowText(Grid* g, int row, WCHAR* wszOut, int cchOut) {
    int end;
    if (!wszOut || cchOut <= 0) return;
    wszOut[0] = 0;
    if (!g || row < 0 || row >= g->rows) return;

    end = g->cols - 1;
    while (end >= 0 && GridCell(g, row, end)->ch == L' ')
        end--;
    if (end < 0) return;

    for (int c = 0; c <= end && c < cchOut - 1; c++) {
        WCHAR ch = GridCell(g, row, c)->ch;
        wszOut[c] = ch ? ch : L' ';
        wszOut[c + 1] = 0;
    }
    TerminalAI_TrimWideInPlace(wszOut);
}

static void TerminalAI_GetPromptLine(WCHAR* wszOut, int cchOut) {
    if (!wszOut || cchOut <= 0) return;
    wszOut[0] = 0;
    if (!g_term || !g_term->grid) return;

    for (int r = g_term->grid->rows - 1; r >= 0; r--) {
        int line = GridBase(g_term->grid) + r;
        if (line < 0 || line >= g_term->grid->used)
            continue;
        if (ClassifyRowTone(&g_term->grid->buf[line * g_term->grid->cols], g_term->grid->cols) == ROW_PROMPT) {
            TerminalAI_CopyGridRowText(g_term->grid, r, wszOut, cchOut);
            if (wszOut[0])
                return;
        }
    }
}

static const WCHAR* TerminalAI_FindPromptDelimiter(const WCHAR* wszText) {
    const WCHAR* wszLast = NULL;
    if (!wszText) return NULL;

    while (*wszText) {
        if (*wszText == L'>' || *wszText == L'$' || *wszText == L'#' || *wszText == L'%')
            wszLast = wszText;
        wszText++;
    }
    return wszLast;
}

static void TerminalAI_ExtractCommandFromPrompt(const WCHAR* wszPrompt, WCHAR* wszOut, int cchOut) {
    const WCHAR* wszStart;
    const WCHAR* wszSplit;
    if (!wszOut || cchOut <= 0) return;
    wszOut[0] = 0;
    if (!wszPrompt || !wszPrompt[0]) return;

    wszStart = wszPrompt;
    wszSplit = TerminalAI_FindPromptDelimiter(wszPrompt);
    if (wszSplit && wszSplit[1])
        wszStart = wszSplit + 1;

    while (*wszStart && TerminalAI_IsSpaceW(*wszStart))
        wszStart++;

    lstrcpynW(wszOut, wszStart, cchOut);
    TerminalAI_TrimWideInPlace(wszOut);
}

static void TerminalAI_GetSubmittedLine(WCHAR* wszOut, int cchOut) {
    WCHAR wszPrompt[TERM_INPUT_MAX];
    if (!wszOut || cchOut <= 0) return;
    wszOut[0] = 0;

    if (g_termAI.wszInputLine[0]) {
        lstrcpynW(wszOut, g_termAI.wszInputLine, cchOut);
        TerminalAI_TrimWideInPlace(wszOut);
        return;
    }

    TerminalAI_GetPromptLine(wszPrompt, ARRAYSIZE(wszPrompt));
    TerminalAI_ExtractCommandFromPrompt(wszPrompt, wszOut, cchOut);
}
static BOOL TerminalAI_IsKnownCommandToken(const WCHAR* wszToken) {
    static const WCHAR* commands[] = {
        L"cd", L"chdir", L"cls", L"clear", L"dir", L"ls", L"pwd", L"echo", L"type",
        L"cat", L"help", L"man", L"git", L"npm", L"pnpm", L"yarn", L"npx", L"node",
        L"python", L"py", L"pip", L"pip3", L"uv", L"cargo", L"go", L"make", L"cmake",
        L"msbuild", L"dotnet", L"java", L"javac", L"gradle", L"mvn", L"bash", L"sh",
        L"wsl", L"cmd", L"powershell", L"pwsh", L"winget", L"choco", L"scoop",
        L"mkdir", L"md", L"rmdir", L"rd", L"del", L"rm", L"copy", L"cp", L"move",
        L"mv", L"ren", L"rename", L"start", L"explorer", L"code", L"where", L"which",
        L"find", L"findstr", L"grep", L"Get-ChildItem", L"Set-Location", L"Get-Location",
        L"Get-Content", L"Set-Content", NULL
    };

    if (!wszToken || !wszToken[0]) return FALSE;
    for (int i = 0; commands[i]; i++) {
        if (lstrcmpiW(wszToken, commands[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static BOOL TerminalAI_IsLikelyShellCommand(const WCHAR* wszLine) {
    WCHAR wszTrimmed[TERM_INPUT_MAX];
    WCHAR wszToken[96];
    int i = 0;
    int j = 0;

    if (!wszLine || !wszLine[0]) return FALSE;
    lstrcpynW(wszTrimmed, wszLine, ARRAYSIZE(wszTrimmed));
    TerminalAI_TrimWideInPlace(wszTrimmed);
    if (!wszTrimmed[0]) return FALSE;

    if (StrStrW(wszTrimmed, L"&&") || StrStrW(wszTrimmed, L"||") ||
        wcschr(wszTrimmed, L'|') || wcschr(wszTrimmed, L'>') ||
        wcschr(wszTrimmed, L'<') || wcschr(wszTrimmed, L';'))
        return TRUE;

    if ((wszTrimmed[0] == L'.' && (wszTrimmed[1] == L'\\' || wszTrimmed[1] == L'/')) ||
        wszTrimmed[0] == L'/' || wszTrimmed[0] == L'\\')
        return TRUE;

    while (wszTrimmed[i] && !TerminalAI_IsSpaceW(wszTrimmed[i]) && j < ARRAYSIZE(wszToken) - 1)
        wszToken[j++] = wszTrimmed[i++];
    wszToken[j] = 0;

    if (!wszToken[0])
        return FALSE;
    if (wcschr(wszToken, L':') || wcschr(wszToken, L'\\') || wcschr(wszToken, L'/'))
        return TRUE;
    if (TerminalAI_IsKnownCommandToken(wszToken))
        return TRUE;

    if (!wcschr(wszTrimmed, L' '))
        return TRUE;

    if (wcschr(wszTrimmed, L'-') || wcschr(wszTrimmed, L'=') ||
        wcschr(wszTrimmed, L'%') || wcschr(wszTrimmed, L'$') ||
        wcschr(wszTrimmed, L'@'))
        return TRUE;

    return FALSE;
}

static BOOL TerminalAI_IsLikelyNaturalLanguage(const WCHAR* wszLine) {
    WCHAR wszTrimmed[TERM_INPUT_MAX];
    int wordCount = 0;
    BOOL hasStopWord = FALSE;

    if (!wszLine || !wszLine[0]) return FALSE;
    lstrcpynW(wszTrimmed, wszLine, ARRAYSIZE(wszTrimmed));
    TerminalAI_TrimWideInPlace(wszTrimmed);
    if (!wszTrimmed[0]) return FALSE;
    if (TerminalAI_IsLikelyShellCommand(wszTrimmed))
        return FALSE;

    if (wcschr(wszTrimmed, L'?'))
        return TRUE;

    if (StrStrIW(wszTrimmed, L"can you") || StrStrIW(wszTrimmed, L"could you") ||
        StrStrIW(wszTrimmed, L"please ") || StrStrIW(wszTrimmed, L"show me") ||
        StrStrIW(wszTrimmed, L"list ") || StrStrIW(wszTrimmed, L"open ") ||
        StrStrIW(wszTrimmed, L"run ") || StrStrIW(wszTrimmed, L"build ") ||
        StrStrIW(wszTrimmed, L"test ") || StrStrIW(wszTrimmed, L"find ") ||
        StrStrIW(wszTrimmed, L"search ") || StrStrIW(wszTrimmed, L"what ") ||
        StrStrIW(wszTrimmed, L"where "))
        return TRUE;

    for (int i = 0; wszTrimmed[i]; ) {
        while (wszTrimmed[i] && TerminalAI_IsSpaceW(wszTrimmed[i]))
            i++;
        if (!wszTrimmed[i])
            break;
        wordCount++;
        if ((wszTrimmed[i] == L't' || wszTrimmed[i] == L'T') && StrCmpNIW(&wszTrimmed[i], L"the", 3) == 0)
            hasStopWord = TRUE;
        if ((wszTrimmed[i] == L't' || wszTrimmed[i] == L'T') && StrCmpNIW(&wszTrimmed[i], L"this", 4) == 0)
            hasStopWord = TRUE;
        if ((wszTrimmed[i] == L'd' || wszTrimmed[i] == L'D') && StrCmpNIW(&wszTrimmed[i], L"directory", 9) == 0)
            hasStopWord = TRUE;
        while (wszTrimmed[i] && !TerminalAI_IsSpaceW(wszTrimmed[i]))
            i++;
    }

    return wordCount >= 3 || hasStopWord;
}

static void TerminalAI_CopyWideToUtf8(const WCHAR* wszText, char* szOut, int cchOut) {
    if (!szOut || cchOut <= 0) return;
    szOut[0] = 0;
    if (!wszText || !wszText[0]) return;
    WideCharToMultiByte(CP_UTF8, 0, wszText, -1, szOut, cchOut, NULL, NULL);
}

static void TerminalAI_RecordSubmittedUtf8(const char* cmd) {
    int n;
    if (!cmd || !cmd[0]) {
        g_termAI.wszLastSubmitted[0] = 0;
        g_termAI.szLastSubmitted[0] = 0;
        g_termAI.szRecentOutput[0] = 0;
        g_termAI.bCommandFailureSuggested = FALSE;
        return;
    }

    lstrcpynA(g_termAI.szLastSubmitted, cmd, ARRAYSIZE(g_termAI.szLastSubmitted));
    TerminalAI_TrimAnsiInPlace(g_termAI.szLastSubmitted);
    n = MultiByteToWideChar(CP_UTF8, 0, g_termAI.szLastSubmitted, -1,
                            g_termAI.wszLastSubmitted, ARRAYSIZE(g_termAI.wszLastSubmitted));
    if (n <= 0)
        g_termAI.wszLastSubmitted[0] = 0;
    g_termAI.szRecentOutput[0] = 0;
    g_termAI.bCommandFailureSuggested = FALSE;
}

static void TerminalAI_AppendOutputChunk(const char* pszText, int cchText) {
    int curLen;
    int copyLen;
    if (!pszText || cchText <= 0)
        return;

    curLen = (int)strlen(g_termAI.szRecentOutput);
    copyLen = cchText;
    if (copyLen >= TERM_AI_OUTPUT_MAX - 1) {
        pszText += copyLen - (TERM_AI_OUTPUT_MAX - 1);
        copyLen = TERM_AI_OUTPUT_MAX - 1;
        curLen = 0;
    }
    if (curLen + copyLen >= TERM_AI_OUTPUT_MAX) {
        int keep = TERM_AI_OUTPUT_MAX - copyLen - 1;
        if (keep < 0) keep = 0;
        MoveMemory(g_termAI.szRecentOutput, g_termAI.szRecentOutput + (curLen - keep), keep);
        g_termAI.szRecentOutput[keep] = 0;
        curLen = keep;
    }
    CopyMemory(g_termAI.szRecentOutput + curLen, pszText, copyLen);
    g_termAI.szRecentOutput[curLen + copyLen] = 0;
}

static BOOL TerminalAI_OutputLooksLikeCommandFailure(const char* pszText) {
    if (!pszText || !pszText[0]) return FALSE;
    return StrStrIA(pszText, "CommandNotFoundException") != NULL ||
           StrStrIA(pszText, "is not recognized as the name of a cmdlet") != NULL ||
           StrStrIA(pszText, "is not recognized as an internal or external command") != NULL ||
           StrStrIA(pszText, "command not found") != NULL ||
           StrStrIA(pszText, "No such file or directory") != NULL;
}

static void TerminalAI_CenterDialogOnParent(HWND hwnd) {
    HWND hwndParent;
    RECT rcDlg;
    RECT rcParent;
    int x;
    int y;

    if (!hwnd)
        return;

    hwndParent = GetWindow(hwnd, GW_OWNER);
    if (!hwndParent || !IsWindow(hwndParent))
        hwndParent = g_hwndMain;
    if (!hwndParent || !IsWindow(hwndParent))
        return;

    GetWindowRect(hwnd, &rcDlg);
    GetWindowRect(hwndParent, &rcParent);

    x = rcParent.left + ((rcParent.right - rcParent.left) - (rcDlg.right - rcDlg.left)) / 2;
    y = rcParent.top + ((rcParent.bottom - rcParent.top) - (rcDlg.bottom - rcDlg.top)) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
}

static int TerminalAI_BuildSuggestionDialogTemplate(BYTE* buffer, int cbBuffer) {
    DLGTEMPLATE* pDlg;
    DLGITEMTEMPLATE* pItem;
    WORD* p;

    if (!buffer || cbBuffer < 256)
        return 0;

    ZeroMemory(buffer, cbBuffer);

    pDlg = (DLGTEMPLATE*)buffer;
    pDlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | WS_VISIBLE;
    pDlg->cdit = 2;
    pDlg->x = 0;
    pDlg->y = 0;
    pDlg->cx = 232;
    pDlg->cy = 140;

    p = (WORD*)(pDlg + 1);
    *p++ = 0;
    *p++ = 0;
    {
        LPCWSTR title = L"Bikode AI Terminal";
        while (*title) *p++ = *title++;
        *p++ = 0;
    }

    if ((ULONG_PTR)p % 4) p++;
    pItem = (DLGITEMTEMPLATE*)p;
    pItem->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_DEFPUSHBUTTON;
    pItem->x = 132; pItem->y = 118; pItem->cx = 44; pItem->cy = 16;
    pItem->id = IDOK;
    p = (WORD*)(pItem + 1);
    *p++ = 0xFFFF; *p++ = 0x0080;
    *p++ = 0;
    *p++ = 0;

    if ((ULONG_PTR)p % 4) p++;
    pItem = (DLGITEMTEMPLATE*)p;
    pItem->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_PUSHBUTTON;
    pItem->x = 180; pItem->y = 118; pItem->cx = 44; pItem->cy = 16;
    pItem->id = IDCANCEL;
    p = (WORD*)(pItem + 1);
    *p++ = 0xFFFF; *p++ = 0x0080;
    *p++ = 0;
    *p++ = 0;

    return (int)((BYTE*)p - buffer);
}

static void TerminalAI_LayoutSuggestionDialog(HWND hwnd) {
    RECT rc;
    HWND hRun;
    HWND hCancel;
    int buttonW = 98;
    int buttonH = 30;
    int gap = 10;
    int bottom = 16;

    GetClientRect(hwnd, &rc);
    hRun = GetDlgItem(hwnd, IDOK);
    hCancel = GetDlgItem(hwnd, IDCANCEL);

    if (hCancel) {
        MoveWindow(hCancel,
            rc.right - buttonW - 16,
            rc.bottom - buttonH - bottom,
            buttonW,
            buttonH,
            TRUE);
    }
    if (hRun) {
        MoveWindow(hRun,
            rc.right - (buttonW * 2) - gap - 16,
            rc.bottom - buttonH - bottom,
            buttonW,
            buttonH,
            TRUE);
    }
}

static void TerminalAI_DrawSuggestionDialog(HWND hwnd, HDC hdc) {
    RECT rcClient;
    RECT rcCard;
    RECT rcHeader;
    RECT rcChip;
    RECT rcModeChip;
    RECT rcTitle;
    RECT rcCmdBox;
    RECT rcCmdText;
    RECT rcFooter;
    RECT rcAccent;
    RECT rcBlip;
    HFONT hOldFont;
    COLORREF oldText;
    HBRUSH hBrush;
    TerminalAISuggestionDialogData* data;
    LPCWSTR wszModeText;

    GetClientRect(hwnd, &rcClient);
    data = (TerminalAISuggestionDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);
    wszModeText = (data && data->mode == TERM_AI_CORRECT) ? L"FIXED COMMAND" : L"FRESH PROMPT";

    BikodeTheme_FillHalftone(hdc, &rcClient, BikodeTheme_GetColor(BKCLR_APP_BG));

    rcCard = rcClient;
    InflateRect(&rcCard, -12, -12);
    BikodeTheme_DrawRoundedPanel(hdc, &rcCard,
        BikodeTheme_GetColor(BKCLR_SURFACE_MAIN),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetMetric(BKMETRIC_RADIUS_DIALOG),
        TRUE);

    rcHeader = rcCard;
    rcHeader.bottom = rcHeader.top + 44;
    BikodeTheme_DrawCutCornerPanel(hdc, &rcHeader,
        BikodeTheme_GetColor(BKCLR_SURFACE_RAISED),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        12,
        FALSE);

    rcAccent = rcHeader;
    rcAccent.left += 10;
    rcAccent.top += 8;
    rcAccent.right = rcAccent.left + 6;
    rcAccent.bottom -= 8;
    hBrush = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
    FillRect(hdc, &rcAccent, hBrush);
    DeleteObject(hBrush);

    rcBlip = rcHeader;
    rcBlip.left = rcHeader.right - 36;
    rcBlip.right = rcBlip.left + 8;
    rcBlip.top += 10;
    rcBlip.bottom = rcBlip.top + 22;
    hBrush = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_HOT_MAGENTA));
    FillRect(hdc, &rcBlip, hBrush);
    DeleteObject(hBrush);
    OffsetRect(&rcBlip, 12, 0);
    hBrush = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));
    FillRect(hdc, &rcBlip, hBrush);
    DeleteObject(hBrush);

    rcChip.left = rcHeader.left + 22;
    rcChip.top = rcHeader.top + 10;
    rcChip.right = rcChip.left + 112;
    rcChip.bottom = rcChip.top + 22;
    BikodeTheme_DrawChip(hdc, &rcChip, L"AI TERMINAL",
        BikodeTheme_GetColor(BKCLR_SURFACE_ELEVATED),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
        BikodeTheme_GetFont(BKFONT_UI_SMALL),
        TRUE,
        BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));

    rcModeChip = rcChip;
    rcModeChip.left = rcChip.right + 10;
    rcModeChip.right = rcModeChip.left + 108;
    BikodeTheme_DrawChip(hdc, &rcModeChip, wszModeText,
        BikodeTheme_GetColor(BKCLR_SURFACE_ELEVATED),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY),
        BikodeTheme_GetFont(BKFONT_UI_SMALL),
        TRUE,
        data && data->mode == TERM_AI_CORRECT
            ? BikodeTheme_GetColor(BKCLR_HOT_MAGENTA)
            : BikodeTheme_GetColor(BKCLR_ELECTRIC_CYAN));

    SetBkMode(hdc, TRANSPARENT);
    oldText = SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    hOldFont = (HFONT)SelectObject(hdc, BikodeTheme_GetFont(BKFONT_DISPLAY));

    rcTitle.left = rcCard.left + 18;
    rcTitle.top = rcHeader.bottom + 16;
    rcTitle.right = rcCard.right - 18;
    rcTitle.bottom = rcTitle.top + 30;
    DrawTextW(hdc, L"Run this in the current terminal?", -1, &rcTitle,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    rcCmdBox.left = rcCard.left + 18;
    rcCmdBox.top = rcTitle.bottom + 10;
    rcCmdBox.right = rcCard.right - 18;
    rcCmdBox.bottom = rcCard.bottom - 58;
    BikodeTheme_DrawRoundedPanel(hdc, &rcCmdBox,
        BikodeTheme_GetColor(BKCLR_EDITOR_BG),
        BikodeTheme_GetColor(BKCLR_STROKE_DARK),
        BikodeTheme_GetColor(BKCLR_STROKE_SOFT),
        BikodeTheme_GetMetric(BKMETRIC_RADIUS_PANEL),
        FALSE);

    rcAccent = rcCmdBox;
    rcAccent.left += 10;
    rcAccent.top += 10;
    rcAccent.right = rcAccent.left + 5;
    rcAccent.bottom -= 10;
    hBrush = CreateSolidBrush(BikodeTheme_GetColor(BKCLR_SIGNAL_YELLOW));
    FillRect(hdc, &rcAccent, hBrush);
    DeleteObject(hBrush);

    rcCmdText = rcCmdBox;
    rcCmdText.left += 24;
    rcCmdText.top += 12;
    rcCmdText.right -= 12;
    rcCmdText.bottom -= 12;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_PRIMARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_MONO));
    DrawTextW(hdc,
        (data && data->wszCommand[0]) ? data->wszCommand : L"",
        -1,
        &rcCmdText,
        DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);

    rcFooter.left = rcCard.left + 18;
    rcFooter.top = rcCmdBox.bottom + 10;
    rcFooter.right = rcCard.right - 150;
    rcFooter.bottom = rcCard.bottom - 12;
    SetTextColor(hdc, BikodeTheme_GetColor(BKCLR_TEXT_SECONDARY));
    SelectObject(hdc, BikodeTheme_GetFont(BKFONT_UI_SMALL));
    DrawTextW(hdc, L"Enter runs it. Esc cancels it.", -1, &rcFooter,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, hOldFont);
    SetTextColor(hdc, oldText);
}

static INT_PTR CALLBACK TerminalAI_SuggestionDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowLongPtr(hwnd, DWLP_USER, lParam);
        DarkMode_ApplyToDialog(hwnd);
        TerminalAI_CenterDialogOnParent(hwnd);
        TerminalAI_LayoutSuggestionDialog(hwnd);
        return TRUE;

    case WM_SIZE:
        TerminalAI_LayoutSuggestionDialog(hwnd);
        return TRUE;

    case WM_ERASEBKGND:
        return TRUE;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        TerminalAI_DrawSuggestionDialog(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return TRUE;
    }

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
        RECT rcFocus;

        if (!dis || dis->CtlType != ODT_BUTTON)
            break;

        BikodeTheme_DrawButton(dis->hDC, &dis->rcItem,
            dis->CtlID == IDOK ? L"Run It" : L"Cancel",
            dis->CtlID == IDOK ? BKGLYPH_TERMINAL : BKGLYPH_NONE,
            (dis->itemState & ODS_HOTLIGHT) != 0,
            (dis->itemState & ODS_SELECTED) != 0,
            dis->CtlID == IDOK,
            FALSE);
        if (dis->itemState & ODS_FOCUS) {
            rcFocus = dis->rcItem;
            InflateRect(&rcFocus, -4, -4);
            DrawFocusRect(dis->hDC, &rcFocus);
        }
        return TRUE;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            TerminalAISuggestionDialogData* data = (TerminalAISuggestionDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);
            if (data)
                data->accepted = (LOWORD(wParam) == IDOK);
            EndDialog(hwnd, LOWORD(wParam));
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static BOOL TerminalAI_ShowSuggestionDialog(HWND hwndParent, const WCHAR* wszCommand, TermAIRequestMode mode) {
    BYTE buffer[512];
    TerminalAISuggestionDialogData data;

    ZeroMemory(&data, sizeof(data));
    lstrcpynW(data.wszCommand, wszCommand ? wszCommand : L"", ARRAYSIZE(data.wszCommand));
    data.mode = mode;

    if (!TerminalAI_BuildSuggestionDialogTemplate(buffer, sizeof(buffer)))
        return FALSE;

    DialogBoxIndirectParamW(GetModuleHandle(NULL),
                            (DLGTEMPLATE*)buffer,
                            hwndParent,
                            TerminalAI_SuggestionDlgProc,
                            (LPARAM)&data);
    return data.accepted;
}

static BOOL TerminalAI_ParseSuggestionLooseCommand(const char* pszResponse,
                                                   char* szCommand, int cchCommand) {
    const char* start;
    const char* end;
    int len;
    char quote = 0;

    if (!pszResponse || !pszResponse[0] || !szCommand || cchCommand <= 0)
        return FALSE;

    start = StrStrIA(pszResponse, "\"command\"");
    if (!start) start = StrStrIA(pszResponse, "'command'");
    if (!start) start = StrStrIA(pszResponse, "command");
    if (!start)
        return FALSE;

    start = strchr(start, ':');
    if (!start)
        return FALSE;
    start++;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (!*start)
        return FALSE;

    if (*start == '"' || *start == '\'' || *start == '`') {
        quote = *start++;
        end = start;
        while (*end && *end != quote) {
            if (*end == '\\' && end[1])
                end += 2;
            else
                end++;
        }
    } else {
        end = start;
        while (*end && *end != '\r' && *end != '\n' && *end != ',' && *end != '}')
            end++;
    }

    if (end <= start)
        return FALSE;

    len = (int)min((size_t)(end - start), (size_t)(cchCommand - 1));
    memcpy(szCommand, start, len);
    szCommand[len] = 0;
    TerminalAI_UnwrapCommand(szCommand);
    return szCommand[0] != 0;
}

static BOOL TerminalAI_ParseSuggestionJson(const char* pszResponse,
                                           char* szCommand, int cchCommand,
                                           char* szReason, int cchReason) {
    const char* pszJson = pszResponse;
    JsonReader r;
    EJsonToken tok;

    if (!pszResponse || !pszResponse[0]) return FALSE;
    if (!szCommand || cchCommand <= 0) return FALSE;
    szCommand[0] = 0;
    if (szReason && cchReason > 0)
        szReason[0] = 0;

    while (*pszJson && *pszJson != '{')
        pszJson++;
    if (*pszJson != '{')
        return FALSE;
    if (!JsonReader_Init(&r, pszJson, (int)strlen(pszJson)))
        return FALSE;
    if (JsonReader_Next(&r) != JSON_OBJECT_START)
        return FALSE;

    while ((tok = JsonReader_Next(&r)) != JSON_OBJECT_END && tok != JSON_ERROR) {
        const char* key;
        if (tok != JSON_KEY)
            continue;
        key = JsonReader_GetString(&r);
        tok = JsonReader_Next(&r);
        if ((strcmp(key, "command") == 0 || strcmp(key, "cmd") == 0) && tok == JSON_STRING) {
            lstrcpynA(szCommand, JsonReader_GetString(&r), cchCommand);
        } else if (szReason && cchReason > 0 && strcmp(key, "reason") == 0 && tok == JSON_STRING) {
            lstrcpynA(szReason, JsonReader_GetString(&r), cchReason);
        } else if (!JsonReader_SkipValue(&r) && JsonReader_IsError(&r)) {
            break;
        }
    }

    TerminalAI_TrimAnsiInPlace(szCommand);
    if (szReason && cchReason > 0)
        TerminalAI_TrimAnsiInPlace(szReason);
    return szCommand[0] != 0;
}

static void TerminalAI_UnwrapCommand(char* szCommand) {
    int len;
    if (!szCommand || !szCommand[0]) return;
    TerminalAI_TrimAnsiInPlace(szCommand);
    len = (int)strlen(szCommand);
    if (len >= 2 && ((szCommand[0] == '`' && szCommand[len - 1] == '`') ||
                     (szCommand[0] == '"' && szCommand[len - 1] == '"') ||
                     (szCommand[0] == '\'' && szCommand[len - 1] == '\''))) {
        MoveMemory(szCommand, szCommand + 1, len - 2);
        szCommand[len - 2] = 0;
    }
    if (StrStrIA(szCommand, "command:") == szCommand)
        MoveMemory(szCommand, szCommand + 8, strlen(szCommand + 8) + 1);
    if (StrStrIA(szCommand, "cmd:") == szCommand)
        MoveMemory(szCommand, szCommand + 4, strlen(szCommand + 4) + 1);
    TerminalAI_TrimAnsiInPlace(szCommand);
}

static BOOL TerminalAI_ParseSuggestionFallback(const char* pszResponse,
                                               char* szCommand, int cchCommand,
                                               char* szReason, int cchReason) {
    const char* start;
    const char* end;
    const char* line;
    int len;

    if (!pszResponse || !pszResponse[0]) return FALSE;
    szCommand[0] = 0;
    if (szReason && cchReason > 0)
        szReason[0] = 0;

    if (TerminalAI_ParseSuggestionLooseCommand(pszResponse, szCommand, cchCommand))
        return TRUE;

    start = strstr(pszResponse, "```");
    if (start) {
        start += 3;
        while (*start == '\r' || *start == '\n')
            start++;
        end = strstr(start, "```");
        if (end && end > start) {
            len = (int)min((size_t)(end - start), (size_t)(cchCommand - 1));
            memcpy(szCommand, start, len);
            szCommand[len] = 0;
        }
    }

    if (!szCommand[0]) {
        start = strchr(pszResponse, '`');
        if (start) {
            start++;
            end = strchr(start, '`');
            if (end && end > start) {
                len = (int)min((size_t)(end - start), (size_t)(cchCommand - 1));
                memcpy(szCommand, start, len);
                szCommand[len] = 0;
            }
        }
    }

    if (!szCommand[0]) {
        line = pszResponse;
        while (*line == '\r' || *line == '\n')
            line++;
        end = line;
        while (*end && *end != '\r' && *end != '\n')
            end++;
        len = (int)min((size_t)(end - line), (size_t)(cchCommand - 1));
        memcpy(szCommand, line, len);
        szCommand[len] = 0;
    }

    TerminalAI_UnwrapCommand(szCommand);
    if (szReason && cchReason > 0 && !szReason[0])
        lstrcpynA(szReason, "Bikode AI suggested a better terminal command.", cchReason);
    return szCommand[0] != 0;
}

static BOOL TerminalAI_ParseSuggestion(const char* pszResponse,
                                       char* szCommand, int cchCommand,
                                       char* szReason, int cchReason) {
    if (TerminalAI_ParseSuggestionJson(pszResponse, szCommand, cchCommand, szReason, cchReason))
        return TRUE;
    return TerminalAI_ParseSuggestionFallback(pszResponse, szCommand, cchCommand, szReason, cchReason);
}

static BOOL TerminalAI_StartSuggestion(HWND hwnd, TermAIRequestMode mode, const WCHAR* wszInput, const char* pszFailure) {
    WCHAR wszPrompt[TERM_INPUT_MAX] = L"";
    WCHAR wszCwd[MAX_PATH] = L"";
    char szShell[64];
    char szInput[TERM_CMD_MAX];
    char szPrompt[TERM_CMD_MAX];
    char szCwd[TERM_CMD_MAX];
    char szUserPrompt[TERM_AI_PROMPT_MAX];
    const AIProviderConfig* pCfg;
    const char* pszSystemPrompt =
        "You translate terminal intent into exactly one shell command. "
        "Return only the command text. "
        "Respect the named shell. Prefer safe, non-destructive commands. "
        "If the user typed plain English, translate it into the best command for that shell. "
        "If the command failed, suggest the corrected command. "
        "If you cannot infer a safe command, return an empty response. "
        "Do not use JSON, markdown fences, labels, quotes, or explanation.";

    if (g_termAI.bPending)
        return FALSE;

    pCfg = AIBridge_GetProviderConfig();
    if (!pCfg)
        return FALSE;

    TerminalAI_GetPromptLine(wszPrompt, ARRAYSIZE(wszPrompt));
    GetCurrentDirectoryW(MAX_PATH, wszCwd);
    TerminalAI_CopyWideToUtf8(g_shells[g_curShell].label, szShell, ARRAYSIZE(szShell));
    TerminalAI_CopyWideToUtf8(wszInput ? wszInput : L"", szInput, ARRAYSIZE(szInput));
    TerminalAI_CopyWideToUtf8(wszPrompt, szPrompt, ARRAYSIZE(szPrompt));
    TerminalAI_CopyWideToUtf8(wszCwd, szCwd, ARRAYSIZE(szCwd));

    if (mode == TERM_AI_CORRECT) {
        _snprintf_s(szUserPrompt, sizeof(szUserPrompt), _TRUNCATE,
            "Mode: repair a failed terminal command.\n"
            "Shell: %s\n"
            "Working directory: %s\n"
            "Visible prompt: %s\n"
            "Original command: %s\n"
            "Recent terminal output:\n%s\n",
            szShell[0] ? szShell : "shell",
            szCwd[0] ? szCwd : "(unknown)",
            szPrompt[0] ? szPrompt : "(none)",
            szInput[0] ? szInput : "(none)",
            pszFailure && pszFailure[0] ? pszFailure : "(no error text)");
    } else {
        _snprintf_s(szUserPrompt, sizeof(szUserPrompt), _TRUNCATE,
            "Mode: translate plain English terminal intent into a command.\n"
            "Shell: %s\n"
            "Working directory: %s\n"
            "Visible prompt: %s\n"
            "User text: %s\n",
            szShell[0] ? szShell : "shell",
            szCwd[0] ? szCwd : "(unknown)",
            szPrompt[0] ? szPrompt : "(none)",
            szInput[0] ? szInput : "(none)");
    }

    g_termAI.bPending = TRUE;
    g_termAI.mode = mode;
    if (mode == TERM_AI_CORRECT)
        g_termAI.bCommandFailureSuggested = TRUE;

    if (!AIDirectCall_ChatAsync(pCfg, pszSystemPrompt, szUserPrompt, g_hwndPanel ? g_hwndPanel : hwnd)) {
        g_termAI.bPending = FALSE;
        Terminal_AppendTranscript(g_hwndMain ? g_hwndMain : hwnd,
            "\r\n[Bikode AI] Terminal suggestions are unavailable right now.\r\n");
        return FALSE;
    }

    return TRUE;
}

static void TerminalAI_HandleSuggestionResponse(HWND hwnd, char* pszResponse) {
    char szCommand[TERM_CMD_MAX];
    WCHAR wszCommand[TERM_CMD_MAX];
    TermAIRequestMode mode = g_termAI.mode;

    g_termAI.bPending = FALSE;

    if (!pszResponse || !pszResponse[0]) {
        Terminal_AppendTranscript(g_hwndMain ? g_hwndMain : hwnd,
            "\r\n[Bikode AI] No terminal suggestion came back.\r\n");
        return;
    }

    if (StrStrIA(pszResponse, "Error:") == pszResponse) {
        Terminal_AppendTranscript(g_hwndMain ? g_hwndMain : hwnd, "\r\n[Bikode AI] ");
        Terminal_AppendTranscript(g_hwndMain ? g_hwndMain : hwnd, pszResponse);
        Terminal_AppendTranscript(g_hwndMain ? g_hwndMain : hwnd, "\r\n");
        return;
    }

    if (!TerminalAI_ParseSuggestion(pszResponse, szCommand, ARRAYSIZE(szCommand), NULL, 0)) {
        Terminal_AppendTranscript(g_hwndMain ? g_hwndMain : hwnd,
            "\r\n[Bikode AI] I could not turn that into a terminal command.\r\n");
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, szCommand, -1, wszCommand, ARRAYSIZE(wszCommand));

    if (TerminalAI_ShowSuggestionDialog(g_hwndMain ? g_hwndMain : hwnd, wszCommand, mode))
        TerminalAI_ExecuteSuggestedCommand(szCommand, mode);
}

static void FillPanelSurface(HDC hdc, const RECT* rc, COLORREF color, BOOL textured) {
    if (!rc) return;
    if (textured) {
        BikodeTheme_FillHalftone(hdc, rc, color);
        return;
    }

    HBRUSH hBrush = CreateSolidBrush(color);
    FillRect(hdc, rc, hBrush);
    DeleteObject(hBrush);
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

        FillPanelSurface(mem, &rc, defBg, dk);

        HFONT ofont = NULL;
        if (g_fontGrid) ofont = SelectObject(mem, g_fontGrid);

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
            if (rowBaseBg != defBg) {
                RECT rowRc = { 0, y, rc.right, min(y + g_cellH, rc.bottom) };
                FillPanelSurface(mem, &rowRc, rowBaseBg, dk);
            }
            if (dk && tone != ROW_NORMAL) {
                RECT accentRc = { 0, y, 3, y + g_cellH };
                HBRUSH hAcc = CreateSolidBrush(accent);
                FillRect(mem, &accentRc, hAcc);
                DeleteObject(hAcc);
            }
            int c = 0;
            while (c < g->cols) {
                BOOL sel = InSel(r, c);
                BOOL opaqueBg = sel || row[c].bg != 0;
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
                    BOOL opaqueBg2 = cs2 || row[c].bg != 0;
                    COLORREF f2 = cs2 ? (dk?RGB(255,255,255):RGB(0,0,0)) : GetFG(row[c].fg);
                    COLORREF b2 = cs2 ? (dk ? C_DKSEL : RGB(180,215,255))
                                      : (row[c].bg == 0 ? rowBaseBg : GetBG(row[c].bg));
                    if (!cs2 && row[c].fg == 7 && dk && tone != ROW_NORMAL) {
                        f2 = BikodeTheme_Mix(accent, C_DKFG, 54);
                    }
                    if (opaqueBg2 != opaqueBg || f2 != fg || b2 != bg) break;
                    run[rl++] = row[c].ch;
                    c++;
                }
                SetTextColor(mem, fg);
                RECT cr = { cs * g_cellW, y, (cs + rl) * g_cellW, y + g_cellH };
                if (opaqueBg) {
                    SetBkMode(mem, OPAQUE);
                    SetBkColor(mem, bg);
                    ExtTextOutW(mem, cs * g_cellW, y, ETO_OPAQUE | ETO_CLIPPED, &cr, run, rl, NULL);
                } else {
                    SetBkMode(mem, TRANSPARENT);
                    ExtTextOutW(mem, cs * g_cellW, y, ETO_CLIPPED, &cr, run, rl, NULL);
                }
            }
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
            else {
                TerminalAI_ResetInputLine();
                Terminal_Write("\x03", 1);
            }
            return 0;
        }

        /* Skip chars already handled as complex operations in WM_KEYDOWN */
        if (wch == 0x16) return 0;  /* Ctrl+V → paste handled in WM_KEYDOWN */
        if (wch == 0x0C) return 0;  /* Ctrl+L → clear handled in WM_KEYDOWN */

        if (wch == L'\r') {
            WCHAR wszSubmitted[TERM_INPUT_MAX];
            char szSubmitted[TERM_CMD_MAX];

            TerminalAI_GetSubmittedLine(wszSubmitted, ARRAYSIZE(wszSubmitted));
            if (wszSubmitted[0]) {
                if (TerminalAI_IsLikelyNaturalLanguage(wszSubmitted) &&
                    TerminalAI_StartSuggestion(g_hwndPanel ? g_hwndPanel : hwnd, TERM_AI_TRANSLATE, wszSubmitted, NULL)) {
                    TerminalAI_ResetInputLine();
                    g_hasSel = FALSE;
                    return 0;
                }
                TerminalAI_CopyWideToUtf8(wszSubmitted, szSubmitted, ARRAYSIZE(szSubmitted));
                TerminalAI_RecordSubmittedUtf8(szSubmitted);
            } else {
                g_termAI.szRecentOutput[0] = 0;
                g_termAI.bCommandFailureSuggested = FALSE;
            }
            TerminalAI_ResetInputLine();
        } else if (wch == L'\b') {
            TerminalAI_BackspaceInputChar();
        } else if (wch >= L' ' || wch == L'\t') {
            TerminalAI_AppendInputChar(wch);
        }

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
                        TerminalAI_AppendInputText(wz);
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
            TerminalAI_ResetInputLine();
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
        case VK_UP:     TerminalAI_ResetInputLine(); Terminal_Write("\033[A", 3); return 0;
        case VK_DOWN:   TerminalAI_ResetInputLine(); Terminal_Write("\033[B", 3); return 0;
        case VK_RIGHT:  TerminalAI_ResetInputLine(); Terminal_Write("\033[C", 3); return 0;
        case VK_LEFT:   TerminalAI_ResetInputLine(); Terminal_Write("\033[D", 3); return 0;
        case VK_DELETE: TerminalAI_ResetInputLine(); Terminal_Write("\033[3~", 4); return 0;
        case VK_HOME:   TerminalAI_ResetInputLine(); Terminal_Write("\033[H", 3); return 0;
        case VK_END:    TerminalAI_ResetInputLine(); Terminal_Write("\033[F", 3); return 0;
        case VK_PRIOR:  TerminalAI_ResetInputLine(); Terminal_Write("\033[5~", 4); return 0;
        case VK_NEXT:   TerminalAI_ResetInputLine(); Terminal_Write("\033[6~", 4); return 0;
        case VK_INSERT: TerminalAI_ResetInputLine(); Terminal_Write("\033[2~", 4); return 0;
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
                TerminalAI_ResetInputLine();
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
        FillPanelSurface(dc, &rc, dk ? C_DKBG : C_LTBG, dk);
        RECT rs = rc; rs.bottom = SPLITTER_H;
        FillPanelSurface(dc, &rs, dk ? C_DKSPLIT : C_LTSPLIT, FALSE);
        PaintHeader(hwnd, dc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND: {
        HDC dc = (HDC)wParam;
        RECT rc; GetClientRect(hwnd, &rc);
        BOOL dk = DarkMode_IsEnabled();
        FillPanelSurface(dc, &rc, dk ? C_DKBG : C_LTBG, dk);
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
            TerminalAI_AppendOutputChunk(d, n);
            if (!g_termAI.bPending &&
                !g_termAI.bCommandFailureSuggested &&
                g_termAI.szLastSubmitted[0] &&
                TerminalAI_OutputLooksLikeCommandFailure(g_termAI.szRecentOutput) &&
                !TerminalAI_StartSuggestion(hwnd, TERM_AI_CORRECT, g_termAI.wszLastSubmitted, g_termAI.szRecentOutput)) {
                g_termAI.bCommandFailureSuggested = TRUE;
            }
            if (g_term->hwndView)
                InvalidateRect(g_term->hwndView, NULL, FALSE);
        }
        if (d) n2e_Free(d);
        return 0;
    }
    case WM_AI_DIRECT_RESPONSE: {
        char* pszResponse = (char*)lParam;
        TerminalAI_HandleSuggestionResponse(hwnd, pszResponse);
        if (pszResponse)
            free(pszResponse);
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

