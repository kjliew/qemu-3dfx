#include <windows.h>
#include <stdint.h>
#include <stdio.h>

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
        DWORD (WINAPI *mmTime)(void) = (DWORD (WINAPI *)(void))
            GetProcAddress(GetModuleHandle("winmm.dll"), "timeGetTime");
        LONGLONG mmTick = (mmTime)? mmTime():0;
        QueryPerformanceFrequency(&ref.freq);
        __atomic_store_n(&ref.run.QuadPart, (((((mmTick * TICK_ACPI) / 1000) >> 24) + 1) << 24), __ATOMIC_RELAXED);
    }
    else
        *tick = &ref;
}

static BOOL WINAPI elapsedTickProc(LARGE_INTEGER *count)
{
    struct tckRef *tick;
    uintptr_t aligned = (uintptr_t)count;
    HookTimeTckRef(&tick);

    if (tick->freq.QuadPart < TICK_8254) {
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

void HookParseRange(uint32_t *start, uint32_t **iat, const DWORD range)
{
    const char idata[] = ".idata", rdata[] = ".rdata";
    uint32_t addr = *start;

    if (addr && (0x4550U == *(uint32_t *)addr)) {
        for (int i = 0; i < range; i += 0x04) {
            if (!memcmp((void *)(addr + i), idata, sizeof(idata))) {
                addr = (addr & ~(range - 1)) + ((uint32_t *)(addr + i))[3];
                *iat = (uint32_t *)addr;
                *start = addr;
                break;
            }
        }
        for (int i = 0; i < range; i += 0x04) {
            if (addr == (uint32_t)(*iat))
                break;
            if (!memcmp((void *)(addr + i), rdata, sizeof(rdata))) {
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
static void HookPatchTimer(const uint32_t start, const uint32_t *iat, const DWORD range)
{
    DWORD oldProt;
    uint32_t addr = start, *patch = (uint32_t *)iat;
    const char funcTime[] = "timeGetTime", funcPerf[] = "QueryPerformanceCounter";

    if (addr && (addr == (uint32_t)patch) &&
        VirtualProtect(patch, sizeof(intptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
        DWORD hkTime = (DWORD)GetProcAddress(GetModuleHandle("winmm.dll"), funcTime),
              hkPerf = (DWORD)GetProcAddress(GetModuleHandle("kernel32.dll"), funcPerf);
        for (int i = 0; i < (range >> 2); i++) {
            if (hkTime && (hkTime == patch[i])) {
                HookEntryHook(&patch[i], patch[i]);
                patch[i] = (uint32_t)&TimeHookProc;
                hkTime = 0;
                OHST_DMESG("..hooked %s", funcTime);
            }
            if (hkPerf && (hkPerf == patch[i])) {
                HookEntryHook(&patch[i], patch[i]);
                patch[i] = (uint32_t)&elapsedTickProc;
                hkPerf = 0;
                OHST_DMESG("..hooked %s", funcPerf);
            }
            if (!hkTime && !hkPerf)
                break;
        }
        VirtualProtect(patch, sizeof(intptr_t), oldProt, &oldProt);
    }
}

void HookTimeGetTime(const uint32_t caddr)
{
    uint32_t addr, *patch;
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

    if (len && len < (MAX_PATH - sizeof(dotstr))) {
        strncat(buffer, dotstr, MAX_PATH);
        FILE *fp = fopen(buffer, "r");
        if (fp) {
            char line[32];
            while(fgets(line, sizeof(line), fp)) {
                addr = strtoul(line, 0, 16);
                if (addr) {
                    addr &= ~(si.dwPageSize - 1);
                    patch = (uint32_t *)addr;
                    HookPatchTimer(addr, patch, si.dwPageSize);
                }
                else {
                    if (!memcmp(line, "0x0,", strlen("0x0,")) && modList.modName[modList.modNum]) {
                        line[strcspn(line, "\r\n")] = 0;
                        strncpy(modList.modName[modList.modNum], line + strlen("0x0,"), (MAX_PATH / 8));
                        modList.modNum++;
                    }
                }
            }
            fclose(fp);
            if (modList.modNum == 0)
                return;
        }
        modList.modName[modList.modNum] = NULL;
    }

    if (caddr && !IsBadReadPtr((void *)(caddr - 0x06), 0x06)) {
        uint16_t *callOp = (uint16_t *)(caddr - 0x06);
        if (0x15ff == (*callOp)) {
            addr = *(uint32_t *)(caddr - 0x04);
            addr &= ~(si.dwPageSize - 1);
            patch = (uint32_t *)addr;
            HookPatchTimer(addr, patch, si.dwPageSize);
        }
    }
#define TICK_HOOK(mod) \
    addr = (uint32_t)GetModuleHandle(mod); \
    for (int i = 0; addr && (i < si.dwPageSize); i+=0x04) { \
        if (0x4550U == *(uint32_t *)addr) break; \
        addr += 0x04; \
    } \
    addr = (addr && (0x4550U == *(uint32_t *)addr))? addr:0; \
    patch = (uint32_t *)(addr & ~(si.dwPageSize - 1)); \
    HookParseRange(&addr, &patch, si.dwPageSize); \
    HookPatchTimer(addr, patch, si.dwPageSize - (((uint32_t)patch) & (si.dwPageSize - 1)));
    for (int i = 0; i <= modList.modNum; i++) {
        TICK_HOOK(modList.modName[i]);
    }
#undef TICK_HOOK
}

