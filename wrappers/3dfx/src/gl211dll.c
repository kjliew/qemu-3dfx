/*
 * QEMU 3Dfx Glide Pass-Through
 * Guest wrapper DLL - GLIDE.DLL
 *
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>

#include "fxlib.h"
#include "g2xfuncs.h"
#include "szgrdata.h"

#define INLINE inline
#define PT_CALL __stdcall
#define LOG_FNAME "C:\\WRAPFX32.LOG"
//#define DEBUG_FXSTUB

#ifdef DEBUG_FXSTUB
static FILE *logfp;
#define DPRINTF(fmt, ...) \
    do {time_t curr = time(NULL); fprintf(logfp, "%s ", ctime(&curr)); \
	fprintf(logfp, "fxwrap: " fmt , ## __VA_ARGS__); } while(0)
#else
#define DPRINTF(fmt, ...)
#endif

#define GLIDEVER 0x211
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

static volatile uint32_t *ft;
static volatile uint32_t *ptm;
static volatile uint32_t *pt0;
static uint32_t *pt;
static uint32_t *lfb;
static uint32_t *m3df;
static uint32_t *mfifo;
static uint32_t *mdata;
static uint32_t *vgLfb;

static void FiniGlidePTMMBase(PDRVFUNC pDrv)
{
    pDrv->UnmapLinear((FxU32)vgLfb, SHLFB_SIZE);
    pDrv->UnmapLinear((FxU32)lfb, GRLFB_SIZE);
    pDrv->UnmapLinear((FxU32)mfifo, GRSHM_SIZE);
    pDrv->UnmapLinear((FxU32)ptm, PAGE_SIZE);
}
static int InitGlidePTMMBase(PDRVFUNC pDrv)
{
#define MAPMEM(x,paddr,len) \
    do { unsigned long vaddr, valen = len; \
        x = pDrv->MapLinear(0, paddr, &vaddr, &valen)? (void *)vaddr:0; } while(0)
    MAPMEM(ptm, GLIDEPT_MM_BASE, PAGE_SIZE);
    MAPMEM(mfifo, GLIDE_FIFO_BASE, GRSHM_SIZE);
    MAPMEM(lfb, GLIDE_LFB_BASE, GRLFB_SIZE);
    MAPMEM(vgLfb, (GLIDE_LFB_BASE + GRLFB_SIZE), SHLFB_SIZE);

    if (!ptm || !mfifo)
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

    //DPRINTF("%s forced paging addr 0x%08x len 0x%lx\n", func, addr, size);
    for (i = 0; i < cnt; i++) {
	*(volatile uint32_t *)start;
	start += (PAGE_SIZE >> 2);
    }
    *(volatile uint32_t *)start;
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

static int grGlidePresent;
static int grGlideWnd;
char *basename(const char *name);

#define FIFO_EN 1
#define FIFO_GRFUNC(_func,_nargs) \
    if (FIFO_EN && ((mfifo[0] + (_nargs + 1)) < MAX_FIFO) && (mdata[0] < MAX_DATA))  \
        fifoAddEntry(&pt[1], _func, _nargs); \
    else *pt0 = _func \


/* Start - generated by wrapper_genfuncs */

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
        while (GetTickCount() < nexttick)
            Sleep(0);
        nexttick = GetTickCount();
        while (nexttick >= (UINT32_MAX - (1000 / ret)))
            nexttick = GetTickCount();
        nexttick += (1000 / ret);
    }
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
  /*  pt[1] = arg0; */
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideGetVersion;
    fifoOutData(0, arg0, sizeof(char[80]));
}
void PT_CALL grGlideInit(void) {

    int fd;
    uint32_t len, *ptVer, *rsp, ret;
    asm volatile("lea 0x1c(%%esp), %0;":"=rm"(rsp));
    ret = rsp[0];
    fd = open("glide.cfg", O_BINARY | O_RDONLY);
    if (fd > 0) {
        len = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
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
    grGlidePresent = 1;
    HookTimeGetTime(ret);
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
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grGlideShutdown;
    grGlidePresent = 0;
    HookEntryHook(0, 0);
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
void PT_CALL grLfbWriteColorFormat(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbWriteColorFormat;
}
void PT_CALL grLfbWriteColorSwizzle(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbWriteColorSwizzle;
}
void PT_CALL grRenderBuffer(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GRFUNC(FEnum_grRenderBuffer, 1);
}
void PT_CALL grResetTriStats(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grResetTriStats;
}
void PT_CALL grSstConfigPipeline(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstConfigPipeline;
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
    fifoAddData(1, arg0, SIZE_GRHWCONFIG);
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstQueryBoards;
    ret = *pt0;
    if (ret) 
        fifoOutData(0, arg0, SIZE_GRHWCONFIG);
    return ret;
}
uint32_t PT_CALL grSstQueryHardware(uint32_t arg0) {
    int ret = 0;
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

    fd = open((char *)arg0, O_BINARY | O_RDONLY);
    if (fd == -1)
	return 0;
    len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
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
void PT_CALL grLfbBegin(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbBegin;
}
void PT_CALL grLfbBypassMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbBypassMode;
}
void PT_CALL grLfbEnd(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbEnd;
}
uint32_t PT_CALL grLfbGetReadPtr(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbGetReadPtr;
    ret = *pt0;
    if (ret == 0)
        ret = (uint32_t)vgLfb;
    return ret;
}
uint32_t PT_CALL grLfbGetWritePtr(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbGetWritePtr;
    ret = *pt0;
    if (ret == 0)
        ret = (uint32_t)vgLfb;
    return ret;
}
void PT_CALL grLfbOrigin(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbOrigin;
}
void PT_CALL grLfbWriteMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grLfbWriteMode;
}
uint32_t PT_CALL grSstOpen(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    uint32_t ret, wait = 1;
    if (!grGlidePresent)
        grGlideInit();
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5;
    pt[7] = (uint32_t)lfb;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstOpen;
    ret = *pt0;
    while (ret && wait)
        wait = ptm[0xfb8U >> 2];
    return ret;
}
void PT_CALL grSstPassthruMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSstPassthruMode;
}
void PT_CALL guFbReadRegion(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guFbReadRegion;
    memcpy((uint8_t *)arg4, &mdata[ALIGNED(1) >> 2], (arg3 * arg5));
}
void PT_CALL guFbWriteRegion(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(0, arg4, (arg3 * arg5));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_guFbWriteRegion;
}
void PT_CALL grSplash(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_grSplash0;
}
/* End - generated by wrapper_genfuncs */


