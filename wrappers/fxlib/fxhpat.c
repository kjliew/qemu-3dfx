#include <windows.h>
#include <stdio.h>
#include "hpat.h"

static struct E_PATCH tie95_vga[] = {
    PATCH_D(0x16092, "\xE9\x7F\x01"),
    E_PATCH_END()
};
static struct E_PATCH tie95_exe[] = {
    PATCH_D(0x30377, "\x38\xE0"),
    E_PATCH_END()
};
static struct E_PATCH xwg95_exe[] = {
    PATCH_D(0x856d7, "\x38\xE0"),
    E_PATCH_END()
};
static struct E_PATCH fforce_exe[] = {
    PATCH_D(0x366f1, "\x38\xE0"),
    PATCH_D(0x3673f, "\x38\xE0"),
    PATCH_D(0x3678d, "\x38\xE0"),
    PATCH_D(0x367d4, "\x38\xE0"),
    E_PATCH_END()
};
static struct E_PATCH engrel_blit[] = {
    PATCH_D(0xbf0a8, "\x00"),
    E_PATCH_END()
};
static struct E_PATCH engrel_cursor[] = {
    PATCH_D(0x1805a, "\x38\xC4"),
    E_PATCH_END()
};
static struct E_PATCH req_demo[] = {
    PATCH_D(0x64a0c, "\xEB"),
    E_PATCH_END()
};
static struct E_PATCH d3d_exe[] = {
    PATCH_D(0x65b2d, "\xEB"),
    E_PATCH_END()
};
static struct E_PATCH hg_exe[] = {
    PATCH_D(0x0133e, "\x90\x90"),
    E_PATCH_END()
};
static struct E_PATCH tomb3_exe_1[] = {
    PATCH_D(0x8ec41, "\xB4\x01\x90"),
    PATCH_D(0x8ec52, "\xB0\x00\x90"),
    E_PATCH_END()
};
static struct E_PATCH tomb3_exe_2[] = {
    PATCH_D(0x97321, "\xB4\x01\x90"),
    PATCH_D(0x97332, "\xB0\x00\x90"),
    E_PATCH_END()
};
static struct E_PATCH tomb4_exe[] = {
    PATCH_D(0x8da53, "\x90\xB4\x00"),
    PATCH_D(0x8da64, "\x90\xB0\x00"),
    E_PATCH_END()
};
static struct E_PATCH go_retail[] = {
    PATCH_D(0x40a52, "\xEB\x0D"),
    E_PATCH_END()
};
static struct E_PATCH go_g400_1[] = {
    PATCH_D(0x40d0f, "\xEB\x0D"),
    E_PATCH_END()
};
static struct E_PATCH go_g400_2[] = {
    PATCH_D(0x0c2f4, "\xEB\x08"),
    PATCH_D(0x0c30e, "\xEB"),
    E_PATCH_END()
};
static struct E_PATCH go_g400_3[] = {
    PATCH_D(0x097a9, "\xEB"),
    E_PATCH_END()
};
static struct E_PATCH turok_dem[] = {
    PATCH_D(0x0c6e2, "\xEB\x06"),
    E_PATCH_END()
};
static struct E_PATCH turok_exe[] = {
    PATCH_D(0x0c122, "\xEB\x06"),
    E_PATCH_END()
};
static struct E_PATCH pod_d3d[] = {
    PATCH_D(0xc7a5a, "\x04"),
    E_PATCH_END()
};
static struct E_PATCH pod_3dfx[] = {
    PATCH_D(0x86347, "\x04"),
    E_PATCH_END()
};
static struct E_PATCH matrox_reef_1[] = {
    PATCH_D(0x03f57, "\xEB"),
    E_PATCH_END()
};
static struct E_PATCH matrox_reef_2[] = {
    PATCH_D(0x79935, "\x4B"),
    E_PATCH_END()
};
static COMPATFX fxCompatTbl[] = {
    /* Rage Expendable Retailed & G400 EMBM */
    { "go.exe", "330113cfeb00ae4de299f041fb5714ba", HP_ANYO, go_g400_3 },
    { "go.exe", "c86b59bdfa1360eb43879e59fb3ac89f", HP_ANYO, go_g400_2 },
    { "go.exe", "6cf594d3a8704ba5281f4a11fc3adf7e", HP_ANYO, go_g400_1 },
    { "go.exe", "b64b448d7448103417edbb413e13283c", HP_ANYO, go_retail },
    /* Tie95 3D */
    { "tie95.exe", "384d3ae028aadae67c617a09ed5ea085", HP_2KXP, tie95_exe },
    { "tie95.exe", "a736f8ec53825b4a824060b3af7a332b", HP_2KXP, tie95_vga },
    /* X-Wing95 3D */
    { "xwing95.exe", "7e490350a5f3d35d674c7e6d923660e2", HP_2KXP, xwg95_exe },
    /* Fighting Force */
    { "fforce.exe", "81ee5e035d23e130430f31723cecaf64", HP_2KXP, fforce_exe },
    /* Warhammer: Dark Omen */
    { "engrel.exe", "8dc25757be926088167cb1663b7c7b76", HP_ANYO, engrel_blit },
    { "engrel.exe", "1a0b17352c8fee8c62732ef4f7aae95f", HP_2KXP, engrel_cursor },
    /* Requiem Demo D3D */
    { "reqdemo_d3d.exe", "2ee1cf9120c4f13eef06da62600b0c23", HP_ANYO, req_demo },
    /* Requiem D3D 1.2 */
    { "d3d.exe",   "b783b9fbca594286b606eb07912740b6", HP_ANYO, d3d_exe },
    /* Heavy Gear 1.2 */
    { "hg.exe",    "4685aa795e3916c1bb0de5616a86bfa0", HP_2KXP, hg_exe },
    /* Tomb Raider III */
    { "tomb3.exe", "47c2bb0445fce035d2d264b71bf1faae", HP_2KXP, tomb3_exe_1 },
    { "tomb3.exe", "160e4d0cc6740731ff4598c47c75719c", HP_2KXP, tomb3_exe_2 },
    /* Tomb Raider IV */
    { "tomb4.exe", "e720ab3d4682cbd563a9c3943812fcac", HP_2KXP, tomb4_exe },
    /* Turok Demo v1.01 */
    { "TurokDemo.exe", "58d26953c755bcc64f330f1a3c0441bd", HP_ANYO, turok_dem },
    /* Turok Dinosaur Hunter USA v1.0 */
    { "Turok.exe", "cba05017b943451e8a7b55f22a3d0de9", HP_ANYO, turok_exe },
    /* P.O.D 2.0 Gold/MMX - D3D */
    { "podd3dx.exe", "63313d5b17e048b2fff0dd65e81c22da", HP_ANYO, pod_d3d },
    /* P.O.D 2.0 Gold/MMX - 3Dfx */
    { "podx3dfx.exe", "81176f654fca4af5f50b5636b993a953", HP_ANYO, pod_3dfx },
    /* Matrox Reef Demo 1.1 */
    { "FishDemoClient.exe", "f02a846eeb79b6bf68f48f8746931580", HP_ANYO, matrox_reef_1 },
    { "FishDemoClient.exe", "016a923e609f837871396888e5a63fb9", HP_ANYO, matrox_reef_2 },
    E_PATCH_END()
};
   
const char *basename(const char *name);
const char *md5page(const char *msg);
const int fxCompatPlatformId(const int id)
{
    static int PlatformId;
    PlatformId = (id)? id:PlatformId;
    return PlatformId;
}
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
    fxCompatPlatformId(hpMask);
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
