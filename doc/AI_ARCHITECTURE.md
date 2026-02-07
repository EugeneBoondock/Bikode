# AI-Native Minimal Editor â€” Architecture & Design Document

**Codename:** Biko
**Base:** Notepad2e (BSD 3-Clause)
**Platform:** Windows (Win32 / Scintilla)
**Language:** C / C++

---

## 1. High-Level Architecture

The system is two processes: an editor and an AI engine. They communicate over a local named pipe using length-prefixed JSON messages. The editor never depends on the engine being alive.

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚                   BIKO EDITOR (Win32)                â”‚
â”‚                                                     â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”‚
â”‚  â”‚ Scintillaâ”‚  â”‚ Undo/Redoâ”‚  â”‚  Patch Previewer  â”‚  â”‚
â”‚  â”‚  Control â”‚  â”‚  Stack   â”‚  â”‚  (Inline Diff)    â”‚  â”‚
â”‚  â””â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”˜  â””â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”˜  â””â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â”‚
â”‚       â”‚              â”‚               â”‚              â”‚
â”‚  â”Œâ”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”      â”‚
â”‚  â”‚         Editor Core (Notepad2.c)          â”‚      â”‚
â”‚  â”‚  MsgCommand / MsgNotify / State Globals   â”‚      â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜      â”‚
â”‚                       â”‚                             â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”      â”‚
â”‚  â”‚           AI Bridge (aiBridge.c)          â”‚      â”‚
â”‚  â”‚  Named pipe client Â· JSON serialization   â”‚      â”‚
â”‚  â”‚  Request queue Â· Timeout Â· Retry          â”‚      â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜      â”‚
â”‚                       â”‚                             â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                        â”‚  Named Pipe (\\.\pipe\biko_ai)
                        â”‚  Length-prefixed JSON
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¼â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚                       â”‚                             â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”      â”‚
â”‚  â”‚         Pipe Server / Router              â”‚      â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜      â”‚
â”‚                       â”‚                             â”‚
â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”  â”‚
â”‚  â”‚Context â”‚  â”‚ Prompt Builder â”‚  â”‚ Model Router â”‚  â”‚
â”‚  â”‚Gathererâ”‚  â”‚ & Template Eng â”‚  â”‚ (local/cloud)â”‚  â”‚
â”‚  â””â”€â”€â”€â”¬â”€â”€â”€â”€â”˜  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜  â””â”€â”€â”€â”€â”€â”€â”¬â”€â”€â”€â”€â”€â”€â”€â”˜  â”‚
â”‚      â”‚                                  â”‚          â”‚
â”‚  â”Œâ”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”      â”‚
â”‚  â”‚          Model Adapters                  â”‚      â”‚
â”‚  â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â” â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â” â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â” â”‚      â”‚
â”‚  â”‚  â”‚ llama   â”‚ â”‚ OpenAI   â”‚ â”‚ Anthropic â”‚ â”‚      â”‚
â”‚  â”‚  â”‚ .cpp    â”‚ â”‚ API      â”‚ â”‚ API       â”‚ â”‚      â”‚
â”‚  â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜ â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜ â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜ â”‚      â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜      â”‚
â”‚                                                     â”‚
â”‚               AI ENGINE (biko-engine.exe)           â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

### Component Responsibilities

| Component | Process | Language | Role |
|---|---|---|---|
| Editor Core | biko.exe | C | Text editing, file I/O, UI, undo/redo, command dispatch |
| AI Bridge | biko.exe | C | Serialize context, send requests, receive patches, manage connection |
| Patch Previewer | biko.exe | C | Render inline diffs in Scintilla using annotation/indicator layers |
| AI Engine | biko-engine.exe | C++ or Rust | Context reasoning, prompt orchestration, diff generation, model routing |
| Context Gatherer | biko-engine.exe | â€” | Scan repo, index symbols, rank file relevance |
| Model Router | biko-engine.exe | â€” | Route to local (llama.cpp) or cloud (OpenAI/Anthropic) backends |

### Why Named Pipes

