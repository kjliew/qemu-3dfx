#include <windows.h>
#include <stdio.h>
#include "hpat.h"

static struct E_PATCH engrel_exe[] = {
    { 0xbf0a8, 1, "\x00" },
    E_PATCH_END()
};
static struct E_PATCH d3d_exe[] = {
    { 0x65b2d, 1, "\xEB" },
    E_PATCH_END()
};
static struct E_PATCH hg_exe[] = {
    { 0x0133e, 2, "\x90\x90" },
    E_PATCH_END()
};
static struct E_PATCH tomb4_exe[] = {
    { 0x8da53, 3, "\x90\xB4\x00" },
    { 0x8da64, 3, "\x90\xB0\x00" },
    E_PATCH_END()
};
static COMPATFX fxCompatTbl[] = {
    /* Warhammer: Dark Omen */
    { "engrel.exe", "8dc25757be926088167cb1663b7c7b76", HP_ANYO, engrel_exe },
    /* Requiem D3D 1.2 */
    { "d3d.exe",   "b783b9fbca594286b606eb07912740b6", HP_ANYO, d3d_exe },
    /* Heavy Gear 1.2 */
    { "hg.exe",    "4685aa795e3916c1bb0de5616a86bfa0", HP_2KXP, hg_exe },
    /* Tomb Raider IV */
    { "tomb4.exe", "e720ab3d4682cbd563a9c3943812fcac", HP_2KXP, tomb4_exe },
    E_PATCH_END()
};
   
const char *basename(const char *name);
const char *md5page(const char *msg);
const PCOMPATFX fxCompatTblPtr(void)
{
    static int once;
    if (!once) {
        once = !once;
        return fxCompatTbl;
    }
    return 0;
}
void HookPatchfxCompat(const DWORD hpMask)
{
    TCHAR modName[MAX_PATH];
    if (GetModuleFileName(NULL, modName, MAX_PATH) < MAX_PATH) {
        int i = 0, j;
        while (fxCompatTbl[i].modName) {
            DWORD modBase = (DWORD)GetModuleHandle(0), oldProt;
            void *modPage;
            j = 0;
            modPage = (unsigned char *)(modBase + (fxCompatTbl[i].ptr[j].offs & ~0xFFFU));
            if ((fxCompatTbl[i].op_mask & hpMask) &&
                !stricmp(fxCompatTbl[i].modName, basename(modName)) &&
                !strcmp(fxCompatTbl[i].md5, md5page((const char *)modPage)) &&
                VirtualProtect(modPage, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProt)) {
                fxCompatTbl[i].op_mask |= HP_DONE;
                while(fxCompatTbl[i].ptr[j].offs) {
                    memcpy((unsigned char *)(modBase + fxCompatTbl[i].ptr[j].offs),
                            fxCompatTbl[i].ptr[j].cb, fxCompatTbl[i].ptr[j].len);
                    j++;
                }
                VirtualProtect(modPage, sizeof(void *), oldProt, &oldProt);
            }
            i++;
        }
    }
}
