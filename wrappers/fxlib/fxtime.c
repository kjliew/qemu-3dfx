/*
 * MMSYSTEM time functions
 *
 * Copyright 1993 Martin Ayotte
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <windows.h>
#include "hpat.h"

typedef struct tagWINE_TIMERENTRY {
    UINT                        wDelay;
    UINT                        wResol;
    LPTIMECALLBACK              lpFunc; /* can be lots of things */
    DWORD_PTR                   dwUser;
    UINT16                      wFlags;
    UINT16                      wTimerID;
    DWORD                       dwTriggerTime;
} WINE_TIMERENTRY, *LPWINE_TIMERENTRY;

static WINE_TIMERENTRY timers[16];
static UINT timers_created;
static CRITICAL_SECTION WINMM_cs, TIME_cbcrst;

static    HANDLE                TIME_hMMTimer;

#define MMSYSTIME_MININTERVAL (1)
#define MMSYSTIME_MAXINTERVAL (65535)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE ARRAYSIZE
#endif

extern DWORD (WINAPI *fxTick)(void);

static int TIME_MMSysTimeCallback(void)
{
    WINE_TIMERENTRY *timer;
    int i, delta_time;

    /* since timeSetEvent() and timeKillEvent() can be called
     * from 16 bit code, there are cases where win16 lock is
     * locked upon entering timeSetEvent(), and then the mm timer
     * critical section is locked. This function cannot call the
     * timer callback with the crit sect locked (because callback
     * may need to acquire Win16 lock, thus providing a deadlock
     * situation).
     * To cope with that, we just copy the WINE_TIMERENTRY struct
     * that need to trigger the callback, and call it without the
     * mm timer crit sect locked.
     */

    for (;;)
    {
        for (i = 0; i < ARRAY_SIZE(timers); i++)
            if (timers[i].wTimerID) break;
        if (i == ARRAY_SIZE(timers)) return -1;
        timer = timers + i;
        for (i++; i < ARRAY_SIZE(timers); i++)
        {
            if (!timers[i].wTimerID) continue;
            if (timers[i].dwTriggerTime < timer->dwTriggerTime)
                timer = timers + i;
        }

        delta_time = timer->dwTriggerTime - fxTick();
        if (delta_time > 0) break;

        if (timer->wFlags & TIME_PERIODIC)
            timer->dwTriggerTime += timer->wDelay;

        switch(timer->wFlags & (TIME_CALLBACK_EVENT_SET|TIME_CALLBACK_EVENT_PULSE))
        {
        case TIME_CALLBACK_EVENT_SET:
            SetEvent(timer->lpFunc);
            break;
        case TIME_CALLBACK_EVENT_PULSE:
            PulseEvent(timer->lpFunc);
            break;
        case TIME_CALLBACK_FUNCTION:
            {
                DWORD_PTR user = timer->dwUser;
                UINT16 id = timer->wTimerID;
                UINT16 flags = timer->wFlags;
                LPTIMECALLBACK func = timer->lpFunc;

                if (flags & TIME_KILL_SYNCHRONOUS) EnterCriticalSection(&TIME_cbcrst);
                LeaveCriticalSection(&WINMM_cs);

                func(id, 0, user, 0, 0);

                EnterCriticalSection(&WINMM_cs);
                if (flags & TIME_KILL_SYNCHRONOUS) LeaveCriticalSection(&TIME_cbcrst);
                if (id != timer->wTimerID) timer = NULL;
            }
            break;
        }
        if (timer && !(timer->wFlags & TIME_PERIODIC))
            timer->wTimerID = 0;
    }
    return delta_time;
}

