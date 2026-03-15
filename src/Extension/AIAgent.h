#pragma once
/******************************************************************************
*
* Biko
*
* AIAgent.h
*   Agentic AI loop with tool execution capabilities.
*   The agent can read/write files, execute commands, and list directories.
*   It runs a multi-turn conversation loop on a background thread.
*
******************************************************************************/

#include <wtypes.h>
#include "AIProvider.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Custom messages posted to the chat panel during agent execution
//=============================================================================

// Status update (lParam = heap-allocated char* status text, receiver must free)
#define WM_AI_AGENT_STATUS  (WM_USER + 0x701)

// Tool call display (lParam = heap-allocated char* tool info, receiver must free)
#define WM_AI_AGENT_TOOL    (WM_USER + 0x702)

// Open a file in the editor (lParam = heap-allocated WCHAR* file path, receiver must free)
#define WM_AI_OPEN_FILE     (WM_USER + 0x703)

// Insert text into the editor (lParam = heap-allocated char* UTF-8 text, receiver must free)
#define WM_AI_INSERT_TEXT   (WM_USER + 0x704)

// Replace the active editor buffer with UTF-8 text (lParam = heap-allocated char* text, receiver must free)
#define WM_AI_REPLACE_EDITOR (WM_USER + 0x705)

// Create a new untitled editor buffer and fill it with UTF-8 text (lParam = heap-allocated char* text, receiver must free)
#define WM_AI_NEW_FILE_TEXT  (WM_USER + 0x706)
// Refresh the explorer around a file or folder path (lParam = heap-allocated WCHAR* path, receiver must free)
#define WM_AI_REFRESH_PATH   (WM_USER + 0x707)

//=============================================================================
// Public API
//=============================================================================

// Initialize the agent subsystem. Call once at startup.
void AIAgent_Init(void);

// Shut down the agent. Frees conversation history.
void AIAgent_Shutdown(void);

// Shut down the agent. Frees conversation history.
void AIAgent_Shutdown(void);

// Start an async agent chat. Runs the full agent loop on a background thread.

// Start an async agent chat. Runs the full agent loop on a background thread.
// Posts WM_AI_AGENT_STATUS / WM_AI_AGENT_TOOL during execution.
// Posts WM_AI_DIRECT_RESPONSE with the final answer when done.
// Returns FALSE if a request is already in progress.
BOOL AIAgent_ChatAsync(const AIProviderConfig* pCfg,
                       const char* szUserMessage,
                       const AIChatAttachment* pAttachments,
                       int cAttachments,
                       HWND hwndTarget,
                       HWND hwndMainWnd);

// Clear conversation history (start fresh context).
void AIAgent_ClearHistory(void);

//=============================================================================
// Synchronous agentic tool loop (for use by AgentRuntime / Mission Control)
//=============================================================================

// Result structure returned by AIAgent_RunToolLoop.
typedef struct {
    char*   pszResult;          // Final response text (heap-allocated, caller frees)
    int     inputTokens;        // Cumulative input tokens across all iterations
    int     outputTokens;       // Cumulative output tokens across all iterations
    int     toolCalls;          // Total tool calls executed
    int     filesChanged;       // Number of file-mutation tool calls
} AIAgentLoopResult;

// Callback for runtime event reporting during the tool loop.
// nodeIndex: the caller's node index (opaque, passed through)
// eventType: 0=status, 1=tool
// message: event description (do NOT free; it's stack-allocated)
typedef void (*AIAgentLoopCallback)(int nodeIndex, int eventType, const char* message);

// Run a synchronous multi-turn agentic loop on the calling thread.
// The model is called repeatedly until it produces a final answer (no tool
// calls) or the iteration/tool budget is exhausted.
//
// pCfg:        Provider configuration (model, API key, etc.)
// systemPrompt: System prompt including tool documentation
// userPrompt:  The task/instruction for the model
// maxIter:     Maximum LLM round-trips (0 = default 12)
// maxTools:    Maximum total tool calls (0 = default 30)
// callback:    Optional callback for status/tool events (may be NULL)
// cbNodeIndex: Opaque node index passed to callback
// pResult:     Output structure (caller must free pszResult)
void AIAgent_RunToolLoop(const AIProviderConfig* pCfg,
                         const char* systemPrompt,
                         const char* userPrompt,
                         int maxIter,
                         int maxTools,
                         AIAgentLoopCallback callback,
                         int cbNodeIndex,
                         AIAgentLoopResult* pResult);

#ifdef __cplusplus
}
#endif