- Zero network exposure by default (no localhost port to scan)
- Built into Win32 (`CreateNamedPipe`, `ConnectNamedPipe`)
- Supports overlapped I/O for non-blocking reads in the editor
- Process-local security (can restrict to same logon session via DACL)
- Notepad2e already uses IPC patterns (WM_COPYDATA, named events, file mappings in `src/Extension/IPC/`)

### Failure Model

The editor never blocks on AI. All pipe communication is asynchronous (overlapped I/O on the editor side). If `biko-engine.exe` is not running, crashes, or times out:

- Status bar shows "AI: offline" (neutral, no error dialogs)
- All AI menu items gray out
- Editor continues as a normal text editor
- On reconnection, state is re-synced automatically

---

## 2. AI Request/Response Schema

All messages are length-prefixed UTF-8 JSON over the named pipe. Format:

```
[4 bytes: uint32 little-endian message length][JSON payload]
```

### 2.1 Request Schema

```jsonc
{
  // Header â€” present in every request
  "id": "req_001",                    // unique, monotonic per session
  "type": "patch" | "hint" | "explain" | "context_sync" | "ping",
  "version": 1,                       // schema version

  // Context â€” attached to patch/hint/explain requests
  "context": {
    "file": {
      "path": "C:\\proj\\src\\main.c",
      "language": "c",                // from Scintilla lexer ID
      "content": "...",               // full file text (UTF-8)
      "encoding": "utf-8",           // original encoding label
      "eol": "crlf"
    },
    "cursor": {
      "line": 42,                     // 0-based
      "column": 12,                   // 0-based, byte offset in line
      "selection": {                  // null if no selection
        "startLine": 40,
        "startCol": 0,
        "endLine": 45,
        "endCol": 22,
        "text": "..."                 // selected text
      }
    },
    "viewport": {
      "firstVisibleLine": 30,
      "lastVisibleLine": 70
    },
    "project": {
      "root": "C:\\proj",             // detected or configured project root
      "activeFiles": [                // other open files (if any in future)
        "C:\\proj\\src\\utils.h"
      ]
    }
  },

  // Intent â€” what the user wants
  "intent": {
    "action": "refactor" | "fix" | "explain" | "complete" | "transform" | "custom",
    "instruction": "Extract this block into a function", // user-provided or empty
    "scope": "selection" | "function" | "file"
  },

  // Engine hints â€” editor preferences
  "preferences": {
    "maxResponseTokens": 4096,
    "model": null,                    // null = engine decides
    "temperature": null               // null = engine decides
  }
}
```

### 2.2 Response Schema

```jsonc
{
  "id": "req_001",                    // echoed from request
  "status": "ok" | "error" | "partial",
  "version": 1,

  // Present when status == "ok" and type was "patch"
  "patches": [
    {
      "file": "C:\\proj\\src\\main.c",
      "diff": "--- a/src/main.c\n+++ b/src/main.c\n@@ -40,6 +40,10 @@\n ...",
      "description": "Extracted render_frame() from main loop"
    }
  ],

  // Present when type was "hint"
  "hint": {
    "text": "Consider using size_t here",
    "line": 42,
    "column": 12,
    "confidence": 0.85              // 0.0â€“1.0, editor can threshold display
  },

  // Present when type was "explain"
  "explanation": {
    "summary": "This function computes the CRC32 checksum...",
    "details": "..."                // optional extended explanation
  },

  // Present when status == "error"
  "error": {
    "code": "model_unavailable" | "context_too_large" | "timeout" | "rate_limited",
    "message": "Model server returned 503"
  },

  // Metadata
  "meta": {
    "model": "gpt-4o-mini",
    "inputTokens": 1200,
    "outputTokens": 340,
    "latencyMs": 1820,
    "contextFiles": [               // files the engine included beyond the current file
      "C:\\proj\\src\\utils.h"
    ]
  }
}
```

### 2.3 Streaming

For responsiveness, `patch` and `explain` responses may stream. The engine sends partial JSON chunks:

