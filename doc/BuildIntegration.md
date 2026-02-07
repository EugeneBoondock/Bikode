# Biko â€” Build Integration Guide

## New Source Files to Add to Project

The following files need to be added to `Notepad2e.vcxproj` under the existing
Extension source filter.

### Editor Plugin Sources (compile as part of biko.exe)

| Header | Source | Purpose |
|--------|--------|---------|
| `src/Extension/mono_json.h` | `src/Extension/mono_json.c` | JSON serializer/deserializer |
| `src/Extension/AIBridge.h` | `src/Extension/AIBridge.c` | Named pipe client, AI engine communication |
| `src/Extension/AIContext.h` | `src/Extension/AIContext.c` | Editor context extraction |
| `src/Extension/DiffParse.h` | `src/Extension/DiffParse.c` | Unified diff parser |
| `src/Extension/DiffPreview.h` | `src/Extension/DiffPreview.c` | Inline diff preview |
| `src/Extension/PatchUndo.h` | `src/Extension/PatchUndo.c` | Multi-file undo journal |
| `src/Extension/AICommands.h` | `src/Extension/AICommands.c` | Command handlers and menu |
| `src/Extension/ChatPanel.h` | `src/Extension/ChatPanel.c` | Chat panel UI |
| `src/Extension/DarkMode.h` | `src/Extension/DarkMode.c` | Dark mode theming |
| `src/Extension/GitUI.h` | `src/Extension/GitUI.c` | Git integration |
| `src/Extension/Terminal.h` | `src/Extension/Terminal.c` | Embedded terminal |
| `src/Extension/MarkdownPreview.h` | `src/Extension/MarkdownPreview.c` | Markdown preview |

### AI Engine (separate executable: biko-engine.exe)

| Source | Purpose |
|--------|---------|
| `src/Extension/biko-engine.c` | AI engine companion process |

The engine should be built as a separate console application target.
Linker dependencies: `winhttp.lib`, `kernel32.lib`.

---

## Modified Existing Files

### `src/Notepad2.c`

Three integration points have been added:

1. **Include block** (line ~56): Added includes for AICommands, AIBridge, ChatPanel, Terminal, MarkdownPreview headers.

2. **MsgCreate** (end of function): Added `AICommands_Init(hwnd, _hwndEdit)` call.

3. **WM_DESTROY** (cleanup): Added `AICommands_Shutdown()` call before MRU_Destroy.

4. **MsgSize** (layout): Added panel layout calls (ChatPanel_Layout, Terminal_Layout, MarkdownPreview_Layout) before BeginDeferWindowPos to reserve space.

5. **MainWndProc** (message dispatch): Added cases for `WM_AI_RESPONSE` through `WM_AI_CHUNK` messages, forwarding to `AICommands_HandleMessage`.

6. **MsgCommand** (command dispatch): Added default case at end of switch to forward unknown commands to `AICommands_HandleCommand`.

All changes are marked with `// [biko]:` / `// [/biko]` comment tags following the existing `// [2e]:` convention.

---

## vcxproj XML Snippets

### Adding source files to the main project:

```xml
<!-- Inside <ItemGroup> with ClCompile elements -->
<ClCompile Include="src\Extension\mono_json.c" />
<ClCompile Include="src\Extension\AIBridge.c" />
<ClCompile Include="src\Extension\AIContext.c" />
<ClCompile Include="src\Extension\DiffParse.c" />
<ClCompile Include="src\Extension\DiffPreview.c" />
<ClCompile Include="src\Extension\PatchUndo.c" />
<ClCompile Include="src\Extension\AICommands.c" />
<ClCompile Include="src\Extension\ChatPanel.c" />
<ClCompile Include="src\Extension\DarkMode.c" />
<ClCompile Include="src\Extension\GitUI.c" />
<ClCompile Include="src\Extension\Terminal.c" />
<ClCompile Include="src\Extension\MarkdownPreview.c" />
```

```xml
<!-- Inside <ItemGroup> with ClInclude elements -->
<ClInclude Include="src\Extension\mono_json.h" />
<ClInclude Include="src\Extension\AIBridge.h" />
<ClInclude Include="src\Extension\AIContext.h" />
<ClInclude Include="src\Extension\DiffParse.h" />
<ClInclude Include="src\Extension\DiffPreview.h" />
<ClInclude Include="src\Extension\PatchUndo.h" />
<ClInclude Include="src\Extension\AICommands.h" />
<ClInclude Include="src\Extension\ChatPanel.h" />
<ClInclude Include="src\Extension\DarkMode.h" />
<ClInclude Include="src\Extension\GitUI.h" />
<ClInclude Include="src\Extension\Terminal.h" />
<ClInclude Include="src\Extension\MarkdownPreview.h" />
```

### Additional linker dependencies (DarkMode):

```xml
<!-- Add to AdditionalDependencies -->
dwmapi.lib;uxtheme.lib
```

### Filter file additions (`Notepad2e.vcxproj.filters`):

All new files should go under `Source Files\Extension` and `Header Files\Extension` filters.

---

## Accelerator / Hotkey Table

The following accelerators should be added to the resource file or handled programmatically:

| Key Combination | Command | ID |
|---|---|---|
| Ctrl+Shift+T | Transform (AI instruction) | IDM_AI_TRANSFORM (41000) |
| Ctrl+Shift+R | Refactor | IDM_AI_REFACTOR (41001) |
| Ctrl+Shift+E | Explain | IDM_AI_EXPLAIN (41002) |
| Ctrl+Shift+X | Fix | IDM_AI_FIX (41003) |
| Ctrl+Shift+Space | Complete | IDM_AI_COMPLETE (41004) |
| Ctrl+Shift+C | Chat toggle | IDM_AI_TOGGLE_CHAT (41032) |
| Ctrl+` | Terminal toggle | IDM_TERMINAL_TOGGLE (41060) |
| Ctrl+Shift+M | Markdown preview | IDM_MARKDOWN_PREVIEW (41070) |
| Ctrl+Shift+D | Dark mode toggle | IDM_VIEW_DARKMODE (41040) |

---

## Environment Variables (for AI Engine)

| Variable | Description | Default |
|---|---|---|
| `BIKO_AI_KEY` | API key for LLM provider | (none) |
| `OPENAI_API_KEY` | Fallback API key | (none) |
| `BIKO_AI_MODEL` | Model name | gpt-4o-mini |
| `BIKO_AI_URL` | API hostname | api.openai.com |
| `BIKO_AI_PATH` | API endpoint path | /v1/chat/completions |
| `BIKO_AI_PORT` | API port | 443 |
| `BIKO_VERBOSE` | Enable verbose engine logging | 0 |
