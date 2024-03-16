#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "hpat.h"

void HookEntryHook(uint32_t *patch, const uint32_t orig)
{
#define MAX_HOOK_ENTRY 8
    static int nhook;
    static struct hookrec {
        uint32_t *patch;
        uint32_t data;
    } HookTbl[MAX_HOOK_ENTRY];

    if (patch && orig && (nhook < MAX_HOOK_ENTRY)) {
        HookTbl[nhook].patch = patch;
        HookTbl[nhook].data = orig;
        nhook++;
    }

    if (!patch && !orig) {
        DWORD oldProt;
        if (nhook) {
            for (int i = 0; i < nhook; i++) {
                VirtualProtect(HookTbl[i].patch, sizeof(intptr_t), PAGE_EXECUTE_READWRITE, &oldProt);
                if (!IsBadWritePtr(HookTbl[i].patch, sizeof(intptr_t)))
                    (HookTbl[i].patch)[0] = HookTbl[i].data;
                VirtualProtect(HookTbl[i].patch, sizeof(intptr_t), oldProt, &oldProt);
            }
        }
        memset(HookTbl, 0, sizeof(struct hookrec[MAX_HOOK_ENTRY]));
        nhook = 0;
    }
#undef MAX_HOOK_ENTRY
}

static inline int acpi_tick_asm(void)
{
    int ret;
    int pmio = 0x608;
    asm volatile(
        "movl %1, %%edx\n"
        "in %%dx, %%eax\n"
        "movl %%eax, %0\n"
         :"=m"(ret)
         :"r"(pmio)
         :"edx"
    );
    return ret;
}

struct tckRef {
    LARGE_INTEGER freq, run;
};
static void HookTimeTckRef(struct tckRef **tick)
{
#define TICK_8254 0x1234F0U /* 1.193200 MHz */
#define TICK_ACPI 0x369E99U /* 3.579545 MHz */
    static struct tckRef ref;

    if (!tick) {
        if (!ref.freq.u.LowPart) {
            QueryPerformanceFrequency(&ref.freq);
            if ((VER_PLATFORM_WIN32_WINDOWS == fxCompatPlatformId(0)) && (ref.freq.QuadPart < TICK_8254)) {
                LONGLONG mmTick = GetTickCount();
                __atomic_store_n(&ref.run.QuadPart, ((mmTick * TICK_ACPI) / 1000), __ATOMIC_RELAXED);
                while (acpi_tick_asm() < (ref.run.u.LowPart & 0x00FFFFFFU))
                    asm volatile("pause\n");
            }
        }
    }
    else
        *tick = &ref;
}

static BOOL WINAPI elapsedTickProc(LARGE_INTEGER *count)
{
    struct tckRef *tick;
    uintptr_t aligned = (uintptr_t)count;
    HookTimeTckRef(&tick);

    if ((VER_PLATFORM_WIN32_WINDOWS == fxCompatPlatformId(0)) && (tick->freq.QuadPart < TICK_8254)) {
        DWORD t = acpi_tick_asm();
        if ((tick->run.u.LowPart & 0x00FFFFFFU) > t)
            __atomic_store_n(&tick->run.QuadPart, ((((tick->run.QuadPart >> 24) + 1) << 24) | t), __ATOMIC_RELAXED);
        else
            __atomic_store_n(&tick->run.u.LowPart, ((tick->run.u.LowPart & 0xFF000000U) | t), __ATOMIC_RELAXED);

        if (count && !(aligned & (sizeof(uintptr_t)-1)) && !IsBadWritePtr(count, sizeof(LARGE_INTEGER))) {
            __atomic_store_n(&count->QuadPart, ((tick->run.QuadPart * tick->freq.QuadPart) / TICK_ACPI), __ATOMIC_RELAXED);
            SetLastError(0);
            return TRUE;
        }
    }
    else if (count && !(aligned & (sizeof(uintptr_t)-1)) && !IsBadWritePtr(count, sizeof(LARGE_INTEGER)))
        return QueryPerformanceCounter(count);
    SetLastError(ERROR_NOACCESS);
    return FALSE;
}

static DWORD WINAPI TimeHookProc(void)
{
    struct tckRef *tick;
    LARGE_INTEGER li;
    HookTimeTckRef(&tick);
    if (tick->freq.QuadPart < TICK_8254)
        elapsedTickProc(&li);
    else
        QueryPerformanceCounter(&li);
    return (li.QuadPart * 1000) / tick->freq.QuadPart;
}
#undef TICK_8254
#undef TICK_ACPI

DWORD (WINAPI *fxTick)(void) = (DWORD (WINAPI *)(void))&TimeHookProc;

