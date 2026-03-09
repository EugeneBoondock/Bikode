#pragma once
#include <wtypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef COUNTOF
#define COUNTOF(ar) (sizeof(ar) / sizeof((ar)[0]))
#endif

LPVOID n2e_Alloc(size_t size);
void n2e_Free(LPVOID ptr);
LPVOID n2e_Realloc(LPVOID ptr, size_t size);

#ifdef __cplusplus
}
#endif
