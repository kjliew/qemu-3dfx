/*
 * QEMU 3Dfx Glide Pass-Through
 * Guest wrapper DOS OVL - GLIDE2X.OVL
 *
 */

#include "stamp.h"
#include "g2xfuncs.h"
#include "szgrdata.h"

#define INLINE __inline
#define PT_CALL __export __stdcall
#define LOG_FNAME "C:\\WRAPFX32.LOG"
//#define DEBUG_FXSTUB

#ifdef DEBUG_FXSTUB
#define DPRINTF(fmt, ...) \
    do {printf("ovl: " fmt , ## __VA_ARGS__); } while(0)
#else
//#define DPRINTF(fmt, ...)
#endif
#include "clib.h"

#define GLIDEVER 0x243
#define GLIDEPT_MM_BASE 0xfbdff000
#define PUSH_F3DF 1
#define MAX_3DF 256*1024
#define MAX_GUTEX 0x1000

typedef struct {
    uint32_t small;
    uint32_t large;
    uint32_t aspect;
    uint32_t format;
    void* data;
} wrTexInfo;

typedef struct {
    uint32_t small;
    uint32_t large;
    uint32_t aspect;
    uint32_t format;
    uint32_t oddEven;
    uint32_t mmid;
} guTexInfo;

typedef struct {
    uint8_t header[SIZE_GU3DFHEADER];
    uint8_t table[SIZE_GUTEXTABLE];
    void *data;
    uint32_t mem_required;
} wr3dfInfo;

typedef struct { 
    int size; 
    void *lfbPtr; 
    uint32_t stride; 
    uint32_t writeMode; 
    uint32_t origin;
} wrLfbInfo;

int  Init(void);
void Fini(void);

static volatile uint32_t *ft;
static volatile uint32_t *ptm;
static volatile uint32_t *pt0;
static uint32_t *pt;
static uint32_t *lfb;
static uint32_t *m3df;
static uint32_t *mfifo;
static uint32_t *mdata;
static uint32_t *vgLfb;
static void *mbufo;

static int InitGlidePTMMBase(void)
{
#define MAPMEM(x,paddr,len) \
    do { unsigned long vaddr, valen = len; \
        x = fxMapLinear(0, paddr, &vaddr, &valen)? (void *)vaddr:0; } while(0)
    MAPMEM(ptm, GLIDEPT_MM_BASE, PAGE_SIZE);
    MAPMEM(mfifo, GLIDE_FIFO_BASE, GRSHM_SIZE);
    MAPMEM(lfb, GLIDE_LFB_BASE, GRLFB_SIZE);
    MAPMEM(vgLfb, (GLIDE_LFB_BASE + GRLFB_SIZE), SHLFB_SIZE);
    MAPMEM(mbufo, MBUFO_BASE, MBUFO_SIZE);

    if (ptm == 0)
        return 1;
    mdata = &mfifo[MAX_FIFO];
    pt = &mfifo[1];
    if (mfifo[1] == (uint32_t)(ptm + (0xFC0U >> 2)))
        return 1;
    mfifo[0] = FIRST_FIFO;
    mdata[0] = ALIGNED(1) >> 2;
    pt[0] = (uint32_t)(ptm + (0xFC0U >> 2));
    m3df = &mfifo[(GRSHM_SIZE - MAX_3DF) >> 2];
    ft = ptm + (0xFB0U >> 2);

    return 0;
}

static INLINE void forcedPageIn(const uint32_t addr, const uint32_t size, const char *func)
{
    int i;
    uint32_t *start = (uint32_t *)(addr & ~(PAGE_SIZE - 1));
    uint32_t cnt = (((addr + size) & ~(PAGE_SIZE - 1)) - (addr & ~(PAGE_SIZE - 1))) / PAGE_SIZE;
    uint32_t tmp;

    //DPRINTF("%s forced paging addr 0x%08x len 0x%lx\n", func, addr, size);
    for (i = 0; i < cnt; i++) {
	tmp = *(volatile uint32_t *)start;
	start += (PAGE_SIZE >> 2);
    }
    tmp = *(volatile uint32_t *)start;
}

static INLINE void fifoAddEntry(uint32_t *pt, int FEnum, int numArgs)
{
    int i, j;

    j = mfifo[0];
    mfifo[j++] = FEnum;
    for (i = 0; i < numArgs; i++)
        mfifo[j++] = pt[i];
    mfifo[0] = j;
}

static INLINE void fifoAddData(int nArg, uint32_t argData, int cbData)
{
    uint32_t *data = (uint32_t *)argData;
    uint32_t numData = (cbData & 0x03)? ((cbData >> 2) + 1):(cbData >> 2);

    int j = mdata[0];
    mdata[0] = ((j + numData) & 0x01)? (j + numData + 1):(j + numData);
    pt[nArg] = (nArg)? argData:pt[nArg];
    memcpy(&mdata[j], data, (numData << 2));
}

static INLINE void fifoOutData(int offs, uint32_t darg, int cbData)
{
    uint32_t *src = &mfifo[(GRSHM_SIZE - PAGE_SIZE + offs) >> 2];
    uint32_t *dst = (uint32_t *)darg;
    uint32_t numData = (cbData & 0x03)? ((cbData >> 2) + 1):(cbData >> 2);

    memcpy(dst, src, (numData << 2));
}

uint32_t PT_CALL grTexTextureMemRequired(uint32_t arg0, uint32_t arg1);
static guTexInfo guTex[MAX_GUTEX];
static int guTexNum;
static void guTexReset(void)
{
    int i;
    for (i = 0; i < MAX_GUTEX; i++)
        guTex[i].mmid = -1;
    guTexNum = 0;
}

static uint32_t guTexRecord(const uint32_t mmid, const uint32_t oddEven, 
        const uint32_t small, const uint32_t large, const uint32_t aspect, const uint32_t format)
{
    uint32_t retval = mmid;

    if (guTexNum < MAX_GUTEX) {
        guTex[guTexNum].mmid = mmid;
        guTex[guTexNum].oddEven = oddEven;
        guTex[guTexNum].small = small;
        guTex[guTexNum].large = large;
        guTex[guTexNum].aspect = aspect;
        guTex[guTexNum].format = format;
        guTexNum++;
    }
    else retval = -1;

    return retval;
}

static uint32_t guTexSize(const uint32_t mmid, const int lodLevel)
{
    int i;
    wrTexInfo texInfo;
    uint32_t oddEven, texBytes = 0;

    for (i = 0; i < guTexNum; i++) {
        if (mmid == guTex[i].mmid) {
            texInfo.small = guTex[i].small;
            texInfo.large = guTex[i].large;
            texInfo.aspect = guTex[i].aspect;
            texInfo.format = guTex[i].format;
            oddEven = GR_MIPMAPLEVELMASK_BOTH;
            if ((lodLevel < 0x00) || (lodLevel > 0x08)) { }
            else {
                texInfo.small = lodLevel;
                texInfo.large = lodLevel;
            }
            texBytes = grTexTextureMemRequired(oddEven, (uint32_t)&texInfo);
            break;
        }
    }
    return texBytes;
}

void PT_CALL grConstantColorValue(uint32_t arg0);
void PT_CALL grConstantColorValue4(uint32_t arg0, uint32_t arg1, uint32_t ar2, uint32_t arg3);
static int tomb;
static int detTomb(void) {
    char *cmd;
    int i;

    cmd = (char *)getDosPSPSeg();
    cmd += 0x83;

    for (i = 0; i < 0x7c; i++) {
	if (*(uint32_t *)cmd == 0x4558452e)
	    break;
	cmd++;
    }
    cmd -= 0x06;

    if (strncmp(cmd, "TOMBUB", 6) == 0)
	return 2;
    if (strncmp(&cmd[2], "TOMB", 4) == 0)
	return 1;

    return 0;
}

static int grGlidePresent;
static int grGlideWnd;
static char texmem[MAX_3DF];
void PT_CALL grTexDownloadMipMap(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3);
void PT_CALL grSstWinClose(void);

#define FIFO_EN 1
#define FIFO_GRFUNC(_func,_nargs) \
    if (FIFO_EN && ((mfifo[0] + (_nargs + 1)) < MAX_FIFO) && (mdata[0] < MAX_DATA))  \
        fifoAddEntry(&pt[1], _func, _nargs); \
    else *pt0 = _func \


/* Start - generated by wrapper_genfuncs */

void PT_CALL ConvertAndDownloadRle(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15) {
#if 0
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; pt[16] = arg15; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_ConvertAndDownloadRle;
#endif
    uint8_t c, *bmp = (uint8_t *)arg7;
    uint32_t scount = 0, dcount = 0, offset = 4 + arg8;
    uint16_t *tex = (uint16_t *)texmem, *tlut = (uint16_t *)arg15,
                   *src = tex + (arg13 * arg14);
    int j, k, count;
    wrTexInfo info;
    // Line offset (v0)
    for (j = 0; j < arg10; j++)
        offset += bmp[4 + j];
    // Write height lines
    for (k = 0; k < arg12; k++) {
        // Decode one RLE line
        scount = offset;
        while((c = bmp[scount]) != 0xE0U) {
            if (c > 0xE0U) {
                for (count = 0; count < (c & 0x1FU); count++) {
                    // tlut is FxU16*
                    src[dcount] = tlut[bmp[scount + 1]];
                    dcount++;
                }
                scount += 2;
            }
            else {
                src[dcount] = tlut[c];
                dcount++; scount++;
            }
        }
        // Copy line into destination texture, offset u0
        memcpy(tex + (k * arg13), src + arg9, arg13 * sizeof(uint16_t));
        offset += bmp[4 + j++];
        dcount = 0;
    }
    // One additional line
    if (arg12 < arg14)
        memcpy(tex + (k * arg13), src + arg9, arg13 * sizeof(uint16_t));
    // Download decoded texture
    info.small = arg2;
    info.large = arg3;
    info.aspect = arg4;
    info.format = arg5;
    info.data = tex;
    grTexDownloadMipMap(arg0, arg1, arg6, (uint32_t)&info);
}
void PT_CALL grAADrawLine(uint32_t arg0, uint32_t arg1) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    fifoAddData(2, arg1, SIZE_GRVERTEX);
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAADrawLine, 2);
}
void PT_CALL grAADrawPoint(uint32_t arg0) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAADrawPoint, 1);
}
void PT_CALL grAADrawPolygon(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    int i, *ilist = (int *)arg1;
    fifoAddData(0, arg1, arg0 * sizeof(int));
    for (i = 0; i < arg0; i++)
        fifoAddData(0, (arg2 + (ilist[i] * SIZE_GRVERTEX)), SIZE_GRVERTEX);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAADrawPolygon, 3);
}
void PT_CALL grAADrawPolygonVertexList(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0 * SIZE_GRVERTEX);
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAADrawPolygonVertexList, 2);
}
void PT_CALL grAADrawTriangle(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    fifoAddData(2, arg1, SIZE_GRVERTEX);
    fifoAddData(3, arg2, SIZE_GRVERTEX);
    pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAADrawTriangle, 6);
}
void PT_CALL grAlphaBlendFunction(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (tomb == 2)
	grConstantColorValue4(f2u32(127.0f), f2u32(0.0f), f2u32(0.0f), f2u32(0.0f));
    if (tomb == 1)
	grConstantColorValue(0x7f000000);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAlphaBlendFunction, 4);
}
void PT_CALL grAlphaCombine(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAlphaCombine, 5);
}
void PT_CALL grAlphaControlsITRGBLighting(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAlphaControlsITRGBLighting, 1);
}
void PT_CALL grAlphaTestFunction(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAlphaTestFunction, 1);
}
void PT_CALL grAlphaTestReferenceValue(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grAlphaTestReferenceValue, 1);
}
void PT_CALL grBufferClear(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grBufferClear, 3);
}
uint32_t PT_CALL grBufferNumPending(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grBufferNumPending;
    return *pt0;
}
void PT_CALL grBufferSwap(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grBufferSwap;
    ret = *pt0;
    if (ret) {
        static uint32_t nexttick;
        while (getTickAcpi() < nexttick) {
            __asm db 0xF3,0x90; /* pause */
        }
        nexttick = getTickAcpi();
        while (nexttick >= (UINT32_MAX - (TICK_ACPI / ret)))
            nexttick = getTickAcpi();
        nexttick += (TICK_ACPI / ret);
    }
}
void PT_CALL grCheckForRoom(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grCheckForRoom;
}
void PT_CALL grChromakeyMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grChromakeyMode, 1);
}
void PT_CALL grChromakeyValue(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grChromakeyValue, 1);
}
void PT_CALL grClipWindow(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grClipWindow, 4);
}
void PT_CALL grColorCombine(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grColorCombine, 5);
}
void PT_CALL grColorMask(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grColorMask, 2);
}
void PT_CALL grConstantColorValue4(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grConstantColorValue4, 4);
}
void PT_CALL grConstantColorValue(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grConstantColorValue, 1);
}
void PT_CALL grCullMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grCullMode, 1);
}
void PT_CALL grDepthBiasLevel(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDepthBiasLevel, 1);
}
void PT_CALL grDepthBufferFunction(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDepthBufferFunction, 1);
}
void PT_CALL grDepthBufferMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDepthBufferMode, 1);
}
void PT_CALL grDepthMask(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDepthMask, 1);
}
void PT_CALL grDisableAllEffects(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDisableAllEffects, 0);
}
void PT_CALL grDitherMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDitherMode, 1);
}
void PT_CALL grDrawLine(uint32_t arg0, uint32_t arg1) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    fifoAddData(2, arg1, SIZE_GRVERTEX);
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDrawLine, 2);
}
void PT_CALL grDrawPlanarPolygon(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    int i, *ilist = (int *)arg1;
    fifoAddData(0, arg1, arg0 * sizeof(int));
    for (i = 0; i < arg0; i++)
        fifoAddData(0, (arg2 + (ilist[i] * SIZE_GRVERTEX)), SIZE_GRVERTEX);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDrawPlanarPolygon, 3);
}
void PT_CALL grDrawPlanarPolygonVertexList(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0 * SIZE_GRVERTEX);
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDrawPlanarPolygonVertexList, 2);
}
void PT_CALL grDrawPoint(uint32_t arg0) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDrawPoint, 1);
}
void PT_CALL grDrawPolygon(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    int i, *ilist = (int *)arg1;
    fifoAddData(0, arg1, arg0 * sizeof(int));
    for (i = 0; i < arg0; i++)
        fifoAddData(0, (arg2 + (ilist[i] * SIZE_GRVERTEX)), SIZE_GRVERTEX);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDrawPolygon, 3);
}
void PT_CALL grDrawPolygonVertexList(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0 * SIZE_GRVERTEX);
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDrawPolygonVertexList, 2);
}
void PT_CALL grDrawTriangle(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    fifoAddData(2, arg1, SIZE_GRVERTEX);
    fifoAddData(3, arg2, SIZE_GRVERTEX);
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grDrawTriangle, 3);
}
void PT_CALL grErrorSetCallback(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grErrorSetCallback;
}
void PT_CALL grFogColorValue(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grFogColorValue, 1);
}
void PT_CALL grFogMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grFogMode, 1);
}
void PT_CALL grFogTable(uint32_t arg0) {
    uint32_t n = 64;
    fifoAddData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoAddData(0, arg0, n * sizeof(uint8_t));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grFogTable, 1);
}
void PT_CALL grGammaCorrectionValue(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grGammaCorrectionValue, 1);
}
void PT_CALL grGlideGetState(uint32_t arg0) {
    pt[1] = arg0;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideGetState;
}
void PT_CALL grGlideGetVersion(uint32_t arg0) {
    if ((!grGlidePresent) && (!Init()))
        return;
  /*  pt[1] = arg0; */
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideGetVersion;
    fifoOutData(0, arg0, sizeof(char[80]));
}
void PT_CALL grGlideInit(void) {

    int fd;
    uint32_t len, *ptVer;
    if ((!grGlidePresent) && (!Init()))
        return;
    if (grGlideWnd)
	return;
    fd = open("glide.cfg");
    if (fd > 0) {
        len = fsize(fd);
        if (len > (MAX_3DF - ALIGNED(1)))
            len = (MAX_3DF - ALIGNED(1));
        read(fd, &m3df[ALIGNED(1) >> 2], len);
        close(fd);
        m3df[0] = len;
        ft[0] = MAX_3DF;
    }
    ptVer = &mfifo[(GRSHM_SIZE - PAGE_SIZE) >> 2];
    memcpy(ptVer, buildstr, sizeof(buildstr));
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideInit;
    guTexReset();
    grGlideWnd = 0;
}
void PT_CALL grGlideSetState(uint32_t arg0) {
    pt[1] = arg0;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideSetState;
}
void PT_CALL grGlideShamelessPlug(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideShamelessPlug;
}
void PT_CALL grGlideShutdown(void) {
    grSstWinClose(); 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideShutdown;
    Fini();
    grGlidePresent = 0;
}
void PT_CALL grHints(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grHints, 2);
}
void PT_CALL grLfbConstantAlpha(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grLfbConstantAlpha, 1);
}
void PT_CALL grLfbConstantDepth(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grLfbConstantDepth, 1);
}
uint32_t PT_CALL grLfbLock(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    int ret;
    uint32_t shmaddr;
    fifoAddData(0, arg5, SIZE_GRLFBINFO);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbLock;
    ret = *pt0;
    fifoOutData(0, arg5, SIZE_GRLFBINFO);
    if (ret & 0x20U) {
        shmaddr = (uint32_t)mbufo + (uint32_t)(((wrLfbInfo *)arg5)->lfbPtr);
        ((wrLfbInfo *)arg5)->lfbPtr = (uint32_t *)shmaddr;
        ret = 1;
    }
    if (ret & 0x10U) {
        shmaddr = (uint32_t)vgLfb + (uint32_t)(((wrLfbInfo *)arg5)->lfbPtr);
        ((wrLfbInfo *)arg5)->lfbPtr = (uint32_t *)shmaddr;
        ((wrLfbInfo *)arg5)->stride = ((arg2 & 0x0EU) == 0x04)? 0x1000:0x800;
        ((wrLfbInfo *)arg5)->writeMode = arg2;
        ((wrLfbInfo *)arg5)->origin = arg3;
        ret = 1;
    }
    return ret;
}
uint32_t PT_CALL grLfbReadRegion(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    int ret;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbReadRegion;
    ret = *pt0;
    if (ret)
        memcpy((uint8_t*)arg6, &mdata[ALIGNED(1) >> 2], (arg4 * arg5));
    return ret;
}
uint32_t PT_CALL grLfbUnlock(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbUnlock;
    return *pt0;
}
void PT_CALL grLfbWriteColorFormat(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbWriteColorFormat;
}
void PT_CALL grLfbWriteColorSwizzle(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbWriteColorSwizzle;
}
uint32_t PT_CALL grLfbWriteRegion(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    int ret;
    fifoAddData(0, arg7, (arg5 * arg6));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbWriteRegion;
    ret = *pt0;
    return ret;
}
void PT_CALL grRenderBuffer(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grRenderBuffer, 1);
}
void PT_CALL grResetTriStats(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grResetTriStats;
}
void PT_CALL grSplash(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSplash;
}
void PT_CALL grSstConfigPipeline(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstConfigPipeline;
}
uint32_t PT_CALL grSstControl(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstControl;
    return *pt0;
}
void PT_CALL grSstIdle(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstIdle;
}
uint32_t PT_CALL grSstIsBusy(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstIsBusy;
    return *pt0;
}
void PT_CALL grSstOrigin(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstOrigin;
}
void PT_CALL grSstPerfStats(uint32_t arg0) {
    fifoAddData(1, arg0, SIZE_GRSSTPERFSTATS);
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstPerfStats;
    fifoOutData(0, arg0, SIZE_GRSSTPERFSTATS);
}
uint32_t PT_CALL grSstQueryBoards(uint32_t arg0) {
    int ret = 0;
    if ((!grGlidePresent) && (!Init()))
	return ret;
    fifoAddData(1, arg0, SIZE_GRHWCONFIG);
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstQueryBoards;
    ret = *pt0;
    if (ret) 
        fifoOutData(0, arg0, SIZE_GRHWCONFIG);
    return ret;
}
uint32_t PT_CALL grSstQueryHardware(uint32_t arg0) {
    int ret = 0;
    if ((!grGlidePresent) && (!Init()))
	return ret;
    fifoAddData(1, arg0, SIZE_GRHWCONFIG);
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstQueryHardware;
    ret = *pt0;
    if (ret)
        fifoOutData(0, arg0, SIZE_GRHWCONFIG);
    return ret;
}
void PT_CALL grSstResetPerfStats(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstResetPerfStats;
}
uint32_t PT_CALL grSstScreenHeight(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstScreenHeight;
    return *pt0;
}
uint32_t PT_CALL grSstScreenWidth(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstScreenWidth;
    return *pt0;
}
void PT_CALL grSstSelect(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstSelect;
}
uint32_t PT_CALL grSstStatus(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstStatus;
    return *pt0;
}
uint32_t PT_CALL grSstVRetraceOn(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstVRetraceOn;
    return *pt0;
}
void PT_CALL grSstVidMode(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstVidMode;
}
uint32_t PT_CALL grSstVideoLine(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstVideoLine;
    return *pt0;
}
void PT_CALL grSstWinClose(void) {
    uint32_t wait = 1;
    if (!grGlideWnd)
	return;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstWinClose;
    grGlideWnd = 0;
    while (wait)
        wait = ptm[0xfb8U >> 2];
}
uint32_t PT_CALL grSstWinOpen(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    uint32_t ret, wait = 1;
    if (!grGlidePresent)
        grGlideInit();
    if (grGlideWnd)
	grSstWinClose();
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6;
    pt[8] = (uint32_t)lfb;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstWinOpen;
    ret = *pt0;
    while (ret && wait)
        wait = ptm[0xfb8U >> 2];
    grGlideWnd = ret;
    return ret;
}
uint32_t PT_CALL grTexCalcMemRequired(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grTexCalcMemRequired;
    return *pt0;
}
void PT_CALL grTexClampMode(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexClampMode, 3);
}
void PT_CALL grTexCombine(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexCombine, 7);
}
void PT_CALL grTexCombineFunction(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexCombineFunction, 2);
}
void PT_CALL grTexDetailControl(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexDetailControl, 4);
}
void PT_CALL grTexDownloadMipMap(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    wrTexInfo info;
    uint32_t addr = (uint32_t)((wrTexInfo *)arg3)->data, dlen;

    info.small = ((wrTexInfo *)arg3)->small;
    info.large = ((wrTexInfo *)arg3)->large;
    info.aspect = ((wrTexInfo *)arg3)->aspect;
    info.format = ((wrTexInfo *)arg3)->format;
    dlen =  grTexTextureMemRequired(GR_MIPMAPLEVELMASK_BOTH, (uint32_t)&info);

    fifoAddData(0, arg3, SIZE_GRTEXINFO);
    fifoAddData(0, addr, dlen);

    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = dlen;
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexDownloadMipMap, 5);
}
void PT_CALL grTexDownloadMipMapLevel(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    wrTexInfo info;
    uint32_t dlen;

    info.small = arg2;
    info.large = arg2;
    info.aspect = arg4;
    info.format = arg5;
    dlen =  grTexTextureMemRequired(arg6, (uint32_t)&info);

    fifoAddData(0, arg7, dlen);

    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = dlen;
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexDownloadMipMapLevel, 9);
}
void PT_CALL grTexDownloadMipMapLevelPartial(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    int texBytes, thisLOD, aspect;
    
    thisLOD = arg2;
    aspect = arg4;
    texBytes = 256 >> thisLOD;
    if (aspect & 0x04)
        texBytes >>= ((aspect & 0x03) + 1);
    texBytes >>= (arg5 < 0x08)? 2:1;
    if (texBytes <= 0)
        texBytes = 1;
    texBytes = texBytes * (arg9 - arg8 + 1) * 4;

    fifoAddData(0, arg7, texBytes);

    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = texBytes; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexDownloadMipMapLevelPartial, 11);
}
void PT_CALL grTexDownloadTable(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, (arg1 == 0x02)? SIZE_GUTEXPALETTE:SIZE_GUNCCTABLE);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexDownloadTable, 3);
}
void PT_CALL grTexDownloadTablePartial(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    fifoAddData(0, arg2, (arg1 == 0x02)? (arg4 + 1)*sizeof(uint32_t):SIZE_GUNCCTABLE);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexDownloadTablePartial, 5);
}
void PT_CALL grTexFilterMode(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexFilterMode, 3);
}
void PT_CALL grTexLodBiasValue(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexLodBiasValue, 2);
}
uint32_t PT_CALL grTexMaxAddress(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grTexMaxAddress;
    return *pt0;
}
uint32_t PT_CALL grTexMinAddress(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grTexMinAddress;
    return *pt0;
}
void PT_CALL grTexMipMapMode(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexMipMapMode, 3);
}
void PT_CALL grTexMultibase(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grTexMultibase;
}
void PT_CALL grTexMultibaseAddress(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grTexMultibaseAddress;
}
void PT_CALL grTexNCCTable(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexNCCTable, 2);
}
void PT_CALL grTexSource(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, SIZE_GRTEXINFO);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grTexSource, 4);
}
uint32_t PT_CALL grTexTextureMemRequired(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, SIZE_GRTEXINFO);
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grTexTextureMemRequired;
    return *pt0;
}
void PT_CALL grTriStats(uint32_t arg0, uint32_t arg1) {
    fifoAddData(1, arg0, sizeof(uint32_t));
    fifoAddData(2, arg1, sizeof(uint32_t));
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grTriStats;
    fifoOutData(0, arg0, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg1, sizeof(uint32_t));
}
uint32_t PT_CALL gu3dfGetInfo(uint32_t arg0, uint32_t arg1) {
    int ret;
#if PUSH_F3DF    
    int fd;
    uint32_t len;

    fd = open((char *)arg0);
    if (fd == -1)
	return 0;
    len = fsize(fd);
    if (len > (MAX_3DF - ALIGNED(1)))
        len = (MAX_3DF - ALIGNED(1));
    read(fd, &m3df[ALIGNED(1) >> 2], len);
    close(fd);
    m3df[0] = len;
    ft[0] = MAX_3DF;
#endif
    fifoAddData(0, (uint32_t)(basename((const char *)arg0)), sizeof(char[64]));
    pt[1] = arg0; pt[2] = arg1;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_gu3dfGetInfo;
    ret = *pt0;
    if (ret)
        fifoOutData(0, arg1, SIZE_GU3DFINFO);
    return ret;
}
uint32_t PT_CALL gu3dfLoad(uint32_t arg0, uint32_t arg1) {
    int ret;
    wr3dfInfo *info = (wr3dfInfo *)arg1;
    fifoAddData(0, (uint32_t)(basename((const char *)arg0)), sizeof(char[64]));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_gu3dfLoad;
    ret = *pt0;
    if (ret)
        memcpy(info->data, &m3df[ALIGNED(1) >> 2], info->mem_required);
    return ret;
}
void PT_CALL guAADrawTriangleWithClip(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    fifoAddData(2, arg1, SIZE_GRVERTEX);
    fifoAddData(3, arg2, SIZE_GRVERTEX);
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guAADrawTriangleWithClip, 3);
}
void PT_CALL guAlphaSource(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guAlphaSource, 1);
}
void PT_CALL guColorCombineFunction(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guColorCombineFunction, 1);
}
void PT_CALL guDrawPolygonVertexListWithClip(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0 * SIZE_GRVERTEX);
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guDrawPolygonVertexListWithClip, 2);
}
void PT_CALL guDrawTriangleWithClip(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(1, arg0, SIZE_GRVERTEX);
    fifoAddData(2, arg1, SIZE_GRVERTEX);
    fifoAddData(3, arg2, SIZE_GRVERTEX);
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guDrawTriangleWithClip, 3);
}
void PT_CALL guEncodeRLE16(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guEncodeRLE16;
}
void PT_CALL guEndianSwapBytes(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guEndianSwapBytes;
}
void PT_CALL guEndianSwapWords(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guEndianSwapWords;
}
void PT_CALL guFogGenerateExp2(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guFogGenerateExp2;
    fifoOutData(0, arg0, sizeof(uint8_t[64]));
}
void PT_CALL guFogGenerateExp(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guFogGenerateExp;
    fifoOutData(0, arg0, sizeof(uint8_t[64]));
}
void PT_CALL guFogGenerateLinear(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guFogGenerateLinear;
    fifoOutData(0, arg0, sizeof(uint8_t[64]));
}
float PT_CALL guFogTableIndexToW(uint32_t arg0) {
    float ret;
    uint32_t r;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guFogTableIndexToW;
    r = *pt0;
    memcpy(&ret, &r, sizeof(uint32_t));
    return ret;
}
void PT_CALL guMPDrawTriangle(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guMPDrawTriangle;
}
void PT_CALL guMPInit(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guMPInit;
}
void PT_CALL guMPTexCombineFunction(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guMPTexCombineFunction;
}
void PT_CALL guMPTexSource(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guMPTexSource;
}
void PT_CALL guMovieSetName(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guMovieSetName;
}
void PT_CALL guMovieStart(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guMovieStart;
}
void PT_CALL guMovieStop(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guMovieStop;
}
uint32_t PT_CALL guTexAllocateMemory(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14) {
    uint32_t mmid;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guTexAllocateMemory;
    mmid = *pt0;
    if (mmid != -1)
        guTexRecord(mmid, arg1, arg6, arg7, arg8, arg4);
    return mmid;
}
uint32_t PT_CALL guTexChangeAttributes(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guTexChangeAttributes;
    return *pt0;
}
void PT_CALL guTexCombineFunction(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guTexCombineFunction, 2);
}
void PT_CALL guTexCreateColorMipMap(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guTexCreateColorMipMap;
}
void PT_CALL guTexDownloadMipMap(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t texBytes  = guTexSize(arg0, 0x0F);
    if (texBytes)
        fifoAddData(0, arg1, texBytes);
    if (arg2)
        fifoAddData(0, arg2, SIZE_GUNCCTABLE);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = texBytes;
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guTexDownloadMipMap, 4);
}
void PT_CALL guTexDownloadMipMapLevel(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    void **src = (void **)arg2;
    uint8_t pad[ALIGNED(1)];
    uint32_t texBytes  = guTexSize(arg0, arg1);
    if (texBytes) {
        fifoAddData(0, (uint32_t)(*src), texBytes);
        fifoAddData(0, (uint32_t)pad, ALIGNED(1));
        *src = (void *)(((uint32_t)*src) + texBytes);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = texBytes;
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guTexDownloadMipMapLevel, 4);
}
uint32_t PT_CALL guTexGetCurrentMipMap(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guTexGetCurrentMipMap;
    return *pt0;
}
uint32_t PT_CALL guTexGetMipMapInfo(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guTexGetMipMapInfo;
    return *pt0;
}
uint32_t PT_CALL guTexMemQueryAvail(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guTexMemQueryAvail;
    return *pt0;
}
void PT_CALL guTexMemReset(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guTexMemReset;
    guTexReset();
}
void PT_CALL guTexSource(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_guTexSource, 1);
}
/* End - generated by wrapper_genfuncs */


int Init(void)
{
    uint32_t HostRet;

    if (grGlidePresent)
	return 1;

    if (InitGlidePTMMBase())
        return 0;

    memcpy(&vgLfb[(SHLFB_SIZE - ALIGNBO(1)) >> 2], buildstr, ALIGNED(1));
    ptm[(0xFBCU >> 2)] = (0xA0UL << 12) | GLIDEVER;
    HostRet = ptm[(0xFBCU >> 2)];
    if (HostRet != ((GLIDEVER << 8) | 0xA0UL))
        return 0;

    tomb = detTomb();

    grGlidePresent = 1;
    return 1;
}

void Fini(void)
{
    ptm[(0xFBCU >> 2)] = (0xD0UL << 12) | GLIDEVER;
    memset(&vgLfb[(SHLFB_SIZE - ALIGNBO(1)) >> 2], 0, ALIGNED(1));
    mfifo[1] = 0;
}

int *__8087, *_fltused_;
int __DLLstart_(void *inst, unsigned reason)
{
    return Init();
}
