#pragma once
/******************************************************************************
*
* Biko
*
* AISubscriptionAgent.h
*   Chat-panel backend that delegates control to a locally installed
*   subscription-based coding agent CLI such as Codex or Claude Code.
*
******************************************************************************/

#include <wtypes.h>
#include "AIBridge.h"
#include "AIProvider.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL AISubscriptionAgent_ChatAsync(const AIConfig* pConfig,
                                   const char* szUserMessage,
                                   const AIChatAttachment* pAttachments,
                                   int cAttachments,
                                   HWND hwndTarget);

BOOL AISubscriptionAgent_IsBusy(void);
void AISubscriptionAgent_ResetSessions(void);
BOOL AISubscriptionAgent_IsAuthenticated(EAIChatAccessMode eMode);
BOOL AISubscriptionAgent_Logout(EAIChatAccessMode eMode);
BOOL AISubscriptionAgent_OpenLoginFlow(EAIChatAccessMode eMode, HWND hwndOwner);
void AISubscriptionAgent_GetModeDisplayName(EAIChatAccessMode eMode,
                                            WCHAR* wszOut, int cchOut);

#ifdef __cplusplus
}
#endif