```jsonc
{"id":"req_001","status":"partial","chunk":"@@ -40,6 +40,10 @@\n context\n-old\n+new\n"}
{"id":"req_001","status":"partial","chunk":" more context\n"}
{"id":"req_001","status":"ok","patches":[...], "meta":{...}}
```

The editor buffers chunks and shows a "thinking..." indicator in the status bar. The final `"ok"` message contains the complete, canonical response.

---

## 3. Patch Application Flow

This is the critical path. Every AI-generated code change goes through this pipeline. No exceptions.

### 3.1 Lifecycle

```
User triggers AI action (hotkey / context menu)
        â”‚
        â–¼
Editor extracts context (file, cursor, selection)
        â”‚
        â–¼
AI Bridge sends request to engine via named pipe
        â”‚
        â–¼
Status bar: "AI: thinking..." (with cancel hotkey Esc)
        â”‚
        â–¼
Engine returns unified diff patch(es)
        â”‚
        â–¼
â”Œâ”€â”€â”€â”€â”€â”€â”€â”´â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚           PATCH PREVIEW MODE                  â”‚
â”‚                                               â”‚
â”‚  Scintilla shows inline diff:                 â”‚
â”‚  - Deleted lines: red background indicator    â”‚
â”‚  - Added lines: green annotation beneath      â”‚
â”‚  - Modified lines: side-by-side highlight     â”‚
â”‚                                               â”‚
â”‚  Status bar: "Patch: 3 files, +12 -5"         â”‚
â”‚  Toolbar: [Apply] [Reject] [Next Hunk]        â”‚
â”‚                                               â”‚
â”‚  Hotkeys:                                     â”‚
â”‚    Enter / Ctrl+Shift+A  â†’ Apply all          â”‚
â”‚    Escape                â†’ Reject all         â”‚
â”‚    Tab                   â†’ Next hunk          â”‚
â”‚    Space                 â†’ Toggle hunk         â”‚
â”‚    Ctrl+Z after apply    â†’ Undo entire patch   â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
        â”‚
        â–¼ (on Apply)
Editor enters UNDO GROUP:
  SCI_BEGINUNDOACTION
  For each hunk:
    - Compute target line range from diff header
    - SCI_DELETERANGE to remove old lines
    - SCI_INSERTTEXT to add new lines
  SCI_ENDUNDOACTION
        â”‚
        â–¼
Clear preview indicators / annotations
Status bar: "Patch applied (+12 -5). Ctrl+Z to undo."
```

### 3.2 Diff Parsing

The editor-side diff parser is intentionally minimal. It only needs to handle:

- Standard unified diff format (`---`, `+++`, `@@` headers)
- Context lines for matching (to handle off-by-few drift)
- Hunk offsets (`@@ -L,N +L,N @@`)

Implementation: a single C file, `diffApply.c`, ~400 lines. No dependency on `patch.exe` or any external tool.

### 3.3 Multi-File Patches

When the engine returns patches for multiple files:

1. Editor shows a file list in a simple dialog (not a tree view â€” this is Notepad, not an IDE):
   ```
   Patch changes 3 files:
     [x] src/main.c      (+8, -3)
     [x] src/utils.h      (+2, -0)
     [ ] src/config.c     (+1, -1)
   
   [Apply Selected]  [Cancel]
   ```
2. User selects which files to patch.
3. Each file is opened, patched inside `SCI_BEGINUNDOACTION`/`SCI_ENDUNDOACTION`, and marked modified.
4. A "patch group" undo restores all files on Ctrl+Z (requires custom undo journal â€” see 3.4).

### 3.4 Undo Journal

Scintilla's built-in undo is per-buffer. For multi-file atomic undo, the editor maintains a lightweight journal:

```c
typedef struct {
    UINT groupId;          // unique patch group ID
    WCHAR files[MAX_PATCH_FILES][MAX_PATH];
    int fileCount;
    // For each file: snapshot of SCI_GETUNDOCOLLECTION state before patch
    int undoSavePoints[MAX_PATCH_FILES];
} PATCH_UNDO_GROUP;
```

On "undo patch group": iterate files, load each, call `SCI_UNDO` repeatedly until reaching the save point. This is bounded (max undo steps = number of hunks applied).

