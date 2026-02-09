#pragma once
/******************************************************************************
*
* Biko
*
* AIDirectCall.h
*   In-process WinHTTP calls to LLM APIs.
*   Bypasses the pipe-based biko-engine architecture by making HTTP requests
*   directly from within the editor process on a background thread.
*
******************************************************************************/

#include <wtypes.h>
#include "AIProvider.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Custom message posted back to the chat panel when the AI response arrives
//=============================================================================
#define WM_AI_DIRECT_RESPONSE  (WM_USER + 0x700)

//=============================================================================
// Synchronous call (use from worker thread only)
//=============================================================================

// Makes a synchronous HTTP request to the configured LLM provider.
// Returns a heap-allocated UTF-8 string (caller must free with free()).
// Returns an "Error: ..." string on failure.
char* AIDirectCall_Chat(const AIProviderConfig* pCfg,
                        const char* szSystemPrompt,
                        const char* szUserMessage);

//=============================================================================
// Async call (fire-and-forget from UI thread)
//=============================================================================

// Launches a background thread that calls AIDirectCall_Chat and posts
// WM_AI_DIRECT_RESPONSE to hwndTarget with lParam = heap-allocated result string.
// The receiver MUST free the lParam string with free().
BOOL AIDirectCall_ChatAsync(const AIProviderConfig* pCfg,
                            const char* szSystemPrompt,
                            const char* szUserMessage,
                            HWND hwndTarget);

//=============================================================================
// Multi-message conversation support
//=============================================================================

typedef struct {
    const char* role;       // "system", "user", "assistant"
    const char* content;    // message text (UTF-8)
    AIChatAttachment* attachments; // Optional attachments
    int attachmentCount;
} AIChatMessage;

// Synchronous multi-message call. Returns heap-allocated response (caller frees).
char* AIDirectCall_ChatMulti(const AIProviderConfig* pCfg,
                             const AIChatMessage* messages,
                             int messageCount);

#ifdef __cplusplus
}
#endif