BOOL APIENTRY DllMain( HINSTANCE hModule,
        DWORD dwReason,
        LPVOID lpReserved
        )
{
    static char cbref, *refcnt;
    uint32_t HostRet;
    OSVERSIONINFO osInfo;
    DRVFUNC drv;
    osInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&osInfo);
    if (osInfo.dwPlatformId == VER_PLATFORM_WIN32_NT)
        kmdDrvInit(&drv);
    else
        vxdDrvInit(&drv);

    switch(dwReason) {
        case DLL_THREAD_ATTACH:
            break;
        case DLL_THREAD_DETACH:
            break;
        case DLL_PROCESS_ATTACH:
            if (drv.Init()) {
                if (InitGlidePTMMBase(&drv)) {
                    drv.Fini();
                    refcnt = (ptm)? (char *)&mdata[1]:&cbref;
                    (*refcnt)++;
                    return (ptm)? TRUE:FALSE;
                }
                drv.Fini();
                mdata[1] = 1;
                refcnt = (char *)&mdata[1];
            }
            else {
                refcnt = &cbref;
                (*refcnt)++;
                return FALSE;
            }
#ifdef DEBUG_FXSTUB
	    logfp = fopen(LOG_FNAME, "w");
#endif
	    DPRINTF("ptm 0x%08x, lfb 0x%08x\n", (uint32_t)ptm, (uint32_t)lfb);
            memcpy(&vgLfb[(SHLFB_SIZE - ALIGNBO(1)) >> 2], buildstr, ALIGNED(1));
	    ptm[(0xFBCU >> 2)] = (0xA0UL << 12) | GLIDEVER;
	    HostRet = ptm[(0xFBCU >> 2)];
	    if (HostRet != ((GLIDEVER << 8) | 0xA0UL)) {
		DPRINTF("Error - Glide init failed 0x%08x\n", HostRet);
		return FALSE;
	    }
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            if (--(*refcnt))
                break;
	    if (grGlidePresent)
		grGlideShutdown();
	    ptm[(0xFBCU >> 2)] = (0xD0UL << 12) | GLIDEVER;
            memset(&vgLfb[(SHLFB_SIZE - ALIGNBO(1)) >> 2], 0, ALIGNED(1));
            mfifo[1] = 0;
            if (drv.Init()) {
                FiniGlidePTMMBase(&drv);
                drv.Fini();
            }
#ifdef DEBUG_FXSTUB
	    fclose(logfp);
#endif
            break;
    }

    return TRUE;
}
