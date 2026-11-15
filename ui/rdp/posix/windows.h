/*
 * A stand-in for <windows.h> on hosts that are not Windows.
 *
 * The RDP protocol files in ui/rdp come from ntrdps, an RDP server for
 * Windows NT, and stay as ntrdps has them.  They use a few Windows types
 * and calls.  A Windows host build gives them the real <windows.h>; other
 * hosts find this file first (ui/meson.build adds this folder to their
 * include path only there).
 *
 * Copyright (c) 2026 LEMU authors.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef LEMU_RDP_POSIX_WINDOWS_H
#define LEMU_RDP_POSIX_WINDOWS_H

#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define __cdecl
#define CP_ACP 0

typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef unsigned int UINT;
typedef uint16_t WCHAR;
typedef const char *LPCSTR;

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT;

typedef pthread_mutex_t CRITICAL_SECTION;

static inline void InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutex_init(cs, NULL);
}

static inline void DeleteCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutex_destroy(cs);
}

static inline void EnterCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutex_lock(cs);
}

static inline void LeaveCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutex_unlock(cs);
}

/* Milliseconds since some fixed time.  Wraps after 49.7 days, as on NT. */
static inline DWORD GetTickCount(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * Only what ntrdps asks for: the client's 8-bit text (CP_ACP).  The
 * bytes are read as ISO 8859-1, so each byte becomes the same code.
 */
static inline int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR s,
                                      int n, WCHAR *out, int cap)
{
    int i;

    for (i = 0; i < n && i < cap; i++) {
        out[i] = (unsigned char)s[i];
    }
    return i;
}

#endif
