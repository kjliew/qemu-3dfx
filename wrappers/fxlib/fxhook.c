#include <windows.h>
#include <stdint.h>

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

static void HookTimeGetFreq(LARGE_INTEGER *f)
{
    static LARGE_INTEGER freq;
    if (!f)
        QueryPerformanceFrequency(&freq);
    else
        *f = freq;
}

static BOOL WINAPI PerformanceCounterProc(LARGE_INTEGER *count)
{
#define TICK_8254 0x1234F0U /* 1.193200 MHz */
#define TICK_ACPI 0x369E99U /* 3.579545 MHz */
    static LARGE_INTEGER tick;
    LARGE_INTEGER f;
    HookTimeGetFreq(&f);
    DWORD t = acpi_tick_asm();
    tick.QuadPart = (tick.QuadPart == 0)? t:tick.QuadPart;
    if ((tick.u.LowPart & 0x00FFFFFFU) > t)
        tick.QuadPart = (((tick.QuadPart >> 24) + 1) << 24) | t;
    else
        tick.u.LowPart = (tick.u.LowPart & 0xFF000000U) | t;
    count->QuadPart = (tick.QuadPart * f.QuadPart) / TICK_ACPI;
    return TRUE;
}

static DWORD WINAPI TimeHookProc(void)
{
    LARGE_INTEGER f, li;
    HookTimeGetFreq(&f);
    if (f.QuadPart < TICK_8254) {
        PerformanceCounterProc(&li);
        return (li.QuadPart * 1000) / f.QuadPart;
    }
    QueryPerformanceCounter(&li);
    return (li.QuadPart * 1000) / f.QuadPart;
}

void HookParseRange(uint32_t *start, uint32_t **iat, const DWORD range)
{
    const char idata[] = ".idata", rdata[] = ".rdata";
    uint32_t addr = *start;

    if (addr && (0x4550U == *(uint32_t *)addr)) {
        for (int i = 0; i < range; i += 0x04) {
            if (!memcmp((void *)(addr + i), idata, sizeof(idata))) {
                addr += ((uint32_t *)(addr + i))[3];
                *iat = (uint32_t *)addr;
                *start = addr;
                break;
            }
        }
        for (int i = 0; i < range; i += 0x04) {
            if (addr == (uint32_t)(*iat))
                break;
            if (!memcmp((void *)(addr + i), rdata, sizeof(rdata))) {
                addr += ((uint32_t *)(addr + i))[3];
                *iat = (uint32_t *)addr;
                *start = addr;
                break;
            }
        }
    }
}

static void HookPatchTimer(const uint32_t start, const uint32_t *iat, const DWORD range)
{
    LARGE_INTEGER f;
    DWORD oldProt, hkGet;
    HookTimeGetFreq(&f);
    hkGet = (DWORD)GetProcAddress(GetModuleHandle("winmm.dll"), "timeGetTime");
#undef TICK_8254
#undef TICK_ACPI
    uint32_t addr = start, *patch = (uint32_t *)iat;

    if ((addr == (uint32_t)patch) &&
        VirtualProtect(patch, sizeof(intptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
        for (int i = 0; i < (range >> 2); i++) {
            if (hkGet && (hkGet == patch[i])) {
                HookEntryHook(&patch[i], patch[i]);
                patch[i] = (uint32_t)&TimeHookProc;
                break;
            }
        }
        VirtualProtect(patch, sizeof(intptr_t), oldProt, &oldProt);
    }
}

void HookTimeGetTime(const uint32_t caddr)
{
    uint32_t addr, *patch;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    HookTimeGetFreq(0);

    if (caddr) {
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
    HookParseRange(&addr, &patch, si.dwPageSize); \
    HookPatchTimer(addr, patch, si.dwPageSize - (((uint32_t)patch) & (si.dwPageSize - 1)));
    TICK_HOOK(0);
#undef TICK_HOOK
}

