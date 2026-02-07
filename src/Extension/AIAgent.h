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

//=============================================================================
// Public API
//=============================================================================

// Initialize the agent subsystem. Call once at startup.
void AIAgent_Init(void);

// Shut down the agent. Frees conversation history.
void AIAgent_Shutdown(void);

// Start an async agent chat. Runs the full agent loop on a background thread.
// Posts WM_AI_AGENT_STATUS / WM_AI_AGENT_TOOL during execution.
// Posts WM_AI_DIRECT_RESPONSE with the final answer when done.
// Returns FALSE if a request is already in progress.
BOOL AIAgent_ChatAsync(const AIProviderConfig* pCfg,
                       const char* szUserMessage,
                       HWND hwndTarget);

// Clear conversation history (start fresh context).
void AIAgent_ClearHistory(void);

#ifdef __cplusplus
}
#endif