static DWORD CALLBACK TIME_MMSysTimeThread(LPVOID arg)
{
    int sleep_time;

    EnterCriticalSection(&WINMM_cs);
    while (1)
    {
        sleep_time = TIME_MMSysTimeCallback();

        if (sleep_time < 0)
            break;
        if (sleep_time == 0)
            continue;

        LeaveCriticalSection(&WINMM_cs);
        Sleep(1);
        EnterCriticalSection(&WINMM_cs);
    }
    CloseHandle(TIME_hMMTimer);
    TIME_hMMTimer = NULL;
    LeaveCriticalSection(&WINMM_cs);
    //FreeLibraryAndExitThread(arg, 0);
    return 0;
}

static void TIME_MMTimeStart(void)
{
    HMODULE mod;
    if (TIME_hMMTimer) return;

    DWORD thread_id;
    BOOL (WINAPI *p_getModuleHandleExA)(DWORD, LPCSTR, HMODULE *) =
        GetProcAddress(GetModuleHandle("kernel32.dll"), "GetModuleHandleExA");
    mod = NULL;
    if (p_getModuleHandleExA)
        p_getModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)TIME_MMSysTimeThread, &mod);
    TIME_hMMTimer = CreateThread(NULL, 0, TIME_MMSysTimeThread, mod, 0, &thread_id);
    SetThreadPriority(TIME_hMMTimer, THREAD_PRIORITY_TIME_CRITICAL);
}

static MMRESULT WINAPI HookSetEvent(UINT wDelay, UINT wResol, LPTIMECALLBACK lpFunc,
                            DWORD_PTR dwUser, UINT wFlags)
{
    WORD new_id = 0;
    int i;

    if (wDelay < MMSYSTIME_MININTERVAL || wDelay > MMSYSTIME_MAXINTERVAL)
	return 0;

    if (!TIME_hMMTimer) {
        InitializeCriticalSection(&WINMM_cs);
        InitializeCriticalSection(&TIME_cbcrst);
    }
    EnterCriticalSection(&WINMM_cs);

    for (i = 0; i < ARRAY_SIZE(timers); i++)
        if (!timers[i].wTimerID) break;
    if (i == ARRAY_SIZE(timers))
    {
        LeaveCriticalSection(&WINMM_cs);
        return 0;
    }

    new_id = ARRAY_SIZE(timers)*(++timers_created) + i;
    if (!new_id) new_id = ARRAY_SIZE(timers)*(++timers_created) + i;

    timers[i].wDelay = wDelay;
    timers[i].dwTriggerTime = fxTick() + wDelay;

    /* FIXME - wResol is not respected, although it is not clear
       that we could change our precision meaningfully  */
    timers[i].wResol = wResol;
    timers[i].lpFunc = lpFunc;
    timers[i].dwUser = dwUser;
    timers[i].wFlags = wFlags;
    timers[i].wTimerID = new_id;

    TIME_MMTimeStart();

    LeaveCriticalSection(&WINMM_cs);

    return new_id;
}

static MMRESULT WINAPI HookKillEvent(UINT wID)
{
    WINE_TIMERENTRY *timer;
    WORD flags;

    EnterCriticalSection(&WINMM_cs);

    timer = &timers[wID % ARRAY_SIZE(timers)];
    if (timer->wTimerID != wID)
    {
        LeaveCriticalSection(&WINMM_cs);
        return TIMERR_NOCANDO;
    }

    timer->wTimerID = 0;
    flags = timer->wFlags;
    int i = 0;
    for (; i < ARRAY_SIZE(timers); i++)
        if (timers[i].wTimerID) break;
    LeaveCriticalSection(&WINMM_cs);

    if (flags & TIME_KILL_SYNCHRONOUS)
    {
        EnterCriticalSection(&TIME_cbcrst);
        LeaveCriticalSection(&TIME_cbcrst);
    }

    if (i == ARRAY_SIZE(timers)) {
        while(TIME_hMMTimer) Sleep(1);
        DeleteCriticalSection(&TIME_cbcrst);
        DeleteCriticalSection(&WINMM_cs);
    }

    return TIMERR_NOERROR;
}

void fxEventHookPtr(const PEVENTFX e)
{
    e->Kill = &HookKillEvent;
    e->Set = &HookSetEvent;
}