void HookParseRange(uint32_t *start, uint32_t **iat, uint32_t *eoffs)
{
    const char idata[] = ".idata", rdata[] = ".rdata", dtext[] = ".text";
    uint32_t addr = *start, range = *eoffs;

    if (addr && (0x4550U == *(uint32_t *)addr)) {
        for (int i = 0; i < range; i += 0x04) {
            if (!memcmp((void *)(addr + i), idata, sizeof(idata))) {
                *eoffs = (((uint32_t *)(addr + i))[2])?
                    ((uint32_t *)(addr + i))[2]:((uint32_t *)(addr + i))[4];
                addr = (addr & ~(range - 1)) + ((uint32_t *)(addr + i))[3];
                *iat = (uint32_t *)addr;
                *start = addr;
                break;
            }
        }
        for (int i = 0; (addr != (uint32_t)(*iat)) && (i < range); i += 0x04) {
            if (!memcmp((void *)(addr + i), rdata, sizeof(rdata))) {
                addr = (addr & ~(range - 1)) + ((uint32_t *)(addr + i))[3];
                *iat = (uint32_t *)addr;
                *start = addr;
                break;
            }
        }
        for (int i = 0; (addr != (uint32_t)(*iat)) && (i < range); i += 0x04) {
            if(!memcmp((void *)(addr + i), dtext, sizeof(dtext))) {
                addr = (addr & ~(range - 1)) + ((uint32_t *)(addr + i))[3];
                *iat = (uint32_t *)addr;
                *start = addr;
                break;
            }
        }
    }
}

