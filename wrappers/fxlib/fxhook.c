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
        QueryPerformanceFrequency(&ref.freq);
        __sync_bool_compare_and_swap(&ref.run.QuadPart, ref.run.QuadPart, (mmTime)? mmTime():0);
        __sync_bool_compare_and_swap(&ref.run.QuadPart, ref.run.QuadPart,
            ((((ref.run.QuadPart * TICK_ACPI) / 1000) >> 24) + 1) << 24);
    }
    else
        *tick = &ref;
}

static DWORD WINAPI elapsedTickProc(LARGE_INTEGER *count)
{
    struct tckRef *tick;
    DWORD t = acpi_tick_asm();
    HookTimeTckRef(&tick);

    if ((tick->run.u.LowPart & 0x00FFFFFFU) > t)
        __sync_bool_compare_and_swap(&tick->run.QuadPart, tick->run.QuadPart, (((tick->run.QuadPart >> 24) + 1) << 24) | t);
    else
        __sync_bool_compare_and_swap(&tick->run.u.LowPart, tick->run.u.LowPart, (tick->run.u.LowPart & 0xFF000000U) | t);

    if (count)
        __sync_bool_compare_and_swap(&count->QuadPart, count->QuadPart, (tick->run.QuadPart * tick->freq.QuadPart) / TICK_ACPI);
    return TRUE;
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
    struct tckRef *tick;
    DWORD oldProt;
    uint32_t addr = start, *patch = (uint32_t *)iat;
    HookTimeTckRef(&tick);

    if ((addr == (uint32_t)patch) &&
        VirtualProtect(patch, sizeof(intptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
        DWORD hkTime = (DWORD)GetProcAddress(GetModuleHandle("winmm.dll"), "timeGetTime"),
              hkPerf = (tick->freq.QuadPart < TICK_8254)?
              (DWORD)GetProcAddress(GetModuleHandle("kernel32.dll"), "QueryPerformanceCounter"):0;
#undef TICK_8254
#undef TICK_ACPI
        for (int i = 0; i < (range >> 2); i++) {
            if (hkTime && (hkTime == patch[i])) {
                HookEntryHook(&patch[i], patch[i]);
                patch[i] = (uint32_t)&TimeHookProc;
                hkTime = 0;
            }
            if (hkPerf && (hkPerf == patch[i])) {
                HookEntryHook(&patch[i], patch[i]);
                patch[i] = (uint32_t)&elapsedTickProc;
                hkPerf = 0;
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
    GetSystemInfo(&si);
    HookTimeTckRef(0);

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