### 3.5 Conflict Handling

If the file changed between request and response (user edited while AI was thinking):

1. Re-parse the diff against current buffer content.
2. If context lines match: apply normally.
3. If some hunks fail: show partial preview, mark failed hunks in the status bar.
4. Never silently drop or force a hunk.

---

## 4. Minimal UI Integration Plan

### 4.1 Principle: No New Chrome

No new panels, sidebars, or floating windows. All AI interaction uses existing Notepad2 UI surfaces:

| Surface | AI Usage |
|---|---|
| **Status bar** | Connection status, thinking indicator, patch summary, token cost |
| **Context menu** | "AI: Refactor", "AI: Explain", "AI: Fix", "AI: Transform..." |
| **Hotkeys** | `Ctrl+Shift+A` (AI action on selection), `Ctrl+Shift+E` (explain), `Alt+Enter` (apply suggestion) |
| **Scintilla indicators** | Inline diff preview (green/red backgrounds, annotations) |
| **Scintilla annotations** | Hint text below relevant lines |
| **Existing dialog style** | Multi-file patch confirmation, settings |
| **Toolbar** | One optional icon (brain) that shows engine status; click opens AI settings |

### 4.2 Status Bar Segments

The existing Notepad2e status bar has segments for line/col, encoding, EOL, lexer. Add one segment at the right edge:

```
Ln 42, Col 12 | UTF-8 | CRLF | C/C++ |        AI: ready
```

States:
- `AI: ready` â€” Engine connected, idle (default text color)
- `AI: thinking...` â€” Request in flight (animated ellipsis via timer)
- `AI: offline` â€” Engine not running (gray text)
- `AI: patch +12 -5` â€” Patch preview active (highlight color)
- `AI: error` â€” Last request failed (red, clears after 5s)

### 4.3 Context Menu Additions

Right-click context menu gains a separator and submenu:

```
â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
AI â–º
    Refactor          Ctrl+Shift+R
    Explain           Ctrl+Shift+E
    Fix               Ctrl+Shift+F
    Transform...      Ctrl+Shift+T
    â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    Open AI Settings
```

"Transform..." opens a single-line input dialog: "Describe the transformation:" with a text field. This is the escape hatch for custom instructions.

### 4.4 Inline Diff Preview â€” Scintilla Implementation

Scintilla supports:

- **Indicators** (`SCI_INDICSETSTYLE`): background/foreground highlighting on arbitrary ranges. Used for "deleted" highlighting (red tint, `INDIC_STRAIGHTBOX`).
- **Annotations** (`SCI_ANNOTATIONSETTEXT`): text displayed below a line. Used for "added" lines (green-tinted annotation below the preceding context line).
- **Margins**: a narrow margin with `+`/`-` markers for hunks (using `SCI_MARGINSETTEXT`).

The preview is **read-only decorations on the current buffer**. No phantom buffer or temporary file. The user sees their real code with visual overlays showing what would change.

### 4.5 Settings

AI settings stored in the existing INI file (`Notepad2.ini` â†’ `Biko.ini`):

```ini
[AI]
Enabled=1
EnginePath=biko-engine.exe
PipeName=biko_ai
TimeoutMs=30000
HintConfidenceThreshold=0.7
ShowTokenCost=0
MaxContextFiles=10
```

Configurable via:
- Menu: View â†’ AI Settings (opens a simple dialog, not a JSON file)
- Direct INI edit for advanced users

---

## 5. v0.1 MVP Feature List

Goal: Prove the architecture works end-to-end. Ship to internal testers.

### Editor-side (biko.exe)

