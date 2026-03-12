#pragma once
/******************************************************************************
*
* Biko
*
* CodeEmbeddingIndex.h
*   Local code-search index built from hashed lexical embeddings.
*   Persists JSON artifacts under .bikode/index to keep repo context local.
*
******************************************************************************/

#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

BOOL CodeEmbeddingIndex_QueryProject(const WCHAR* wszProjectRoot,
                                     const char* query,
                                     const char* pathHint,
                                     int maxResults,
                                     char** ppszResult);

void CodeEmbeddingIndex_InvalidateProject(const WCHAR* wszProjectRoot);

#ifdef __cplusplus
}
#endif