#ifdef __REV__
#define OHST_DMESG(fmt, ...)
#else
#define PT_CALL __stdcall
#define ALIGNED(x) ((x%8)?(((x>>3)+1)<<3):x)
#define GL_DEBUG_SOURCE_OTHER_ARB         0x824B
#define GL_DEBUG_TYPE_OTHER_ARB           0x8251
#define GL_DEBUG_SEVERITY_LOW_ARB         0x9148
#define OHST_DMESG(fmt, ...) \
    do { void PT_CALL glDebugMessageInsertARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5); \
        FILE *f = fopen("NUL", "w"); int c = fprintf(f, fmt, ##__VA_ARGS__); fclose(f); \
        char *str = HeapAlloc(GetProcessHeap(), 0, ALIGNED((c+1))); \
        sprintf(str, fmt, ##__VA_ARGS__); \
        glDebugMessageInsertARB(GL_DEBUG_SOURCE_OTHER_ARB, GL_DEBUG_TYPE_OTHER_ARB, GL_DEBUG_SEVERITY_LOW_ARB, -1, (c+1), (uint32_t)str); \
        HeapFree(GetProcessHeap(), 0, str); \
    } while(0)
#endif
#define FFOP_KERNELTICK 0x0001
#define FFOP_TIMEEVENT  0x0002
static void HookPatchTimer(const uint32_t start, const uint32_t *iat,
        const DWORD range, const DWORD dwFFop)
{
    DWORD oldProt;
    uint32_t addr = start, *patch = (uint32_t *)iat;
    const char funcTime[] = "timeGetTime",
          funcEventKill[] = "timeKillEvent",
          funcEventSet[] = "timeSetEvent",
          funcTick[] = "GetTickCount",
          funcPerf[] = "QueryPerformanceCounter";

    if (addr && (addr == (uint32_t)patch) &&
        VirtualProtect(patch, sizeof(intptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
        DWORD hkTime = (DWORD)GetProcAddress(GetModuleHandle("winmm.dll"), funcTime),
              hkEventKill = (dwFFop & FFOP_TIMEEVENT)?
                  (DWORD)GetProcAddress(GetModuleHandle("winmm.dll"), funcEventKill):0,
              hkEventSet = (dwFFop & FFOP_TIMEEVENT)?
                  (DWORD)GetProcAddress(GetModuleHandle("winmm.dll"), funcEventSet):0,
              hkTick = (dwFFop & FFOP_KERNELTICK)?
                  (DWORD)GetProcAddress(GetModuleHandle("kernel32.dll"), funcTick):0,
              hkPerf = (VER_PLATFORM_WIN32_WINDOWS == fxCompatPlatformId(0))?
                  (DWORD)GetProcAddress(GetModuleHandle("kernel32.dll"), funcPerf):0;
        EVENTFX timeEvent;
        fxEventHookPtr(&timeEvent);
        for (int i = 0; i < (range >> 2); i++) {
#define HOOKPROC(haddr, proc, name) \
            if (haddr && (haddr == patch[i])) { \
                HookEntryHook(&patch[i], patch[i]); \
                patch[i] = (uint32_t)proc; \
                haddr = 0; \
                OHST_DMESG("..hooked %s", name); }
            HOOKPROC(hkTime, &TimeHookProc, funcTime);
            HOOKPROC(hkEventKill, timeEvent.Kill, funcEventKill);
            HOOKPROC(hkEventSet, timeEvent.Set, funcEventSet);
            HOOKPROC(hkTick, &TimeHookProc, funcTick);
            HOOKPROC(hkPerf, &elapsedTickProc, funcPerf);
        }
        VirtualProtect(patch, sizeof(intptr_t), oldProt, &oldProt);
    }
}

static void dolog_compat_patched(void)
{
    PCOMPATFX nptr = fxCompatTblPtr();
    for (int i = 0; nptr && nptr[i].modName; i++) {
        if (nptr[i].op_mask & HP_DONE)
            OHST_DMESG("..%s patched", nptr[i].modName);
    }
}

void HookTimeGetTime(const uint32_t caddr)
{
    uint32_t addr, *patch, range;
    SYSTEM_INFO si;
    char buffer[MAX_PATH + 1], dotstr[] = ".hook";
    unsigned int len = GetModuleFileName(0, buffer, sizeof(buffer));
    struct {
        int modNum;
        char *modName[9];
    } modList = {
        .modNum = 0,
        .modName = {
            &buffer[(0 * (MAX_PATH / 8))],
            &buffer[(1 * (MAX_PATH / 8))],
            &buffer[(2 * (MAX_PATH / 8))],
            &buffer[(3 * (MAX_PATH / 8))],
            &buffer[(4 * (MAX_PATH / 8))],
            &buffer[(5 * (MAX_PATH / 8))],
            &buffer[(6 * (MAX_PATH / 8))],
            &buffer[(7 * (MAX_PATH / 8))],
            NULL,
        }
    };

    GetSystemInfo(&si);
    HookTimeTckRef(0);
    DWORD dwFFop = 0;

    if (len && len < (MAX_PATH - sizeof(dotstr))) {
        strncat(buffer, dotstr, MAX_PATH);
        FILE *fp = fopen(buffer, "r");
        if (fp) {
            char line[32];
            while(fgets(line, sizeof(line), fp)) {
                addr = strtoul(line, 0, 16);
                if (addr > 0x1000) {
                    addr &= ~(si.dwPageSize - 1);
                    patch = (uint32_t *)addr;
                    HookPatchTimer(addr, patch, si.dwPageSize, dwFFop);
                    dwFFop = 0;
                }
                else {
                    if (!memcmp(line, "0xFF,KernelTick", strlen("0xFF,KernelTick")))
                        dwFFop |= FFOP_KERNELTICK;
                    if (!memcmp(line, "0xFF,TimeEvent", strlen("0xFF,TimeEvent")))
                        dwFFop |= FFOP_TIMEEVENT;
                    if (!memcmp(line, "0x0,", strlen("0x0,")) && modList.modName[modList.modNum]) {
                        line[strcspn(line, "\r\n")] = 0;
                        strncpy(modList.modName[modList.modNum], line + strlen("0x0,"), (MAX_PATH / 8));
                        modList.modNum++;
                    }
                }
            }
            fclose(fp);
            if (!modList.modNum && !dwFFop) {
                dolog_compat_patched();
                return;
            }
        }
        modList.modName[modList.modNum] = NULL;
    }

    if (caddr && !IsBadReadPtr((void *)(caddr - 0x06), 0x06)) {
        uint16_t *callOp = (uint16_t *)(caddr - 0x06);
        uint8_t *callOp2 = (uint8_t *)(caddr - 0x05);
        addr = (0x15ff == (*callOp))? *(uint32_t *)(caddr - 0x04):0;
        if (0xe8 == (*callOp2)) {
            uint32_t rel = *(uint32_t *)(caddr - 0x04);
            uint16_t *jmpOp = (uint16_t *)(caddr + rel);
            if (0x25ff == (*jmpOp))
                addr = *(uint32_t *)(caddr + rel + 0x02);
        }
        if (addr > 0x1000) {
            addr &= ~(si.dwPageSize - 1);
            patch = (uint32_t *)addr;
            HookPatchTimer(addr, patch, si.dwPageSize, dwFFop);
        }
    }
#define TICK_HOOK(mod) \
    addr = (uint32_t)GetModuleHandle(mod); \
    addr = (addr)? addr:((uint32_t)LoadLibrary(mod)); \
    for (int i = 0; addr && (i < si.dwPageSize); i+=0x04) { \
        if (0x4550U == *(uint32_t *)addr) break; \
        addr += 0x04; \
    } \
    addr = (addr && (0x4550U == *(uint32_t *)addr))? addr:0; \
    patch = (uint32_t *)(addr & ~(si.dwPageSize - 1)); \
    range = si.dwPageSize; \
    HookParseRange(&addr, &patch, &range); \
    HookPatchTimer(addr, patch, range - (((uint32_t)patch) & (si.dwPageSize - 1)), dwFFop);
    for (int i = 0; i <= modList.modNum; i++) {
        TICK_HOOK(modList.modName[i]);
    }
#undef TICK_HOOK
    dolog_compat_patched();
}