| # | Feature | Complexity | Notes |
|---|---------|-----------|-------|
| 1 | AI Bridge module (`aiBridge.c/h`) | Medium | Named pipe client, overlapped I/O, JSON serialization, request queue |
| 2 | Status bar AI segment | Low | One new segment in existing status bar |
| 3 | Context extraction | Low | Current file text + cursor + selection â†’ JSON |
| 4 | Unified diff parser (`diffApply.c/h`) | Medium | Parse `@@` hunks, map to buffer positions |
| 5 | Inline diff preview (indicators + annotations) | Medium | Scintilla indicator/annotation API |
| 6 | Apply / Reject hotkeys | Low | Enter = apply, Escape = reject |
| 7 | Undo group for patches | Low | `SCI_BEGINUNDOACTION` / `SCI_ENDUNDOACTION` |
| 8 | Context menu: "AI: Transform..." | Low | One menu item + input dialog |
| 9 | INI settings for AI | Low | 5â€“6 keys in existing INI system |

### Engine-side (biko-engine.exe)

| # | Feature | Complexity | Notes |
|---|---------|-----------|-------|
| 1 | Named pipe server | Medium | Single-client for MVP |
| 2 | OpenAI API adapter | Low | HTTP POST to chat completions |
| 3 | Prompt template: code transform | Low | System prompt + user file + instruction â†’ unified diff |
| 4 | Diff extraction from model output | Medium | Parse model response, extract valid unified diff |
| 5 | Error handling + timeout | Low | 30s timeout, structured error response |

### Explicitly NOT in v0.1

- No local model support (llama.cpp)
- No multi-file patches
- No context gathering beyond current file
- No inline hints
- No streaming
- No licensing/auth system
- No repo indexing

### Success Criteria for v0.1

1. User selects code, presses `Ctrl+Shift+T`, types "add error handling"
2. Status bar shows "AI: thinking..."
3. 2â€“5 seconds later, inline diff preview appears
4. User presses Enter â†’ code is replaced
5. User presses Ctrl+Z â†’ code is restored
6. If engine is not running, menu items are grayed out, editor works normally

---

## 6. Evolution Plan

### v0.2 â€” Context & Polish

| Feature | Description |
|---|---|
| Streaming responses | Show partial diffs as they arrive; progressive preview |
| Multi-file patches | Patch confirmation dialog, undo journal |
| Context gathering | Engine scans `#include`/`import` to pull related files |
| Explain action | Selection â†’ explanation shown as a tooltip or annotation |
| Fix action | Lint/compile-error aware prompt (editor sends diagnostic text) |
| Connection resilience | Auto-reconnect, engine health check, restart button |
| Cancel in-flight | Escape during "thinking" cancels the request |

### v0.5 â€” Intelligence

| Feature | Description |
|---|---|
| Repo indexing | Engine builds a symbol index (ctags-level, not LSP) for the project directory |
| File relevance ranking | Engine decides which files to include as context, weighted by imports/proximity |
| Inline hints | Subtle, dismissable annotations triggered by idle cursor (not keystroke-based) |
| Local model support | llama.cpp integration in the engine, GGUF model loading |
| Model routing | Config-driven: "use local for hints, cloud for refactor" |
| Confidence thresholding | Hints below threshold are suppressed |
| Hunk-level apply | Tab through hunks, Space to toggle, apply only selected hunks |

### v1.0 â€” Commercial Release

| Feature | Description |
|---|---|
| Licensing module | Separate DLL: validates license key, controls feature tier |
| Tier enforcement | Free: editor only. Pro: cloud AI. Team: shared context + audit log |
| Audit trail | JSON log of all AI actions: timestamp, request hash, patch summary, applied/rejected |
| Multiple model backends | OpenAI, Anthropic, Azure OpenAI, Ollama, custom endpoint |
| Token budget controls | Per-session and per-month limits, visible in settings |
| Settings UI polish | Proper dialog with tabs, not just INI editing |
| Auto-update | Background check for new engine/editor versions (no forced updates) |
| Installer | MSI/MSIX with optional engine bundling |
| Documentation | User guide, keybinding reference, API schema reference |

### v2.0 â€” Platform (Future)

| Feature | Description |
|---|---|
| Multi-file editing | Open multiple files (tabs or split), context spans all open files |
| Project-level actions | "Rename symbol across project", "Add feature to codebase" |
| Team context sharing | Shared project index on a server, multi-user context |
| Plugin protocol | Allow third-party engines to connect via the same named pipe protocol |

---

## 7. Risks and Mitigation

| # | Risk | Severity | Likelihood | Mitigation |
|---|------|----------|-----------|------------|
| 1 | **Model returns invalid/malformed diffs** | High | High | Validate diff syntax before preview. Reject malformed diffs with retry prompt. Include explicit "respond ONLY with a unified diff" in system prompt. Show raw response in a debug pane (Ctrl+Shift+D) for diagnosis. |
| 2 | **Diff applies to wrong location** | High | Medium | Context matching: require 3+ context lines to match before applying a hunk. If context doesn't match, mark hunk as failed and show warning. Never force-apply. |
| 3 | **Named pipe security** | Medium | Low | Restrict pipe DACL to current user SID. Pipe name includes a session-unique nonce. Don't accept connections from other users. |
| 4 | **Engine crash / hang** | Medium | Medium | Editor uses overlapped I/O with timeouts. Engine crash = status bar shows "offline". One-click restart from toolbar. Watchdog thread in editor can auto-restart engine. |
| 5 | **Large file / long context** | Medium | Medium | Truncate context to configurable max (default 32KB). Send viewport-adjacent code only if file exceeds limit. Engine can request additional ranges. |
| 6 | **API cost surprises** | Medium | Medium | Show estimated token count before sending (in status bar). Configurable per-session budget. Engine logs cost per request. |
| 7 | **GPL contamination** | High | Low | No GPL dependencies. llama.cpp is MIT. Scintilla is custom permissive. Notepad2e is BSD-3-Clause. Audit all dependencies before inclusion. |
| 8 | **Scintilla annotation/indicator limits** | Low | Medium | Annotations are line-based, not range-based â€” large insertions may look awkward. Fallback: show added lines in a temporary readonly split view. |
| 9 | **User edits during AI thinking** | Medium | High | Version the buffer content at request time (store a hash). On response, re-validate context lines. If drift is minor, adjust offsets. If major, show "file changed, re-run?" prompt. |
| 10 | **Competitive pressure from VS Code / Cursor** | High | Certain | Differentiate on: startup speed (<100ms), memory footprint (<15MB), offline capability, no telemetry, no account required, transparent cost, native Win32 feel. Position as "the Notepad of AI editors" â€” not competing with IDEs. |

---

## 8. What NOT to Build

These are explicit exclusions. Do not build, plan for, or hint at these in the product:

| Exclusion | Reason |
|---|---|
| **Chat panel / conversational UI** | Violates the Notepad philosophy. AI is a tool, not a conversation partner. If the user wants chat, they can use a browser. |
| **File tree / project explorer** | This is a single-file editor that happens to understand projects. No tree views. |
| **Terminal / console pane** | Out of scope. Use cmd.exe / Windows Terminal. |
| **Git integration UI** | No status bars with branch names, no commit dialogs. The AI engine may *read* git state for context, but the editor never shows it. |
| **Markdown preview** | No. |
| **Extension / plugin marketplace** | The engine protocol is open, but there's no in-app store. |
| **AI model training or fine-tuning** | Never upload user code for training. Make this a public policy. |
| **Syntax-aware editing (LSP)** | No Language Server Protocol. Scintilla lexers provide highlighting. The AI engine provides intelligence. LSP is too heavy and IDE-adjacent. |
| **Telemetry** | No usage tracking. No analytics. No crash reporting without explicit opt-in. |
| **Auto-apply mode** | Never apply AI changes without preview. Not even as an option. Trust requires friction. |
| **Browser-based UI (Electron, WebView2)** | No. Pure Win32. No exceptions. |
| **Cross-platform at v1.0** | Windows only. Do not abstract the platform layer prematurely. If a macOS/Linux version is needed later, port the engine first (it's already platform-neutral if written in C++/Rust), then evaluate Scintilla on GTK. |
| **Collaborative editing** | This is a single-user, single-machine editor. |
| **Notebook / REPL interface** | Out of scope. |
| **Voice input** | No. |

---

## Appendix A: New Source Files (Editor Side)

These files are added to `src/Extension/` following the existing `n2e_` naming convention:

| File | Purpose | Approx. Size |
|---|---|---|
| `AIBridge.c` / `AIBridge.h` | Named pipe client, request/response serialization, connection lifecycle | ~800 LOC |
| `AIContext.c` / `AIContext.h` | Extract current file, cursor, selection, viewport into JSON context struct | ~300 LOC |
| `DiffParse.c` / `DiffParse.h` | Parse unified diff format into structured hunk list | ~400 LOC |
| `DiffApply.c` / `DiffApply.h` | Apply/preview hunks to Scintilla buffer using indicators and annotations | ~500 LOC |
| `DiffPreview.c` / `DiffPreview.h` | Manage preview state: enter/exit preview mode, hunk navigation, toggle | ~400 LOC |
| `AICommands.c` / `AICommands.h` | Command handlers for AI menu items, hotkey dispatch | ~200 LOC |
| `PatchUndo.c` / `PatchUndo.h` | Multi-file undo journal | ~200 LOC |
| `json.c` / `json.h` | Minimal JSON writer/reader (no dependency on external JSON libs) | ~500 LOC |

Total: ~3,300 LOC for the editor-side AI integration.

## Appendix B: JSON Serialization

To avoid external dependencies, the editor uses a minimal hand-written JSON serializer. It only needs to:

- **Write**: objects, arrays, strings (with escaping), integers, floats, booleans, null
- **Read**: objects and arrays (streaming-style, key-at-a-time, no full DOM)

This keeps the editor binary small and avoids any license concerns. The engine side (separate binary) can use whatever JSON library fits its language (cJSON, nlohmann, serde, etc.).

## Appendix C: Scintilla API Surface for Diff Preview

| API | Usage |
|---|---|
| `SCI_INDICSETSTYLE(indic, INDIC_STRAIGHTBOX)` | Define indicator styles for deleted-line highlighting |
| `SCI_INDICSETFORE(indic, RGB(255,200,200))` | Red tint for deletions |
| `SCI_INDICATORFILLRANGE(pos, len)` | Apply indicator to a range |
| `SCI_INDICATORCLEARRANGE(pos, len)` | Remove indicator from a range |
| `SCI_ANNOTATIONSETTEXT(line, text)` | Show added lines as annotations below the preceding line |
| `SCI_ANNOTATIONSETSTYLE(line, style)` | Style the annotation (green background) |
| `SCI_ANNOTATIONSETVISIBLE(ANNOTATION_STANDARD)` | Enable annotation display |
| `SCI_MARGINSETTEXT(line, text)` | Show `+`/`-` markers in a margin |
| `SCI_SETREADONLY(true)` | Lock buffer during preview mode |
| `SCI_BEGINUNDOACTION` / `SCI_ENDUNDOACTION` | Group all hunk applications into one undo step |
| `SCI_GETTEXT` / `SCI_SETTEXT` | Read/write full buffer for context extraction |
| `SCI_GETTEXTRANGE` | Read specific ranges |
| `SCI_DELETERANGE(pos, len)` | Delete old text during apply |
| `SCI_INSERTTEXT(pos, text)` | Insert new text during apply |

## Appendix D: Command IDs

Reserve a block of command IDs for AI features. The existing Notepad2e uses `40000â€“40730` for standard commands and `20000â€“20044` for internal commands. Reserve `41000â€“41099`:

```c
#define IDM_AI_TRANSFORM       41000
#define IDM_AI_REFACTOR        41001
#define IDM_AI_EXPLAIN         41002
#define IDM_AI_FIX             41003
#define IDM_AI_APPLY_PATCH     41010
#define IDM_AI_REJECT_PATCH    41011
#define IDM_AI_NEXT_HUNK       41012
#define IDM_AI_PREV_HUNK       41013
#define IDM_AI_TOGGLE_HUNK     41014
#define IDM_AI_CANCEL          41020
#define IDM_AI_SETTINGS        41030
#define IDM_AI_RESTART_ENGINE  41031
```

## Appendix E: Named Pipe Protocol Details

### Pipe Name

```
\\.\pipe\biko_ai_{SessionId}
```

Where `{SessionId}` is a random 64-bit hex nonce generated by the editor at startup and passed to the engine as a command-line argument:

```
biko-engine.exe --pipe biko_ai_A1B2C3D4E5F6G7H8
```

### Connection Lifecycle

1. Editor starts â†’ spawns `biko-engine.exe` (or connects to existing)
2. Engine creates the named pipe and waits for connection
3. Editor connects with `CreateFile(pipeName, GENERIC_READ|GENERIC_WRITE, ...)`
4. Messages flow bidirectionally
5. On editor exit: sends a `{"type":"shutdown"}` message, then closes the pipe handle
6. Engine detects broken pipe â†’ cleans up and exits (or waits for new connection if running as a service)

### Threading Model (Editor Side)

- **Main thread**: UI, Scintilla, command dispatch
- **AI thread**: Pipe I/O, JSON serialization/deserialization
- Communication between threads: `PostMessage(hwndMain, WM_AI_RESPONSE, ...)` with response data in a thread-safe queue

Custom window messages:

```c
#define WM_AI_RESPONSE    (WM_USER + 100)
#define WM_AI_STATUS      (WM_USER + 101)
#define WM_AI_CONNECTED   (WM_USER + 102)
#define WM_AI_DISCONNECTED (WM_USER + 103)
```

This keeps the main thread never blocking on AI operations.

---

## Appendix F: Implementation Status

### Phase 1 â€” Core AI Infrastructure (DONE)

| Module | Files | LOC (approx) | Status |
|--------|-------|-------------|--------|
| JSON serializer | `mono_json.h/c` | ~500 | âœ… Complete |
| AI Bridge | `AIBridge.h/c` | ~600 | âœ… Complete |
| AI Context | `AIContext.h/c` | ~250 | âœ… Complete |
| Diff Parser | `DiffParse.h/c` | ~400 | âœ… Complete |
| Diff Preview | `DiffPreview.h/c` | ~350 | âœ… Complete |
| Patch Undo | `PatchUndo.h/c` | ~150 | âœ… Complete |
| AI Commands | `AICommands.h/c` | ~450 | âœ… Complete |

### Phase 2 â€” Feature Modules (DONE)

| Module | Files | Status |
|--------|-------|--------|
| Chat Panel | `ChatPanel.h/c` | âœ… Complete â€” Scintilla output, edit input, resizable splitter |
| Dark Mode | `DarkMode.h/c` | âœ… Complete â€” Win10 1809+ DwmSetWindowAttribute, undocumented uxtheme |
| Git UI | `GitUI.h/c` | âœ… Complete â€” git.exe subprocess, repo detection, status/diff/commit/log |
| Terminal | `Terminal.h/c` | âœ… Complete â€” ConPTY on Win10+, pipe fallback, Scintilla terminal view |
| Markdown Preview | `MarkdownPreview.h/c` | âœ… Complete â€” Scintilla styled preview, auto-refresh |

### Phase 3 â€” AI Engine (DONE)

| Module | Files | Status |
|--------|-------|--------|
| mono-engine | `mono-engine.c` | âœ… Complete â€” WinHTTP, OpenAI-compatible API, named pipe server |

### Phase 4 â€” Integration (DONE)

- `Notepad2.c` modified: includes, MsgCreate init, WM_DESTROY shutdown, MsgSize layout, WM_AI_* messages, MsgCommand dispatch
- Build integration guide: `doc/BuildIntegration.md`
- All changes tagged with `// [mono]:` / `// [/mono]` markers

### Remaining Work

- [ ] Add new source files to `Notepad2e.vcxproj` and `.filters`
- [ ] Add `dwmapi.lib` and `uxtheme.lib` to linker dependencies
- [ ] Create separate build target for `biko-engine.exe`
- [ ] Add accelerator table entries for AI hotkeys
- [ ] Testing and compilation fixes
- [ ] INI file settings for AI preferences (model, key, etc.)
- [ ] Proper commit dialog (replace MessageBox placeholder in GitUI)
- [ ] VT100 escape sequence handling in Terminal (beyond basic stripping)
