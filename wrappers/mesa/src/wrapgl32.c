/*
 * QEMU MESA GL Pass-Through
 * Guest wrapper DLL - OPENGL32.DLL
 *
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#include "fxlib.h"
#include "mglfuncs.h"

#define INLINE inline
#define PT_CALL __stdcall
#define COMPACT __attribute__((optimize("Os")))
#define COMPACT_FRAME COMPACT \
    __attribute__((target("no-sse2"))) \
    __attribute__((optimize("-fno-omit-frame-pointer")))
#define LOG_NAME "C:\\WRAPGL32.LOG"
#define TRACE_PNAME(p) \
    if ((logpname[p>>3] & (1<<(p%8))) == 0) { \
        logpname[p>>3] |= (1<<(p%8)); \
        char str[255]; wsprintf(str, "%s() %04X\n", __func__ , p); OutputDebugString(str); \
    } \

#ifdef DEBUG_GLSTUB
static FILE *logfp = NULL;
#define DPRINTF(fmt, ...) \
    do {fprintf(logfp, "glwrap: " fmt "\n" , ## __VA_ARGS__); } while(0)
#define DPRINTF_COND(cond, fmt, ...) \
    if (cond) {fprintf(logfp, "glwrap: " fmt "\n" , ## __VA_ARGS__); }
#else
  #ifndef _DPRINT_H
    #define DPRINTF(fmt, ...)
    #define DPRINTF_COND(cond, fmt, ...)
  #endif
#endif

#define MESAVER 0x320
#define MESAPT_MM_BASE 0xefffe000

extern const char rev_[];
static volatile uint32_t *ptm;
static volatile uint32_t *pt0;
static uint32_t *pt;
static uint32_t *mfifo;
static uint32_t *mdata;
static uint32_t *fbtm;
static void *mbufo;

static void FiniMesaPTMMBase(PDRVFUNC pDrv)
{
    pDrv->UnmapLinear((FxU32)mbufo, MBUFO_SIZE);
    pDrv->UnmapLinear((FxU32)fbtm, MGLFBT_SIZE);
    pDrv->UnmapLinear((FxU32)mfifo, MGLSHM_SIZE);
    pDrv->UnmapLinear((FxU32)ptm, PAGE_SIZE);
}
static int InitMesaPTMMBase(PDRVFUNC pDrv)
{
#define MAPMEM(x,paddr,len) \
    do { unsigned long vaddr, valen = len; \
        x = pDrv->MapLinear(0, paddr, &vaddr, &valen)? (void *)vaddr:0; } while(0)
    MAPMEM(ptm, MESAPT_MM_BASE, PAGE_SIZE);
    MAPMEM(mfifo, MESA_FIFO_BASE, MGLSHM_SIZE);
    MAPMEM(fbtm, MESA_FBTM_BASE, MGLFBT_SIZE);
    MAPMEM(mbufo, MBUFO_BASE, MBUFO_SIZE);

    if (!ptm || !mfifo)
        return 1;
    mdata = &mfifo[MAX_FIFO];
    pt = &mfifo[1];
    if ((mfifo[1] & 0xFFFU) == ((uint32_t)(ptm + (0xFC0U >> 2)) & 0xFFFU))
        return 1;
    mfifo[0] = FIRST_FIFO;
    mdata[0] = ALIGNED(1) >> 2;
    pt[0] = (uint32_t)(ptm + (0xFC0U >> 2));

    return 0;
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
    DPRINTF_COND(((j + numData) >= MAX_DATA),
        "FIFO AddData overbound %06x curr %06x chunk %06x", (j + numData), j, numData);
    mdata[0] = ((j + numData) & 0x01)? (j + numData + 1):(j + numData);
    pt[nArg] = (nArg)? argData:pt[nArg];
    memcpy(&mdata[j], data, (numData << 2));
}

static INLINE void fifoOutData(int offs, uint32_t darg, int cbData)
{
    uint32_t *src = &mfifo[(MGLSHM_SIZE - (3*PAGE_SIZE) + offs) >> 2];
    uint32_t *dst = (uint32_t *)darg;
    uint32_t numData = (cbData & 0x03)? ((cbData >> 2) + 1):(cbData >> 2);

    DPRINTF_COND(((offs + (numData << 2)) > (3*PAGE_SIZE)),
        "FIFO OutData overflowed %06x %06x", offs, numData);
    memcpy(dst, src, (numData << 2));
}

#define FIFO_FLUSH(x) \
    if (x) ptm[0x0FCU >> 2] = MESAGL_MAGIC
#define FBTMMCPY(d,s,n) \
    if (n <= MGLFBT_SIZE) memcpy(d,s,n)
#define RENDERER_VALID(x) \
    if (memcmp(rendstr, x, strlen(x))) return
static HWND GLwnd;
static HHOOK hHook;
static int currPixFmt;
static uint32_t currDC, currGLRC;
static uint32_t currPB[MAX_PBUFFER];
static char vendstr[64];
static char rendstr[128];
static char vernstr[80];
static char extnstr[3*PAGE_SIZE];
static char glslstr[48];
static char *logpname;
static struct {
    vtxarry_t Color, EdgeFlag, Index, Normal, TexCoord[MAX_TEXUNIT], Vertex,
              SecondaryColor, FogCoord, Weight, GenAttrib[2];
    int texUnit;
    int arrayBuf;
    int elemArryBuf;
    int vao;
} vtxArry;
static vtxarry_t Interleaved;
static int pixPackBuf, pixUnpackBuf;
static int szPackWidth, szUnpackWidth;
static int szPackHeight, szUnpackHeight;
static int queryBuf;
static void vtxarry_init(vtxarry_t *varry, int size, int type, int stride, void *ptr)
{
    varry->size = size;
    varry->type = type;
    varry->stride = stride;
    varry->ptr = ptr;
}
static void vtxarry_ptr_reset(void)
{
    vtxArry.Color.ptr = 0;
    vtxArry.EdgeFlag.ptr = 0;
    vtxArry.Index.ptr = 0;
    vtxArry.Normal.ptr = 0;
    for (int i = 0; i < MAX_TEXUNIT; i++)
        vtxArry.TexCoord[i].ptr = 0;
    vtxArry.Vertex.ptr = 0;
    vtxArry.SecondaryColor.ptr = 0;
    vtxArry.FogCoord.ptr = 0;
    vtxArry.Weight.ptr = 0;
    vtxArry.GenAttrib[0].ptr = 0;
    vtxArry.GenAttrib[1].ptr = 0;
}
static void vtxarry_state(uint32_t arg0, int st)
{
#define GENERIC_ATTRIB6 0x06
#define GENERIC_ATTRIB7 0x07
    switch(arg0) {
        case GL_COLOR_ARRAY:
            vtxArry.Color.enable = st;
            break;
        case GL_EDGE_FLAG_ARRAY:
            vtxArry.EdgeFlag.enable = st;
            break;
        case GL_INDEX_ARRAY:
            vtxArry.Index.enable = st;
            break;
        case GL_NORMAL_ARRAY:
            vtxArry.Normal.enable = st;
            break;
        case GL_TEXTURE_COORD_ARRAY:
            vtxArry.TexCoord[vtxArry.texUnit].enable = st;
            break;
        case GL_VERTEX_ARRAY:
            vtxArry.Vertex.enable = st;
            break;
        case GL_SECONDARY_COLOR_ARRAY:
            vtxArry.SecondaryColor.enable = st;
            break;
        case GL_FOG_COORDINATE_ARRAY:
            vtxArry.FogCoord.enable = st;
            break;
        case GL_WEIGHT_ARRAY_ARB:
            vtxArry.Weight.enable = st;
            break;
        case GENERIC_ATTRIB6:
            vtxArry.GenAttrib[0].enable = st;
            break;
        case GENERIC_ATTRIB7:
            vtxArry.GenAttrib[1].enable = st;
            break;
        default:
            break;
    }
}
static uint32_t vattr2arry_state(int attr)
{
    static const uint32_t st_arry[] = {
        GL_VERTEX_ARRAY,
        GL_WEIGHT_ARRAY_ARB,
        GL_NORMAL_ARRAY,
        GL_COLOR_ARRAY,
        GL_SECONDARY_COLOR_ARRAY,
        GL_FOG_COORDINATE_ARRAY,
        GENERIC_ATTRIB6, GENERIC_ATTRIB7,
    };
    uint32_t st = st_arry[attr & 0x07U];
    if (attr & 0x08U) {
        vtxArry.texUnit = attr & 0x07U;
        st = GL_TEXTURE_COORD_ARRAY;
    }
    return st;
}
static vtxarry_t *vattr2arry(int attr)
{
    static vtxarry_t *attr2arry[] = {
        &vtxArry.Vertex,
        &vtxArry.Weight,
        &vtxArry.Normal,
        &vtxArry.Color,
        &vtxArry.SecondaryColor,
        &vtxArry.FogCoord,
        &vtxArry.GenAttrib[0],
        &vtxArry.GenAttrib[1],
    };
    vtxarry_t *arry = attr2arry[attr & 0x07U];
    if (attr & 0x08U) {
        int i = (attr & 0x07U);
        arry = &vtxArry.TexCoord[i];
    }
    return arry;
}
static void PrepVertexArray(int start, int end, int sizei)
{
    int i, n = sizei, cbElem;
    if (Interleaved.enable && Interleaved.ptr) {
        cbElem = (Interleaved.stride)? Interleaved.stride:Interleaved.size;
        n += ALIGNED((cbElem*(end - start) + Interleaved.size));
    }
    else {
        if (vtxArry.Color.enable && vtxArry.Color.ptr) {
            cbElem = (vtxArry.Color.stride)? vtxArry.Color.stride:szgldata(vtxArry.Color.size, vtxArry.Color.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.Color.size, vtxArry.Color.type)));
        }
        if (vtxArry.EdgeFlag.enable && vtxArry.EdgeFlag.ptr) {
            cbElem = (vtxArry.EdgeFlag.stride)? vtxArry.EdgeFlag.stride:szgldata(vtxArry.EdgeFlag.size, vtxArry.EdgeFlag.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.EdgeFlag.size, vtxArry.EdgeFlag.type)));
        }
        if (vtxArry.Index.enable && vtxArry.Index.ptr) {
            cbElem = (vtxArry.Index.stride)? vtxArry.Index.stride:szgldata(vtxArry.Index.size, vtxArry.Index.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.Index.size, vtxArry.Index.type)));
        }
        if (vtxArry.Normal.enable && vtxArry.Normal.ptr) {
            cbElem = (vtxArry.Normal.stride)? vtxArry.Normal.stride:szgldata(vtxArry.Normal.size, vtxArry.Normal.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.Normal.size, vtxArry.Normal.type)));
        }
        for (i = 0; i < MAX_TEXUNIT; i++) {
            if (vtxArry.TexCoord[i].enable && vtxArry.TexCoord[i].ptr) {
                int ucb;
                cbElem = (vtxArry.TexCoord[i].stride)? vtxArry.TexCoord[i].stride:szgldata(vtxArry.TexCoord[i].size, vtxArry.TexCoord[i].type);
                ucb = ALIGNED((cbElem*(end - start) + szgldata(vtxArry.TexCoord[i].size, vtxArry.TexCoord[i].type)));
                if (IsBadReadPtr(vtxArry.TexCoord[i].ptr, ucb)) {
                    void PT_CALL glClientActiveTexture(uint32_t);
                    void PT_CALL glDisableClientState(uint32_t);
                    glClientActiveTexture(GL_TEXTURE0 + i);
                    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                }
                else
                    n += ucb;
            }
        }
        if (vtxArry.Vertex.enable && vtxArry.Vertex.ptr) {
            cbElem = (vtxArry.Vertex.stride)? vtxArry.Vertex.stride:szgldata(vtxArry.Vertex.size, vtxArry.Vertex.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.Vertex.size, vtxArry.Vertex.type)));
        }
        if (vtxArry.SecondaryColor.enable && vtxArry.SecondaryColor.ptr) {
            cbElem = (vtxArry.SecondaryColor.stride)? vtxArry.SecondaryColor.stride:szgldata(vtxArry.SecondaryColor.size, vtxArry.SecondaryColor.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.SecondaryColor.size, vtxArry.SecondaryColor.type)));
        }
        if (vtxArry.FogCoord.enable && vtxArry.FogCoord.ptr) {
            cbElem = (vtxArry.FogCoord.stride)? vtxArry.FogCoord.stride:szgldata(vtxArry.FogCoord.size, vtxArry.FogCoord.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.FogCoord.size, vtxArry.FogCoord.type)));
        }
        if (vtxArry.Weight.enable && vtxArry.Weight.ptr) {
            cbElem = (vtxArry.Weight.stride)? vtxArry.Weight.stride:szgldata(vtxArry.Weight.size, vtxArry.Weight.type);
            n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.Weight.size, vtxArry.Weight.type)));
        }
        for (i = 0; i < 2; i++) {
            if (vtxArry.GenAttrib[i].enable && vtxArry.GenAttrib[i].ptr) {
                cbElem = (vtxArry.GenAttrib[i].stride)? vtxArry.GenAttrib[i].stride:szgldata(vtxArry.GenAttrib[i].size, vtxArry.GenAttrib[i].type);
                n += ALIGNED((cbElem*(end - start) + szgldata(vtxArry.GenAttrib[i].size, vtxArry.GenAttrib[i].type)));
            }
        }
    }
    n >>= 2;
    if ((mdata[0] + n) >= MAX_DATA) {
        DPRINTF("FIFO flush %x %x %06x %06x %06x", start, end, (mdata[0] + n), mdata[0], n);
        FIFO_FLUSH(1);
    }
}
static void PushVertexArray(int start, int end) 
{
    int i, cbElem;
    if (Interleaved.enable && Interleaved.ptr) {
        cbElem = (Interleaved.stride)? Interleaved.stride:Interleaved.size;
        fifoAddData(0, (uint32_t)(Interleaved.ptr+(start*cbElem)), (cbElem*(end - start) + Interleaved.size));
        Interleaved.enable = 0;
    }
    else {
        if (vtxArry.Color.enable && vtxArry.Color.ptr) {
            cbElem = (vtxArry.Color.stride)? vtxArry.Color.stride:szgldata(vtxArry.Color.size, vtxArry.Color.type);
            fifoAddData(0, (uint32_t)(vtxArry.Color.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.Color.size, vtxArry.Color.type)));
        }
        if (vtxArry.EdgeFlag.enable && vtxArry.EdgeFlag.ptr) {
            cbElem = (vtxArry.EdgeFlag.stride)? vtxArry.EdgeFlag.stride:szgldata(vtxArry.EdgeFlag.size, vtxArry.EdgeFlag.type);
            fifoAddData(0, (uint32_t)(vtxArry.EdgeFlag.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.EdgeFlag.size, vtxArry.EdgeFlag.type)));
        }
        if (vtxArry.Index.enable && vtxArry.Index.ptr) {
            cbElem = (vtxArry.Index.stride)? vtxArry.Index.stride:szgldata(vtxArry.Index.size, vtxArry.Index.type);
            fifoAddData(0, (uint32_t)(vtxArry.Index.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.Index.size, vtxArry.Index.type)));
        }
        if (vtxArry.Normal.enable && vtxArry.Normal.ptr) {
            cbElem = (vtxArry.Normal.stride)? vtxArry.Normal.stride:szgldata(vtxArry.Normal.size, vtxArry.Normal.type);
            fifoAddData(0, (uint32_t)(vtxArry.Normal.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.Normal.size, vtxArry.Normal.type)));
        }
        for (i = 0; i < MAX_TEXUNIT; i++) {
            if (vtxArry.TexCoord[i].enable && vtxArry.TexCoord[i].ptr) {
                cbElem = (vtxArry.TexCoord[i].stride)? vtxArry.TexCoord[i].stride:szgldata(vtxArry.TexCoord[i].size, vtxArry.TexCoord[i].type);
                fifoAddData(0, (uint32_t)(vtxArry.TexCoord[i].ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.TexCoord[i].size, vtxArry.TexCoord[i].type)));
            }
        }
        if (vtxArry.Vertex.enable && vtxArry.Vertex.ptr) {
            cbElem = (vtxArry.Vertex.stride)? vtxArry.Vertex.stride:szgldata(vtxArry.Vertex.size, vtxArry.Vertex.type);
            fifoAddData(0, (uint32_t)(vtxArry.Vertex.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.Vertex.size, vtxArry.Vertex.type)));
        }
        if (vtxArry.SecondaryColor.enable && vtxArry.SecondaryColor.ptr) {
            cbElem = (vtxArry.SecondaryColor.stride)? vtxArry.SecondaryColor.stride:szgldata(vtxArry.SecondaryColor.size, vtxArry.SecondaryColor.type);
            fifoAddData(0, (uint32_t)(vtxArry.SecondaryColor.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.SecondaryColor.size, vtxArry.SecondaryColor.type)));
        }
        if (vtxArry.FogCoord.enable && vtxArry.FogCoord.ptr) {
            cbElem = (vtxArry.FogCoord.stride)? vtxArry.FogCoord.stride:szgldata(vtxArry.FogCoord.size, vtxArry.FogCoord.type);
            fifoAddData(0, (uint32_t)(vtxArry.FogCoord.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.FogCoord.size, vtxArry.FogCoord.type)));
        }
        if (vtxArry.Weight.enable && vtxArry.Weight.ptr) {
            cbElem = (vtxArry.Weight.stride)? vtxArry.Weight.stride:szgldata(vtxArry.Weight.size, vtxArry.Weight.type);
            fifoAddData(0, (uint32_t)(vtxArry.Weight.ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.Weight.size, vtxArry.Weight.type)));
        }
        for (i = 0; i < 2; i++) {
            if (vtxArry.GenAttrib[i].enable && vtxArry.GenAttrib[i].ptr) {
                cbElem = (vtxArry.GenAttrib[i].stride)? vtxArry.GenAttrib[i].stride:szgldata(vtxArry.GenAttrib[i].size, vtxArry.GenAttrib[i].type);
                fifoAddData(0, (uint32_t)(vtxArry.GenAttrib[i].ptr+(start*cbElem)), (cbElem*(end - start) + szgldata(vtxArry.GenAttrib[i].size, vtxArry.GenAttrib[i].type)));
            }
        }
    }
    //DPRINTF("PushVertexArray() %04x %04x", start, end);
}
static void InitClientStates(void) 
{
    memset(&vtxArry, 0, sizeof(vtxArry));
    memset(&Interleaved, 0, sizeof(vtxarry_t));
    pixPackBuf = 0; pixUnpackBuf = 0;
    szPackWidth = 0; szUnpackWidth = 0;
    szPackHeight = 0; szUnpackHeight = 0;
    queryBuf = 0;
}

#define OHST_DMESG(fmt, ...) \
    do { void PT_CALL glDebugMessageInsertARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5); \
        FILE *f = fopen("NUL", "w"); int c = fprintf(f, fmt, ##__VA_ARGS__); fclose(f); \
        char *str = HeapAlloc(GetProcessHeap(), 0, ALIGNED((c+1))); \
        wsprintf(str, fmt, ##__VA_ARGS__); \
        glDebugMessageInsertARB(GL_DEBUG_SOURCE_OTHER, GL_DEBUG_TYPE_OTHER, GL_DEBUG_SEVERITY_LOW, -1, (c+1), (uint32_t)str); \
        HeapFree(GetProcessHeap(), 0, str); \
    } while(0)

static FILE * opt_fopen(void)
{
#define XSTRCFG "wrapgl32.ext"
    char cfg_path[MAX_PATH];
    FILE *ret = NULL;
    int i;
    GetModuleFileName(NULL, cfg_path, MAX_PATH);
    i = strnlen(cfg_path, MAX_PATH);
    while (i && cfg_path[i] != '\\') i--;
    if (cfg_path[i] == '\\' && (MAX_PATH - i - 1) > sizeof(XSTRCFG)) {
        strcpy(&cfg_path[++i], XSTRCFG);
        ret = fopen(cfg_path, "r");
    }
    return ret;
}

static void fltrxstr(const char *xstr, size_t len, const char *bless)
{
#define MAX_XSTR 128
    char *str = (char *)xstr, *tmp = (char *)&fbtm[(MGLFBT_SIZE - (3*PAGE_SIZE)) >> 2];
    FILE *f = opt_fopen();
    len = (len > (3*PAGE_SIZE))? (3*PAGE_SIZE):len;
    *tmp = ' ';
    *(tmp + 1) = '\0';
    if (f) {
        char *stok, line[MAX_XSTR];
        size_t slen;
        stok = strtok(str, " ");
        while (stok) {
            size_t xlen = strnlen(stok, MAX_XSTR);
            int fltr = 0;
            while(fgets(line, MAX_XSTR, f)) {
                slen = strnlen(line, MAX_XSTR) - 1;
                if ((slen == xlen) && !strncmp(stok, line, xlen)) {
                    fltr = 1;
                    OHST_DMESG("..ignoring %s", stok);
                    break;
                }
            }
            if (!fltr) {
                memcpy(tmp, stok, xlen);
                tmp += xlen;
                *(tmp++) = ' ';
            }
            fseek(f, 0, SEEK_SET);
            stok = strtok(NULL, " ");
        }
        while(fgets(line, MAX_XSTR, f)) {
            slen = strnlen(line, MAX_XSTR) - 1;
            if (bless && !memcmp(line, bless, strlen(bless))) {
                line[slen] = '\0';
                OHST_DMESG("..blessing %s", &line[1]);
                memcpy(tmp, &line[1], slen - 1);
                tmp += slen - 1;
                *(tmp++) = ' ';
            }
        }
        *(--tmp) = '\0';
        fclose(f);
        strncpy(str, (char *)&fbtm[(MGLFBT_SIZE - (3*PAGE_SIZE)) >> 2], len);
    }
}
struct mglOptions {
    int bufoAcc;
    int dispTimerMS;
    int swapInt;
    int useMSAA;
    int useSRGB;
    int useZERO;
    int bltFlip;
    int scalerOff;
    int vsyncOff;
    int xstrYear;
};
static int swapCur, swapFps, texClampFix;
static int parse_value(const char *str, const char *tok, int *val)
{
    int ret = (memcmp(str, tok, strlen(tok)))? 0:1;
    if (ret)
        *val = strtol(str + strlen(tok), 0, 10);
    return ret;
}
static int display_device_supported(void)
{
    DISPLAY_DEVICE dd = { .cb = sizeof(DISPLAY_DEVICE) };
    const char vidstr[] = "QEMU Bochs";
    return (EnumDisplayDevices(NULL, 0, &dd, 0) &&
        !memcmp(dd.DeviceString, vidstr, strlen(vidstr)))? 1:0;
}
static int ctx0_quirks(void)
{
    const char *use_ctx0[] = {
        "DX7HRDisplay",
        "DX7HRTnLDisplay",
        0,
    };
    int i;
    for (i = 0; use_ctx0[i]; i++) {
        if (GetModuleHandle(use_ctx0[i]))
            break;
    }
    return (use_ctx0[i])? 1:0;
}
static void parse_options(struct mglOptions *opt)
{
    FILE *f = opt_fopen();
    memset(opt, 0, sizeof(struct mglOptions));
    /* Sync host color cursor only for Bochs SVGA */
    swapCur = display_device_supported();
    opt->useZERO = ctx0_quirks() << 5;
    if (f) {
        char line[MAX_XSTR];
        int i, v;
        while(fgets(line, MAX_XSTR, f)) {
            i = parse_value(line, "DispTimerMS,", &v);
            opt->dispTimerMS = (i == 1)? (0x8000U | (v & 0x7FFFU)):opt->dispTimerMS;
            i = parse_value(line, "SwapInterval,", &v);
            opt->swapInt = (i == 1)? (v & 0x03U):opt->swapInt;
            i = parse_value(line, "BufOAccelEN,", &v);
            opt->bufoAcc = ((i == 1) && v)? 1:opt->bufoAcc;
            i = parse_value(line, "ContextMSAA,", &v);
            opt->useMSAA = ((i == 1) && v)? ((v & 0x03U) << 2):opt->useMSAA;
            i = parse_value(line, "ContextSRGB,", &v);
            opt->useSRGB = ((i == 1) && v)? 1:opt->useSRGB;
            i = parse_value(line, "CtxZeroQuirksOff,", &v);
            opt->useZERO = ((i == 1) && v)? 0:opt->useZERO;
            i = parse_value(line, "ScalerBltFlip,", &v);
            opt->bltFlip = ((i == 1) && v)? 0x12U:opt->bltFlip;
            i = parse_value(line, "RenderScalerOff,", &v);
            opt->scalerOff = ((i == 1) && v)? 2:opt->scalerOff;
            i = parse_value(line, "ContextVsyncOff,", &v);
            opt->vsyncOff = ((i == 1) && v)? 1:opt->vsyncOff;
            i = parse_value(line, "ExtensionsYear,", &v);
            opt->xstrYear = (i == 1)? v:opt->xstrYear;
            i = parse_value(line, "ConformantTexClampOff,", &v);
            texClampFix = ((i == 1) && v)? 1:texClampFix;
            i = parse_value(line, "CursorSyncOff,", &v);
            swapCur = ((i == 1) && v)? 0:swapCur;
            i = parse_value(line, "FpsLimit,", &v);
            swapFps = (i == 1)? (v & 0x7FU):swapFps;
        }
        fclose(f);
    }
}

#define FIFO_EN 1
#define FIFO_GLFUNC(_func,_nargs) \
    if (FIFO_EN && ((mfifo[0] + (_nargs + 1)) < MAX_FIFO) && (mdata[0] < MAX_DATA))  \
        fifoAddEntry(&pt[1], _func, _nargs); \
    else *pt0 = _func \


/* Start - generated by mstub_genfunc */

void PT_CALL glAccum(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glAccum, 2);
}
void PT_CALL glAccumxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAccumxOES;
}
void PT_CALL glAcquireKeyedMutexWin32EXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAcquireKeyedMutexWin32EXT;
}
void PT_CALL glActiveProgramEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glActiveProgramEXT;
}
void PT_CALL glActiveShaderProgram(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glActiveShaderProgram;
}
void PT_CALL glActiveStencilFaceEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glActiveStencilFaceEXT, 1);
}
void PT_CALL glActiveTexture(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glActiveTexture, 1);
}
void PT_CALL glActiveTextureARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glActiveTextureARB, 1);
}
void PT_CALL glActiveVaryingNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glActiveVaryingNV;
}
void PT_CALL glAlphaFragmentOp1ATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glAlphaFragmentOp1ATI, 6);
}
void PT_CALL glAlphaFragmentOp2ATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glAlphaFragmentOp2ATI, 9);
}
void PT_CALL glAlphaFragmentOp3ATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glAlphaFragmentOp3ATI, 12);
}
void PT_CALL glAlphaFunc(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glAlphaFunc, 2);
}
void PT_CALL glAlphaFuncxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAlphaFuncxOES;
}
void PT_CALL glAlphaToCoverageDitherControlNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAlphaToCoverageDitherControlNV;
}
void PT_CALL glApplyFramebufferAttachmentCMAAINTEL(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glApplyFramebufferAttachmentCMAAINTEL;
}
void PT_CALL glApplyTextureEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glApplyTextureEXT;
}
uint32_t PT_CALL glAreProgramsResidentNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t ret;
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAreProgramsResidentNV;
    ret = *pt0;
    if (ret == 0)
        fifoOutData(0, arg2, (arg0 * sizeof(uint32_t)));
    return ret;
}
uint32_t PT_CALL glAreTexturesResident(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t ret;
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAreTexturesResident;
    ret = *pt0;
    if (ret == 0)
        fifoOutData(0, arg2, (arg0 * sizeof(uint32_t)));
    return ret;
}
uint32_t PT_CALL glAreTexturesResidentEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t ret;
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAreTexturesResidentEXT;
    ret = *pt0;
    if (ret == 0)
        fifoOutData(0, arg2, (arg0 * sizeof(uint32_t)));
    return ret;
}
void PT_CALL glArrayElement(uint32_t arg0) {
    PrepVertexArray(arg0, arg0, 0);
    PushVertexArray(arg0, arg0);
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glArrayElement, 1);
}
void PT_CALL glArrayElementEXT(uint32_t arg0) {
    PrepVertexArray(arg0, arg0, 0);
    PushVertexArray(arg0, arg0);
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glArrayElementEXT, 1);
}
void PT_CALL glArrayObjectATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glArrayObjectATI;
}
void PT_CALL glAsyncMarkerSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAsyncMarkerSGIX;
}
void PT_CALL glAttachObjectARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glAttachObjectARB;
}
void PT_CALL glAttachShader(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glAttachShader, 2);
}
void PT_CALL glBegin(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBegin, 1);
}
void PT_CALL glBeginConditionalRender(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginConditionalRender;
}
void PT_CALL glBeginConditionalRenderNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginConditionalRenderNV;
}
void PT_CALL glBeginConditionalRenderNVX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginConditionalRenderNVX;
}
void PT_CALL glBeginFragmentShaderATI(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBeginFragmentShaderATI, 0);
}
void PT_CALL glBeginOcclusionQueryNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBeginOcclusionQueryNV, 1);
}
void PT_CALL glBeginPerfMonitorAMD(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginPerfMonitorAMD;
}
void PT_CALL glBeginPerfQueryINTEL(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginPerfQueryINTEL;
}
void PT_CALL glBeginQuery(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBeginQuery, 2);
}
void PT_CALL glBeginQueryARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBeginQueryARB, 2);
}
void PT_CALL glBeginQueryIndexed(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBeginQueryIndexed, 3);
}
void PT_CALL glBeginTransformFeedback(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBeginTransformFeedback, 1);
}
void PT_CALL glBeginTransformFeedbackEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBeginTransformFeedbackEXT, 1);
}
void PT_CALL glBeginTransformFeedbackNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginTransformFeedbackNV;
}
void PT_CALL glBeginVertexShaderEXT(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginVertexShaderEXT;
}
void PT_CALL glBeginVideoCaptureNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBeginVideoCaptureNV;
}
void PT_CALL glBindAttribLocation(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED((strlen((char *)arg2) + 1)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindAttribLocation, 3);
}
void PT_CALL glBindAttribLocationARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED((strlen((char *)arg2) + 1)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindAttribLocationARB, 3);
}
void PT_CALL glBindBuffer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindBuffer, 2);
    pixPackBuf = (arg0 == GL_PIXEL_PACK_BUFFER)? arg1:pixPackBuf;
    pixUnpackBuf = (arg0 == GL_PIXEL_UNPACK_BUFFER)? arg1:pixUnpackBuf;
    queryBuf = (arg0 == GL_QUERY_BUFFER)? arg1:queryBuf;
    vtxArry.arrayBuf = (arg0 == GL_ARRAY_BUFFER)? arg1:vtxArry.arrayBuf;
    vtxArry.elemArryBuf = (arg0 == GL_ELEMENT_ARRAY_BUFFER)? arg1:vtxArry.elemArryBuf;
    if ((vtxArry.vao == 0) && (arg0 == GL_ARRAY_BUFFER) && (arg1 == 0))
        vtxarry_ptr_reset();
    if (vtxArry.vao) {
        vtxArry.arrayBuf = vtxArry.vao;
        vtxArry.elemArryBuf = vtxArry.vao;
    }
}
void PT_CALL glBindBufferARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindBufferARB, 2);
    pixPackBuf = (arg0 == GL_PIXEL_PACK_BUFFER)? arg1:pixPackBuf;
    pixUnpackBuf = (arg0 == GL_PIXEL_UNPACK_BUFFER)? arg1:pixUnpackBuf;
    queryBuf = (arg0 == GL_QUERY_BUFFER)? arg1:queryBuf;
    vtxArry.arrayBuf = (arg0 == GL_ARRAY_BUFFER)? arg1:vtxArry.arrayBuf;
    vtxArry.elemArryBuf = (arg0 == GL_ELEMENT_ARRAY_BUFFER)? arg1:vtxArry.elemArryBuf;
    if ((vtxArry.vao == 0) && (arg0 == GL_ARRAY_BUFFER) && (arg1 == 0))
        vtxarry_ptr_reset();
    if (vtxArry.vao) {
        vtxArry.arrayBuf = vtxArry.vao;
        vtxArry.elemArryBuf = vtxArry.vao;
    }
}
void PT_CALL glBindBufferBase(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindBufferBase, 3);
}
void PT_CALL glBindBufferBaseEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindBufferBaseEXT, 3);
}
void PT_CALL glBindBufferBaseNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindBufferBaseNV;
}
void PT_CALL glBindBufferOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindBufferOffsetEXT, 4);
}
void PT_CALL glBindBufferOffsetNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindBufferOffsetNV;
}
void PT_CALL glBindBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindBufferRange, 5);
}
void PT_CALL glBindBufferRangeEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindBufferRangeEXT, 5);
}
void PT_CALL glBindBufferRangeNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindBufferRangeNV;
}
void PT_CALL glBindBuffersBase(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindBuffersBase;
}
void PT_CALL glBindBuffersRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindBuffersRange;
}
void PT_CALL glBindFragDataLocation(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED((strlen((char *)arg2) + 1)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindFragDataLocation, 3);
}
void PT_CALL glBindFragDataLocationEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED((strlen((char *)arg2) + 1)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindFragDataLocationEXT, 3);
}
void PT_CALL glBindFragDataLocationIndexed(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg2, ALIGNED((strlen((char *)arg3) + 1)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindFragDataLocationIndexed, 4);
}
void PT_CALL glBindFragmentShaderATI(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindFragmentShaderATI, 1);
}
void PT_CALL glBindFramebuffer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindFramebuffer, 2);
}
void PT_CALL glBindFramebufferEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindFramebufferEXT, 2);
}
void PT_CALL glBindImageTexture(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindImageTexture, 7);
}
void PT_CALL glBindImageTextureEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindImageTextureEXT, 7);
}
void PT_CALL glBindImageTextures(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindImageTextures, 3);
}
void PT_CALL glBindLightParameterEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindLightParameterEXT;
}
void PT_CALL glBindMaterialParameterEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindMaterialParameterEXT;
}
void PT_CALL glBindMultiTextureEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindMultiTextureEXT;
}
void PT_CALL glBindParameterEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindParameterEXT;
}
void PT_CALL glBindProgramARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindProgramARB, 2);
}
void PT_CALL glBindProgramNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindProgramNV, 2);
}
void PT_CALL glBindProgramPipeline(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindProgramPipeline;
}
void PT_CALL glBindRenderbuffer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindRenderbuffer, 2);
}
void PT_CALL glBindRenderbufferEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindRenderbufferEXT, 2);
}
void PT_CALL glBindSampler(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindSampler, 2);
}
void PT_CALL glBindSamplers(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindSamplers, 3);
}
void PT_CALL glBindShadingRateImageNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindShadingRateImageNV;
}
void PT_CALL glBindTexGenParameterEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindTexGenParameterEXT;
}
void PT_CALL glBindTexture(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindTexture, 2);
}
void PT_CALL glBindTextureEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindTextureEXT, 2);
}
void PT_CALL glBindTextureUnit(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindTextureUnit;
}
void PT_CALL glBindTextureUnitParameterEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindTextureUnitParameterEXT;
}
void PT_CALL glBindTextures(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindTextures;
}
void PT_CALL glBindTransformFeedback(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindTransformFeedback;
}
void PT_CALL glBindTransformFeedbackNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindTransformFeedbackNV;
}
void PT_CALL glBindVertexArray(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBindVertexArray, 1);
    vtxArry.vao = arg0;
    vtxArry.arrayBuf = vtxArry.vao;
    vtxArry.elemArryBuf = vtxArry.vao;
}
void PT_CALL glBindVertexArrayAPPLE(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindVertexArrayAPPLE;
}
void PT_CALL glBindVertexBuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindVertexBuffer;
}
void PT_CALL glBindVertexBuffers(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindVertexBuffers;
}
void PT_CALL glBindVertexShaderEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindVertexShaderEXT;
}
void PT_CALL glBindVideoCaptureStreamBufferNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindVideoCaptureStreamBufferNV;
}
void PT_CALL glBindVideoCaptureStreamTextureNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBindVideoCaptureStreamTextureNV;
}
void PT_CALL glBinormal3bEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3bEXT;
}
void PT_CALL glBinormal3bvEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3bvEXT;
}
void PT_CALL glBinormal3dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3dEXT;
}
void PT_CALL glBinormal3dvEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3dvEXT;
}
void PT_CALL glBinormal3fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3fEXT;
}
void PT_CALL glBinormal3fvEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3fvEXT;
}
void PT_CALL glBinormal3iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3iEXT;
}
void PT_CALL glBinormal3ivEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3ivEXT;
}
void PT_CALL glBinormal3sEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3sEXT;
}
void PT_CALL glBinormal3svEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormal3svEXT;
}
void PT_CALL glBinormalPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBinormalPointerEXT;
}
void PT_CALL glBitmap(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (arg6 && (pixUnpackBuf == 0)) {
        uint32_t szBmp = ((szUnpackWidth == 0)? arg0:szUnpackWidth) * arg1;
        uint32_t *bmpPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szBmp)) >> 2];
        FBTMMCPY(bmpPtr, (char *)arg6, szBmp);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBitmap;
}
void PT_CALL glBitmapxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBitmapxOES;
}
void PT_CALL glBlendBarrierKHR(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendBarrierKHR;
}
void PT_CALL glBlendBarrierNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendBarrierNV;
}
void PT_CALL glBlendColor(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendColor, 4);
}
void PT_CALL glBlendColorEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendColorEXT, 4);
}
void PT_CALL glBlendColorxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendColorxOES;
}
void PT_CALL glBlendEquation(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquation, 1);
}
void PT_CALL glBlendEquationEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquationEXT, 1);
}
void PT_CALL glBlendEquationIndexedAMD(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendEquationIndexedAMD;
}
void PT_CALL glBlendEquationSeparate(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquationSeparate, 2);
}
void PT_CALL glBlendEquationSeparateEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquationSeparateEXT, 2);
}
void PT_CALL glBlendEquationSeparateIndexedAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendEquationSeparateIndexedAMD;
}
void PT_CALL glBlendEquationSeparatei(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquationSeparatei, 3);
}
void PT_CALL glBlendEquationSeparateiARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquationSeparateiARB, 3);
}
void PT_CALL glBlendEquationi(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquationi, 2);
}
void PT_CALL glBlendEquationiARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendEquationiARB, 2);
}
void PT_CALL glBlendFunc(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendFunc, 2);
}
void PT_CALL glBlendFuncIndexedAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendFuncIndexedAMD;
}
void PT_CALL glBlendFuncSeparate(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendFuncSeparate, 4);
}
void PT_CALL glBlendFuncSeparateEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendFuncSeparateEXT, 4);
}
void PT_CALL glBlendFuncSeparateINGR(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendFuncSeparateINGR;
}
void PT_CALL glBlendFuncSeparateIndexedAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendFuncSeparateIndexedAMD;
}
void PT_CALL glBlendFuncSeparatei(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendFuncSeparatei, 5);
}
void PT_CALL glBlendFuncSeparateiARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendFuncSeparateiARB, 5);
}
void PT_CALL glBlendFunci(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendFunci, 3);
}
void PT_CALL glBlendFunciARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlendFunciARB, 3);
}
void PT_CALL glBlendParameteriNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlendParameteriNV;
}
void PT_CALL glBlitFramebuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlitFramebuffer, 10);
}
void PT_CALL glBlitFramebufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBlitFramebufferEXT, 10);
}
void PT_CALL glBlitNamedFramebuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBlitNamedFramebuffer;
}
void PT_CALL glBufferAddressRangeNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBufferAddressRangeNV;
}
void PT_CALL glBufferAttachMemoryNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBufferAttachMemoryNV;
}
void PT_CALL glBufferData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (arg2)
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(arg1)) >> 2], (unsigned char *)arg2, arg1);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; /**pt0 = FEnum_glBufferData;*/
    if (!arg2) { FIFO_GLFUNC(FEnum_glBufferData, 4); }
    else *pt0 = FEnum_glBufferData;
}
void PT_CALL glBufferDataARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (arg2)
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(arg1)) >> 2], (unsigned char *)arg2, arg1);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; /**pt0 = FEnum_glBufferDataARB;*/
    if (!arg2) { FIFO_GLFUNC(FEnum_glBufferDataARB, 4); }
    else *pt0 = FEnum_glBufferDataARB;
}
void PT_CALL glBufferPageCommitmentARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBufferPageCommitmentARB;
}
void PT_CALL glBufferParameteriAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glBufferParameteriAPPLE, 3);
}
void PT_CALL glBufferStorage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (arg2)
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(arg1)) >> 2], (unsigned char *)arg2, arg1);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; /**pt0 = FEnum_glBufferStorage;*/
    if (!arg2) { FIFO_GLFUNC(FEnum_glBufferStorage, 4); }
    else *pt0 = FEnum_glBufferStorage;
}
void PT_CALL glBufferStorageExternalEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBufferStorageExternalEXT;
}
void PT_CALL glBufferStorageMemEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBufferStorageMemEXT;
}
void PT_CALL glBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t offst = arg1, remain = arg2, ptr = arg3;
    while(remain) {
        uint32_t chunk = (remain > (MGLFBT_SIZE - PAGE_SIZE))? (MGLFBT_SIZE - PAGE_SIZE):remain;
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(chunk)) >> 2], (unsigned char *)ptr, chunk);
        pt[1] = arg0; pt[2] = offst; pt[3] = chunk; pt[4] = ptr;
        pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBufferSubData;
        offst += chunk;
        ptr += chunk;
        remain -= chunk;
    }
}
void PT_CALL glBufferSubDataARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t offst = arg1, remain = arg2, ptr = arg3;
    while(remain) {
        uint32_t chunk = (remain > (MGLFBT_SIZE - PAGE_SIZE))? (MGLFBT_SIZE - PAGE_SIZE):remain;
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(chunk)) >> 2], (unsigned char *)ptr, chunk);
        pt[1] = arg0; pt[2] = offst; pt[3] = chunk; pt[4] = ptr;
        pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glBufferSubDataARB;
        offst += chunk;
        ptr += chunk;
        remain -= chunk;
    }
}
void PT_CALL glCallCommandListNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCallCommandListNV;
}
void PT_CALL glCallList(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCallList, 1);
}
void PT_CALL glCallLists(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg0*szgldata(0, arg1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCallLists, 3);
}
uint32_t PT_CALL glCheckFramebufferStatus(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCheckFramebufferStatus;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glCheckFramebufferStatusEXT(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCheckFramebufferStatusEXT;
    ret = *pt0;
    return ret;
}
void PT_CALL glCheckNamedFramebufferStatus(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCheckNamedFramebufferStatus;
}
void PT_CALL glCheckNamedFramebufferStatusEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCheckNamedFramebufferStatusEXT;
}
void PT_CALL glClampColor(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClampColor, 2);
}
void PT_CALL glClampColorARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClampColorARB, 2);
}
void PT_CALL glClear(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClear, 1);
}
void PT_CALL glClearAccum(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearAccum, 4);
}
void PT_CALL glClearAccumxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearAccumxOES;
}
void PT_CALL glClearBufferData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    if (arg4)
        fifoAddData(0, arg4, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearBufferData, 5);
}
void PT_CALL glClearBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (arg6)
        fifoAddData(0, arg6, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearBufferSubData, 7);
}
void PT_CALL glClearBufferfi(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearBufferfi, 4);
}
void PT_CALL glClearBufferfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, (arg0 == GL_COLOR)? 4*sizeof(float):sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearBufferfv, 3);
}
void PT_CALL glClearBufferiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, (arg0 == GL_COLOR)? 4*sizeof(int):sizeof(int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearBufferiv, 3);
}
void PT_CALL glClearBufferuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, (arg0 == GL_COLOR)? 4*sizeof(unsigned int):sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearBufferuiv, 3);
}
void PT_CALL glClearColor(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearColor, 4);
}
void PT_CALL glClearColorIiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearColorIiEXT;
}
void PT_CALL glClearColorIuiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearColorIuiEXT;
}
void PT_CALL glClearColorxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearColorxOES;
}
void PT_CALL glClearDepth(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearDepth, 2);
}
void PT_CALL glClearDepthdNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearDepthdNV;
}
void PT_CALL glClearDepthf(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearDepthf, 1);
}
void PT_CALL glClearDepthfOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearDepthfOES;
}
void PT_CALL glClearDepthxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearDepthxOES;
}
void PT_CALL glClearIndex(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearIndex, 1);
}
void PT_CALL glClearNamedBufferData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    if (arg4)
        fifoAddData(0, arg4, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearNamedBufferData, 5);
}
void PT_CALL glClearNamedBufferDataEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    if (arg4)
        fifoAddData(0, arg4, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearNamedBufferDataEXT, 5);
}
void PT_CALL glClearNamedBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (arg6)
        fifoAddData(0, arg6, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearNamedBufferSubData, 7);
}
void PT_CALL glClearNamedBufferSubDataEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (arg6)
        fifoAddData(0, arg6, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearNamedBufferSubDataEXT, 7);
}
void PT_CALL glClearNamedFramebufferfi(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearNamedFramebufferfi;
}
void PT_CALL glClearNamedFramebufferfv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearNamedFramebufferfv;
}
void PT_CALL glClearNamedFramebufferiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearNamedFramebufferiv;
}
void PT_CALL glClearNamedFramebufferuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClearNamedFramebufferuiv;
}
void PT_CALL glClearStencil(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearStencil, 1);
}
void PT_CALL glClearTexImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    if (arg4)
        fifoAddData(0, arg4, ALIGNED(szgldata(arg2, arg3)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearTexImage, 5);
}
void PT_CALL glClearTexSubImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    if (arg10)
        fifoAddData(0, arg10, ALIGNED(szgldata(arg8, arg9)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClearTexSubImage, 11);
}
void PT_CALL glClientActiveTexture(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClientActiveTexture, 1);
    if ((arg0 & 0xFFE0U) == GL_TEXTURE0)
        vtxArry.texUnit = arg0 & (MAX_TEXUNIT - 1);
}
void PT_CALL glClientActiveTextureARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClientActiveTextureARB, 1);
    if ((arg0 & 0xFFE0U) == GL_TEXTURE0)
        vtxArry.texUnit = arg0 & (MAX_TEXUNIT - 1);
}
void PT_CALL glClientActiveVertexStreamATI(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClientActiveVertexStreamATI;
}
void PT_CALL glClientAttribDefaultEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClientAttribDefaultEXT;
}
uint32_t PT_CALL glClientWaitSync(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClientWaitSync;
    ret = *pt0;
    return ret;
}
void PT_CALL glClipControl(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClipControl, 2);
}
void PT_CALL glClipPlane(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glClipPlane, 2);
}
void PT_CALL glClipPlanefOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClipPlanefOES;
}
void PT_CALL glClipPlanexOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glClipPlanexOES;
}
void PT_CALL glColor3b(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3b, 3);
}
void PT_CALL glColor3bv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3bv, 1);
}
void PT_CALL glColor3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3d, 6);
}
void PT_CALL glColor3dv(uint32_t arg0) {
    fifoAddData(0, arg0, 3*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3dv, 1);
}
void PT_CALL glColor3f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3f, 3);
}
void PT_CALL glColor3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor3fVertex3fSUN;
}
void PT_CALL glColor3fVertex3fvSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor3fVertex3fvSUN;
}
void PT_CALL glColor3fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3fv, 1);
}
void PT_CALL glColor3hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor3hNV;
}
void PT_CALL glColor3hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor3hvNV;
}
void PT_CALL glColor3i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3i, 3);
}
void PT_CALL glColor3iv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3iv, 1);
}
void PT_CALL glColor3s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3s, 3);
}
void PT_CALL glColor3sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3sv, 1);
}
void PT_CALL glColor3ub(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3ub, 3);
}
void PT_CALL glColor3ubv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3ubv, 1);
}
void PT_CALL glColor3ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3ui, 3);
}
void PT_CALL glColor3uiv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3uiv, 1);
}
void PT_CALL glColor3us(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3us, 3);
}
void PT_CALL glColor3usv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor3usv, 1);
}
void PT_CALL glColor3xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor3xOES;
}
void PT_CALL glColor3xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor3xvOES;
}
void PT_CALL glColor4b(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4b, 4);
}
void PT_CALL glColor4bv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(4*sizeof(char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4bv, 1);
}
void PT_CALL glColor4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4d, 8);
}
void PT_CALL glColor4dv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4dv, 1);
}
void PT_CALL glColor4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4f, 4);
}
void PT_CALL glColor4fNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4fNormal3fVertex3fSUN;
}
void PT_CALL glColor4fNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4fNormal3fVertex3fvSUN;
}
void PT_CALL glColor4fv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4fv, 1);
}
void PT_CALL glColor4hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4hNV;
}
void PT_CALL glColor4hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4hvNV;
}
void PT_CALL glColor4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4i, 4);
}
void PT_CALL glColor4iv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4iv, 1);
}
void PT_CALL glColor4s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4s, 4);
}
void PT_CALL glColor4sv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(short));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4sv, 1);
}
void PT_CALL glColor4ub(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4ub, 4);
}
void PT_CALL glColor4ubVertex2fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4ubVertex2fSUN;
}
void PT_CALL glColor4ubVertex2fvSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4ubVertex2fvSUN;
}
void PT_CALL glColor4ubVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4ubVertex3fSUN;
}
void PT_CALL glColor4ubVertex3fvSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4ubVertex3fvSUN;
}
void PT_CALL glColor4ubv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(4*sizeof(unsigned char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4ubv, 1);
}
void PT_CALL glColor4ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4ui, 4);
}
void PT_CALL glColor4uiv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(unsigned int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4uiv, 1);
}
void PT_CALL glColor4us(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4us, 4);
}
void PT_CALL glColor4usv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(unsigned short));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColor4usv, 1);
}
void PT_CALL glColor4xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4xOES;
}
void PT_CALL glColor4xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColor4xvOES;
}
void PT_CALL glColorFormatNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorFormatNV;
}
void PT_CALL glColorFragmentOp1ATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorFragmentOp1ATI, 7);
}
void PT_CALL glColorFragmentOp2ATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorFragmentOp2ATI, 10);
}
void PT_CALL glColorFragmentOp3ATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorFragmentOp3ATI, 13);
}
void PT_CALL glColorMask(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorMask, 4);
}
void PT_CALL glColorMaskIndexedEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorMaskIndexedEXT, 5);
}
void PT_CALL glColorMaski(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorMaski, 5);
}
void PT_CALL glColorMaterial(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorMaterial, 2);
}
void PT_CALL glColorP3ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorP3ui;
}
void PT_CALL glColorP3uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorP3uiv;
}
void PT_CALL glColorP4ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorP4ui;
}
void PT_CALL glColorP4uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorP4uiv;
}
void PT_CALL glColorPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorPointer, 4);
    vtxarry_init(&vtxArry.Color, arg0, arg1, arg2, (void *)arg3);
}
void PT_CALL glColorPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorPointerEXT, 5);
    vtxarry_init(&vtxArry.Color, arg0, arg1, arg2, (void *)arg4);
}
void PT_CALL glColorPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorPointerListIBM;
}
void PT_CALL glColorPointervINTEL(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorPointervINTEL;
}
void PT_CALL glColorSubTable(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(0, arg5, ALIGNED(arg2*szgldata(arg3, arg4)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorSubTable, 6);
}
void PT_CALL glColorSubTableEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(0, arg5, ALIGNED(arg2*szgldata(arg3, arg4)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorSubTableEXT, 6);
}
void PT_CALL glColorTable(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(0, arg5, ALIGNED(arg2*szgldata(arg3, arg4)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorTable, 6);
}
void PT_CALL glColorTableEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(0, arg5, ALIGNED(arg2*szgldata(arg3, arg4)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorTableEXT, 6);
}
void PT_CALL glColorTableParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorTableParameterfv, 3);
}
void PT_CALL glColorTableParameterfvSGI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorTableParameterfvSGI;
}
void PT_CALL glColorTableParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glColorTableParameteriv, 3);
}
void PT_CALL glColorTableParameterivSGI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorTableParameterivSGI;
}
void PT_CALL glColorTableSGI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glColorTableSGI;
}
void PT_CALL glCombinerInputNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    RENDERER_VALID("NVIDIA ");
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCombinerInputNV, 6);
}
void PT_CALL glCombinerOutputNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    RENDERER_VALID("NVIDIA ");
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCombinerOutputNV, 10);
}
void PT_CALL glCombinerParameterfNV(uint32_t arg0, uint32_t arg1) {
    RENDERER_VALID("NVIDIA ");
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCombinerParameterfNV, 2);
}
void PT_CALL glCombinerParameterfvNV(uint32_t arg0, uint32_t arg1) {
    RENDERER_VALID("NVIDIA ");
    fifoAddData(0, arg1, szglname(arg0)*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCombinerParameterfvNV, 2);
}
void PT_CALL glCombinerParameteriNV(uint32_t arg0, uint32_t arg1) {
    RENDERER_VALID("NVIDIA ");
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCombinerParameteriNV, 2);
}
void PT_CALL glCombinerParameterivNV(uint32_t arg0, uint32_t arg1) {
    RENDERER_VALID("NVIDIA ");
    fifoAddData(0, arg1, szglname(arg0)*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCombinerParameterivNV, 2);
}
void PT_CALL glCombinerStageParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    RENDERER_VALID("NVIDIA ");
    fifoAddData(0, arg2, szglname(arg1)*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCombinerStageParameterfvNV, 3);
}
void PT_CALL glCommandListSegmentsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCommandListSegmentsNV;
}
void PT_CALL glCompileCommandListNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompileCommandListNV;
}
void PT_CALL glCompileShader(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCompileShader, 1);
}
void PT_CALL glCompileShaderARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCompileShaderARB, 1);
}
void PT_CALL glCompileShaderIncludeARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompileShaderIncludeARB;
}
void PT_CALL glCompressedMultiTexImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedMultiTexImage1DEXT;
}
void PT_CALL glCompressedMultiTexImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedMultiTexImage2DEXT;
}
void PT_CALL glCompressedMultiTexImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedMultiTexImage3DEXT;
}
void PT_CALL glCompressedMultiTexSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedMultiTexSubImage1DEXT;
}
void PT_CALL glCompressedMultiTexSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedMultiTexSubImage2DEXT;
}
void PT_CALL glCompressedMultiTexSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedMultiTexSubImage3DEXT;
}
void PT_CALL glCompressedTexImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg5)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg6, arg5);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexImage1D;
}
void PT_CALL glCompressedTexImage1DARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg5)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg6, arg5);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexImage1DARB;
}
void PT_CALL glCompressedTexImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg6)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg7, arg6);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexImage2D;
}
void PT_CALL glCompressedTexImage2DARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg6)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg7, arg6);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexImage2DARB;
}
void PT_CALL glCompressedTexImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg7)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg8, arg7);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexImage3D;
}
void PT_CALL glCompressedTexImage3DARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg7)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg8, arg7);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexImage3DARB;
}
void PT_CALL glCompressedTexSubImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg5)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg6, arg5);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexSubImage1D;
}
void PT_CALL glCompressedTexSubImage1DARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg5)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg6, arg5);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexSubImage1DARB;
}
void PT_CALL glCompressedTexSubImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg7)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg8, arg7);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexSubImage2D;
}
void PT_CALL glCompressedTexSubImage2DARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg7)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg8, arg7);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexSubImage2DARB;
}
void PT_CALL glCompressedTexSubImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg9)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg10, arg9);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexSubImage3D;
}
void PT_CALL glCompressedTexSubImage3DARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    if (pixUnpackBuf == 0) {
        uint32_t *texPtr;
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(arg9)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg10, arg9);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTexSubImage3DARB;
}
void PT_CALL glCompressedTextureImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureImage1DEXT;
}
void PT_CALL glCompressedTextureImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureImage2DEXT;
}
void PT_CALL glCompressedTextureImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureImage3DEXT;
}
void PT_CALL glCompressedTextureSubImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureSubImage1D;
}
void PT_CALL glCompressedTextureSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureSubImage1DEXT;
}
void PT_CALL glCompressedTextureSubImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureSubImage2D;
}
void PT_CALL glCompressedTextureSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureSubImage2DEXT;
}
void PT_CALL glCompressedTextureSubImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureSubImage3D;
}
void PT_CALL glCompressedTextureSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCompressedTextureSubImage3DEXT;
}
void PT_CALL glConservativeRasterParameterfNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConservativeRasterParameterfNV;
}
void PT_CALL glConservativeRasterParameteriNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConservativeRasterParameteriNV;
}
void PT_CALL glConvolutionFilter1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionFilter1D;
}
void PT_CALL glConvolutionFilter1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionFilter1DEXT;
}
void PT_CALL glConvolutionFilter2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionFilter2D;
}
void PT_CALL glConvolutionFilter2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionFilter2DEXT;
}
void PT_CALL glConvolutionParameterf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameterf;
}
void PT_CALL glConvolutionParameterfEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameterfEXT;
}
void PT_CALL glConvolutionParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameterfv;
}
void PT_CALL glConvolutionParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameterfvEXT;
}
void PT_CALL glConvolutionParameteri(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameteri;
}
void PT_CALL glConvolutionParameteriEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameteriEXT;
}
void PT_CALL glConvolutionParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameteriv;
}
void PT_CALL glConvolutionParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameterivEXT;
}
void PT_CALL glConvolutionParameterxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameterxOES;
}
void PT_CALL glConvolutionParameterxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glConvolutionParameterxvOES;
}
void PT_CALL glCopyBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyBufferSubData, 5);
}
void PT_CALL glCopyColorSubTable(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyColorSubTable;
}
void PT_CALL glCopyColorSubTableEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyColorSubTableEXT;
}
void PT_CALL glCopyColorTable(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyColorTable;
}
void PT_CALL glCopyColorTableSGI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyColorTableSGI;
}
void PT_CALL glCopyConvolutionFilter1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyConvolutionFilter1D;
}
void PT_CALL glCopyConvolutionFilter1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyConvolutionFilter1DEXT;
}
void PT_CALL glCopyConvolutionFilter2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyConvolutionFilter2D;
}
void PT_CALL glCopyConvolutionFilter2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyConvolutionFilter2DEXT;
}
void PT_CALL glCopyImageSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyImageSubData, 15);
}
void PT_CALL glCopyImageSubDataNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyImageSubDataNV;
}
void PT_CALL glCopyMultiTexImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyMultiTexImage1DEXT;
}
void PT_CALL glCopyMultiTexImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyMultiTexImage2DEXT;
}
void PT_CALL glCopyMultiTexSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyMultiTexSubImage1DEXT;
}
void PT_CALL glCopyMultiTexSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyMultiTexSubImage2DEXT;
}
void PT_CALL glCopyMultiTexSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyMultiTexSubImage3DEXT;
}
void PT_CALL glCopyNamedBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyNamedBufferSubData;
}
void PT_CALL glCopyPathNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCopyPathNV;
}
void PT_CALL glCopyPixels(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyPixels, 5);
}
void PT_CALL glCopyTexImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexImage1D, 7);
}
void PT_CALL glCopyTexImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexImage1DEXT, 7);
}
void PT_CALL glCopyTexImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexImage2D, 8);
}
void PT_CALL glCopyTexImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexImage2DEXT, 8);
}
void PT_CALL glCopyTexSubImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexSubImage1D, 6);
}
void PT_CALL glCopyTexSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexSubImage1DEXT, 6);
}
void PT_CALL glCopyTexSubImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexSubImage2D, 8);
}
void PT_CALL glCopyTexSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexSubImage2DEXT, 8);
}
void PT_CALL glCopyTexSubImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexSubImage3D, 9);
}
void PT_CALL glCopyTexSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTexSubImage3DEXT, 9);
}
void PT_CALL glCopyTextureImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureImage1DEXT, 8);
}
void PT_CALL glCopyTextureImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureImage2DEXT, 9);
}
void PT_CALL glCopyTextureSubImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureSubImage1D, 6);
}
void PT_CALL glCopyTextureSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureSubImage1DEXT, 7);
}
void PT_CALL glCopyTextureSubImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureSubImage2D, 8);
}
void PT_CALL glCopyTextureSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureSubImage2DEXT, 9);
}
void PT_CALL glCopyTextureSubImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureSubImage3D, 9);
}
void PT_CALL glCopyTextureSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCopyTextureSubImage3DEXT, 10);
}
void PT_CALL glCoverFillPathInstancedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCoverFillPathInstancedNV;
}
void PT_CALL glCoverFillPathNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCoverFillPathNV;
}
void PT_CALL glCoverStrokePathInstancedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCoverStrokePathInstancedNV;
}
void PT_CALL glCoverStrokePathNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCoverStrokePathNV;
}
void PT_CALL glCoverageModulationNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCoverageModulationNV;
}
void PT_CALL glCoverageModulationTableNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCoverageModulationTableNV;
}
void PT_CALL glCreateBuffers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateBuffers;
}
void PT_CALL glCreateCommandListsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateCommandListsNV;
}
void PT_CALL glCreateFramebuffers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateFramebuffers;
}
void PT_CALL glCreateMemoryObjectsEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateMemoryObjectsEXT;
}
void PT_CALL glCreatePerfQueryINTEL(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreatePerfQueryINTEL;
}
uint32_t PT_CALL glCreateProgram(void) {
    uint32_t ret;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateProgram;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glCreateProgramObjectARB(void) {
    uint32_t ret;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateProgramObjectARB;
    ret = *pt0;
    return ret;
}
void PT_CALL glCreateProgramPipelines(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateProgramPipelines;
}
void PT_CALL glCreateQueries(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateQueries;
}
void PT_CALL glCreateRenderbuffers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateRenderbuffers;
}
void PT_CALL glCreateSamplers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateSamplers;
}
uint32_t PT_CALL glCreateShader(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateShader;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glCreateShaderObjectARB(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateShaderObjectARB;
    ret = *pt0;
    return ret;
}
void PT_CALL glCreateShaderProgramEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateShaderProgramEXT;
}
void PT_CALL glCreateShaderProgramv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateShaderProgramv;
}
void PT_CALL glCreateStatesNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateStatesNV;
}
void PT_CALL glCreateSyncFromCLeventARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateSyncFromCLeventARB;
}
void PT_CALL glCreateTextures(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateTextures;
}
void PT_CALL glCreateTransformFeedbacks(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateTransformFeedbacks;
}
void PT_CALL glCreateVertexArrays(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCreateVertexArrays;
}
void PT_CALL glCullFace(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glCullFace, 1);
}
void PT_CALL glCullParameterdvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCullParameterdvEXT;
}
void PT_CALL glCullParameterfvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCullParameterfvEXT;
}
void PT_CALL glCurrentPaletteMatrixARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glCurrentPaletteMatrixARB;
}
void PT_CALL glDebugMessageCallback(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageCallback;
}
void PT_CALL glDebugMessageCallbackAMD(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageCallbackAMD;
}
void PT_CALL glDebugMessageCallbackARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageCallbackARB;
}
void PT_CALL glDebugMessageControl(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageControl;
}
void PT_CALL glDebugMessageControlARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageControlARB;
}
void PT_CALL glDebugMessageEnableAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageEnableAMD;
}
void PT_CALL glDebugMessageInsert(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageInsert;
}
void PT_CALL glDebugMessageInsertAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageInsertAMD;
}
void PT_CALL glDebugMessageInsertARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(0, arg5, arg4);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDebugMessageInsertARB;
}
void PT_CALL glDeformSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeformSGIX;
}
void PT_CALL glDeformationMap3dSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15, uint32_t arg16, uint32_t arg17, uint32_t arg18, uint32_t arg19) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; pt[16] = arg15; pt[17] = arg16; pt[18] = arg17; pt[19] = arg18; pt[20] = arg19; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeformationMap3dSGIX;
}
void PT_CALL glDeformationMap3fSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeformationMap3fSGIX;
}
void PT_CALL glDeleteAsyncMarkersSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteAsyncMarkersSGIX;
}
void PT_CALL glDeleteBuffers(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteBuffers, 2);
    for (int i = 0; i < arg0; i++) {
        pixPackBuf = (((uint32_t *)arg1)[i] == pixPackBuf)? 0:pixPackBuf;
        pixUnpackBuf = (((uint32_t *)arg1)[i] == pixUnpackBuf)? 0:pixUnpackBuf;
        queryBuf = (((uint32_t *)arg1)[i] == queryBuf)? 0:queryBuf;
        if ((vtxArry.vao == 0) && vtxArry.arrayBuf && (((uint32_t *)arg1)[i] == vtxArry.arrayBuf))
            vtxarry_ptr_reset();
        vtxArry.arrayBuf = (((uint32_t *)arg1)[i] == vtxArry.arrayBuf)? 0:vtxArry.arrayBuf;
        vtxArry.elemArryBuf = (((uint32_t *)arg1)[i] == vtxArry.elemArryBuf)? 0:vtxArry.elemArryBuf;
    }
    if (vtxArry.vao) {
        vtxArry.arrayBuf = vtxArry.vao;
        vtxArry.elemArryBuf = vtxArry.vao;
    }
}
void PT_CALL glDeleteBuffersARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteBuffersARB, 2);
    for (int i = 0; i < arg0; i++) {
        pixPackBuf = (((uint32_t *)arg1)[i] == pixPackBuf)? 0:pixPackBuf;
        pixUnpackBuf = (((uint32_t *)arg1)[i] == pixUnpackBuf)? 0:pixUnpackBuf;
        queryBuf = (((uint32_t *)arg1)[i] == queryBuf)? 0:queryBuf;
        if ((vtxArry.vao == 0) && vtxArry.arrayBuf && (((uint32_t *)arg1)[i] == vtxArry.arrayBuf))
            vtxarry_ptr_reset();
        vtxArry.arrayBuf = (((uint32_t *)arg1)[i] == vtxArry.arrayBuf)? 0:vtxArry.arrayBuf;
        vtxArry.elemArryBuf = (((uint32_t *)arg1)[i] == vtxArry.elemArryBuf)? 0:vtxArry.elemArryBuf;
    }
    if (vtxArry.vao) {
        vtxArry.arrayBuf = vtxArry.vao;
        vtxArry.elemArryBuf = vtxArry.vao;
    }
}
void PT_CALL glDeleteCommandListsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteCommandListsNV;
}
void PT_CALL glDeleteFencesAPPLE(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteFencesAPPLE, 2);
}
void PT_CALL glDeleteFencesNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteFencesNV, 2);
}
void PT_CALL glDeleteFragmentShaderATI(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteFragmentShaderATI, 1);
}
void PT_CALL glDeleteFramebuffers(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteFramebuffers, 2);
}
void PT_CALL glDeleteFramebuffersEXT(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteFramebuffersEXT, 2);
}
void PT_CALL glDeleteLists(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteLists, 2);
}
void PT_CALL glDeleteMemoryObjectsEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteMemoryObjectsEXT;
}
void PT_CALL glDeleteNamedStringARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteNamedStringARB;
}
void PT_CALL glDeleteNamesAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteNamesAMD;
}
void PT_CALL glDeleteObjectARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteObjectARB, 1);
}
void PT_CALL glDeleteOcclusionQueriesNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteOcclusionQueriesNV, 2);
}
void PT_CALL glDeletePathsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeletePathsNV;
}
void PT_CALL glDeletePerfMonitorsAMD(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeletePerfMonitorsAMD;
}
void PT_CALL glDeletePerfQueryINTEL(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeletePerfQueryINTEL;
}
void PT_CALL glDeleteProgram(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteProgram, 1);
}
void PT_CALL glDeleteProgramPipelines(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteProgramPipelines;
}
void PT_CALL glDeleteProgramsARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteProgramsARB, 2);
}
void PT_CALL glDeleteProgramsNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteProgramsNV, 2);
}
void PT_CALL glDeleteQueries(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteQueries, 2);
}
void PT_CALL glDeleteQueriesARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteQueriesARB, 2);
}
void PT_CALL glDeleteQueryResourceTagNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteQueryResourceTagNV;
}
void PT_CALL glDeleteRenderbuffers(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteRenderbuffers, 2);
}
void PT_CALL glDeleteRenderbuffersEXT(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteRenderbuffersEXT, 2);
}
void PT_CALL glDeleteSamplers(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteSamplers, 2);
}
void PT_CALL glDeleteSemaphoresEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteSemaphoresEXT;
}
void PT_CALL glDeleteShader(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteShader, 1);
}
void PT_CALL glDeleteStatesNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteStatesNV;
}
void PT_CALL glDeleteSync(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteSync, 1);
}
void PT_CALL glDeleteTextures(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteTextures, 2);
}
void PT_CALL glDeleteTexturesEXT(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteTexturesEXT, 2);
}
void PT_CALL glDeleteTransformFeedbacks(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteTransformFeedbacks;
}
void PT_CALL glDeleteTransformFeedbacksNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteTransformFeedbacksNV;
}
void PT_CALL glDeleteVertexArrays(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDeleteVertexArrays, 2);
    for (int i = 0; i < arg0; i++)
        vtxArry.vao = (((uint32_t *)arg1)[i] == vtxArry.vao)? 0:vtxArry.vao;
    vtxArry.arrayBuf = vtxArry.vao;
    vtxArry.elemArryBuf = vtxArry.vao;
}
void PT_CALL glDeleteVertexArraysAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteVertexArraysAPPLE;
}
void PT_CALL glDeleteVertexShaderEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDeleteVertexShaderEXT;
}
void PT_CALL glDepthBoundsEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDepthBoundsEXT, 4);
}
void PT_CALL glDepthBoundsdNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDepthBoundsdNV;
}
void PT_CALL glDepthFunc(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDepthFunc, 1);
}
void PT_CALL glDepthMask(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDepthMask, 1);
}
void PT_CALL glDepthRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDepthRange, 4);
}
void PT_CALL glDepthRangeArrayv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*2*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDepthRangeArrayv, 3);
}
void PT_CALL glDepthRangeIndexed(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDepthRangeIndexed;
}
void PT_CALL glDepthRangedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDepthRangedNV;
}
void PT_CALL glDepthRangef(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDepthRangef;
}
void PT_CALL glDepthRangefOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDepthRangefOES;
}
void PT_CALL glDepthRangexOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDepthRangexOES;
}
void PT_CALL glDetachObjectARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDetachObjectARB, 2);
}
void PT_CALL glDetachShader(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDetachShader;
}
void PT_CALL glDetailTexFuncSGIS(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDetailTexFuncSGIS;
}
void PT_CALL glDisable(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDisable, 1);
    vtxarry_state(arg0, 0);
}
void PT_CALL glDisableClientState(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDisableClientState, 1);
    if ((arg0 & 0xFFF0U) == GL_VERTEX_ATTRIB_ARRAY0_NV)
        vtxarry_state(vattr2arry_state(arg0 & 0xFU), 0);
    else
        vtxarry_state(arg0, 0);
}
void PT_CALL glDisableClientStateIndexedEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDisableClientStateIndexedEXT;
}
void PT_CALL glDisableClientStateiEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDisableClientStateiEXT;
}
void PT_CALL glDisableIndexedEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDisableIndexedEXT, 2);
}
void PT_CALL glDisableVariantClientStateEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDisableVariantClientStateEXT;
}
void PT_CALL glDisableVertexArrayAttrib(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDisableVertexArrayAttrib;
}
void PT_CALL glDisableVertexArrayAttribEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDisableVertexArrayAttribEXT;
}
void PT_CALL glDisableVertexArrayEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDisableVertexArrayEXT;
}
void PT_CALL glDisableVertexAttribAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDisableVertexAttribAPPLE;
}
void PT_CALL glDisableVertexAttribArray(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDisableVertexAttribArray, 1);
    vtxarry_state(vattr2arry_state(arg0), 0);
}
void PT_CALL glDisableVertexAttribArrayARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDisableVertexAttribArrayARB, 1);
    vtxarry_state(vattr2arry_state(arg0), 0);
}
void PT_CALL glDisablei(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDisablei, 2);
}
void PT_CALL glDispatchCompute(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDispatchCompute;
}
void PT_CALL glDispatchComputeGroupSizeARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDispatchComputeGroupSizeARB;
}
void PT_CALL glDispatchComputeIndirect(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDispatchComputeIndirect;
}
void PT_CALL glDrawArrays(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    if (arg2 && (vtxArry.arrayBuf == 0)) {
        PrepVertexArray(arg1, arg1 + arg2 - 1, 0);
        PushVertexArray(arg1, arg1 + arg2 - 1);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawArrays, 3);
}
void PT_CALL glDrawArraysEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    if (arg2 && (vtxArry.arrayBuf == 0)) {
        PrepVertexArray(arg1, arg1 + arg2 - 1, 0);
        PushVertexArray(arg1, arg1 + arg2 - 1);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawArraysEXT, 3);
}
void PT_CALL glDrawArraysIndirect(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawArraysIndirect, 2);
}
void PT_CALL glDrawArraysInstanced(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawArraysInstanced, 4);
}
void PT_CALL glDrawArraysInstancedARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawArraysInstancedARB, 4);
}
void PT_CALL glDrawArraysInstancedBaseInstance(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawArraysInstancedBaseInstance, 5);
}
void PT_CALL glDrawArraysInstancedEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawArraysInstancedEXT, 4);
}
void PT_CALL glDrawBuffer(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawBuffer, 1);
}
void PT_CALL glDrawBuffers(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawBuffers, 2);
}
void PT_CALL glDrawBuffersARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawBuffersARB, 2);
}
void PT_CALL glDrawBuffersATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawBuffersATI;
}
void PT_CALL glDrawCommandsAddressNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawCommandsAddressNV;
}
void PT_CALL glDrawCommandsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawCommandsNV;
}
void PT_CALL glDrawCommandsStatesAddressNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawCommandsStatesAddressNV;
}
void PT_CALL glDrawCommandsStatesNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawCommandsStatesNV;
}
void PT_CALL glDrawElementArrayAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawElementArrayAPPLE;
}
void PT_CALL glDrawElementArrayATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawElementArrayATI;
}
void PT_CALL glDrawElements(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(start, end, ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray(start, end);
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElements, 4);
}
void PT_CALL glDrawElementsBaseVertex(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray((start + arg4), (end + arg4), ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray((start + arg4), (end + arg4));
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsBaseVertex, 5);
}
void PT_CALL glDrawElementsIndirect(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 5*sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsIndirect, 3);
}
void PT_CALL glDrawElementsInstanced(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(start, end, ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray(start, end);
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsInstanced, 5);
}
void PT_CALL glDrawElementsInstancedARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(start, end, ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray(start, end);
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsInstancedARB, 5);
}
void PT_CALL glDrawElementsInstancedBaseInstance(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(start, end, ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray(start, end);
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsInstancedBaseInstance, 6);
}
void PT_CALL glDrawElementsInstancedBaseVertex(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray((start + arg5), (end + arg5), ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray((start + arg5), (end + arg5));
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsInstancedBaseVertex, 6);
}
void PT_CALL glDrawElementsInstancedBaseVertexBaseInstance(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray((start + arg5), (end + arg5), ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray((start + arg5), (end + arg5));
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsInstancedBaseVertexBaseInstance, 7);
}
void PT_CALL glDrawElementsInstancedEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    int start, end;
    if (vtxArry.elemArryBuf == 0) {
        end = 0;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                end = (p[i] > end)? p[i]:end;
            }
        }
        start = end;
        for (int i = 0; i < arg1; i++) {
            if (szgldata(0, arg2) == 1) {
                uint8_t *p = (uint8_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 2) {
                uint16_t *p = (uint16_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
            if (szgldata(0, arg2) == 4) {
                uint32_t *p = (uint32_t *)arg3;
                start = (p[i] < start)? p[i]:start;
            }
        }
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(start, end, ALIGNED(arg1 * szgldata(0, arg2)));
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
            PushVertexArray(start, end);
        }
        else
            fifoAddData(0, arg3, ALIGNED(arg1 * szgldata(0, arg2)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawElementsInstancedEXT, 5);
}
void PT_CALL glDrawMeshArraysSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawMeshArraysSUN;
}
void PT_CALL glDrawMeshTasksIndirectNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawMeshTasksIndirectNV;
}
void PT_CALL glDrawMeshTasksNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawMeshTasksNV;
}
void PT_CALL glDrawPixels(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    if (pixUnpackBuf == 0) {
        uint32_t szPix;
        szPix = ((szUnpackWidth == 0)? arg0:szUnpackWidth) * arg1 * szgldata(arg2, arg3);
        fifoAddData(0, arg4, ALIGNED(szPix));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawPixels, 5);
}
void PT_CALL glDrawRangeElementArrayAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawRangeElementArrayAPPLE;
}
void PT_CALL glDrawRangeElementArrayATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawRangeElementArrayATI;
}
void PT_CALL glDrawRangeElements(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    if (vtxArry.elemArryBuf == 0) {
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(arg1, arg2, ALIGNED(arg3 * szgldata(0, arg4)));
            fifoAddData(0, arg5, ALIGNED(arg3 * szgldata(0, arg4)));
            PushVertexArray(arg1, arg2);
        }
        else
            fifoAddData(0, arg5, ALIGNED(arg3 * szgldata(0, arg4)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawRangeElements, 6);
}
void PT_CALL glDrawRangeElementsBaseVertex(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (vtxArry.elemArryBuf == 0) {
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(arg1 + arg6, arg2 + arg6, ALIGNED(arg3 * szgldata(0, arg4)));
            fifoAddData(0, arg5, ALIGNED(arg3 * szgldata(0, arg4)));
            PushVertexArray(arg1 + arg6, arg2 + arg6);
        }
        else
            fifoAddData(0, arg5, ALIGNED(arg3 * szgldata(0, arg4)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawRangeElementsBaseVertex, 7);
}
void PT_CALL glDrawRangeElementsEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    if (vtxArry.elemArryBuf == 0) {
        if (vtxArry.arrayBuf == 0) {
            PrepVertexArray(arg1, arg2, ALIGNED(arg3 * szgldata(0, arg4)));
            fifoAddData(0, arg5, ALIGNED(arg3 * szgldata(0, arg4)));
            PushVertexArray(arg1, arg2);
        }
        else
            fifoAddData(0, arg5, ALIGNED(arg3 * szgldata(0, arg4)));
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glDrawRangeElementsEXT, 6);
}
void PT_CALL glDrawTextureNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawTextureNV;
}
void PT_CALL glDrawTransformFeedback(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawTransformFeedback;
}
void PT_CALL glDrawTransformFeedbackInstanced(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawTransformFeedbackInstanced;
}
void PT_CALL glDrawTransformFeedbackNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawTransformFeedbackNV;
}
void PT_CALL glDrawTransformFeedbackStream(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawTransformFeedbackStream;
}
void PT_CALL glDrawTransformFeedbackStreamInstanced(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawTransformFeedbackStreamInstanced;
}
void PT_CALL glDrawVkImageNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glDrawVkImageNV;
}
void PT_CALL glEGLImageTargetTexStorageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEGLImageTargetTexStorageEXT;
}
void PT_CALL glEGLImageTargetTextureStorageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEGLImageTargetTextureStorageEXT;
}
void PT_CALL glEdgeFlag(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEdgeFlag, 1);
}
void PT_CALL glEdgeFlagFormatNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEdgeFlagFormatNV;
}
void PT_CALL glEdgeFlagPointer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEdgeFlagPointer, 2);
    vtxarry_init(&vtxArry.EdgeFlag, 1, GL_BYTE, arg0, (void *)arg1);
}
void PT_CALL glEdgeFlagPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEdgeFlagPointerEXT, 3);
    vtxarry_init(&vtxArry.EdgeFlag, 1, GL_BYTE, arg0, (void *)arg2);
}
void PT_CALL glEdgeFlagPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEdgeFlagPointerListIBM;
}
void PT_CALL glEdgeFlagv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(uint32_t)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEdgeFlagv, 1);
}
void PT_CALL glElementPointerAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glElementPointerAPPLE;
}
void PT_CALL glElementPointerATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glElementPointerATI;
}
void PT_CALL glEnable(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEnable, 1);
    vtxarry_state(arg0, 1);
}
void PT_CALL glEnableClientState(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEnableClientState, 1);
    if ((arg0 & 0xFFF0U) == GL_VERTEX_ATTRIB_ARRAY0_NV)
        vtxarry_state(vattr2arry_state(arg0 & 0xFU), 1);
    else
        vtxarry_state(arg0, 1);
}
void PT_CALL glEnableClientStateIndexedEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEnableClientStateIndexedEXT;
}
void PT_CALL glEnableClientStateiEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEnableClientStateiEXT;
}
void PT_CALL glEnableIndexedEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEnableIndexedEXT, 2);
}
void PT_CALL glEnableVariantClientStateEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEnableVariantClientStateEXT;
}
void PT_CALL glEnableVertexArrayAttrib(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEnableVertexArrayAttrib;
}
void PT_CALL glEnableVertexArrayAttribEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEnableVertexArrayAttribEXT;
}
void PT_CALL glEnableVertexArrayEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEnableVertexArrayEXT;
}
void PT_CALL glEnableVertexAttribAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEnableVertexAttribAPPLE;
}
void PT_CALL glEnableVertexAttribArray(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEnableVertexAttribArray, 1);
    vtxarry_state(vattr2arry_state(arg0), 1);
}
void PT_CALL glEnableVertexAttribArrayARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEnableVertexAttribArrayARB, 1);
    vtxarry_state(vattr2arry_state(arg0), 1);
}
void PT_CALL glEnablei(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEnablei, 2);
}
void PT_CALL glEnd(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEnd, 0);
}
void PT_CALL glEndConditionalRender(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndConditionalRender;
}
void PT_CALL glEndConditionalRenderNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndConditionalRenderNV;
}
void PT_CALL glEndConditionalRenderNVX(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndConditionalRenderNVX;
}
void PT_CALL glEndFragmentShaderATI(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndFragmentShaderATI, 0);
}
void PT_CALL glEndList(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndList, 0);
}
void PT_CALL glEndOcclusionQueryNV(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndOcclusionQueryNV, 0);
}
void PT_CALL glEndPerfMonitorAMD(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndPerfMonitorAMD;
}
void PT_CALL glEndPerfQueryINTEL(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndPerfQueryINTEL;
}
void PT_CALL glEndQuery(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndQuery, 1);
}
void PT_CALL glEndQueryARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndQueryARB, 1);
}
void PT_CALL glEndQueryIndexed(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndQueryIndexed, 2);
}
void PT_CALL glEndTransformFeedback(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndTransformFeedback, 0);
}
void PT_CALL glEndTransformFeedbackEXT(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEndTransformFeedbackEXT, 0);
}
void PT_CALL glEndTransformFeedbackNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndTransformFeedbackNV;
}
void PT_CALL glEndVertexShaderEXT(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndVertexShaderEXT;
}
void PT_CALL glEndVideoCaptureNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEndVideoCaptureNV;
}
void PT_CALL glEvalCoord1d(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord1d, 2);
}
void PT_CALL glEvalCoord1dv(uint32_t arg0) {
    fifoAddData(0, arg0, sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord1dv, 1);
}
void PT_CALL glEvalCoord1f(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord1f, 1);
}
void PT_CALL glEvalCoord1fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord1fv, 1);
}
void PT_CALL glEvalCoord1xOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEvalCoord1xOES;
}
void PT_CALL glEvalCoord1xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEvalCoord1xvOES;
}
void PT_CALL glEvalCoord2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord2d, 4);
}
void PT_CALL glEvalCoord2dv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord2dv, 1);
}
void PT_CALL glEvalCoord2f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord2f, 2);
}
void PT_CALL glEvalCoord2fv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalCoord2fv, 1);
}
void PT_CALL glEvalCoord2xOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEvalCoord2xOES;
}
void PT_CALL glEvalCoord2xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEvalCoord2xvOES;
}
void PT_CALL glEvalMapsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEvalMapsNV;
}
void PT_CALL glEvalMesh1(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalMesh1, 3);
}
void PT_CALL glEvalMesh2(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalMesh2, 5);
}
void PT_CALL glEvalPoint1(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalPoint1, 1);
}
void PT_CALL glEvalPoint2(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glEvalPoint2, 2);
}
void PT_CALL glEvaluateDepthValuesARB(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glEvaluateDepthValuesARB;
}
void PT_CALL glExecuteProgramNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glExecuteProgramNV, 3);
}
void PT_CALL glExtractComponentEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glExtractComponentEXT;
}
void PT_CALL glFeedbackBuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFeedbackBuffer;
    fifoOutData(0, arg2, (arg0*sizeof(float)));
}
void PT_CALL glFeedbackBufferxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFeedbackBufferxOES;
}
uint32_t PT_CALL glFenceSync(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFenceSync;
    ret = *pt0;
    return ret;
}
void PT_CALL glFinalCombinerInputNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    RENDERER_VALID("NVIDIA ");
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFinalCombinerInputNV, 4);
}
void PT_CALL glFinish(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFinish;
}
void PT_CALL glFinishAsyncSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFinishAsyncSGIX;
}
void PT_CALL glFinishFenceAPPLE(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFinishFenceAPPLE, 1);
}
void PT_CALL glFinishFenceNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFinishFenceNV, 1);
}
void PT_CALL glFinishObjectAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFinishObjectAPPLE, 1);
}
void PT_CALL glFinishTextureSUNX(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFinishTextureSUNX;
}
void PT_CALL glFlush(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlush;
}
void PT_CALL glFlushMappedBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFlushMappedBufferRange, 3);
}
void PT_CALL glFlushMappedBufferRangeAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFlushMappedBufferRangeAPPLE, 3);
}
void PT_CALL glFlushMappedNamedBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlushMappedNamedBufferRange;
}
void PT_CALL glFlushMappedNamedBufferRangeEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlushMappedNamedBufferRangeEXT;
}
void PT_CALL glFlushPixelDataRangeNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlushPixelDataRangeNV;
}
void PT_CALL glFlushRasterSGIX(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlushRasterSGIX;
}
void PT_CALL glFlushStaticDataIBM(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlushStaticDataIBM;
}
void PT_CALL glFlushVertexArrayRangeAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlushVertexArrayRangeAPPLE;
}
void PT_CALL glFlushVertexArrayRangeNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFlushVertexArrayRangeNV;
}
void PT_CALL glFogCoordFormatNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFogCoordFormatNV;
}
void PT_CALL glFogCoordPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoordPointer, 3);
    vtxarry_init(&vtxArry.FogCoord, 1, arg0, arg1, (void *)arg2);
}
void PT_CALL glFogCoordPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoordPointerEXT, 3);
    vtxarry_init(&vtxArry.FogCoord, 1, arg0, arg1, (void *)arg2);
}
void PT_CALL glFogCoordPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFogCoordPointerListIBM;
}
void PT_CALL glFogCoordd(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoordd, 2);
}
void PT_CALL glFogCoorddEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoorddEXT, 2);
}
void PT_CALL glFogCoorddv(uint32_t arg0) {
    fifoAddData(0, arg0, sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoorddv, 1);
}
void PT_CALL glFogCoorddvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoorddvEXT, 1);
}
void PT_CALL glFogCoordf(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoordf, 1);
}
void PT_CALL glFogCoordfEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoordfEXT, 1);
}
void PT_CALL glFogCoordfv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoordfv, 1);
}
void PT_CALL glFogCoordfvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogCoordfvEXT, 1);
}
void PT_CALL glFogCoordhNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFogCoordhNV;
}
void PT_CALL glFogCoordhvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFogCoordhvNV;
}
void PT_CALL glFogFuncSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFogFuncSGIS;
}
void PT_CALL glFogf(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogf, 2);
}
void PT_CALL glFogfv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogfv, 2);
}
void PT_CALL glFogi(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogi, 2);
}
void PT_CALL glFogiv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFogiv, 2);
}
void PT_CALL glFogxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFogxOES;
}
void PT_CALL glFogxvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFogxvOES;
}
void PT_CALL glFragmentColorMaterialSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentColorMaterialSGIX;
}
void PT_CALL glFragmentCoverageColorNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentCoverageColorNV;
}
void PT_CALL glFragmentLightModelfSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightModelfSGIX;
}
void PT_CALL glFragmentLightModelfvSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightModelfvSGIX;
}
void PT_CALL glFragmentLightModeliSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightModeliSGIX;
}
void PT_CALL glFragmentLightModelivSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightModelivSGIX;
}
void PT_CALL glFragmentLightfSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightfSGIX;
}
void PT_CALL glFragmentLightfvSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightfvSGIX;
}
void PT_CALL glFragmentLightiSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightiSGIX;
}
void PT_CALL glFragmentLightivSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentLightivSGIX;
}
void PT_CALL glFragmentMaterialfSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentMaterialfSGIX;
}
void PT_CALL glFragmentMaterialfvSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentMaterialfvSGIX;
}
void PT_CALL glFragmentMaterialiSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentMaterialiSGIX;
}
void PT_CALL glFragmentMaterialivSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFragmentMaterialivSGIX;
}
void PT_CALL glFrameTerminatorGREMEDY(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFrameTerminatorGREMEDY;
}
void PT_CALL glFrameZoomSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFrameZoomSGIX;
}
void PT_CALL glFramebufferDrawBufferEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferDrawBufferEXT;
}
void PT_CALL glFramebufferDrawBuffersEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferDrawBuffersEXT;
}
void PT_CALL glFramebufferFetchBarrierEXT(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferFetchBarrierEXT;
}
void PT_CALL glFramebufferParameteri(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferParameteri, 3);
}
void PT_CALL glFramebufferReadBufferEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferReadBufferEXT;
}
void PT_CALL glFramebufferRenderbuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferRenderbuffer, 4);
}
void PT_CALL glFramebufferRenderbufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferRenderbufferEXT, 4);
}
void PT_CALL glFramebufferSampleLocationsfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferSampleLocationsfvARB;
}
void PT_CALL glFramebufferSampleLocationsfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferSampleLocationsfvNV;
}
void PT_CALL glFramebufferSamplePositionsfvAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferSamplePositionsfvAMD;
}
void PT_CALL glFramebufferTexture1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTexture1D, 5);
}
void PT_CALL glFramebufferTexture1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTexture1DEXT, 5);
}
void PT_CALL glFramebufferTexture2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTexture2D, 5);
}
void PT_CALL glFramebufferTexture2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTexture2DEXT, 5);
}
void PT_CALL glFramebufferTexture3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTexture3D, 6);
}
void PT_CALL glFramebufferTexture3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTexture3DEXT, 6);
}
void PT_CALL glFramebufferTexture(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTexture, 4);
}
void PT_CALL glFramebufferTextureARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTextureARB, 4);
}
void PT_CALL glFramebufferTextureEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTextureEXT, 4);
}
void PT_CALL glFramebufferTextureFaceARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTextureFaceARB, 5);
}
void PT_CALL glFramebufferTextureFaceEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTextureFaceEXT, 5);
}
void PT_CALL glFramebufferTextureLayer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTextureLayer, 5);
}
void PT_CALL glFramebufferTextureLayerARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTextureLayerARB, 5);
}
void PT_CALL glFramebufferTextureLayerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFramebufferTextureLayerEXT, 5);
}
void PT_CALL glFramebufferTextureMultiviewOVR(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFramebufferTextureMultiviewOVR;
}
void PT_CALL glFreeObjectBufferATI(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFreeObjectBufferATI;
}
void PT_CALL glFrontFace(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFrontFace, 1);
}
void PT_CALL glFrustum(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glFrustum, 12);
}
void PT_CALL glFrustumfOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFrustumfOES;
}
void PT_CALL glFrustumxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glFrustumxOES;
}
void PT_CALL glGenAsyncMarkersSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenAsyncMarkersSGIX;
}
void PT_CALL glGenBuffers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenBuffers;
    fifoOutData(0, arg1, arg0*sizeof(int));
}
void PT_CALL glGenBuffersARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenBuffersARB;
    fifoOutData(0, arg1, arg0*sizeof(int));
}
void PT_CALL glGenFencesAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenFencesAPPLE;
    fifoOutData(0, arg1, arg0*sizeof(uint32_t));
}
void PT_CALL glGenFencesNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenFencesNV;
    fifoOutData(0, arg1, arg0*sizeof(uint32_t));
}
uint32_t PT_CALL glGenFragmentShadersATI(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenFragmentShadersATI;
    ret = *pt0;
    return ret;
}
void PT_CALL glGenFramebuffers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenFramebuffers;
    fifoOutData(0, arg1, arg0*sizeof(int));
}
void PT_CALL glGenFramebuffersEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenFramebuffersEXT;
    fifoOutData(0, arg1, arg0*sizeof(int));
}
uint32_t PT_CALL glGenLists(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenLists;
    ret = *pt0;
    return ret;
}
void PT_CALL glGenNamesAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenNamesAMD;
}
void PT_CALL glGenOcclusionQueriesNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glGenOcclusionQueriesNV, 2);
    fifoOutData(0, arg1, arg0*sizeof(int));
}
void PT_CALL glGenPathsNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenPathsNV;
}
void PT_CALL glGenPerfMonitorsAMD(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenPerfMonitorsAMD;
}
void PT_CALL glGenProgramPipelines(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenProgramPipelines;
}
void PT_CALL glGenProgramsARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenProgramsARB;
    fifoOutData(0, arg1, (arg0 * sizeof(uint32_t)));
}
void PT_CALL glGenProgramsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenProgramsNV;
    fifoOutData(0, arg1, (arg0 * sizeof(uint32_t)));
}
void PT_CALL glGenQueries(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenQueries;
    fifoOutData(0, arg1, (arg0 * sizeof(uint32_t)));
}
void PT_CALL glGenQueriesARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenQueriesARB;
    fifoOutData(0, arg1, (arg0 * sizeof(uint32_t)));
}
void PT_CALL glGenQueryResourceTagNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenQueryResourceTagNV;
}
void PT_CALL glGenRenderbuffers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenRenderbuffers;
    fifoOutData(0, arg1, arg0*sizeof(int));
}
void PT_CALL glGenRenderbuffersEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenRenderbuffersEXT;
    fifoOutData(0, arg1, arg0*sizeof(int));
}
void PT_CALL glGenSamplers(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenSamplers;
    fifoOutData(0, arg1, arg0*sizeof(int));
}
void PT_CALL glGenSemaphoresEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenSemaphoresEXT;
}
void PT_CALL glGenSymbolsEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenSymbolsEXT;
}
void PT_CALL glGenTextures(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenTextures;
    fifoOutData(0, arg1, (arg0 * sizeof(uint32_t)));
}
void PT_CALL glGenTexturesEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenTexturesEXT;
    fifoOutData(0, arg1, (arg0 * sizeof(uint32_t)));
}
void PT_CALL glGenTransformFeedbacks(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenTransformFeedbacks;
}
void PT_CALL glGenTransformFeedbacksNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenTransformFeedbacksNV;
}
void PT_CALL glGenVertexArrays(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenVertexArrays;
    fifoOutData(0, arg1, (arg0 * sizeof(uint32_t)));
}
void PT_CALL glGenVertexArraysAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenVertexArraysAPPLE;
}
void PT_CALL glGenVertexShadersEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenVertexShadersEXT;
}
void PT_CALL glGenerateMipmap(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glGenerateMipmap, 1);
}
void PT_CALL glGenerateMipmapEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glGenerateMipmapEXT, 1);
}
void PT_CALL glGenerateMultiTexMipmapEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenerateMultiTexMipmapEXT;
}
void PT_CALL glGenerateTextureMipmap(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenerateTextureMipmap;
}
void PT_CALL glGenerateTextureMipmapEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGenerateTextureMipmapEXT;
}
void PT_CALL glGetActiveAtomicCounterBufferiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveAtomicCounterBufferiv;
}
void PT_CALL glGetActiveAttrib(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveAttrib;
}
void PT_CALL glGetActiveAttribARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveAttribARB;
}
void PT_CALL glGetActiveSubroutineName(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveSubroutineName;
}
void PT_CALL glGetActiveSubroutineUniformName(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveSubroutineUniformName;
}
void PT_CALL glGetActiveSubroutineUniformiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveSubroutineUniformiv;
}
void PT_CALL glGetActiveUniform(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    uint32_t n, e;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveUniform;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(2*ALIGNED(1), (uint32_t)&e, sizeof(uint32_t));
    if (e) {
        if (arg3)
            memcpy((char *)arg3, &n, sizeof(uint32_t));
        memcpy((char *)arg5, &e, sizeof(uint32_t));
        fifoOutData(ALIGNED(1), arg4, sizeof(uint32_t));
        fifoOutData(3*ALIGNED(1), arg6, ((n + 1) > arg2)? arg2:(n + 1));
    }
}
void PT_CALL glGetActiveUniformARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    uint32_t n, e;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveUniformARB;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(2*ALIGNED(1), (uint32_t)&e, sizeof(uint32_t));
    if (e) {
        if (arg3)
            memcpy((char *)arg3, &n, sizeof(uint32_t));
        memcpy((char *)arg5, &e, sizeof(uint32_t));
        fifoOutData(ALIGNED(1), arg4, sizeof(uint32_t));
        fifoOutData(3*ALIGNED(1), arg6, ((n + 1) > arg2)? arg2:(n + 1));
    }
}
void PT_CALL glGetActiveUniformBlockName(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveUniformBlockName;
}
void PT_CALL glGetActiveUniformBlockiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveUniformBlockiv;
}
void PT_CALL glGetActiveUniformName(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveUniformName;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    if (n) {
        if (arg3)
            memcpy((char *)arg3, &n, sizeof(uint32_t));
        fifoOutData(ALIGNED(1), arg4, ((n + 1) > arg2)? arg2:(n + 1));
    }
}
void PT_CALL glGetActiveUniformsiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveUniformsiv;
}
void PT_CALL glGetActiveVaryingNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetActiveVaryingNV;
}
void PT_CALL glGetArrayObjectfvATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetArrayObjectfvATI;
}
void PT_CALL glGetArrayObjectivATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetArrayObjectivATI;
}
void PT_CALL glGetAttachedObjectsARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetAttachedObjectsARB;
}
void PT_CALL glGetAttachedShaders(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetAttachedShaders;
    fifoOutData(0, (uint32_t)&n, sizeof(int));
    if (arg2)
        memcpy((char *)arg2, &n, sizeof(int));
    if (arg3 && n)
        fifoOutData(ALIGNED(1), arg3, (n > arg2)? (arg2*sizeof(int)):(n*sizeof(int)));
}
uint32_t PT_CALL glGetAttribLocation(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    fifoAddData(0, arg1, ALIGNED((strlen((char *)arg1) + 1)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetAttribLocation;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glGetAttribLocationARB(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    fifoAddData(0, arg1, ALIGNED((strlen((char *)arg1) + 1)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetAttribLocationARB;
    ret = *pt0;
    return ret;
}
void PT_CALL glGetBooleanIndexedvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBooleanIndexedvEXT;
}
void PT_CALL glGetBooleani_v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBooleani_v;
}
void PT_CALL glGetBooleanv(uint32_t arg0, uint32_t arg1) {
    uint32_t n;
    unsigned char cb[32];
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBooleanv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), (uint32_t)cb, n*sizeof(unsigned char));
    memcpy((char *)arg1, cb, n);
}
void PT_CALL glGetBufferParameteri64v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferParameteri64v;
}
void PT_CALL glGetBufferParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferParameteriv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetBufferParameterivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferParameterivARB;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetBufferParameterui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferParameterui64vNV;
}
void PT_CALL glGetBufferPointerv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferPointerv;
}
void PT_CALL glGetBufferPointervARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferPointervARB;
}
void PT_CALL glGetBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t offst = arg1, remain = arg2, ptr = arg3;
    while(remain) {
        uint32_t chunk = (remain > (MGLFBT_SIZE - PAGE_SIZE))? (MGLFBT_SIZE - PAGE_SIZE):remain;
        pt[1] = arg0; pt[2] = offst; pt[3] = chunk; pt[4] = ptr;
        pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferSubData;
        FBTMMCPY((unsigned char *)ptr, &fbtm[(MGLFBT_SIZE - ALIGNED(chunk)) >> 2], chunk);
        offst += chunk;
        ptr += chunk;
        remain -= chunk;
    }
}
void PT_CALL glGetBufferSubDataARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t offst = arg1, remain = arg2, ptr = arg3;
    while(remain) {
        uint32_t chunk = (remain > (MGLFBT_SIZE - PAGE_SIZE))? (MGLFBT_SIZE - PAGE_SIZE):remain;
        pt[1] = arg0; pt[2] = offst; pt[3] = chunk; pt[4] = ptr;
        pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetBufferSubDataARB;
        FBTMMCPY((unsigned char *)ptr, &fbtm[(MGLFBT_SIZE - ALIGNED(chunk)) >> 2], chunk);
        offst += chunk;
        ptr += chunk;
        remain -= chunk;
    }
}
void PT_CALL glGetClipPlane(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetClipPlane;
    fifoOutData(0, arg1, 4*sizeof(double));
}
void PT_CALL glGetClipPlanefOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetClipPlanefOES;
}
void PT_CALL glGetClipPlanexOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetClipPlanexOES;
}
void PT_CALL glGetColorTable(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTable;
}
void PT_CALL glGetColorTableEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableEXT;
}
void PT_CALL glGetColorTableParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableParameterfv;
}
void PT_CALL glGetColorTableParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableParameterfvEXT;
}
void PT_CALL glGetColorTableParameterfvSGI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableParameterfvSGI;
}
void PT_CALL glGetColorTableParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableParameteriv;
}
void PT_CALL glGetColorTableParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableParameterivEXT;
}
void PT_CALL glGetColorTableParameterivSGI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableParameterivSGI;
}
void PT_CALL glGetColorTableSGI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetColorTableSGI;
}
void PT_CALL glGetCombinerInputParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCombinerInputParameterfvNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg4, n*sizeof(float));
}
void PT_CALL glGetCombinerInputParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCombinerInputParameterivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg4, n*sizeof(int));
}
void PT_CALL glGetCombinerOutputParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCombinerOutputParameterfvNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg3, n*sizeof(float));
}
void PT_CALL glGetCombinerOutputParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCombinerOutputParameterivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg3, n*sizeof(int));
}
void PT_CALL glGetCombinerStageParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCombinerStageParameterfvNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetCommandHeaderNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCommandHeaderNV;
}
void PT_CALL glGetCompressedMultiTexImageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCompressedMultiTexImageEXT;
}
void PT_CALL glGetCompressedTexImage(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCompressedTexImage;
    if (pixPackBuf == 0)
        FBTMMCPY((unsigned char *)arg2, &fbtm[ALIGNED(1) >> 2], fbtm[0]);
}
void PT_CALL glGetCompressedTexImageARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCompressedTexImageARB;
    if (pixPackBuf == 0)
        FBTMMCPY((unsigned char *)arg2, &fbtm[ALIGNED(1) >> 2], fbtm[0]);
}
void PT_CALL glGetCompressedTextureImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCompressedTextureImage;
}
void PT_CALL glGetCompressedTextureImageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCompressedTextureImageEXT;
}
void PT_CALL glGetCompressedTextureSubImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCompressedTextureSubImage;
}
void PT_CALL glGetConvolutionFilter(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetConvolutionFilter;
}
void PT_CALL glGetConvolutionFilterEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetConvolutionFilterEXT;
}
void PT_CALL glGetConvolutionParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetConvolutionParameterfv;
}
void PT_CALL glGetConvolutionParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetConvolutionParameterfvEXT;
}
void PT_CALL glGetConvolutionParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetConvolutionParameteriv;
}
void PT_CALL glGetConvolutionParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetConvolutionParameterivEXT;
}
void PT_CALL glGetConvolutionParameterxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetConvolutionParameterxvOES;
}
void PT_CALL glGetCoverageModulationTableNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetCoverageModulationTableNV;
}
void PT_CALL glGetDebugMessageLog(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDebugMessageLog;
}
void PT_CALL glGetDebugMessageLogAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDebugMessageLogAMD;
}
void PT_CALL glGetDebugMessageLogARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDebugMessageLogARB;
}
void PT_CALL glGetDetailTexFuncSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDetailTexFuncSGIS;
}
void PT_CALL glGetDoubleIndexedvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDoubleIndexedvEXT;
}
void PT_CALL glGetDoublei_v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDoublei_v;
}
void PT_CALL glGetDoublei_vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDoublei_vEXT;
}
void PT_CALL glGetDoublev(uint32_t arg0, uint32_t arg1) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetDoublev;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg1, n*sizeof(double));
}
uint32_t PT_CALL glGetError(void) {
    uint32_t ret;
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetError;
    ret = *pt0;
    return ret;
}
void PT_CALL glGetFenceivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFenceivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetFinalCombinerInputParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFinalCombinerInputParameterfvNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetFinalCombinerInputParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFinalCombinerInputParameterivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetFirstPerfQueryIdINTEL(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFirstPerfQueryIdINTEL;
}
void PT_CALL glGetFixedvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFixedvOES;
}
void PT_CALL glGetFloatIndexedvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFloatIndexedvEXT;
}
void PT_CALL glGetFloati_v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFloati_v;
}
void PT_CALL glGetFloati_vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFloati_vEXT;
}
void PT_CALL glGetFloatv(uint32_t arg0, uint32_t arg1) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFloatv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg1, n*sizeof(float));
}
void PT_CALL glGetFogFuncSGIS(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFogFuncSGIS;
}
void PT_CALL glGetFragDataIndex(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFragDataIndex;
}
uint32_t PT_CALL glGetFragDataLocation(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFragDataLocation;
    ret = *pt0;
    return ret;
}
void PT_CALL glGetFragDataLocationEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFragDataLocationEXT;
}
void PT_CALL glGetFragmentLightfvSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFragmentLightfvSGIX;
}
void PT_CALL glGetFragmentLightivSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFragmentLightivSGIX;
}
void PT_CALL glGetFragmentMaterialfvSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFragmentMaterialfvSGIX;
}
void PT_CALL glGetFragmentMaterialivSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFragmentMaterialivSGIX;
}
void PT_CALL glGetFramebufferAttachmentParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFramebufferAttachmentParameteriv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg3, n*sizeof(float));
}
void PT_CALL glGetFramebufferAttachmentParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFramebufferAttachmentParameterivEXT;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg3, n*sizeof(float));
}
void PT_CALL glGetFramebufferParameterfvAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFramebufferParameterfvAMD;
}
void PT_CALL glGetFramebufferParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFramebufferParameteriv;
}
void PT_CALL glGetFramebufferParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetFramebufferParameterivEXT;
}
void PT_CALL glGetGraphicsResetStatus(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetGraphicsResetStatus;
}
void PT_CALL glGetGraphicsResetStatusARB(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetGraphicsResetStatusARB;
}
uint32_t PT_CALL glGetHandleARB(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHandleARB;
    ret = *pt0;
    return ret;
}
void PT_CALL glGetHistogram(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHistogram;
}
void PT_CALL glGetHistogramEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHistogramEXT;
}
void PT_CALL glGetHistogramParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHistogramParameterfv;
}
void PT_CALL glGetHistogramParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHistogramParameterfvEXT;
}
void PT_CALL glGetHistogramParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHistogramParameteriv;
}
void PT_CALL glGetHistogramParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHistogramParameterivEXT;
}
void PT_CALL glGetHistogramParameterxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetHistogramParameterxvOES;
}
void PT_CALL glGetImageHandleARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetImageHandleARB;
}
void PT_CALL glGetImageHandleNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetImageHandleNV;
}
void PT_CALL glGetImageTransformParameterfvHP(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetImageTransformParameterfvHP;
}
void PT_CALL glGetImageTransformParameterivHP(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetImageTransformParameterivHP;
}
void PT_CALL glGetInfoLogARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t len;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInfoLogARB;
    fifoOutData(0, (uint32_t)&len, sizeof(uint32_t));
    len = (len > arg1)? arg1:len;
    fifoOutData(0, arg3, len);
}
void PT_CALL glGetInstrumentsSGIX(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInstrumentsSGIX;
}
void PT_CALL glGetInteger64i_v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInteger64i_v;
}
void PT_CALL glGetInteger64v(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInteger64v;
}
void PT_CALL glGetIntegerIndexedvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetIntegerIndexedvEXT;
}
void PT_CALL glGetIntegeri_v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetIntegeri_v;
}
void PT_CALL glGetIntegerui64i_vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetIntegerui64i_vNV;
}
void PT_CALL glGetIntegerui64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetIntegerui64vNV;
}
void PT_CALL glGetIntegerv(uint32_t arg0, uint32_t arg1) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetIntegerv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(int)), arg1, n*sizeof(int));
    if ((arg0 == GL_NUM_EXTENSIONS) &&
        !memcmp(vernstr, "4.1 Metal", strlen("4.1 Metal")))
        *((int *)arg1) += 1;
}
void PT_CALL glGetInternalformatSampleivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInternalformatSampleivNV;
}
void PT_CALL glGetInternalformati64v(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInternalformati64v;
}
void PT_CALL glGetInternalformativ(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInternalformativ;
    fifoOutData(0, arg4, arg3*sizeof(int));
}
void PT_CALL glGetInvariantBooleanvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInvariantBooleanvEXT;
}
void PT_CALL glGetInvariantFloatvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInvariantFloatvEXT;
}
void PT_CALL glGetInvariantIntegervEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetInvariantIntegervEXT;
}
void PT_CALL glGetLightfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetLightfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetLightiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetLightiv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetLightxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetLightxOES;
}
void PT_CALL glGetListParameterfvSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetListParameterfvSGIX;
}
void PT_CALL glGetListParameterivSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetListParameterivSGIX;
}
void PT_CALL glGetLocalConstantBooleanvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetLocalConstantBooleanvEXT;
}
void PT_CALL glGetLocalConstantFloatvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetLocalConstantFloatvEXT;
}
void PT_CALL glGetLocalConstantIntegervEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetLocalConstantIntegervEXT;
}
void PT_CALL glGetMapAttribParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapAttribParameterfvNV;
}
void PT_CALL glGetMapAttribParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapAttribParameterivNV;
}
void PT_CALL glGetMapControlPointsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapControlPointsNV;
}
void PT_CALL glGetMapParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapParameterfvNV;
}
void PT_CALL glGetMapParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapParameterivNV;
}
void PT_CALL glGetMapdv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapdv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(double));
}
void PT_CALL glGetMapfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetMapiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapiv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetMapxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMapxvOES;
}
void PT_CALL glGetMaterialfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMaterialfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetMaterialiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMaterialiv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetMaterialxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMaterialxOES;
}
void PT_CALL glGetMemoryObjectDetachedResourcesuivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMemoryObjectDetachedResourcesuivNV;
}
void PT_CALL glGetMemoryObjectParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMemoryObjectParameterivEXT;
}
void PT_CALL glGetMinmax(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMinmax;
}
void PT_CALL glGetMinmaxEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMinmaxEXT;
}
void PT_CALL glGetMinmaxParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMinmaxParameterfv;
}
void PT_CALL glGetMinmaxParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMinmaxParameterfvEXT;
}
void PT_CALL glGetMinmaxParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMinmaxParameteriv;
}
void PT_CALL glGetMinmaxParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMinmaxParameterivEXT;
}
void PT_CALL glGetMultiTexEnvfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexEnvfvEXT;
}
void PT_CALL glGetMultiTexEnvivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexEnvivEXT;
}
void PT_CALL glGetMultiTexGendvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexGendvEXT;
}
void PT_CALL glGetMultiTexGenfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexGenfvEXT;
}
void PT_CALL glGetMultiTexGenivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexGenivEXT;
}
void PT_CALL glGetMultiTexImageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexImageEXT;
}
void PT_CALL glGetMultiTexLevelParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexLevelParameterfvEXT;
}
void PT_CALL glGetMultiTexLevelParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexLevelParameterivEXT;
}
void PT_CALL glGetMultiTexParameterIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexParameterIivEXT;
}
void PT_CALL glGetMultiTexParameterIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexParameterIuivEXT;
}
void PT_CALL glGetMultiTexParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexParameterfvEXT;
}
void PT_CALL glGetMultiTexParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultiTexParameterivEXT;
}
void PT_CALL glGetMultisamplefv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultisamplefv;
}
void PT_CALL glGetMultisamplefvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetMultisamplefvNV;
}
void PT_CALL glGetNamedBufferParameteri64v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferParameteri64v;
}
void PT_CALL glGetNamedBufferParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferParameteriv;
}
void PT_CALL glGetNamedBufferParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferParameterivEXT;
}
void PT_CALL glGetNamedBufferParameterui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferParameterui64vNV;
}
void PT_CALL glGetNamedBufferPointerv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferPointerv;
}
void PT_CALL glGetNamedBufferPointervEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferPointervEXT;
}
void PT_CALL glGetNamedBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferSubData;
}
void PT_CALL glGetNamedBufferSubDataEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedBufferSubDataEXT;
}
void PT_CALL glGetNamedFramebufferAttachmentParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedFramebufferAttachmentParameteriv;
}
void PT_CALL glGetNamedFramebufferAttachmentParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedFramebufferAttachmentParameterivEXT;
}
void PT_CALL glGetNamedFramebufferParameterfvAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedFramebufferParameterfvAMD;
}
void PT_CALL glGetNamedFramebufferParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedFramebufferParameteriv;
}
void PT_CALL glGetNamedFramebufferParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedFramebufferParameterivEXT;
}
void PT_CALL glGetNamedProgramLocalParameterIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedProgramLocalParameterIivEXT;
}
void PT_CALL glGetNamedProgramLocalParameterIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedProgramLocalParameterIuivEXT;
}
void PT_CALL glGetNamedProgramLocalParameterdvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedProgramLocalParameterdvEXT;
}
void PT_CALL glGetNamedProgramLocalParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedProgramLocalParameterfvEXT;
}
void PT_CALL glGetNamedProgramStringEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedProgramStringEXT;
}
void PT_CALL glGetNamedProgramivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedProgramivEXT;
}
void PT_CALL glGetNamedRenderbufferParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedRenderbufferParameteriv;
}
void PT_CALL glGetNamedRenderbufferParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedRenderbufferParameterivEXT;
}
void PT_CALL glGetNamedStringARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedStringARB;
}
void PT_CALL glGetNamedStringivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNamedStringivARB;
}
void PT_CALL glGetNextPerfQueryIdINTEL(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetNextPerfQueryIdINTEL;
}
void PT_CALL glGetObjectBufferfvATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectBufferfvATI;
}
void PT_CALL glGetObjectBufferivATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectBufferivATI;
}
void PT_CALL glGetObjectLabel(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectLabel;
}
void PT_CALL glGetObjectLabelEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectLabelEXT;
}
void PT_CALL glGetObjectParameterfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectParameterfvARB;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetObjectParameterivAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectParameterivAPPLE;
}
void PT_CALL glGetObjectParameterivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectParameterivARB;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetObjectPtrLabel(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetObjectPtrLabel;
}
void PT_CALL glGetOcclusionQueryivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetOcclusionQueryivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetOcclusionQueryuivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetOcclusionQueryuivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetPathColorGenfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathColorGenfvNV;
}
void PT_CALL glGetPathColorGenivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathColorGenivNV;
}
void PT_CALL glGetPathCommandsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathCommandsNV;
}
void PT_CALL glGetPathCoordsNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathCoordsNV;
}
void PT_CALL glGetPathDashArrayNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathDashArrayNV;
}
void PT_CALL glGetPathLengthNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathLengthNV;
}
void PT_CALL glGetPathMetricRangeNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathMetricRangeNV;
}
void PT_CALL glGetPathMetricsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathMetricsNV;
}
void PT_CALL glGetPathParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathParameterfvNV;
}
void PT_CALL glGetPathParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathParameterivNV;
}
void PT_CALL glGetPathSpacingNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathSpacingNV;
}
void PT_CALL glGetPathTexGenfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathTexGenfvNV;
}
void PT_CALL glGetPathTexGenivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPathTexGenivNV;
}
void PT_CALL glGetPerfCounterInfoINTEL(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfCounterInfoINTEL;
}
void PT_CALL glGetPerfMonitorCounterDataAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfMonitorCounterDataAMD;
}
void PT_CALL glGetPerfMonitorCounterInfoAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfMonitorCounterInfoAMD;
}
void PT_CALL glGetPerfMonitorCounterStringAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfMonitorCounterStringAMD;
}
void PT_CALL glGetPerfMonitorCountersAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfMonitorCountersAMD;
}
void PT_CALL glGetPerfMonitorGroupStringAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfMonitorGroupStringAMD;
}
void PT_CALL glGetPerfMonitorGroupsAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfMonitorGroupsAMD;
}
void PT_CALL glGetPerfQueryDataINTEL(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfQueryDataINTEL;
}
void PT_CALL glGetPerfQueryIdByNameINTEL(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfQueryIdByNameINTEL;
}
void PT_CALL glGetPerfQueryInfoINTEL(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPerfQueryInfoINTEL;
}
void PT_CALL glGetPixelMapfv(uint32_t arg0, uint32_t arg1) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelMapfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg1, n*sizeof(float));
}
void PT_CALL glGetPixelMapuiv(uint32_t arg0, uint32_t arg1) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelMapuiv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg1, n*sizeof(int));
}
void PT_CALL glGetPixelMapusv(uint32_t arg0, uint32_t arg1) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelMapusv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg1, n*sizeof(unsigned short));
}
void PT_CALL glGetPixelMapxv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelMapxv;
}
void PT_CALL glGetPixelTexGenParameterfvSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelTexGenParameterfvSGIS;
}
void PT_CALL glGetPixelTexGenParameterivSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelTexGenParameterivSGIS;
}
void PT_CALL glGetPixelTransformParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelTransformParameterfvEXT;
}
void PT_CALL glGetPixelTransformParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPixelTransformParameterivEXT;
}
void PT_CALL glGetPointerIndexedvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPointerIndexedvEXT;
}
void PT_CALL glGetPointeri_vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPointeri_vEXT;
}
void PT_CALL glGetPointerv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPointerv;
}
void PT_CALL glGetPointervEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPointervEXT;
}
void PT_CALL glGetPolygonStipple(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetPolygonStipple;
}
void PT_CALL glGetProgramBinary(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramBinary;
}
void PT_CALL glGetProgramEnvParameterIivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramEnvParameterIivNV;
}
void PT_CALL glGetProgramEnvParameterIuivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramEnvParameterIuivNV;
}
void PT_CALL glGetProgramEnvParameterdvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramEnvParameterdvARB;
}
void PT_CALL glGetProgramEnvParameterfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramEnvParameterfvARB;
}
void PT_CALL glGetProgramInfoLog(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t len;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramInfoLog;
    fifoOutData(0, (uint32_t)&len, sizeof(uint32_t));
    len = (len > arg1)? arg1:len;
    fifoOutData(0, arg3, len);
}
void PT_CALL glGetProgramInterfaceiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramInterfaceiv;
}
void PT_CALL glGetProgramLocalParameterIivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramLocalParameterIivNV;
}
void PT_CALL glGetProgramLocalParameterIuivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramLocalParameterIuivNV;
}
void PT_CALL glGetProgramLocalParameterdvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramLocalParameterdvARB;
}
void PT_CALL glGetProgramLocalParameterfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramLocalParameterfvARB;
}
void PT_CALL glGetProgramNamedParameterdvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramNamedParameterdvNV;
}
void PT_CALL glGetProgramNamedParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramNamedParameterfvNV;
}
void PT_CALL glGetProgramParameterdvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramParameterdvNV;
}
void PT_CALL glGetProgramParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramParameterfvNV;
}
void PT_CALL glGetProgramPipelineInfoLog(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramPipelineInfoLog;
}
void PT_CALL glGetProgramPipelineiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramPipelineiv;
}
void PT_CALL glGetProgramResourceIndex(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramResourceIndex;
}
void PT_CALL glGetProgramResourceLocation(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramResourceLocation;
}
void PT_CALL glGetProgramResourceLocationIndex(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramResourceLocationIndex;
}
void PT_CALL glGetProgramResourceName(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramResourceName;
}
void PT_CALL glGetProgramResourcefvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramResourcefvNV;
}
void PT_CALL glGetProgramResourceiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramResourceiv;
}
void PT_CALL glGetProgramStageiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramStageiv;
}
void PT_CALL glGetProgramStringARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramStringARB;
}
void PT_CALL glGetProgramStringNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramStringNV;
}
void PT_CALL glGetProgramSubroutineParameteruivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramSubroutineParameteruivNV;
}
void PT_CALL glGetProgramiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramiv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetProgramivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramivARB;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetProgramivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetProgramivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetQueryBufferObjecti64v(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryBufferObjecti64v;
}
void PT_CALL glGetQueryBufferObjectiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryBufferObjectiv;
}
void PT_CALL glGetQueryBufferObjectui64v(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryBufferObjectui64v;
}
void PT_CALL glGetQueryBufferObjectuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryBufferObjectuiv;
}
void PT_CALL glGetQueryIndexediv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryIndexediv;
}
void PT_CALL glGetQueryObjecti64v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjecti64v;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(uint64_t));
    }
}
void PT_CALL glGetQueryObjecti64vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjecti64vEXT;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(uint64_t));
    }
}
void PT_CALL glGetQueryObjectiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjectiv;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
    }
}
void PT_CALL glGetQueryObjectivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjectivARB;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
    }
}
void PT_CALL glGetQueryObjectui64v(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjectui64v;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(uint64_t));
    }
}
void PT_CALL glGetQueryObjectui64vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjectui64vEXT;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(uint64_t));
    }
}
void PT_CALL glGetQueryObjectuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjectuiv;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
    }
}
void PT_CALL glGetQueryObjectuivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryObjectuivARB;
    if (queryBuf == 0) {
        uint32_t n;
        fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
        fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
    }
}
void PT_CALL glGetQueryiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryiv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetQueryivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetQueryivARB;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetRenderbufferParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetRenderbufferParameteriv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetRenderbufferParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetRenderbufferParameterivEXT;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetSamplerParameterIiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSamplerParameterIiv;
}
void PT_CALL glGetSamplerParameterIuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSamplerParameterIuiv;
}
void PT_CALL glGetSamplerParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSamplerParameterfv;
}
void PT_CALL glGetSamplerParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSamplerParameteriv;
}
void PT_CALL glGetSemaphoreParameterui64vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSemaphoreParameterui64vEXT;
}
void PT_CALL glGetSeparableFilter(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSeparableFilter;
}
void PT_CALL glGetSeparableFilterEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSeparableFilterEXT;
}
void PT_CALL glGetShaderInfoLog(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t len;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetShaderInfoLog;
    fifoOutData(0, (uint32_t)&len, sizeof(uint32_t));
    len = (len > arg1)? arg1:len;
    fifoOutData(0, arg3, len);
}
void PT_CALL glGetShaderPrecisionFormat(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetShaderPrecisionFormat;
}
void PT_CALL glGetShaderSource(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetShaderSource;
}
void PT_CALL glGetShaderSourceARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetShaderSourceARB;
}
void PT_CALL glGetShaderiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetShaderiv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetShadingRateImagePaletteNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetShadingRateImagePaletteNV;
}
void PT_CALL glGetShadingRateSampleLocationivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetShadingRateSampleLocationivNV;
}
void PT_CALL glGetSharpenTexFuncSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSharpenTexFuncSGIS;
}
void PT_CALL glGetStageIndexNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetStageIndexNV;
}
uint8_t * PT_CALL glGetString(uint32_t arg0) {
    static char ARBperr[256];
    static const char *cstrTbl[] = {
        vendstr, rendstr, vernstr, extnstr, glslstr, ARBperr,
    };
    static const int cstrsz[] = {
        sizeof(vendstr) - 1,
        sizeof(rendstr) - 1,
        sizeof(vernstr) - 1,
        sizeof(extnstr) - 1,
        sizeof(glslstr) - 1,
        sizeof(ARBperr) - 1,
    };
    struct mglOptions cfg;
    int sel;

    if (!currGLRC)
        return 0;

    switch(arg0) {
        case GL_EXTENSIONS:
            parse_options(&cfg);
            fifoAddData(0, (uint32_t)&cfg.xstrYear, sizeof(int));
            /* fall through */
        case GL_VENDOR:
        case GL_RENDERER:
        case GL_VERSION:
            sel = arg0 & 0x03U;
            break;
        case GL_SHADING_LANGUAGE_VERSION:
            sel = 4;
            break;
        case GL_PROGRAM_ERROR_STRING_ARB:
            sel = 5;
            break;
        default:
            return 0;
    }

    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetString;
    fifoOutData(0, (uint32_t)cstrTbl[sel], cstrsz[sel]);
    if (sel == 0x03U)
        fltrxstr(cstrTbl[sel], cstrsz[sel], (cfg.xstrYear)? "+GL_":0);
    //DPRINTF("%s [ %04x ]", cstrTbl[sel], arg0);
    return (uint8_t *)cstrTbl[sel];
}
uint8_t * PT_CALL glGetStringi(uint32_t arg0, uint32_t arg1) {
    static char str[256];
    uint32_t n, nexts;
    if ((arg0 == GL_EXTENSIONS) &&
        !memcmp(vernstr, "4.1 Metal", strlen("4.1 Metal"))) {
        glGetIntegerv(GL_NUM_EXTENSIONS, (uint32_t)&nexts);
        if (arg1 == (nexts - 1)) {
            strcpy(str, "GL_ARB_debug_output");
            return (uint8_t *)str;
        }
    }
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetStringi;
    fifoOutData(0, (uint32_t)&n, sizeof(int));
    fifoOutData(sizeof(int), (uint32_t)str, n);
    //DPRINTF("GetStringi() %04x %s", arg1, str);
    return (uint8_t *)str;
}
void PT_CALL glGetSubroutineIndex(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSubroutineIndex;
}
void PT_CALL glGetSubroutineUniformLocation(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSubroutineUniformLocation;
}
void PT_CALL glGetSynciv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetSynciv;
}
void PT_CALL glGetTexBumpParameterfvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexBumpParameterfvATI;
}
void PT_CALL glGetTexBumpParameterivATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexBumpParameterivATI;
}
void PT_CALL glGetTexEnvfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexEnvfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetTexEnviv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexEnviv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetTexEnvxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexEnvxvOES;
}
void PT_CALL glGetTexFilterFuncSGIS(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexFilterFuncSGIS;
}
void PT_CALL glGetTexGendv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexGendv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(double));
}
void PT_CALL glGetTexGenfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexGenfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetTexGeniv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexGeniv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetTexGenxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexGenxvOES;
}
void PT_CALL glGetTexImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexImage;
    if (pixPackBuf == 0)
        FBTMMCPY((unsigned char *)arg4, &fbtm[ALIGNED(1) >> 2], fbtm[0]);
}
void PT_CALL glGetTexLevelParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexLevelParameterfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg3, n*sizeof(float));
}
void PT_CALL glGetTexLevelParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexLevelParameteriv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg3, n*sizeof(int));
}
void PT_CALL glGetTexLevelParameterxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexLevelParameterxvOES;
}
void PT_CALL glGetTexParameterIiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameterIiv;
}
void PT_CALL glGetTexParameterIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameterIivEXT;
}
void PT_CALL glGetTexParameterIuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameterIuiv;
}
void PT_CALL glGetTexParameterIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameterIuivEXT;
}
void PT_CALL glGetTexParameterPointervAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameterPointervAPPLE;
}
void PT_CALL glGetTexParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameterfv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(float));
}
void PT_CALL glGetTexParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameteriv;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg2, n*sizeof(int));
}
void PT_CALL glGetTexParameterxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTexParameterxvOES;
}
void PT_CALL glGetTextureHandleARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureHandleARB;
}
void PT_CALL glGetTextureHandleNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureHandleNV;
}
void PT_CALL glGetTextureImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureImage;
}
void PT_CALL glGetTextureImageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureImageEXT;
}
void PT_CALL glGetTextureLevelParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureLevelParameterfv;
}
void PT_CALL glGetTextureLevelParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureLevelParameterfvEXT;
}
void PT_CALL glGetTextureLevelParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureLevelParameteriv;
}
void PT_CALL glGetTextureLevelParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureLevelParameterivEXT;
}
void PT_CALL glGetTextureParameterIiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameterIiv;
}
void PT_CALL glGetTextureParameterIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameterIivEXT;
}
void PT_CALL glGetTextureParameterIuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameterIuiv;
}
void PT_CALL glGetTextureParameterIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameterIuivEXT;
}
void PT_CALL glGetTextureParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameterfv;
}
void PT_CALL glGetTextureParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameterfvEXT;
}
void PT_CALL glGetTextureParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameteriv;
}
void PT_CALL glGetTextureParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureParameterivEXT;
}
void PT_CALL glGetTextureSamplerHandleARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureSamplerHandleARB;
}
void PT_CALL glGetTextureSamplerHandleNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureSamplerHandleNV;
}
void PT_CALL glGetTextureSubImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTextureSubImage;
}
void PT_CALL glGetTrackMatrixivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t n;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTrackMatrixivNV;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(ALIGNED(sizeof(uint32_t)), arg3, n*sizeof(int));
}
void PT_CALL glGetTransformFeedbackVarying(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    uint32_t n, e;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTransformFeedbackVarying;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(2*ALIGNED(1), (uint32_t)&e, sizeof(uint32_t));
    if (e) {
        if (arg3)
            memcpy((char *)arg3, &n, sizeof(uint32_t));
        memcpy((char *)arg5, &e, sizeof(uint32_t));
        fifoOutData(ALIGNED(1), arg4, sizeof(uint32_t));
        fifoOutData(3*ALIGNED(1), arg6, ((n + 1) > arg2)? arg2:(n + 1));
    }
}
void PT_CALL glGetTransformFeedbackVaryingEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    uint32_t n, e;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTransformFeedbackVaryingEXT;
    fifoOutData(0, (uint32_t)&n, sizeof(uint32_t));
    fifoOutData(2*ALIGNED(1), (uint32_t)&e, sizeof(uint32_t));
    if (e) {
        if (arg3)
            memcpy((char *)arg3, &n, sizeof(uint32_t));
        memcpy((char *)arg5, &e, sizeof(uint32_t));
        fifoOutData(ALIGNED(1), arg4, sizeof(uint32_t));
        fifoOutData(3*ALIGNED(1), arg6, ((n + 1) > arg2)? arg2:(n + 1));
    }
}
void PT_CALL glGetTransformFeedbackVaryingNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTransformFeedbackVaryingNV;
}
void PT_CALL glGetTransformFeedbacki64_v(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTransformFeedbacki64_v;
}
void PT_CALL glGetTransformFeedbacki_v(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTransformFeedbacki_v;
}
void PT_CALL glGetTransformFeedbackiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetTransformFeedbackiv;
}
uint32_t PT_CALL glGetUniformBlockIndex(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    fifoAddData(0, arg1, ALIGNED((strlen((char *)arg1) + 1)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformBlockIndex;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glGetUniformBufferSizeEXT(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformBufferSizeEXT;
    ret = *pt0;
    return ret;
}
void PT_CALL glGetUniformIndices(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformIndices;
}
uint32_t PT_CALL glGetUniformLocation(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    fifoAddData(0, arg1, ALIGNED((strlen((char *)arg1) + 1)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformLocation;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glGetUniformLocationARB(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    fifoAddData(0, arg1, ALIGNED((strlen((char *)arg1) + 1)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformLocationARB;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glGetUniformOffsetEXT(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformOffsetEXT;
    ret = *pt0;
    return ret;
}
void PT_CALL glGetUniformSubroutineuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformSubroutineuiv;
}
void PT_CALL glGetUniformdv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformdv;
}
void PT_CALL glGetUniformfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformfv;
}
void PT_CALL glGetUniformfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformfvARB;
}
void PT_CALL glGetUniformi64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformi64vARB;
}
void PT_CALL glGetUniformi64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformi64vNV;
}
void PT_CALL glGetUniformiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformiv;
}
void PT_CALL glGetUniformivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformivARB;
}
void PT_CALL glGetUniformui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformui64vARB;
}
void PT_CALL glGetUniformui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformui64vNV;
}
void PT_CALL glGetUniformuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformuiv;
}
void PT_CALL glGetUniformuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUniformuivEXT;
}
void PT_CALL glGetUnsignedBytei_vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUnsignedBytei_vEXT;
}
void PT_CALL glGetUnsignedBytevEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetUnsignedBytevEXT;
}
void PT_CALL glGetVariantArrayObjectfvATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVariantArrayObjectfvATI;
}
void PT_CALL glGetVariantArrayObjectivATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVariantArrayObjectivATI;
}
void PT_CALL glGetVariantBooleanvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVariantBooleanvEXT;
}
void PT_CALL glGetVariantFloatvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVariantFloatvEXT;
}
void PT_CALL glGetVariantIntegervEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVariantIntegervEXT;
}
void PT_CALL glGetVariantPointervEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVariantPointervEXT;
}
void PT_CALL glGetVaryingLocationNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVaryingLocationNV;
}
void PT_CALL glGetVertexArrayIndexed64iv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexArrayIndexed64iv;
}
void PT_CALL glGetVertexArrayIndexediv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexArrayIndexediv;
}
void PT_CALL glGetVertexArrayIntegeri_vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexArrayIntegeri_vEXT;
}
void PT_CALL glGetVertexArrayIntegervEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexArrayIntegervEXT;
}
void PT_CALL glGetVertexArrayPointeri_vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexArrayPointeri_vEXT;
}
void PT_CALL glGetVertexArrayPointervEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexArrayPointervEXT;
}
void PT_CALL glGetVertexArrayiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexArrayiv;
}
void PT_CALL glGetVertexAttribArrayObjectfvATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribArrayObjectfvATI;
}
void PT_CALL glGetVertexAttribArrayObjectivATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribArrayObjectivATI;
}
void PT_CALL glGetVertexAttribIiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribIiv;
}
void PT_CALL glGetVertexAttribIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribIivEXT;
}
void PT_CALL glGetVertexAttribIuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribIuiv;
}
void PT_CALL glGetVertexAttribIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribIuivEXT;
}
void PT_CALL glGetVertexAttribLdv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribLdv;
}
void PT_CALL glGetVertexAttribLdvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribLdvEXT;
}
void PT_CALL glGetVertexAttribLi64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribLi64vNV;
}
void PT_CALL glGetVertexAttribLui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribLui64vARB;
}
void PT_CALL glGetVertexAttribLui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribLui64vNV;
}
void PT_CALL glGetVertexAttribPointerv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribPointerv;
}
void PT_CALL glGetVertexAttribPointervARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribPointervARB;
}
void PT_CALL glGetVertexAttribPointervNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribPointervNV;
}
void PT_CALL glGetVertexAttribdv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribdv;
}
void PT_CALL glGetVertexAttribdvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribdvARB;
}
void PT_CALL glGetVertexAttribdvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribdvNV;
}
void PT_CALL glGetVertexAttribfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribfv;
}
void PT_CALL glGetVertexAttribfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribfvARB;
}
void PT_CALL glGetVertexAttribfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribfvNV;
}
void PT_CALL glGetVertexAttribiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribiv;
}
void PT_CALL glGetVertexAttribivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribivARB;
}
void PT_CALL glGetVertexAttribivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVertexAttribivNV;
}
void PT_CALL glGetVideoCaptureStreamdvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideoCaptureStreamdvNV;
}
void PT_CALL glGetVideoCaptureStreamfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideoCaptureStreamfvNV;
}
void PT_CALL glGetVideoCaptureStreamivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideoCaptureStreamivNV;
}
void PT_CALL glGetVideoCaptureivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideoCaptureivNV;
}
void PT_CALL glGetVideoi64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideoi64vNV;
}
void PT_CALL glGetVideoivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideoivNV;
}
void PT_CALL glGetVideoui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideoui64vNV;
}
void PT_CALL glGetVideouivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVideouivNV;
}
void PT_CALL glGetVkProcAddrNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetVkProcAddrNV;
}
void PT_CALL glGetnColorTable(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnColorTable;
}
void PT_CALL glGetnColorTableARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnColorTableARB;
}
void PT_CALL glGetnCompressedTexImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnCompressedTexImage;
}
void PT_CALL glGetnCompressedTexImageARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnCompressedTexImageARB;
}
void PT_CALL glGetnConvolutionFilter(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnConvolutionFilter;
}
void PT_CALL glGetnConvolutionFilterARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnConvolutionFilterARB;
}
void PT_CALL glGetnHistogram(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnHistogram;
}
void PT_CALL glGetnHistogramARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnHistogramARB;
}
void PT_CALL glGetnMapdv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMapdv;
}
void PT_CALL glGetnMapdvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMapdvARB;
}
void PT_CALL glGetnMapfv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMapfv;
}
void PT_CALL glGetnMapfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMapfvARB;
}
void PT_CALL glGetnMapiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMapiv;
}
void PT_CALL glGetnMapivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMapivARB;
}
void PT_CALL glGetnMinmax(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMinmax;
}
void PT_CALL glGetnMinmaxARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnMinmaxARB;
}
void PT_CALL glGetnPixelMapfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPixelMapfv;
}
void PT_CALL glGetnPixelMapfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPixelMapfvARB;
}
void PT_CALL glGetnPixelMapuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPixelMapuiv;
}
void PT_CALL glGetnPixelMapuivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPixelMapuivARB;
}
void PT_CALL glGetnPixelMapusv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPixelMapusv;
}
void PT_CALL glGetnPixelMapusvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPixelMapusvARB;
}
void PT_CALL glGetnPolygonStipple(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPolygonStipple;
}
void PT_CALL glGetnPolygonStippleARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnPolygonStippleARB;
}
void PT_CALL glGetnSeparableFilter(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnSeparableFilter;
}
void PT_CALL glGetnSeparableFilterARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnSeparableFilterARB;
}
void PT_CALL glGetnTexImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnTexImage;
}
void PT_CALL glGetnTexImageARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnTexImageARB;
}
void PT_CALL glGetnUniformdv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformdv;
}
void PT_CALL glGetnUniformdvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformdvARB;
}
void PT_CALL glGetnUniformfv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformfv;
}
void PT_CALL glGetnUniformfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformfvARB;
}
void PT_CALL glGetnUniformi64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformi64vARB;
}
void PT_CALL glGetnUniformiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformiv;
}
void PT_CALL glGetnUniformivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformivARB;
}
void PT_CALL glGetnUniformui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformui64vARB;
}
void PT_CALL glGetnUniformuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformuiv;
}
void PT_CALL glGetnUniformuivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGetnUniformuivARB;
}
void PT_CALL glGlobalAlphaFactorbSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactorbSUN;
}
void PT_CALL glGlobalAlphaFactordSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactordSUN;
}
void PT_CALL glGlobalAlphaFactorfSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactorfSUN;
}
void PT_CALL glGlobalAlphaFactoriSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactoriSUN;
}
void PT_CALL glGlobalAlphaFactorsSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactorsSUN;
}
void PT_CALL glGlobalAlphaFactorubSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactorubSUN;
}
void PT_CALL glGlobalAlphaFactoruiSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactoruiSUN;
}
void PT_CALL glGlobalAlphaFactorusSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glGlobalAlphaFactorusSUN;
}
void PT_CALL glHint(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glHint, 2);
}
void PT_CALL glHintPGI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glHintPGI;
}
void PT_CALL glHistogram(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glHistogram;
}
void PT_CALL glHistogramEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glHistogramEXT;
}
void PT_CALL glIglooInterfaceSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIglooInterfaceSGIX;
}
void PT_CALL glImageTransformParameterfHP(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImageTransformParameterfHP;
}
void PT_CALL glImageTransformParameterfvHP(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImageTransformParameterfvHP;
}
void PT_CALL glImageTransformParameteriHP(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImageTransformParameteriHP;
}
void PT_CALL glImageTransformParameterivHP(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImageTransformParameterivHP;
}
void PT_CALL glImportMemoryFdEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImportMemoryFdEXT;
}
void PT_CALL glImportMemoryWin32HandleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImportMemoryWin32HandleEXT;
}
void PT_CALL glImportMemoryWin32NameEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImportMemoryWin32NameEXT;
}
void PT_CALL glImportSemaphoreFdEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImportSemaphoreFdEXT;
}
void PT_CALL glImportSemaphoreWin32HandleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImportSemaphoreWin32HandleEXT;
}
void PT_CALL glImportSemaphoreWin32NameEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImportSemaphoreWin32NameEXT;
}
void PT_CALL glImportSyncEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glImportSyncEXT;
}
void PT_CALL glIndexFormatNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIndexFormatNV;
}
void PT_CALL glIndexFuncEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIndexFuncEXT;
}
void PT_CALL glIndexMask(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexMask, 1);
}
void PT_CALL glIndexMaterialEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIndexMaterialEXT;
}
void PT_CALL glIndexPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexPointer, 3);
    vtxarry_init(&vtxArry.Index, 1, arg0, arg1, (void *)arg2);
}
void PT_CALL glIndexPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexPointerEXT, 4);
    vtxarry_init(&vtxArry.Index, 1, arg0, arg1, (void *)arg3);
}
void PT_CALL glIndexPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIndexPointerListIBM;
}
void PT_CALL glIndexd(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexd, 2);
}
void PT_CALL glIndexdv(uint32_t arg0) {
    fifoAddData(0, arg0, sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexdv, 1);
}
void PT_CALL glIndexf(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexf, 1);
}
void PT_CALL glIndexfv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexfv, 1);
}
void PT_CALL glIndexi(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexi, 1);
}
void PT_CALL glIndexiv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexiv, 1);
}
void PT_CALL glIndexs(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexs, 1);
}
void PT_CALL glIndexsv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexsv, 1);
}
void PT_CALL glIndexub(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexub, 1);
}
void PT_CALL glIndexubv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(unsigned char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glIndexubv, 1);
}
void PT_CALL glIndexxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIndexxOES;
}
void PT_CALL glIndexxvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIndexxvOES;
}
void PT_CALL glInitNames(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glInitNames, 0);
}
void PT_CALL glInsertComponentEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInsertComponentEXT;
}
void PT_CALL glInsertEventMarkerEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInsertEventMarkerEXT;
}
void PT_CALL glInstrumentsBufferSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInstrumentsBufferSGIX;
}
void PT_CALL glInterleavedArrays(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glInterleavedArrays, 3);
    vtxarry_init(&Interleaved, szgldata(arg0, 0), 0, arg1, (void *)arg2);
    Interleaved.enable = 1;
}
void PT_CALL glInterpolatePathsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInterpolatePathsNV;
}
void PT_CALL glInvalidateBufferData(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateBufferData;
}
void PT_CALL glInvalidateBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateBufferSubData;
}
void PT_CALL glInvalidateFramebuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateFramebuffer;
}
void PT_CALL glInvalidateNamedFramebufferData(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateNamedFramebufferData;
}
void PT_CALL glInvalidateNamedFramebufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateNamedFramebufferSubData;
}
void PT_CALL glInvalidateSubFramebuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateSubFramebuffer;
}
void PT_CALL glInvalidateTexImage(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateTexImage;
}
void PT_CALL glInvalidateTexSubImage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glInvalidateTexSubImage;
}
void PT_CALL glIsAsyncMarkerSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsAsyncMarkerSGIX;
}
uint32_t PT_CALL glIsBuffer(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsBuffer;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsBufferARB(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsBufferARB;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsBufferResidentNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsBufferResidentNV;
}
void PT_CALL glIsCommandListNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsCommandListNV;
}
uint32_t PT_CALL glIsEnabled(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsEnabled;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsEnabledIndexedEXT(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsEnabledIndexedEXT;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsEnabledi(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsEnabledi;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsFenceAPPLE(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsFenceAPPLE;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsFenceNV(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsFenceNV;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsFramebuffer(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsFramebuffer;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsFramebufferEXT(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsFramebufferEXT;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsImageHandleResidentARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsImageHandleResidentARB;
}
void PT_CALL glIsImageHandleResidentNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsImageHandleResidentNV;
}
uint32_t PT_CALL glIsList(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsList;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsMemoryObjectEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsMemoryObjectEXT;
}
void PT_CALL glIsNameAMD(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsNameAMD;
}
void PT_CALL glIsNamedBufferResidentNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsNamedBufferResidentNV;
}
void PT_CALL glIsNamedStringARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsNamedStringARB;
}
void PT_CALL glIsObjectBufferATI(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsObjectBufferATI;
}
uint32_t PT_CALL glIsOcclusionQueryNV(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsOcclusionQueryNV;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsPathNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsPathNV;
}
void PT_CALL glIsPointInFillPathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsPointInFillPathNV;
}
void PT_CALL glIsPointInStrokePathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsPointInStrokePathNV;
}
uint32_t PT_CALL glIsProgram(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsProgram;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsProgramARB(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsProgramARB;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsProgramNV(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsProgramNV;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsProgramPipeline(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsProgramPipeline;
}
uint32_t PT_CALL glIsQuery(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsQuery;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsQueryARB(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsQueryARB;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsRenderbuffer(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsRenderbuffer;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsRenderbufferEXT(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsRenderbufferEXT;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsSampler(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsSampler;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsSemaphoreEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsSemaphoreEXT;
}
uint32_t PT_CALL glIsShader(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsShader;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsStateNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsStateNV;
}
void PT_CALL glIsSync(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsSync;
}
uint32_t PT_CALL glIsTexture(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsTexture;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glIsTextureEXT(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsTextureEXT;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsTextureHandleResidentARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsTextureHandleResidentARB;
}
void PT_CALL glIsTextureHandleResidentNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsTextureHandleResidentNV;
}
void PT_CALL glIsTransformFeedback(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsTransformFeedback;
}
void PT_CALL glIsTransformFeedbackNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsTransformFeedbackNV;
}
void PT_CALL glIsVariantEnabledEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsVariantEnabledEXT;
}
uint32_t PT_CALL glIsVertexArray(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsVertexArray;
    ret = *pt0;
    return ret;
}
void PT_CALL glIsVertexArrayAPPLE(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsVertexArrayAPPLE;
}
void PT_CALL glIsVertexAttribEnabledAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glIsVertexAttribEnabledAPPLE;
}
void PT_CALL glLGPUCopyImageSubDataNVX(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15, uint32_t arg16) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; pt[16] = arg15; pt[17] = arg16; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLGPUCopyImageSubDataNVX;
}
void PT_CALL glLGPUInterlockNVX(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLGPUInterlockNVX;
}
void PT_CALL glLGPUNamedBufferSubDataNVX(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLGPUNamedBufferSubDataNVX;
}
void PT_CALL glLabelObjectEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLabelObjectEXT;
}
void PT_CALL glLightEnviSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLightEnviSGIX;
}
void PT_CALL glLightModelf(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLightModelf, 2);
}
void PT_CALL glLightModelfv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLightModelfv, 2);
}
void PT_CALL glLightModeli(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLightModeli, 2);
}
void PT_CALL glLightModeliv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLightModeliv, 2);
}
void PT_CALL glLightModelxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLightModelxOES;
}
void PT_CALL glLightModelxvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLightModelxvOES;
}
void PT_CALL glLightf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLightf, 3);
}
void PT_CALL glLightfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLightfv, 3);
}
void PT_CALL glLighti(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLighti, 3);
}
void PT_CALL glLightiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLightiv, 3);
}
void PT_CALL glLightxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLightxOES;
}
void PT_CALL glLightxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLightxvOES;
}
void PT_CALL glLineStipple(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLineStipple, 2);
}
void PT_CALL glLineWidth(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLineWidth, 1);
}
void PT_CALL glLineWidthxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLineWidthxOES;
}
void PT_CALL glLinkProgram(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLinkProgram, 1);
}
void PT_CALL glLinkProgramARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLinkProgramARB, 1);
}
void PT_CALL glListBase(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glListBase, 1);
}
void PT_CALL glListDrawCommandsStatesClientNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glListDrawCommandsStatesClientNV;
}
void PT_CALL glListParameterfSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glListParameterfSGIX;
}
void PT_CALL glListParameterfvSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glListParameterfvSGIX;
}
void PT_CALL glListParameteriSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glListParameteriSGIX;
}
void PT_CALL glListParameterivSGIX(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glListParameterivSGIX;
}
void PT_CALL glLoadIdentity(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLoadIdentity, 0);
}
void PT_CALL glLoadIdentityDeformationMapSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLoadIdentityDeformationMapSGIX;
}
void PT_CALL glLoadMatrixd(uint32_t arg0) {
    fifoAddData(0, arg0, 16*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLoadMatrixd, 1);
}
void PT_CALL glLoadMatrixf(uint32_t arg0) {
    fifoAddData(0, arg0, 16*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLoadMatrixf, 1);
}
void PT_CALL glLoadMatrixxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLoadMatrixxOES;
}
void PT_CALL glLoadName(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLoadName, 1);
}
void PT_CALL glLoadProgramNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    const int strz[2] = {0, 0};
    fifoAddData(0, arg3, ALIGNED(arg2));
    fifoAddData(0, (uint32_t)strz, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLoadProgramNV, 4);
}
void PT_CALL glLoadTransposeMatrixd(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLoadTransposeMatrixd;
}
void PT_CALL glLoadTransposeMatrixdARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLoadTransposeMatrixdARB;
}
void PT_CALL glLoadTransposeMatrixf(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLoadTransposeMatrixf;
}
void PT_CALL glLoadTransposeMatrixfARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLoadTransposeMatrixfARB;
}
void PT_CALL glLoadTransposeMatrixxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glLoadTransposeMatrixxOES;
}
void PT_CALL glLockArraysEXT(uint32_t arg0, uint32_t arg1) {
    if (arg1) {
        PrepVertexArray(arg0, arg0 + arg1 - 1, 0);
        PushVertexArray(arg0, arg0 + arg1 - 1);
    }
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLockArraysEXT, 2);
}
void PT_CALL glLogicOp(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glLogicOp, 1);
}
void PT_CALL glMakeBufferNonResidentNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeBufferNonResidentNV;
}
void PT_CALL glMakeBufferResidentNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeBufferResidentNV;
}
void PT_CALL glMakeImageHandleNonResidentARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeImageHandleNonResidentARB;
}
void PT_CALL glMakeImageHandleNonResidentNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeImageHandleNonResidentNV;
}
void PT_CALL glMakeImageHandleResidentARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeImageHandleResidentARB;
}
void PT_CALL glMakeImageHandleResidentNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeImageHandleResidentNV;
}
void PT_CALL glMakeNamedBufferNonResidentNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeNamedBufferNonResidentNV;
}
void PT_CALL glMakeNamedBufferResidentNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeNamedBufferResidentNV;
}
void PT_CALL glMakeTextureHandleNonResidentARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeTextureHandleNonResidentARB;
}
void PT_CALL glMakeTextureHandleNonResidentNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeTextureHandleNonResidentNV;
}
void PT_CALL glMakeTextureHandleResidentARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeTextureHandleResidentARB;
}
void PT_CALL glMakeTextureHandleResidentNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMakeTextureHandleResidentNV;
}
void PT_CALL glMap1d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    fifoAddData(0, arg7, szglname(arg0)*arg5*arg6*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMap1d, 8);
}
void PT_CALL glMap1f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    fifoAddData(0, arg5, ALIGNED(szglname(arg0)*arg3*arg4*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMap1f, 6);
}
void PT_CALL glMap1xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMap1xOES;
}
void PT_CALL glMap2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13) {
    fifoAddData(0, arg13, szglname(arg0)*arg5*arg6*arg11*arg12*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMap2d, 14);
}
void PT_CALL glMap2f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    fifoAddData(0, arg9, ALIGNED(szglname(arg0)*arg3*arg4*arg7*arg8*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMap2f, 10);
}
void PT_CALL glMap2xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMap2xOES;
}
void * PT_CALL glMapBuffer(uint32_t arg0, uint32_t arg1) {
    uint32_t szBuf;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapBuffer;
    szBuf = *pt0;
    return (szBuf & 0x01U)? (void *)&fbtm[(MGLFBT_SIZE - szBuf + 1) >> 2]:(mbufo + szBuf);
}
void * PT_CALL glMapBufferARB(uint32_t arg0, uint32_t arg1) {
    uint32_t szBuf;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapBufferARB;
    szBuf = *pt0;
    return (szBuf & 0x01U)? (void *)&fbtm[(MGLFBT_SIZE - szBuf + 1) >> 2]:(mbufo + szBuf);
}
void * PT_CALL glMapBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t szBuf;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapBufferRange;
    szBuf = *pt0;
    return (szBuf & 0x01U)? (void *)&fbtm[(MGLFBT_SIZE - szBuf + 1) >> 2]:(mbufo + szBuf);
}
void PT_CALL glMapControlPointsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapControlPointsNV;
}
void PT_CALL glMapGrid1d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMapGrid1d, 5);
}
void PT_CALL glMapGrid1f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMapGrid1f, 3);
}
void PT_CALL glMapGrid1xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapGrid1xOES;
}
void PT_CALL glMapGrid2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMapGrid2d, 10);
}
void PT_CALL glMapGrid2f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMapGrid2f, 6);
}
void PT_CALL glMapGrid2xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapGrid2xOES;
}
void * PT_CALL glMapNamedBuffer(uint32_t arg0, uint32_t arg1) {
    uint32_t szBuf;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapNamedBuffer;
    szBuf = *pt0;
    return (void *)&fbtm[(MGLFBT_SIZE - ALIGNED(szBuf)) >> 2];
}
void * PT_CALL glMapNamedBufferEXT(uint32_t arg0, uint32_t arg1) {
    uint32_t szBuf;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapNamedBufferEXT;
    szBuf = *pt0;
    return (void *)&fbtm[(MGLFBT_SIZE - ALIGNED(szBuf)) >> 2];
}
void * PT_CALL glMapNamedBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t szBuf;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapNamedBufferRange;
    szBuf = *pt0;
    return (void *)&fbtm[(MGLFBT_SIZE - ALIGNED(szBuf)) >> 2];
}
void * PT_CALL glMapNamedBufferRangeEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t szBuf;
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapNamedBufferRangeEXT;
    szBuf = *pt0;
    return (void *)&fbtm[(MGLFBT_SIZE - ALIGNED(szBuf)) >> 2];
}
void PT_CALL glMapObjectBufferATI(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapObjectBufferATI;
}
void PT_CALL glMapParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapParameterfvNV;
}
void PT_CALL glMapParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapParameterivNV;
}
void PT_CALL glMapTexture2DINTEL(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapTexture2DINTEL;
}
void PT_CALL glMapVertexAttrib1dAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapVertexAttrib1dAPPLE;
}
void PT_CALL glMapVertexAttrib1fAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapVertexAttrib1fAPPLE;
}
void PT_CALL glMapVertexAttrib2dAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapVertexAttrib2dAPPLE;
}
void PT_CALL glMapVertexAttrib2fAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMapVertexAttrib2fAPPLE;
}
void PT_CALL glMaterialf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMaterialf, 3);
}
void PT_CALL glMaterialfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMaterialfv, 3);
}
void PT_CALL glMateriali(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMateriali, 3);
}
void PT_CALL glMaterialiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMaterialiv, 3);
}
void PT_CALL glMaterialxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMaterialxOES;
}
void PT_CALL glMaterialxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMaterialxvOES;
}
void PT_CALL glMatrixFrustumEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixFrustumEXT;
}
void PT_CALL glMatrixIndexPointerARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixIndexPointerARB;
}
void PT_CALL glMatrixIndexubvARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixIndexubvARB;
}
void PT_CALL glMatrixIndexuivARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixIndexuivARB;
}
void PT_CALL glMatrixIndexusvARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixIndexusvARB;
}
void PT_CALL glMatrixLoad3x2fNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoad3x2fNV;
}
void PT_CALL glMatrixLoad3x3fNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoad3x3fNV;
}
void PT_CALL glMatrixLoadIdentityEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoadIdentityEXT;
}
void PT_CALL glMatrixLoadTranspose3x3fNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoadTranspose3x3fNV;
}
void PT_CALL glMatrixLoadTransposedEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoadTransposedEXT;
}
void PT_CALL glMatrixLoadTransposefEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoadTransposefEXT;
}
void PT_CALL glMatrixLoaddEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoaddEXT;
}
void PT_CALL glMatrixLoadfEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixLoadfEXT;
}
void PT_CALL glMatrixMode(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMatrixMode, 1);
}
void PT_CALL glMatrixMult3x2fNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixMult3x2fNV;
}
void PT_CALL glMatrixMult3x3fNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixMult3x3fNV;
}
void PT_CALL glMatrixMultTranspose3x3fNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixMultTranspose3x3fNV;
}
void PT_CALL glMatrixMultTransposedEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixMultTransposedEXT;
}
void PT_CALL glMatrixMultTransposefEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixMultTransposefEXT;
}
void PT_CALL glMatrixMultdEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixMultdEXT;
}
void PT_CALL glMatrixMultfEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixMultfEXT;
}
void PT_CALL glMatrixOrthoEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixOrthoEXT;
}
void PT_CALL glMatrixPopEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixPopEXT;
}
void PT_CALL glMatrixPushEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixPushEXT;
}
void PT_CALL glMatrixRotatedEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixRotatedEXT;
}
void PT_CALL glMatrixRotatefEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixRotatefEXT;
}
void PT_CALL glMatrixScaledEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixScaledEXT;
}
void PT_CALL glMatrixScalefEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixScalefEXT;
}
void PT_CALL glMatrixTranslatedEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixTranslatedEXT;
}
void PT_CALL glMatrixTranslatefEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMatrixTranslatefEXT;
}
void PT_CALL glMaxShaderCompilerThreadsARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMaxShaderCompilerThreadsARB;
}
void PT_CALL glMaxShaderCompilerThreadsKHR(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMaxShaderCompilerThreadsKHR;
}
void PT_CALL glMemoryBarrier(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMemoryBarrier;
}
void PT_CALL glMemoryBarrierByRegion(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMemoryBarrierByRegion;
}
void PT_CALL glMemoryBarrierEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMemoryBarrierEXT;
}
void PT_CALL glMemoryObjectParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMemoryObjectParameterivEXT;
}
void PT_CALL glMinSampleShading(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMinSampleShading;
}
void PT_CALL glMinSampleShadingARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMinSampleShadingARB;
}
void PT_CALL glMinmax(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMinmax;
}
void PT_CALL glMinmaxEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMinmaxEXT;
}
void PT_CALL glMultMatrixd(uint32_t arg0) {
    fifoAddData(0, arg0, 16*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultMatrixd, 1);
}
void PT_CALL glMultMatrixf(uint32_t arg0) {
    fifoAddData(0, arg0, 16*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultMatrixf, 1);
}
void PT_CALL glMultMatrixxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultMatrixxOES;
}
void PT_CALL glMultTransposeMatrixd(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultTransposeMatrixd;
}
void PT_CALL glMultTransposeMatrixdARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultTransposeMatrixdARB;
}
void PT_CALL glMultTransposeMatrixf(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultTransposeMatrixf;
}
void PT_CALL glMultTransposeMatrixfARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultTransposeMatrixfARB;
}
void PT_CALL glMultTransposeMatrixxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultTransposeMatrixxOES;
}
void PT_CALL glMultiDrawArrays(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArrays;
}
void PT_CALL glMultiDrawArraysEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArraysEXT;
}
void PT_CALL glMultiDrawArraysIndirect(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArraysIndirect;
}
void PT_CALL glMultiDrawArraysIndirectAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArraysIndirectAMD;
}
void PT_CALL glMultiDrawArraysIndirectBindlessCountNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArraysIndirectBindlessCountNV;
}
void PT_CALL glMultiDrawArraysIndirectBindlessNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArraysIndirectBindlessNV;
}
void PT_CALL glMultiDrawArraysIndirectCount(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArraysIndirectCount;
}
void PT_CALL glMultiDrawArraysIndirectCountARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawArraysIndirectCountARB;
}
void PT_CALL glMultiDrawElementArrayAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementArrayAPPLE;
}
void PT_CALL glMultiDrawElements(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElements;
}
void PT_CALL glMultiDrawElementsBaseVertex(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsBaseVertex;
}
void PT_CALL glMultiDrawElementsEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsEXT;
}
void PT_CALL glMultiDrawElementsIndirect(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsIndirect;
}
void PT_CALL glMultiDrawElementsIndirectAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsIndirectAMD;
}
void PT_CALL glMultiDrawElementsIndirectBindlessCountNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsIndirectBindlessCountNV;
}
void PT_CALL glMultiDrawElementsIndirectBindlessNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsIndirectBindlessNV;
}
void PT_CALL glMultiDrawElementsIndirectCount(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsIndirectCount;
}
void PT_CALL glMultiDrawElementsIndirectCountARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawElementsIndirectCountARB;
}
void PT_CALL glMultiDrawMeshTasksIndirectCountNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawMeshTasksIndirectCountNV;
}
void PT_CALL glMultiDrawMeshTasksIndirectNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawMeshTasksIndirectNV;
}
void PT_CALL glMultiDrawRangeElementArrayAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiDrawRangeElementArrayAPPLE;
}
void PT_CALL glMultiModeDrawArraysIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiModeDrawArraysIBM;
}
void PT_CALL glMultiModeDrawElementsIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiModeDrawElementsIBM;
}
void PT_CALL glMultiTexBufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexBufferEXT;
}
void PT_CALL glMultiTexCoord1bOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord1bOES;
}
void PT_CALL glMultiTexCoord1bvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord1bvOES;
}
void PT_CALL glMultiTexCoord1d(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1d, 3);
}
void PT_CALL glMultiTexCoord1dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1dARB, 3);
}
void PT_CALL glMultiTexCoord1dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1dv, 2);
}
void PT_CALL glMultiTexCoord1dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1dvARB, 2);
}
void PT_CALL glMultiTexCoord1f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1f, 2);
}
void PT_CALL glMultiTexCoord1fARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1fARB, 2);
}
void PT_CALL glMultiTexCoord1fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1fv, 2);
}
void PT_CALL glMultiTexCoord1fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1fvARB, 2);
}
void PT_CALL glMultiTexCoord1hNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord1hNV;
}
void PT_CALL glMultiTexCoord1hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord1hvNV;
}
void PT_CALL glMultiTexCoord1i(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1i, 2);
}
void PT_CALL glMultiTexCoord1iARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1iARB, 2);
}
void PT_CALL glMultiTexCoord1iv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1iv, 2);
}
void PT_CALL glMultiTexCoord1ivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1ivARB, 2);
}
void PT_CALL glMultiTexCoord1s(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1s, 2);
}
void PT_CALL glMultiTexCoord1sARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1sARB, 2);
}
void PT_CALL glMultiTexCoord1sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1sv, 2);
}
void PT_CALL glMultiTexCoord1svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord1svARB, 2);
}
void PT_CALL glMultiTexCoord1xOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord1xOES;
}
void PT_CALL glMultiTexCoord1xvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord1xvOES;
}
void PT_CALL glMultiTexCoord2bOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord2bOES;
}
void PT_CALL glMultiTexCoord2bvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord2bvOES;
}
void PT_CALL glMultiTexCoord2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2d, 5);
}
void PT_CALL glMultiTexCoord2dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2dARB, 5);
}
void PT_CALL glMultiTexCoord2dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2dv, 2);
}
void PT_CALL glMultiTexCoord2dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2dvARB, 2);
}
void PT_CALL glMultiTexCoord2f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2f, 3);
}
void PT_CALL glMultiTexCoord2fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2fARB, 3);
}
void PT_CALL glMultiTexCoord2fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2fv, 2);
}
void PT_CALL glMultiTexCoord2fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2fvARB, 2);
}
void PT_CALL glMultiTexCoord2hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord2hNV;
}
void PT_CALL glMultiTexCoord2hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord2hvNV;
}
void PT_CALL glMultiTexCoord2i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2i, 3);
}
void PT_CALL glMultiTexCoord2iARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2iARB, 3);
}
void PT_CALL glMultiTexCoord2iv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2iv, 2);
}
void PT_CALL glMultiTexCoord2ivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2ivARB, 2);
}
void PT_CALL glMultiTexCoord2s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2s, 3);
}
void PT_CALL glMultiTexCoord2sARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2sARB, 3);
}
void PT_CALL glMultiTexCoord2sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2sv, 2);
}
void PT_CALL glMultiTexCoord2svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord2svARB, 2);
}
void PT_CALL glMultiTexCoord2xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord2xOES;
}
void PT_CALL glMultiTexCoord2xvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord2xvOES;
}
void PT_CALL glMultiTexCoord3bOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord3bOES;
}
void PT_CALL glMultiTexCoord3bvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord3bvOES;
}
void PT_CALL glMultiTexCoord3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3d, 7);
}
void PT_CALL glMultiTexCoord3dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3dARB, 7);
}
void PT_CALL glMultiTexCoord3dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 3*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3dv, 2);
}
void PT_CALL glMultiTexCoord3dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 3*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3dvARB, 2);
}
void PT_CALL glMultiTexCoord3f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3f, 4);
}
void PT_CALL glMultiTexCoord3fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3fARB, 4);
}
void PT_CALL glMultiTexCoord3fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3fv, 2);
}
void PT_CALL glMultiTexCoord3fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3fvARB, 2);
}
void PT_CALL glMultiTexCoord3hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord3hNV;
}
void PT_CALL glMultiTexCoord3hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord3hvNV;
}
void PT_CALL glMultiTexCoord3i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3i, 4);
}
void PT_CALL glMultiTexCoord3iARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3iARB, 4);
}
void PT_CALL glMultiTexCoord3iv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3iv, 2);
}
void PT_CALL glMultiTexCoord3ivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3ivARB, 2);
}
void PT_CALL glMultiTexCoord3s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3s, 4);
}
void PT_CALL glMultiTexCoord3sARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3sARB, 4);
}
void PT_CALL glMultiTexCoord3sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3sv, 2);
}
void PT_CALL glMultiTexCoord3svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord3svARB, 2);
}
void PT_CALL glMultiTexCoord3xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord3xOES;
}
void PT_CALL glMultiTexCoord3xvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord3xvOES;
}
void PT_CALL glMultiTexCoord4bOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord4bOES;
}
void PT_CALL glMultiTexCoord4bvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord4bvOES;
}
void PT_CALL glMultiTexCoord4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4d, 9);
}
void PT_CALL glMultiTexCoord4dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4dARB, 9);
}
void PT_CALL glMultiTexCoord4dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4dv, 2);
}
void PT_CALL glMultiTexCoord4dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4dvARB, 2);
}
void PT_CALL glMultiTexCoord4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4f, 5);
}
void PT_CALL glMultiTexCoord4fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4fARB, 5);
}
void PT_CALL glMultiTexCoord4fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4fv, 2);
}
void PT_CALL glMultiTexCoord4fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4fvARB, 2);
}
void PT_CALL glMultiTexCoord4hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord4hNV;
}
void PT_CALL glMultiTexCoord4hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord4hvNV;
}
void PT_CALL glMultiTexCoord4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4i, 5);
}
void PT_CALL glMultiTexCoord4iARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4iARB, 5);
}
void PT_CALL glMultiTexCoord4iv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4iv, 2);
}
void PT_CALL glMultiTexCoord4ivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4ivARB, 2);
}
void PT_CALL glMultiTexCoord4s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4s, 5);
}
void PT_CALL glMultiTexCoord4sARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4sARB, 5);
}
void PT_CALL glMultiTexCoord4sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4sv, 2);
}
void PT_CALL glMultiTexCoord4svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glMultiTexCoord4svARB, 2);
}
void PT_CALL glMultiTexCoord4xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord4xOES;
}
void PT_CALL glMultiTexCoord4xvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoord4xvOES;
}
void PT_CALL glMultiTexCoordP1ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP1ui;
}
void PT_CALL glMultiTexCoordP1uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP1uiv;
}
void PT_CALL glMultiTexCoordP2ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP2ui;
}
void PT_CALL glMultiTexCoordP2uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP2uiv;
}
void PT_CALL glMultiTexCoordP3ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP3ui;
}
void PT_CALL glMultiTexCoordP3uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP3uiv;
}
void PT_CALL glMultiTexCoordP4ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP4ui;
}
void PT_CALL glMultiTexCoordP4uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordP4uiv;
}
void PT_CALL glMultiTexCoordPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexCoordPointerEXT;
}
void PT_CALL glMultiTexEnvfEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexEnvfEXT;
}
void PT_CALL glMultiTexEnvfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexEnvfvEXT;
}
void PT_CALL glMultiTexEnviEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexEnviEXT;
}
void PT_CALL glMultiTexEnvivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexEnvivEXT;
}
void PT_CALL glMultiTexGendEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexGendEXT;
}
void PT_CALL glMultiTexGendvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexGendvEXT;
}
void PT_CALL glMultiTexGenfEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexGenfEXT;
}
void PT_CALL glMultiTexGenfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexGenfvEXT;
}
void PT_CALL glMultiTexGeniEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexGeniEXT;
}
void PT_CALL glMultiTexGenivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexGenivEXT;
}
void PT_CALL glMultiTexImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexImage1DEXT;
}
void PT_CALL glMultiTexImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexImage2DEXT;
}
void PT_CALL glMultiTexImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexImage3DEXT;
}
void PT_CALL glMultiTexParameterIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexParameterIivEXT;
}
void PT_CALL glMultiTexParameterIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexParameterIuivEXT;
}
void PT_CALL glMultiTexParameterfEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexParameterfEXT;
}
void PT_CALL glMultiTexParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexParameterfvEXT;
}
void PT_CALL glMultiTexParameteriEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexParameteriEXT;
}
void PT_CALL glMultiTexParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexParameterivEXT;
}
void PT_CALL glMultiTexRenderbufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexRenderbufferEXT;
}
void PT_CALL glMultiTexSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexSubImage1DEXT;
}
void PT_CALL glMultiTexSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexSubImage2DEXT;
}
void PT_CALL glMultiTexSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMultiTexSubImage3DEXT;
}
void PT_CALL glMulticastBarrierNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastBarrierNV;
}
void PT_CALL glMulticastBlitFramebufferNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastBlitFramebufferNV;
}
void PT_CALL glMulticastBufferSubDataNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastBufferSubDataNV;
}
void PT_CALL glMulticastCopyBufferSubDataNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastCopyBufferSubDataNV;
}
void PT_CALL glMulticastCopyImageSubDataNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15, uint32_t arg16) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; pt[16] = arg15; pt[17] = arg16; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastCopyImageSubDataNV;
}
void PT_CALL glMulticastFramebufferSampleLocationsfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastFramebufferSampleLocationsfvNV;
}
void PT_CALL glMulticastGetQueryObjecti64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastGetQueryObjecti64vNV;
}
void PT_CALL glMulticastGetQueryObjectivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastGetQueryObjectivNV;
}
void PT_CALL glMulticastGetQueryObjectui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastGetQueryObjectui64vNV;
}
void PT_CALL glMulticastGetQueryObjectuivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastGetQueryObjectuivNV;
}
void PT_CALL glMulticastWaitSyncNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glMulticastWaitSyncNV;
}
void PT_CALL glNamedBufferAttachMemoryNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferAttachMemoryNV;
}
void PT_CALL glNamedBufferData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (arg2)
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(arg1)) >> 2], (unsigned char *)arg2, arg1);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferData;
}
void PT_CALL glNamedBufferDataEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (arg2)
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(arg1)) >> 2], (unsigned char *)arg2, arg1);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferDataEXT;
}
void PT_CALL glNamedBufferPageCommitmentARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferPageCommitmentARB;
}
void PT_CALL glNamedBufferPageCommitmentEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferPageCommitmentEXT;
}
void PT_CALL glNamedBufferStorage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (arg2)
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(arg1)) >> 2], (unsigned char *)arg2, arg1);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferStorage;
}
void PT_CALL glNamedBufferStorageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    if (arg2)
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(arg1)) >> 2], (unsigned char *)arg2, arg1);
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferStorageEXT;
}
void PT_CALL glNamedBufferStorageExternalEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferStorageExternalEXT;
}
void PT_CALL glNamedBufferStorageMemEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferStorageMemEXT;
}
void PT_CALL glNamedBufferSubData(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t offst = arg1, remain = arg2, ptr = arg3;
    while(remain) {
        uint32_t chunk = (remain > (MGLFBT_SIZE - PAGE_SIZE))? (MGLFBT_SIZE - PAGE_SIZE):remain;
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(chunk)) >> 2], (unsigned char *)ptr, chunk);
        pt[1] = arg0; pt[2] = offst; pt[3] = chunk; pt[4] = ptr;
        pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferSubData;
        offst += chunk;
        ptr += chunk;
        remain -= chunk;
    }
}
void PT_CALL glNamedBufferSubDataEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t offst = arg1, remain = arg2, ptr = arg3;
    while(remain) {
        uint32_t chunk = (remain > (MGLFBT_SIZE - PAGE_SIZE))? (MGLFBT_SIZE - PAGE_SIZE):remain;
        FBTMMCPY(&fbtm[(MGLFBT_SIZE - ALIGNED(chunk)) >> 2], (unsigned char *)ptr, chunk);
        pt[1] = arg0; pt[2] = offst; pt[3] = chunk; pt[4] = ptr;
        pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedBufferSubDataEXT;
        offst += chunk;
        ptr += chunk;
        remain -= chunk;
    }
}
void PT_CALL glNamedCopyBufferSubDataEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedCopyBufferSubDataEXT;
}
void PT_CALL glNamedFramebufferDrawBuffer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferDrawBuffer;
}
void PT_CALL glNamedFramebufferDrawBuffers(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferDrawBuffers;
}
void PT_CALL glNamedFramebufferParameteri(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNamedFramebufferParameteri, 3);
}
void PT_CALL glNamedFramebufferParameteriEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNamedFramebufferParameteriEXT, 3);
}
void PT_CALL glNamedFramebufferReadBuffer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferReadBuffer;
}
void PT_CALL glNamedFramebufferRenderbuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferRenderbuffer;
}
void PT_CALL glNamedFramebufferRenderbufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferRenderbufferEXT;
}
void PT_CALL glNamedFramebufferSampleLocationsfvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferSampleLocationsfvARB;
}
void PT_CALL glNamedFramebufferSampleLocationsfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferSampleLocationsfvNV;
}
void PT_CALL glNamedFramebufferSamplePositionsfvAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferSamplePositionsfvAMD;
}
void PT_CALL glNamedFramebufferTexture1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTexture1DEXT;
}
void PT_CALL glNamedFramebufferTexture2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTexture2DEXT;
}
void PT_CALL glNamedFramebufferTexture3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTexture3DEXT;
}
void PT_CALL glNamedFramebufferTexture(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTexture;
}
void PT_CALL glNamedFramebufferTextureEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTextureEXT;
}
void PT_CALL glNamedFramebufferTextureFaceEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTextureFaceEXT;
}
void PT_CALL glNamedFramebufferTextureLayer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTextureLayer;
}
void PT_CALL glNamedFramebufferTextureLayerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedFramebufferTextureLayerEXT;
}
void PT_CALL glNamedProgramLocalParameter4dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameter4dEXT;
}
void PT_CALL glNamedProgramLocalParameter4dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameter4dvEXT;
}
void PT_CALL glNamedProgramLocalParameter4fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameter4fEXT;
}
void PT_CALL glNamedProgramLocalParameter4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameter4fvEXT;
}
void PT_CALL glNamedProgramLocalParameterI4iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameterI4iEXT;
}
void PT_CALL glNamedProgramLocalParameterI4ivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameterI4ivEXT;
}
void PT_CALL glNamedProgramLocalParameterI4uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameterI4uiEXT;
}
void PT_CALL glNamedProgramLocalParameterI4uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameterI4uivEXT;
}
void PT_CALL glNamedProgramLocalParameters4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParameters4fvEXT;
}
void PT_CALL glNamedProgramLocalParametersI4ivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParametersI4ivEXT;
}
void PT_CALL glNamedProgramLocalParametersI4uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramLocalParametersI4uivEXT;
}
void PT_CALL glNamedProgramStringEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedProgramStringEXT;
}
void PT_CALL glNamedRenderbufferStorage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedRenderbufferStorage;
}
void PT_CALL glNamedRenderbufferStorageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedRenderbufferStorageEXT;
}
void PT_CALL glNamedRenderbufferStorageMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedRenderbufferStorageMultisample;
}
void PT_CALL glNamedRenderbufferStorageMultisampleAdvancedAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedRenderbufferStorageMultisampleAdvancedAMD;
}
void PT_CALL glNamedRenderbufferStorageMultisampleCoverageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedRenderbufferStorageMultisampleCoverageEXT;
}
void PT_CALL glNamedRenderbufferStorageMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedRenderbufferStorageMultisampleEXT;
}
void PT_CALL glNamedStringARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNamedStringARB;
}
void PT_CALL glNewList(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNewList, 2);
}
void PT_CALL glNewObjectBufferATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNewObjectBufferATI;
}
void PT_CALL glNormal3b(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3b, 3);
}
void PT_CALL glNormal3bv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3bv, 1);
}
void PT_CALL glNormal3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3d, 6);
}
void PT_CALL glNormal3dv(uint32_t arg0) {
    fifoAddData(0, arg0, 3*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3dv, 1);
}
void PT_CALL glNormal3f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3f, 3);
}
void PT_CALL glNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormal3fVertex3fSUN;
}
void PT_CALL glNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormal3fVertex3fvSUN;
}
void PT_CALL glNormal3fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3fv, 1);
}
void PT_CALL glNormal3hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormal3hNV;
}
void PT_CALL glNormal3hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormal3hvNV;
}
void PT_CALL glNormal3i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3i, 3);
}
void PT_CALL glNormal3iv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3iv, 1);
}
void PT_CALL glNormal3s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3s, 3);
}
void PT_CALL glNormal3sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormal3sv, 1);
}
void PT_CALL glNormal3xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormal3xOES;
}
void PT_CALL glNormal3xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormal3xvOES;
}
void PT_CALL glNormalFormatNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalFormatNV;
}
void PT_CALL glNormalP3ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalP3ui;
}
void PT_CALL glNormalP3uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalP3uiv;
}
void PT_CALL glNormalPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormalPointer, 3);
    vtxarry_init(&vtxArry.Normal, 3, arg0, arg1, (void *)arg2);
}
void PT_CALL glNormalPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glNormalPointerEXT, 4);
    vtxarry_init(&vtxArry.Normal, 3, arg0, arg1, (void *)arg3);
}
void PT_CALL glNormalPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalPointerListIBM;
}
void PT_CALL glNormalPointervINTEL(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalPointervINTEL;
}
void PT_CALL glNormalStream3bATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3bATI;
}
void PT_CALL glNormalStream3bvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3bvATI;
}
void PT_CALL glNormalStream3dATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3dATI;
}
void PT_CALL glNormalStream3dvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3dvATI;
}
void PT_CALL glNormalStream3fATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3fATI;
}
void PT_CALL glNormalStream3fvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3fvATI;
}
void PT_CALL glNormalStream3iATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3iATI;
}
void PT_CALL glNormalStream3ivATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3ivATI;
}
void PT_CALL glNormalStream3sATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3sATI;
}
void PT_CALL glNormalStream3svATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glNormalStream3svATI;
}
void PT_CALL glObjectLabel(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glObjectLabel;
}
void PT_CALL glObjectPtrLabel(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glObjectPtrLabel;
}
void PT_CALL glObjectPurgeableAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glObjectPurgeableAPPLE;
}
void PT_CALL glObjectUnpurgeableAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glObjectUnpurgeableAPPLE;
}
void PT_CALL glOrtho(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glOrtho, 12);
}
void PT_CALL glOrthofOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glOrthofOES;
}
void PT_CALL glOrthoxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glOrthoxOES;
}
void PT_CALL glPNTrianglesfATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPNTrianglesfATI;
}
void PT_CALL glPNTrianglesiATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPNTrianglesiATI;
}
void PT_CALL glPassTexCoordATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPassTexCoordATI, 3);
}
void PT_CALL glPassThrough(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPassThrough, 1);
}
void PT_CALL glPassThroughxOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPassThroughxOES;
}
void PT_CALL glPatchParameterfv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPatchParameterfv;
}
void PT_CALL glPatchParameteri(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPatchParameteri;
}
void PT_CALL glPathColorGenNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathColorGenNV;
}
void PT_CALL glPathCommandsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathCommandsNV;
}
void PT_CALL glPathCoordsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathCoordsNV;
}
void PT_CALL glPathCoverDepthFuncNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathCoverDepthFuncNV;
}
void PT_CALL glPathDashArrayNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathDashArrayNV;
}
void PT_CALL glPathFogGenNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathFogGenNV;
}
void PT_CALL glPathGlyphIndexArrayNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathGlyphIndexArrayNV;
}
void PT_CALL glPathGlyphIndexRangeNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathGlyphIndexRangeNV;
}
void PT_CALL glPathGlyphRangeNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathGlyphRangeNV;
}
void PT_CALL glPathGlyphsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathGlyphsNV;
}
void PT_CALL glPathMemoryGlyphIndexArrayNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathMemoryGlyphIndexArrayNV;
}
void PT_CALL glPathParameterfNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathParameterfNV;
}
void PT_CALL glPathParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathParameterfvNV;
}
void PT_CALL glPathParameteriNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathParameteriNV;
}
void PT_CALL glPathParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathParameterivNV;
}
void PT_CALL glPathStencilDepthOffsetNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathStencilDepthOffsetNV;
}
void PT_CALL glPathStencilFuncNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathStencilFuncNV;
}
void PT_CALL glPathStringNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathStringNV;
}
void PT_CALL glPathSubCommandsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathSubCommandsNV;
}
void PT_CALL glPathSubCoordsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathSubCoordsNV;
}
void PT_CALL glPathTexGenNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPathTexGenNV;
}
void PT_CALL glPauseTransformFeedback(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPauseTransformFeedback, 0);
}
void PT_CALL glPauseTransformFeedbackNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPauseTransformFeedbackNV;
}
void PT_CALL glPixelDataRangeNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelDataRangeNV;
}
void PT_CALL glPixelMapfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelMapfv, 3);
}
void PT_CALL glPixelMapuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(unsigned int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelMapuiv, 3);
}
void PT_CALL glPixelMapusv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(unsigned short)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelMapusv, 3);
}
void PT_CALL glPixelMapx(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelMapx;
}
void PT_CALL glPixelStoref(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelStoref, 2);
}
void PT_CALL glPixelStorei(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelStorei, 2);
    szPackWidth = (arg0 == GL_PACK_ROW_LENGTH)? arg1:szPackWidth;
    szPackHeight = (arg0 == GL_PACK_IMAGE_HEIGHT)? arg1:szPackHeight;
    szUnpackWidth = (arg0 == GL_UNPACK_ROW_LENGTH)? arg1:szUnpackWidth;
    szUnpackHeight = (arg0 == GL_UNPACK_IMAGE_HEIGHT)? arg1:szUnpackHeight;
}
void PT_CALL glPixelStorex(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelStorex;
}
void PT_CALL glPixelTexGenParameterfSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTexGenParameterfSGIS;
}
void PT_CALL glPixelTexGenParameterfvSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTexGenParameterfvSGIS;
}
void PT_CALL glPixelTexGenParameteriSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTexGenParameteriSGIS;
}
void PT_CALL glPixelTexGenParameterivSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTexGenParameterivSGIS;
}
void PT_CALL glPixelTexGenSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTexGenSGIX;
}
void PT_CALL glPixelTransferf(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelTransferf, 2);
}
void PT_CALL glPixelTransferi(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelTransferi, 2);
}
void PT_CALL glPixelTransferxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTransferxOES;
}
void PT_CALL glPixelTransformParameterfEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTransformParameterfEXT;
}
void PT_CALL glPixelTransformParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTransformParameterfvEXT;
}
void PT_CALL glPixelTransformParameteriEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTransformParameteriEXT;
}
void PT_CALL glPixelTransformParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelTransformParameterivEXT;
}
void PT_CALL glPixelZoom(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPixelZoom, 2);
}
void PT_CALL glPixelZoomxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPixelZoomxOES;
}
void PT_CALL glPointAlongPathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPointAlongPathNV;
}
void PT_CALL glPointParameterf(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameterf, 2);
}
void PT_CALL glPointParameterfARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameterfARB, 2);
}
void PT_CALL glPointParameterfEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameterfEXT, 2);
}
void PT_CALL glPointParameterfSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPointParameterfSGIS;
}
void PT_CALL glPointParameterfv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameterfv, 2);
}
void PT_CALL glPointParameterfvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameterfvARB, 2);
}
void PT_CALL glPointParameterfvEXT(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameterfvEXT, 2);
}
void PT_CALL glPointParameterfvSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPointParameterfvSGIS;
}
void PT_CALL glPointParameteri(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameteri, 2);
}
void PT_CALL glPointParameteriNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPointParameteriNV;
}
void PT_CALL glPointParameteriv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(szglname(arg0)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointParameteriv, 2);
}
void PT_CALL glPointParameterivNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPointParameterivNV;
}
void PT_CALL glPointParameterxvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPointParameterxvOES;
}
void PT_CALL glPointSize(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPointSize, 1);
}
void PT_CALL glPointSizexOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPointSizexOES;
}
void PT_CALL glPollAsyncSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPollAsyncSGIX;
}
void PT_CALL glPollInstrumentsSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPollInstrumentsSGIX;
}
void PT_CALL glPolygonMode(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPolygonMode, 2);
}
void PT_CALL glPolygonOffset(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPolygonOffset, 2);
}
void PT_CALL glPolygonOffsetClamp(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPolygonOffsetClamp, 3);
}
void PT_CALL glPolygonOffsetClampEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPolygonOffsetClampEXT, 3);
}
void PT_CALL glPolygonOffsetEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPolygonOffsetEXT, 2);
}
void PT_CALL glPolygonOffsetxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPolygonOffsetxOES;
}
void PT_CALL glPolygonStipple(uint32_t arg0) {
    if (pixUnpackBuf == 0) {
        uint32_t szPix;
        szPix = ((szUnpackWidth == 0)? 32:szUnpackWidth)*32;
        fifoAddData(0, arg0, ALIGNED(szPix));
    }
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPolygonStipple, 1);
}
void PT_CALL glPopAttrib(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPopAttrib, 0);
}
void PT_CALL glPopClientAttrib(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPopClientAttrib, 0);
}
void PT_CALL glPopDebugGroup(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPopDebugGroup;
}
void PT_CALL glPopGroupMarkerEXT(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPopGroupMarkerEXT;
}
void PT_CALL glPopMatrix(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPopMatrix, 0);
}
void PT_CALL glPopName(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPopName, 0);
}
void PT_CALL glPresentFrameDualFillNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPresentFrameDualFillNV;
}
void PT_CALL glPresentFrameKeyedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPresentFrameKeyedNV;
}
void PT_CALL glPrimitiveBoundingBoxARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPrimitiveBoundingBoxARB;
}
void PT_CALL glPrimitiveRestartIndex(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPrimitiveRestartIndex;
}
void PT_CALL glPrimitiveRestartIndexNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPrimitiveRestartIndexNV;
}
void PT_CALL glPrimitiveRestartNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPrimitiveRestartNV;
}
void PT_CALL glPrioritizeTextures(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg1, ALIGNED(arg0*sizeof(int)));
    fifoAddData(0, arg2, ALIGNED(arg0*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPrioritizeTextures, 3);
}
void PT_CALL glPrioritizeTexturesEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg1, ALIGNED(arg0*sizeof(int)));
    fifoAddData(0, arg2, ALIGNED(arg0*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPrioritizeTexturesEXT, 3);
}
void PT_CALL glPrioritizeTexturesxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPrioritizeTexturesxOES;
}
void PT_CALL glProgramBinary(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramBinary;
}
void PT_CALL glProgramBufferParametersIivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramBufferParametersIivNV;
}
void PT_CALL glProgramBufferParametersIuivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramBufferParametersIuivNV;
}
void PT_CALL glProgramBufferParametersfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramBufferParametersfvNV;
}
void PT_CALL glProgramEnvParameter4dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramEnvParameter4dARB, 10);
}
void PT_CALL glProgramEnvParameter4dvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramEnvParameter4dvARB, 3);
}
void PT_CALL glProgramEnvParameter4fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramEnvParameter4fARB, 6);
}
void PT_CALL glProgramEnvParameter4fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramEnvParameter4fvARB, 3);
}
void PT_CALL glProgramEnvParameterI4iNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramEnvParameterI4iNV;
}
void PT_CALL glProgramEnvParameterI4ivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramEnvParameterI4ivNV;
}
void PT_CALL glProgramEnvParameterI4uiNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramEnvParameterI4uiNV;
}
void PT_CALL glProgramEnvParameterI4uivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramEnvParameterI4uivNV;
}
void PT_CALL glProgramEnvParameters4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 4*arg2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramEnvParameters4fvEXT, 4);
}
void PT_CALL glProgramEnvParametersI4ivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramEnvParametersI4ivNV;
}
void PT_CALL glProgramEnvParametersI4uivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramEnvParametersI4uivNV;
}
void PT_CALL glProgramLocalParameter4dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramLocalParameter4dARB, 10);
}
void PT_CALL glProgramLocalParameter4dvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramLocalParameter4dvARB, 3);
}
void PT_CALL glProgramLocalParameter4fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramLocalParameter4fARB, 6);
}
void PT_CALL glProgramLocalParameter4fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramLocalParameter4fvARB, 3);
}
void PT_CALL glProgramLocalParameterI4iNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramLocalParameterI4iNV;
}
void PT_CALL glProgramLocalParameterI4ivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramLocalParameterI4ivNV;
}
void PT_CALL glProgramLocalParameterI4uiNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramLocalParameterI4uiNV;
}
void PT_CALL glProgramLocalParameterI4uivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramLocalParameterI4uivNV;
}
void PT_CALL glProgramLocalParameters4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 4*arg2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramLocalParameters4fvEXT, 4);
}
void PT_CALL glProgramLocalParametersI4ivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramLocalParametersI4ivNV;
}
void PT_CALL glProgramLocalParametersI4uivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramLocalParametersI4uivNV;
}
void PT_CALL glProgramNamedParameter4dNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    fifoAddData(0, arg2, ALIGNED(arg1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramNamedParameter4dNV, 11);
}
void PT_CALL glProgramNamedParameter4dvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg2, ALIGNED(arg1));
    fifoAddData(0, arg3, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramNamedParameter4dvNV, 4);
}
void PT_CALL glProgramNamedParameter4fNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    fifoAddData(0, arg2, ALIGNED(arg1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramNamedParameter4fNV, 7);
}
void PT_CALL glProgramNamedParameter4fvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg2, ALIGNED(arg1));
    fifoAddData(0, arg3, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramNamedParameter4fvNV, 4);
}
void PT_CALL glProgramParameter4dNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramParameter4dNV, 10);
}
void PT_CALL glProgramParameter4dvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramParameter4dvNV, 3);
}
void PT_CALL glProgramParameter4fNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramParameter4fNV, 6);
}
void PT_CALL glProgramParameter4fvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramParameter4fvNV, 3);
}
void PT_CALL glProgramParameteri(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramParameteri;
}
void PT_CALL glProgramParameteriARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramParameteriARB;
}
void PT_CALL glProgramParameteriEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramParameteriEXT;
}
void PT_CALL glProgramParameters4dvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 4*arg2*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramParameters4dvNV, 4);
}
void PT_CALL glProgramParameters4fvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 4*arg2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramParameters4fvNV, 4);
}
void PT_CALL glProgramPathFragmentInputGenNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramPathFragmentInputGenNV;
}
void PT_CALL glProgramStringARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    const int strz[2] = {0, 0};
    fifoAddData(0, arg3, ALIGNED(arg2));
    fifoAddData(0, (uint32_t)strz, ALIGNED(1));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProgramStringARB, 4);
}
void PT_CALL glProgramSubroutineParametersuivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramSubroutineParametersuivNV;
}
void PT_CALL glProgramUniform1d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1d;
}
void PT_CALL glProgramUniform1dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1dEXT;
}
void PT_CALL glProgramUniform1dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1dv;
}
void PT_CALL glProgramUniform1dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1dvEXT;
}
void PT_CALL glProgramUniform1f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1f;
}
void PT_CALL glProgramUniform1fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1fEXT;
}
void PT_CALL glProgramUniform1fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1fv;
}
void PT_CALL glProgramUniform1fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1fvEXT;
}
void PT_CALL glProgramUniform1i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1i64ARB;
}
void PT_CALL glProgramUniform1i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1i64NV;
}
void PT_CALL glProgramUniform1i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1i64vARB;
}
void PT_CALL glProgramUniform1i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1i64vNV;
}
void PT_CALL glProgramUniform1i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1i;
}
void PT_CALL glProgramUniform1iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1iEXT;
}
void PT_CALL glProgramUniform1iv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1iv;
}
void PT_CALL glProgramUniform1ivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1ivEXT;
}
void PT_CALL glProgramUniform1ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1ui64ARB;
}
void PT_CALL glProgramUniform1ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1ui64NV;
}
void PT_CALL glProgramUniform1ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1ui64vARB;
}
void PT_CALL glProgramUniform1ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1ui64vNV;
}
void PT_CALL glProgramUniform1ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1ui;
}
void PT_CALL glProgramUniform1uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1uiEXT;
}
void PT_CALL glProgramUniform1uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1uiv;
}
void PT_CALL glProgramUniform1uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform1uivEXT;
}
void PT_CALL glProgramUniform2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2d;
}
void PT_CALL glProgramUniform2dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2dEXT;
}
void PT_CALL glProgramUniform2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2dv;
}
void PT_CALL glProgramUniform2dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2dvEXT;
}
void PT_CALL glProgramUniform2f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2f;
}
void PT_CALL glProgramUniform2fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2fEXT;
}
void PT_CALL glProgramUniform2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2fv;
}
void PT_CALL glProgramUniform2fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2fvEXT;
}
void PT_CALL glProgramUniform2i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2i64ARB;
}
void PT_CALL glProgramUniform2i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2i64NV;
}
void PT_CALL glProgramUniform2i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2i64vARB;
}
void PT_CALL glProgramUniform2i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2i64vNV;
}
void PT_CALL glProgramUniform2i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2i;
}
void PT_CALL glProgramUniform2iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2iEXT;
}
void PT_CALL glProgramUniform2iv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2iv;
}
void PT_CALL glProgramUniform2ivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2ivEXT;
}
void PT_CALL glProgramUniform2ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2ui64ARB;
}
void PT_CALL glProgramUniform2ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2ui64NV;
}
void PT_CALL glProgramUniform2ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2ui64vARB;
}
void PT_CALL glProgramUniform2ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2ui64vNV;
}
void PT_CALL glProgramUniform2ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2ui;
}
void PT_CALL glProgramUniform2uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2uiEXT;
}
void PT_CALL glProgramUniform2uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2uiv;
}
void PT_CALL glProgramUniform2uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform2uivEXT;
}
void PT_CALL glProgramUniform3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3d;
}
void PT_CALL glProgramUniform3dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3dEXT;
}
void PT_CALL glProgramUniform3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3dv;
}
void PT_CALL glProgramUniform3dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3dvEXT;
}
void PT_CALL glProgramUniform3f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3f;
}
void PT_CALL glProgramUniform3fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3fEXT;
}
void PT_CALL glProgramUniform3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3fv;
}
void PT_CALL glProgramUniform3fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3fvEXT;
}
void PT_CALL glProgramUniform3i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3i64ARB;
}
void PT_CALL glProgramUniform3i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3i64NV;
}
void PT_CALL glProgramUniform3i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3i64vARB;
}
void PT_CALL glProgramUniform3i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3i64vNV;
}
void PT_CALL glProgramUniform3i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3i;
}
void PT_CALL glProgramUniform3iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3iEXT;
}
void PT_CALL glProgramUniform3iv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3iv;
}
void PT_CALL glProgramUniform3ivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3ivEXT;
}
void PT_CALL glProgramUniform3ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3ui64ARB;
}
void PT_CALL glProgramUniform3ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3ui64NV;
}
void PT_CALL glProgramUniform3ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3ui64vARB;
}
void PT_CALL glProgramUniform3ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3ui64vNV;
}
void PT_CALL glProgramUniform3ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3ui;
}
void PT_CALL glProgramUniform3uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3uiEXT;
}
void PT_CALL glProgramUniform3uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3uiv;
}
void PT_CALL glProgramUniform3uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform3uivEXT;
}
void PT_CALL glProgramUniform4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4d;
}
void PT_CALL glProgramUniform4dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4dEXT;
}
void PT_CALL glProgramUniform4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4dv;
}
void PT_CALL glProgramUniform4dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4dvEXT;
}
void PT_CALL glProgramUniform4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4f;
}
void PT_CALL glProgramUniform4fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4fEXT;
}
void PT_CALL glProgramUniform4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4fv;
}
void PT_CALL glProgramUniform4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4fvEXT;
}
void PT_CALL glProgramUniform4i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4i64ARB;
}
void PT_CALL glProgramUniform4i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4i64NV;
}
void PT_CALL glProgramUniform4i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4i64vARB;
}
void PT_CALL glProgramUniform4i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4i64vNV;
}
void PT_CALL glProgramUniform4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4i;
}
void PT_CALL glProgramUniform4iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4iEXT;
}
void PT_CALL glProgramUniform4iv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4iv;
}
void PT_CALL glProgramUniform4ivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4ivEXT;
}
void PT_CALL glProgramUniform4ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4ui64ARB;
}
void PT_CALL glProgramUniform4ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4ui64NV;
}
void PT_CALL glProgramUniform4ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4ui64vARB;
}
void PT_CALL glProgramUniform4ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4ui64vNV;
}
void PT_CALL glProgramUniform4ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4ui;
}
void PT_CALL glProgramUniform4uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4uiEXT;
}
void PT_CALL glProgramUniform4uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4uiv;
}
void PT_CALL glProgramUniform4uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniform4uivEXT;
}
void PT_CALL glProgramUniformHandleui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformHandleui64ARB;
}
void PT_CALL glProgramUniformHandleui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformHandleui64NV;
}
void PT_CALL glProgramUniformHandleui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformHandleui64vARB;
}
void PT_CALL glProgramUniformHandleui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformHandleui64vNV;
}
void PT_CALL glProgramUniformMatrix2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2dv;
}
void PT_CALL glProgramUniformMatrix2dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2dvEXT;
}
void PT_CALL glProgramUniformMatrix2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2fv;
}
void PT_CALL glProgramUniformMatrix2fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2fvEXT;
}
void PT_CALL glProgramUniformMatrix2x3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x3dv;
}
void PT_CALL glProgramUniformMatrix2x3dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x3dvEXT;
}
void PT_CALL glProgramUniformMatrix2x3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x3fv;
}
void PT_CALL glProgramUniformMatrix2x3fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x3fvEXT;
}
void PT_CALL glProgramUniformMatrix2x4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x4dv;
}
void PT_CALL glProgramUniformMatrix2x4dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x4dvEXT;
}
void PT_CALL glProgramUniformMatrix2x4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x4fv;
}
void PT_CALL glProgramUniformMatrix2x4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix2x4fvEXT;
}
void PT_CALL glProgramUniformMatrix3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3dv;
}
void PT_CALL glProgramUniformMatrix3dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3dvEXT;
}
void PT_CALL glProgramUniformMatrix3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3fv;
}
void PT_CALL glProgramUniformMatrix3fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3fvEXT;
}
void PT_CALL glProgramUniformMatrix3x2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x2dv;
}
void PT_CALL glProgramUniformMatrix3x2dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x2dvEXT;
}
void PT_CALL glProgramUniformMatrix3x2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x2fv;
}
void PT_CALL glProgramUniformMatrix3x2fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x2fvEXT;
}
void PT_CALL glProgramUniformMatrix3x4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x4dv;
}
void PT_CALL glProgramUniformMatrix3x4dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x4dvEXT;
}
void PT_CALL glProgramUniformMatrix3x4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x4fv;
}
void PT_CALL glProgramUniformMatrix3x4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix3x4fvEXT;
}
void PT_CALL glProgramUniformMatrix4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4dv;
}
void PT_CALL glProgramUniformMatrix4dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4dvEXT;
}
void PT_CALL glProgramUniformMatrix4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4fv;
}
void PT_CALL glProgramUniformMatrix4fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4fvEXT;
}
void PT_CALL glProgramUniformMatrix4x2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x2dv;
}
void PT_CALL glProgramUniformMatrix4x2dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x2dvEXT;
}
void PT_CALL glProgramUniformMatrix4x2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x2fv;
}
void PT_CALL glProgramUniformMatrix4x2fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x2fvEXT;
}
void PT_CALL glProgramUniformMatrix4x3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x3dv;
}
void PT_CALL glProgramUniformMatrix4x3dvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x3dvEXT;
}
void PT_CALL glProgramUniformMatrix4x3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x3fv;
}
void PT_CALL glProgramUniformMatrix4x3fvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformMatrix4x3fvEXT;
}
void PT_CALL glProgramUniformui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformui64NV;
}
void PT_CALL glProgramUniformui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramUniformui64vNV;
}
void PT_CALL glProgramVertexLimitNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glProgramVertexLimitNV;
}
void PT_CALL glProvokingVertex(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProvokingVertex, 1);
}
void PT_CALL glProvokingVertexEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glProvokingVertexEXT, 1);
}
void PT_CALL glPushAttrib(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPushAttrib, 1);
}
void PT_CALL glPushClientAttrib(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPushClientAttrib, 1);
}
void PT_CALL glPushClientAttribDefaultEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPushClientAttribDefaultEXT, 1);
}
void PT_CALL glPushDebugGroup(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPushDebugGroup;
}
void PT_CALL glPushGroupMarkerEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glPushGroupMarkerEXT;
}
void PT_CALL glPushMatrix(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPushMatrix, 0);
}
void PT_CALL glPushName(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glPushName, 1);
}
void PT_CALL glQueryCounter(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glQueryCounter;
}
void PT_CALL glQueryMatrixxOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glQueryMatrixxOES;
}
void PT_CALL glQueryObjectParameteruiAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glQueryObjectParameteruiAMD;
}
void PT_CALL glQueryResourceNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glQueryResourceNV;
}
void PT_CALL glQueryResourceTagNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glQueryResourceTagNV;
}
void PT_CALL glRasterPos2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2d, 4);
}
void PT_CALL glRasterPos2dv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2dv, 1);
}
void PT_CALL glRasterPos2f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2f, 2);
}
void PT_CALL glRasterPos2fv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2fv, 1);
}
void PT_CALL glRasterPos2i(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2i, 2);
}
void PT_CALL glRasterPos2iv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2iv, 1);
}
void PT_CALL glRasterPos2s(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2s, 2);
}
void PT_CALL glRasterPos2sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos2sv, 1);
}
void PT_CALL glRasterPos2xOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRasterPos2xOES;
}
void PT_CALL glRasterPos2xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRasterPos2xvOES;
}
void PT_CALL glRasterPos3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3d, 6);
}
void PT_CALL glRasterPos3dv(uint32_t arg0) {
    fifoAddData(0, arg0, 3*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3dv, 1);
}
void PT_CALL glRasterPos3f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3f, 3);
}
void PT_CALL glRasterPos3fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3fv, 1);
}
void PT_CALL glRasterPos3i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3i, 3);
}
void PT_CALL glRasterPos3iv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3iv, 1);
}
void PT_CALL glRasterPos3s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3s, 3);
}
void PT_CALL glRasterPos3sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos3sv, 1);
}
void PT_CALL glRasterPos3xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRasterPos3xOES;
}
void PT_CALL glRasterPos3xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRasterPos3xvOES;
}
void PT_CALL glRasterPos4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4d, 8);
}
void PT_CALL glRasterPos4dv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4dv, 1);
}
void PT_CALL glRasterPos4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4f, 4);
}
void PT_CALL glRasterPos4fv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4fv, 1);
}
void PT_CALL glRasterPos4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4i, 4);
}
void PT_CALL glRasterPos4iv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4iv, 1);
}
void PT_CALL glRasterPos4s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4s, 4);
}
void PT_CALL glRasterPos4sv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(short));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRasterPos4sv, 1);
}
void PT_CALL glRasterPos4xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRasterPos4xOES;
}
void PT_CALL glRasterPos4xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRasterPos4xvOES;
}
void PT_CALL glRasterSamplesEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRasterSamplesEXT;
}
void PT_CALL glReadBuffer(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glReadBuffer, 1);
}
void PT_CALL glReadInstrumentsSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReadInstrumentsSGIX;
}
void PT_CALL glReadPixels(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReadPixels;
    if (pixPackBuf == 0) {
        uint32_t szPix;
        szPix = ((szPackWidth == 0)? arg2:szPackWidth) * arg3 * szgldata(arg4, arg5);
        memcpy((unsigned char *)arg6, fbtm, szPix);
    }
}
void PT_CALL glReadnPixels(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReadnPixels;
}
void PT_CALL glReadnPixelsARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReadnPixelsARB;
}
void PT_CALL glRectd(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRectd, 8);
}
void PT_CALL glRectdv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg0, sizeof(double)); fifoAddData(0, arg1, sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRectdv, 2);
}
void PT_CALL glRectf(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRectf, 4);
}
void PT_CALL glRectfv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg0, ALIGNED(sizeof(float))); fifoAddData(0, arg1, ALIGNED(sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRectfv, 2);
}
void PT_CALL glRecti(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRecti, 4);
}
void PT_CALL glRectiv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg0, ALIGNED(sizeof(int))); fifoAddData(0, arg1, ALIGNED(sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRectiv, 2);
}
void PT_CALL glRects(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRects, 4);
}
void PT_CALL glRectsv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg0, ALIGNED(sizeof(short))); fifoAddData(0, arg1, ALIGNED(sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRectsv, 2);
}
void PT_CALL glRectxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRectxOES;
}
void PT_CALL glRectxvOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRectxvOES;
}
void PT_CALL glReferencePlaneSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReferencePlaneSGIX;
}
void PT_CALL glReleaseKeyedMutexWin32EXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReleaseKeyedMutexWin32EXT;
}
void PT_CALL glReleaseShaderCompiler(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReleaseShaderCompiler;
}
void PT_CALL glRenderGpuMaskNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRenderGpuMaskNV;
}
uint32_t PT_CALL glRenderMode(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRenderMode;
    ret = *pt0;
    return ret;
}
void PT_CALL glRenderbufferStorage(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRenderbufferStorage, 4);
}
void PT_CALL glRenderbufferStorageEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRenderbufferStorageEXT, 4);
}
void PT_CALL glRenderbufferStorageMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRenderbufferStorageMultisample, 5);
}
void PT_CALL glRenderbufferStorageMultisampleAdvancedAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRenderbufferStorageMultisampleAdvancedAMD;
}
void PT_CALL glRenderbufferStorageMultisampleCoverageNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRenderbufferStorageMultisampleCoverageNV;
}
void PT_CALL glRenderbufferStorageMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRenderbufferStorageMultisampleEXT, 5);
}
void PT_CALL glReplacementCodePointerSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodePointerSUN;
}
void PT_CALL glReplacementCodeubSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeubSUN;
}
void PT_CALL glReplacementCodeubvSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeubvSUN;
}
void PT_CALL glReplacementCodeuiColor3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiColor3fVertex3fSUN;
}
void PT_CALL glReplacementCodeuiColor3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiColor3fVertex3fvSUN;
}
void PT_CALL glReplacementCodeuiColor4fNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiColor4fNormal3fVertex3fSUN;
}
void PT_CALL glReplacementCodeuiColor4fNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiColor4fNormal3fVertex3fvSUN;
}
void PT_CALL glReplacementCodeuiColor4ubVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiColor4ubVertex3fSUN;
}
void PT_CALL glReplacementCodeuiColor4ubVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiColor4ubVertex3fvSUN;
}
void PT_CALL glReplacementCodeuiNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiNormal3fVertex3fSUN;
}
void PT_CALL glReplacementCodeuiNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiNormal3fVertex3fvSUN;
}
void PT_CALL glReplacementCodeuiSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiSUN;
}
void PT_CALL glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fSUN;
}
void PT_CALL glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fvSUN;
}
void PT_CALL glReplacementCodeuiTexCoord2fNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiTexCoord2fNormal3fVertex3fSUN;
}
void PT_CALL glReplacementCodeuiTexCoord2fNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiTexCoord2fNormal3fVertex3fvSUN;
}
void PT_CALL glReplacementCodeuiTexCoord2fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiTexCoord2fVertex3fSUN;
}
void PT_CALL glReplacementCodeuiTexCoord2fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiTexCoord2fVertex3fvSUN;
}
void PT_CALL glReplacementCodeuiVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiVertex3fSUN;
}
void PT_CALL glReplacementCodeuiVertex3fvSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuiVertex3fvSUN;
}
void PT_CALL glReplacementCodeuivSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeuivSUN;
}
void PT_CALL glReplacementCodeusSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeusSUN;
}
void PT_CALL glReplacementCodeusvSUN(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glReplacementCodeusvSUN;
}
void PT_CALL glRequestResidentProgramsNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0 * sizeof(uint32_t));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRequestResidentProgramsNV, 2);
}
void PT_CALL glResetHistogram(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResetHistogram;
}
void PT_CALL glResetHistogramEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResetHistogramEXT;
}
void PT_CALL glResetMemoryObjectParameterNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResetMemoryObjectParameterNV;
}
void PT_CALL glResetMinmax(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResetMinmax;
}
void PT_CALL glResetMinmaxEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResetMinmaxEXT;
}
void PT_CALL glResizeBuffersMESA(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResizeBuffersMESA;
}
void PT_CALL glResolveDepthValuesNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResolveDepthValuesNV;
}
void PT_CALL glResumeTransformFeedback(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResumeTransformFeedback;
}
void PT_CALL glResumeTransformFeedbackNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glResumeTransformFeedbackNV;
}
void PT_CALL glRotated(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRotated, 8);
}
void PT_CALL glRotatef(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glRotatef, 4);
}
void PT_CALL glRotatexOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glRotatexOES;
}
void PT_CALL glSampleCoverage(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSampleCoverage;
}
void PT_CALL glSampleCoverageARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSampleCoverageARB;
}
void PT_CALL glSampleMapATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSampleMapATI, 3);
}
void PT_CALL glSampleMaskEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSampleMaskEXT;
}
void PT_CALL glSampleMaskIndexedNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSampleMaskIndexedNV;
}
void PT_CALL glSampleMaskSGIS(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSampleMaskSGIS;
}
void PT_CALL glSampleMaski(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSampleMaski;
}
void PT_CALL glSamplePatternEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSamplePatternEXT;
}
void PT_CALL glSamplePatternSGIS(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSamplePatternSGIS;
}
void PT_CALL glSamplerParameterIiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSamplerParameterIiv, 3);
}
void PT_CALL glSamplerParameterIuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(unsigned int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSamplerParameterIuiv, 3);
}
void PT_CALL glSamplerParameterf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSamplerParameterf, 3);
}
void PT_CALL glSamplerParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSamplerParameterfv, 3);
}
void PT_CALL glSamplerParameteri(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSamplerParameteri, 3);
}
void PT_CALL glSamplerParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSamplerParameteriv, 3);
}
void PT_CALL glScaled(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glScaled, 6);
}
void PT_CALL glScalef(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glScalef, 3);
}
void PT_CALL glScalexOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glScalexOES;
}
void PT_CALL glScissor(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glScissor, 4);
}
void PT_CALL glScissorArrayv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glScissorArrayv, 3);
}
void PT_CALL glScissorExclusiveArrayvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glScissorExclusiveArrayvNV;
}
void PT_CALL glScissorExclusiveNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glScissorExclusiveNV;
}
void PT_CALL glScissorIndexed(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glScissorIndexed, 5);
}
void PT_CALL glScissorIndexedv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glScissorIndexedv, 2);
}
void PT_CALL glSecondaryColor3b(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3b, 3);
}
void PT_CALL glSecondaryColor3bEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3bEXT, 3);
}
void PT_CALL glSecondaryColor3bv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3bv, 1);
}
void PT_CALL glSecondaryColor3bvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3bvEXT, 1);
}
void PT_CALL glSecondaryColor3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3d, 6);
}
void PT_CALL glSecondaryColor3dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3dEXT, 6);
}
void PT_CALL glSecondaryColor3dv(uint32_t arg0) {
    fifoAddData(0, arg0, 3*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3dv, 1);
}
void PT_CALL glSecondaryColor3dvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, 3*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3dvEXT, 1);
}
void PT_CALL glSecondaryColor3f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3f, 3);
}
void PT_CALL glSecondaryColor3fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3fEXT, 3);
}
void PT_CALL glSecondaryColor3fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3fv, 1);
}
void PT_CALL glSecondaryColor3fvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3fvEXT, 1);
}
void PT_CALL glSecondaryColor3hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSecondaryColor3hNV;
}
void PT_CALL glSecondaryColor3hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSecondaryColor3hvNV;
}
void PT_CALL glSecondaryColor3i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3i, 3);
}
void PT_CALL glSecondaryColor3iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3iEXT, 3);
}
void PT_CALL glSecondaryColor3iv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3iv, 1);
}
void PT_CALL glSecondaryColor3ivEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3ivEXT, 1);
}
void PT_CALL glSecondaryColor3s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3s, 3);
}
void PT_CALL glSecondaryColor3sEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3sEXT, 3);
}
void PT_CALL glSecondaryColor3sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3sv, 1);
}
void PT_CALL glSecondaryColor3svEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3svEXT, 1);
}
void PT_CALL glSecondaryColor3ub(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3ub, 3);
}
void PT_CALL glSecondaryColor3ubEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3ubEXT, 3);
}
void PT_CALL glSecondaryColor3ubv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3ubv, 1);
}
void PT_CALL glSecondaryColor3ubvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned char)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3ubvEXT, 1);
}
void PT_CALL glSecondaryColor3ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3ui, 3);
}
void PT_CALL glSecondaryColor3uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3uiEXT, 3);
}
void PT_CALL glSecondaryColor3uiv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3uiv, 1);
}
void PT_CALL glSecondaryColor3uivEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3uivEXT, 1);
}
void PT_CALL glSecondaryColor3us(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3us, 3);
}
void PT_CALL glSecondaryColor3usEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3usEXT, 3);
}
void PT_CALL glSecondaryColor3usv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3usv, 1);
}
void PT_CALL glSecondaryColor3usvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(unsigned short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColor3usvEXT, 1);
}
void PT_CALL glSecondaryColorFormatNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSecondaryColorFormatNV;
}
void PT_CALL glSecondaryColorP3ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSecondaryColorP3ui;
}
void PT_CALL glSecondaryColorP3uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSecondaryColorP3uiv;
}
void PT_CALL glSecondaryColorPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColorPointer, 4);
    vtxarry_init(&vtxArry.SecondaryColor, arg0, arg1, arg2, (void *)arg3);
}
void PT_CALL glSecondaryColorPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSecondaryColorPointerEXT, 4);
    vtxarry_init(&vtxArry.SecondaryColor, arg0, arg1, arg2, (void *)arg3);
}
void PT_CALL glSecondaryColorPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSecondaryColorPointerListIBM;
}
void PT_CALL glSelectBuffer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSelectBuffer;
    fifoOutData(0, arg1, (arg0*sizeof(uint32_t)));
}
void PT_CALL glSelectPerfMonitorCountersAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSelectPerfMonitorCountersAMD;
}
void PT_CALL glSemaphoreParameterui64vEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSemaphoreParameterui64vEXT;
}
void PT_CALL glSeparableFilter2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSeparableFilter2D;
}
void PT_CALL glSeparableFilter2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSeparableFilter2DEXT;
}
void PT_CALL glSetFenceAPPLE(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSetFenceAPPLE, 1);
}
void PT_CALL glSetFenceNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSetFenceNV, 1);
}
void PT_CALL glSetFragmentShaderConstantATI(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glSetFragmentShaderConstantATI, 2);
}
void PT_CALL glSetInvariantEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSetInvariantEXT;
}
void PT_CALL glSetLocalConstantEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSetLocalConstantEXT;
}
void PT_CALL glSetMultisamplefvAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSetMultisamplefvAMD;
}
void PT_CALL glShadeModel(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glShadeModel, 1);
}
void PT_CALL glShaderBinary(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShaderBinary;
}
void PT_CALL glShaderOp1EXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShaderOp1EXT;
}
void PT_CALL glShaderOp2EXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShaderOp2EXT;
}
void PT_CALL glShaderOp3EXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShaderOp3EXT;
}
void PT_CALL glShaderSource(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    int i;
    const int strz[2] = { 0, 0 };
    char **str = (char **)arg2;
    if (arg3) {
        int *len = (int *)arg3;
        fifoAddData(0, arg3, ALIGNED((arg1 * sizeof(uint32_t))));
        for (i = 0; i < arg1; i++) {
            fifoAddData(0, (uint32_t)str[i], (len[i] > 0)? ALIGNED(len[i]):ALIGNED(strlen(str[i])));
            fifoAddData(0, (uint32_t)strz, ALIGNED(1));
        }
    }
    else {
        for (i = 0; i < arg1; i++)
            fifoAddData(0, (uint32_t)str[i], ALIGNED((strlen(str[i]) + 1)));
    }
    fifoAddData(0, arg2, (arg1 * ALIGNED(1)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glShaderSource, 4);
}
void PT_CALL glShaderSourceARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    int i;
    const int strz[2] = { 0, 0 };
    char **str = (char **)arg2;
    if (arg3) {
        int *len = (int *)arg3;
        fifoAddData(0, arg3, ALIGNED(arg1*sizeof(int)));
        for (i = 0; i < arg1; i++) {
            fifoAddData(0, (uint32_t)str[i], (len[i] > 0)? ALIGNED(len[i]):ALIGNED(strlen(str[i])));
            fifoAddData(0, (uint32_t)strz, ALIGNED(1));
        }
    }
    else {
        for (i = 0; i < arg1; i++)
            fifoAddData(0, (uint32_t)str[i], ALIGNED((strlen(str[i]) + 1)));
    }
    fifoAddData(0, arg2, ALIGNED((arg1 * sizeof(uint32_t))));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glShaderSourceARB, 4);
}
void PT_CALL glShaderStorageBlockBinding(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShaderStorageBlockBinding;
}
void PT_CALL glShadingRateImageBarrierNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShadingRateImageBarrierNV;
}
void PT_CALL glShadingRateImagePaletteNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShadingRateImagePaletteNV;
}
void PT_CALL glShadingRateSampleOrderCustomNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShadingRateSampleOrderCustomNV;
}
void PT_CALL glShadingRateSampleOrderNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glShadingRateSampleOrderNV;
}
void PT_CALL glSharpenTexFuncSGIS(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSharpenTexFuncSGIS;
}
void PT_CALL glSignalSemaphoreEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSignalSemaphoreEXT;
}
void PT_CALL glSignalVkFenceNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSignalVkFenceNV;
}
void PT_CALL glSignalVkSemaphoreNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSignalVkSemaphoreNV;
}
void PT_CALL glSpecializeShader(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSpecializeShader;
}
void PT_CALL glSpecializeShaderARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSpecializeShaderARB;
}
void PT_CALL glSpriteParameterfSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSpriteParameterfSGIX;
}
void PT_CALL glSpriteParameterfvSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSpriteParameterfvSGIX;
}
void PT_CALL glSpriteParameteriSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSpriteParameteriSGIX;
}
void PT_CALL glSpriteParameterivSGIX(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSpriteParameterivSGIX;
}
void PT_CALL glStartInstrumentsSGIX(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStartInstrumentsSGIX;
}
void PT_CALL glStateCaptureNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStateCaptureNV;
}
void PT_CALL glStencilClearTagEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilClearTagEXT;
}
void PT_CALL glStencilFillPathInstancedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilFillPathInstancedNV;
}
void PT_CALL glStencilFillPathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilFillPathNV;
}
void PT_CALL glStencilFunc(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilFunc, 3);
}
void PT_CALL glStencilFuncSeparate(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilFuncSeparate, 4);
}
void PT_CALL glStencilFuncSeparateATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilFuncSeparateATI, 4);
}
void PT_CALL glStencilMask(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilMask, 1);
}
void PT_CALL glStencilMaskSeparate(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilMaskSeparate, 2);
}
void PT_CALL glStencilOp(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilOp, 3);
}
void PT_CALL glStencilOpSeparate(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilOpSeparate, 4);
}
void PT_CALL glStencilOpSeparateATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glStencilOpSeparateATI, 4);
}
void PT_CALL glStencilOpValueAMD(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilOpValueAMD;
}
void PT_CALL glStencilStrokePathInstancedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilStrokePathInstancedNV;
}
void PT_CALL glStencilStrokePathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilStrokePathNV;
}
void PT_CALL glStencilThenCoverFillPathInstancedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilThenCoverFillPathInstancedNV;
}
void PT_CALL glStencilThenCoverFillPathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilThenCoverFillPathNV;
}
void PT_CALL glStencilThenCoverStrokePathInstancedNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilThenCoverStrokePathInstancedNV;
}
void PT_CALL glStencilThenCoverStrokePathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStencilThenCoverStrokePathNV;
}
void PT_CALL glStopInstrumentsSGIX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStopInstrumentsSGIX;
}
void PT_CALL glStringMarkerGREMEDY(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glStringMarkerGREMEDY;
}
void PT_CALL glSubpixelPrecisionBiasNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSubpixelPrecisionBiasNV;
}
void PT_CALL glSwizzleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSwizzleEXT;
}
void PT_CALL glSyncTextureINTEL(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glSyncTextureINTEL;
}
void PT_CALL glTagSampleBufferSGIX(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTagSampleBufferSGIX;
}
void PT_CALL glTangent3bEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3bEXT;
}
void PT_CALL glTangent3bvEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3bvEXT;
}
void PT_CALL glTangent3dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3dEXT;
}
void PT_CALL glTangent3dvEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3dvEXT;
}
void PT_CALL glTangent3fEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3fEXT;
}
void PT_CALL glTangent3fvEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3fvEXT;
}
void PT_CALL glTangent3iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3iEXT;
}
void PT_CALL glTangent3ivEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3ivEXT;
}
void PT_CALL glTangent3sEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3sEXT;
}
void PT_CALL glTangent3svEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangent3svEXT;
}
void PT_CALL glTangentPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTangentPointerEXT;
}
void PT_CALL glTbufferMask3DFX(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTbufferMask3DFX;
}
void PT_CALL glTessellationFactorAMD(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTessellationFactorAMD;
}
void PT_CALL glTessellationModeAMD(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTessellationModeAMD;
}
uint32_t PT_CALL glTestFenceAPPLE(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTestFenceAPPLE;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glTestFenceNV(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTestFenceNV;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glTestObjectAPPLE(uint32_t arg0, uint32_t arg1) {
    uint32_t ret;
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTestObjectAPPLE;
    ret = *pt0;
    return ret;
}
void PT_CALL glTexAttachMemoryNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexAttachMemoryNV;
}
void PT_CALL glTexBuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexBuffer, 3);
}
void PT_CALL glTexBufferARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexBufferARB, 3);
}
void PT_CALL glTexBufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexBufferEXT, 3);
}
void PT_CALL glTexBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexBufferRange, 5);
}
void PT_CALL glTexBumpParameterfvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexBumpParameterfvATI;
}
void PT_CALL glTexBumpParameterivATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexBumpParameterivATI;
}
void PT_CALL glTexCoord1bOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord1bOES;
}
void PT_CALL glTexCoord1bvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord1bvOES;
}
void PT_CALL glTexCoord1d(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1d, 2);
}
void PT_CALL glTexCoord1dv(uint32_t arg0) {
    fifoAddData(0, arg0, sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1dv, 1);
}
void PT_CALL glTexCoord1f(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1f, 1);
}
void PT_CALL glTexCoord1fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1fv, 1);
}
void PT_CALL glTexCoord1hNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord1hNV;
}
void PT_CALL glTexCoord1hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord1hvNV;
}
void PT_CALL glTexCoord1i(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1i, 1);
}
void PT_CALL glTexCoord1iv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1iv, 1);
}
void PT_CALL glTexCoord1s(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1s, 1);
}
void PT_CALL glTexCoord1sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord1sv, 1);
}
void PT_CALL glTexCoord1xOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord1xOES;
}
void PT_CALL glTexCoord1xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord1xvOES;
}
void PT_CALL glTexCoord2bOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2bOES;
}
void PT_CALL glTexCoord2bvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2bvOES;
}
void PT_CALL glTexCoord2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2d, 4);
}
void PT_CALL glTexCoord2dv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2dv, 1);
}
void PT_CALL glTexCoord2f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2f, 2);
}
void PT_CALL glTexCoord2fColor3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fColor3fVertex3fSUN;
}
void PT_CALL glTexCoord2fColor3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fColor3fVertex3fvSUN;
}
void PT_CALL glTexCoord2fColor4fNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fColor4fNormal3fVertex3fSUN;
}
void PT_CALL glTexCoord2fColor4fNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fColor4fNormal3fVertex3fvSUN;
}
void PT_CALL glTexCoord2fColor4ubVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fColor4ubVertex3fSUN;
}
void PT_CALL glTexCoord2fColor4ubVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fColor4ubVertex3fvSUN;
}
void PT_CALL glTexCoord2fNormal3fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fNormal3fVertex3fSUN;
}
void PT_CALL glTexCoord2fNormal3fVertex3fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fNormal3fVertex3fvSUN;
}
void PT_CALL glTexCoord2fVertex3fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fVertex3fSUN;
}
void PT_CALL glTexCoord2fVertex3fvSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2fVertex3fvSUN;
}
void PT_CALL glTexCoord2fv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2fv, 1);
}
void PT_CALL glTexCoord2hNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2hNV;
}
void PT_CALL glTexCoord2hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2hvNV;
}
void PT_CALL glTexCoord2i(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2i, 2);
}
void PT_CALL glTexCoord2iv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2iv, 1);
}
void PT_CALL glTexCoord2s(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2s, 2);
}
void PT_CALL glTexCoord2sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord2sv, 1);
}
void PT_CALL glTexCoord2xOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2xOES;
}
void PT_CALL glTexCoord2xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord2xvOES;
}
void PT_CALL glTexCoord3bOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord3bOES;
}
void PT_CALL glTexCoord3bvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord3bvOES;
}
void PT_CALL glTexCoord3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3d, 6);
}
void PT_CALL glTexCoord3dv(uint32_t arg0) {
    fifoAddData(0, arg0, 3*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3dv, 1);
}
void PT_CALL glTexCoord3f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3f, 3);
}
void PT_CALL glTexCoord3fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3fv, 1);
}
void PT_CALL glTexCoord3hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord3hNV;
}
void PT_CALL glTexCoord3hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord3hvNV;
}
void PT_CALL glTexCoord3i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3i, 3);
}
void PT_CALL glTexCoord3iv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3iv, 1);
}
void PT_CALL glTexCoord3s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3s, 3);
}
void PT_CALL glTexCoord3sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord3sv, 1);
}
void PT_CALL glTexCoord3xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord3xOES;
}
void PT_CALL glTexCoord3xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord3xvOES;
}
void PT_CALL glTexCoord4bOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4bOES;
}
void PT_CALL glTexCoord4bvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4bvOES;
}
void PT_CALL glTexCoord4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4d, 8);
}
void PT_CALL glTexCoord4dv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4dv, 1);
}
void PT_CALL glTexCoord4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4f, 4);
}
void PT_CALL glTexCoord4fColor4fNormal3fVertex4fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; pt[14] = arg13; pt[15] = arg14; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4fColor4fNormal3fVertex4fSUN;
}
void PT_CALL glTexCoord4fColor4fNormal3fVertex4fvSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4fColor4fNormal3fVertex4fvSUN;
}
void PT_CALL glTexCoord4fVertex4fSUN(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4fVertex4fSUN;
}
void PT_CALL glTexCoord4fVertex4fvSUN(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4fVertex4fvSUN;
}
void PT_CALL glTexCoord4fv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4fv, 1);
}
void PT_CALL glTexCoord4hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4hNV;
}
void PT_CALL glTexCoord4hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4hvNV;
}
void PT_CALL glTexCoord4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4i, 4);
}
void PT_CALL glTexCoord4iv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4iv, 1);
}
void PT_CALL glTexCoord4s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4s, 4);
}
void PT_CALL glTexCoord4sv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(short));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoord4sv, 1);
}
void PT_CALL glTexCoord4xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4xOES;
}
void PT_CALL glTexCoord4xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoord4xvOES;
}
void PT_CALL glTexCoordFormatNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordFormatNV;
}
void PT_CALL glTexCoordP1ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP1ui;
}
void PT_CALL glTexCoordP1uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP1uiv;
}
void PT_CALL glTexCoordP2ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP2ui;
}
void PT_CALL glTexCoordP2uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP2uiv;
}
void PT_CALL glTexCoordP3ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP3ui;
}
void PT_CALL glTexCoordP3uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP3uiv;
}
void PT_CALL glTexCoordP4ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP4ui;
}
void PT_CALL glTexCoordP4uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordP4uiv;
}
void PT_CALL glTexCoordPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoordPointer, 4);
    vtxarry_init(&vtxArry.TexCoord[vtxArry.texUnit], arg0, arg1, arg2, (void *)arg3);
}
void PT_CALL glTexCoordPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexCoordPointerEXT, 5);
    vtxarry_init(&vtxArry.TexCoord[vtxArry.texUnit], arg0, arg1, arg2, (void *)arg4);
}
void PT_CALL glTexCoordPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordPointerListIBM;
}
void PT_CALL glTexCoordPointervINTEL(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexCoordPointervINTEL;
}
void PT_CALL glTexEnvf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexEnvf, 3);
}
void PT_CALL glTexEnvfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexEnvfv, 3);
}
void PT_CALL glTexEnvi(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexEnvi, 3);
}
void PT_CALL glTexEnviv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexEnviv, 3);
}
void PT_CALL glTexEnvxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexEnvxOES;
}
void PT_CALL glTexEnvxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexEnvxvOES;
}
void PT_CALL glTexFilterFuncSGIS(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexFilterFuncSGIS;
}
void PT_CALL glTexGend(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexGend, 4);
}
void PT_CALL glTexGendv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, szglname(arg1)*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexGendv, 3);
}
void PT_CALL glTexGenf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexGenf, 3);
}
void PT_CALL glTexGenfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexGenfv, 3);
}
void PT_CALL glTexGeni(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexGeni, 3);
}
void PT_CALL glTexGeniv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexGeniv, 3);
}
void PT_CALL glTexGenxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexGenxOES;
}
void PT_CALL glTexGenxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexGenxvOES;
}
void PT_CALL glTexImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    uint32_t szTex, *texPtr;
    if (arg7 && (pixUnpackBuf == 0)) {
        szTex = ((szUnpackWidth == 0)? arg3:szUnpackWidth) * szgldata(arg5, arg6);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg7, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexImage1D;
}
void PT_CALL glTexImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    uint32_t szTex, *texPtr;
    if (arg8 && (pixUnpackBuf == 0)) {
        szTex = ((szUnpackWidth == 0)? arg3:szUnpackWidth) * arg4 * szgldata(arg6, arg7);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        //DPRINTF("TexImage2D() %x,%x,%x,%x,%x,%x,%x,%x size %07x", arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, szTex);
        FBTMMCPY(texPtr, (unsigned char *)arg8, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexImage2D;
}
void PT_CALL glTexImage2DMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexImage2DMultisample, 6);
}
void PT_CALL glTexImage2DMultisampleCoverageNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexImage2DMultisampleCoverageNV;
}
void PT_CALL glTexImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    uint32_t szTex, *texPtr;
    if (arg9 && (pixUnpackBuf == 0)) {
        szTex = ((szUnpackWidth == 0)? arg3:szUnpackWidth) * ((szUnpackHeight == 0)? arg4:szUnpackHeight) * arg5 * szgldata(arg7, arg8);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg9, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexImage3D;
}
void PT_CALL glTexImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    uint32_t szTex, *texPtr;
    if (arg9 && (pixUnpackBuf == 0)) {
        szTex = ((szUnpackWidth == 0)? arg3:szUnpackWidth) * ((szUnpackHeight == 0)? arg4:szUnpackHeight) * arg5 * szgldata(arg7, arg8);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        FBTMMCPY(texPtr, (unsigned char *)arg9, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexImage3DEXT;
}
void PT_CALL glTexImage3DMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexImage3DMultisample, 7);
}
void PT_CALL glTexImage3DMultisampleCoverageNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexImage3DMultisampleCoverageNV;
}
void PT_CALL glTexImage4DSGIS(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexImage4DSGIS;
}
void PT_CALL glTexPageCommitmentARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexPageCommitmentARB;
}
void PT_CALL glTexParameterIiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexParameterIiv;
}
void PT_CALL glTexParameterIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexParameterIivEXT;
}
void PT_CALL glTexParameterIuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexParameterIuiv;
}
void PT_CALL glTexParameterIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexParameterIuivEXT;
}
void PT_CALL glTexParameterf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    float *f = (float *)&pt[3];
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    if (texClampFix && GL_CLAMP == *f) *f = GL_CLAMP_TO_EDGE;
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexParameterf, 3);
}
void PT_CALL glTexParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexParameterfv, 3);
}
void PT_CALL glTexParameteri(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    if (texClampFix && GL_CLAMP == pt[3]) pt[3] = GL_CLAMP_TO_EDGE;
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexParameteri, 3);
}
void PT_CALL glTexParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(szglname(arg1)*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexParameteriv, 3);
}
void PT_CALL glTexParameterxOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexParameterxOES;
}
void PT_CALL glTexParameterxvOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexParameterxvOES;
}
void PT_CALL glTexRenderbufferNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexRenderbufferNV;
}
void PT_CALL glTexStorage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexStorage1D, 4);
}
void PT_CALL glTexStorage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexStorage2D, 5);
}
void PT_CALL glTexStorage2DMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexStorage2DMultisample, 6);
}
void PT_CALL glTexStorage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexStorage3D, 6);
}
void PT_CALL glTexStorage3DMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTexStorage3DMultisample, 7);
}
void PT_CALL glTexStorageMem1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexStorageMem1DEXT;
}
void PT_CALL glTexStorageMem2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexStorageMem2DEXT;
}
void PT_CALL glTexStorageMem2DMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexStorageMem2DMultisampleEXT;
}
void PT_CALL glTexStorageMem3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexStorageMem3DEXT;
}
void PT_CALL glTexStorageMem3DMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexStorageMem3DMultisampleEXT;
}
void PT_CALL glTexStorageSparseAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexStorageSparseAMD;
}
void PT_CALL glTexSubImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (pixUnpackBuf == 0) {
        uint32_t szTex, *texPtr;
        szTex = ((szUnpackWidth == 0)? arg3:szUnpackWidth) * szgldata(arg4, arg5);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        szTex -= ((szUnpackWidth)? arg2:0) * szgldata(arg4, arg5);
        FBTMMCPY(texPtr, (unsigned char *)arg6, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexSubImage1D;
}
void PT_CALL glTexSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    if (pixUnpackBuf == 0) {
        uint32_t szTex, *texPtr;
        szTex = ((szUnpackWidth == 0)? arg3:szUnpackWidth) * szgldata(arg4, arg5);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        szTex -= ((szUnpackWidth)? arg2:0) * szgldata(arg4, arg5);
        FBTMMCPY(texPtr, (unsigned char *)arg6, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexSubImage1DEXT;
}
void PT_CALL glTexSubImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    if (pixUnpackBuf == 0) {
        uint32_t szTex, *texPtr;
        szTex = ((szUnpackWidth == 0)? arg4:szUnpackWidth) * arg5 * szgldata(arg6, arg7);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        szTex -= ((szUnpackWidth)? arg2:0) * szgldata(arg6, arg7);
        //DPRINTF("TexSubImage2D() %x,%x,%x,%x,%x,%x,%x,%x size %07x", arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, szTex);
        FBTMMCPY(texPtr, (unsigned char *)arg8, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexSubImage2D;
}
void PT_CALL glTexSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    if (pixUnpackBuf == 0) {
        uint32_t szTex, *texPtr;
        szTex = ((szUnpackWidth == 0)? arg4:szUnpackWidth) * arg5 * szgldata(arg6, arg7);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        szTex -= ((szUnpackWidth)? arg2:0) * szgldata(arg6, arg7);
        //DPRINTF("TexSubImage2D() %x,%x,%x,%x,%x,%x,%x,%x size %07x", arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, szTex);
        FBTMMCPY(texPtr, (unsigned char *)arg8, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexSubImage2DEXT;
}
void PT_CALL glTexSubImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    if (pixUnpackBuf == 0) {
        uint32_t szTex, *texPtr;
        szTex = ((szUnpackWidth == 0)? arg5:szUnpackWidth) * ((szUnpackHeight == 0)? arg6:szUnpackHeight) * arg7 * szgldata(arg8, arg9);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        szTex -= ((szUnpackWidth)? arg2:0) * szgldata(arg8, arg9);
        FBTMMCPY(texPtr, (unsigned char *)arg10, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexSubImage3D;
}
void PT_CALL glTexSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    if (pixUnpackBuf == 0) {
        uint32_t szTex, *texPtr;
        szTex = ((szUnpackWidth == 0)? arg5:szUnpackWidth) * ((szUnpackHeight == 0)? arg6:szUnpackHeight) * arg7 * szgldata(arg8, arg9);
        texPtr = &fbtm[(MGLFBT_SIZE - ALIGNED(szTex)) >> 2];
        szTex -= ((szUnpackWidth)? arg2:0) * szgldata(arg8, arg9);
        FBTMMCPY(texPtr, (unsigned char *)arg10, szTex);
    }
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexSubImage3DEXT;
}
void PT_CALL glTexSubImage4DSGIS(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; pt[13] = arg12; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexSubImage4DSGIS;
}
void PT_CALL glTextureAttachMemoryNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureAttachMemoryNV;
}
void PT_CALL glTextureBarrier(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureBarrier;
}
void PT_CALL glTextureBarrierNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureBarrierNV;
}
void PT_CALL glTextureBuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTextureBuffer, 3);
}
void PT_CALL glTextureBufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTextureBufferEXT, 3);
}
void PT_CALL glTextureBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureBufferRange;
}
void PT_CALL glTextureBufferRangeEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureBufferRangeEXT;
}
void PT_CALL glTextureColorMaskSGIS(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureColorMaskSGIS;
}
void PT_CALL glTextureImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureImage1DEXT;
}
void PT_CALL glTextureImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureImage2DEXT;
}
void PT_CALL glTextureImage2DMultisampleCoverageNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureImage2DMultisampleCoverageNV;
}
void PT_CALL glTextureImage2DMultisampleNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureImage2DMultisampleNV;
}
void PT_CALL glTextureImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureImage3DEXT;
}
void PT_CALL glTextureImage3DMultisampleCoverageNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureImage3DMultisampleCoverageNV;
}
void PT_CALL glTextureImage3DMultisampleNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureImage3DMultisampleNV;
}
void PT_CALL glTextureLightEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureLightEXT;
}
void PT_CALL glTextureMaterialEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureMaterialEXT;
}
void PT_CALL glTextureNormalEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureNormalEXT;
}
void PT_CALL glTexturePageCommitmentEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTexturePageCommitmentEXT;
}
void PT_CALL glTextureParameterIiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterIiv;
}
void PT_CALL glTextureParameterIivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterIivEXT;
}
void PT_CALL glTextureParameterIuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterIuiv;
}
void PT_CALL glTextureParameterIuivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterIuivEXT;
}
void PT_CALL glTextureParameterf(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterf;
}
void PT_CALL glTextureParameterfEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterfEXT;
}
void PT_CALL glTextureParameterfv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterfv;
}
void PT_CALL glTextureParameterfvEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterfvEXT;
}
void PT_CALL glTextureParameteri(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameteri;
}
void PT_CALL glTextureParameteriEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameteriEXT;
}
void PT_CALL glTextureParameteriv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameteriv;
}
void PT_CALL glTextureParameterivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureParameterivEXT;
}
void PT_CALL glTextureRangeAPPLE(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureRangeAPPLE;
}
void PT_CALL glTextureRenderbufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureRenderbufferEXT;
}
void PT_CALL glTextureStorage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage1D;
}
void PT_CALL glTextureStorage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage1DEXT;
}
void PT_CALL glTextureStorage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage2D;
}
void PT_CALL glTextureStorage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage2DEXT;
}
void PT_CALL glTextureStorage2DMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage2DMultisample;
}
void PT_CALL glTextureStorage2DMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage2DMultisampleEXT;
}
void PT_CALL glTextureStorage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage3D;
}
void PT_CALL glTextureStorage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage3DEXT;
}
void PT_CALL glTextureStorage3DMultisample(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage3DMultisample;
}
void PT_CALL glTextureStorage3DMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorage3DMultisampleEXT;
}
void PT_CALL glTextureStorageMem1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorageMem1DEXT;
}
void PT_CALL glTextureStorageMem2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorageMem2DEXT;
}
void PT_CALL glTextureStorageMem2DMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorageMem2DMultisampleEXT;
}
void PT_CALL glTextureStorageMem3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorageMem3DEXT;
}
void PT_CALL glTextureStorageMem3DMultisampleEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorageMem3DMultisampleEXT;
}
void PT_CALL glTextureStorageSparseAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureStorageSparseAMD;
}
void PT_CALL glTextureSubImage1D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureSubImage1D;
}
void PT_CALL glTextureSubImage1DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureSubImage1DEXT;
}
void PT_CALL glTextureSubImage2D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureSubImage2D;
}
void PT_CALL glTextureSubImage2DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureSubImage2DEXT;
}
void PT_CALL glTextureSubImage3D(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureSubImage3D;
}
void PT_CALL glTextureSubImage3DEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; pt[10] = arg9; pt[11] = arg10; pt[12] = arg11; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTextureSubImage3DEXT;
}
void PT_CALL glTextureView(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTextureView, 8);
}
void PT_CALL glTrackMatrixNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTrackMatrixNV, 4);
}
void PT_CALL glTransformFeedbackAttribsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTransformFeedbackAttribsNV;
}
void PT_CALL glTransformFeedbackBufferBase(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTransformFeedbackBufferBase;
}
void PT_CALL glTransformFeedbackBufferRange(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTransformFeedbackBufferRange;
}
void PT_CALL glTransformFeedbackStreamAttribsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTransformFeedbackStreamAttribsNV;
}
void PT_CALL glTransformFeedbackVaryings(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t i;
    char **varys = (char **)arg2;
    for (i = 0; i < arg1; i++)
        fifoAddData(0, (uint32_t)varys[i], ALIGNED((strlen(varys[i] + 1))));
    fifoAddData(0, arg2, ALIGNED((arg1 * sizeof(uint32_t))));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTransformFeedbackVaryings, 4);
}
void PT_CALL glTransformFeedbackVaryingsEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    uint32_t i;
    char **varys = (char **)arg2;
    for (i = 0; i < arg1; i++)
        fifoAddData(0, (uint32_t)varys[i], ALIGNED((strlen(varys[i] + 1))));
    fifoAddData(0, arg2, ALIGNED((arg1 * sizeof(uint32_t))));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTransformFeedbackVaryingsEXT, 4);
}
void PT_CALL glTransformFeedbackVaryingsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTransformFeedbackVaryingsNV;
}
void PT_CALL glTransformPathNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTransformPathNV;
}
void PT_CALL glTranslated(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTranslated, 6);
}
void PT_CALL glTranslatef(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glTranslatef, 3);
}
void PT_CALL glTranslatexOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glTranslatexOES;
}
void PT_CALL glUniform1d(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1d, 3);
}
void PT_CALL glUniform1dv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1dv, 3);
}
void PT_CALL glUniform1f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1f, 2);
}
void PT_CALL glUniform1fARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1fARB, 2);
}
void PT_CALL glUniform1fv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1fv, 3);
}
void PT_CALL glUniform1fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1fvARB, 3);
}
void PT_CALL glUniform1i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1i64ARB;
}
void PT_CALL glUniform1i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1i64NV;
}
void PT_CALL glUniform1i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1i64vARB;
}
void PT_CALL glUniform1i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1i64vNV;
}
void PT_CALL glUniform1i(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1i, 2);
}
void PT_CALL glUniform1iARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1iARB, 2);
}
void PT_CALL glUniform1iv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1iv, 3);
}
void PT_CALL glUniform1ivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1ivARB, 3);
}
void PT_CALL glUniform1ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1ui64ARB;
}
void PT_CALL glUniform1ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1ui64NV;
}
void PT_CALL glUniform1ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1ui64vARB;
}
void PT_CALL glUniform1ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform1ui64vNV;
}
void PT_CALL glUniform1ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1ui, 2);
}
void PT_CALL glUniform1uiEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1uiEXT, 2);
}
void PT_CALL glUniform1uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(unsigned int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1uiv, 3);
}
void PT_CALL glUniform1uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(arg1*sizeof(unsigned int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform1uivEXT, 3);
}
void PT_CALL glUniform2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2d, 5);
}
void PT_CALL glUniform2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2dv, 3);
}
void PT_CALL glUniform2f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2f, 3);
}
void PT_CALL glUniform2fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2fARB, 3);
}
void PT_CALL glUniform2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2fv, 3);
}
void PT_CALL glUniform2fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2fvARB, 3);
}
void PT_CALL glUniform2i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2i64ARB;
}
void PT_CALL glUniform2i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2i64NV;
}
void PT_CALL glUniform2i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2i64vARB;
}
void PT_CALL glUniform2i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2i64vNV;
}
void PT_CALL glUniform2i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2i, 3);
}
void PT_CALL glUniform2iARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2iARB, 3);
}
void PT_CALL glUniform2iv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2iv, 3);
}
void PT_CALL glUniform2ivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2ivARB, 3);
}
void PT_CALL glUniform2ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2ui64ARB;
}
void PT_CALL glUniform2ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2ui64NV;
}
void PT_CALL glUniform2ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2ui64vARB;
}
void PT_CALL glUniform2ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform2ui64vNV;
}
void PT_CALL glUniform2ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2ui, 3);
}
void PT_CALL glUniform2uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2uiEXT, 3);
}
void PT_CALL glUniform2uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2uiv, 3);
}
void PT_CALL glUniform2uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform2uivEXT, 3);
}
void PT_CALL glUniform3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3d, 7);
}
void PT_CALL glUniform3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 3*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3dv, 3);
}
void PT_CALL glUniform3f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3f, 4);
}
void PT_CALL glUniform3fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3fARB, 4);
}
void PT_CALL glUniform3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(3*arg1*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3fv, 3);
}
void PT_CALL glUniform3fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(3*arg1*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3fvARB, 3);
}
void PT_CALL glUniform3i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3i64ARB;
}
void PT_CALL glUniform3i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3i64NV;
}
void PT_CALL glUniform3i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3i64vARB;
}
void PT_CALL glUniform3i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3i64vNV;
}
void PT_CALL glUniform3i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3i, 4);
}
void PT_CALL glUniform3iARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3iARB, 4);
}
void PT_CALL glUniform3iv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(3*arg1*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3iv, 3);
}
void PT_CALL glUniform3ivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(3*arg1*sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3ivARB, 3);
}
void PT_CALL glUniform3ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3ui64ARB;
}
void PT_CALL glUniform3ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3ui64NV;
}
void PT_CALL glUniform3ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3ui64vARB;
}
void PT_CALL glUniform3ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform3ui64vNV;
}
void PT_CALL glUniform3ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3ui, 4);
}
void PT_CALL glUniform3uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3uiEXT, 4);
}
void PT_CALL glUniform3uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(3*arg1*sizeof(unsigned int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3uiv, 3);
}
void PT_CALL glUniform3uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, ALIGNED(3*arg1*sizeof(unsigned int)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform3uivEXT, 3);
}
void PT_CALL glUniform4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4d, 9);
}
void PT_CALL glUniform4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4dv, 3);
}
void PT_CALL glUniform4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4f, 5);
}
void PT_CALL glUniform4fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4fARB, 5);
}
void PT_CALL glUniform4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4fv, 3);
}
void PT_CALL glUniform4fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4fvARB, 3);
}
void PT_CALL glUniform4i64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4i64ARB;
}
void PT_CALL glUniform4i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4i64NV;
}
void PT_CALL glUniform4i64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4i64vARB;
}
void PT_CALL glUniform4i64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4i64vNV;
}
void PT_CALL glUniform4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4i, 5);
}
void PT_CALL glUniform4iARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4iARB, 5);
}
void PT_CALL glUniform4iv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4iv, 3);
}
void PT_CALL glUniform4ivARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4ivARB, 3);
}
void PT_CALL glUniform4ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4ui64ARB;
}
void PT_CALL glUniform4ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4ui64NV;
}
void PT_CALL glUniform4ui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4ui64vARB;
}
void PT_CALL glUniform4ui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniform4ui64vNV;
}
void PT_CALL glUniform4ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4ui, 5);
}
void PT_CALL glUniform4uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4uiEXT, 5);
}
void PT_CALL glUniform4uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4uiv, 3);
}
void PT_CALL glUniform4uivEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniform4uivEXT, 3);
}
void PT_CALL glUniformBlockBinding(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformBlockBinding, 3);
}
void PT_CALL glUniformBufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformBufferEXT, 3);
}
void PT_CALL glUniformHandleui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniformHandleui64ARB;
}
void PT_CALL glUniformHandleui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniformHandleui64NV;
}
void PT_CALL glUniformHandleui64vARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniformHandleui64vARB;
}
void PT_CALL glUniformHandleui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniformHandleui64vNV;
}
void PT_CALL glUniformMatrix2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 4*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix2dv, 4);
}
void PT_CALL glUniformMatrix2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 4*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix2fv, 4);
}
void PT_CALL glUniformMatrix2fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 4*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix2fvARB, 4);
}
void PT_CALL glUniformMatrix2x3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 6*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix2x3dv, 4);
}
void PT_CALL glUniformMatrix2x3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 6*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix2x3fv, 4);
}
void PT_CALL glUniformMatrix2x4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 8*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix2x4dv, 4);
}
void PT_CALL glUniformMatrix2x4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 8*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix2x4fv, 4);
}
void PT_CALL glUniformMatrix3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 9*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix3dv, 4);
}
void PT_CALL glUniformMatrix3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, ALIGNED(9*arg1*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix3fv, 4);
}
void PT_CALL glUniformMatrix3fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, ALIGNED(9*arg1*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix3fvARB, 4);
}
void PT_CALL glUniformMatrix3x2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 6*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix3x2dv, 4);
}
void PT_CALL glUniformMatrix3x2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 6*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix3x2fv, 4);
}
void PT_CALL glUniformMatrix3x4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 12*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix3x4dv, 4);
}
void PT_CALL glUniformMatrix3x4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 12*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix3x4fv, 4);
}
void PT_CALL glUniformMatrix4dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 16*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix4dv, 4);
}
void PT_CALL glUniformMatrix4fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 16*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix4fv, 4);
}
void PT_CALL glUniformMatrix4fvARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 16*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix4fvARB, 4);
}
void PT_CALL glUniformMatrix4x2dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 8*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix4x2dv, 4);
}
void PT_CALL glUniformMatrix4x2fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 8*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix4x2fv, 4);
}
void PT_CALL glUniformMatrix4x3dv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 12*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix4x3dv, 4);
}
void PT_CALL glUniformMatrix4x3fv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    fifoAddData(0, arg3, 12*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUniformMatrix4x3fv, 4);
}
void PT_CALL glUniformSubroutinesuiv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniformSubroutinesuiv;
}
void PT_CALL glUniformui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniformui64NV;
}
void PT_CALL glUniformui64vNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUniformui64vNV;
}
void PT_CALL glUnlockArraysEXT(void) {
    
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUnlockArraysEXT, 0);
}
uint32_t PT_CALL glUnmapBuffer(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUnmapBuffer;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glUnmapBufferARB(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUnmapBufferARB;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glUnmapNamedBuffer(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUnmapNamedBuffer;
    ret = *pt0;
    return ret;
}
uint32_t PT_CALL glUnmapNamedBufferEXT(uint32_t arg0) {
    uint32_t ret;
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUnmapNamedBufferEXT;
    ret = *pt0;
    return ret;
}
void PT_CALL glUnmapObjectBufferATI(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUnmapObjectBufferATI;
}
void PT_CALL glUnmapTexture2DINTEL(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUnmapTexture2DINTEL;
}
void PT_CALL glUpdateObjectBufferATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUpdateObjectBufferATI;
}
void PT_CALL glUseProgram(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUseProgram, 1);
}
void PT_CALL glUseProgramObjectARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glUseProgramObjectARB, 1);
}
void PT_CALL glUseProgramStages(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUseProgramStages;
}
void PT_CALL glUseShaderProgramEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glUseShaderProgramEXT;
}
void PT_CALL glVDPAUFiniNV(void) {
    
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUFiniNV;
}
void PT_CALL glVDPAUGetSurfaceivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUGetSurfaceivNV;
}
void PT_CALL glVDPAUInitNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUInitNV;
}
void PT_CALL glVDPAUIsSurfaceNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUIsSurfaceNV;
}
void PT_CALL glVDPAUMapSurfacesNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUMapSurfacesNV;
}
void PT_CALL glVDPAURegisterOutputSurfaceNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAURegisterOutputSurfaceNV;
}
void PT_CALL glVDPAURegisterVideoSurfaceNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAURegisterVideoSurfaceNV;
}
void PT_CALL glVDPAURegisterVideoSurfaceWithPictureStructureNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAURegisterVideoSurfaceWithPictureStructureNV;
}
void PT_CALL glVDPAUSurfaceAccessNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUSurfaceAccessNV;
}
void PT_CALL glVDPAUUnmapSurfacesNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUUnmapSurfacesNV;
}
void PT_CALL glVDPAUUnregisterSurfaceNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVDPAUUnregisterSurfaceNV;
}
void PT_CALL glValidateProgram(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glValidateProgram, 1);
}
void PT_CALL glValidateProgramARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glValidateProgramARB, 1);
}
void PT_CALL glValidateProgramPipeline(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glValidateProgramPipeline;
}
void PT_CALL glVariantArrayObjectATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantArrayObjectATI;
}
void PT_CALL glVariantPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantPointerEXT;
}
void PT_CALL glVariantbvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantbvEXT;
}
void PT_CALL glVariantdvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantdvEXT;
}
void PT_CALL glVariantfvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantfvEXT;
}
void PT_CALL glVariantivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantivEXT;
}
void PT_CALL glVariantsvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantsvEXT;
}
void PT_CALL glVariantubvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantubvEXT;
}
void PT_CALL glVariantuivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantuivEXT;
}
void PT_CALL glVariantusvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVariantusvEXT;
}
void PT_CALL glVertex2bOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex2bOES;
}
void PT_CALL glVertex2bvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex2bvOES;
}
void PT_CALL glVertex2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2d, 4);
}
void PT_CALL glVertex2dv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2dv, 1);
}
void PT_CALL glVertex2f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2f, 2);
}
void PT_CALL glVertex2fv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2fv, 1);
}
void PT_CALL glVertex2hNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex2hNV;
}
void PT_CALL glVertex2hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex2hvNV;
}
void PT_CALL glVertex2i(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2i, 2);
}
void PT_CALL glVertex2iv(uint32_t arg0) {
    fifoAddData(0, arg0, 2*sizeof(int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2iv, 1);
}
void PT_CALL glVertex2s(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2s, 2);
}
void PT_CALL glVertex2sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex2sv, 1);
}
void PT_CALL glVertex2xOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex2xOES;
}
void PT_CALL glVertex2xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex2xvOES;
}
void PT_CALL glVertex3bOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex3bOES;
}
void PT_CALL glVertex3bvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex3bvOES;
}
void PT_CALL glVertex3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3d, 6);
}
void PT_CALL glVertex3dv(uint32_t arg0) {
    fifoAddData(0, arg0, 3*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3dv, 1);
}
void PT_CALL glVertex3f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3f, 3);
}
void PT_CALL glVertex3fv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3fv, 1);
}
void PT_CALL glVertex3hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex3hNV;
}
void PT_CALL glVertex3hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex3hvNV;
}
void PT_CALL glVertex3i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3i, 3);
}
void PT_CALL glVertex3iv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(int)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3iv, 1);
}
void PT_CALL glVertex3s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3s, 3);
}
void PT_CALL glVertex3sv(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex3sv, 1);
}
void PT_CALL glVertex3xOES(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex3xOES;
}
void PT_CALL glVertex3xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex3xvOES;
}
void PT_CALL glVertex4bOES(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex4bOES;
}
void PT_CALL glVertex4bvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex4bvOES;
}
void PT_CALL glVertex4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4d, 8);
}
void PT_CALL glVertex4dv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(double));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4dv, 1);
}
void PT_CALL glVertex4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4f, 4);
}
void PT_CALL glVertex4fv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(float));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4fv, 1);
}
void PT_CALL glVertex4hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex4hNV;
}
void PT_CALL glVertex4hvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex4hvNV;
}
void PT_CALL glVertex4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4i, 4);
}
void PT_CALL glVertex4iv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(int));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4iv, 1);
}
void PT_CALL glVertex4s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4s, 4);
}
void PT_CALL glVertex4sv(uint32_t arg0) {
    fifoAddData(0, arg0, 4*sizeof(short));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertex4sv, 1);
}
void PT_CALL glVertex4xOES(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex4xOES;
}
void PT_CALL glVertex4xvOES(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertex4xvOES;
}
void PT_CALL glVertexArrayAttribBinding(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayAttribBinding;
}
void PT_CALL glVertexArrayAttribFormat(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayAttribFormat;
}
void PT_CALL glVertexArrayAttribIFormat(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayAttribIFormat;
}
void PT_CALL glVertexArrayAttribLFormat(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayAttribLFormat;
}
void PT_CALL glVertexArrayBindVertexBufferEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayBindVertexBufferEXT;
}
void PT_CALL glVertexArrayBindingDivisor(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayBindingDivisor;
}
void PT_CALL glVertexArrayColorOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayColorOffsetEXT;
}
void PT_CALL glVertexArrayEdgeFlagOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayEdgeFlagOffsetEXT;
}
void PT_CALL glVertexArrayElementBuffer(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayElementBuffer;
}
void PT_CALL glVertexArrayFogCoordOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayFogCoordOffsetEXT;
}
void PT_CALL glVertexArrayIndexOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayIndexOffsetEXT;
}
void PT_CALL glVertexArrayMultiTexCoordOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayMultiTexCoordOffsetEXT;
}
void PT_CALL glVertexArrayNormalOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayNormalOffsetEXT;
}
void PT_CALL glVertexArrayParameteriAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayParameteriAPPLE;
}
void PT_CALL glVertexArrayRangeAPPLE(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayRangeAPPLE;
}
void PT_CALL glVertexArrayRangeNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayRangeNV;
}
void PT_CALL glVertexArraySecondaryColorOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArraySecondaryColorOffsetEXT;
}
void PT_CALL glVertexArrayTexCoordOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayTexCoordOffsetEXT;
}
void PT_CALL glVertexArrayVertexAttribBindingEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribBindingEXT;
}
void PT_CALL glVertexArrayVertexAttribDivisorEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribDivisorEXT;
}
void PT_CALL glVertexArrayVertexAttribFormatEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribFormatEXT;
}
void PT_CALL glVertexArrayVertexAttribIFormatEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribIFormatEXT;
}
void PT_CALL glVertexArrayVertexAttribIOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribIOffsetEXT;
}
void PT_CALL glVertexArrayVertexAttribLFormatEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribLFormatEXT;
}
void PT_CALL glVertexArrayVertexAttribLOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribLOffsetEXT;
}
void PT_CALL glVertexArrayVertexAttribOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexAttribOffsetEXT;
}
void PT_CALL glVertexArrayVertexBindingDivisorEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexBindingDivisorEXT;
}
void PT_CALL glVertexArrayVertexBuffer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexBuffer;
}
void PT_CALL glVertexArrayVertexBuffers(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexBuffers;
}
void PT_CALL glVertexArrayVertexOffsetEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexArrayVertexOffsetEXT;
}
void PT_CALL glVertexAttrib1d(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1d, 3);
}
void PT_CALL glVertexAttrib1dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1dARB, 3);
}
void PT_CALL glVertexAttrib1dNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1dNV, 3);
}
void PT_CALL glVertexAttrib1dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1dv, 2);
}
void PT_CALL glVertexAttrib1dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1dvARB, 2);
}
void PT_CALL glVertexAttrib1dvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1dvNV, 2);
}
void PT_CALL glVertexAttrib1f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1f, 2);
}
void PT_CALL glVertexAttrib1fARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1fARB, 2);
}
void PT_CALL glVertexAttrib1fNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1fNV, 2);
}
void PT_CALL glVertexAttrib1fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1fv, 2);
}
void PT_CALL glVertexAttrib1fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1fvARB, 2);
}
void PT_CALL glVertexAttrib1fvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1fvNV, 2);
}
void PT_CALL glVertexAttrib1hNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib1hNV;
}
void PT_CALL glVertexAttrib1hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib1hvNV;
}
void PT_CALL glVertexAttrib1s(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1s, 2);
}
void PT_CALL glVertexAttrib1sARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1sARB, 2);
}
void PT_CALL glVertexAttrib1sNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1sNV, 2);
}
void PT_CALL glVertexAttrib1sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1sv, 2);
}
void PT_CALL glVertexAttrib1svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1svARB, 2);
}
void PT_CALL glVertexAttrib1svNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib1svNV, 2);
}
void PT_CALL glVertexAttrib2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2d, 5);
}
void PT_CALL glVertexAttrib2dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2dARB, 5);
}
void PT_CALL glVertexAttrib2dNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2dNV, 5);
}
void PT_CALL glVertexAttrib2dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2dv, 2);
}
void PT_CALL glVertexAttrib2dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2dvARB, 2);
}
void PT_CALL glVertexAttrib2dvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2dvNV, 2);
}
void PT_CALL glVertexAttrib2f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2f, 3);
}
void PT_CALL glVertexAttrib2fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2fARB, 3);
}
void PT_CALL glVertexAttrib2fNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2fNV, 3);
}
void PT_CALL glVertexAttrib2fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2fv, 2);
}
void PT_CALL glVertexAttrib2fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2fvARB, 2);
}
void PT_CALL glVertexAttrib2fvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 2*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2fvNV, 2);
}
void PT_CALL glVertexAttrib2hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib2hNV;
}
void PT_CALL glVertexAttrib2hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib2hvNV;
}
void PT_CALL glVertexAttrib2s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2s, 3);
}
void PT_CALL glVertexAttrib2sARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2sARB, 3);
}
void PT_CALL glVertexAttrib2sNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2sNV, 3);
}
void PT_CALL glVertexAttrib2sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2sv, 2);
}
void PT_CALL glVertexAttrib2svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2svARB, 2);
}
void PT_CALL glVertexAttrib2svNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(2*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib2svNV, 2);
}
void PT_CALL glVertexAttrib3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3d, 7);
}
void PT_CALL glVertexAttrib3dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3dARB, 7);
}
void PT_CALL glVertexAttrib3dNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3dNV, 7);
}
void PT_CALL glVertexAttrib3dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 3*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3dv, 2);
}
void PT_CALL glVertexAttrib3dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 3*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3dvARB, 2);
}
void PT_CALL glVertexAttrib3dvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 3*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3dvNV, 2);
}
void PT_CALL glVertexAttrib3f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3f, 4);
}
void PT_CALL glVertexAttrib3fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3fARB, 4);
}
void PT_CALL glVertexAttrib3fNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3fNV, 4);
}
void PT_CALL glVertexAttrib3fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3fv, 2);
}
void PT_CALL glVertexAttrib3fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3fvARB, 2);
}
void PT_CALL glVertexAttrib3fvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3fvNV, 2);
}
void PT_CALL glVertexAttrib3hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib3hNV;
}
void PT_CALL glVertexAttrib3hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib3hvNV;
}
void PT_CALL glVertexAttrib3s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3s, 4);
}
void PT_CALL glVertexAttrib3sARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3sARB, 4);
}
void PT_CALL glVertexAttrib3sNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3sNV, 4);
}
void PT_CALL glVertexAttrib3sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3sv, 2);
}
void PT_CALL glVertexAttrib3svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3svARB, 2);
}
void PT_CALL glVertexAttrib3svNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(3*sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib3svNV, 2);
}
void PT_CALL glVertexAttrib4Nbv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4Nbv, 2);
}
void PT_CALL glVertexAttrib4NbvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4NbvARB, 2);
}
void PT_CALL glVertexAttrib4Niv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4Niv, 2);
}
void PT_CALL glVertexAttrib4NivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4NivARB, 2);
}
void PT_CALL glVertexAttrib4Nsv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4Nsv, 2);
}
void PT_CALL glVertexAttrib4NsvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4NsvARB, 2);
}
void PT_CALL glVertexAttrib4Nub(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4Nub, 5);
}
void PT_CALL glVertexAttrib4NubARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4NubARB, 5);
}
void PT_CALL glVertexAttrib4Nubv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(unsigned char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4Nubv, 2);
}
void PT_CALL glVertexAttrib4NubvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(unsigned char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4NubvARB, 2);
}
void PT_CALL glVertexAttrib4Nuiv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4Nuiv, 2);
}
void PT_CALL glVertexAttrib4NuivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4NuivARB, 2);
}
void PT_CALL glVertexAttrib4Nusv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4Nusv, 2);
}
void PT_CALL glVertexAttrib4NusvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4NusvARB, 2);
}
void PT_CALL glVertexAttrib4bv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4bv, 2);
}
void PT_CALL glVertexAttrib4bvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4bvARB, 2);
}
void PT_CALL glVertexAttrib4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4d, 9);
}
void PT_CALL glVertexAttrib4dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4dARB, 9);
}
void PT_CALL glVertexAttrib4dNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4dNV, 9);
}
void PT_CALL glVertexAttrib4dv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4dv, 2);
}
void PT_CALL glVertexAttrib4dvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4dvARB, 2);
}
void PT_CALL glVertexAttrib4dvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4dvNV, 2);
}
void PT_CALL glVertexAttrib4f(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4f, 5);
}
void PT_CALL glVertexAttrib4fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4fARB, 5);
}
void PT_CALL glVertexAttrib4fNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4fNV, 5);
}
void PT_CALL glVertexAttrib4fv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4fv, 2);
}
void PT_CALL glVertexAttrib4fvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4fvARB, 2);
}
void PT_CALL glVertexAttrib4fvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4fvNV, 2);
}
void PT_CALL glVertexAttrib4hNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib4hNV;
}
void PT_CALL glVertexAttrib4hvNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttrib4hvNV;
}
void PT_CALL glVertexAttrib4iv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4iv, 2);
}
void PT_CALL glVertexAttrib4ivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4ivARB, 2);
}
void PT_CALL glVertexAttrib4s(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4s, 5);
}
void PT_CALL glVertexAttrib4sARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4sARB, 5);
}
void PT_CALL glVertexAttrib4sNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4sNV, 5);
}
void PT_CALL glVertexAttrib4sv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4sv, 2);
}
void PT_CALL glVertexAttrib4svARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4svARB, 2);
}
void PT_CALL glVertexAttrib4svNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4svNV, 2);
}
void PT_CALL glVertexAttrib4ubNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4ubNV, 5);
}
void PT_CALL glVertexAttrib4ubv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(unsigned char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4ubv, 2);
}
void PT_CALL glVertexAttrib4ubvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(unsigned char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4ubvARB, 2);
}
void PT_CALL glVertexAttrib4ubvNV(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(4*sizeof(unsigned char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4ubvNV, 2);
}
void PT_CALL glVertexAttrib4uiv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4uiv, 2);
}
void PT_CALL glVertexAttrib4uivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned int));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4uivARB, 2);
}
void PT_CALL glVertexAttrib4usv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4usv, 2);
}
void PT_CALL glVertexAttrib4usvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(unsigned short));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttrib4usvARB, 2);
}
void PT_CALL glVertexAttribArrayObjectATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribArrayObjectATI;
}
void PT_CALL glVertexAttribBinding(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribBinding;
}
void PT_CALL glVertexAttribDivisor(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribDivisor, 2);
}
void PT_CALL glVertexAttribDivisorARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribDivisorARB, 2);
}
void PT_CALL glVertexAttribFormat(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribFormat;
}
void PT_CALL glVertexAttribFormatNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribFormatNV;
}
void PT_CALL glVertexAttribI1i(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1i;
}
void PT_CALL glVertexAttribI1iEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1iEXT;
}
void PT_CALL glVertexAttribI1iv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1iv;
}
void PT_CALL glVertexAttribI1ivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1ivEXT;
}
void PT_CALL glVertexAttribI1ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1ui;
}
void PT_CALL glVertexAttribI1uiEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1uiEXT;
}
void PT_CALL glVertexAttribI1uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1uiv;
}
void PT_CALL glVertexAttribI1uivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI1uivEXT;
}
void PT_CALL glVertexAttribI2i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2i;
}
void PT_CALL glVertexAttribI2iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2iEXT;
}
void PT_CALL glVertexAttribI2iv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2iv;
}
void PT_CALL glVertexAttribI2ivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2ivEXT;
}
void PT_CALL glVertexAttribI2ui(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2ui;
}
void PT_CALL glVertexAttribI2uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2uiEXT;
}
void PT_CALL glVertexAttribI2uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2uiv;
}
void PT_CALL glVertexAttribI2uivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI2uivEXT;
}
void PT_CALL glVertexAttribI3i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3i;
}
void PT_CALL glVertexAttribI3iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3iEXT;
}
void PT_CALL glVertexAttribI3iv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3iv;
}
void PT_CALL glVertexAttribI3ivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3ivEXT;
}
void PT_CALL glVertexAttribI3ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3ui;
}
void PT_CALL glVertexAttribI3uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3uiEXT;
}
void PT_CALL glVertexAttribI3uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3uiv;
}
void PT_CALL glVertexAttribI3uivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI3uivEXT;
}
void PT_CALL glVertexAttribI4bv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4bv;
}
void PT_CALL glVertexAttribI4bvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4bvEXT;
}
void PT_CALL glVertexAttribI4i(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4i;
}
void PT_CALL glVertexAttribI4iEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4iEXT;
}
void PT_CALL glVertexAttribI4iv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4iv;
}
void PT_CALL glVertexAttribI4ivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4ivEXT;
}
void PT_CALL glVertexAttribI4sv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4sv;
}
void PT_CALL glVertexAttribI4svEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4svEXT;
}
void PT_CALL glVertexAttribI4ubv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4ubv;
}
void PT_CALL glVertexAttribI4ubvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4ubvEXT;
}
void PT_CALL glVertexAttribI4ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4ui;
}
void PT_CALL glVertexAttribI4uiEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4uiEXT;
}
void PT_CALL glVertexAttribI4uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4uiv;
}
void PT_CALL glVertexAttribI4uivEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4uivEXT;
}
void PT_CALL glVertexAttribI4usv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4usv;
}
void PT_CALL glVertexAttribI4usvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribI4usvEXT;
}
void PT_CALL glVertexAttribIFormat(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribIFormat;
}
void PT_CALL glVertexAttribIFormatNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribIFormatNV;
}
void PT_CALL glVertexAttribIPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribIPointer, 5);
    vtxarry_init(vattr2arry(arg0), arg1, arg2, arg3, (void *)arg4);
}
void PT_CALL glVertexAttribIPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribIPointerEXT, 5);
    vtxarry_init(vattr2arry(arg0), arg1, arg2, arg3, (void *)arg4);
}
void PT_CALL glVertexAttribL1d(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1d;
}
void PT_CALL glVertexAttribL1dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1dEXT;
}
void PT_CALL glVertexAttribL1dv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1dv;
}
void PT_CALL glVertexAttribL1dvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1dvEXT;
}
void PT_CALL glVertexAttribL1i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1i64NV;
}
void PT_CALL glVertexAttribL1i64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1i64vNV;
}
void PT_CALL glVertexAttribL1ui64ARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1ui64ARB;
}
void PT_CALL glVertexAttribL1ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1ui64NV;
}
void PT_CALL glVertexAttribL1ui64vARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1ui64vARB;
}
void PT_CALL glVertexAttribL1ui64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL1ui64vNV;
}
void PT_CALL glVertexAttribL2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2d;
}
void PT_CALL glVertexAttribL2dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2dEXT;
}
void PT_CALL glVertexAttribL2dv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2dv;
}
void PT_CALL glVertexAttribL2dvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2dvEXT;
}
void PT_CALL glVertexAttribL2i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2i64NV;
}
void PT_CALL glVertexAttribL2i64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2i64vNV;
}
void PT_CALL glVertexAttribL2ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2ui64NV;
}
void PT_CALL glVertexAttribL2ui64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL2ui64vNV;
}
void PT_CALL glVertexAttribL3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3d;
}
void PT_CALL glVertexAttribL3dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3dEXT;
}
void PT_CALL glVertexAttribL3dv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3dv;
}
void PT_CALL glVertexAttribL3dvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3dvEXT;
}
void PT_CALL glVertexAttribL3i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3i64NV;
}
void PT_CALL glVertexAttribL3i64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3i64vNV;
}
void PT_CALL glVertexAttribL3ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3ui64NV;
}
void PT_CALL glVertexAttribL3ui64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL3ui64vNV;
}
void PT_CALL glVertexAttribL4d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4d;
}
void PT_CALL glVertexAttribL4dEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4dEXT;
}
void PT_CALL glVertexAttribL4dv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4dv;
}
void PT_CALL glVertexAttribL4dvEXT(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4dvEXT;
}
void PT_CALL glVertexAttribL4i64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4i64NV;
}
void PT_CALL glVertexAttribL4i64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4i64vNV;
}
void PT_CALL glVertexAttribL4ui64NV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4ui64NV;
}
void PT_CALL glVertexAttribL4ui64vNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribL4ui64vNV;
}
void PT_CALL glVertexAttribLFormat(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribLFormat;
}
void PT_CALL glVertexAttribLFormatNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribLFormatNV;
}
void PT_CALL glVertexAttribLPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribLPointer, 5);
    vtxarry_init(vattr2arry(arg0), arg1, arg2, arg3, (void *)arg4);
}
void PT_CALL glVertexAttribLPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribLPointerEXT, 5);
    vtxarry_init(vattr2arry(arg0), arg1, arg2, arg3, (void *)arg4);
}
void PT_CALL glVertexAttribP1ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP1ui;
}
void PT_CALL glVertexAttribP1uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP1uiv;
}
void PT_CALL glVertexAttribP2ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP2ui;
}
void PT_CALL glVertexAttribP2uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP2uiv;
}
void PT_CALL glVertexAttribP3ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP3ui;
}
void PT_CALL glVertexAttribP3uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP3uiv;
}
void PT_CALL glVertexAttribP4ui(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP4ui;
}
void PT_CALL glVertexAttribP4uiv(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribP4uiv;
}
void PT_CALL glVertexAttribParameteriAMD(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribParameteriAMD;
}
void PT_CALL glVertexAttribPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribPointer, 6);
    vtxarry_init(vattr2arry(arg0), arg1, arg2, arg4, (void *)arg5);
}
void PT_CALL glVertexAttribPointerARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribPointerARB, 6);
    vtxarry_init(vattr2arry(arg0), arg1, arg2, arg4, (void *)arg5);
}
void PT_CALL glVertexAttribPointerNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribPointerNV, 5);
    vtxarry_init(vattr2arry(arg0), arg1, arg2, arg3, (void *)arg4);
}
void PT_CALL glVertexAttribs1dvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs1dvNV, 3);
}
void PT_CALL glVertexAttribs1fvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs1fvNV, 3);
}
void PT_CALL glVertexAttribs1hvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribs1hvNV;
}
void PT_CALL glVertexAttribs1svNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs1svNV, 3);
}
void PT_CALL glVertexAttribs2dvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs2dvNV, 3);
}
void PT_CALL glVertexAttribs2fvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs2fvNV, 3);
}
void PT_CALL glVertexAttribs2hvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribs2hvNV;
}
void PT_CALL glVertexAttribs2svNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 2*arg1*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs2svNV, 3);
}
void PT_CALL glVertexAttribs3dvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 3*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs3dvNV, 3);
}
void PT_CALL glVertexAttribs3fvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 3*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs3fvNV, 3);
}
void PT_CALL glVertexAttribs3hvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribs3hvNV;
}
void PT_CALL glVertexAttribs3svNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 3*arg1*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs3svNV, 3);
}
void PT_CALL glVertexAttribs4dvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(double));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs4dvNV, 3);
}
void PT_CALL glVertexAttribs4fvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs4fvNV, 3);
}
void PT_CALL glVertexAttribs4hvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexAttribs4hvNV;
}
void PT_CALL glVertexAttribs4svNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(short));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs4svNV, 3);
}
void PT_CALL glVertexAttribs4ubvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, 4*arg1*sizeof(unsigned char));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexAttribs4ubvNV, 3);
}
void PT_CALL glVertexBindingDivisor(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexBindingDivisor;
}
void PT_CALL glVertexBlendARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexBlendARB, 1);
}
void PT_CALL glVertexBlendEnvfATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexBlendEnvfATI;
}
void PT_CALL glVertexBlendEnviATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexBlendEnviATI;
}
void PT_CALL glVertexFormatNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexFormatNV;
}
void PT_CALL glVertexP2ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexP2ui;
}
void PT_CALL glVertexP2uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexP2uiv;
}
void PT_CALL glVertexP3ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexP3ui;
}
void PT_CALL glVertexP3uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexP3uiv;
}
void PT_CALL glVertexP4ui(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexP4ui;
}
void PT_CALL glVertexP4uiv(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexP4uiv;
}
void PT_CALL glVertexPointer(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexPointer, 4);
    vtxarry_init(&vtxArry.Vertex, arg0, arg1, arg2, (void *)arg3);
}
void PT_CALL glVertexPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexPointerEXT, 5);
    vtxarry_init(&vtxArry.Vertex, arg0, arg1, arg2, (void *)arg4);
}
void PT_CALL glVertexPointerListIBM(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexPointerListIBM;
}
void PT_CALL glVertexPointervINTEL(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexPointervINTEL;
}
void PT_CALL glVertexStream1dATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1dATI;
}
void PT_CALL glVertexStream1dvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1dvATI;
}
void PT_CALL glVertexStream1fATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1fATI;
}
void PT_CALL glVertexStream1fvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1fvATI;
}
void PT_CALL glVertexStream1iATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1iATI;
}
void PT_CALL glVertexStream1ivATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1ivATI;
}
void PT_CALL glVertexStream1sATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1sATI;
}
void PT_CALL glVertexStream1svATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream1svATI;
}
void PT_CALL glVertexStream2dATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2dATI;
}
void PT_CALL glVertexStream2dvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2dvATI;
}
void PT_CALL glVertexStream2fATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2fATI;
}
void PT_CALL glVertexStream2fvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2fvATI;
}
void PT_CALL glVertexStream2iATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2iATI;
}
void PT_CALL glVertexStream2ivATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2ivATI;
}
void PT_CALL glVertexStream2sATI(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2sATI;
}
void PT_CALL glVertexStream2svATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream2svATI;
}
void PT_CALL glVertexStream3dATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3dATI;
}
void PT_CALL glVertexStream3dvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3dvATI;
}
void PT_CALL glVertexStream3fATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3fATI;
}
void PT_CALL glVertexStream3fvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3fvATI;
}
void PT_CALL glVertexStream3iATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3iATI;
}
void PT_CALL glVertexStream3ivATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3ivATI;
}
void PT_CALL glVertexStream3sATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3sATI;
}
void PT_CALL glVertexStream3svATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream3svATI;
}
void PT_CALL glVertexStream4dATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; pt[9] = arg8; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4dATI;
}
void PT_CALL glVertexStream4dvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4dvATI;
}
void PT_CALL glVertexStream4fATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4fATI;
}
void PT_CALL glVertexStream4fvATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4fvATI;
}
void PT_CALL glVertexStream4iATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4iATI;
}
void PT_CALL glVertexStream4ivATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4ivATI;
}
void PT_CALL glVertexStream4sATI(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4sATI;
}
void PT_CALL glVertexStream4svATI(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexStream4svATI;
}
void PT_CALL glVertexWeightPointerEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexWeightPointerEXT, 4);
    vtxarry_init(&vtxArry.Weight, arg0, arg1, arg2, (void *)arg3);
}
void PT_CALL glVertexWeightfEXT(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexWeightfEXT, 1);
}
void PT_CALL glVertexWeightfvEXT(uint32_t arg0) {
    fifoAddData(0, arg0, ALIGNED(sizeof(float)));
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glVertexWeightfvEXT, 1);
}
void PT_CALL glVertexWeighthNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexWeighthNV;
}
void PT_CALL glVertexWeighthvNV(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVertexWeighthvNV;
}
void PT_CALL glVideoCaptureNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVideoCaptureNV;
}
void PT_CALL glVideoCaptureStreamParameterdvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVideoCaptureStreamParameterdvNV;
}
void PT_CALL glVideoCaptureStreamParameterfvNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVideoCaptureStreamParameterfvNV;
}
void PT_CALL glVideoCaptureStreamParameterivNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glVideoCaptureStreamParameterivNV;
}
void PT_CALL glViewport(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glViewport, 4);
}
void PT_CALL glViewportArrayv(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    fifoAddData(0, arg2, arg1*4*sizeof(int));
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glViewportArrayv, 3);
}
void PT_CALL glViewportIndexedf(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glViewportIndexedf, 5);
}
void PT_CALL glViewportIndexedfv(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, 4*sizeof(float));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glViewportIndexedfv, 2);
}
void PT_CALL glViewportPositionWScaleNV(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glViewportPositionWScaleNV;
}
void PT_CALL glViewportSwizzleNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glViewportSwizzleNV;
}
void PT_CALL glWaitSemaphoreEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWaitSemaphoreEXT;
}
void PT_CALL glWaitSync(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWaitSync, 4);
}
void PT_CALL glWaitVkSemaphoreNV(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWaitVkSemaphoreNV;
}
void PT_CALL glWeightPathsNV(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWeightPathsNV;
}
void PT_CALL glWeightPointerARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightPointerARB, 4);
    vtxarry_init(&vtxArry.Weight, arg0, arg1, arg2, (void *)arg3);
}
void PT_CALL glWeightbvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(arg0 * sizeof(char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightbvARB, 2);
}
void PT_CALL glWeightdvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, arg0 * sizeof(double));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightdvARB, 2);
}
void PT_CALL glWeightfvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(arg0 * sizeof(float)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightfvARB, 2);
}
void PT_CALL glWeightivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(arg0 * sizeof(int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightivARB, 2);
}
void PT_CALL glWeightsvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(arg0 * sizeof(short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightsvARB, 2);
}
void PT_CALL glWeightubvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(arg0 * sizeof(unsigned char)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightubvARB, 2);
}
void PT_CALL glWeightuivARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(arg0 * sizeof(unsigned int)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightuivARB, 2);
}
void PT_CALL glWeightusvARB(uint32_t arg0, uint32_t arg1) {
    fifoAddData(0, arg1, ALIGNED(arg0 * sizeof(unsigned short)));
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; FIFO_GLFUNC(FEnum_glWeightusvARB, 2);
}
void PT_CALL glWindowPos2d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2d;
}
void PT_CALL glWindowPos2dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2dARB;
}
void PT_CALL glWindowPos2dMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2dMESA;
}
void PT_CALL glWindowPos2dv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2dv;
}
void PT_CALL glWindowPos2dvARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2dvARB;
}
void PT_CALL glWindowPos2dvMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2dvMESA;
}
void PT_CALL glWindowPos2f(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2f;
}
void PT_CALL glWindowPos2fARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2fARB;
}
void PT_CALL glWindowPos2fMESA(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2fMESA;
}
void PT_CALL glWindowPos2fv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2fv;
}
void PT_CALL glWindowPos2fvARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2fvARB;
}
void PT_CALL glWindowPos2fvMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2fvMESA;
}
void PT_CALL glWindowPos2i(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2i;
}
void PT_CALL glWindowPos2iARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2iARB;
}
void PT_CALL glWindowPos2iMESA(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2iMESA;
}
void PT_CALL glWindowPos2iv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2iv;
}
void PT_CALL glWindowPos2ivARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2ivARB;
}
void PT_CALL glWindowPos2ivMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2ivMESA;
}
void PT_CALL glWindowPos2s(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2s;
}
void PT_CALL glWindowPos2sARB(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2sARB;
}
void PT_CALL glWindowPos2sMESA(uint32_t arg0, uint32_t arg1) {
    pt[1] = arg0; pt[2] = arg1; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2sMESA;
}
void PT_CALL glWindowPos2sv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2sv;
}
void PT_CALL glWindowPos2svARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2svARB;
}
void PT_CALL glWindowPos2svMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos2svMESA;
}
void PT_CALL glWindowPos3d(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3d;
}
void PT_CALL glWindowPos3dARB(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3dARB;
}
void PT_CALL glWindowPos3dMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3dMESA;
}
void PT_CALL glWindowPos3dv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3dv;
}
void PT_CALL glWindowPos3dvARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3dvARB;
}
void PT_CALL glWindowPos3dvMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3dvMESA;
}
void PT_CALL glWindowPos3f(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3f;
}
void PT_CALL glWindowPos3fARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3fARB;
}
void PT_CALL glWindowPos3fMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3fMESA;
}
void PT_CALL glWindowPos3fv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3fv;
}
void PT_CALL glWindowPos3fvARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3fvARB;
}
void PT_CALL glWindowPos3fvMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3fvMESA;
}
void PT_CALL glWindowPos3i(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3i;
}
void PT_CALL glWindowPos3iARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3iARB;
}
void PT_CALL glWindowPos3iMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3iMESA;
}
void PT_CALL glWindowPos3iv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3iv;
}
void PT_CALL glWindowPos3ivARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3ivARB;
}
void PT_CALL glWindowPos3ivMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3ivMESA;
}
void PT_CALL glWindowPos3s(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3s;
}
void PT_CALL glWindowPos3sARB(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3sARB;
}
void PT_CALL glWindowPos3sMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3sMESA;
}
void PT_CALL glWindowPos3sv(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3sv;
}
void PT_CALL glWindowPos3svARB(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3svARB;
}
void PT_CALL glWindowPos3svMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos3svMESA;
}
void PT_CALL glWindowPos4dMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; pt[7] = arg6; pt[8] = arg7; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4dMESA;
}
void PT_CALL glWindowPos4dvMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4dvMESA;
}
void PT_CALL glWindowPos4fMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4fMESA;
}
void PT_CALL glWindowPos4fvMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4fvMESA;
}
void PT_CALL glWindowPos4iMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4iMESA;
}
void PT_CALL glWindowPos4ivMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4ivMESA;
}
void PT_CALL glWindowPos4sMESA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4sMESA;
}
void PT_CALL glWindowPos4svMESA(uint32_t arg0) {
    pt[1] = arg0; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowPos4svMESA;
}
void PT_CALL glWindowRectanglesEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWindowRectanglesEXT;
}
void PT_CALL glWriteMaskEXT(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    pt[1] = arg0; pt[2] = arg1; pt[3] = arg2; pt[4] = arg3; pt[5] = arg4; pt[6] = arg5; 
    pt0 = (uint32_t *)pt[0]; *pt0 = FEnum_glWriteMaskEXT;
}
/* End - generated by mstub_genfuncs */

static uint32_t getExtNameEntry(char *name)
{
    static const struct {
        char *name;
        void *entry;
    } ExtNameEntryTbl[] = {

/* -------- BEGIN generated by mkmexttbl -------- */

    { .name = "glAccumxOES", .entry = (void *)&glAccumxOES },
    { .name = "glAcquireKeyedMutexWin32EXT", .entry = (void *)&glAcquireKeyedMutexWin32EXT },
    { .name = "glActiveProgramEXT", .entry = (void *)&glActiveProgramEXT },
    { .name = "glActiveShaderProgram", .entry = (void *)&glActiveShaderProgram },
    { .name = "glActiveStencilFaceEXT", .entry = (void *)&glActiveStencilFaceEXT },
    { .name = "glActiveTexture", .entry = (void *)&glActiveTexture },
    { .name = "glActiveTextureARB", .entry = (void *)&glActiveTextureARB },
    { .name = "glActiveVaryingNV", .entry = (void *)&glActiveVaryingNV },
    { .name = "glAlphaFragmentOp1ATI", .entry = (void *)&glAlphaFragmentOp1ATI },
    { .name = "glAlphaFragmentOp2ATI", .entry = (void *)&glAlphaFragmentOp2ATI },
    { .name = "glAlphaFragmentOp3ATI", .entry = (void *)&glAlphaFragmentOp3ATI },
    { .name = "glAlphaFuncxOES", .entry = (void *)&glAlphaFuncxOES },
    { .name = "glAlphaToCoverageDitherControlNV", .entry = (void *)&glAlphaToCoverageDitherControlNV },
    { .name = "glApplyFramebufferAttachmentCMAAINTEL", .entry = (void *)&glApplyFramebufferAttachmentCMAAINTEL },
    { .name = "glApplyTextureEXT", .entry = (void *)&glApplyTextureEXT },
    { .name = "glAreProgramsResidentNV", .entry = (void *)&glAreProgramsResidentNV },
    { .name = "glAreTexturesResidentEXT", .entry = (void *)&glAreTexturesResidentEXT },
    { .name = "glArrayElementEXT", .entry = (void *)&glArrayElementEXT },
    { .name = "glArrayObjectATI", .entry = (void *)&glArrayObjectATI },
    { .name = "glAsyncMarkerSGIX", .entry = (void *)&glAsyncMarkerSGIX },
    { .name = "glAttachObjectARB", .entry = (void *)&glAttachObjectARB },
    { .name = "glAttachShader", .entry = (void *)&glAttachShader },
    { .name = "glBeginConditionalRender", .entry = (void *)&glBeginConditionalRender },
    { .name = "glBeginConditionalRenderNV", .entry = (void *)&glBeginConditionalRenderNV },
    { .name = "glBeginConditionalRenderNVX", .entry = (void *)&glBeginConditionalRenderNVX },
    { .name = "glBeginFragmentShaderATI", .entry = (void *)&glBeginFragmentShaderATI },
    { .name = "glBeginOcclusionQueryNV", .entry = (void *)&glBeginOcclusionQueryNV },
    { .name = "glBeginPerfMonitorAMD", .entry = (void *)&glBeginPerfMonitorAMD },
    { .name = "glBeginPerfQueryINTEL", .entry = (void *)&glBeginPerfQueryINTEL },
    { .name = "glBeginQuery", .entry = (void *)&glBeginQuery },
    { .name = "glBeginQueryARB", .entry = (void *)&glBeginQueryARB },
    { .name = "glBeginQueryIndexed", .entry = (void *)&glBeginQueryIndexed },
    { .name = "glBeginTransformFeedback", .entry = (void *)&glBeginTransformFeedback },
    { .name = "glBeginTransformFeedbackEXT", .entry = (void *)&glBeginTransformFeedbackEXT },
    { .name = "glBeginTransformFeedbackNV", .entry = (void *)&glBeginTransformFeedbackNV },
    { .name = "glBeginVertexShaderEXT", .entry = (void *)&glBeginVertexShaderEXT },
    { .name = "glBeginVideoCaptureNV", .entry = (void *)&glBeginVideoCaptureNV },
    { .name = "glBindAttribLocation", .entry = (void *)&glBindAttribLocation },
    { .name = "glBindAttribLocationARB", .entry = (void *)&glBindAttribLocationARB },
    { .name = "glBindBuffer", .entry = (void *)&glBindBuffer },
    { .name = "glBindBufferARB", .entry = (void *)&glBindBufferARB },
    { .name = "glBindBufferBase", .entry = (void *)&glBindBufferBase },
    { .name = "glBindBufferBaseEXT", .entry = (void *)&glBindBufferBaseEXT },
    { .name = "glBindBufferBaseNV", .entry = (void *)&glBindBufferBaseNV },
    { .name = "glBindBufferOffsetEXT", .entry = (void *)&glBindBufferOffsetEXT },
    { .name = "glBindBufferOffsetNV", .entry = (void *)&glBindBufferOffsetNV },
    { .name = "glBindBufferRange", .entry = (void *)&glBindBufferRange },
    { .name = "glBindBufferRangeEXT", .entry = (void *)&glBindBufferRangeEXT },
    { .name = "glBindBufferRangeNV", .entry = (void *)&glBindBufferRangeNV },
    { .name = "glBindBuffersBase", .entry = (void *)&glBindBuffersBase },
    { .name = "glBindBuffersRange", .entry = (void *)&glBindBuffersRange },
    { .name = "glBindFragDataLocation", .entry = (void *)&glBindFragDataLocation },
    { .name = "glBindFragDataLocationEXT", .entry = (void *)&glBindFragDataLocationEXT },
    { .name = "glBindFragDataLocationIndexed", .entry = (void *)&glBindFragDataLocationIndexed },
    { .name = "glBindFragmentShaderATI", .entry = (void *)&glBindFragmentShaderATI },
    { .name = "glBindFramebuffer", .entry = (void *)&glBindFramebuffer },
    { .name = "glBindFramebufferEXT", .entry = (void *)&glBindFramebufferEXT },
    { .name = "glBindImageTexture", .entry = (void *)&glBindImageTexture },
    { .name = "glBindImageTextureEXT", .entry = (void *)&glBindImageTextureEXT },
    { .name = "glBindImageTextures", .entry = (void *)&glBindImageTextures },
    { .name = "glBindLightParameterEXT", .entry = (void *)&glBindLightParameterEXT },
    { .name = "glBindMaterialParameterEXT", .entry = (void *)&glBindMaterialParameterEXT },
    { .name = "glBindMultiTextureEXT", .entry = (void *)&glBindMultiTextureEXT },
    { .name = "glBindParameterEXT", .entry = (void *)&glBindParameterEXT },
    { .name = "glBindProgramARB", .entry = (void *)&glBindProgramARB },
    { .name = "glBindProgramNV", .entry = (void *)&glBindProgramNV },
    { .name = "glBindProgramPipeline", .entry = (void *)&glBindProgramPipeline },
    { .name = "glBindRenderbuffer", .entry = (void *)&glBindRenderbuffer },
    { .name = "glBindRenderbufferEXT", .entry = (void *)&glBindRenderbufferEXT },
    { .name = "glBindSampler", .entry = (void *)&glBindSampler },
    { .name = "glBindSamplers", .entry = (void *)&glBindSamplers },
    { .name = "glBindShadingRateImageNV", .entry = (void *)&glBindShadingRateImageNV },
    { .name = "glBindTexGenParameterEXT", .entry = (void *)&glBindTexGenParameterEXT },
    { .name = "glBindTextureEXT", .entry = (void *)&glBindTextureEXT },
    { .name = "glBindTextures", .entry = (void *)&glBindTextures },
    { .name = "glBindTextureUnit", .entry = (void *)&glBindTextureUnit },
    { .name = "glBindTextureUnitParameterEXT", .entry = (void *)&glBindTextureUnitParameterEXT },
    { .name = "glBindTransformFeedback", .entry = (void *)&glBindTransformFeedback },
    { .name = "glBindTransformFeedbackNV", .entry = (void *)&glBindTransformFeedbackNV },
    { .name = "glBindVertexArray", .entry = (void *)&glBindVertexArray },
    { .name = "glBindVertexArrayAPPLE", .entry = (void *)&glBindVertexArrayAPPLE },
    { .name = "glBindVertexBuffer", .entry = (void *)&glBindVertexBuffer },
    { .name = "glBindVertexBuffers", .entry = (void *)&glBindVertexBuffers },
    { .name = "glBindVertexShaderEXT", .entry = (void *)&glBindVertexShaderEXT },
    { .name = "glBindVideoCaptureStreamBufferNV", .entry = (void *)&glBindVideoCaptureStreamBufferNV },
    { .name = "glBindVideoCaptureStreamTextureNV", .entry = (void *)&glBindVideoCaptureStreamTextureNV },
    { .name = "glBinormal3bEXT", .entry = (void *)&glBinormal3bEXT },
    { .name = "glBinormal3bvEXT", .entry = (void *)&glBinormal3bvEXT },
    { .name = "glBinormal3dEXT", .entry = (void *)&glBinormal3dEXT },
    { .name = "glBinormal3dvEXT", .entry = (void *)&glBinormal3dvEXT },
    { .name = "glBinormal3fEXT", .entry = (void *)&glBinormal3fEXT },
    { .name = "glBinormal3fvEXT", .entry = (void *)&glBinormal3fvEXT },
    { .name = "glBinormal3iEXT", .entry = (void *)&glBinormal3iEXT },
    { .name = "glBinormal3ivEXT", .entry = (void *)&glBinormal3ivEXT },
    { .name = "glBinormal3sEXT", .entry = (void *)&glBinormal3sEXT },
    { .name = "glBinormal3svEXT", .entry = (void *)&glBinormal3svEXT },
    { .name = "glBinormalPointerEXT", .entry = (void *)&glBinormalPointerEXT },
    { .name = "glBitmapxOES", .entry = (void *)&glBitmapxOES },
    { .name = "glBlendBarrierKHR", .entry = (void *)&glBlendBarrierKHR },
    { .name = "glBlendBarrierNV", .entry = (void *)&glBlendBarrierNV },
    { .name = "glBlendColor", .entry = (void *)&glBlendColor },
    { .name = "glBlendColorEXT", .entry = (void *)&glBlendColorEXT },
    { .name = "glBlendColorxOES", .entry = (void *)&glBlendColorxOES },
    { .name = "glBlendEquation", .entry = (void *)&glBlendEquation },
    { .name = "glBlendEquationEXT", .entry = (void *)&glBlendEquationEXT },
    { .name = "glBlendEquationi", .entry = (void *)&glBlendEquationi },
    { .name = "glBlendEquationiARB", .entry = (void *)&glBlendEquationiARB },
    { .name = "glBlendEquationIndexedAMD", .entry = (void *)&glBlendEquationIndexedAMD },
    { .name = "glBlendEquationSeparate", .entry = (void *)&glBlendEquationSeparate },
    { .name = "glBlendEquationSeparateEXT", .entry = (void *)&glBlendEquationSeparateEXT },
    { .name = "glBlendEquationSeparatei", .entry = (void *)&glBlendEquationSeparatei },
    { .name = "glBlendEquationSeparateiARB", .entry = (void *)&glBlendEquationSeparateiARB },
    { .name = "glBlendEquationSeparateIndexedAMD", .entry = (void *)&glBlendEquationSeparateIndexedAMD },
    { .name = "glBlendFunci", .entry = (void *)&glBlendFunci },
    { .name = "glBlendFunciARB", .entry = (void *)&glBlendFunciARB },
    { .name = "glBlendFuncIndexedAMD", .entry = (void *)&glBlendFuncIndexedAMD },
    { .name = "glBlendFuncSeparate", .entry = (void *)&glBlendFuncSeparate },
    { .name = "glBlendFuncSeparateEXT", .entry = (void *)&glBlendFuncSeparateEXT },
    { .name = "glBlendFuncSeparatei", .entry = (void *)&glBlendFuncSeparatei },
    { .name = "glBlendFuncSeparateiARB", .entry = (void *)&glBlendFuncSeparateiARB },
    { .name = "glBlendFuncSeparateIndexedAMD", .entry = (void *)&glBlendFuncSeparateIndexedAMD },
    { .name = "glBlendFuncSeparateINGR", .entry = (void *)&glBlendFuncSeparateINGR },
    { .name = "glBlendParameteriNV", .entry = (void *)&glBlendParameteriNV },
    { .name = "glBlitFramebuffer", .entry = (void *)&glBlitFramebuffer },
    { .name = "glBlitFramebufferEXT", .entry = (void *)&glBlitFramebufferEXT },
    { .name = "glBlitNamedFramebuffer", .entry = (void *)&glBlitNamedFramebuffer },
    { .name = "glBufferAddressRangeNV", .entry = (void *)&glBufferAddressRangeNV },
    { .name = "glBufferAttachMemoryNV", .entry = (void *)&glBufferAttachMemoryNV },
    { .name = "glBufferData", .entry = (void *)&glBufferData },
    { .name = "glBufferDataARB", .entry = (void *)&glBufferDataARB },
    { .name = "glBufferPageCommitmentARB", .entry = (void *)&glBufferPageCommitmentARB },
    { .name = "glBufferParameteriAPPLE", .entry = (void *)&glBufferParameteriAPPLE },
    { .name = "glBufferStorage", .entry = (void *)&glBufferStorage },
    { .name = "glBufferStorageExternalEXT", .entry = (void *)&glBufferStorageExternalEXT },
    { .name = "glBufferStorageMemEXT", .entry = (void *)&glBufferStorageMemEXT },
    { .name = "glBufferSubData", .entry = (void *)&glBufferSubData },
    { .name = "glBufferSubDataARB", .entry = (void *)&glBufferSubDataARB },
    { .name = "glCallCommandListNV", .entry = (void *)&glCallCommandListNV },
    { .name = "glCheckFramebufferStatus", .entry = (void *)&glCheckFramebufferStatus },
    { .name = "glCheckFramebufferStatusEXT", .entry = (void *)&glCheckFramebufferStatusEXT },
    { .name = "glCheckNamedFramebufferStatus", .entry = (void *)&glCheckNamedFramebufferStatus },
    { .name = "glCheckNamedFramebufferStatusEXT", .entry = (void *)&glCheckNamedFramebufferStatusEXT },
    { .name = "glClampColor", .entry = (void *)&glClampColor },
    { .name = "glClampColorARB", .entry = (void *)&glClampColorARB },
    { .name = "glClearAccumxOES", .entry = (void *)&glClearAccumxOES },
    { .name = "glClearBufferData", .entry = (void *)&glClearBufferData },
    { .name = "glClearBufferfi", .entry = (void *)&glClearBufferfi },
    { .name = "glClearBufferfv", .entry = (void *)&glClearBufferfv },
    { .name = "glClearBufferiv", .entry = (void *)&glClearBufferiv },
    { .name = "glClearBufferSubData", .entry = (void *)&glClearBufferSubData },
    { .name = "glClearBufferuiv", .entry = (void *)&glClearBufferuiv },
    { .name = "glClearColorIiEXT", .entry = (void *)&glClearColorIiEXT },
    { .name = "glClearColorIuiEXT", .entry = (void *)&glClearColorIuiEXT },
    { .name = "glClearColorxOES", .entry = (void *)&glClearColorxOES },
    { .name = "glClearDepthdNV", .entry = (void *)&glClearDepthdNV },
    { .name = "glClearDepthf", .entry = (void *)&glClearDepthf },
    { .name = "glClearDepthfOES", .entry = (void *)&glClearDepthfOES },
    { .name = "glClearDepthxOES", .entry = (void *)&glClearDepthxOES },
    { .name = "glClearNamedBufferData", .entry = (void *)&glClearNamedBufferData },
    { .name = "glClearNamedBufferDataEXT", .entry = (void *)&glClearNamedBufferDataEXT },
    { .name = "glClearNamedBufferSubData", .entry = (void *)&glClearNamedBufferSubData },
    { .name = "glClearNamedBufferSubDataEXT", .entry = (void *)&glClearNamedBufferSubDataEXT },
    { .name = "glClearNamedFramebufferfi", .entry = (void *)&glClearNamedFramebufferfi },
    { .name = "glClearNamedFramebufferfv", .entry = (void *)&glClearNamedFramebufferfv },
    { .name = "glClearNamedFramebufferiv", .entry = (void *)&glClearNamedFramebufferiv },
    { .name = "glClearNamedFramebufferuiv", .entry = (void *)&glClearNamedFramebufferuiv },
    { .name = "glClearTexImage", .entry = (void *)&glClearTexImage },
    { .name = "glClearTexSubImage", .entry = (void *)&glClearTexSubImage },
    { .name = "glClientActiveTexture", .entry = (void *)&glClientActiveTexture },
    { .name = "glClientActiveTextureARB", .entry = (void *)&glClientActiveTextureARB },
    { .name = "glClientActiveVertexStreamATI", .entry = (void *)&glClientActiveVertexStreamATI },
    { .name = "glClientAttribDefaultEXT", .entry = (void *)&glClientAttribDefaultEXT },
    { .name = "glClientWaitSync", .entry = (void *)&glClientWaitSync },
    { .name = "glClipControl", .entry = (void *)&glClipControl },
    { .name = "glClipPlanefOES", .entry = (void *)&glClipPlanefOES },
    { .name = "glClipPlanexOES", .entry = (void *)&glClipPlanexOES },
    { .name = "glColor3fVertex3fSUN", .entry = (void *)&glColor3fVertex3fSUN },
    { .name = "glColor3fVertex3fvSUN", .entry = (void *)&glColor3fVertex3fvSUN },
    { .name = "glColor3hNV", .entry = (void *)&glColor3hNV },
    { .name = "glColor3hvNV", .entry = (void *)&glColor3hvNV },
    { .name = "glColor3xOES", .entry = (void *)&glColor3xOES },
    { .name = "glColor3xvOES", .entry = (void *)&glColor3xvOES },
    { .name = "glColor4fNormal3fVertex3fSUN", .entry = (void *)&glColor4fNormal3fVertex3fSUN },
    { .name = "glColor4fNormal3fVertex3fvSUN", .entry = (void *)&glColor4fNormal3fVertex3fvSUN },
    { .name = "glColor4hNV", .entry = (void *)&glColor4hNV },
    { .name = "glColor4hvNV", .entry = (void *)&glColor4hvNV },
    { .name = "glColor4ubVertex2fSUN", .entry = (void *)&glColor4ubVertex2fSUN },
    { .name = "glColor4ubVertex2fvSUN", .entry = (void *)&glColor4ubVertex2fvSUN },
    { .name = "glColor4ubVertex3fSUN", .entry = (void *)&glColor4ubVertex3fSUN },
    { .name = "glColor4ubVertex3fvSUN", .entry = (void *)&glColor4ubVertex3fvSUN },
    { .name = "glColor4xOES", .entry = (void *)&glColor4xOES },
    { .name = "glColor4xvOES", .entry = (void *)&glColor4xvOES },
    { .name = "glColorFormatNV", .entry = (void *)&glColorFormatNV },
    { .name = "glColorFragmentOp1ATI", .entry = (void *)&glColorFragmentOp1ATI },
    { .name = "glColorFragmentOp2ATI", .entry = (void *)&glColorFragmentOp2ATI },
    { .name = "glColorFragmentOp3ATI", .entry = (void *)&glColorFragmentOp3ATI },
    { .name = "glColorMaski", .entry = (void *)&glColorMaski },
    { .name = "glColorMaskIndexedEXT", .entry = (void *)&glColorMaskIndexedEXT },
    { .name = "glColorP3ui", .entry = (void *)&glColorP3ui },
    { .name = "glColorP3uiv", .entry = (void *)&glColorP3uiv },
    { .name = "glColorP4ui", .entry = (void *)&glColorP4ui },
    { .name = "glColorP4uiv", .entry = (void *)&glColorP4uiv },
    { .name = "glColorPointerEXT", .entry = (void *)&glColorPointerEXT },
    { .name = "glColorPointerListIBM", .entry = (void *)&glColorPointerListIBM },
    { .name = "glColorPointervINTEL", .entry = (void *)&glColorPointervINTEL },
    { .name = "glColorSubTable", .entry = (void *)&glColorSubTable },
    { .name = "glColorSubTableEXT", .entry = (void *)&glColorSubTableEXT },
    { .name = "glColorTable", .entry = (void *)&glColorTable },
    { .name = "glColorTableEXT", .entry = (void *)&glColorTableEXT },
    { .name = "glColorTableParameterfv", .entry = (void *)&glColorTableParameterfv },
    { .name = "glColorTableParameterfvSGI", .entry = (void *)&glColorTableParameterfvSGI },
    { .name = "glColorTableParameteriv", .entry = (void *)&glColorTableParameteriv },
    { .name = "glColorTableParameterivSGI", .entry = (void *)&glColorTableParameterivSGI },
    { .name = "glColorTableSGI", .entry = (void *)&glColorTableSGI },
    { .name = "glCombinerInputNV", .entry = (void *)&glCombinerInputNV },
    { .name = "glCombinerOutputNV", .entry = (void *)&glCombinerOutputNV },
    { .name = "glCombinerParameterfNV", .entry = (void *)&glCombinerParameterfNV },
    { .name = "glCombinerParameterfvNV", .entry = (void *)&glCombinerParameterfvNV },
    { .name = "glCombinerParameteriNV", .entry = (void *)&glCombinerParameteriNV },
    { .name = "glCombinerParameterivNV", .entry = (void *)&glCombinerParameterivNV },
    { .name = "glCombinerStageParameterfvNV", .entry = (void *)&glCombinerStageParameterfvNV },
    { .name = "glCommandListSegmentsNV", .entry = (void *)&glCommandListSegmentsNV },
    { .name = "glCompileCommandListNV", .entry = (void *)&glCompileCommandListNV },
    { .name = "glCompileShader", .entry = (void *)&glCompileShader },
    { .name = "glCompileShaderARB", .entry = (void *)&glCompileShaderARB },
    { .name = "glCompileShaderIncludeARB", .entry = (void *)&glCompileShaderIncludeARB },
    { .name = "glCompressedMultiTexImage1DEXT", .entry = (void *)&glCompressedMultiTexImage1DEXT },
    { .name = "glCompressedMultiTexImage2DEXT", .entry = (void *)&glCompressedMultiTexImage2DEXT },
    { .name = "glCompressedMultiTexImage3DEXT", .entry = (void *)&glCompressedMultiTexImage3DEXT },
    { .name = "glCompressedMultiTexSubImage1DEXT", .entry = (void *)&glCompressedMultiTexSubImage1DEXT },
    { .name = "glCompressedMultiTexSubImage2DEXT", .entry = (void *)&glCompressedMultiTexSubImage2DEXT },
    { .name = "glCompressedMultiTexSubImage3DEXT", .entry = (void *)&glCompressedMultiTexSubImage3DEXT },
    { .name = "glCompressedTexImage1D", .entry = (void *)&glCompressedTexImage1D },
    { .name = "glCompressedTexImage1DARB", .entry = (void *)&glCompressedTexImage1DARB },
    { .name = "glCompressedTexImage2D", .entry = (void *)&glCompressedTexImage2D },
    { .name = "glCompressedTexImage2DARB", .entry = (void *)&glCompressedTexImage2DARB },
    { .name = "glCompressedTexImage3D", .entry = (void *)&glCompressedTexImage3D },
    { .name = "glCompressedTexImage3DARB", .entry = (void *)&glCompressedTexImage3DARB },
    { .name = "glCompressedTexSubImage1D", .entry = (void *)&glCompressedTexSubImage1D },
    { .name = "glCompressedTexSubImage1DARB", .entry = (void *)&glCompressedTexSubImage1DARB },
    { .name = "glCompressedTexSubImage2D", .entry = (void *)&glCompressedTexSubImage2D },
    { .name = "glCompressedTexSubImage2DARB", .entry = (void *)&glCompressedTexSubImage2DARB },
    { .name = "glCompressedTexSubImage3D", .entry = (void *)&glCompressedTexSubImage3D },
    { .name = "glCompressedTexSubImage3DARB", .entry = (void *)&glCompressedTexSubImage3DARB },
    { .name = "glCompressedTextureImage1DEXT", .entry = (void *)&glCompressedTextureImage1DEXT },
    { .name = "glCompressedTextureImage2DEXT", .entry = (void *)&glCompressedTextureImage2DEXT },
    { .name = "glCompressedTextureImage3DEXT", .entry = (void *)&glCompressedTextureImage3DEXT },
    { .name = "glCompressedTextureSubImage1D", .entry = (void *)&glCompressedTextureSubImage1D },
    { .name = "glCompressedTextureSubImage1DEXT", .entry = (void *)&glCompressedTextureSubImage1DEXT },
    { .name = "glCompressedTextureSubImage2D", .entry = (void *)&glCompressedTextureSubImage2D },
    { .name = "glCompressedTextureSubImage2DEXT", .entry = (void *)&glCompressedTextureSubImage2DEXT },
    { .name = "glCompressedTextureSubImage3D", .entry = (void *)&glCompressedTextureSubImage3D },
    { .name = "glCompressedTextureSubImage3DEXT", .entry = (void *)&glCompressedTextureSubImage3DEXT },
    { .name = "glConservativeRasterParameterfNV", .entry = (void *)&glConservativeRasterParameterfNV },
    { .name = "glConservativeRasterParameteriNV", .entry = (void *)&glConservativeRasterParameteriNV },
    { .name = "glConvolutionFilter1D", .entry = (void *)&glConvolutionFilter1D },
    { .name = "glConvolutionFilter1DEXT", .entry = (void *)&glConvolutionFilter1DEXT },
    { .name = "glConvolutionFilter2D", .entry = (void *)&glConvolutionFilter2D },
    { .name = "glConvolutionFilter2DEXT", .entry = (void *)&glConvolutionFilter2DEXT },
    { .name = "glConvolutionParameterf", .entry = (void *)&glConvolutionParameterf },
    { .name = "glConvolutionParameterfEXT", .entry = (void *)&glConvolutionParameterfEXT },
    { .name = "glConvolutionParameterfv", .entry = (void *)&glConvolutionParameterfv },
    { .name = "glConvolutionParameterfvEXT", .entry = (void *)&glConvolutionParameterfvEXT },
    { .name = "glConvolutionParameteri", .entry = (void *)&glConvolutionParameteri },
    { .name = "glConvolutionParameteriEXT", .entry = (void *)&glConvolutionParameteriEXT },
    { .name = "glConvolutionParameteriv", .entry = (void *)&glConvolutionParameteriv },
    { .name = "glConvolutionParameterivEXT", .entry = (void *)&glConvolutionParameterivEXT },
    { .name = "glConvolutionParameterxOES", .entry = (void *)&glConvolutionParameterxOES },
    { .name = "glConvolutionParameterxvOES", .entry = (void *)&glConvolutionParameterxvOES },
    { .name = "glCopyBufferSubData", .entry = (void *)&glCopyBufferSubData },
    { .name = "glCopyColorSubTable", .entry = (void *)&glCopyColorSubTable },
    { .name = "glCopyColorSubTableEXT", .entry = (void *)&glCopyColorSubTableEXT },
    { .name = "glCopyColorTable", .entry = (void *)&glCopyColorTable },
    { .name = "glCopyColorTableSGI", .entry = (void *)&glCopyColorTableSGI },
    { .name = "glCopyConvolutionFilter1D", .entry = (void *)&glCopyConvolutionFilter1D },
    { .name = "glCopyConvolutionFilter1DEXT", .entry = (void *)&glCopyConvolutionFilter1DEXT },
    { .name = "glCopyConvolutionFilter2D", .entry = (void *)&glCopyConvolutionFilter2D },
    { .name = "glCopyConvolutionFilter2DEXT", .entry = (void *)&glCopyConvolutionFilter2DEXT },
    { .name = "glCopyImageSubData", .entry = (void *)&glCopyImageSubData },
    { .name = "glCopyImageSubDataNV", .entry = (void *)&glCopyImageSubDataNV },
    { .name = "glCopyMultiTexImage1DEXT", .entry = (void *)&glCopyMultiTexImage1DEXT },
    { .name = "glCopyMultiTexImage2DEXT", .entry = (void *)&glCopyMultiTexImage2DEXT },
    { .name = "glCopyMultiTexSubImage1DEXT", .entry = (void *)&glCopyMultiTexSubImage1DEXT },
    { .name = "glCopyMultiTexSubImage2DEXT", .entry = (void *)&glCopyMultiTexSubImage2DEXT },
    { .name = "glCopyMultiTexSubImage3DEXT", .entry = (void *)&glCopyMultiTexSubImage3DEXT },
    { .name = "glCopyNamedBufferSubData", .entry = (void *)&glCopyNamedBufferSubData },
    { .name = "glCopyPathNV", .entry = (void *)&glCopyPathNV },
    { .name = "glCopyTexImage1DEXT", .entry = (void *)&glCopyTexImage1DEXT },
    { .name = "glCopyTexImage2DEXT", .entry = (void *)&glCopyTexImage2DEXT },
    { .name = "glCopyTexSubImage1DEXT", .entry = (void *)&glCopyTexSubImage1DEXT },
    { .name = "glCopyTexSubImage2DEXT", .entry = (void *)&glCopyTexSubImage2DEXT },
    { .name = "glCopyTexSubImage3D", .entry = (void *)&glCopyTexSubImage3D },
    { .name = "glCopyTexSubImage3DEXT", .entry = (void *)&glCopyTexSubImage3DEXT },
    { .name = "glCopyTextureImage1DEXT", .entry = (void *)&glCopyTextureImage1DEXT },
    { .name = "glCopyTextureImage2DEXT", .entry = (void *)&glCopyTextureImage2DEXT },
    { .name = "glCopyTextureSubImage1D", .entry = (void *)&glCopyTextureSubImage1D },
    { .name = "glCopyTextureSubImage1DEXT", .entry = (void *)&glCopyTextureSubImage1DEXT },
    { .name = "glCopyTextureSubImage2D", .entry = (void *)&glCopyTextureSubImage2D },
    { .name = "glCopyTextureSubImage2DEXT", .entry = (void *)&glCopyTextureSubImage2DEXT },
    { .name = "glCopyTextureSubImage3D", .entry = (void *)&glCopyTextureSubImage3D },
    { .name = "glCopyTextureSubImage3DEXT", .entry = (void *)&glCopyTextureSubImage3DEXT },
    { .name = "glCoverageModulationNV", .entry = (void *)&glCoverageModulationNV },
    { .name = "glCoverageModulationTableNV", .entry = (void *)&glCoverageModulationTableNV },
    { .name = "glCoverFillPathInstancedNV", .entry = (void *)&glCoverFillPathInstancedNV },
    { .name = "glCoverFillPathNV", .entry = (void *)&glCoverFillPathNV },
    { .name = "glCoverStrokePathInstancedNV", .entry = (void *)&glCoverStrokePathInstancedNV },
    { .name = "glCoverStrokePathNV", .entry = (void *)&glCoverStrokePathNV },
    { .name = "glCreateBuffers", .entry = (void *)&glCreateBuffers },
    { .name = "glCreateCommandListsNV", .entry = (void *)&glCreateCommandListsNV },
    { .name = "glCreateFramebuffers", .entry = (void *)&glCreateFramebuffers },
    { .name = "glCreateMemoryObjectsEXT", .entry = (void *)&glCreateMemoryObjectsEXT },
    { .name = "glCreatePerfQueryINTEL", .entry = (void *)&glCreatePerfQueryINTEL },
    { .name = "glCreateProgram", .entry = (void *)&glCreateProgram },
    { .name = "glCreateProgramObjectARB", .entry = (void *)&glCreateProgramObjectARB },
    { .name = "glCreateProgramPipelines", .entry = (void *)&glCreateProgramPipelines },
    { .name = "glCreateQueries", .entry = (void *)&glCreateQueries },
    { .name = "glCreateRenderbuffers", .entry = (void *)&glCreateRenderbuffers },
    { .name = "glCreateSamplers", .entry = (void *)&glCreateSamplers },
    { .name = "glCreateShader", .entry = (void *)&glCreateShader },
    { .name = "glCreateShaderObjectARB", .entry = (void *)&glCreateShaderObjectARB },
    { .name = "glCreateShaderProgramEXT", .entry = (void *)&glCreateShaderProgramEXT },
    { .name = "glCreateShaderProgramv", .entry = (void *)&glCreateShaderProgramv },
    { .name = "glCreateStatesNV", .entry = (void *)&glCreateStatesNV },
    { .name = "glCreateSyncFromCLeventARB", .entry = (void *)&glCreateSyncFromCLeventARB },
    { .name = "glCreateTextures", .entry = (void *)&glCreateTextures },
    { .name = "glCreateTransformFeedbacks", .entry = (void *)&glCreateTransformFeedbacks },
    { .name = "glCreateVertexArrays", .entry = (void *)&glCreateVertexArrays },
    { .name = "glCullParameterdvEXT", .entry = (void *)&glCullParameterdvEXT },
    { .name = "glCullParameterfvEXT", .entry = (void *)&glCullParameterfvEXT },
    { .name = "glCurrentPaletteMatrixARB", .entry = (void *)&glCurrentPaletteMatrixARB },
    { .name = "glDebugMessageCallback", .entry = (void *)&glDebugMessageCallback },
    { .name = "glDebugMessageCallbackAMD", .entry = (void *)&glDebugMessageCallbackAMD },
    { .name = "glDebugMessageCallbackARB", .entry = (void *)&glDebugMessageCallbackARB },
    { .name = "glDebugMessageControl", .entry = (void *)&glDebugMessageControl },
    { .name = "glDebugMessageControlARB", .entry = (void *)&glDebugMessageControlARB },
    { .name = "glDebugMessageEnableAMD", .entry = (void *)&glDebugMessageEnableAMD },
    { .name = "glDebugMessageInsert", .entry = (void *)&glDebugMessageInsert },
    { .name = "glDebugMessageInsertAMD", .entry = (void *)&glDebugMessageInsertAMD },
    { .name = "glDebugMessageInsertARB", .entry = (void *)&glDebugMessageInsertARB },
    { .name = "glDeformationMap3dSGIX", .entry = (void *)&glDeformationMap3dSGIX },
    { .name = "glDeformationMap3fSGIX", .entry = (void *)&glDeformationMap3fSGIX },
    { .name = "glDeformSGIX", .entry = (void *)&glDeformSGIX },
    { .name = "glDeleteAsyncMarkersSGIX", .entry = (void *)&glDeleteAsyncMarkersSGIX },
    { .name = "glDeleteBuffers", .entry = (void *)&glDeleteBuffers },
    { .name = "glDeleteBuffersARB", .entry = (void *)&glDeleteBuffersARB },
    { .name = "glDeleteCommandListsNV", .entry = (void *)&glDeleteCommandListsNV },
    { .name = "glDeleteFencesAPPLE", .entry = (void *)&glDeleteFencesAPPLE },
    { .name = "glDeleteFencesNV", .entry = (void *)&glDeleteFencesNV },
    { .name = "glDeleteFragmentShaderATI", .entry = (void *)&glDeleteFragmentShaderATI },
    { .name = "glDeleteFramebuffers", .entry = (void *)&glDeleteFramebuffers },
    { .name = "glDeleteFramebuffersEXT", .entry = (void *)&glDeleteFramebuffersEXT },
    { .name = "glDeleteMemoryObjectsEXT", .entry = (void *)&glDeleteMemoryObjectsEXT },
    { .name = "glDeleteNamedStringARB", .entry = (void *)&glDeleteNamedStringARB },
    { .name = "glDeleteNamesAMD", .entry = (void *)&glDeleteNamesAMD },
    { .name = "glDeleteObjectARB", .entry = (void *)&glDeleteObjectARB },
    { .name = "glDeleteOcclusionQueriesNV", .entry = (void *)&glDeleteOcclusionQueriesNV },
    { .name = "glDeletePathsNV", .entry = (void *)&glDeletePathsNV },
    { .name = "glDeletePerfMonitorsAMD", .entry = (void *)&glDeletePerfMonitorsAMD },
    { .name = "glDeletePerfQueryINTEL", .entry = (void *)&glDeletePerfQueryINTEL },
    { .name = "glDeleteProgram", .entry = (void *)&glDeleteProgram },
    { .name = "glDeleteProgramPipelines", .entry = (void *)&glDeleteProgramPipelines },
    { .name = "glDeleteProgramsARB", .entry = (void *)&glDeleteProgramsARB },
    { .name = "glDeleteProgramsNV", .entry = (void *)&glDeleteProgramsNV },
    { .name = "glDeleteQueries", .entry = (void *)&glDeleteQueries },
    { .name = "glDeleteQueriesARB", .entry = (void *)&glDeleteQueriesARB },
    { .name = "glDeleteQueryResourceTagNV", .entry = (void *)&glDeleteQueryResourceTagNV },
    { .name = "glDeleteRenderbuffers", .entry = (void *)&glDeleteRenderbuffers },
    { .name = "glDeleteRenderbuffersEXT", .entry = (void *)&glDeleteRenderbuffersEXT },
    { .name = "glDeleteSamplers", .entry = (void *)&glDeleteSamplers },
    { .name = "glDeleteSemaphoresEXT", .entry = (void *)&glDeleteSemaphoresEXT },
    { .name = "glDeleteShader", .entry = (void *)&glDeleteShader },
    { .name = "glDeleteStatesNV", .entry = (void *)&glDeleteStatesNV },
    { .name = "glDeleteSync", .entry = (void *)&glDeleteSync },
    { .name = "glDeleteTexturesEXT", .entry = (void *)&glDeleteTexturesEXT },
    { .name = "glDeleteTransformFeedbacks", .entry = (void *)&glDeleteTransformFeedbacks },
    { .name = "glDeleteTransformFeedbacksNV", .entry = (void *)&glDeleteTransformFeedbacksNV },
    { .name = "glDeleteVertexArrays", .entry = (void *)&glDeleteVertexArrays },
    { .name = "glDeleteVertexArraysAPPLE", .entry = (void *)&glDeleteVertexArraysAPPLE },
    { .name = "glDeleteVertexShaderEXT", .entry = (void *)&glDeleteVertexShaderEXT },
    { .name = "glDepthBoundsdNV", .entry = (void *)&glDepthBoundsdNV },
    { .name = "glDepthBoundsEXT", .entry = (void *)&glDepthBoundsEXT },
    { .name = "glDepthRangeArrayv", .entry = (void *)&glDepthRangeArrayv },
    { .name = "glDepthRangedNV", .entry = (void *)&glDepthRangedNV },
    { .name = "glDepthRangef", .entry = (void *)&glDepthRangef },
    { .name = "glDepthRangefOES", .entry = (void *)&glDepthRangefOES },
    { .name = "glDepthRangeIndexed", .entry = (void *)&glDepthRangeIndexed },
    { .name = "glDepthRangexOES", .entry = (void *)&glDepthRangexOES },
    { .name = "glDetachObjectARB", .entry = (void *)&glDetachObjectARB },
    { .name = "glDetachShader", .entry = (void *)&glDetachShader },
    { .name = "glDetailTexFuncSGIS", .entry = (void *)&glDetailTexFuncSGIS },
    { .name = "glDisableClientStateiEXT", .entry = (void *)&glDisableClientStateiEXT },
    { .name = "glDisableClientStateIndexedEXT", .entry = (void *)&glDisableClientStateIndexedEXT },
    { .name = "glDisablei", .entry = (void *)&glDisablei },
    { .name = "glDisableIndexedEXT", .entry = (void *)&glDisableIndexedEXT },
    { .name = "glDisableVariantClientStateEXT", .entry = (void *)&glDisableVariantClientStateEXT },
    { .name = "glDisableVertexArrayAttrib", .entry = (void *)&glDisableVertexArrayAttrib },
    { .name = "glDisableVertexArrayAttribEXT", .entry = (void *)&glDisableVertexArrayAttribEXT },
    { .name = "glDisableVertexArrayEXT", .entry = (void *)&glDisableVertexArrayEXT },
    { .name = "glDisableVertexAttribAPPLE", .entry = (void *)&glDisableVertexAttribAPPLE },
    { .name = "glDisableVertexAttribArray", .entry = (void *)&glDisableVertexAttribArray },
    { .name = "glDisableVertexAttribArrayARB", .entry = (void *)&glDisableVertexAttribArrayARB },
    { .name = "glDispatchCompute", .entry = (void *)&glDispatchCompute },
    { .name = "glDispatchComputeGroupSizeARB", .entry = (void *)&glDispatchComputeGroupSizeARB },
    { .name = "glDispatchComputeIndirect", .entry = (void *)&glDispatchComputeIndirect },
    { .name = "glDrawArraysEXT", .entry = (void *)&glDrawArraysEXT },
    { .name = "glDrawArraysIndirect", .entry = (void *)&glDrawArraysIndirect },
    { .name = "glDrawArraysInstanced", .entry = (void *)&glDrawArraysInstanced },
    { .name = "glDrawArraysInstancedARB", .entry = (void *)&glDrawArraysInstancedARB },
    { .name = "glDrawArraysInstancedBaseInstance", .entry = (void *)&glDrawArraysInstancedBaseInstance },
    { .name = "glDrawArraysInstancedEXT", .entry = (void *)&glDrawArraysInstancedEXT },
    { .name = "glDrawBuffers", .entry = (void *)&glDrawBuffers },
    { .name = "glDrawBuffersARB", .entry = (void *)&glDrawBuffersARB },
    { .name = "glDrawBuffersATI", .entry = (void *)&glDrawBuffersATI },
    { .name = "glDrawCommandsAddressNV", .entry = (void *)&glDrawCommandsAddressNV },
    { .name = "glDrawCommandsNV", .entry = (void *)&glDrawCommandsNV },
    { .name = "glDrawCommandsStatesAddressNV", .entry = (void *)&glDrawCommandsStatesAddressNV },
    { .name = "glDrawCommandsStatesNV", .entry = (void *)&glDrawCommandsStatesNV },
    { .name = "glDrawElementArrayAPPLE", .entry = (void *)&glDrawElementArrayAPPLE },
    { .name = "glDrawElementArrayATI", .entry = (void *)&glDrawElementArrayATI },
    { .name = "glDrawElementsBaseVertex", .entry = (void *)&glDrawElementsBaseVertex },
    { .name = "glDrawElementsIndirect", .entry = (void *)&glDrawElementsIndirect },
    { .name = "glDrawElementsInstanced", .entry = (void *)&glDrawElementsInstanced },
    { .name = "glDrawElementsInstancedARB", .entry = (void *)&glDrawElementsInstancedARB },
    { .name = "glDrawElementsInstancedBaseInstance", .entry = (void *)&glDrawElementsInstancedBaseInstance },
    { .name = "glDrawElementsInstancedBaseVertex", .entry = (void *)&glDrawElementsInstancedBaseVertex },
    { .name = "glDrawElementsInstancedBaseVertexBaseInstance", .entry = (void *)&glDrawElementsInstancedBaseVertexBaseInstance },
    { .name = "glDrawElementsInstancedEXT", .entry = (void *)&glDrawElementsInstancedEXT },
    { .name = "glDrawMeshArraysSUN", .entry = (void *)&glDrawMeshArraysSUN },
    { .name = "glDrawMeshTasksIndirectNV", .entry = (void *)&glDrawMeshTasksIndirectNV },
    { .name = "glDrawMeshTasksNV", .entry = (void *)&glDrawMeshTasksNV },
    { .name = "glDrawRangeElementArrayAPPLE", .entry = (void *)&glDrawRangeElementArrayAPPLE },
    { .name = "glDrawRangeElementArrayATI", .entry = (void *)&glDrawRangeElementArrayATI },
    { .name = "glDrawRangeElements", .entry = (void *)&glDrawRangeElements },
    { .name = "glDrawRangeElementsBaseVertex", .entry = (void *)&glDrawRangeElementsBaseVertex },
    { .name = "glDrawRangeElementsEXT", .entry = (void *)&glDrawRangeElementsEXT },
    { .name = "glDrawTextureNV", .entry = (void *)&glDrawTextureNV },
    { .name = "glDrawTransformFeedback", .entry = (void *)&glDrawTransformFeedback },
    { .name = "glDrawTransformFeedbackInstanced", .entry = (void *)&glDrawTransformFeedbackInstanced },
    { .name = "glDrawTransformFeedbackNV", .entry = (void *)&glDrawTransformFeedbackNV },
    { .name = "glDrawTransformFeedbackStream", .entry = (void *)&glDrawTransformFeedbackStream },
    { .name = "glDrawTransformFeedbackStreamInstanced", .entry = (void *)&glDrawTransformFeedbackStreamInstanced },
    { .name = "glDrawVkImageNV", .entry = (void *)&glDrawVkImageNV },
    { .name = "glEdgeFlagFormatNV", .entry = (void *)&glEdgeFlagFormatNV },
    { .name = "glEdgeFlagPointerEXT", .entry = (void *)&glEdgeFlagPointerEXT },
    { .name = "glEdgeFlagPointerListIBM", .entry = (void *)&glEdgeFlagPointerListIBM },
    { .name = "glEGLImageTargetTexStorageEXT", .entry = (void *)&glEGLImageTargetTexStorageEXT },
    { .name = "glEGLImageTargetTextureStorageEXT", .entry = (void *)&glEGLImageTargetTextureStorageEXT },
    { .name = "glElementPointerAPPLE", .entry = (void *)&glElementPointerAPPLE },
    { .name = "glElementPointerATI", .entry = (void *)&glElementPointerATI },
    { .name = "glEnableClientStateiEXT", .entry = (void *)&glEnableClientStateiEXT },
    { .name = "glEnableClientStateIndexedEXT", .entry = (void *)&glEnableClientStateIndexedEXT },
    { .name = "glEnablei", .entry = (void *)&glEnablei },
    { .name = "glEnableIndexedEXT", .entry = (void *)&glEnableIndexedEXT },
    { .name = "glEnableVariantClientStateEXT", .entry = (void *)&glEnableVariantClientStateEXT },
    { .name = "glEnableVertexArrayAttrib", .entry = (void *)&glEnableVertexArrayAttrib },
    { .name = "glEnableVertexArrayAttribEXT", .entry = (void *)&glEnableVertexArrayAttribEXT },
    { .name = "glEnableVertexArrayEXT", .entry = (void *)&glEnableVertexArrayEXT },
    { .name = "glEnableVertexAttribAPPLE", .entry = (void *)&glEnableVertexAttribAPPLE },
    { .name = "glEnableVertexAttribArray", .entry = (void *)&glEnableVertexAttribArray },
    { .name = "glEnableVertexAttribArrayARB", .entry = (void *)&glEnableVertexAttribArrayARB },
    { .name = "glEndConditionalRender", .entry = (void *)&glEndConditionalRender },
    { .name = "glEndConditionalRenderNV", .entry = (void *)&glEndConditionalRenderNV },
    { .name = "glEndConditionalRenderNVX", .entry = (void *)&glEndConditionalRenderNVX },
    { .name = "glEndFragmentShaderATI", .entry = (void *)&glEndFragmentShaderATI },
    { .name = "glEndOcclusionQueryNV", .entry = (void *)&glEndOcclusionQueryNV },
    { .name = "glEndPerfMonitorAMD", .entry = (void *)&glEndPerfMonitorAMD },
    { .name = "glEndPerfQueryINTEL", .entry = (void *)&glEndPerfQueryINTEL },
    { .name = "glEndQuery", .entry = (void *)&glEndQuery },
    { .name = "glEndQueryARB", .entry = (void *)&glEndQueryARB },
    { .name = "glEndQueryIndexed", .entry = (void *)&glEndQueryIndexed },
    { .name = "glEndTransformFeedback", .entry = (void *)&glEndTransformFeedback },
    { .name = "glEndTransformFeedbackEXT", .entry = (void *)&glEndTransformFeedbackEXT },
    { .name = "glEndTransformFeedbackNV", .entry = (void *)&glEndTransformFeedbackNV },
    { .name = "glEndVertexShaderEXT", .entry = (void *)&glEndVertexShaderEXT },
    { .name = "glEndVideoCaptureNV", .entry = (void *)&glEndVideoCaptureNV },
    { .name = "glEvalCoord1xOES", .entry = (void *)&glEvalCoord1xOES },
    { .name = "glEvalCoord1xvOES", .entry = (void *)&glEvalCoord1xvOES },
    { .name = "glEvalCoord2xOES", .entry = (void *)&glEvalCoord2xOES },
    { .name = "glEvalCoord2xvOES", .entry = (void *)&glEvalCoord2xvOES },
    { .name = "glEvalMapsNV", .entry = (void *)&glEvalMapsNV },
    { .name = "glEvaluateDepthValuesARB", .entry = (void *)&glEvaluateDepthValuesARB },
    { .name = "glExecuteProgramNV", .entry = (void *)&glExecuteProgramNV },
    { .name = "glExtractComponentEXT", .entry = (void *)&glExtractComponentEXT },
    { .name = "glFeedbackBufferxOES", .entry = (void *)&glFeedbackBufferxOES },
    { .name = "glFenceSync", .entry = (void *)&glFenceSync },
    { .name = "glFinalCombinerInputNV", .entry = (void *)&glFinalCombinerInputNV },
    { .name = "glFinishAsyncSGIX", .entry = (void *)&glFinishAsyncSGIX },
    { .name = "glFinishFenceAPPLE", .entry = (void *)&glFinishFenceAPPLE },
    { .name = "glFinishFenceNV", .entry = (void *)&glFinishFenceNV },
    { .name = "glFinishObjectAPPLE", .entry = (void *)&glFinishObjectAPPLE },
    { .name = "glFinishTextureSUNX", .entry = (void *)&glFinishTextureSUNX },
    { .name = "glFlushMappedBufferRange", .entry = (void *)&glFlushMappedBufferRange },
    { .name = "glFlushMappedBufferRangeAPPLE", .entry = (void *)&glFlushMappedBufferRangeAPPLE },
    { .name = "glFlushMappedNamedBufferRange", .entry = (void *)&glFlushMappedNamedBufferRange },
    { .name = "glFlushMappedNamedBufferRangeEXT", .entry = (void *)&glFlushMappedNamedBufferRangeEXT },
    { .name = "glFlushPixelDataRangeNV", .entry = (void *)&glFlushPixelDataRangeNV },
    { .name = "glFlushRasterSGIX", .entry = (void *)&glFlushRasterSGIX },
    { .name = "glFlushStaticDataIBM", .entry = (void *)&glFlushStaticDataIBM },
    { .name = "glFlushVertexArrayRangeAPPLE", .entry = (void *)&glFlushVertexArrayRangeAPPLE },
    { .name = "glFlushVertexArrayRangeNV", .entry = (void *)&glFlushVertexArrayRangeNV },
    { .name = "glFogCoordd", .entry = (void *)&glFogCoordd },
    { .name = "glFogCoorddEXT", .entry = (void *)&glFogCoorddEXT },
    { .name = "glFogCoorddv", .entry = (void *)&glFogCoorddv },
    { .name = "glFogCoorddvEXT", .entry = (void *)&glFogCoorddvEXT },
    { .name = "glFogCoordf", .entry = (void *)&glFogCoordf },
    { .name = "glFogCoordfEXT", .entry = (void *)&glFogCoordfEXT },
    { .name = "glFogCoordFormatNV", .entry = (void *)&glFogCoordFormatNV },
    { .name = "glFogCoordfv", .entry = (void *)&glFogCoordfv },
    { .name = "glFogCoordfvEXT", .entry = (void *)&glFogCoordfvEXT },
    { .name = "glFogCoordhNV", .entry = (void *)&glFogCoordhNV },
    { .name = "glFogCoordhvNV", .entry = (void *)&glFogCoordhvNV },
    { .name = "glFogCoordPointer", .entry = (void *)&glFogCoordPointer },
    { .name = "glFogCoordPointerEXT", .entry = (void *)&glFogCoordPointerEXT },
    { .name = "glFogCoordPointerListIBM", .entry = (void *)&glFogCoordPointerListIBM },
    { .name = "glFogFuncSGIS", .entry = (void *)&glFogFuncSGIS },
    { .name = "glFogxOES", .entry = (void *)&glFogxOES },
    { .name = "glFogxvOES", .entry = (void *)&glFogxvOES },
    { .name = "glFragmentColorMaterialSGIX", .entry = (void *)&glFragmentColorMaterialSGIX },
    { .name = "glFragmentCoverageColorNV", .entry = (void *)&glFragmentCoverageColorNV },
    { .name = "glFragmentLightfSGIX", .entry = (void *)&glFragmentLightfSGIX },
    { .name = "glFragmentLightfvSGIX", .entry = (void *)&glFragmentLightfvSGIX },
    { .name = "glFragmentLightiSGIX", .entry = (void *)&glFragmentLightiSGIX },
    { .name = "glFragmentLightivSGIX", .entry = (void *)&glFragmentLightivSGIX },
    { .name = "glFragmentLightModelfSGIX", .entry = (void *)&glFragmentLightModelfSGIX },
    { .name = "glFragmentLightModelfvSGIX", .entry = (void *)&glFragmentLightModelfvSGIX },
    { .name = "glFragmentLightModeliSGIX", .entry = (void *)&glFragmentLightModeliSGIX },
    { .name = "glFragmentLightModelivSGIX", .entry = (void *)&glFragmentLightModelivSGIX },
    { .name = "glFragmentMaterialfSGIX", .entry = (void *)&glFragmentMaterialfSGIX },
    { .name = "glFragmentMaterialfvSGIX", .entry = (void *)&glFragmentMaterialfvSGIX },
    { .name = "glFragmentMaterialiSGIX", .entry = (void *)&glFragmentMaterialiSGIX },
    { .name = "glFragmentMaterialivSGIX", .entry = (void *)&glFragmentMaterialivSGIX },
    { .name = "glFramebufferDrawBufferEXT", .entry = (void *)&glFramebufferDrawBufferEXT },
    { .name = "glFramebufferDrawBuffersEXT", .entry = (void *)&glFramebufferDrawBuffersEXT },
    { .name = "glFramebufferFetchBarrierEXT", .entry = (void *)&glFramebufferFetchBarrierEXT },
    { .name = "glFramebufferParameteri", .entry = (void *)&glFramebufferParameteri },
    { .name = "glFramebufferReadBufferEXT", .entry = (void *)&glFramebufferReadBufferEXT },
    { .name = "glFramebufferRenderbuffer", .entry = (void *)&glFramebufferRenderbuffer },
    { .name = "glFramebufferRenderbufferEXT", .entry = (void *)&glFramebufferRenderbufferEXT },
    { .name = "glFramebufferSampleLocationsfvARB", .entry = (void *)&glFramebufferSampleLocationsfvARB },
    { .name = "glFramebufferSampleLocationsfvNV", .entry = (void *)&glFramebufferSampleLocationsfvNV },
    { .name = "glFramebufferSamplePositionsfvAMD", .entry = (void *)&glFramebufferSamplePositionsfvAMD },
    { .name = "glFramebufferTexture", .entry = (void *)&glFramebufferTexture },
    { .name = "glFramebufferTexture1D", .entry = (void *)&glFramebufferTexture1D },
    { .name = "glFramebufferTexture1DEXT", .entry = (void *)&glFramebufferTexture1DEXT },
    { .name = "glFramebufferTexture2D", .entry = (void *)&glFramebufferTexture2D },
    { .name = "glFramebufferTexture2DEXT", .entry = (void *)&glFramebufferTexture2DEXT },
    { .name = "glFramebufferTexture3D", .entry = (void *)&glFramebufferTexture3D },
    { .name = "glFramebufferTexture3DEXT", .entry = (void *)&glFramebufferTexture3DEXT },
    { .name = "glFramebufferTextureARB", .entry = (void *)&glFramebufferTextureARB },
    { .name = "glFramebufferTextureEXT", .entry = (void *)&glFramebufferTextureEXT },
    { .name = "glFramebufferTextureFaceARB", .entry = (void *)&glFramebufferTextureFaceARB },
    { .name = "glFramebufferTextureFaceEXT", .entry = (void *)&glFramebufferTextureFaceEXT },
    { .name = "glFramebufferTextureLayer", .entry = (void *)&glFramebufferTextureLayer },
    { .name = "glFramebufferTextureLayerARB", .entry = (void *)&glFramebufferTextureLayerARB },
    { .name = "glFramebufferTextureLayerEXT", .entry = (void *)&glFramebufferTextureLayerEXT },
    { .name = "glFramebufferTextureMultiviewOVR", .entry = (void *)&glFramebufferTextureMultiviewOVR },
    { .name = "glFrameTerminatorGREMEDY", .entry = (void *)&glFrameTerminatorGREMEDY },
    { .name = "glFrameZoomSGIX", .entry = (void *)&glFrameZoomSGIX },
    { .name = "glFreeObjectBufferATI", .entry = (void *)&glFreeObjectBufferATI },
    { .name = "glFrustumfOES", .entry = (void *)&glFrustumfOES },
    { .name = "glFrustumxOES", .entry = (void *)&glFrustumxOES },
    { .name = "glGenAsyncMarkersSGIX", .entry = (void *)&glGenAsyncMarkersSGIX },
    { .name = "glGenBuffers", .entry = (void *)&glGenBuffers },
    { .name = "glGenBuffersARB", .entry = (void *)&glGenBuffersARB },
    { .name = "glGenerateMipmap", .entry = (void *)&glGenerateMipmap },
    { .name = "glGenerateMipmapEXT", .entry = (void *)&glGenerateMipmapEXT },
    { .name = "glGenerateMultiTexMipmapEXT", .entry = (void *)&glGenerateMultiTexMipmapEXT },
    { .name = "glGenerateTextureMipmap", .entry = (void *)&glGenerateTextureMipmap },
    { .name = "glGenerateTextureMipmapEXT", .entry = (void *)&glGenerateTextureMipmapEXT },
    { .name = "glGenFencesAPPLE", .entry = (void *)&glGenFencesAPPLE },
    { .name = "glGenFencesNV", .entry = (void *)&glGenFencesNV },
    { .name = "glGenFragmentShadersATI", .entry = (void *)&glGenFragmentShadersATI },
    { .name = "glGenFramebuffers", .entry = (void *)&glGenFramebuffers },
    { .name = "glGenFramebuffersEXT", .entry = (void *)&glGenFramebuffersEXT },
    { .name = "glGenNamesAMD", .entry = (void *)&glGenNamesAMD },
    { .name = "glGenOcclusionQueriesNV", .entry = (void *)&glGenOcclusionQueriesNV },
    { .name = "glGenPathsNV", .entry = (void *)&glGenPathsNV },
    { .name = "glGenPerfMonitorsAMD", .entry = (void *)&glGenPerfMonitorsAMD },
    { .name = "glGenProgramPipelines", .entry = (void *)&glGenProgramPipelines },
    { .name = "glGenProgramsARB", .entry = (void *)&glGenProgramsARB },
    { .name = "glGenProgramsNV", .entry = (void *)&glGenProgramsNV },
    { .name = "glGenQueries", .entry = (void *)&glGenQueries },
    { .name = "glGenQueriesARB", .entry = (void *)&glGenQueriesARB },
    { .name = "glGenQueryResourceTagNV", .entry = (void *)&glGenQueryResourceTagNV },
    { .name = "glGenRenderbuffers", .entry = (void *)&glGenRenderbuffers },
    { .name = "glGenRenderbuffersEXT", .entry = (void *)&glGenRenderbuffersEXT },
    { .name = "glGenSamplers", .entry = (void *)&glGenSamplers },
    { .name = "glGenSemaphoresEXT", .entry = (void *)&glGenSemaphoresEXT },
    { .name = "glGenSymbolsEXT", .entry = (void *)&glGenSymbolsEXT },
    { .name = "glGenTexturesEXT", .entry = (void *)&glGenTexturesEXT },
    { .name = "glGenTransformFeedbacks", .entry = (void *)&glGenTransformFeedbacks },
    { .name = "glGenTransformFeedbacksNV", .entry = (void *)&glGenTransformFeedbacksNV },
    { .name = "glGenVertexArrays", .entry = (void *)&glGenVertexArrays },
    { .name = "glGenVertexArraysAPPLE", .entry = (void *)&glGenVertexArraysAPPLE },
    { .name = "glGenVertexShadersEXT", .entry = (void *)&glGenVertexShadersEXT },
    { .name = "glGetActiveAtomicCounterBufferiv", .entry = (void *)&glGetActiveAtomicCounterBufferiv },
    { .name = "glGetActiveAttrib", .entry = (void *)&glGetActiveAttrib },
    { .name = "glGetActiveAttribARB", .entry = (void *)&glGetActiveAttribARB },
    { .name = "glGetActiveSubroutineName", .entry = (void *)&glGetActiveSubroutineName },
    { .name = "glGetActiveSubroutineUniformiv", .entry = (void *)&glGetActiveSubroutineUniformiv },
    { .name = "glGetActiveSubroutineUniformName", .entry = (void *)&glGetActiveSubroutineUniformName },
    { .name = "glGetActiveUniform", .entry = (void *)&glGetActiveUniform },
    { .name = "glGetActiveUniformARB", .entry = (void *)&glGetActiveUniformARB },
    { .name = "glGetActiveUniformBlockiv", .entry = (void *)&glGetActiveUniformBlockiv },
    { .name = "glGetActiveUniformBlockName", .entry = (void *)&glGetActiveUniformBlockName },
    { .name = "glGetActiveUniformName", .entry = (void *)&glGetActiveUniformName },
    { .name = "glGetActiveUniformsiv", .entry = (void *)&glGetActiveUniformsiv },
    { .name = "glGetActiveVaryingNV", .entry = (void *)&glGetActiveVaryingNV },
    { .name = "glGetArrayObjectfvATI", .entry = (void *)&glGetArrayObjectfvATI },
    { .name = "glGetArrayObjectivATI", .entry = (void *)&glGetArrayObjectivATI },
    { .name = "glGetAttachedObjectsARB", .entry = (void *)&glGetAttachedObjectsARB },
    { .name = "glGetAttachedShaders", .entry = (void *)&glGetAttachedShaders },
    { .name = "glGetAttribLocation", .entry = (void *)&glGetAttribLocation },
    { .name = "glGetAttribLocationARB", .entry = (void *)&glGetAttribLocationARB },
    { .name = "glGetBooleani_v", .entry = (void *)&glGetBooleani_v },
    { .name = "glGetBooleanIndexedvEXT", .entry = (void *)&glGetBooleanIndexedvEXT },
    { .name = "glGetBufferParameteri64v", .entry = (void *)&glGetBufferParameteri64v },
    { .name = "glGetBufferParameteriv", .entry = (void *)&glGetBufferParameteriv },
    { .name = "glGetBufferParameterivARB", .entry = (void *)&glGetBufferParameterivARB },
    { .name = "glGetBufferParameterui64vNV", .entry = (void *)&glGetBufferParameterui64vNV },
    { .name = "glGetBufferPointerv", .entry = (void *)&glGetBufferPointerv },
    { .name = "glGetBufferPointervARB", .entry = (void *)&glGetBufferPointervARB },
    { .name = "glGetBufferSubData", .entry = (void *)&glGetBufferSubData },
    { .name = "glGetBufferSubDataARB", .entry = (void *)&glGetBufferSubDataARB },
    { .name = "glGetClipPlanefOES", .entry = (void *)&glGetClipPlanefOES },
    { .name = "glGetClipPlanexOES", .entry = (void *)&glGetClipPlanexOES },
    { .name = "glGetColorTable", .entry = (void *)&glGetColorTable },
    { .name = "glGetColorTableEXT", .entry = (void *)&glGetColorTableEXT },
    { .name = "glGetColorTableParameterfv", .entry = (void *)&glGetColorTableParameterfv },
    { .name = "glGetColorTableParameterfvEXT", .entry = (void *)&glGetColorTableParameterfvEXT },
    { .name = "glGetColorTableParameterfvSGI", .entry = (void *)&glGetColorTableParameterfvSGI },
    { .name = "glGetColorTableParameteriv", .entry = (void *)&glGetColorTableParameteriv },
    { .name = "glGetColorTableParameterivEXT", .entry = (void *)&glGetColorTableParameterivEXT },
    { .name = "glGetColorTableParameterivSGI", .entry = (void *)&glGetColorTableParameterivSGI },
    { .name = "glGetColorTableSGI", .entry = (void *)&glGetColorTableSGI },
    { .name = "glGetCombinerInputParameterfvNV", .entry = (void *)&glGetCombinerInputParameterfvNV },
    { .name = "glGetCombinerInputParameterivNV", .entry = (void *)&glGetCombinerInputParameterivNV },
    { .name = "glGetCombinerOutputParameterfvNV", .entry = (void *)&glGetCombinerOutputParameterfvNV },
    { .name = "glGetCombinerOutputParameterivNV", .entry = (void *)&glGetCombinerOutputParameterivNV },
    { .name = "glGetCombinerStageParameterfvNV", .entry = (void *)&glGetCombinerStageParameterfvNV },
    { .name = "glGetCommandHeaderNV", .entry = (void *)&glGetCommandHeaderNV },
    { .name = "glGetCompressedMultiTexImageEXT", .entry = (void *)&glGetCompressedMultiTexImageEXT },
    { .name = "glGetCompressedTexImage", .entry = (void *)&glGetCompressedTexImage },
    { .name = "glGetCompressedTexImageARB", .entry = (void *)&glGetCompressedTexImageARB },
    { .name = "glGetCompressedTextureImage", .entry = (void *)&glGetCompressedTextureImage },
    { .name = "glGetCompressedTextureImageEXT", .entry = (void *)&glGetCompressedTextureImageEXT },
    { .name = "glGetCompressedTextureSubImage", .entry = (void *)&glGetCompressedTextureSubImage },
    { .name = "glGetConvolutionFilter", .entry = (void *)&glGetConvolutionFilter },
    { .name = "glGetConvolutionFilterEXT", .entry = (void *)&glGetConvolutionFilterEXT },
    { .name = "glGetConvolutionParameterfv", .entry = (void *)&glGetConvolutionParameterfv },
    { .name = "glGetConvolutionParameterfvEXT", .entry = (void *)&glGetConvolutionParameterfvEXT },
    { .name = "glGetConvolutionParameteriv", .entry = (void *)&glGetConvolutionParameteriv },
    { .name = "glGetConvolutionParameterivEXT", .entry = (void *)&glGetConvolutionParameterivEXT },
    { .name = "glGetConvolutionParameterxvOES", .entry = (void *)&glGetConvolutionParameterxvOES },
    { .name = "glGetCoverageModulationTableNV", .entry = (void *)&glGetCoverageModulationTableNV },
    { .name = "glGetDebugMessageLog", .entry = (void *)&glGetDebugMessageLog },
    { .name = "glGetDebugMessageLogAMD", .entry = (void *)&glGetDebugMessageLogAMD },
    { .name = "glGetDebugMessageLogARB", .entry = (void *)&glGetDebugMessageLogARB },
    { .name = "glGetDetailTexFuncSGIS", .entry = (void *)&glGetDetailTexFuncSGIS },
    { .name = "glGetDoublei_v", .entry = (void *)&glGetDoublei_v },
    { .name = "glGetDoublei_vEXT", .entry = (void *)&glGetDoublei_vEXT },
    { .name = "glGetDoubleIndexedvEXT", .entry = (void *)&glGetDoubleIndexedvEXT },
    { .name = "glGetFenceivNV", .entry = (void *)&glGetFenceivNV },
    { .name = "glGetFinalCombinerInputParameterfvNV", .entry = (void *)&glGetFinalCombinerInputParameterfvNV },
    { .name = "glGetFinalCombinerInputParameterivNV", .entry = (void *)&glGetFinalCombinerInputParameterivNV },
    { .name = "glGetFirstPerfQueryIdINTEL", .entry = (void *)&glGetFirstPerfQueryIdINTEL },
    { .name = "glGetFixedvOES", .entry = (void *)&glGetFixedvOES },
    { .name = "glGetFloati_v", .entry = (void *)&glGetFloati_v },
    { .name = "glGetFloati_vEXT", .entry = (void *)&glGetFloati_vEXT },
    { .name = "glGetFloatIndexedvEXT", .entry = (void *)&glGetFloatIndexedvEXT },
    { .name = "glGetFogFuncSGIS", .entry = (void *)&glGetFogFuncSGIS },
    { .name = "glGetFragDataIndex", .entry = (void *)&glGetFragDataIndex },
    { .name = "glGetFragDataLocation", .entry = (void *)&glGetFragDataLocation },
    { .name = "glGetFragDataLocationEXT", .entry = (void *)&glGetFragDataLocationEXT },
    { .name = "glGetFragmentLightfvSGIX", .entry = (void *)&glGetFragmentLightfvSGIX },
    { .name = "glGetFragmentLightivSGIX", .entry = (void *)&glGetFragmentLightivSGIX },
    { .name = "glGetFragmentMaterialfvSGIX", .entry = (void *)&glGetFragmentMaterialfvSGIX },
    { .name = "glGetFragmentMaterialivSGIX", .entry = (void *)&glGetFragmentMaterialivSGIX },
    { .name = "glGetFramebufferAttachmentParameteriv", .entry = (void *)&glGetFramebufferAttachmentParameteriv },
    { .name = "glGetFramebufferAttachmentParameterivEXT", .entry = (void *)&glGetFramebufferAttachmentParameterivEXT },
    { .name = "glGetFramebufferParameterfvAMD", .entry = (void *)&glGetFramebufferParameterfvAMD },
    { .name = "glGetFramebufferParameteriv", .entry = (void *)&glGetFramebufferParameteriv },
    { .name = "glGetFramebufferParameterivEXT", .entry = (void *)&glGetFramebufferParameterivEXT },
    { .name = "glGetGraphicsResetStatus", .entry = (void *)&glGetGraphicsResetStatus },
    { .name = "glGetGraphicsResetStatusARB", .entry = (void *)&glGetGraphicsResetStatusARB },
    { .name = "glGetHandleARB", .entry = (void *)&glGetHandleARB },
    { .name = "glGetHistogram", .entry = (void *)&glGetHistogram },
    { .name = "glGetHistogramEXT", .entry = (void *)&glGetHistogramEXT },
    { .name = "glGetHistogramParameterfv", .entry = (void *)&glGetHistogramParameterfv },
    { .name = "glGetHistogramParameterfvEXT", .entry = (void *)&glGetHistogramParameterfvEXT },
    { .name = "glGetHistogramParameteriv", .entry = (void *)&glGetHistogramParameteriv },
    { .name = "glGetHistogramParameterivEXT", .entry = (void *)&glGetHistogramParameterivEXT },
    { .name = "glGetHistogramParameterxvOES", .entry = (void *)&glGetHistogramParameterxvOES },
    { .name = "glGetImageHandleARB", .entry = (void *)&glGetImageHandleARB },
    { .name = "glGetImageHandleNV", .entry = (void *)&glGetImageHandleNV },
    { .name = "glGetImageTransformParameterfvHP", .entry = (void *)&glGetImageTransformParameterfvHP },
    { .name = "glGetImageTransformParameterivHP", .entry = (void *)&glGetImageTransformParameterivHP },
    { .name = "glGetInfoLogARB", .entry = (void *)&glGetInfoLogARB },
    { .name = "glGetInstrumentsSGIX", .entry = (void *)&glGetInstrumentsSGIX },
    { .name = "glGetInteger64i_v", .entry = (void *)&glGetInteger64i_v },
    { .name = "glGetInteger64v", .entry = (void *)&glGetInteger64v },
    { .name = "glGetIntegeri_v", .entry = (void *)&glGetIntegeri_v },
    { .name = "glGetIntegerIndexedvEXT", .entry = (void *)&glGetIntegerIndexedvEXT },
    { .name = "glGetIntegerui64i_vNV", .entry = (void *)&glGetIntegerui64i_vNV },
    { .name = "glGetIntegerui64vNV", .entry = (void *)&glGetIntegerui64vNV },
    { .name = "glGetInternalformati64v", .entry = (void *)&glGetInternalformati64v },
    { .name = "glGetInternalformativ", .entry = (void *)&glGetInternalformativ },
    { .name = "glGetInternalformatSampleivNV", .entry = (void *)&glGetInternalformatSampleivNV },
    { .name = "glGetInvariantBooleanvEXT", .entry = (void *)&glGetInvariantBooleanvEXT },
    { .name = "glGetInvariantFloatvEXT", .entry = (void *)&glGetInvariantFloatvEXT },
    { .name = "glGetInvariantIntegervEXT", .entry = (void *)&glGetInvariantIntegervEXT },
    { .name = "glGetLightxOES", .entry = (void *)&glGetLightxOES },
    { .name = "glGetListParameterfvSGIX", .entry = (void *)&glGetListParameterfvSGIX },
    { .name = "glGetListParameterivSGIX", .entry = (void *)&glGetListParameterivSGIX },
    { .name = "glGetLocalConstantBooleanvEXT", .entry = (void *)&glGetLocalConstantBooleanvEXT },
    { .name = "glGetLocalConstantFloatvEXT", .entry = (void *)&glGetLocalConstantFloatvEXT },
    { .name = "glGetLocalConstantIntegervEXT", .entry = (void *)&glGetLocalConstantIntegervEXT },
    { .name = "glGetMapAttribParameterfvNV", .entry = (void *)&glGetMapAttribParameterfvNV },
    { .name = "glGetMapAttribParameterivNV", .entry = (void *)&glGetMapAttribParameterivNV },
    { .name = "glGetMapControlPointsNV", .entry = (void *)&glGetMapControlPointsNV },
    { .name = "glGetMapParameterfvNV", .entry = (void *)&glGetMapParameterfvNV },
    { .name = "glGetMapParameterivNV", .entry = (void *)&glGetMapParameterivNV },
    { .name = "glGetMapxvOES", .entry = (void *)&glGetMapxvOES },
    { .name = "glGetMaterialxOES", .entry = (void *)&glGetMaterialxOES },
    { .name = "glGetMemoryObjectDetachedResourcesuivNV", .entry = (void *)&glGetMemoryObjectDetachedResourcesuivNV },
    { .name = "glGetMemoryObjectParameterivEXT", .entry = (void *)&glGetMemoryObjectParameterivEXT },
    { .name = "glGetMinmax", .entry = (void *)&glGetMinmax },
    { .name = "glGetMinmaxEXT", .entry = (void *)&glGetMinmaxEXT },
    { .name = "glGetMinmaxParameterfv", .entry = (void *)&glGetMinmaxParameterfv },
    { .name = "glGetMinmaxParameterfvEXT", .entry = (void *)&glGetMinmaxParameterfvEXT },
    { .name = "glGetMinmaxParameteriv", .entry = (void *)&glGetMinmaxParameteriv },
    { .name = "glGetMinmaxParameterivEXT", .entry = (void *)&glGetMinmaxParameterivEXT },
    { .name = "glGetMultisamplefv", .entry = (void *)&glGetMultisamplefv },
    { .name = "glGetMultisamplefvNV", .entry = (void *)&glGetMultisamplefvNV },
    { .name = "glGetMultiTexEnvfvEXT", .entry = (void *)&glGetMultiTexEnvfvEXT },
    { .name = "glGetMultiTexEnvivEXT", .entry = (void *)&glGetMultiTexEnvivEXT },
    { .name = "glGetMultiTexGendvEXT", .entry = (void *)&glGetMultiTexGendvEXT },
    { .name = "glGetMultiTexGenfvEXT", .entry = (void *)&glGetMultiTexGenfvEXT },
    { .name = "glGetMultiTexGenivEXT", .entry = (void *)&glGetMultiTexGenivEXT },
    { .name = "glGetMultiTexImageEXT", .entry = (void *)&glGetMultiTexImageEXT },
    { .name = "glGetMultiTexLevelParameterfvEXT", .entry = (void *)&glGetMultiTexLevelParameterfvEXT },
    { .name = "glGetMultiTexLevelParameterivEXT", .entry = (void *)&glGetMultiTexLevelParameterivEXT },
    { .name = "glGetMultiTexParameterfvEXT", .entry = (void *)&glGetMultiTexParameterfvEXT },
    { .name = "glGetMultiTexParameterIivEXT", .entry = (void *)&glGetMultiTexParameterIivEXT },
    { .name = "glGetMultiTexParameterIuivEXT", .entry = (void *)&glGetMultiTexParameterIuivEXT },
    { .name = "glGetMultiTexParameterivEXT", .entry = (void *)&glGetMultiTexParameterivEXT },
    { .name = "glGetNamedBufferParameteri64v", .entry = (void *)&glGetNamedBufferParameteri64v },
    { .name = "glGetNamedBufferParameteriv", .entry = (void *)&glGetNamedBufferParameteriv },
    { .name = "glGetNamedBufferParameterivEXT", .entry = (void *)&glGetNamedBufferParameterivEXT },
    { .name = "glGetNamedBufferParameterui64vNV", .entry = (void *)&glGetNamedBufferParameterui64vNV },
    { .name = "glGetNamedBufferPointerv", .entry = (void *)&glGetNamedBufferPointerv },
    { .name = "glGetNamedBufferPointervEXT", .entry = (void *)&glGetNamedBufferPointervEXT },
    { .name = "glGetNamedBufferSubData", .entry = (void *)&glGetNamedBufferSubData },
    { .name = "glGetNamedBufferSubDataEXT", .entry = (void *)&glGetNamedBufferSubDataEXT },
    { .name = "glGetNamedFramebufferAttachmentParameteriv", .entry = (void *)&glGetNamedFramebufferAttachmentParameteriv },
    { .name = "glGetNamedFramebufferAttachmentParameterivEXT", .entry = (void *)&glGetNamedFramebufferAttachmentParameterivEXT },
    { .name = "glGetNamedFramebufferParameterfvAMD", .entry = (void *)&glGetNamedFramebufferParameterfvAMD },
    { .name = "glGetNamedFramebufferParameteriv", .entry = (void *)&glGetNamedFramebufferParameteriv },
    { .name = "glGetNamedFramebufferParameterivEXT", .entry = (void *)&glGetNamedFramebufferParameterivEXT },
    { .name = "glGetNamedProgramivEXT", .entry = (void *)&glGetNamedProgramivEXT },
    { .name = "glGetNamedProgramLocalParameterdvEXT", .entry = (void *)&glGetNamedProgramLocalParameterdvEXT },
    { .name = "glGetNamedProgramLocalParameterfvEXT", .entry = (void *)&glGetNamedProgramLocalParameterfvEXT },
    { .name = "glGetNamedProgramLocalParameterIivEXT", .entry = (void *)&glGetNamedProgramLocalParameterIivEXT },
    { .name = "glGetNamedProgramLocalParameterIuivEXT", .entry = (void *)&glGetNamedProgramLocalParameterIuivEXT },
    { .name = "glGetNamedProgramStringEXT", .entry = (void *)&glGetNamedProgramStringEXT },
    { .name = "glGetNamedRenderbufferParameteriv", .entry = (void *)&glGetNamedRenderbufferParameteriv },
    { .name = "glGetNamedRenderbufferParameterivEXT", .entry = (void *)&glGetNamedRenderbufferParameterivEXT },
    { .name = "glGetNamedStringARB", .entry = (void *)&glGetNamedStringARB },
    { .name = "glGetNamedStringivARB", .entry = (void *)&glGetNamedStringivARB },
    { .name = "glGetnColorTable", .entry = (void *)&glGetnColorTable },
    { .name = "glGetnColorTableARB", .entry = (void *)&glGetnColorTableARB },
    { .name = "glGetnCompressedTexImage", .entry = (void *)&glGetnCompressedTexImage },
    { .name = "glGetnCompressedTexImageARB", .entry = (void *)&glGetnCompressedTexImageARB },
    { .name = "glGetnConvolutionFilter", .entry = (void *)&glGetnConvolutionFilter },
    { .name = "glGetnConvolutionFilterARB", .entry = (void *)&glGetnConvolutionFilterARB },
    { .name = "glGetNextPerfQueryIdINTEL", .entry = (void *)&glGetNextPerfQueryIdINTEL },
    { .name = "glGetnHistogram", .entry = (void *)&glGetnHistogram },
    { .name = "glGetnHistogramARB", .entry = (void *)&glGetnHistogramARB },
    { .name = "glGetnMapdv", .entry = (void *)&glGetnMapdv },
    { .name = "glGetnMapdvARB", .entry = (void *)&glGetnMapdvARB },
    { .name = "glGetnMapfv", .entry = (void *)&glGetnMapfv },
    { .name = "glGetnMapfvARB", .entry = (void *)&glGetnMapfvARB },
    { .name = "glGetnMapiv", .entry = (void *)&glGetnMapiv },
    { .name = "glGetnMapivARB", .entry = (void *)&glGetnMapivARB },
    { .name = "glGetnMinmax", .entry = (void *)&glGetnMinmax },
    { .name = "glGetnMinmaxARB", .entry = (void *)&glGetnMinmaxARB },
    { .name = "glGetnPixelMapfv", .entry = (void *)&glGetnPixelMapfv },
    { .name = "glGetnPixelMapfvARB", .entry = (void *)&glGetnPixelMapfvARB },
    { .name = "glGetnPixelMapuiv", .entry = (void *)&glGetnPixelMapuiv },
    { .name = "glGetnPixelMapuivARB", .entry = (void *)&glGetnPixelMapuivARB },
    { .name = "glGetnPixelMapusv", .entry = (void *)&glGetnPixelMapusv },
    { .name = "glGetnPixelMapusvARB", .entry = (void *)&glGetnPixelMapusvARB },
    { .name = "glGetnPolygonStipple", .entry = (void *)&glGetnPolygonStipple },
    { .name = "glGetnPolygonStippleARB", .entry = (void *)&glGetnPolygonStippleARB },
    { .name = "glGetnSeparableFilter", .entry = (void *)&glGetnSeparableFilter },
    { .name = "glGetnSeparableFilterARB", .entry = (void *)&glGetnSeparableFilterARB },
    { .name = "glGetnTexImage", .entry = (void *)&glGetnTexImage },
    { .name = "glGetnTexImageARB", .entry = (void *)&glGetnTexImageARB },
    { .name = "glGetnUniformdv", .entry = (void *)&glGetnUniformdv },
    { .name = "glGetnUniformdvARB", .entry = (void *)&glGetnUniformdvARB },
    { .name = "glGetnUniformfv", .entry = (void *)&glGetnUniformfv },
    { .name = "glGetnUniformfvARB", .entry = (void *)&glGetnUniformfvARB },
    { .name = "glGetnUniformi64vARB", .entry = (void *)&glGetnUniformi64vARB },
    { .name = "glGetnUniformiv", .entry = (void *)&glGetnUniformiv },
    { .name = "glGetnUniformivARB", .entry = (void *)&glGetnUniformivARB },
    { .name = "glGetnUniformui64vARB", .entry = (void *)&glGetnUniformui64vARB },
    { .name = "glGetnUniformuiv", .entry = (void *)&glGetnUniformuiv },
    { .name = "glGetnUniformuivARB", .entry = (void *)&glGetnUniformuivARB },
    { .name = "glGetObjectBufferfvATI", .entry = (void *)&glGetObjectBufferfvATI },
    { .name = "glGetObjectBufferivATI", .entry = (void *)&glGetObjectBufferivATI },
    { .name = "glGetObjectLabel", .entry = (void *)&glGetObjectLabel },
    { .name = "glGetObjectLabelEXT", .entry = (void *)&glGetObjectLabelEXT },
    { .name = "glGetObjectParameterfvARB", .entry = (void *)&glGetObjectParameterfvARB },
    { .name = "glGetObjectParameterivAPPLE", .entry = (void *)&glGetObjectParameterivAPPLE },
    { .name = "glGetObjectParameterivARB", .entry = (void *)&glGetObjectParameterivARB },
    { .name = "glGetObjectPtrLabel", .entry = (void *)&glGetObjectPtrLabel },
    { .name = "glGetOcclusionQueryivNV", .entry = (void *)&glGetOcclusionQueryivNV },
    { .name = "glGetOcclusionQueryuivNV", .entry = (void *)&glGetOcclusionQueryuivNV },
    { .name = "glGetPathColorGenfvNV", .entry = (void *)&glGetPathColorGenfvNV },
    { .name = "glGetPathColorGenivNV", .entry = (void *)&glGetPathColorGenivNV },
    { .name = "glGetPathCommandsNV", .entry = (void *)&glGetPathCommandsNV },
    { .name = "glGetPathCoordsNV", .entry = (void *)&glGetPathCoordsNV },
    { .name = "glGetPathDashArrayNV", .entry = (void *)&glGetPathDashArrayNV },
    { .name = "glGetPathLengthNV", .entry = (void *)&glGetPathLengthNV },
    { .name = "glGetPathMetricRangeNV", .entry = (void *)&glGetPathMetricRangeNV },
    { .name = "glGetPathMetricsNV", .entry = (void *)&glGetPathMetricsNV },
    { .name = "glGetPathParameterfvNV", .entry = (void *)&glGetPathParameterfvNV },
    { .name = "glGetPathParameterivNV", .entry = (void *)&glGetPathParameterivNV },
    { .name = "glGetPathSpacingNV", .entry = (void *)&glGetPathSpacingNV },
    { .name = "glGetPathTexGenfvNV", .entry = (void *)&glGetPathTexGenfvNV },
    { .name = "glGetPathTexGenivNV", .entry = (void *)&glGetPathTexGenivNV },
    { .name = "glGetPerfCounterInfoINTEL", .entry = (void *)&glGetPerfCounterInfoINTEL },
    { .name = "glGetPerfMonitorCounterDataAMD", .entry = (void *)&glGetPerfMonitorCounterDataAMD },
    { .name = "glGetPerfMonitorCounterInfoAMD", .entry = (void *)&glGetPerfMonitorCounterInfoAMD },
    { .name = "glGetPerfMonitorCountersAMD", .entry = (void *)&glGetPerfMonitorCountersAMD },
    { .name = "glGetPerfMonitorCounterStringAMD", .entry = (void *)&glGetPerfMonitorCounterStringAMD },
    { .name = "glGetPerfMonitorGroupsAMD", .entry = (void *)&glGetPerfMonitorGroupsAMD },
    { .name = "glGetPerfMonitorGroupStringAMD", .entry = (void *)&glGetPerfMonitorGroupStringAMD },
    { .name = "glGetPerfQueryDataINTEL", .entry = (void *)&glGetPerfQueryDataINTEL },
    { .name = "glGetPerfQueryIdByNameINTEL", .entry = (void *)&glGetPerfQueryIdByNameINTEL },
    { .name = "glGetPerfQueryInfoINTEL", .entry = (void *)&glGetPerfQueryInfoINTEL },
    { .name = "glGetPixelMapxv", .entry = (void *)&glGetPixelMapxv },
    { .name = "glGetPixelTexGenParameterfvSGIS", .entry = (void *)&glGetPixelTexGenParameterfvSGIS },
    { .name = "glGetPixelTexGenParameterivSGIS", .entry = (void *)&glGetPixelTexGenParameterivSGIS },
    { .name = "glGetPixelTransformParameterfvEXT", .entry = (void *)&glGetPixelTransformParameterfvEXT },
    { .name = "glGetPixelTransformParameterivEXT", .entry = (void *)&glGetPixelTransformParameterivEXT },
    { .name = "glGetPointeri_vEXT", .entry = (void *)&glGetPointeri_vEXT },
    { .name = "glGetPointerIndexedvEXT", .entry = (void *)&glGetPointerIndexedvEXT },
    { .name = "glGetPointervEXT", .entry = (void *)&glGetPointervEXT },
    { .name = "glGetProgramBinary", .entry = (void *)&glGetProgramBinary },
    { .name = "glGetProgramEnvParameterdvARB", .entry = (void *)&glGetProgramEnvParameterdvARB },
    { .name = "glGetProgramEnvParameterfvARB", .entry = (void *)&glGetProgramEnvParameterfvARB },
    { .name = "glGetProgramEnvParameterIivNV", .entry = (void *)&glGetProgramEnvParameterIivNV },
    { .name = "glGetProgramEnvParameterIuivNV", .entry = (void *)&glGetProgramEnvParameterIuivNV },
    { .name = "glGetProgramInfoLog", .entry = (void *)&glGetProgramInfoLog },
    { .name = "glGetProgramInterfaceiv", .entry = (void *)&glGetProgramInterfaceiv },
    { .name = "glGetProgramiv", .entry = (void *)&glGetProgramiv },
    { .name = "glGetProgramivARB", .entry = (void *)&glGetProgramivARB },
    { .name = "glGetProgramivNV", .entry = (void *)&glGetProgramivNV },
    { .name = "glGetProgramLocalParameterdvARB", .entry = (void *)&glGetProgramLocalParameterdvARB },
    { .name = "glGetProgramLocalParameterfvARB", .entry = (void *)&glGetProgramLocalParameterfvARB },
    { .name = "glGetProgramLocalParameterIivNV", .entry = (void *)&glGetProgramLocalParameterIivNV },
    { .name = "glGetProgramLocalParameterIuivNV", .entry = (void *)&glGetProgramLocalParameterIuivNV },
    { .name = "glGetProgramNamedParameterdvNV", .entry = (void *)&glGetProgramNamedParameterdvNV },
    { .name = "glGetProgramNamedParameterfvNV", .entry = (void *)&glGetProgramNamedParameterfvNV },
    { .name = "glGetProgramParameterdvNV", .entry = (void *)&glGetProgramParameterdvNV },
    { .name = "glGetProgramParameterfvNV", .entry = (void *)&glGetProgramParameterfvNV },
    { .name = "glGetProgramPipelineInfoLog", .entry = (void *)&glGetProgramPipelineInfoLog },
    { .name = "glGetProgramPipelineiv", .entry = (void *)&glGetProgramPipelineiv },
    { .name = "glGetProgramResourcefvNV", .entry = (void *)&glGetProgramResourcefvNV },
    { .name = "glGetProgramResourceIndex", .entry = (void *)&glGetProgramResourceIndex },
    { .name = "glGetProgramResourceiv", .entry = (void *)&glGetProgramResourceiv },
    { .name = "glGetProgramResourceLocation", .entry = (void *)&glGetProgramResourceLocation },
    { .name = "glGetProgramResourceLocationIndex", .entry = (void *)&glGetProgramResourceLocationIndex },
    { .name = "glGetProgramResourceName", .entry = (void *)&glGetProgramResourceName },
    { .name = "glGetProgramStageiv", .entry = (void *)&glGetProgramStageiv },
    { .name = "glGetProgramStringARB", .entry = (void *)&glGetProgramStringARB },
    { .name = "glGetProgramStringNV", .entry = (void *)&glGetProgramStringNV },
    { .name = "glGetProgramSubroutineParameteruivNV", .entry = (void *)&glGetProgramSubroutineParameteruivNV },
    { .name = "glGetQueryBufferObjecti64v", .entry = (void *)&glGetQueryBufferObjecti64v },
    { .name = "glGetQueryBufferObjectiv", .entry = (void *)&glGetQueryBufferObjectiv },
    { .name = "glGetQueryBufferObjectui64v", .entry = (void *)&glGetQueryBufferObjectui64v },
    { .name = "glGetQueryBufferObjectuiv", .entry = (void *)&glGetQueryBufferObjectuiv },
    { .name = "glGetQueryIndexediv", .entry = (void *)&glGetQueryIndexediv },
    { .name = "glGetQueryiv", .entry = (void *)&glGetQueryiv },
    { .name = "glGetQueryivARB", .entry = (void *)&glGetQueryivARB },
    { .name = "glGetQueryObjecti64v", .entry = (void *)&glGetQueryObjecti64v },
    { .name = "glGetQueryObjecti64vEXT", .entry = (void *)&glGetQueryObjecti64vEXT },
    { .name = "glGetQueryObjectiv", .entry = (void *)&glGetQueryObjectiv },
    { .name = "glGetQueryObjectivARB", .entry = (void *)&glGetQueryObjectivARB },
    { .name = "glGetQueryObjectui64v", .entry = (void *)&glGetQueryObjectui64v },
    { .name = "glGetQueryObjectui64vEXT", .entry = (void *)&glGetQueryObjectui64vEXT },
    { .name = "glGetQueryObjectuiv", .entry = (void *)&glGetQueryObjectuiv },
    { .name = "glGetQueryObjectuivARB", .entry = (void *)&glGetQueryObjectuivARB },
    { .name = "glGetRenderbufferParameteriv", .entry = (void *)&glGetRenderbufferParameteriv },
    { .name = "glGetRenderbufferParameterivEXT", .entry = (void *)&glGetRenderbufferParameterivEXT },
    { .name = "glGetSamplerParameterfv", .entry = (void *)&glGetSamplerParameterfv },
    { .name = "glGetSamplerParameterIiv", .entry = (void *)&glGetSamplerParameterIiv },
    { .name = "glGetSamplerParameterIuiv", .entry = (void *)&glGetSamplerParameterIuiv },
    { .name = "glGetSamplerParameteriv", .entry = (void *)&glGetSamplerParameteriv },
    { .name = "glGetSemaphoreParameterui64vEXT", .entry = (void *)&glGetSemaphoreParameterui64vEXT },
    { .name = "glGetSeparableFilter", .entry = (void *)&glGetSeparableFilter },
    { .name = "glGetSeparableFilterEXT", .entry = (void *)&glGetSeparableFilterEXT },
    { .name = "glGetShaderInfoLog", .entry = (void *)&glGetShaderInfoLog },
    { .name = "glGetShaderiv", .entry = (void *)&glGetShaderiv },
    { .name = "glGetShaderPrecisionFormat", .entry = (void *)&glGetShaderPrecisionFormat },
    { .name = "glGetShaderSource", .entry = (void *)&glGetShaderSource },
    { .name = "glGetShaderSourceARB", .entry = (void *)&glGetShaderSourceARB },
    { .name = "glGetShadingRateImagePaletteNV", .entry = (void *)&glGetShadingRateImagePaletteNV },
    { .name = "glGetShadingRateSampleLocationivNV", .entry = (void *)&glGetShadingRateSampleLocationivNV },
    { .name = "glGetSharpenTexFuncSGIS", .entry = (void *)&glGetSharpenTexFuncSGIS },
    { .name = "glGetStageIndexNV", .entry = (void *)&glGetStageIndexNV },
    { .name = "glGetStringi", .entry = (void *)&glGetStringi },
    { .name = "glGetSubroutineIndex", .entry = (void *)&glGetSubroutineIndex },
    { .name = "glGetSubroutineUniformLocation", .entry = (void *)&glGetSubroutineUniformLocation },
    { .name = "glGetSynciv", .entry = (void *)&glGetSynciv },
    { .name = "glGetTexBumpParameterfvATI", .entry = (void *)&glGetTexBumpParameterfvATI },
    { .name = "glGetTexBumpParameterivATI", .entry = (void *)&glGetTexBumpParameterivATI },
    { .name = "glGetTexEnvxvOES", .entry = (void *)&glGetTexEnvxvOES },
    { .name = "glGetTexFilterFuncSGIS", .entry = (void *)&glGetTexFilterFuncSGIS },
    { .name = "glGetTexGenxvOES", .entry = (void *)&glGetTexGenxvOES },
    { .name = "glGetTexLevelParameterxvOES", .entry = (void *)&glGetTexLevelParameterxvOES },
    { .name = "glGetTexParameterIiv", .entry = (void *)&glGetTexParameterIiv },
    { .name = "glGetTexParameterIivEXT", .entry = (void *)&glGetTexParameterIivEXT },
    { .name = "glGetTexParameterIuiv", .entry = (void *)&glGetTexParameterIuiv },
    { .name = "glGetTexParameterIuivEXT", .entry = (void *)&glGetTexParameterIuivEXT },
    { .name = "glGetTexParameterPointervAPPLE", .entry = (void *)&glGetTexParameterPointervAPPLE },
    { .name = "glGetTexParameterxvOES", .entry = (void *)&glGetTexParameterxvOES },
    { .name = "glGetTextureHandleARB", .entry = (void *)&glGetTextureHandleARB },
    { .name = "glGetTextureHandleNV", .entry = (void *)&glGetTextureHandleNV },
    { .name = "glGetTextureImage", .entry = (void *)&glGetTextureImage },
    { .name = "glGetTextureImageEXT", .entry = (void *)&glGetTextureImageEXT },
    { .name = "glGetTextureLevelParameterfv", .entry = (void *)&glGetTextureLevelParameterfv },
    { .name = "glGetTextureLevelParameterfvEXT", .entry = (void *)&glGetTextureLevelParameterfvEXT },
    { .name = "glGetTextureLevelParameteriv", .entry = (void *)&glGetTextureLevelParameteriv },
    { .name = "glGetTextureLevelParameterivEXT", .entry = (void *)&glGetTextureLevelParameterivEXT },
    { .name = "glGetTextureParameterfv", .entry = (void *)&glGetTextureParameterfv },
    { .name = "glGetTextureParameterfvEXT", .entry = (void *)&glGetTextureParameterfvEXT },
    { .name = "glGetTextureParameterIiv", .entry = (void *)&glGetTextureParameterIiv },
    { .name = "glGetTextureParameterIivEXT", .entry = (void *)&glGetTextureParameterIivEXT },
    { .name = "glGetTextureParameterIuiv", .entry = (void *)&glGetTextureParameterIuiv },
    { .name = "glGetTextureParameterIuivEXT", .entry = (void *)&glGetTextureParameterIuivEXT },
    { .name = "glGetTextureParameteriv", .entry = (void *)&glGetTextureParameteriv },
    { .name = "glGetTextureParameterivEXT", .entry = (void *)&glGetTextureParameterivEXT },
    { .name = "glGetTextureSamplerHandleARB", .entry = (void *)&glGetTextureSamplerHandleARB },
    { .name = "glGetTextureSamplerHandleNV", .entry = (void *)&glGetTextureSamplerHandleNV },
    { .name = "glGetTextureSubImage", .entry = (void *)&glGetTextureSubImage },
    { .name = "glGetTrackMatrixivNV", .entry = (void *)&glGetTrackMatrixivNV },
    { .name = "glGetTransformFeedbacki_v", .entry = (void *)&glGetTransformFeedbacki_v },
    { .name = "glGetTransformFeedbacki64_v", .entry = (void *)&glGetTransformFeedbacki64_v },
    { .name = "glGetTransformFeedbackiv", .entry = (void *)&glGetTransformFeedbackiv },
    { .name = "glGetTransformFeedbackVarying", .entry = (void *)&glGetTransformFeedbackVarying },
    { .name = "glGetTransformFeedbackVaryingEXT", .entry = (void *)&glGetTransformFeedbackVaryingEXT },
    { .name = "glGetTransformFeedbackVaryingNV", .entry = (void *)&glGetTransformFeedbackVaryingNV },
    { .name = "glGetUniformBlockIndex", .entry = (void *)&glGetUniformBlockIndex },
    { .name = "glGetUniformBufferSizeEXT", .entry = (void *)&glGetUniformBufferSizeEXT },
    { .name = "glGetUniformdv", .entry = (void *)&glGetUniformdv },
    { .name = "glGetUniformfv", .entry = (void *)&glGetUniformfv },
    { .name = "glGetUniformfvARB", .entry = (void *)&glGetUniformfvARB },
    { .name = "glGetUniformi64vARB", .entry = (void *)&glGetUniformi64vARB },
    { .name = "glGetUniformi64vNV", .entry = (void *)&glGetUniformi64vNV },
    { .name = "glGetUniformIndices", .entry = (void *)&glGetUniformIndices },
    { .name = "glGetUniformiv", .entry = (void *)&glGetUniformiv },
    { .name = "glGetUniformivARB", .entry = (void *)&glGetUniformivARB },
    { .name = "glGetUniformLocation", .entry = (void *)&glGetUniformLocation },
    { .name = "glGetUniformLocationARB", .entry = (void *)&glGetUniformLocationARB },
    { .name = "glGetUniformOffsetEXT", .entry = (void *)&glGetUniformOffsetEXT },
    { .name = "glGetUniformSubroutineuiv", .entry = (void *)&glGetUniformSubroutineuiv },
    { .name = "glGetUniformui64vARB", .entry = (void *)&glGetUniformui64vARB },
    { .name = "glGetUniformui64vNV", .entry = (void *)&glGetUniformui64vNV },
    { .name = "glGetUniformuiv", .entry = (void *)&glGetUniformuiv },
    { .name = "glGetUniformuivEXT", .entry = (void *)&glGetUniformuivEXT },
    { .name = "glGetUnsignedBytei_vEXT", .entry = (void *)&glGetUnsignedBytei_vEXT },
    { .name = "glGetUnsignedBytevEXT", .entry = (void *)&glGetUnsignedBytevEXT },
    { .name = "glGetVariantArrayObjectfvATI", .entry = (void *)&glGetVariantArrayObjectfvATI },
    { .name = "glGetVariantArrayObjectivATI", .entry = (void *)&glGetVariantArrayObjectivATI },
    { .name = "glGetVariantBooleanvEXT", .entry = (void *)&glGetVariantBooleanvEXT },
    { .name = "glGetVariantFloatvEXT", .entry = (void *)&glGetVariantFloatvEXT },
    { .name = "glGetVariantIntegervEXT", .entry = (void *)&glGetVariantIntegervEXT },
    { .name = "glGetVariantPointervEXT", .entry = (void *)&glGetVariantPointervEXT },
    { .name = "glGetVaryingLocationNV", .entry = (void *)&glGetVaryingLocationNV },
    { .name = "glGetVertexArrayIndexed64iv", .entry = (void *)&glGetVertexArrayIndexed64iv },
    { .name = "glGetVertexArrayIndexediv", .entry = (void *)&glGetVertexArrayIndexediv },
    { .name = "glGetVertexArrayIntegeri_vEXT", .entry = (void *)&glGetVertexArrayIntegeri_vEXT },
    { .name = "glGetVertexArrayIntegervEXT", .entry = (void *)&glGetVertexArrayIntegervEXT },
    { .name = "glGetVertexArrayiv", .entry = (void *)&glGetVertexArrayiv },
    { .name = "glGetVertexArrayPointeri_vEXT", .entry = (void *)&glGetVertexArrayPointeri_vEXT },
    { .name = "glGetVertexArrayPointervEXT", .entry = (void *)&glGetVertexArrayPointervEXT },
    { .name = "glGetVertexAttribArrayObjectfvATI", .entry = (void *)&glGetVertexAttribArrayObjectfvATI },
    { .name = "glGetVertexAttribArrayObjectivATI", .entry = (void *)&glGetVertexAttribArrayObjectivATI },
    { .name = "glGetVertexAttribdv", .entry = (void *)&glGetVertexAttribdv },
    { .name = "glGetVertexAttribdvARB", .entry = (void *)&glGetVertexAttribdvARB },
    { .name = "glGetVertexAttribdvNV", .entry = (void *)&glGetVertexAttribdvNV },
    { .name = "glGetVertexAttribfv", .entry = (void *)&glGetVertexAttribfv },
    { .name = "glGetVertexAttribfvARB", .entry = (void *)&glGetVertexAttribfvARB },
    { .name = "glGetVertexAttribfvNV", .entry = (void *)&glGetVertexAttribfvNV },
    { .name = "glGetVertexAttribIiv", .entry = (void *)&glGetVertexAttribIiv },
    { .name = "glGetVertexAttribIivEXT", .entry = (void *)&glGetVertexAttribIivEXT },
    { .name = "glGetVertexAttribIuiv", .entry = (void *)&glGetVertexAttribIuiv },
    { .name = "glGetVertexAttribIuivEXT", .entry = (void *)&glGetVertexAttribIuivEXT },
    { .name = "glGetVertexAttribiv", .entry = (void *)&glGetVertexAttribiv },
    { .name = "glGetVertexAttribivARB", .entry = (void *)&glGetVertexAttribivARB },
    { .name = "glGetVertexAttribivNV", .entry = (void *)&glGetVertexAttribivNV },
    { .name = "glGetVertexAttribLdv", .entry = (void *)&glGetVertexAttribLdv },
    { .name = "glGetVertexAttribLdvEXT", .entry = (void *)&glGetVertexAttribLdvEXT },
    { .name = "glGetVertexAttribLi64vNV", .entry = (void *)&glGetVertexAttribLi64vNV },
    { .name = "glGetVertexAttribLui64vARB", .entry = (void *)&glGetVertexAttribLui64vARB },
    { .name = "glGetVertexAttribLui64vNV", .entry = (void *)&glGetVertexAttribLui64vNV },
    { .name = "glGetVertexAttribPointerv", .entry = (void *)&glGetVertexAttribPointerv },
    { .name = "glGetVertexAttribPointervARB", .entry = (void *)&glGetVertexAttribPointervARB },
    { .name = "glGetVertexAttribPointervNV", .entry = (void *)&glGetVertexAttribPointervNV },
    { .name = "glGetVideoCaptureivNV", .entry = (void *)&glGetVideoCaptureivNV },
    { .name = "glGetVideoCaptureStreamdvNV", .entry = (void *)&glGetVideoCaptureStreamdvNV },
    { .name = "glGetVideoCaptureStreamfvNV", .entry = (void *)&glGetVideoCaptureStreamfvNV },
    { .name = "glGetVideoCaptureStreamivNV", .entry = (void *)&glGetVideoCaptureStreamivNV },
    { .name = "glGetVideoi64vNV", .entry = (void *)&glGetVideoi64vNV },
    { .name = "glGetVideoivNV", .entry = (void *)&glGetVideoivNV },
    { .name = "glGetVideoui64vNV", .entry = (void *)&glGetVideoui64vNV },
    { .name = "glGetVideouivNV", .entry = (void *)&glGetVideouivNV },
    { .name = "glGetVkProcAddrNV", .entry = (void *)&glGetVkProcAddrNV },
    { .name = "glGlobalAlphaFactorbSUN", .entry = (void *)&glGlobalAlphaFactorbSUN },
    { .name = "glGlobalAlphaFactordSUN", .entry = (void *)&glGlobalAlphaFactordSUN },
    { .name = "glGlobalAlphaFactorfSUN", .entry = (void *)&glGlobalAlphaFactorfSUN },
    { .name = "glGlobalAlphaFactoriSUN", .entry = (void *)&glGlobalAlphaFactoriSUN },
    { .name = "glGlobalAlphaFactorsSUN", .entry = (void *)&glGlobalAlphaFactorsSUN },
    { .name = "glGlobalAlphaFactorubSUN", .entry = (void *)&glGlobalAlphaFactorubSUN },
    { .name = "glGlobalAlphaFactoruiSUN", .entry = (void *)&glGlobalAlphaFactoruiSUN },
    { .name = "glGlobalAlphaFactorusSUN", .entry = (void *)&glGlobalAlphaFactorusSUN },
    { .name = "glHintPGI", .entry = (void *)&glHintPGI },
    { .name = "glHistogram", .entry = (void *)&glHistogram },
    { .name = "glHistogramEXT", .entry = (void *)&glHistogramEXT },
    { .name = "glIglooInterfaceSGIX", .entry = (void *)&glIglooInterfaceSGIX },
    { .name = "glImageTransformParameterfHP", .entry = (void *)&glImageTransformParameterfHP },
    { .name = "glImageTransformParameterfvHP", .entry = (void *)&glImageTransformParameterfvHP },
    { .name = "glImageTransformParameteriHP", .entry = (void *)&glImageTransformParameteriHP },
    { .name = "glImageTransformParameterivHP", .entry = (void *)&glImageTransformParameterivHP },
    { .name = "glImportMemoryFdEXT", .entry = (void *)&glImportMemoryFdEXT },
    { .name = "glImportMemoryWin32HandleEXT", .entry = (void *)&glImportMemoryWin32HandleEXT },
    { .name = "glImportMemoryWin32NameEXT", .entry = (void *)&glImportMemoryWin32NameEXT },
    { .name = "glImportSemaphoreFdEXT", .entry = (void *)&glImportSemaphoreFdEXT },
    { .name = "glImportSemaphoreWin32HandleEXT", .entry = (void *)&glImportSemaphoreWin32HandleEXT },
    { .name = "glImportSemaphoreWin32NameEXT", .entry = (void *)&glImportSemaphoreWin32NameEXT },
    { .name = "glImportSyncEXT", .entry = (void *)&glImportSyncEXT },
    { .name = "glIndexFormatNV", .entry = (void *)&glIndexFormatNV },
    { .name = "glIndexFuncEXT", .entry = (void *)&glIndexFuncEXT },
    { .name = "glIndexMaterialEXT", .entry = (void *)&glIndexMaterialEXT },
    { .name = "glIndexPointerEXT", .entry = (void *)&glIndexPointerEXT },
    { .name = "glIndexPointerListIBM", .entry = (void *)&glIndexPointerListIBM },
    { .name = "glIndexxOES", .entry = (void *)&glIndexxOES },
    { .name = "glIndexxvOES", .entry = (void *)&glIndexxvOES },
    { .name = "glInsertComponentEXT", .entry = (void *)&glInsertComponentEXT },
    { .name = "glInsertEventMarkerEXT", .entry = (void *)&glInsertEventMarkerEXT },
    { .name = "glInstrumentsBufferSGIX", .entry = (void *)&glInstrumentsBufferSGIX },
    { .name = "glInterpolatePathsNV", .entry = (void *)&glInterpolatePathsNV },
    { .name = "glInvalidateBufferData", .entry = (void *)&glInvalidateBufferData },
    { .name = "glInvalidateBufferSubData", .entry = (void *)&glInvalidateBufferSubData },
    { .name = "glInvalidateFramebuffer", .entry = (void *)&glInvalidateFramebuffer },
    { .name = "glInvalidateNamedFramebufferData", .entry = (void *)&glInvalidateNamedFramebufferData },
    { .name = "glInvalidateNamedFramebufferSubData", .entry = (void *)&glInvalidateNamedFramebufferSubData },
    { .name = "glInvalidateSubFramebuffer", .entry = (void *)&glInvalidateSubFramebuffer },
    { .name = "glInvalidateTexImage", .entry = (void *)&glInvalidateTexImage },
    { .name = "glInvalidateTexSubImage", .entry = (void *)&glInvalidateTexSubImage },
    { .name = "glIsAsyncMarkerSGIX", .entry = (void *)&glIsAsyncMarkerSGIX },
    { .name = "glIsBuffer", .entry = (void *)&glIsBuffer },
    { .name = "glIsBufferARB", .entry = (void *)&glIsBufferARB },
    { .name = "glIsBufferResidentNV", .entry = (void *)&glIsBufferResidentNV },
    { .name = "glIsCommandListNV", .entry = (void *)&glIsCommandListNV },
    { .name = "glIsEnabledi", .entry = (void *)&glIsEnabledi },
    { .name = "glIsEnabledIndexedEXT", .entry = (void *)&glIsEnabledIndexedEXT },
    { .name = "glIsFenceAPPLE", .entry = (void *)&glIsFenceAPPLE },
    { .name = "glIsFenceNV", .entry = (void *)&glIsFenceNV },
    { .name = "glIsFramebuffer", .entry = (void *)&glIsFramebuffer },
    { .name = "glIsFramebufferEXT", .entry = (void *)&glIsFramebufferEXT },
    { .name = "glIsImageHandleResidentARB", .entry = (void *)&glIsImageHandleResidentARB },
    { .name = "glIsImageHandleResidentNV", .entry = (void *)&glIsImageHandleResidentNV },
    { .name = "glIsMemoryObjectEXT", .entry = (void *)&glIsMemoryObjectEXT },
    { .name = "glIsNameAMD", .entry = (void *)&glIsNameAMD },
    { .name = "glIsNamedBufferResidentNV", .entry = (void *)&glIsNamedBufferResidentNV },
    { .name = "glIsNamedStringARB", .entry = (void *)&glIsNamedStringARB },
    { .name = "glIsObjectBufferATI", .entry = (void *)&glIsObjectBufferATI },
    { .name = "glIsOcclusionQueryNV", .entry = (void *)&glIsOcclusionQueryNV },
    { .name = "glIsPathNV", .entry = (void *)&glIsPathNV },
    { .name = "glIsPointInFillPathNV", .entry = (void *)&glIsPointInFillPathNV },
    { .name = "glIsPointInStrokePathNV", .entry = (void *)&glIsPointInStrokePathNV },
    { .name = "glIsProgram", .entry = (void *)&glIsProgram },
    { .name = "glIsProgramARB", .entry = (void *)&glIsProgramARB },
    { .name = "glIsProgramNV", .entry = (void *)&glIsProgramNV },
    { .name = "glIsProgramPipeline", .entry = (void *)&glIsProgramPipeline },
    { .name = "glIsQuery", .entry = (void *)&glIsQuery },
    { .name = "glIsQueryARB", .entry = (void *)&glIsQueryARB },
    { .name = "glIsRenderbuffer", .entry = (void *)&glIsRenderbuffer },
    { .name = "glIsRenderbufferEXT", .entry = (void *)&glIsRenderbufferEXT },
    { .name = "glIsSampler", .entry = (void *)&glIsSampler },
    { .name = "glIsSemaphoreEXT", .entry = (void *)&glIsSemaphoreEXT },
    { .name = "glIsShader", .entry = (void *)&glIsShader },
    { .name = "glIsStateNV", .entry = (void *)&glIsStateNV },
    { .name = "glIsSync", .entry = (void *)&glIsSync },
    { .name = "glIsTextureEXT", .entry = (void *)&glIsTextureEXT },
    { .name = "glIsTextureHandleResidentARB", .entry = (void *)&glIsTextureHandleResidentARB },
    { .name = "glIsTextureHandleResidentNV", .entry = (void *)&glIsTextureHandleResidentNV },
    { .name = "glIsTransformFeedback", .entry = (void *)&glIsTransformFeedback },
    { .name = "glIsTransformFeedbackNV", .entry = (void *)&glIsTransformFeedbackNV },
    { .name = "glIsVariantEnabledEXT", .entry = (void *)&glIsVariantEnabledEXT },
    { .name = "glIsVertexArray", .entry = (void *)&glIsVertexArray },
    { .name = "glIsVertexArrayAPPLE", .entry = (void *)&glIsVertexArrayAPPLE },
    { .name = "glIsVertexAttribEnabledAPPLE", .entry = (void *)&glIsVertexAttribEnabledAPPLE },
    { .name = "glLabelObjectEXT", .entry = (void *)&glLabelObjectEXT },
    { .name = "glLGPUCopyImageSubDataNVX", .entry = (void *)&glLGPUCopyImageSubDataNVX },
    { .name = "glLGPUInterlockNVX", .entry = (void *)&glLGPUInterlockNVX },
    { .name = "glLGPUNamedBufferSubDataNVX", .entry = (void *)&glLGPUNamedBufferSubDataNVX },
    { .name = "glLightEnviSGIX", .entry = (void *)&glLightEnviSGIX },
    { .name = "glLightModelxOES", .entry = (void *)&glLightModelxOES },
    { .name = "glLightModelxvOES", .entry = (void *)&glLightModelxvOES },
    { .name = "glLightxOES", .entry = (void *)&glLightxOES },
    { .name = "glLightxvOES", .entry = (void *)&glLightxvOES },
    { .name = "glLineWidthxOES", .entry = (void *)&glLineWidthxOES },
    { .name = "glLinkProgram", .entry = (void *)&glLinkProgram },
    { .name = "glLinkProgramARB", .entry = (void *)&glLinkProgramARB },
    { .name = "glListDrawCommandsStatesClientNV", .entry = (void *)&glListDrawCommandsStatesClientNV },
    { .name = "glListParameterfSGIX", .entry = (void *)&glListParameterfSGIX },
    { .name = "glListParameterfvSGIX", .entry = (void *)&glListParameterfvSGIX },
    { .name = "glListParameteriSGIX", .entry = (void *)&glListParameteriSGIX },
    { .name = "glListParameterivSGIX", .entry = (void *)&glListParameterivSGIX },
    { .name = "glLoadIdentityDeformationMapSGIX", .entry = (void *)&glLoadIdentityDeformationMapSGIX },
    { .name = "glLoadMatrixxOES", .entry = (void *)&glLoadMatrixxOES },
    { .name = "glLoadProgramNV", .entry = (void *)&glLoadProgramNV },
    { .name = "glLoadTransposeMatrixd", .entry = (void *)&glLoadTransposeMatrixd },
    { .name = "glLoadTransposeMatrixdARB", .entry = (void *)&glLoadTransposeMatrixdARB },
    { .name = "glLoadTransposeMatrixf", .entry = (void *)&glLoadTransposeMatrixf },
    { .name = "glLoadTransposeMatrixfARB", .entry = (void *)&glLoadTransposeMatrixfARB },
    { .name = "glLoadTransposeMatrixxOES", .entry = (void *)&glLoadTransposeMatrixxOES },
    { .name = "glLockArraysEXT", .entry = (void *)&glLockArraysEXT },
    { .name = "glMakeBufferNonResidentNV", .entry = (void *)&glMakeBufferNonResidentNV },
    { .name = "glMakeBufferResidentNV", .entry = (void *)&glMakeBufferResidentNV },
    { .name = "glMakeImageHandleNonResidentARB", .entry = (void *)&glMakeImageHandleNonResidentARB },
    { .name = "glMakeImageHandleNonResidentNV", .entry = (void *)&glMakeImageHandleNonResidentNV },
    { .name = "glMakeImageHandleResidentARB", .entry = (void *)&glMakeImageHandleResidentARB },
    { .name = "glMakeImageHandleResidentNV", .entry = (void *)&glMakeImageHandleResidentNV },
    { .name = "glMakeNamedBufferNonResidentNV", .entry = (void *)&glMakeNamedBufferNonResidentNV },
    { .name = "glMakeNamedBufferResidentNV", .entry = (void *)&glMakeNamedBufferResidentNV },
    { .name = "glMakeTextureHandleNonResidentARB", .entry = (void *)&glMakeTextureHandleNonResidentARB },
    { .name = "glMakeTextureHandleNonResidentNV", .entry = (void *)&glMakeTextureHandleNonResidentNV },
    { .name = "glMakeTextureHandleResidentARB", .entry = (void *)&glMakeTextureHandleResidentARB },
    { .name = "glMakeTextureHandleResidentNV", .entry = (void *)&glMakeTextureHandleResidentNV },
    { .name = "glMap1xOES", .entry = (void *)&glMap1xOES },
    { .name = "glMap2xOES", .entry = (void *)&glMap2xOES },
    { .name = "glMapBuffer", .entry = (void *)&glMapBuffer },
    { .name = "glMapBufferARB", .entry = (void *)&glMapBufferARB },
    { .name = "glMapBufferRange", .entry = (void *)&glMapBufferRange },
    { .name = "glMapControlPointsNV", .entry = (void *)&glMapControlPointsNV },
    { .name = "glMapGrid1xOES", .entry = (void *)&glMapGrid1xOES },
    { .name = "glMapGrid2xOES", .entry = (void *)&glMapGrid2xOES },
    { .name = "glMapNamedBuffer", .entry = (void *)&glMapNamedBuffer },
    { .name = "glMapNamedBufferEXT", .entry = (void *)&glMapNamedBufferEXT },
    { .name = "glMapNamedBufferRange", .entry = (void *)&glMapNamedBufferRange },
    { .name = "glMapNamedBufferRangeEXT", .entry = (void *)&glMapNamedBufferRangeEXT },
    { .name = "glMapObjectBufferATI", .entry = (void *)&glMapObjectBufferATI },
    { .name = "glMapParameterfvNV", .entry = (void *)&glMapParameterfvNV },
    { .name = "glMapParameterivNV", .entry = (void *)&glMapParameterivNV },
    { .name = "glMapTexture2DINTEL", .entry = (void *)&glMapTexture2DINTEL },
    { .name = "glMapVertexAttrib1dAPPLE", .entry = (void *)&glMapVertexAttrib1dAPPLE },
    { .name = "glMapVertexAttrib1fAPPLE", .entry = (void *)&glMapVertexAttrib1fAPPLE },
    { .name = "glMapVertexAttrib2dAPPLE", .entry = (void *)&glMapVertexAttrib2dAPPLE },
    { .name = "glMapVertexAttrib2fAPPLE", .entry = (void *)&glMapVertexAttrib2fAPPLE },
    { .name = "glMaterialxOES", .entry = (void *)&glMaterialxOES },
    { .name = "glMaterialxvOES", .entry = (void *)&glMaterialxvOES },
    { .name = "glMatrixFrustumEXT", .entry = (void *)&glMatrixFrustumEXT },
    { .name = "glMatrixIndexPointerARB", .entry = (void *)&glMatrixIndexPointerARB },
    { .name = "glMatrixIndexubvARB", .entry = (void *)&glMatrixIndexubvARB },
    { .name = "glMatrixIndexuivARB", .entry = (void *)&glMatrixIndexuivARB },
    { .name = "glMatrixIndexusvARB", .entry = (void *)&glMatrixIndexusvARB },
    { .name = "glMatrixLoad3x2fNV", .entry = (void *)&glMatrixLoad3x2fNV },
    { .name = "glMatrixLoad3x3fNV", .entry = (void *)&glMatrixLoad3x3fNV },
    { .name = "glMatrixLoaddEXT", .entry = (void *)&glMatrixLoaddEXT },
    { .name = "glMatrixLoadfEXT", .entry = (void *)&glMatrixLoadfEXT },
    { .name = "glMatrixLoadIdentityEXT", .entry = (void *)&glMatrixLoadIdentityEXT },
    { .name = "glMatrixLoadTranspose3x3fNV", .entry = (void *)&glMatrixLoadTranspose3x3fNV },
    { .name = "glMatrixLoadTransposedEXT", .entry = (void *)&glMatrixLoadTransposedEXT },
    { .name = "glMatrixLoadTransposefEXT", .entry = (void *)&glMatrixLoadTransposefEXT },
    { .name = "glMatrixMult3x2fNV", .entry = (void *)&glMatrixMult3x2fNV },
    { .name = "glMatrixMult3x3fNV", .entry = (void *)&glMatrixMult3x3fNV },
    { .name = "glMatrixMultdEXT", .entry = (void *)&glMatrixMultdEXT },
    { .name = "glMatrixMultfEXT", .entry = (void *)&glMatrixMultfEXT },
    { .name = "glMatrixMultTranspose3x3fNV", .entry = (void *)&glMatrixMultTranspose3x3fNV },
    { .name = "glMatrixMultTransposedEXT", .entry = (void *)&glMatrixMultTransposedEXT },
    { .name = "glMatrixMultTransposefEXT", .entry = (void *)&glMatrixMultTransposefEXT },
    { .name = "glMatrixOrthoEXT", .entry = (void *)&glMatrixOrthoEXT },
    { .name = "glMatrixPopEXT", .entry = (void *)&glMatrixPopEXT },
    { .name = "glMatrixPushEXT", .entry = (void *)&glMatrixPushEXT },
    { .name = "glMatrixRotatedEXT", .entry = (void *)&glMatrixRotatedEXT },
    { .name = "glMatrixRotatefEXT", .entry = (void *)&glMatrixRotatefEXT },
    { .name = "glMatrixScaledEXT", .entry = (void *)&glMatrixScaledEXT },
    { .name = "glMatrixScalefEXT", .entry = (void *)&glMatrixScalefEXT },
    { .name = "glMatrixTranslatedEXT", .entry = (void *)&glMatrixTranslatedEXT },
    { .name = "glMatrixTranslatefEXT", .entry = (void *)&glMatrixTranslatefEXT },
    { .name = "glMaxShaderCompilerThreadsARB", .entry = (void *)&glMaxShaderCompilerThreadsARB },
    { .name = "glMaxShaderCompilerThreadsKHR", .entry = (void *)&glMaxShaderCompilerThreadsKHR },
    { .name = "glMemoryBarrier", .entry = (void *)&glMemoryBarrier },
    { .name = "glMemoryBarrierByRegion", .entry = (void *)&glMemoryBarrierByRegion },
    { .name = "glMemoryBarrierEXT", .entry = (void *)&glMemoryBarrierEXT },
    { .name = "glMemoryObjectParameterivEXT", .entry = (void *)&glMemoryObjectParameterivEXT },
    { .name = "glMinmax", .entry = (void *)&glMinmax },
    { .name = "glMinmaxEXT", .entry = (void *)&glMinmaxEXT },
    { .name = "glMinSampleShading", .entry = (void *)&glMinSampleShading },
    { .name = "glMinSampleShadingARB", .entry = (void *)&glMinSampleShadingARB },
    { .name = "glMulticastBarrierNV", .entry = (void *)&glMulticastBarrierNV },
    { .name = "glMulticastBlitFramebufferNV", .entry = (void *)&glMulticastBlitFramebufferNV },
    { .name = "glMulticastBufferSubDataNV", .entry = (void *)&glMulticastBufferSubDataNV },
    { .name = "glMulticastCopyBufferSubDataNV", .entry = (void *)&glMulticastCopyBufferSubDataNV },
    { .name = "glMulticastCopyImageSubDataNV", .entry = (void *)&glMulticastCopyImageSubDataNV },
    { .name = "glMulticastFramebufferSampleLocationsfvNV", .entry = (void *)&glMulticastFramebufferSampleLocationsfvNV },
    { .name = "glMulticastGetQueryObjecti64vNV", .entry = (void *)&glMulticastGetQueryObjecti64vNV },
    { .name = "glMulticastGetQueryObjectivNV", .entry = (void *)&glMulticastGetQueryObjectivNV },
    { .name = "glMulticastGetQueryObjectui64vNV", .entry = (void *)&glMulticastGetQueryObjectui64vNV },
    { .name = "glMulticastGetQueryObjectuivNV", .entry = (void *)&glMulticastGetQueryObjectuivNV },
    { .name = "glMulticastWaitSyncNV", .entry = (void *)&glMulticastWaitSyncNV },
    { .name = "glMultiDrawArrays", .entry = (void *)&glMultiDrawArrays },
    { .name = "glMultiDrawArraysEXT", .entry = (void *)&glMultiDrawArraysEXT },
    { .name = "glMultiDrawArraysIndirect", .entry = (void *)&glMultiDrawArraysIndirect },
    { .name = "glMultiDrawArraysIndirectAMD", .entry = (void *)&glMultiDrawArraysIndirectAMD },
    { .name = "glMultiDrawArraysIndirectBindlessCountNV", .entry = (void *)&glMultiDrawArraysIndirectBindlessCountNV },
    { .name = "glMultiDrawArraysIndirectBindlessNV", .entry = (void *)&glMultiDrawArraysIndirectBindlessNV },
    { .name = "glMultiDrawArraysIndirectCount", .entry = (void *)&glMultiDrawArraysIndirectCount },
    { .name = "glMultiDrawArraysIndirectCountARB", .entry = (void *)&glMultiDrawArraysIndirectCountARB },
    { .name = "glMultiDrawElementArrayAPPLE", .entry = (void *)&glMultiDrawElementArrayAPPLE },
    { .name = "glMultiDrawElements", .entry = (void *)&glMultiDrawElements },
    { .name = "glMultiDrawElementsBaseVertex", .entry = (void *)&glMultiDrawElementsBaseVertex },
    { .name = "glMultiDrawElementsEXT", .entry = (void *)&glMultiDrawElementsEXT },
    { .name = "glMultiDrawElementsIndirect", .entry = (void *)&glMultiDrawElementsIndirect },
    { .name = "glMultiDrawElementsIndirectAMD", .entry = (void *)&glMultiDrawElementsIndirectAMD },
    { .name = "glMultiDrawElementsIndirectBindlessCountNV", .entry = (void *)&glMultiDrawElementsIndirectBindlessCountNV },
    { .name = "glMultiDrawElementsIndirectBindlessNV", .entry = (void *)&glMultiDrawElementsIndirectBindlessNV },
    { .name = "glMultiDrawElementsIndirectCount", .entry = (void *)&glMultiDrawElementsIndirectCount },
    { .name = "glMultiDrawElementsIndirectCountARB", .entry = (void *)&glMultiDrawElementsIndirectCountARB },
    { .name = "glMultiDrawMeshTasksIndirectCountNV", .entry = (void *)&glMultiDrawMeshTasksIndirectCountNV },
    { .name = "glMultiDrawMeshTasksIndirectNV", .entry = (void *)&glMultiDrawMeshTasksIndirectNV },
    { .name = "glMultiDrawRangeElementArrayAPPLE", .entry = (void *)&glMultiDrawRangeElementArrayAPPLE },
    { .name = "glMultiModeDrawArraysIBM", .entry = (void *)&glMultiModeDrawArraysIBM },
    { .name = "glMultiModeDrawElementsIBM", .entry = (void *)&glMultiModeDrawElementsIBM },
    { .name = "glMultiTexBufferEXT", .entry = (void *)&glMultiTexBufferEXT },
    { .name = "glMultiTexCoord1bOES", .entry = (void *)&glMultiTexCoord1bOES },
    { .name = "glMultiTexCoord1bvOES", .entry = (void *)&glMultiTexCoord1bvOES },
    { .name = "glMultiTexCoord1d", .entry = (void *)&glMultiTexCoord1d },
    { .name = "glMultiTexCoord1dARB", .entry = (void *)&glMultiTexCoord1dARB },
    { .name = "glMultiTexCoord1dv", .entry = (void *)&glMultiTexCoord1dv },
    { .name = "glMultiTexCoord1dvARB", .entry = (void *)&glMultiTexCoord1dvARB },
    { .name = "glMultiTexCoord1f", .entry = (void *)&glMultiTexCoord1f },
    { .name = "glMultiTexCoord1fARB", .entry = (void *)&glMultiTexCoord1fARB },
    { .name = "glMultiTexCoord1fv", .entry = (void *)&glMultiTexCoord1fv },
    { .name = "glMultiTexCoord1fvARB", .entry = (void *)&glMultiTexCoord1fvARB },
    { .name = "glMultiTexCoord1hNV", .entry = (void *)&glMultiTexCoord1hNV },
    { .name = "glMultiTexCoord1hvNV", .entry = (void *)&glMultiTexCoord1hvNV },
    { .name = "glMultiTexCoord1i", .entry = (void *)&glMultiTexCoord1i },
    { .name = "glMultiTexCoord1iARB", .entry = (void *)&glMultiTexCoord1iARB },
    { .name = "glMultiTexCoord1iv", .entry = (void *)&glMultiTexCoord1iv },
    { .name = "glMultiTexCoord1ivARB", .entry = (void *)&glMultiTexCoord1ivARB },
    { .name = "glMultiTexCoord1s", .entry = (void *)&glMultiTexCoord1s },
    { .name = "glMultiTexCoord1sARB", .entry = (void *)&glMultiTexCoord1sARB },
    { .name = "glMultiTexCoord1sv", .entry = (void *)&glMultiTexCoord1sv },
    { .name = "glMultiTexCoord1svARB", .entry = (void *)&glMultiTexCoord1svARB },
    { .name = "glMultiTexCoord1xOES", .entry = (void *)&glMultiTexCoord1xOES },
    { .name = "glMultiTexCoord1xvOES", .entry = (void *)&glMultiTexCoord1xvOES },
    { .name = "glMultiTexCoord2bOES", .entry = (void *)&glMultiTexCoord2bOES },
    { .name = "glMultiTexCoord2bvOES", .entry = (void *)&glMultiTexCoord2bvOES },
    { .name = "glMultiTexCoord2d", .entry = (void *)&glMultiTexCoord2d },
    { .name = "glMultiTexCoord2dARB", .entry = (void *)&glMultiTexCoord2dARB },
    { .name = "glMultiTexCoord2dv", .entry = (void *)&glMultiTexCoord2dv },
    { .name = "glMultiTexCoord2dvARB", .entry = (void *)&glMultiTexCoord2dvARB },
    { .name = "glMultiTexCoord2f", .entry = (void *)&glMultiTexCoord2f },
    { .name = "glMultiTexCoord2fARB", .entry = (void *)&glMultiTexCoord2fARB },
    { .name = "glMultiTexCoord2fv", .entry = (void *)&glMultiTexCoord2fv },
    { .name = "glMultiTexCoord2fvARB", .entry = (void *)&glMultiTexCoord2fvARB },
    { .name = "glMultiTexCoord2hNV", .entry = (void *)&glMultiTexCoord2hNV },
    { .name = "glMultiTexCoord2hvNV", .entry = (void *)&glMultiTexCoord2hvNV },
    { .name = "glMultiTexCoord2i", .entry = (void *)&glMultiTexCoord2i },
    { .name = "glMultiTexCoord2iARB", .entry = (void *)&glMultiTexCoord2iARB },
    { .name = "glMultiTexCoord2iv", .entry = (void *)&glMultiTexCoord2iv },
    { .name = "glMultiTexCoord2ivARB", .entry = (void *)&glMultiTexCoord2ivARB },
    { .name = "glMultiTexCoord2s", .entry = (void *)&glMultiTexCoord2s },
    { .name = "glMultiTexCoord2sARB", .entry = (void *)&glMultiTexCoord2sARB },
    { .name = "glMultiTexCoord2sv", .entry = (void *)&glMultiTexCoord2sv },
    { .name = "glMultiTexCoord2svARB", .entry = (void *)&glMultiTexCoord2svARB },
    { .name = "glMultiTexCoord2xOES", .entry = (void *)&glMultiTexCoord2xOES },
    { .name = "glMultiTexCoord2xvOES", .entry = (void *)&glMultiTexCoord2xvOES },
    { .name = "glMultiTexCoord3bOES", .entry = (void *)&glMultiTexCoord3bOES },
    { .name = "glMultiTexCoord3bvOES", .entry = (void *)&glMultiTexCoord3bvOES },
    { .name = "glMultiTexCoord3d", .entry = (void *)&glMultiTexCoord3d },
    { .name = "glMultiTexCoord3dARB", .entry = (void *)&glMultiTexCoord3dARB },
    { .name = "glMultiTexCoord3dv", .entry = (void *)&glMultiTexCoord3dv },
    { .name = "glMultiTexCoord3dvARB", .entry = (void *)&glMultiTexCoord3dvARB },
    { .name = "glMultiTexCoord3f", .entry = (void *)&glMultiTexCoord3f },
    { .name = "glMultiTexCoord3fARB", .entry = (void *)&glMultiTexCoord3fARB },
    { .name = "glMultiTexCoord3fv", .entry = (void *)&glMultiTexCoord3fv },
    { .name = "glMultiTexCoord3fvARB", .entry = (void *)&glMultiTexCoord3fvARB },
    { .name = "glMultiTexCoord3hNV", .entry = (void *)&glMultiTexCoord3hNV },
    { .name = "glMultiTexCoord3hvNV", .entry = (void *)&glMultiTexCoord3hvNV },
    { .name = "glMultiTexCoord3i", .entry = (void *)&glMultiTexCoord3i },
    { .name = "glMultiTexCoord3iARB", .entry = (void *)&glMultiTexCoord3iARB },
    { .name = "glMultiTexCoord3iv", .entry = (void *)&glMultiTexCoord3iv },
    { .name = "glMultiTexCoord3ivARB", .entry = (void *)&glMultiTexCoord3ivARB },
    { .name = "glMultiTexCoord3s", .entry = (void *)&glMultiTexCoord3s },
    { .name = "glMultiTexCoord3sARB", .entry = (void *)&glMultiTexCoord3sARB },
    { .name = "glMultiTexCoord3sv", .entry = (void *)&glMultiTexCoord3sv },
    { .name = "glMultiTexCoord3svARB", .entry = (void *)&glMultiTexCoord3svARB },
    { .name = "glMultiTexCoord3xOES", .entry = (void *)&glMultiTexCoord3xOES },
    { .name = "glMultiTexCoord3xvOES", .entry = (void *)&glMultiTexCoord3xvOES },
    { .name = "glMultiTexCoord4bOES", .entry = (void *)&glMultiTexCoord4bOES },
    { .name = "glMultiTexCoord4bvOES", .entry = (void *)&glMultiTexCoord4bvOES },
    { .name = "glMultiTexCoord4d", .entry = (void *)&glMultiTexCoord4d },
    { .name = "glMultiTexCoord4dARB", .entry = (void *)&glMultiTexCoord4dARB },
    { .name = "glMultiTexCoord4dv", .entry = (void *)&glMultiTexCoord4dv },
    { .name = "glMultiTexCoord4dvARB", .entry = (void *)&glMultiTexCoord4dvARB },
    { .name = "glMultiTexCoord4f", .entry = (void *)&glMultiTexCoord4f },
    { .name = "glMultiTexCoord4fARB", .entry = (void *)&glMultiTexCoord4fARB },
    { .name = "glMultiTexCoord4fv", .entry = (void *)&glMultiTexCoord4fv },
    { .name = "glMultiTexCoord4fvARB", .entry = (void *)&glMultiTexCoord4fvARB },
    { .name = "glMultiTexCoord4hNV", .entry = (void *)&glMultiTexCoord4hNV },
    { .name = "glMultiTexCoord4hvNV", .entry = (void *)&glMultiTexCoord4hvNV },
    { .name = "glMultiTexCoord4i", .entry = (void *)&glMultiTexCoord4i },
    { .name = "glMultiTexCoord4iARB", .entry = (void *)&glMultiTexCoord4iARB },
    { .name = "glMultiTexCoord4iv", .entry = (void *)&glMultiTexCoord4iv },
    { .name = "glMultiTexCoord4ivARB", .entry = (void *)&glMultiTexCoord4ivARB },
    { .name = "glMultiTexCoord4s", .entry = (void *)&glMultiTexCoord4s },
    { .name = "glMultiTexCoord4sARB", .entry = (void *)&glMultiTexCoord4sARB },
    { .name = "glMultiTexCoord4sv", .entry = (void *)&glMultiTexCoord4sv },
    { .name = "glMultiTexCoord4svARB", .entry = (void *)&glMultiTexCoord4svARB },
    { .name = "glMultiTexCoord4xOES", .entry = (void *)&glMultiTexCoord4xOES },
    { .name = "glMultiTexCoord4xvOES", .entry = (void *)&glMultiTexCoord4xvOES },
    { .name = "glMultiTexCoordP1ui", .entry = (void *)&glMultiTexCoordP1ui },
    { .name = "glMultiTexCoordP1uiv", .entry = (void *)&glMultiTexCoordP1uiv },
    { .name = "glMultiTexCoordP2ui", .entry = (void *)&glMultiTexCoordP2ui },
    { .name = "glMultiTexCoordP2uiv", .entry = (void *)&glMultiTexCoordP2uiv },
    { .name = "glMultiTexCoordP3ui", .entry = (void *)&glMultiTexCoordP3ui },
    { .name = "glMultiTexCoordP3uiv", .entry = (void *)&glMultiTexCoordP3uiv },
    { .name = "glMultiTexCoordP4ui", .entry = (void *)&glMultiTexCoordP4ui },
    { .name = "glMultiTexCoordP4uiv", .entry = (void *)&glMultiTexCoordP4uiv },
    { .name = "glMultiTexCoordPointerEXT", .entry = (void *)&glMultiTexCoordPointerEXT },
    { .name = "glMultiTexEnvfEXT", .entry = (void *)&glMultiTexEnvfEXT },
    { .name = "glMultiTexEnvfvEXT", .entry = (void *)&glMultiTexEnvfvEXT },
    { .name = "glMultiTexEnviEXT", .entry = (void *)&glMultiTexEnviEXT },
    { .name = "glMultiTexEnvivEXT", .entry = (void *)&glMultiTexEnvivEXT },
    { .name = "glMultiTexGendEXT", .entry = (void *)&glMultiTexGendEXT },
    { .name = "glMultiTexGendvEXT", .entry = (void *)&glMultiTexGendvEXT },
    { .name = "glMultiTexGenfEXT", .entry = (void *)&glMultiTexGenfEXT },
    { .name = "glMultiTexGenfvEXT", .entry = (void *)&glMultiTexGenfvEXT },
    { .name = "glMultiTexGeniEXT", .entry = (void *)&glMultiTexGeniEXT },
    { .name = "glMultiTexGenivEXT", .entry = (void *)&glMultiTexGenivEXT },
    { .name = "glMultiTexImage1DEXT", .entry = (void *)&glMultiTexImage1DEXT },
    { .name = "glMultiTexImage2DEXT", .entry = (void *)&glMultiTexImage2DEXT },
    { .name = "glMultiTexImage3DEXT", .entry = (void *)&glMultiTexImage3DEXT },
    { .name = "glMultiTexParameterfEXT", .entry = (void *)&glMultiTexParameterfEXT },
    { .name = "glMultiTexParameterfvEXT", .entry = (void *)&glMultiTexParameterfvEXT },
    { .name = "glMultiTexParameteriEXT", .entry = (void *)&glMultiTexParameteriEXT },
    { .name = "glMultiTexParameterIivEXT", .entry = (void *)&glMultiTexParameterIivEXT },
    { .name = "glMultiTexParameterIuivEXT", .entry = (void *)&glMultiTexParameterIuivEXT },
    { .name = "glMultiTexParameterivEXT", .entry = (void *)&glMultiTexParameterivEXT },
    { .name = "glMultiTexRenderbufferEXT", .entry = (void *)&glMultiTexRenderbufferEXT },
    { .name = "glMultiTexSubImage1DEXT", .entry = (void *)&glMultiTexSubImage1DEXT },
    { .name = "glMultiTexSubImage2DEXT", .entry = (void *)&glMultiTexSubImage2DEXT },
    { .name = "glMultiTexSubImage3DEXT", .entry = (void *)&glMultiTexSubImage3DEXT },
    { .name = "glMultMatrixxOES", .entry = (void *)&glMultMatrixxOES },
    { .name = "glMultTransposeMatrixd", .entry = (void *)&glMultTransposeMatrixd },
    { .name = "glMultTransposeMatrixdARB", .entry = (void *)&glMultTransposeMatrixdARB },
    { .name = "glMultTransposeMatrixf", .entry = (void *)&glMultTransposeMatrixf },
    { .name = "glMultTransposeMatrixfARB", .entry = (void *)&glMultTransposeMatrixfARB },
    { .name = "glMultTransposeMatrixxOES", .entry = (void *)&glMultTransposeMatrixxOES },
    { .name = "glNamedBufferAttachMemoryNV", .entry = (void *)&glNamedBufferAttachMemoryNV },
    { .name = "glNamedBufferData", .entry = (void *)&glNamedBufferData },
    { .name = "glNamedBufferDataEXT", .entry = (void *)&glNamedBufferDataEXT },
    { .name = "glNamedBufferPageCommitmentARB", .entry = (void *)&glNamedBufferPageCommitmentARB },
    { .name = "glNamedBufferPageCommitmentEXT", .entry = (void *)&glNamedBufferPageCommitmentEXT },
    { .name = "glNamedBufferStorage", .entry = (void *)&glNamedBufferStorage },
    { .name = "glNamedBufferStorageEXT", .entry = (void *)&glNamedBufferStorageEXT },
    { .name = "glNamedBufferStorageExternalEXT", .entry = (void *)&glNamedBufferStorageExternalEXT },
    { .name = "glNamedBufferStorageMemEXT", .entry = (void *)&glNamedBufferStorageMemEXT },
    { .name = "glNamedBufferSubData", .entry = (void *)&glNamedBufferSubData },
    { .name = "glNamedBufferSubDataEXT", .entry = (void *)&glNamedBufferSubDataEXT },
    { .name = "glNamedCopyBufferSubDataEXT", .entry = (void *)&glNamedCopyBufferSubDataEXT },
    { .name = "glNamedFramebufferDrawBuffer", .entry = (void *)&glNamedFramebufferDrawBuffer },
    { .name = "glNamedFramebufferDrawBuffers", .entry = (void *)&glNamedFramebufferDrawBuffers },
    { .name = "glNamedFramebufferParameteri", .entry = (void *)&glNamedFramebufferParameteri },
    { .name = "glNamedFramebufferParameteriEXT", .entry = (void *)&glNamedFramebufferParameteriEXT },
    { .name = "glNamedFramebufferReadBuffer", .entry = (void *)&glNamedFramebufferReadBuffer },
    { .name = "glNamedFramebufferRenderbuffer", .entry = (void *)&glNamedFramebufferRenderbuffer },
    { .name = "glNamedFramebufferRenderbufferEXT", .entry = (void *)&glNamedFramebufferRenderbufferEXT },
    { .name = "glNamedFramebufferSampleLocationsfvARB", .entry = (void *)&glNamedFramebufferSampleLocationsfvARB },
    { .name = "glNamedFramebufferSampleLocationsfvNV", .entry = (void *)&glNamedFramebufferSampleLocationsfvNV },
    { .name = "glNamedFramebufferSamplePositionsfvAMD", .entry = (void *)&glNamedFramebufferSamplePositionsfvAMD },
    { .name = "glNamedFramebufferTexture", .entry = (void *)&glNamedFramebufferTexture },
    { .name = "glNamedFramebufferTexture1DEXT", .entry = (void *)&glNamedFramebufferTexture1DEXT },
    { .name = "glNamedFramebufferTexture2DEXT", .entry = (void *)&glNamedFramebufferTexture2DEXT },
    { .name = "glNamedFramebufferTexture3DEXT", .entry = (void *)&glNamedFramebufferTexture3DEXT },
    { .name = "glNamedFramebufferTextureEXT", .entry = (void *)&glNamedFramebufferTextureEXT },
    { .name = "glNamedFramebufferTextureFaceEXT", .entry = (void *)&glNamedFramebufferTextureFaceEXT },
    { .name = "glNamedFramebufferTextureLayer", .entry = (void *)&glNamedFramebufferTextureLayer },
    { .name = "glNamedFramebufferTextureLayerEXT", .entry = (void *)&glNamedFramebufferTextureLayerEXT },
    { .name = "glNamedProgramLocalParameter4dEXT", .entry = (void *)&glNamedProgramLocalParameter4dEXT },
    { .name = "glNamedProgramLocalParameter4dvEXT", .entry = (void *)&glNamedProgramLocalParameter4dvEXT },
    { .name = "glNamedProgramLocalParameter4fEXT", .entry = (void *)&glNamedProgramLocalParameter4fEXT },
    { .name = "glNamedProgramLocalParameter4fvEXT", .entry = (void *)&glNamedProgramLocalParameter4fvEXT },
    { .name = "glNamedProgramLocalParameterI4iEXT", .entry = (void *)&glNamedProgramLocalParameterI4iEXT },
    { .name = "glNamedProgramLocalParameterI4ivEXT", .entry = (void *)&glNamedProgramLocalParameterI4ivEXT },
    { .name = "glNamedProgramLocalParameterI4uiEXT", .entry = (void *)&glNamedProgramLocalParameterI4uiEXT },
    { .name = "glNamedProgramLocalParameterI4uivEXT", .entry = (void *)&glNamedProgramLocalParameterI4uivEXT },
    { .name = "glNamedProgramLocalParameters4fvEXT", .entry = (void *)&glNamedProgramLocalParameters4fvEXT },
    { .name = "glNamedProgramLocalParametersI4ivEXT", .entry = (void *)&glNamedProgramLocalParametersI4ivEXT },
    { .name = "glNamedProgramLocalParametersI4uivEXT", .entry = (void *)&glNamedProgramLocalParametersI4uivEXT },
    { .name = "glNamedProgramStringEXT", .entry = (void *)&glNamedProgramStringEXT },
    { .name = "glNamedRenderbufferStorage", .entry = (void *)&glNamedRenderbufferStorage },
    { .name = "glNamedRenderbufferStorageEXT", .entry = (void *)&glNamedRenderbufferStorageEXT },
    { .name = "glNamedRenderbufferStorageMultisample", .entry = (void *)&glNamedRenderbufferStorageMultisample },
    { .name = "glNamedRenderbufferStorageMultisampleAdvancedAMD", .entry = (void *)&glNamedRenderbufferStorageMultisampleAdvancedAMD },
    { .name = "glNamedRenderbufferStorageMultisampleCoverageEXT", .entry = (void *)&glNamedRenderbufferStorageMultisampleCoverageEXT },
    { .name = "glNamedRenderbufferStorageMultisampleEXT", .entry = (void *)&glNamedRenderbufferStorageMultisampleEXT },
    { .name = "glNamedStringARB", .entry = (void *)&glNamedStringARB },
    { .name = "glNewObjectBufferATI", .entry = (void *)&glNewObjectBufferATI },
    { .name = "glNormal3fVertex3fSUN", .entry = (void *)&glNormal3fVertex3fSUN },
    { .name = "glNormal3fVertex3fvSUN", .entry = (void *)&glNormal3fVertex3fvSUN },
    { .name = "glNormal3hNV", .entry = (void *)&glNormal3hNV },
    { .name = "glNormal3hvNV", .entry = (void *)&glNormal3hvNV },
    { .name = "glNormal3xOES", .entry = (void *)&glNormal3xOES },
    { .name = "glNormal3xvOES", .entry = (void *)&glNormal3xvOES },
    { .name = "glNormalFormatNV", .entry = (void *)&glNormalFormatNV },
    { .name = "glNormalP3ui", .entry = (void *)&glNormalP3ui },
    { .name = "glNormalP3uiv", .entry = (void *)&glNormalP3uiv },
    { .name = "glNormalPointerEXT", .entry = (void *)&glNormalPointerEXT },
    { .name = "glNormalPointerListIBM", .entry = (void *)&glNormalPointerListIBM },
    { .name = "glNormalPointervINTEL", .entry = (void *)&glNormalPointervINTEL },
    { .name = "glNormalStream3bATI", .entry = (void *)&glNormalStream3bATI },
    { .name = "glNormalStream3bvATI", .entry = (void *)&glNormalStream3bvATI },
    { .name = "glNormalStream3dATI", .entry = (void *)&glNormalStream3dATI },
    { .name = "glNormalStream3dvATI", .entry = (void *)&glNormalStream3dvATI },
    { .name = "glNormalStream3fATI", .entry = (void *)&glNormalStream3fATI },
    { .name = "glNormalStream3fvATI", .entry = (void *)&glNormalStream3fvATI },
    { .name = "glNormalStream3iATI", .entry = (void *)&glNormalStream3iATI },
    { .name = "glNormalStream3ivATI", .entry = (void *)&glNormalStream3ivATI },
    { .name = "glNormalStream3sATI", .entry = (void *)&glNormalStream3sATI },
    { .name = "glNormalStream3svATI", .entry = (void *)&glNormalStream3svATI },
    { .name = "glObjectLabel", .entry = (void *)&glObjectLabel },
    { .name = "glObjectPtrLabel", .entry = (void *)&glObjectPtrLabel },
    { .name = "glObjectPurgeableAPPLE", .entry = (void *)&glObjectPurgeableAPPLE },
    { .name = "glObjectUnpurgeableAPPLE", .entry = (void *)&glObjectUnpurgeableAPPLE },
    { .name = "glOrthofOES", .entry = (void *)&glOrthofOES },
    { .name = "glOrthoxOES", .entry = (void *)&glOrthoxOES },
    { .name = "glPassTexCoordATI", .entry = (void *)&glPassTexCoordATI },
    { .name = "glPassThroughxOES", .entry = (void *)&glPassThroughxOES },
    { .name = "glPatchParameterfv", .entry = (void *)&glPatchParameterfv },
    { .name = "glPatchParameteri", .entry = (void *)&glPatchParameteri },
    { .name = "glPathColorGenNV", .entry = (void *)&glPathColorGenNV },
    { .name = "glPathCommandsNV", .entry = (void *)&glPathCommandsNV },
    { .name = "glPathCoordsNV", .entry = (void *)&glPathCoordsNV },
    { .name = "glPathCoverDepthFuncNV", .entry = (void *)&glPathCoverDepthFuncNV },
    { .name = "glPathDashArrayNV", .entry = (void *)&glPathDashArrayNV },
    { .name = "glPathFogGenNV", .entry = (void *)&glPathFogGenNV },
    { .name = "glPathGlyphIndexArrayNV", .entry = (void *)&glPathGlyphIndexArrayNV },
    { .name = "glPathGlyphIndexRangeNV", .entry = (void *)&glPathGlyphIndexRangeNV },
    { .name = "glPathGlyphRangeNV", .entry = (void *)&glPathGlyphRangeNV },
    { .name = "glPathGlyphsNV", .entry = (void *)&glPathGlyphsNV },
    { .name = "glPathMemoryGlyphIndexArrayNV", .entry = (void *)&glPathMemoryGlyphIndexArrayNV },
    { .name = "glPathParameterfNV", .entry = (void *)&glPathParameterfNV },
    { .name = "glPathParameterfvNV", .entry = (void *)&glPathParameterfvNV },
    { .name = "glPathParameteriNV", .entry = (void *)&glPathParameteriNV },
    { .name = "glPathParameterivNV", .entry = (void *)&glPathParameterivNV },
    { .name = "glPathStencilDepthOffsetNV", .entry = (void *)&glPathStencilDepthOffsetNV },
    { .name = "glPathStencilFuncNV", .entry = (void *)&glPathStencilFuncNV },
    { .name = "glPathStringNV", .entry = (void *)&glPathStringNV },
    { .name = "glPathSubCommandsNV", .entry = (void *)&glPathSubCommandsNV },
    { .name = "glPathSubCoordsNV", .entry = (void *)&glPathSubCoordsNV },
    { .name = "glPathTexGenNV", .entry = (void *)&glPathTexGenNV },
    { .name = "glPauseTransformFeedback", .entry = (void *)&glPauseTransformFeedback },
    { .name = "glPauseTransformFeedbackNV", .entry = (void *)&glPauseTransformFeedbackNV },
    { .name = "glPixelDataRangeNV", .entry = (void *)&glPixelDataRangeNV },
    { .name = "glPixelMapx", .entry = (void *)&glPixelMapx },
    { .name = "glPixelStorex", .entry = (void *)&glPixelStorex },
    { .name = "glPixelTexGenParameterfSGIS", .entry = (void *)&glPixelTexGenParameterfSGIS },
    { .name = "glPixelTexGenParameterfvSGIS", .entry = (void *)&glPixelTexGenParameterfvSGIS },
    { .name = "glPixelTexGenParameteriSGIS", .entry = (void *)&glPixelTexGenParameteriSGIS },
    { .name = "glPixelTexGenParameterivSGIS", .entry = (void *)&glPixelTexGenParameterivSGIS },
    { .name = "glPixelTexGenSGIX", .entry = (void *)&glPixelTexGenSGIX },
    { .name = "glPixelTransferxOES", .entry = (void *)&glPixelTransferxOES },
    { .name = "glPixelTransformParameterfEXT", .entry = (void *)&glPixelTransformParameterfEXT },
    { .name = "glPixelTransformParameterfvEXT", .entry = (void *)&glPixelTransformParameterfvEXT },
    { .name = "glPixelTransformParameteriEXT", .entry = (void *)&glPixelTransformParameteriEXT },
    { .name = "glPixelTransformParameterivEXT", .entry = (void *)&glPixelTransformParameterivEXT },
    { .name = "glPixelZoomxOES", .entry = (void *)&glPixelZoomxOES },
    { .name = "glPNTrianglesfATI", .entry = (void *)&glPNTrianglesfATI },
    { .name = "glPNTrianglesiATI", .entry = (void *)&glPNTrianglesiATI },
    { .name = "glPointAlongPathNV", .entry = (void *)&glPointAlongPathNV },
    { .name = "glPointParameterf", .entry = (void *)&glPointParameterf },
    { .name = "glPointParameterfARB", .entry = (void *)&glPointParameterfARB },
    { .name = "glPointParameterfEXT", .entry = (void *)&glPointParameterfEXT },
    { .name = "glPointParameterfSGIS", .entry = (void *)&glPointParameterfSGIS },
    { .name = "glPointParameterfv", .entry = (void *)&glPointParameterfv },
    { .name = "glPointParameterfvARB", .entry = (void *)&glPointParameterfvARB },
    { .name = "glPointParameterfvEXT", .entry = (void *)&glPointParameterfvEXT },
    { .name = "glPointParameterfvSGIS", .entry = (void *)&glPointParameterfvSGIS },
    { .name = "glPointParameteri", .entry = (void *)&glPointParameteri },
    { .name = "glPointParameteriNV", .entry = (void *)&glPointParameteriNV },
    { .name = "glPointParameteriv", .entry = (void *)&glPointParameteriv },
    { .name = "glPointParameterivNV", .entry = (void *)&glPointParameterivNV },
    { .name = "glPointParameterxvOES", .entry = (void *)&glPointParameterxvOES },
    { .name = "glPointSizexOES", .entry = (void *)&glPointSizexOES },
    { .name = "glPollAsyncSGIX", .entry = (void *)&glPollAsyncSGIX },
    { .name = "glPollInstrumentsSGIX", .entry = (void *)&glPollInstrumentsSGIX },
    { .name = "glPolygonOffsetClamp", .entry = (void *)&glPolygonOffsetClamp },
    { .name = "glPolygonOffsetClampEXT", .entry = (void *)&glPolygonOffsetClampEXT },
    { .name = "glPolygonOffsetEXT", .entry = (void *)&glPolygonOffsetEXT },
    { .name = "glPolygonOffsetxOES", .entry = (void *)&glPolygonOffsetxOES },
    { .name = "glPopDebugGroup", .entry = (void *)&glPopDebugGroup },
    { .name = "glPopGroupMarkerEXT", .entry = (void *)&glPopGroupMarkerEXT },
    { .name = "glPresentFrameDualFillNV", .entry = (void *)&glPresentFrameDualFillNV },
    { .name = "glPresentFrameKeyedNV", .entry = (void *)&glPresentFrameKeyedNV },
    { .name = "glPrimitiveBoundingBoxARB", .entry = (void *)&glPrimitiveBoundingBoxARB },
    { .name = "glPrimitiveRestartIndex", .entry = (void *)&glPrimitiveRestartIndex },
    { .name = "glPrimitiveRestartIndexNV", .entry = (void *)&glPrimitiveRestartIndexNV },
    { .name = "glPrimitiveRestartNV", .entry = (void *)&glPrimitiveRestartNV },
    { .name = "glPrioritizeTexturesEXT", .entry = (void *)&glPrioritizeTexturesEXT },
    { .name = "glPrioritizeTexturesxOES", .entry = (void *)&glPrioritizeTexturesxOES },
    { .name = "glProgramBinary", .entry = (void *)&glProgramBinary },
    { .name = "glProgramBufferParametersfvNV", .entry = (void *)&glProgramBufferParametersfvNV },
    { .name = "glProgramBufferParametersIivNV", .entry = (void *)&glProgramBufferParametersIivNV },
    { .name = "glProgramBufferParametersIuivNV", .entry = (void *)&glProgramBufferParametersIuivNV },
    { .name = "glProgramEnvParameter4dARB", .entry = (void *)&glProgramEnvParameter4dARB },
    { .name = "glProgramEnvParameter4dvARB", .entry = (void *)&glProgramEnvParameter4dvARB },
    { .name = "glProgramEnvParameter4fARB", .entry = (void *)&glProgramEnvParameter4fARB },
    { .name = "glProgramEnvParameter4fvARB", .entry = (void *)&glProgramEnvParameter4fvARB },
    { .name = "glProgramEnvParameterI4iNV", .entry = (void *)&glProgramEnvParameterI4iNV },
    { .name = "glProgramEnvParameterI4ivNV", .entry = (void *)&glProgramEnvParameterI4ivNV },
    { .name = "glProgramEnvParameterI4uiNV", .entry = (void *)&glProgramEnvParameterI4uiNV },
    { .name = "glProgramEnvParameterI4uivNV", .entry = (void *)&glProgramEnvParameterI4uivNV },
    { .name = "glProgramEnvParameters4fvEXT", .entry = (void *)&glProgramEnvParameters4fvEXT },
    { .name = "glProgramEnvParametersI4ivNV", .entry = (void *)&glProgramEnvParametersI4ivNV },
    { .name = "glProgramEnvParametersI4uivNV", .entry = (void *)&glProgramEnvParametersI4uivNV },
    { .name = "glProgramLocalParameter4dARB", .entry = (void *)&glProgramLocalParameter4dARB },
    { .name = "glProgramLocalParameter4dvARB", .entry = (void *)&glProgramLocalParameter4dvARB },
    { .name = "glProgramLocalParameter4fARB", .entry = (void *)&glProgramLocalParameter4fARB },
    { .name = "glProgramLocalParameter4fvARB", .entry = (void *)&glProgramLocalParameter4fvARB },
    { .name = "glProgramLocalParameterI4iNV", .entry = (void *)&glProgramLocalParameterI4iNV },
    { .name = "glProgramLocalParameterI4ivNV", .entry = (void *)&glProgramLocalParameterI4ivNV },
    { .name = "glProgramLocalParameterI4uiNV", .entry = (void *)&glProgramLocalParameterI4uiNV },
    { .name = "glProgramLocalParameterI4uivNV", .entry = (void *)&glProgramLocalParameterI4uivNV },
    { .name = "glProgramLocalParameters4fvEXT", .entry = (void *)&glProgramLocalParameters4fvEXT },
    { .name = "glProgramLocalParametersI4ivNV", .entry = (void *)&glProgramLocalParametersI4ivNV },
    { .name = "glProgramLocalParametersI4uivNV", .entry = (void *)&glProgramLocalParametersI4uivNV },
    { .name = "glProgramNamedParameter4dNV", .entry = (void *)&glProgramNamedParameter4dNV },
    { .name = "glProgramNamedParameter4dvNV", .entry = (void *)&glProgramNamedParameter4dvNV },
    { .name = "glProgramNamedParameter4fNV", .entry = (void *)&glProgramNamedParameter4fNV },
    { .name = "glProgramNamedParameter4fvNV", .entry = (void *)&glProgramNamedParameter4fvNV },
    { .name = "glProgramParameter4dNV", .entry = (void *)&glProgramParameter4dNV },
    { .name = "glProgramParameter4dvNV", .entry = (void *)&glProgramParameter4dvNV },
    { .name = "glProgramParameter4fNV", .entry = (void *)&glProgramParameter4fNV },
    { .name = "glProgramParameter4fvNV", .entry = (void *)&glProgramParameter4fvNV },
    { .name = "glProgramParameteri", .entry = (void *)&glProgramParameteri },
    { .name = "glProgramParameteriARB", .entry = (void *)&glProgramParameteriARB },
    { .name = "glProgramParameteriEXT", .entry = (void *)&glProgramParameteriEXT },
    { .name = "glProgramParameters4dvNV", .entry = (void *)&glProgramParameters4dvNV },
    { .name = "glProgramParameters4fvNV", .entry = (void *)&glProgramParameters4fvNV },
    { .name = "glProgramPathFragmentInputGenNV", .entry = (void *)&glProgramPathFragmentInputGenNV },
    { .name = "glProgramStringARB", .entry = (void *)&glProgramStringARB },
    { .name = "glProgramSubroutineParametersuivNV", .entry = (void *)&glProgramSubroutineParametersuivNV },
    { .name = "glProgramUniform1d", .entry = (void *)&glProgramUniform1d },
    { .name = "glProgramUniform1dEXT", .entry = (void *)&glProgramUniform1dEXT },
    { .name = "glProgramUniform1dv", .entry = (void *)&glProgramUniform1dv },
    { .name = "glProgramUniform1dvEXT", .entry = (void *)&glProgramUniform1dvEXT },
    { .name = "glProgramUniform1f", .entry = (void *)&glProgramUniform1f },
    { .name = "glProgramUniform1fEXT", .entry = (void *)&glProgramUniform1fEXT },
    { .name = "glProgramUniform1fv", .entry = (void *)&glProgramUniform1fv },
    { .name = "glProgramUniform1fvEXT", .entry = (void *)&glProgramUniform1fvEXT },
    { .name = "glProgramUniform1i", .entry = (void *)&glProgramUniform1i },
    { .name = "glProgramUniform1i64ARB", .entry = (void *)&glProgramUniform1i64ARB },
    { .name = "glProgramUniform1i64NV", .entry = (void *)&glProgramUniform1i64NV },
    { .name = "glProgramUniform1i64vARB", .entry = (void *)&glProgramUniform1i64vARB },
    { .name = "glProgramUniform1i64vNV", .entry = (void *)&glProgramUniform1i64vNV },
    { .name = "glProgramUniform1iEXT", .entry = (void *)&glProgramUniform1iEXT },
    { .name = "glProgramUniform1iv", .entry = (void *)&glProgramUniform1iv },
    { .name = "glProgramUniform1ivEXT", .entry = (void *)&glProgramUniform1ivEXT },
    { .name = "glProgramUniform1ui", .entry = (void *)&glProgramUniform1ui },
    { .name = "glProgramUniform1ui64ARB", .entry = (void *)&glProgramUniform1ui64ARB },
    { .name = "glProgramUniform1ui64NV", .entry = (void *)&glProgramUniform1ui64NV },
    { .name = "glProgramUniform1ui64vARB", .entry = (void *)&glProgramUniform1ui64vARB },
    { .name = "glProgramUniform1ui64vNV", .entry = (void *)&glProgramUniform1ui64vNV },
    { .name = "glProgramUniform1uiEXT", .entry = (void *)&glProgramUniform1uiEXT },
    { .name = "glProgramUniform1uiv", .entry = (void *)&glProgramUniform1uiv },
    { .name = "glProgramUniform1uivEXT", .entry = (void *)&glProgramUniform1uivEXT },
    { .name = "glProgramUniform2d", .entry = (void *)&glProgramUniform2d },
    { .name = "glProgramUniform2dEXT", .entry = (void *)&glProgramUniform2dEXT },
    { .name = "glProgramUniform2dv", .entry = (void *)&glProgramUniform2dv },
    { .name = "glProgramUniform2dvEXT", .entry = (void *)&glProgramUniform2dvEXT },
    { .name = "glProgramUniform2f", .entry = (void *)&glProgramUniform2f },
    { .name = "glProgramUniform2fEXT", .entry = (void *)&glProgramUniform2fEXT },
    { .name = "glProgramUniform2fv", .entry = (void *)&glProgramUniform2fv },
    { .name = "glProgramUniform2fvEXT", .entry = (void *)&glProgramUniform2fvEXT },
    { .name = "glProgramUniform2i", .entry = (void *)&glProgramUniform2i },
    { .name = "glProgramUniform2i64ARB", .entry = (void *)&glProgramUniform2i64ARB },
    { .name = "glProgramUniform2i64NV", .entry = (void *)&glProgramUniform2i64NV },
    { .name = "glProgramUniform2i64vARB", .entry = (void *)&glProgramUniform2i64vARB },
    { .name = "glProgramUniform2i64vNV", .entry = (void *)&glProgramUniform2i64vNV },
    { .name = "glProgramUniform2iEXT", .entry = (void *)&glProgramUniform2iEXT },
    { .name = "glProgramUniform2iv", .entry = (void *)&glProgramUniform2iv },
    { .name = "glProgramUniform2ivEXT", .entry = (void *)&glProgramUniform2ivEXT },
    { .name = "glProgramUniform2ui", .entry = (void *)&glProgramUniform2ui },
    { .name = "glProgramUniform2ui64ARB", .entry = (void *)&glProgramUniform2ui64ARB },
    { .name = "glProgramUniform2ui64NV", .entry = (void *)&glProgramUniform2ui64NV },
    { .name = "glProgramUniform2ui64vARB", .entry = (void *)&glProgramUniform2ui64vARB },
    { .name = "glProgramUniform2ui64vNV", .entry = (void *)&glProgramUniform2ui64vNV },
    { .name = "glProgramUniform2uiEXT", .entry = (void *)&glProgramUniform2uiEXT },
    { .name = "glProgramUniform2uiv", .entry = (void *)&glProgramUniform2uiv },
    { .name = "glProgramUniform2uivEXT", .entry = (void *)&glProgramUniform2uivEXT },
    { .name = "glProgramUniform3d", .entry = (void *)&glProgramUniform3d },
    { .name = "glProgramUniform3dEXT", .entry = (void *)&glProgramUniform3dEXT },
    { .name = "glProgramUniform3dv", .entry = (void *)&glProgramUniform3dv },
    { .name = "glProgramUniform3dvEXT", .entry = (void *)&glProgramUniform3dvEXT },
    { .name = "glProgramUniform3f", .entry = (void *)&glProgramUniform3f },
    { .name = "glProgramUniform3fEXT", .entry = (void *)&glProgramUniform3fEXT },
    { .name = "glProgramUniform3fv", .entry = (void *)&glProgramUniform3fv },
    { .name = "glProgramUniform3fvEXT", .entry = (void *)&glProgramUniform3fvEXT },
    { .name = "glProgramUniform3i", .entry = (void *)&glProgramUniform3i },
    { .name = "glProgramUniform3i64ARB", .entry = (void *)&glProgramUniform3i64ARB },
    { .name = "glProgramUniform3i64NV", .entry = (void *)&glProgramUniform3i64NV },
    { .name = "glProgramUniform3i64vARB", .entry = (void *)&glProgramUniform3i64vARB },
    { .name = "glProgramUniform3i64vNV", .entry = (void *)&glProgramUniform3i64vNV },
    { .name = "glProgramUniform3iEXT", .entry = (void *)&glProgramUniform3iEXT },
    { .name = "glProgramUniform3iv", .entry = (void *)&glProgramUniform3iv },
    { .name = "glProgramUniform3ivEXT", .entry = (void *)&glProgramUniform3ivEXT },
    { .name = "glProgramUniform3ui", .entry = (void *)&glProgramUniform3ui },
    { .name = "glProgramUniform3ui64ARB", .entry = (void *)&glProgramUniform3ui64ARB },
    { .name = "glProgramUniform3ui64NV", .entry = (void *)&glProgramUniform3ui64NV },
    { .name = "glProgramUniform3ui64vARB", .entry = (void *)&glProgramUniform3ui64vARB },
    { .name = "glProgramUniform3ui64vNV", .entry = (void *)&glProgramUniform3ui64vNV },
    { .name = "glProgramUniform3uiEXT", .entry = (void *)&glProgramUniform3uiEXT },
    { .name = "glProgramUniform3uiv", .entry = (void *)&glProgramUniform3uiv },
    { .name = "glProgramUniform3uivEXT", .entry = (void *)&glProgramUniform3uivEXT },
    { .name = "glProgramUniform4d", .entry = (void *)&glProgramUniform4d },
    { .name = "glProgramUniform4dEXT", .entry = (void *)&glProgramUniform4dEXT },
    { .name = "glProgramUniform4dv", .entry = (void *)&glProgramUniform4dv },
    { .name = "glProgramUniform4dvEXT", .entry = (void *)&glProgramUniform4dvEXT },
    { .name = "glProgramUniform4f", .entry = (void *)&glProgramUniform4f },
    { .name = "glProgramUniform4fEXT", .entry = (void *)&glProgramUniform4fEXT },
    { .name = "glProgramUniform4fv", .entry = (void *)&glProgramUniform4fv },
    { .name = "glProgramUniform4fvEXT", .entry = (void *)&glProgramUniform4fvEXT },
    { .name = "glProgramUniform4i", .entry = (void *)&glProgramUniform4i },
    { .name = "glProgramUniform4i64ARB", .entry = (void *)&glProgramUniform4i64ARB },
    { .name = "glProgramUniform4i64NV", .entry = (void *)&glProgramUniform4i64NV },
    { .name = "glProgramUniform4i64vARB", .entry = (void *)&glProgramUniform4i64vARB },
    { .name = "glProgramUniform4i64vNV", .entry = (void *)&glProgramUniform4i64vNV },
    { .name = "glProgramUniform4iEXT", .entry = (void *)&glProgramUniform4iEXT },
    { .name = "glProgramUniform4iv", .entry = (void *)&glProgramUniform4iv },
    { .name = "glProgramUniform4ivEXT", .entry = (void *)&glProgramUniform4ivEXT },
    { .name = "glProgramUniform4ui", .entry = (void *)&glProgramUniform4ui },
    { .name = "glProgramUniform4ui64ARB", .entry = (void *)&glProgramUniform4ui64ARB },
    { .name = "glProgramUniform4ui64NV", .entry = (void *)&glProgramUniform4ui64NV },
    { .name = "glProgramUniform4ui64vARB", .entry = (void *)&glProgramUniform4ui64vARB },
    { .name = "glProgramUniform4ui64vNV", .entry = (void *)&glProgramUniform4ui64vNV },
    { .name = "glProgramUniform4uiEXT", .entry = (void *)&glProgramUniform4uiEXT },
    { .name = "glProgramUniform4uiv", .entry = (void *)&glProgramUniform4uiv },
    { .name = "glProgramUniform4uivEXT", .entry = (void *)&glProgramUniform4uivEXT },
    { .name = "glProgramUniformHandleui64ARB", .entry = (void *)&glProgramUniformHandleui64ARB },
    { .name = "glProgramUniformHandleui64NV", .entry = (void *)&glProgramUniformHandleui64NV },
    { .name = "glProgramUniformHandleui64vARB", .entry = (void *)&glProgramUniformHandleui64vARB },
    { .name = "glProgramUniformHandleui64vNV", .entry = (void *)&glProgramUniformHandleui64vNV },
    { .name = "glProgramUniformMatrix2dv", .entry = (void *)&glProgramUniformMatrix2dv },
    { .name = "glProgramUniformMatrix2dvEXT", .entry = (void *)&glProgramUniformMatrix2dvEXT },
    { .name = "glProgramUniformMatrix2fv", .entry = (void *)&glProgramUniformMatrix2fv },
    { .name = "glProgramUniformMatrix2fvEXT", .entry = (void *)&glProgramUniformMatrix2fvEXT },
    { .name = "glProgramUniformMatrix2x3dv", .entry = (void *)&glProgramUniformMatrix2x3dv },
    { .name = "glProgramUniformMatrix2x3dvEXT", .entry = (void *)&glProgramUniformMatrix2x3dvEXT },
    { .name = "glProgramUniformMatrix2x3fv", .entry = (void *)&glProgramUniformMatrix2x3fv },
    { .name = "glProgramUniformMatrix2x3fvEXT", .entry = (void *)&glProgramUniformMatrix2x3fvEXT },
    { .name = "glProgramUniformMatrix2x4dv", .entry = (void *)&glProgramUniformMatrix2x4dv },
    { .name = "glProgramUniformMatrix2x4dvEXT", .entry = (void *)&glProgramUniformMatrix2x4dvEXT },
    { .name = "glProgramUniformMatrix2x4fv", .entry = (void *)&glProgramUniformMatrix2x4fv },
    { .name = "glProgramUniformMatrix2x4fvEXT", .entry = (void *)&glProgramUniformMatrix2x4fvEXT },
    { .name = "glProgramUniformMatrix3dv", .entry = (void *)&glProgramUniformMatrix3dv },
    { .name = "glProgramUniformMatrix3dvEXT", .entry = (void *)&glProgramUniformMatrix3dvEXT },
    { .name = "glProgramUniformMatrix3fv", .entry = (void *)&glProgramUniformMatrix3fv },
    { .name = "glProgramUniformMatrix3fvEXT", .entry = (void *)&glProgramUniformMatrix3fvEXT },
    { .name = "glProgramUniformMatrix3x2dv", .entry = (void *)&glProgramUniformMatrix3x2dv },
    { .name = "glProgramUniformMatrix3x2dvEXT", .entry = (void *)&glProgramUniformMatrix3x2dvEXT },
    { .name = "glProgramUniformMatrix3x2fv", .entry = (void *)&glProgramUniformMatrix3x2fv },
    { .name = "glProgramUniformMatrix3x2fvEXT", .entry = (void *)&glProgramUniformMatrix3x2fvEXT },
    { .name = "glProgramUniformMatrix3x4dv", .entry = (void *)&glProgramUniformMatrix3x4dv },
    { .name = "glProgramUniformMatrix3x4dvEXT", .entry = (void *)&glProgramUniformMatrix3x4dvEXT },
    { .name = "glProgramUniformMatrix3x4fv", .entry = (void *)&glProgramUniformMatrix3x4fv },
    { .name = "glProgramUniformMatrix3x4fvEXT", .entry = (void *)&glProgramUniformMatrix3x4fvEXT },
    { .name = "glProgramUniformMatrix4dv", .entry = (void *)&glProgramUniformMatrix4dv },
    { .name = "glProgramUniformMatrix4dvEXT", .entry = (void *)&glProgramUniformMatrix4dvEXT },
    { .name = "glProgramUniformMatrix4fv", .entry = (void *)&glProgramUniformMatrix4fv },
    { .name = "glProgramUniformMatrix4fvEXT", .entry = (void *)&glProgramUniformMatrix4fvEXT },
    { .name = "glProgramUniformMatrix4x2dv", .entry = (void *)&glProgramUniformMatrix4x2dv },
    { .name = "glProgramUniformMatrix4x2dvEXT", .entry = (void *)&glProgramUniformMatrix4x2dvEXT },
    { .name = "glProgramUniformMatrix4x2fv", .entry = (void *)&glProgramUniformMatrix4x2fv },
    { .name = "glProgramUniformMatrix4x2fvEXT", .entry = (void *)&glProgramUniformMatrix4x2fvEXT },
    { .name = "glProgramUniformMatrix4x3dv", .entry = (void *)&glProgramUniformMatrix4x3dv },
    { .name = "glProgramUniformMatrix4x3dvEXT", .entry = (void *)&glProgramUniformMatrix4x3dvEXT },
    { .name = "glProgramUniformMatrix4x3fv", .entry = (void *)&glProgramUniformMatrix4x3fv },
    { .name = "glProgramUniformMatrix4x3fvEXT", .entry = (void *)&glProgramUniformMatrix4x3fvEXT },
    { .name = "glProgramUniformui64NV", .entry = (void *)&glProgramUniformui64NV },
    { .name = "glProgramUniformui64vNV", .entry = (void *)&glProgramUniformui64vNV },
    { .name = "glProgramVertexLimitNV", .entry = (void *)&glProgramVertexLimitNV },
    { .name = "glProvokingVertex", .entry = (void *)&glProvokingVertex },
    { .name = "glProvokingVertexEXT", .entry = (void *)&glProvokingVertexEXT },
    { .name = "glPushClientAttribDefaultEXT", .entry = (void *)&glPushClientAttribDefaultEXT },
    { .name = "glPushDebugGroup", .entry = (void *)&glPushDebugGroup },
    { .name = "glPushGroupMarkerEXT", .entry = (void *)&glPushGroupMarkerEXT },
    { .name = "glQueryCounter", .entry = (void *)&glQueryCounter },
    { .name = "glQueryMatrixxOES", .entry = (void *)&glQueryMatrixxOES },
    { .name = "glQueryObjectParameteruiAMD", .entry = (void *)&glQueryObjectParameteruiAMD },
    { .name = "glQueryResourceNV", .entry = (void *)&glQueryResourceNV },
    { .name = "glQueryResourceTagNV", .entry = (void *)&glQueryResourceTagNV },
    { .name = "glRasterPos2xOES", .entry = (void *)&glRasterPos2xOES },
    { .name = "glRasterPos2xvOES", .entry = (void *)&glRasterPos2xvOES },
    { .name = "glRasterPos3xOES", .entry = (void *)&glRasterPos3xOES },
    { .name = "glRasterPos3xvOES", .entry = (void *)&glRasterPos3xvOES },
    { .name = "glRasterPos4xOES", .entry = (void *)&glRasterPos4xOES },
    { .name = "glRasterPos4xvOES", .entry = (void *)&glRasterPos4xvOES },
    { .name = "glRasterSamplesEXT", .entry = (void *)&glRasterSamplesEXT },
    { .name = "glReadInstrumentsSGIX", .entry = (void *)&glReadInstrumentsSGIX },
    { .name = "glReadnPixels", .entry = (void *)&glReadnPixels },
    { .name = "glReadnPixelsARB", .entry = (void *)&glReadnPixelsARB },
    { .name = "glRectxOES", .entry = (void *)&glRectxOES },
    { .name = "glRectxvOES", .entry = (void *)&glRectxvOES },
    { .name = "glReferencePlaneSGIX", .entry = (void *)&glReferencePlaneSGIX },
    { .name = "glReleaseKeyedMutexWin32EXT", .entry = (void *)&glReleaseKeyedMutexWin32EXT },
    { .name = "glReleaseShaderCompiler", .entry = (void *)&glReleaseShaderCompiler },
    { .name = "glRenderbufferStorage", .entry = (void *)&glRenderbufferStorage },
    { .name = "glRenderbufferStorageEXT", .entry = (void *)&glRenderbufferStorageEXT },
    { .name = "glRenderbufferStorageMultisample", .entry = (void *)&glRenderbufferStorageMultisample },
    { .name = "glRenderbufferStorageMultisampleAdvancedAMD", .entry = (void *)&glRenderbufferStorageMultisampleAdvancedAMD },
    { .name = "glRenderbufferStorageMultisampleCoverageNV", .entry = (void *)&glRenderbufferStorageMultisampleCoverageNV },
    { .name = "glRenderbufferStorageMultisampleEXT", .entry = (void *)&glRenderbufferStorageMultisampleEXT },
    { .name = "glRenderGpuMaskNV", .entry = (void *)&glRenderGpuMaskNV },
    { .name = "glReplacementCodePointerSUN", .entry = (void *)&glReplacementCodePointerSUN },
    { .name = "glReplacementCodeubSUN", .entry = (void *)&glReplacementCodeubSUN },
    { .name = "glReplacementCodeubvSUN", .entry = (void *)&glReplacementCodeubvSUN },
    { .name = "glReplacementCodeuiColor3fVertex3fSUN", .entry = (void *)&glReplacementCodeuiColor3fVertex3fSUN },
    { .name = "glReplacementCodeuiColor3fVertex3fvSUN", .entry = (void *)&glReplacementCodeuiColor3fVertex3fvSUN },
    { .name = "glReplacementCodeuiColor4fNormal3fVertex3fSUN", .entry = (void *)&glReplacementCodeuiColor4fNormal3fVertex3fSUN },
    { .name = "glReplacementCodeuiColor4fNormal3fVertex3fvSUN", .entry = (void *)&glReplacementCodeuiColor4fNormal3fVertex3fvSUN },
    { .name = "glReplacementCodeuiColor4ubVertex3fSUN", .entry = (void *)&glReplacementCodeuiColor4ubVertex3fSUN },
    { .name = "glReplacementCodeuiColor4ubVertex3fvSUN", .entry = (void *)&glReplacementCodeuiColor4ubVertex3fvSUN },
    { .name = "glReplacementCodeuiNormal3fVertex3fSUN", .entry = (void *)&glReplacementCodeuiNormal3fVertex3fSUN },
    { .name = "glReplacementCodeuiNormal3fVertex3fvSUN", .entry = (void *)&glReplacementCodeuiNormal3fVertex3fvSUN },
    { .name = "glReplacementCodeuiSUN", .entry = (void *)&glReplacementCodeuiSUN },
    { .name = "glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fSUN", .entry = (void *)&glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fSUN },
    { .name = "glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fvSUN", .entry = (void *)&glReplacementCodeuiTexCoord2fColor4fNormal3fVertex3fvSUN },
    { .name = "glReplacementCodeuiTexCoord2fNormal3fVertex3fSUN", .entry = (void *)&glReplacementCodeuiTexCoord2fNormal3fVertex3fSUN },
    { .name = "glReplacementCodeuiTexCoord2fNormal3fVertex3fvSUN", .entry = (void *)&glReplacementCodeuiTexCoord2fNormal3fVertex3fvSUN },
    { .name = "glReplacementCodeuiTexCoord2fVertex3fSUN", .entry = (void *)&glReplacementCodeuiTexCoord2fVertex3fSUN },
    { .name = "glReplacementCodeuiTexCoord2fVertex3fvSUN", .entry = (void *)&glReplacementCodeuiTexCoord2fVertex3fvSUN },
    { .name = "glReplacementCodeuiVertex3fSUN", .entry = (void *)&glReplacementCodeuiVertex3fSUN },
    { .name = "glReplacementCodeuiVertex3fvSUN", .entry = (void *)&glReplacementCodeuiVertex3fvSUN },
    { .name = "glReplacementCodeuivSUN", .entry = (void *)&glReplacementCodeuivSUN },
    { .name = "glReplacementCodeusSUN", .entry = (void *)&glReplacementCodeusSUN },
    { .name = "glReplacementCodeusvSUN", .entry = (void *)&glReplacementCodeusvSUN },
    { .name = "glRequestResidentProgramsNV", .entry = (void *)&glRequestResidentProgramsNV },
    { .name = "glResetHistogram", .entry = (void *)&glResetHistogram },
    { .name = "glResetHistogramEXT", .entry = (void *)&glResetHistogramEXT },
    { .name = "glResetMemoryObjectParameterNV", .entry = (void *)&glResetMemoryObjectParameterNV },
    { .name = "glResetMinmax", .entry = (void *)&glResetMinmax },
    { .name = "glResetMinmaxEXT", .entry = (void *)&glResetMinmaxEXT },
    { .name = "glResizeBuffersMESA", .entry = (void *)&glResizeBuffersMESA },
    { .name = "glResolveDepthValuesNV", .entry = (void *)&glResolveDepthValuesNV },
    { .name = "glResumeTransformFeedback", .entry = (void *)&glResumeTransformFeedback },
    { .name = "glResumeTransformFeedbackNV", .entry = (void *)&glResumeTransformFeedbackNV },
    { .name = "glRotatexOES", .entry = (void *)&glRotatexOES },
    { .name = "glSampleCoverage", .entry = (void *)&glSampleCoverage },
    { .name = "glSampleCoverageARB", .entry = (void *)&glSampleCoverageARB },
    { .name = "glSampleMapATI", .entry = (void *)&glSampleMapATI },
    { .name = "glSampleMaskEXT", .entry = (void *)&glSampleMaskEXT },
    { .name = "glSampleMaski", .entry = (void *)&glSampleMaski },
    { .name = "glSampleMaskIndexedNV", .entry = (void *)&glSampleMaskIndexedNV },
    { .name = "glSampleMaskSGIS", .entry = (void *)&glSampleMaskSGIS },
    { .name = "glSamplePatternEXT", .entry = (void *)&glSamplePatternEXT },
    { .name = "glSamplePatternSGIS", .entry = (void *)&glSamplePatternSGIS },
    { .name = "glSamplerParameterf", .entry = (void *)&glSamplerParameterf },
    { .name = "glSamplerParameterfv", .entry = (void *)&glSamplerParameterfv },
    { .name = "glSamplerParameteri", .entry = (void *)&glSamplerParameteri },
    { .name = "glSamplerParameterIiv", .entry = (void *)&glSamplerParameterIiv },
    { .name = "glSamplerParameterIuiv", .entry = (void *)&glSamplerParameterIuiv },
    { .name = "glSamplerParameteriv", .entry = (void *)&glSamplerParameteriv },
    { .name = "glScalexOES", .entry = (void *)&glScalexOES },
    { .name = "glScissorArrayv", .entry = (void *)&glScissorArrayv },
    { .name = "glScissorExclusiveArrayvNV", .entry = (void *)&glScissorExclusiveArrayvNV },
    { .name = "glScissorExclusiveNV", .entry = (void *)&glScissorExclusiveNV },
    { .name = "glScissorIndexed", .entry = (void *)&glScissorIndexed },
    { .name = "glScissorIndexedv", .entry = (void *)&glScissorIndexedv },
    { .name = "glSecondaryColor3b", .entry = (void *)&glSecondaryColor3b },
    { .name = "glSecondaryColor3bEXT", .entry = (void *)&glSecondaryColor3bEXT },
    { .name = "glSecondaryColor3bv", .entry = (void *)&glSecondaryColor3bv },
    { .name = "glSecondaryColor3bvEXT", .entry = (void *)&glSecondaryColor3bvEXT },
    { .name = "glSecondaryColor3d", .entry = (void *)&glSecondaryColor3d },
    { .name = "glSecondaryColor3dEXT", .entry = (void *)&glSecondaryColor3dEXT },
    { .name = "glSecondaryColor3dv", .entry = (void *)&glSecondaryColor3dv },
    { .name = "glSecondaryColor3dvEXT", .entry = (void *)&glSecondaryColor3dvEXT },
    { .name = "glSecondaryColor3f", .entry = (void *)&glSecondaryColor3f },
    { .name = "glSecondaryColor3fEXT", .entry = (void *)&glSecondaryColor3fEXT },
    { .name = "glSecondaryColor3fv", .entry = (void *)&glSecondaryColor3fv },
    { .name = "glSecondaryColor3fvEXT", .entry = (void *)&glSecondaryColor3fvEXT },
    { .name = "glSecondaryColor3hNV", .entry = (void *)&glSecondaryColor3hNV },
    { .name = "glSecondaryColor3hvNV", .entry = (void *)&glSecondaryColor3hvNV },
    { .name = "glSecondaryColor3i", .entry = (void *)&glSecondaryColor3i },
    { .name = "glSecondaryColor3iEXT", .entry = (void *)&glSecondaryColor3iEXT },
    { .name = "glSecondaryColor3iv", .entry = (void *)&glSecondaryColor3iv },
    { .name = "glSecondaryColor3ivEXT", .entry = (void *)&glSecondaryColor3ivEXT },
    { .name = "glSecondaryColor3s", .entry = (void *)&glSecondaryColor3s },
    { .name = "glSecondaryColor3sEXT", .entry = (void *)&glSecondaryColor3sEXT },
    { .name = "glSecondaryColor3sv", .entry = (void *)&glSecondaryColor3sv },
    { .name = "glSecondaryColor3svEXT", .entry = (void *)&glSecondaryColor3svEXT },
    { .name = "glSecondaryColor3ub", .entry = (void *)&glSecondaryColor3ub },
    { .name = "glSecondaryColor3ubEXT", .entry = (void *)&glSecondaryColor3ubEXT },
    { .name = "glSecondaryColor3ubv", .entry = (void *)&glSecondaryColor3ubv },
    { .name = "glSecondaryColor3ubvEXT", .entry = (void *)&glSecondaryColor3ubvEXT },
    { .name = "glSecondaryColor3ui", .entry = (void *)&glSecondaryColor3ui },
    { .name = "glSecondaryColor3uiEXT", .entry = (void *)&glSecondaryColor3uiEXT },
    { .name = "glSecondaryColor3uiv", .entry = (void *)&glSecondaryColor3uiv },
    { .name = "glSecondaryColor3uivEXT", .entry = (void *)&glSecondaryColor3uivEXT },
    { .name = "glSecondaryColor3us", .entry = (void *)&glSecondaryColor3us },
    { .name = "glSecondaryColor3usEXT", .entry = (void *)&glSecondaryColor3usEXT },
    { .name = "glSecondaryColor3usv", .entry = (void *)&glSecondaryColor3usv },
    { .name = "glSecondaryColor3usvEXT", .entry = (void *)&glSecondaryColor3usvEXT },
    { .name = "glSecondaryColorFormatNV", .entry = (void *)&glSecondaryColorFormatNV },
    { .name = "glSecondaryColorP3ui", .entry = (void *)&glSecondaryColorP3ui },
    { .name = "glSecondaryColorP3uiv", .entry = (void *)&glSecondaryColorP3uiv },
    { .name = "glSecondaryColorPointer", .entry = (void *)&glSecondaryColorPointer },
    { .name = "glSecondaryColorPointerEXT", .entry = (void *)&glSecondaryColorPointerEXT },
    { .name = "glSecondaryColorPointerListIBM", .entry = (void *)&glSecondaryColorPointerListIBM },
    { .name = "glSelectPerfMonitorCountersAMD", .entry = (void *)&glSelectPerfMonitorCountersAMD },
    { .name = "glSemaphoreParameterui64vEXT", .entry = (void *)&glSemaphoreParameterui64vEXT },
    { .name = "glSeparableFilter2D", .entry = (void *)&glSeparableFilter2D },
    { .name = "glSeparableFilter2DEXT", .entry = (void *)&glSeparableFilter2DEXT },
    { .name = "glSetFenceAPPLE", .entry = (void *)&glSetFenceAPPLE },
    { .name = "glSetFenceNV", .entry = (void *)&glSetFenceNV },
    { .name = "glSetFragmentShaderConstantATI", .entry = (void *)&glSetFragmentShaderConstantATI },
    { .name = "glSetInvariantEXT", .entry = (void *)&glSetInvariantEXT },
    { .name = "glSetLocalConstantEXT", .entry = (void *)&glSetLocalConstantEXT },
    { .name = "glSetMultisamplefvAMD", .entry = (void *)&glSetMultisamplefvAMD },
    { .name = "glShaderBinary", .entry = (void *)&glShaderBinary },
    { .name = "glShaderOp1EXT", .entry = (void *)&glShaderOp1EXT },
    { .name = "glShaderOp2EXT", .entry = (void *)&glShaderOp2EXT },
    { .name = "glShaderOp3EXT", .entry = (void *)&glShaderOp3EXT },
    { .name = "glShaderSource", .entry = (void *)&glShaderSource },
    { .name = "glShaderSourceARB", .entry = (void *)&glShaderSourceARB },
    { .name = "glShaderStorageBlockBinding", .entry = (void *)&glShaderStorageBlockBinding },
    { .name = "glShadingRateImageBarrierNV", .entry = (void *)&glShadingRateImageBarrierNV },
    { .name = "glShadingRateImagePaletteNV", .entry = (void *)&glShadingRateImagePaletteNV },
    { .name = "glShadingRateSampleOrderCustomNV", .entry = (void *)&glShadingRateSampleOrderCustomNV },
    { .name = "glShadingRateSampleOrderNV", .entry = (void *)&glShadingRateSampleOrderNV },
    { .name = "glSharpenTexFuncSGIS", .entry = (void *)&glSharpenTexFuncSGIS },
    { .name = "glSignalSemaphoreEXT", .entry = (void *)&glSignalSemaphoreEXT },
    { .name = "glSignalVkFenceNV", .entry = (void *)&glSignalVkFenceNV },
    { .name = "glSignalVkSemaphoreNV", .entry = (void *)&glSignalVkSemaphoreNV },
    { .name = "glSpecializeShader", .entry = (void *)&glSpecializeShader },
    { .name = "glSpecializeShaderARB", .entry = (void *)&glSpecializeShaderARB },
    { .name = "glSpriteParameterfSGIX", .entry = (void *)&glSpriteParameterfSGIX },
    { .name = "glSpriteParameterfvSGIX", .entry = (void *)&glSpriteParameterfvSGIX },
    { .name = "glSpriteParameteriSGIX", .entry = (void *)&glSpriteParameteriSGIX },
    { .name = "glSpriteParameterivSGIX", .entry = (void *)&glSpriteParameterivSGIX },
    { .name = "glStartInstrumentsSGIX", .entry = (void *)&glStartInstrumentsSGIX },
    { .name = "glStateCaptureNV", .entry = (void *)&glStateCaptureNV },
    { .name = "glStencilClearTagEXT", .entry = (void *)&glStencilClearTagEXT },
    { .name = "glStencilFillPathInstancedNV", .entry = (void *)&glStencilFillPathInstancedNV },
    { .name = "glStencilFillPathNV", .entry = (void *)&glStencilFillPathNV },
    { .name = "glStencilFuncSeparate", .entry = (void *)&glStencilFuncSeparate },
    { .name = "glStencilFuncSeparateATI", .entry = (void *)&glStencilFuncSeparateATI },
    { .name = "glStencilMaskSeparate", .entry = (void *)&glStencilMaskSeparate },
    { .name = "glStencilOpSeparate", .entry = (void *)&glStencilOpSeparate },
    { .name = "glStencilOpSeparateATI", .entry = (void *)&glStencilOpSeparateATI },
    { .name = "glStencilOpValueAMD", .entry = (void *)&glStencilOpValueAMD },
    { .name = "glStencilStrokePathInstancedNV", .entry = (void *)&glStencilStrokePathInstancedNV },
    { .name = "glStencilStrokePathNV", .entry = (void *)&glStencilStrokePathNV },
    { .name = "glStencilThenCoverFillPathInstancedNV", .entry = (void *)&glStencilThenCoverFillPathInstancedNV },
    { .name = "glStencilThenCoverFillPathNV", .entry = (void *)&glStencilThenCoverFillPathNV },
    { .name = "glStencilThenCoverStrokePathInstancedNV", .entry = (void *)&glStencilThenCoverStrokePathInstancedNV },
    { .name = "glStencilThenCoverStrokePathNV", .entry = (void *)&glStencilThenCoverStrokePathNV },
    { .name = "glStopInstrumentsSGIX", .entry = (void *)&glStopInstrumentsSGIX },
    { .name = "glStringMarkerGREMEDY", .entry = (void *)&glStringMarkerGREMEDY },
    { .name = "glSubpixelPrecisionBiasNV", .entry = (void *)&glSubpixelPrecisionBiasNV },
    { .name = "glSwizzleEXT", .entry = (void *)&glSwizzleEXT },
    { .name = "glSyncTextureINTEL", .entry = (void *)&glSyncTextureINTEL },
    { .name = "glTagSampleBufferSGIX", .entry = (void *)&glTagSampleBufferSGIX },
    { .name = "glTangent3bEXT", .entry = (void *)&glTangent3bEXT },
    { .name = "glTangent3bvEXT", .entry = (void *)&glTangent3bvEXT },
    { .name = "glTangent3dEXT", .entry = (void *)&glTangent3dEXT },
    { .name = "glTangent3dvEXT", .entry = (void *)&glTangent3dvEXT },
    { .name = "glTangent3fEXT", .entry = (void *)&glTangent3fEXT },
    { .name = "glTangent3fvEXT", .entry = (void *)&glTangent3fvEXT },
    { .name = "glTangent3iEXT", .entry = (void *)&glTangent3iEXT },
    { .name = "glTangent3ivEXT", .entry = (void *)&glTangent3ivEXT },
    { .name = "glTangent3sEXT", .entry = (void *)&glTangent3sEXT },
    { .name = "glTangent3svEXT", .entry = (void *)&glTangent3svEXT },
    { .name = "glTangentPointerEXT", .entry = (void *)&glTangentPointerEXT },
    { .name = "glTbufferMask3DFX", .entry = (void *)&glTbufferMask3DFX },
    { .name = "glTessellationFactorAMD", .entry = (void *)&glTessellationFactorAMD },
    { .name = "glTessellationModeAMD", .entry = (void *)&glTessellationModeAMD },
    { .name = "glTestFenceAPPLE", .entry = (void *)&glTestFenceAPPLE },
    { .name = "glTestFenceNV", .entry = (void *)&glTestFenceNV },
    { .name = "glTestObjectAPPLE", .entry = (void *)&glTestObjectAPPLE },
    { .name = "glTexAttachMemoryNV", .entry = (void *)&glTexAttachMemoryNV },
    { .name = "glTexBuffer", .entry = (void *)&glTexBuffer },
    { .name = "glTexBufferARB", .entry = (void *)&glTexBufferARB },
    { .name = "glTexBufferEXT", .entry = (void *)&glTexBufferEXT },
    { .name = "glTexBufferRange", .entry = (void *)&glTexBufferRange },
    { .name = "glTexBumpParameterfvATI", .entry = (void *)&glTexBumpParameterfvATI },
    { .name = "glTexBumpParameterivATI", .entry = (void *)&glTexBumpParameterivATI },
    { .name = "glTexCoord1bOES", .entry = (void *)&glTexCoord1bOES },
    { .name = "glTexCoord1bvOES", .entry = (void *)&glTexCoord1bvOES },
    { .name = "glTexCoord1hNV", .entry = (void *)&glTexCoord1hNV },
    { .name = "glTexCoord1hvNV", .entry = (void *)&glTexCoord1hvNV },
    { .name = "glTexCoord1xOES", .entry = (void *)&glTexCoord1xOES },
    { .name = "glTexCoord1xvOES", .entry = (void *)&glTexCoord1xvOES },
    { .name = "glTexCoord2bOES", .entry = (void *)&glTexCoord2bOES },
    { .name = "glTexCoord2bvOES", .entry = (void *)&glTexCoord2bvOES },
    { .name = "glTexCoord2fColor3fVertex3fSUN", .entry = (void *)&glTexCoord2fColor3fVertex3fSUN },
    { .name = "glTexCoord2fColor3fVertex3fvSUN", .entry = (void *)&glTexCoord2fColor3fVertex3fvSUN },
    { .name = "glTexCoord2fColor4fNormal3fVertex3fSUN", .entry = (void *)&glTexCoord2fColor4fNormal3fVertex3fSUN },
    { .name = "glTexCoord2fColor4fNormal3fVertex3fvSUN", .entry = (void *)&glTexCoord2fColor4fNormal3fVertex3fvSUN },
    { .name = "glTexCoord2fColor4ubVertex3fSUN", .entry = (void *)&glTexCoord2fColor4ubVertex3fSUN },
    { .name = "glTexCoord2fColor4ubVertex3fvSUN", .entry = (void *)&glTexCoord2fColor4ubVertex3fvSUN },
    { .name = "glTexCoord2fNormal3fVertex3fSUN", .entry = (void *)&glTexCoord2fNormal3fVertex3fSUN },
    { .name = "glTexCoord2fNormal3fVertex3fvSUN", .entry = (void *)&glTexCoord2fNormal3fVertex3fvSUN },
    { .name = "glTexCoord2fVertex3fSUN", .entry = (void *)&glTexCoord2fVertex3fSUN },
    { .name = "glTexCoord2fVertex3fvSUN", .entry = (void *)&glTexCoord2fVertex3fvSUN },
    { .name = "glTexCoord2hNV", .entry = (void *)&glTexCoord2hNV },
    { .name = "glTexCoord2hvNV", .entry = (void *)&glTexCoord2hvNV },
    { .name = "glTexCoord2xOES", .entry = (void *)&glTexCoord2xOES },
    { .name = "glTexCoord2xvOES", .entry = (void *)&glTexCoord2xvOES },
    { .name = "glTexCoord3bOES", .entry = (void *)&glTexCoord3bOES },
    { .name = "glTexCoord3bvOES", .entry = (void *)&glTexCoord3bvOES },
    { .name = "glTexCoord3hNV", .entry = (void *)&glTexCoord3hNV },
    { .name = "glTexCoord3hvNV", .entry = (void *)&glTexCoord3hvNV },
    { .name = "glTexCoord3xOES", .entry = (void *)&glTexCoord3xOES },
    { .name = "glTexCoord3xvOES", .entry = (void *)&glTexCoord3xvOES },
    { .name = "glTexCoord4bOES", .entry = (void *)&glTexCoord4bOES },
    { .name = "glTexCoord4bvOES", .entry = (void *)&glTexCoord4bvOES },
    { .name = "glTexCoord4fColor4fNormal3fVertex4fSUN", .entry = (void *)&glTexCoord4fColor4fNormal3fVertex4fSUN },
    { .name = "glTexCoord4fColor4fNormal3fVertex4fvSUN", .entry = (void *)&glTexCoord4fColor4fNormal3fVertex4fvSUN },
    { .name = "glTexCoord4fVertex4fSUN", .entry = (void *)&glTexCoord4fVertex4fSUN },
    { .name = "glTexCoord4fVertex4fvSUN", .entry = (void *)&glTexCoord4fVertex4fvSUN },
    { .name = "glTexCoord4hNV", .entry = (void *)&glTexCoord4hNV },
    { .name = "glTexCoord4hvNV", .entry = (void *)&glTexCoord4hvNV },
    { .name = "glTexCoord4xOES", .entry = (void *)&glTexCoord4xOES },
    { .name = "glTexCoord4xvOES", .entry = (void *)&glTexCoord4xvOES },
    { .name = "glTexCoordFormatNV", .entry = (void *)&glTexCoordFormatNV },
    { .name = "glTexCoordP1ui", .entry = (void *)&glTexCoordP1ui },
    { .name = "glTexCoordP1uiv", .entry = (void *)&glTexCoordP1uiv },
    { .name = "glTexCoordP2ui", .entry = (void *)&glTexCoordP2ui },
    { .name = "glTexCoordP2uiv", .entry = (void *)&glTexCoordP2uiv },
    { .name = "glTexCoordP3ui", .entry = (void *)&glTexCoordP3ui },
    { .name = "glTexCoordP3uiv", .entry = (void *)&glTexCoordP3uiv },
    { .name = "glTexCoordP4ui", .entry = (void *)&glTexCoordP4ui },
    { .name = "glTexCoordP4uiv", .entry = (void *)&glTexCoordP4uiv },
    { .name = "glTexCoordPointerEXT", .entry = (void *)&glTexCoordPointerEXT },
    { .name = "glTexCoordPointerListIBM", .entry = (void *)&glTexCoordPointerListIBM },
    { .name = "glTexCoordPointervINTEL", .entry = (void *)&glTexCoordPointervINTEL },
    { .name = "glTexEnvxOES", .entry = (void *)&glTexEnvxOES },
    { .name = "glTexEnvxvOES", .entry = (void *)&glTexEnvxvOES },
    { .name = "glTexFilterFuncSGIS", .entry = (void *)&glTexFilterFuncSGIS },
    { .name = "glTexGenxOES", .entry = (void *)&glTexGenxOES },
    { .name = "glTexGenxvOES", .entry = (void *)&glTexGenxvOES },
    { .name = "glTexImage2DMultisample", .entry = (void *)&glTexImage2DMultisample },
    { .name = "glTexImage2DMultisampleCoverageNV", .entry = (void *)&glTexImage2DMultisampleCoverageNV },
    { .name = "glTexImage3D", .entry = (void *)&glTexImage3D },
    { .name = "glTexImage3DEXT", .entry = (void *)&glTexImage3DEXT },
    { .name = "glTexImage3DMultisample", .entry = (void *)&glTexImage3DMultisample },
    { .name = "glTexImage3DMultisampleCoverageNV", .entry = (void *)&glTexImage3DMultisampleCoverageNV },
    { .name = "glTexImage4DSGIS", .entry = (void *)&glTexImage4DSGIS },
    { .name = "glTexPageCommitmentARB", .entry = (void *)&glTexPageCommitmentARB },
    { .name = "glTexParameterIiv", .entry = (void *)&glTexParameterIiv },
    { .name = "glTexParameterIivEXT", .entry = (void *)&glTexParameterIivEXT },
    { .name = "glTexParameterIuiv", .entry = (void *)&glTexParameterIuiv },
    { .name = "glTexParameterIuivEXT", .entry = (void *)&glTexParameterIuivEXT },
    { .name = "glTexParameterxOES", .entry = (void *)&glTexParameterxOES },
    { .name = "glTexParameterxvOES", .entry = (void *)&glTexParameterxvOES },
    { .name = "glTexRenderbufferNV", .entry = (void *)&glTexRenderbufferNV },
    { .name = "glTexStorage1D", .entry = (void *)&glTexStorage1D },
    { .name = "glTexStorage2D", .entry = (void *)&glTexStorage2D },
    { .name = "glTexStorage2DMultisample", .entry = (void *)&glTexStorage2DMultisample },
    { .name = "glTexStorage3D", .entry = (void *)&glTexStorage3D },
    { .name = "glTexStorage3DMultisample", .entry = (void *)&glTexStorage3DMultisample },
    { .name = "glTexStorageMem1DEXT", .entry = (void *)&glTexStorageMem1DEXT },
    { .name = "glTexStorageMem2DEXT", .entry = (void *)&glTexStorageMem2DEXT },
    { .name = "glTexStorageMem2DMultisampleEXT", .entry = (void *)&glTexStorageMem2DMultisampleEXT },
    { .name = "glTexStorageMem3DEXT", .entry = (void *)&glTexStorageMem3DEXT },
    { .name = "glTexStorageMem3DMultisampleEXT", .entry = (void *)&glTexStorageMem3DMultisampleEXT },
    { .name = "glTexStorageSparseAMD", .entry = (void *)&glTexStorageSparseAMD },
    { .name = "glTexSubImage1DEXT", .entry = (void *)&glTexSubImage1DEXT },
    { .name = "glTexSubImage2DEXT", .entry = (void *)&glTexSubImage2DEXT },
    { .name = "glTexSubImage3D", .entry = (void *)&glTexSubImage3D },
    { .name = "glTexSubImage3DEXT", .entry = (void *)&glTexSubImage3DEXT },
    { .name = "glTexSubImage4DSGIS", .entry = (void *)&glTexSubImage4DSGIS },
    { .name = "glTextureAttachMemoryNV", .entry = (void *)&glTextureAttachMemoryNV },
    { .name = "glTextureBarrier", .entry = (void *)&glTextureBarrier },
    { .name = "glTextureBarrierNV", .entry = (void *)&glTextureBarrierNV },
    { .name = "glTextureBuffer", .entry = (void *)&glTextureBuffer },
    { .name = "glTextureBufferEXT", .entry = (void *)&glTextureBufferEXT },
    { .name = "glTextureBufferRange", .entry = (void *)&glTextureBufferRange },
    { .name = "glTextureBufferRangeEXT", .entry = (void *)&glTextureBufferRangeEXT },
    { .name = "glTextureColorMaskSGIS", .entry = (void *)&glTextureColorMaskSGIS },
    { .name = "glTextureImage1DEXT", .entry = (void *)&glTextureImage1DEXT },
    { .name = "glTextureImage2DEXT", .entry = (void *)&glTextureImage2DEXT },
    { .name = "glTextureImage2DMultisampleCoverageNV", .entry = (void *)&glTextureImage2DMultisampleCoverageNV },
    { .name = "glTextureImage2DMultisampleNV", .entry = (void *)&glTextureImage2DMultisampleNV },
    { .name = "glTextureImage3DEXT", .entry = (void *)&glTextureImage3DEXT },
    { .name = "glTextureImage3DMultisampleCoverageNV", .entry = (void *)&glTextureImage3DMultisampleCoverageNV },
    { .name = "glTextureImage3DMultisampleNV", .entry = (void *)&glTextureImage3DMultisampleNV },
    { .name = "glTextureLightEXT", .entry = (void *)&glTextureLightEXT },
    { .name = "glTextureMaterialEXT", .entry = (void *)&glTextureMaterialEXT },
    { .name = "glTextureNormalEXT", .entry = (void *)&glTextureNormalEXT },
    { .name = "glTexturePageCommitmentEXT", .entry = (void *)&glTexturePageCommitmentEXT },
    { .name = "glTextureParameterf", .entry = (void *)&glTextureParameterf },
    { .name = "glTextureParameterfEXT", .entry = (void *)&glTextureParameterfEXT },
    { .name = "glTextureParameterfv", .entry = (void *)&glTextureParameterfv },
    { .name = "glTextureParameterfvEXT", .entry = (void *)&glTextureParameterfvEXT },
    { .name = "glTextureParameteri", .entry = (void *)&glTextureParameteri },
    { .name = "glTextureParameteriEXT", .entry = (void *)&glTextureParameteriEXT },
    { .name = "glTextureParameterIiv", .entry = (void *)&glTextureParameterIiv },
    { .name = "glTextureParameterIivEXT", .entry = (void *)&glTextureParameterIivEXT },
    { .name = "glTextureParameterIuiv", .entry = (void *)&glTextureParameterIuiv },
    { .name = "glTextureParameterIuivEXT", .entry = (void *)&glTextureParameterIuivEXT },
    { .name = "glTextureParameteriv", .entry = (void *)&glTextureParameteriv },
    { .name = "glTextureParameterivEXT", .entry = (void *)&glTextureParameterivEXT },
    { .name = "glTextureRangeAPPLE", .entry = (void *)&glTextureRangeAPPLE },
    { .name = "glTextureRenderbufferEXT", .entry = (void *)&glTextureRenderbufferEXT },
    { .name = "glTextureStorage1D", .entry = (void *)&glTextureStorage1D },
    { .name = "glTextureStorage1DEXT", .entry = (void *)&glTextureStorage1DEXT },
    { .name = "glTextureStorage2D", .entry = (void *)&glTextureStorage2D },
    { .name = "glTextureStorage2DEXT", .entry = (void *)&glTextureStorage2DEXT },
    { .name = "glTextureStorage2DMultisample", .entry = (void *)&glTextureStorage2DMultisample },
    { .name = "glTextureStorage2DMultisampleEXT", .entry = (void *)&glTextureStorage2DMultisampleEXT },
    { .name = "glTextureStorage3D", .entry = (void *)&glTextureStorage3D },
    { .name = "glTextureStorage3DEXT", .entry = (void *)&glTextureStorage3DEXT },
    { .name = "glTextureStorage3DMultisample", .entry = (void *)&glTextureStorage3DMultisample },
    { .name = "glTextureStorage3DMultisampleEXT", .entry = (void *)&glTextureStorage3DMultisampleEXT },
    { .name = "glTextureStorageMem1DEXT", .entry = (void *)&glTextureStorageMem1DEXT },
    { .name = "glTextureStorageMem2DEXT", .entry = (void *)&glTextureStorageMem2DEXT },
    { .name = "glTextureStorageMem2DMultisampleEXT", .entry = (void *)&glTextureStorageMem2DMultisampleEXT },
    { .name = "glTextureStorageMem3DEXT", .entry = (void *)&glTextureStorageMem3DEXT },
    { .name = "glTextureStorageMem3DMultisampleEXT", .entry = (void *)&glTextureStorageMem3DMultisampleEXT },
    { .name = "glTextureStorageSparseAMD", .entry = (void *)&glTextureStorageSparseAMD },
    { .name = "glTextureSubImage1D", .entry = (void *)&glTextureSubImage1D },
    { .name = "glTextureSubImage1DEXT", .entry = (void *)&glTextureSubImage1DEXT },
    { .name = "glTextureSubImage2D", .entry = (void *)&glTextureSubImage2D },
    { .name = "glTextureSubImage2DEXT", .entry = (void *)&glTextureSubImage2DEXT },
    { .name = "glTextureSubImage3D", .entry = (void *)&glTextureSubImage3D },
    { .name = "glTextureSubImage3DEXT", .entry = (void *)&glTextureSubImage3DEXT },
    { .name = "glTextureView", .entry = (void *)&glTextureView },
    { .name = "glTrackMatrixNV", .entry = (void *)&glTrackMatrixNV },
    { .name = "glTransformFeedbackAttribsNV", .entry = (void *)&glTransformFeedbackAttribsNV },
    { .name = "glTransformFeedbackBufferBase", .entry = (void *)&glTransformFeedbackBufferBase },
    { .name = "glTransformFeedbackBufferRange", .entry = (void *)&glTransformFeedbackBufferRange },
    { .name = "glTransformFeedbackStreamAttribsNV", .entry = (void *)&glTransformFeedbackStreamAttribsNV },
    { .name = "glTransformFeedbackVaryings", .entry = (void *)&glTransformFeedbackVaryings },
    { .name = "glTransformFeedbackVaryingsEXT", .entry = (void *)&glTransformFeedbackVaryingsEXT },
    { .name = "glTransformFeedbackVaryingsNV", .entry = (void *)&glTransformFeedbackVaryingsNV },
    { .name = "glTransformPathNV", .entry = (void *)&glTransformPathNV },
    { .name = "glTranslatexOES", .entry = (void *)&glTranslatexOES },
    { .name = "glUniform1d", .entry = (void *)&glUniform1d },
    { .name = "glUniform1dv", .entry = (void *)&glUniform1dv },
    { .name = "glUniform1f", .entry = (void *)&glUniform1f },
    { .name = "glUniform1fARB", .entry = (void *)&glUniform1fARB },
    { .name = "glUniform1fv", .entry = (void *)&glUniform1fv },
    { .name = "glUniform1fvARB", .entry = (void *)&glUniform1fvARB },
    { .name = "glUniform1i", .entry = (void *)&glUniform1i },
    { .name = "glUniform1i64ARB", .entry = (void *)&glUniform1i64ARB },
    { .name = "glUniform1i64NV", .entry = (void *)&glUniform1i64NV },
    { .name = "glUniform1i64vARB", .entry = (void *)&glUniform1i64vARB },
    { .name = "glUniform1i64vNV", .entry = (void *)&glUniform1i64vNV },
    { .name = "glUniform1iARB", .entry = (void *)&glUniform1iARB },
    { .name = "glUniform1iv", .entry = (void *)&glUniform1iv },
    { .name = "glUniform1ivARB", .entry = (void *)&glUniform1ivARB },
    { .name = "glUniform1ui", .entry = (void *)&glUniform1ui },
    { .name = "glUniform1ui64ARB", .entry = (void *)&glUniform1ui64ARB },
    { .name = "glUniform1ui64NV", .entry = (void *)&glUniform1ui64NV },
    { .name = "glUniform1ui64vARB", .entry = (void *)&glUniform1ui64vARB },
    { .name = "glUniform1ui64vNV", .entry = (void *)&glUniform1ui64vNV },
    { .name = "glUniform1uiEXT", .entry = (void *)&glUniform1uiEXT },
    { .name = "glUniform1uiv", .entry = (void *)&glUniform1uiv },
    { .name = "glUniform1uivEXT", .entry = (void *)&glUniform1uivEXT },
    { .name = "glUniform2d", .entry = (void *)&glUniform2d },
    { .name = "glUniform2dv", .entry = (void *)&glUniform2dv },
    { .name = "glUniform2f", .entry = (void *)&glUniform2f },
    { .name = "glUniform2fARB", .entry = (void *)&glUniform2fARB },
    { .name = "glUniform2fv", .entry = (void *)&glUniform2fv },
    { .name = "glUniform2fvARB", .entry = (void *)&glUniform2fvARB },
    { .name = "glUniform2i", .entry = (void *)&glUniform2i },
    { .name = "glUniform2i64ARB", .entry = (void *)&glUniform2i64ARB },
    { .name = "glUniform2i64NV", .entry = (void *)&glUniform2i64NV },
    { .name = "glUniform2i64vARB", .entry = (void *)&glUniform2i64vARB },
    { .name = "glUniform2i64vNV", .entry = (void *)&glUniform2i64vNV },
    { .name = "glUniform2iARB", .entry = (void *)&glUniform2iARB },
    { .name = "glUniform2iv", .entry = (void *)&glUniform2iv },
    { .name = "glUniform2ivARB", .entry = (void *)&glUniform2ivARB },
    { .name = "glUniform2ui", .entry = (void *)&glUniform2ui },
    { .name = "glUniform2ui64ARB", .entry = (void *)&glUniform2ui64ARB },
    { .name = "glUniform2ui64NV", .entry = (void *)&glUniform2ui64NV },
    { .name = "glUniform2ui64vARB", .entry = (void *)&glUniform2ui64vARB },
    { .name = "glUniform2ui64vNV", .entry = (void *)&glUniform2ui64vNV },
    { .name = "glUniform2uiEXT", .entry = (void *)&glUniform2uiEXT },
    { .name = "glUniform2uiv", .entry = (void *)&glUniform2uiv },
    { .name = "glUniform2uivEXT", .entry = (void *)&glUniform2uivEXT },
    { .name = "glUniform3d", .entry = (void *)&glUniform3d },
    { .name = "glUniform3dv", .entry = (void *)&glUniform3dv },
    { .name = "glUniform3f", .entry = (void *)&glUniform3f },
    { .name = "glUniform3fARB", .entry = (void *)&glUniform3fARB },
    { .name = "glUniform3fv", .entry = (void *)&glUniform3fv },
    { .name = "glUniform3fvARB", .entry = (void *)&glUniform3fvARB },
    { .name = "glUniform3i", .entry = (void *)&glUniform3i },
    { .name = "glUniform3i64ARB", .entry = (void *)&glUniform3i64ARB },
    { .name = "glUniform3i64NV", .entry = (void *)&glUniform3i64NV },
    { .name = "glUniform3i64vARB", .entry = (void *)&glUniform3i64vARB },
    { .name = "glUniform3i64vNV", .entry = (void *)&glUniform3i64vNV },
    { .name = "glUniform3iARB", .entry = (void *)&glUniform3iARB },
    { .name = "glUniform3iv", .entry = (void *)&glUniform3iv },
    { .name = "glUniform3ivARB", .entry = (void *)&glUniform3ivARB },
    { .name = "glUniform3ui", .entry = (void *)&glUniform3ui },
    { .name = "glUniform3ui64ARB", .entry = (void *)&glUniform3ui64ARB },
    { .name = "glUniform3ui64NV", .entry = (void *)&glUniform3ui64NV },
    { .name = "glUniform3ui64vARB", .entry = (void *)&glUniform3ui64vARB },
    { .name = "glUniform3ui64vNV", .entry = (void *)&glUniform3ui64vNV },
    { .name = "glUniform3uiEXT", .entry = (void *)&glUniform3uiEXT },
    { .name = "glUniform3uiv", .entry = (void *)&glUniform3uiv },
    { .name = "glUniform3uivEXT", .entry = (void *)&glUniform3uivEXT },
    { .name = "glUniform4d", .entry = (void *)&glUniform4d },
    { .name = "glUniform4dv", .entry = (void *)&glUniform4dv },
    { .name = "glUniform4f", .entry = (void *)&glUniform4f },
    { .name = "glUniform4fARB", .entry = (void *)&glUniform4fARB },
    { .name = "glUniform4fv", .entry = (void *)&glUniform4fv },
    { .name = "glUniform4fvARB", .entry = (void *)&glUniform4fvARB },
    { .name = "glUniform4i", .entry = (void *)&glUniform4i },
    { .name = "glUniform4i64ARB", .entry = (void *)&glUniform4i64ARB },
    { .name = "glUniform4i64NV", .entry = (void *)&glUniform4i64NV },
    { .name = "glUniform4i64vARB", .entry = (void *)&glUniform4i64vARB },
    { .name = "glUniform4i64vNV", .entry = (void *)&glUniform4i64vNV },
    { .name = "glUniform4iARB", .entry = (void *)&glUniform4iARB },
    { .name = "glUniform4iv", .entry = (void *)&glUniform4iv },
    { .name = "glUniform4ivARB", .entry = (void *)&glUniform4ivARB },
    { .name = "glUniform4ui", .entry = (void *)&glUniform4ui },
    { .name = "glUniform4ui64ARB", .entry = (void *)&glUniform4ui64ARB },
    { .name = "glUniform4ui64NV", .entry = (void *)&glUniform4ui64NV },
    { .name = "glUniform4ui64vARB", .entry = (void *)&glUniform4ui64vARB },
    { .name = "glUniform4ui64vNV", .entry = (void *)&glUniform4ui64vNV },
    { .name = "glUniform4uiEXT", .entry = (void *)&glUniform4uiEXT },
    { .name = "glUniform4uiv", .entry = (void *)&glUniform4uiv },
    { .name = "glUniform4uivEXT", .entry = (void *)&glUniform4uivEXT },
    { .name = "glUniformBlockBinding", .entry = (void *)&glUniformBlockBinding },
    { .name = "glUniformBufferEXT", .entry = (void *)&glUniformBufferEXT },
    { .name = "glUniformHandleui64ARB", .entry = (void *)&glUniformHandleui64ARB },
    { .name = "glUniformHandleui64NV", .entry = (void *)&glUniformHandleui64NV },
    { .name = "glUniformHandleui64vARB", .entry = (void *)&glUniformHandleui64vARB },
    { .name = "glUniformHandleui64vNV", .entry = (void *)&glUniformHandleui64vNV },
    { .name = "glUniformMatrix2dv", .entry = (void *)&glUniformMatrix2dv },
    { .name = "glUniformMatrix2fv", .entry = (void *)&glUniformMatrix2fv },
    { .name = "glUniformMatrix2fvARB", .entry = (void *)&glUniformMatrix2fvARB },
    { .name = "glUniformMatrix2x3dv", .entry = (void *)&glUniformMatrix2x3dv },
    { .name = "glUniformMatrix2x3fv", .entry = (void *)&glUniformMatrix2x3fv },
    { .name = "glUniformMatrix2x4dv", .entry = (void *)&glUniformMatrix2x4dv },
    { .name = "glUniformMatrix2x4fv", .entry = (void *)&glUniformMatrix2x4fv },
    { .name = "glUniformMatrix3dv", .entry = (void *)&glUniformMatrix3dv },
    { .name = "glUniformMatrix3fv", .entry = (void *)&glUniformMatrix3fv },
    { .name = "glUniformMatrix3fvARB", .entry = (void *)&glUniformMatrix3fvARB },
    { .name = "glUniformMatrix3x2dv", .entry = (void *)&glUniformMatrix3x2dv },
    { .name = "glUniformMatrix3x2fv", .entry = (void *)&glUniformMatrix3x2fv },
    { .name = "glUniformMatrix3x4dv", .entry = (void *)&glUniformMatrix3x4dv },
    { .name = "glUniformMatrix3x4fv", .entry = (void *)&glUniformMatrix3x4fv },
    { .name = "glUniformMatrix4dv", .entry = (void *)&glUniformMatrix4dv },
    { .name = "glUniformMatrix4fv", .entry = (void *)&glUniformMatrix4fv },
    { .name = "glUniformMatrix4fvARB", .entry = (void *)&glUniformMatrix4fvARB },
    { .name = "glUniformMatrix4x2dv", .entry = (void *)&glUniformMatrix4x2dv },
    { .name = "glUniformMatrix4x2fv", .entry = (void *)&glUniformMatrix4x2fv },
    { .name = "glUniformMatrix4x3dv", .entry = (void *)&glUniformMatrix4x3dv },
    { .name = "glUniformMatrix4x3fv", .entry = (void *)&glUniformMatrix4x3fv },
    { .name = "glUniformSubroutinesuiv", .entry = (void *)&glUniformSubroutinesuiv },
    { .name = "glUniformui64NV", .entry = (void *)&glUniformui64NV },
    { .name = "glUniformui64vNV", .entry = (void *)&glUniformui64vNV },
    { .name = "glUnlockArraysEXT", .entry = (void *)&glUnlockArraysEXT },
    { .name = "glUnmapBuffer", .entry = (void *)&glUnmapBuffer },
    { .name = "glUnmapBufferARB", .entry = (void *)&glUnmapBufferARB },
    { .name = "glUnmapNamedBuffer", .entry = (void *)&glUnmapNamedBuffer },
    { .name = "glUnmapNamedBufferEXT", .entry = (void *)&glUnmapNamedBufferEXT },
    { .name = "glUnmapObjectBufferATI", .entry = (void *)&glUnmapObjectBufferATI },
    { .name = "glUnmapTexture2DINTEL", .entry = (void *)&glUnmapTexture2DINTEL },
    { .name = "glUpdateObjectBufferATI", .entry = (void *)&glUpdateObjectBufferATI },
    { .name = "glUseProgram", .entry = (void *)&glUseProgram },
    { .name = "glUseProgramObjectARB", .entry = (void *)&glUseProgramObjectARB },
    { .name = "glUseProgramStages", .entry = (void *)&glUseProgramStages },
    { .name = "glUseShaderProgramEXT", .entry = (void *)&glUseShaderProgramEXT },
    { .name = "glValidateProgram", .entry = (void *)&glValidateProgram },
    { .name = "glValidateProgramARB", .entry = (void *)&glValidateProgramARB },
    { .name = "glValidateProgramPipeline", .entry = (void *)&glValidateProgramPipeline },
    { .name = "glVariantArrayObjectATI", .entry = (void *)&glVariantArrayObjectATI },
    { .name = "glVariantbvEXT", .entry = (void *)&glVariantbvEXT },
    { .name = "glVariantdvEXT", .entry = (void *)&glVariantdvEXT },
    { .name = "glVariantfvEXT", .entry = (void *)&glVariantfvEXT },
    { .name = "glVariantivEXT", .entry = (void *)&glVariantivEXT },
    { .name = "glVariantPointerEXT", .entry = (void *)&glVariantPointerEXT },
    { .name = "glVariantsvEXT", .entry = (void *)&glVariantsvEXT },
    { .name = "glVariantubvEXT", .entry = (void *)&glVariantubvEXT },
    { .name = "glVariantuivEXT", .entry = (void *)&glVariantuivEXT },
    { .name = "glVariantusvEXT", .entry = (void *)&glVariantusvEXT },
    { .name = "glVDPAUFiniNV", .entry = (void *)&glVDPAUFiniNV },
    { .name = "glVDPAUGetSurfaceivNV", .entry = (void *)&glVDPAUGetSurfaceivNV },
    { .name = "glVDPAUInitNV", .entry = (void *)&glVDPAUInitNV },
    { .name = "glVDPAUIsSurfaceNV", .entry = (void *)&glVDPAUIsSurfaceNV },
    { .name = "glVDPAUMapSurfacesNV", .entry = (void *)&glVDPAUMapSurfacesNV },
    { .name = "glVDPAURegisterOutputSurfaceNV", .entry = (void *)&glVDPAURegisterOutputSurfaceNV },
    { .name = "glVDPAURegisterVideoSurfaceNV", .entry = (void *)&glVDPAURegisterVideoSurfaceNV },
    { .name = "glVDPAURegisterVideoSurfaceWithPictureStructureNV", .entry = (void *)&glVDPAURegisterVideoSurfaceWithPictureStructureNV },
    { .name = "glVDPAUSurfaceAccessNV", .entry = (void *)&glVDPAUSurfaceAccessNV },
    { .name = "glVDPAUUnmapSurfacesNV", .entry = (void *)&glVDPAUUnmapSurfacesNV },
    { .name = "glVDPAUUnregisterSurfaceNV", .entry = (void *)&glVDPAUUnregisterSurfaceNV },
    { .name = "glVertex2bOES", .entry = (void *)&glVertex2bOES },
    { .name = "glVertex2bvOES", .entry = (void *)&glVertex2bvOES },
    { .name = "glVertex2hNV", .entry = (void *)&glVertex2hNV },
    { .name = "glVertex2hvNV", .entry = (void *)&glVertex2hvNV },
    { .name = "glVertex2xOES", .entry = (void *)&glVertex2xOES },
    { .name = "glVertex2xvOES", .entry = (void *)&glVertex2xvOES },
    { .name = "glVertex3bOES", .entry = (void *)&glVertex3bOES },
    { .name = "glVertex3bvOES", .entry = (void *)&glVertex3bvOES },
    { .name = "glVertex3hNV", .entry = (void *)&glVertex3hNV },
    { .name = "glVertex3hvNV", .entry = (void *)&glVertex3hvNV },
    { .name = "glVertex3xOES", .entry = (void *)&glVertex3xOES },
    { .name = "glVertex3xvOES", .entry = (void *)&glVertex3xvOES },
    { .name = "glVertex4bOES", .entry = (void *)&glVertex4bOES },
    { .name = "glVertex4bvOES", .entry = (void *)&glVertex4bvOES },
    { .name = "glVertex4hNV", .entry = (void *)&glVertex4hNV },
    { .name = "glVertex4hvNV", .entry = (void *)&glVertex4hvNV },
    { .name = "glVertex4xOES", .entry = (void *)&glVertex4xOES },
    { .name = "glVertex4xvOES", .entry = (void *)&glVertex4xvOES },
    { .name = "glVertexArrayAttribBinding", .entry = (void *)&glVertexArrayAttribBinding },
    { .name = "glVertexArrayAttribFormat", .entry = (void *)&glVertexArrayAttribFormat },
    { .name = "glVertexArrayAttribIFormat", .entry = (void *)&glVertexArrayAttribIFormat },
    { .name = "glVertexArrayAttribLFormat", .entry = (void *)&glVertexArrayAttribLFormat },
    { .name = "glVertexArrayBindingDivisor", .entry = (void *)&glVertexArrayBindingDivisor },
    { .name = "glVertexArrayBindVertexBufferEXT", .entry = (void *)&glVertexArrayBindVertexBufferEXT },
    { .name = "glVertexArrayColorOffsetEXT", .entry = (void *)&glVertexArrayColorOffsetEXT },
    { .name = "glVertexArrayEdgeFlagOffsetEXT", .entry = (void *)&glVertexArrayEdgeFlagOffsetEXT },
    { .name = "glVertexArrayElementBuffer", .entry = (void *)&glVertexArrayElementBuffer },
    { .name = "glVertexArrayFogCoordOffsetEXT", .entry = (void *)&glVertexArrayFogCoordOffsetEXT },
    { .name = "glVertexArrayIndexOffsetEXT", .entry = (void *)&glVertexArrayIndexOffsetEXT },
    { .name = "glVertexArrayMultiTexCoordOffsetEXT", .entry = (void *)&glVertexArrayMultiTexCoordOffsetEXT },
    { .name = "glVertexArrayNormalOffsetEXT", .entry = (void *)&glVertexArrayNormalOffsetEXT },
    { .name = "glVertexArrayParameteriAPPLE", .entry = (void *)&glVertexArrayParameteriAPPLE },
    { .name = "glVertexArrayRangeAPPLE", .entry = (void *)&glVertexArrayRangeAPPLE },
    { .name = "glVertexArrayRangeNV", .entry = (void *)&glVertexArrayRangeNV },
    { .name = "glVertexArraySecondaryColorOffsetEXT", .entry = (void *)&glVertexArraySecondaryColorOffsetEXT },
    { .name = "glVertexArrayTexCoordOffsetEXT", .entry = (void *)&glVertexArrayTexCoordOffsetEXT },
    { .name = "glVertexArrayVertexAttribBindingEXT", .entry = (void *)&glVertexArrayVertexAttribBindingEXT },
    { .name = "glVertexArrayVertexAttribDivisorEXT", .entry = (void *)&glVertexArrayVertexAttribDivisorEXT },
    { .name = "glVertexArrayVertexAttribFormatEXT", .entry = (void *)&glVertexArrayVertexAttribFormatEXT },
    { .name = "glVertexArrayVertexAttribIFormatEXT", .entry = (void *)&glVertexArrayVertexAttribIFormatEXT },
    { .name = "glVertexArrayVertexAttribIOffsetEXT", .entry = (void *)&glVertexArrayVertexAttribIOffsetEXT },
    { .name = "glVertexArrayVertexAttribLFormatEXT", .entry = (void *)&glVertexArrayVertexAttribLFormatEXT },
    { .name = "glVertexArrayVertexAttribLOffsetEXT", .entry = (void *)&glVertexArrayVertexAttribLOffsetEXT },
    { .name = "glVertexArrayVertexAttribOffsetEXT", .entry = (void *)&glVertexArrayVertexAttribOffsetEXT },
    { .name = "glVertexArrayVertexBindingDivisorEXT", .entry = (void *)&glVertexArrayVertexBindingDivisorEXT },
    { .name = "glVertexArrayVertexBuffer", .entry = (void *)&glVertexArrayVertexBuffer },
    { .name = "glVertexArrayVertexBuffers", .entry = (void *)&glVertexArrayVertexBuffers },
    { .name = "glVertexArrayVertexOffsetEXT", .entry = (void *)&glVertexArrayVertexOffsetEXT },
    { .name = "glVertexAttrib1d", .entry = (void *)&glVertexAttrib1d },
    { .name = "glVertexAttrib1dARB", .entry = (void *)&glVertexAttrib1dARB },
    { .name = "glVertexAttrib1dNV", .entry = (void *)&glVertexAttrib1dNV },
    { .name = "glVertexAttrib1dv", .entry = (void *)&glVertexAttrib1dv },
    { .name = "glVertexAttrib1dvARB", .entry = (void *)&glVertexAttrib1dvARB },
    { .name = "glVertexAttrib1dvNV", .entry = (void *)&glVertexAttrib1dvNV },
    { .name = "glVertexAttrib1f", .entry = (void *)&glVertexAttrib1f },
    { .name = "glVertexAttrib1fARB", .entry = (void *)&glVertexAttrib1fARB },
    { .name = "glVertexAttrib1fNV", .entry = (void *)&glVertexAttrib1fNV },
    { .name = "glVertexAttrib1fv", .entry = (void *)&glVertexAttrib1fv },
    { .name = "glVertexAttrib1fvARB", .entry = (void *)&glVertexAttrib1fvARB },
    { .name = "glVertexAttrib1fvNV", .entry = (void *)&glVertexAttrib1fvNV },
    { .name = "glVertexAttrib1hNV", .entry = (void *)&glVertexAttrib1hNV },
    { .name = "glVertexAttrib1hvNV", .entry = (void *)&glVertexAttrib1hvNV },
    { .name = "glVertexAttrib1s", .entry = (void *)&glVertexAttrib1s },
    { .name = "glVertexAttrib1sARB", .entry = (void *)&glVertexAttrib1sARB },
    { .name = "glVertexAttrib1sNV", .entry = (void *)&glVertexAttrib1sNV },
    { .name = "glVertexAttrib1sv", .entry = (void *)&glVertexAttrib1sv },
    { .name = "glVertexAttrib1svARB", .entry = (void *)&glVertexAttrib1svARB },
    { .name = "glVertexAttrib1svNV", .entry = (void *)&glVertexAttrib1svNV },
    { .name = "glVertexAttrib2d", .entry = (void *)&glVertexAttrib2d },
    { .name = "glVertexAttrib2dARB", .entry = (void *)&glVertexAttrib2dARB },
    { .name = "glVertexAttrib2dNV", .entry = (void *)&glVertexAttrib2dNV },
    { .name = "glVertexAttrib2dv", .entry = (void *)&glVertexAttrib2dv },
    { .name = "glVertexAttrib2dvARB", .entry = (void *)&glVertexAttrib2dvARB },
    { .name = "glVertexAttrib2dvNV", .entry = (void *)&glVertexAttrib2dvNV },
    { .name = "glVertexAttrib2f", .entry = (void *)&glVertexAttrib2f },
    { .name = "glVertexAttrib2fARB", .entry = (void *)&glVertexAttrib2fARB },
    { .name = "glVertexAttrib2fNV", .entry = (void *)&glVertexAttrib2fNV },
    { .name = "glVertexAttrib2fv", .entry = (void *)&glVertexAttrib2fv },
    { .name = "glVertexAttrib2fvARB", .entry = (void *)&glVertexAttrib2fvARB },
    { .name = "glVertexAttrib2fvNV", .entry = (void *)&glVertexAttrib2fvNV },
    { .name = "glVertexAttrib2hNV", .entry = (void *)&glVertexAttrib2hNV },
    { .name = "glVertexAttrib2hvNV", .entry = (void *)&glVertexAttrib2hvNV },
    { .name = "glVertexAttrib2s", .entry = (void *)&glVertexAttrib2s },
    { .name = "glVertexAttrib2sARB", .entry = (void *)&glVertexAttrib2sARB },
    { .name = "glVertexAttrib2sNV", .entry = (void *)&glVertexAttrib2sNV },
    { .name = "glVertexAttrib2sv", .entry = (void *)&glVertexAttrib2sv },
    { .name = "glVertexAttrib2svARB", .entry = (void *)&glVertexAttrib2svARB },
    { .name = "glVertexAttrib2svNV", .entry = (void *)&glVertexAttrib2svNV },
    { .name = "glVertexAttrib3d", .entry = (void *)&glVertexAttrib3d },
    { .name = "glVertexAttrib3dARB", .entry = (void *)&glVertexAttrib3dARB },
    { .name = "glVertexAttrib3dNV", .entry = (void *)&glVertexAttrib3dNV },
    { .name = "glVertexAttrib3dv", .entry = (void *)&glVertexAttrib3dv },
    { .name = "glVertexAttrib3dvARB", .entry = (void *)&glVertexAttrib3dvARB },
    { .name = "glVertexAttrib3dvNV", .entry = (void *)&glVertexAttrib3dvNV },
    { .name = "glVertexAttrib3f", .entry = (void *)&glVertexAttrib3f },
    { .name = "glVertexAttrib3fARB", .entry = (void *)&glVertexAttrib3fARB },
    { .name = "glVertexAttrib3fNV", .entry = (void *)&glVertexAttrib3fNV },
    { .name = "glVertexAttrib3fv", .entry = (void *)&glVertexAttrib3fv },
    { .name = "glVertexAttrib3fvARB", .entry = (void *)&glVertexAttrib3fvARB },
    { .name = "glVertexAttrib3fvNV", .entry = (void *)&glVertexAttrib3fvNV },
    { .name = "glVertexAttrib3hNV", .entry = (void *)&glVertexAttrib3hNV },
    { .name = "glVertexAttrib3hvNV", .entry = (void *)&glVertexAttrib3hvNV },
    { .name = "glVertexAttrib3s", .entry = (void *)&glVertexAttrib3s },
    { .name = "glVertexAttrib3sARB", .entry = (void *)&glVertexAttrib3sARB },
    { .name = "glVertexAttrib3sNV", .entry = (void *)&glVertexAttrib3sNV },
    { .name = "glVertexAttrib3sv", .entry = (void *)&glVertexAttrib3sv },
    { .name = "glVertexAttrib3svARB", .entry = (void *)&glVertexAttrib3svARB },
    { .name = "glVertexAttrib3svNV", .entry = (void *)&glVertexAttrib3svNV },
    { .name = "glVertexAttrib4bv", .entry = (void *)&glVertexAttrib4bv },
    { .name = "glVertexAttrib4bvARB", .entry = (void *)&glVertexAttrib4bvARB },
    { .name = "glVertexAttrib4d", .entry = (void *)&glVertexAttrib4d },
    { .name = "glVertexAttrib4dARB", .entry = (void *)&glVertexAttrib4dARB },
    { .name = "glVertexAttrib4dNV", .entry = (void *)&glVertexAttrib4dNV },
    { .name = "glVertexAttrib4dv", .entry = (void *)&glVertexAttrib4dv },
    { .name = "glVertexAttrib4dvARB", .entry = (void *)&glVertexAttrib4dvARB },
    { .name = "glVertexAttrib4dvNV", .entry = (void *)&glVertexAttrib4dvNV },
    { .name = "glVertexAttrib4f", .entry = (void *)&glVertexAttrib4f },
    { .name = "glVertexAttrib4fARB", .entry = (void *)&glVertexAttrib4fARB },
    { .name = "glVertexAttrib4fNV", .entry = (void *)&glVertexAttrib4fNV },
    { .name = "glVertexAttrib4fv", .entry = (void *)&glVertexAttrib4fv },
    { .name = "glVertexAttrib4fvARB", .entry = (void *)&glVertexAttrib4fvARB },
    { .name = "glVertexAttrib4fvNV", .entry = (void *)&glVertexAttrib4fvNV },
    { .name = "glVertexAttrib4hNV", .entry = (void *)&glVertexAttrib4hNV },
    { .name = "glVertexAttrib4hvNV", .entry = (void *)&glVertexAttrib4hvNV },
    { .name = "glVertexAttrib4iv", .entry = (void *)&glVertexAttrib4iv },
    { .name = "glVertexAttrib4ivARB", .entry = (void *)&glVertexAttrib4ivARB },
    { .name = "glVertexAttrib4Nbv", .entry = (void *)&glVertexAttrib4Nbv },
    { .name = "glVertexAttrib4NbvARB", .entry = (void *)&glVertexAttrib4NbvARB },
    { .name = "glVertexAttrib4Niv", .entry = (void *)&glVertexAttrib4Niv },
    { .name = "glVertexAttrib4NivARB", .entry = (void *)&glVertexAttrib4NivARB },
    { .name = "glVertexAttrib4Nsv", .entry = (void *)&glVertexAttrib4Nsv },
    { .name = "glVertexAttrib4NsvARB", .entry = (void *)&glVertexAttrib4NsvARB },
    { .name = "glVertexAttrib4Nub", .entry = (void *)&glVertexAttrib4Nub },
    { .name = "glVertexAttrib4NubARB", .entry = (void *)&glVertexAttrib4NubARB },
    { .name = "glVertexAttrib4Nubv", .entry = (void *)&glVertexAttrib4Nubv },
    { .name = "glVertexAttrib4NubvARB", .entry = (void *)&glVertexAttrib4NubvARB },
    { .name = "glVertexAttrib4Nuiv", .entry = (void *)&glVertexAttrib4Nuiv },
    { .name = "glVertexAttrib4NuivARB", .entry = (void *)&glVertexAttrib4NuivARB },
    { .name = "glVertexAttrib4Nusv", .entry = (void *)&glVertexAttrib4Nusv },
    { .name = "glVertexAttrib4NusvARB", .entry = (void *)&glVertexAttrib4NusvARB },
    { .name = "glVertexAttrib4s", .entry = (void *)&glVertexAttrib4s },
    { .name = "glVertexAttrib4sARB", .entry = (void *)&glVertexAttrib4sARB },
    { .name = "glVertexAttrib4sNV", .entry = (void *)&glVertexAttrib4sNV },
    { .name = "glVertexAttrib4sv", .entry = (void *)&glVertexAttrib4sv },
    { .name = "glVertexAttrib4svARB", .entry = (void *)&glVertexAttrib4svARB },
    { .name = "glVertexAttrib4svNV", .entry = (void *)&glVertexAttrib4svNV },
    { .name = "glVertexAttrib4ubNV", .entry = (void *)&glVertexAttrib4ubNV },
    { .name = "glVertexAttrib4ubv", .entry = (void *)&glVertexAttrib4ubv },
    { .name = "glVertexAttrib4ubvARB", .entry = (void *)&glVertexAttrib4ubvARB },
    { .name = "glVertexAttrib4ubvNV", .entry = (void *)&glVertexAttrib4ubvNV },
    { .name = "glVertexAttrib4uiv", .entry = (void *)&glVertexAttrib4uiv },
    { .name = "glVertexAttrib4uivARB", .entry = (void *)&glVertexAttrib4uivARB },
    { .name = "glVertexAttrib4usv", .entry = (void *)&glVertexAttrib4usv },
    { .name = "glVertexAttrib4usvARB", .entry = (void *)&glVertexAttrib4usvARB },
    { .name = "glVertexAttribArrayObjectATI", .entry = (void *)&glVertexAttribArrayObjectATI },
    { .name = "glVertexAttribBinding", .entry = (void *)&glVertexAttribBinding },
    { .name = "glVertexAttribDivisor", .entry = (void *)&glVertexAttribDivisor },
    { .name = "glVertexAttribDivisorARB", .entry = (void *)&glVertexAttribDivisorARB },
    { .name = "glVertexAttribFormat", .entry = (void *)&glVertexAttribFormat },
    { .name = "glVertexAttribFormatNV", .entry = (void *)&glVertexAttribFormatNV },
    { .name = "glVertexAttribI1i", .entry = (void *)&glVertexAttribI1i },
    { .name = "glVertexAttribI1iEXT", .entry = (void *)&glVertexAttribI1iEXT },
    { .name = "glVertexAttribI1iv", .entry = (void *)&glVertexAttribI1iv },
    { .name = "glVertexAttribI1ivEXT", .entry = (void *)&glVertexAttribI1ivEXT },
    { .name = "glVertexAttribI1ui", .entry = (void *)&glVertexAttribI1ui },
    { .name = "glVertexAttribI1uiEXT", .entry = (void *)&glVertexAttribI1uiEXT },
    { .name = "glVertexAttribI1uiv", .entry = (void *)&glVertexAttribI1uiv },
    { .name = "glVertexAttribI1uivEXT", .entry = (void *)&glVertexAttribI1uivEXT },
    { .name = "glVertexAttribI2i", .entry = (void *)&glVertexAttribI2i },
    { .name = "glVertexAttribI2iEXT", .entry = (void *)&glVertexAttribI2iEXT },
    { .name = "glVertexAttribI2iv", .entry = (void *)&glVertexAttribI2iv },
    { .name = "glVertexAttribI2ivEXT", .entry = (void *)&glVertexAttribI2ivEXT },
    { .name = "glVertexAttribI2ui", .entry = (void *)&glVertexAttribI2ui },
    { .name = "glVertexAttribI2uiEXT", .entry = (void *)&glVertexAttribI2uiEXT },
    { .name = "glVertexAttribI2uiv", .entry = (void *)&glVertexAttribI2uiv },
    { .name = "glVertexAttribI2uivEXT", .entry = (void *)&glVertexAttribI2uivEXT },
    { .name = "glVertexAttribI3i", .entry = (void *)&glVertexAttribI3i },
    { .name = "glVertexAttribI3iEXT", .entry = (void *)&glVertexAttribI3iEXT },
    { .name = "glVertexAttribI3iv", .entry = (void *)&glVertexAttribI3iv },
    { .name = "glVertexAttribI3ivEXT", .entry = (void *)&glVertexAttribI3ivEXT },
    { .name = "glVertexAttribI3ui", .entry = (void *)&glVertexAttribI3ui },
    { .name = "glVertexAttribI3uiEXT", .entry = (void *)&glVertexAttribI3uiEXT },
    { .name = "glVertexAttribI3uiv", .entry = (void *)&glVertexAttribI3uiv },
    { .name = "glVertexAttribI3uivEXT", .entry = (void *)&glVertexAttribI3uivEXT },
    { .name = "glVertexAttribI4bv", .entry = (void *)&glVertexAttribI4bv },
    { .name = "glVertexAttribI4bvEXT", .entry = (void *)&glVertexAttribI4bvEXT },
    { .name = "glVertexAttribI4i", .entry = (void *)&glVertexAttribI4i },
    { .name = "glVertexAttribI4iEXT", .entry = (void *)&glVertexAttribI4iEXT },
    { .name = "glVertexAttribI4iv", .entry = (void *)&glVertexAttribI4iv },
    { .name = "glVertexAttribI4ivEXT", .entry = (void *)&glVertexAttribI4ivEXT },
    { .name = "glVertexAttribI4sv", .entry = (void *)&glVertexAttribI4sv },
    { .name = "glVertexAttribI4svEXT", .entry = (void *)&glVertexAttribI4svEXT },
    { .name = "glVertexAttribI4ubv", .entry = (void *)&glVertexAttribI4ubv },
    { .name = "glVertexAttribI4ubvEXT", .entry = (void *)&glVertexAttribI4ubvEXT },
    { .name = "glVertexAttribI4ui", .entry = (void *)&glVertexAttribI4ui },
    { .name = "glVertexAttribI4uiEXT", .entry = (void *)&glVertexAttribI4uiEXT },
    { .name = "glVertexAttribI4uiv", .entry = (void *)&glVertexAttribI4uiv },
    { .name = "glVertexAttribI4uivEXT", .entry = (void *)&glVertexAttribI4uivEXT },
    { .name = "glVertexAttribI4usv", .entry = (void *)&glVertexAttribI4usv },
    { .name = "glVertexAttribI4usvEXT", .entry = (void *)&glVertexAttribI4usvEXT },
    { .name = "glVertexAttribIFormat", .entry = (void *)&glVertexAttribIFormat },
    { .name = "glVertexAttribIFormatNV", .entry = (void *)&glVertexAttribIFormatNV },
    { .name = "glVertexAttribIPointer", .entry = (void *)&glVertexAttribIPointer },
    { .name = "glVertexAttribIPointerEXT", .entry = (void *)&glVertexAttribIPointerEXT },
    { .name = "glVertexAttribL1d", .entry = (void *)&glVertexAttribL1d },
    { .name = "glVertexAttribL1dEXT", .entry = (void *)&glVertexAttribL1dEXT },
    { .name = "glVertexAttribL1dv", .entry = (void *)&glVertexAttribL1dv },
    { .name = "glVertexAttribL1dvEXT", .entry = (void *)&glVertexAttribL1dvEXT },
    { .name = "glVertexAttribL1i64NV", .entry = (void *)&glVertexAttribL1i64NV },
    { .name = "glVertexAttribL1i64vNV", .entry = (void *)&glVertexAttribL1i64vNV },
    { .name = "glVertexAttribL1ui64ARB", .entry = (void *)&glVertexAttribL1ui64ARB },
    { .name = "glVertexAttribL1ui64NV", .entry = (void *)&glVertexAttribL1ui64NV },
    { .name = "glVertexAttribL1ui64vARB", .entry = (void *)&glVertexAttribL1ui64vARB },
    { .name = "glVertexAttribL1ui64vNV", .entry = (void *)&glVertexAttribL1ui64vNV },
    { .name = "glVertexAttribL2d", .entry = (void *)&glVertexAttribL2d },
    { .name = "glVertexAttribL2dEXT", .entry = (void *)&glVertexAttribL2dEXT },
    { .name = "glVertexAttribL2dv", .entry = (void *)&glVertexAttribL2dv },
    { .name = "glVertexAttribL2dvEXT", .entry = (void *)&glVertexAttribL2dvEXT },
    { .name = "glVertexAttribL2i64NV", .entry = (void *)&glVertexAttribL2i64NV },
    { .name = "glVertexAttribL2i64vNV", .entry = (void *)&glVertexAttribL2i64vNV },
    { .name = "glVertexAttribL2ui64NV", .entry = (void *)&glVertexAttribL2ui64NV },
    { .name = "glVertexAttribL2ui64vNV", .entry = (void *)&glVertexAttribL2ui64vNV },
    { .name = "glVertexAttribL3d", .entry = (void *)&glVertexAttribL3d },
    { .name = "glVertexAttribL3dEXT", .entry = (void *)&glVertexAttribL3dEXT },
    { .name = "glVertexAttribL3dv", .entry = (void *)&glVertexAttribL3dv },
    { .name = "glVertexAttribL3dvEXT", .entry = (void *)&glVertexAttribL3dvEXT },
    { .name = "glVertexAttribL3i64NV", .entry = (void *)&glVertexAttribL3i64NV },
    { .name = "glVertexAttribL3i64vNV", .entry = (void *)&glVertexAttribL3i64vNV },
    { .name = "glVertexAttribL3ui64NV", .entry = (void *)&glVertexAttribL3ui64NV },
    { .name = "glVertexAttribL3ui64vNV", .entry = (void *)&glVertexAttribL3ui64vNV },
    { .name = "glVertexAttribL4d", .entry = (void *)&glVertexAttribL4d },
    { .name = "glVertexAttribL4dEXT", .entry = (void *)&glVertexAttribL4dEXT },
    { .name = "glVertexAttribL4dv", .entry = (void *)&glVertexAttribL4dv },
    { .name = "glVertexAttribL4dvEXT", .entry = (void *)&glVertexAttribL4dvEXT },
    { .name = "glVertexAttribL4i64NV", .entry = (void *)&glVertexAttribL4i64NV },
    { .name = "glVertexAttribL4i64vNV", .entry = (void *)&glVertexAttribL4i64vNV },
    { .name = "glVertexAttribL4ui64NV", .entry = (void *)&glVertexAttribL4ui64NV },
    { .name = "glVertexAttribL4ui64vNV", .entry = (void *)&glVertexAttribL4ui64vNV },
    { .name = "glVertexAttribLFormat", .entry = (void *)&glVertexAttribLFormat },
    { .name = "glVertexAttribLFormatNV", .entry = (void *)&glVertexAttribLFormatNV },
    { .name = "glVertexAttribLPointer", .entry = (void *)&glVertexAttribLPointer },
    { .name = "glVertexAttribLPointerEXT", .entry = (void *)&glVertexAttribLPointerEXT },
    { .name = "glVertexAttribP1ui", .entry = (void *)&glVertexAttribP1ui },
    { .name = "glVertexAttribP1uiv", .entry = (void *)&glVertexAttribP1uiv },
    { .name = "glVertexAttribP2ui", .entry = (void *)&glVertexAttribP2ui },
    { .name = "glVertexAttribP2uiv", .entry = (void *)&glVertexAttribP2uiv },
    { .name = "glVertexAttribP3ui", .entry = (void *)&glVertexAttribP3ui },
    { .name = "glVertexAttribP3uiv", .entry = (void *)&glVertexAttribP3uiv },
    { .name = "glVertexAttribP4ui", .entry = (void *)&glVertexAttribP4ui },
    { .name = "glVertexAttribP4uiv", .entry = (void *)&glVertexAttribP4uiv },
    { .name = "glVertexAttribParameteriAMD", .entry = (void *)&glVertexAttribParameteriAMD },
    { .name = "glVertexAttribPointer", .entry = (void *)&glVertexAttribPointer },
    { .name = "glVertexAttribPointerARB", .entry = (void *)&glVertexAttribPointerARB },
    { .name = "glVertexAttribPointerNV", .entry = (void *)&glVertexAttribPointerNV },
    { .name = "glVertexAttribs1dvNV", .entry = (void *)&glVertexAttribs1dvNV },
    { .name = "glVertexAttribs1fvNV", .entry = (void *)&glVertexAttribs1fvNV },
    { .name = "glVertexAttribs1hvNV", .entry = (void *)&glVertexAttribs1hvNV },
    { .name = "glVertexAttribs1svNV", .entry = (void *)&glVertexAttribs1svNV },
    { .name = "glVertexAttribs2dvNV", .entry = (void *)&glVertexAttribs2dvNV },
    { .name = "glVertexAttribs2fvNV", .entry = (void *)&glVertexAttribs2fvNV },
    { .name = "glVertexAttribs2hvNV", .entry = (void *)&glVertexAttribs2hvNV },
    { .name = "glVertexAttribs2svNV", .entry = (void *)&glVertexAttribs2svNV },
    { .name = "glVertexAttribs3dvNV", .entry = (void *)&glVertexAttribs3dvNV },
    { .name = "glVertexAttribs3fvNV", .entry = (void *)&glVertexAttribs3fvNV },
    { .name = "glVertexAttribs3hvNV", .entry = (void *)&glVertexAttribs3hvNV },
    { .name = "glVertexAttribs3svNV", .entry = (void *)&glVertexAttribs3svNV },
    { .name = "glVertexAttribs4dvNV", .entry = (void *)&glVertexAttribs4dvNV },
    { .name = "glVertexAttribs4fvNV", .entry = (void *)&glVertexAttribs4fvNV },
    { .name = "glVertexAttribs4hvNV", .entry = (void *)&glVertexAttribs4hvNV },
    { .name = "glVertexAttribs4svNV", .entry = (void *)&glVertexAttribs4svNV },
    { .name = "glVertexAttribs4ubvNV", .entry = (void *)&glVertexAttribs4ubvNV },
    { .name = "glVertexBindingDivisor", .entry = (void *)&glVertexBindingDivisor },
    { .name = "glVertexBlendARB", .entry = (void *)&glVertexBlendARB },
    { .name = "glVertexBlendEnvfATI", .entry = (void *)&glVertexBlendEnvfATI },
    { .name = "glVertexBlendEnviATI", .entry = (void *)&glVertexBlendEnviATI },
    { .name = "glVertexFormatNV", .entry = (void *)&glVertexFormatNV },
    { .name = "glVertexP2ui", .entry = (void *)&glVertexP2ui },
    { .name = "glVertexP2uiv", .entry = (void *)&glVertexP2uiv },
    { .name = "glVertexP3ui", .entry = (void *)&glVertexP3ui },
    { .name = "glVertexP3uiv", .entry = (void *)&glVertexP3uiv },
    { .name = "glVertexP4ui", .entry = (void *)&glVertexP4ui },
    { .name = "glVertexP4uiv", .entry = (void *)&glVertexP4uiv },
    { .name = "glVertexPointerEXT", .entry = (void *)&glVertexPointerEXT },
    { .name = "glVertexPointerListIBM", .entry = (void *)&glVertexPointerListIBM },
    { .name = "glVertexPointervINTEL", .entry = (void *)&glVertexPointervINTEL },
    { .name = "glVertexStream1dATI", .entry = (void *)&glVertexStream1dATI },
    { .name = "glVertexStream1dvATI", .entry = (void *)&glVertexStream1dvATI },
    { .name = "glVertexStream1fATI", .entry = (void *)&glVertexStream1fATI },
    { .name = "glVertexStream1fvATI", .entry = (void *)&glVertexStream1fvATI },
    { .name = "glVertexStream1iATI", .entry = (void *)&glVertexStream1iATI },
    { .name = "glVertexStream1ivATI", .entry = (void *)&glVertexStream1ivATI },
    { .name = "glVertexStream1sATI", .entry = (void *)&glVertexStream1sATI },
    { .name = "glVertexStream1svATI", .entry = (void *)&glVertexStream1svATI },
    { .name = "glVertexStream2dATI", .entry = (void *)&glVertexStream2dATI },
    { .name = "glVertexStream2dvATI", .entry = (void *)&glVertexStream2dvATI },
    { .name = "glVertexStream2fATI", .entry = (void *)&glVertexStream2fATI },
    { .name = "glVertexStream2fvATI", .entry = (void *)&glVertexStream2fvATI },
    { .name = "glVertexStream2iATI", .entry = (void *)&glVertexStream2iATI },
    { .name = "glVertexStream2ivATI", .entry = (void *)&glVertexStream2ivATI },
    { .name = "glVertexStream2sATI", .entry = (void *)&glVertexStream2sATI },
    { .name = "glVertexStream2svATI", .entry = (void *)&glVertexStream2svATI },
    { .name = "glVertexStream3dATI", .entry = (void *)&glVertexStream3dATI },
    { .name = "glVertexStream3dvATI", .entry = (void *)&glVertexStream3dvATI },
    { .name = "glVertexStream3fATI", .entry = (void *)&glVertexStream3fATI },
    { .name = "glVertexStream3fvATI", .entry = (void *)&glVertexStream3fvATI },
    { .name = "glVertexStream3iATI", .entry = (void *)&glVertexStream3iATI },
    { .name = "glVertexStream3ivATI", .entry = (void *)&glVertexStream3ivATI },
    { .name = "glVertexStream3sATI", .entry = (void *)&glVertexStream3sATI },
    { .name = "glVertexStream3svATI", .entry = (void *)&glVertexStream3svATI },
    { .name = "glVertexStream4dATI", .entry = (void *)&glVertexStream4dATI },
    { .name = "glVertexStream4dvATI", .entry = (void *)&glVertexStream4dvATI },
    { .name = "glVertexStream4fATI", .entry = (void *)&glVertexStream4fATI },
    { .name = "glVertexStream4fvATI", .entry = (void *)&glVertexStream4fvATI },
    { .name = "glVertexStream4iATI", .entry = (void *)&glVertexStream4iATI },
    { .name = "glVertexStream4ivATI", .entry = (void *)&glVertexStream4ivATI },
    { .name = "glVertexStream4sATI", .entry = (void *)&glVertexStream4sATI },
    { .name = "glVertexStream4svATI", .entry = (void *)&glVertexStream4svATI },
    { .name = "glVertexWeightfEXT", .entry = (void *)&glVertexWeightfEXT },
    { .name = "glVertexWeightfvEXT", .entry = (void *)&glVertexWeightfvEXT },
    { .name = "glVertexWeighthNV", .entry = (void *)&glVertexWeighthNV },
    { .name = "glVertexWeighthvNV", .entry = (void *)&glVertexWeighthvNV },
    { .name = "glVertexWeightPointerEXT", .entry = (void *)&glVertexWeightPointerEXT },
    { .name = "glVideoCaptureNV", .entry = (void *)&glVideoCaptureNV },
    { .name = "glVideoCaptureStreamParameterdvNV", .entry = (void *)&glVideoCaptureStreamParameterdvNV },
    { .name = "glVideoCaptureStreamParameterfvNV", .entry = (void *)&glVideoCaptureStreamParameterfvNV },
    { .name = "glVideoCaptureStreamParameterivNV", .entry = (void *)&glVideoCaptureStreamParameterivNV },
    { .name = "glViewportArrayv", .entry = (void *)&glViewportArrayv },
    { .name = "glViewportIndexedf", .entry = (void *)&glViewportIndexedf },
    { .name = "glViewportIndexedfv", .entry = (void *)&glViewportIndexedfv },
    { .name = "glViewportPositionWScaleNV", .entry = (void *)&glViewportPositionWScaleNV },
    { .name = "glViewportSwizzleNV", .entry = (void *)&glViewportSwizzleNV },
    { .name = "glWaitSemaphoreEXT", .entry = (void *)&glWaitSemaphoreEXT },
    { .name = "glWaitSync", .entry = (void *)&glWaitSync },
    { .name = "glWaitVkSemaphoreNV", .entry = (void *)&glWaitVkSemaphoreNV },
    { .name = "glWeightbvARB", .entry = (void *)&glWeightbvARB },
    { .name = "glWeightdvARB", .entry = (void *)&glWeightdvARB },
    { .name = "glWeightfvARB", .entry = (void *)&glWeightfvARB },
    { .name = "glWeightivARB", .entry = (void *)&glWeightivARB },
    { .name = "glWeightPathsNV", .entry = (void *)&glWeightPathsNV },
    { .name = "glWeightPointerARB", .entry = (void *)&glWeightPointerARB },
    { .name = "glWeightsvARB", .entry = (void *)&glWeightsvARB },
    { .name = "glWeightubvARB", .entry = (void *)&glWeightubvARB },
    { .name = "glWeightuivARB", .entry = (void *)&glWeightuivARB },
    { .name = "glWeightusvARB", .entry = (void *)&glWeightusvARB },
    { .name = "glWindowPos2d", .entry = (void *)&glWindowPos2d },
    { .name = "glWindowPos2dARB", .entry = (void *)&glWindowPos2dARB },
    { .name = "glWindowPos2dMESA", .entry = (void *)&glWindowPos2dMESA },
    { .name = "glWindowPos2dv", .entry = (void *)&glWindowPos2dv },
    { .name = "glWindowPos2dvARB", .entry = (void *)&glWindowPos2dvARB },
    { .name = "glWindowPos2dvMESA", .entry = (void *)&glWindowPos2dvMESA },
    { .name = "glWindowPos2f", .entry = (void *)&glWindowPos2f },
    { .name = "glWindowPos2fARB", .entry = (void *)&glWindowPos2fARB },
    { .name = "glWindowPos2fMESA", .entry = (void *)&glWindowPos2fMESA },
    { .name = "glWindowPos2fv", .entry = (void *)&glWindowPos2fv },
    { .name = "glWindowPos2fvARB", .entry = (void *)&glWindowPos2fvARB },
    { .name = "glWindowPos2fvMESA", .entry = (void *)&glWindowPos2fvMESA },
    { .name = "glWindowPos2i", .entry = (void *)&glWindowPos2i },
    { .name = "glWindowPos2iARB", .entry = (void *)&glWindowPos2iARB },
    { .name = "glWindowPos2iMESA", .entry = (void *)&glWindowPos2iMESA },
    { .name = "glWindowPos2iv", .entry = (void *)&glWindowPos2iv },
    { .name = "glWindowPos2ivARB", .entry = (void *)&glWindowPos2ivARB },
    { .name = "glWindowPos2ivMESA", .entry = (void *)&glWindowPos2ivMESA },
    { .name = "glWindowPos2s", .entry = (void *)&glWindowPos2s },
    { .name = "glWindowPos2sARB", .entry = (void *)&glWindowPos2sARB },
    { .name = "glWindowPos2sMESA", .entry = (void *)&glWindowPos2sMESA },
    { .name = "glWindowPos2sv", .entry = (void *)&glWindowPos2sv },
    { .name = "glWindowPos2svARB", .entry = (void *)&glWindowPos2svARB },
    { .name = "glWindowPos2svMESA", .entry = (void *)&glWindowPos2svMESA },
    { .name = "glWindowPos3d", .entry = (void *)&glWindowPos3d },
    { .name = "glWindowPos3dARB", .entry = (void *)&glWindowPos3dARB },
    { .name = "glWindowPos3dMESA", .entry = (void *)&glWindowPos3dMESA },
    { .name = "glWindowPos3dv", .entry = (void *)&glWindowPos3dv },
    { .name = "glWindowPos3dvARB", .entry = (void *)&glWindowPos3dvARB },
    { .name = "glWindowPos3dvMESA", .entry = (void *)&glWindowPos3dvMESA },
    { .name = "glWindowPos3f", .entry = (void *)&glWindowPos3f },
    { .name = "glWindowPos3fARB", .entry = (void *)&glWindowPos3fARB },
    { .name = "glWindowPos3fMESA", .entry = (void *)&glWindowPos3fMESA },
    { .name = "glWindowPos3fv", .entry = (void *)&glWindowPos3fv },
    { .name = "glWindowPos3fvARB", .entry = (void *)&glWindowPos3fvARB },
    { .name = "glWindowPos3fvMESA", .entry = (void *)&glWindowPos3fvMESA },
    { .name = "glWindowPos3i", .entry = (void *)&glWindowPos3i },
    { .name = "glWindowPos3iARB", .entry = (void *)&glWindowPos3iARB },
    { .name = "glWindowPos3iMESA", .entry = (void *)&glWindowPos3iMESA },
    { .name = "glWindowPos3iv", .entry = (void *)&glWindowPos3iv },
    { .name = "glWindowPos3ivARB", .entry = (void *)&glWindowPos3ivARB },
    { .name = "glWindowPos3ivMESA", .entry = (void *)&glWindowPos3ivMESA },
    { .name = "glWindowPos3s", .entry = (void *)&glWindowPos3s },
    { .name = "glWindowPos3sARB", .entry = (void *)&glWindowPos3sARB },
    { .name = "glWindowPos3sMESA", .entry = (void *)&glWindowPos3sMESA },
    { .name = "glWindowPos3sv", .entry = (void *)&glWindowPos3sv },
    { .name = "glWindowPos3svARB", .entry = (void *)&glWindowPos3svARB },
    { .name = "glWindowPos3svMESA", .entry = (void *)&glWindowPos3svMESA },
    { .name = "glWindowPos4dMESA", .entry = (void *)&glWindowPos4dMESA },
    { .name = "glWindowPos4dvMESA", .entry = (void *)&glWindowPos4dvMESA },
    { .name = "glWindowPos4fMESA", .entry = (void *)&glWindowPos4fMESA },
    { .name = "glWindowPos4fvMESA", .entry = (void *)&glWindowPos4fvMESA },
    { .name = "glWindowPos4iMESA", .entry = (void *)&glWindowPos4iMESA },
    { .name = "glWindowPos4ivMESA", .entry = (void *)&glWindowPos4ivMESA },
    { .name = "glWindowPos4sMESA", .entry = (void *)&glWindowPos4sMESA },
    { .name = "glWindowPos4svMESA", .entry = (void *)&glWindowPos4svMESA },
    { .name = "glWindowRectanglesEXT", .entry = (void *)&glWindowRectanglesEXT },
    { .name = "glWriteMaskEXT", .entry = (void *)&glWriteMaskEXT },
    { 0, 0 },


/* -------- generated by mkmexttbl END -------- */

    };

    int i;
    for (i = 0; ExtNameEntryTbl[i].entry; i++) {
        if (!strncmp(ExtNameEntryTbl[i].name, name, 64))
            break;
    }
    return (uint32_t)ExtNameEntryTbl[i].entry;
}

#define WGL_FUNCP(a) \
    uint32_t *funcp = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2]; \
    uint32_t *argsp = funcp + (ALIGNED(sizeof(a)) >> 2); \
    (void)argsp; \
    memcpy(funcp, a, sizeof(a))

#define WGL_FUNCP_RET(a) \
    (void)a; \
    a = argsp[0]

static uint32_t PT_CALL wglSwapIntervalEXT (uint32_t arg0)
{
    uint32_t ret;
    struct mglOptions cfg;
    parse_options(&cfg);
    WGL_FUNCP("wglSwapIntervalEXT");
    argsp[0] = (cfg.vsyncOff)? 0:arg0;
    swapFps = (argsp[0] == 0)? swapFps:0xFEU;
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    WGL_FUNCP_RET(ret);
    return ret;
}

static uint32_t PT_CALL wglGetSwapIntervalEXT (void)
{
    uint32_t ret;
    WGL_FUNCP("wglGetSwapIntervalEXT");
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    WGL_FUNCP_RET(ret);
    return ret;
}

static uint32_t PT_CALL wglGetExtensionsStringARB(uint32_t arg0)
{
    static char wstr[PAGE_SIZE];
    static const char *wstrtbl[] = {
        wstr
    };
    WGL_FUNCP("wglGetExtensionsStringARB");
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    strncpy((char *)wstrtbl[0], (char *)&mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2], PAGE_SIZE);
    fltrxstr(wstrtbl[0], PAGE_SIZE - 1, "+WGL_");
    //DPRINTF("GetExtensionsStringARB %s", wstrtbl[0]);
    return (uint32_t)wstrtbl[0];
}
static uint32_t PT_CALL wglGetExtensionsStringEXT(void)
{
    return wglGetExtensionsStringARB(0);
}

/* WGL_ARB_pixel_format */
static BOOL WINAPI
wglGetPixelFormatAttribivARB (HDC hdc,
			      int iPixelFormat,
			      int iLayerPlane,
			      UINT nAttributes,
			      const int *piAttributes,
			      int *piValues)
{
  uint32_t ret;
  WGL_FUNCP("wglGetPixelFormatAttribivARB");
  argsp[0] = iPixelFormat; argsp[1] = iLayerPlane; argsp[2] = nAttributes;
  memcpy(&argsp[4], piAttributes, nAttributes*sizeof(int));
  //DPRINTF("GetPixelFormatAttribivARB");
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  if (ret && piValues)
      memcpy(piValues, &argsp[2], nAttributes*sizeof(int));
  return ret;
}

static BOOL WINAPI
wglGetPixelFormatAttribfvARB (HDC hdc,
			      int iPixelFormat,
			      int iLayerPlane,
			      UINT nAttributes,
			      const int *piAttributes,
			      FLOAT *pfValues)
{
  uint32_t ret;
  WGL_FUNCP("wglGetPixelFormatAttribfvARB");
  argsp[0] = iPixelFormat; argsp[1] = iLayerPlane; argsp[2] = nAttributes;
  memcpy(&argsp[4], piAttributes, nAttributes*sizeof(int));
  //DPRINTF("GetPixelFormatAttribfvARB");
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  if (ret && pfValues)
      memcpy(pfValues, &argsp[2], nAttributes*sizeof(float));
  return ret;
}

static BOOL WINAPI
wglChoosePixelFormatARB (HDC hdc,
			 const int *piAttribIList,
			 const FLOAT *pfAttribFList,
			 UINT nMaxFormats,
			 int *piFormats,
			 UINT *nNumFormats)
{
  uint32_t i, ret;
  WGL_FUNCP("wglChoosePixelFormatARB");
  for (i = 0; piAttribIList[i]; i+=2) {
      argsp[i] = piAttribIList[i];
      argsp[i+1] = piAttribIList[i+1];
  }
  argsp[i] = 0; argsp[i+1] = 0;
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  if (ret && piFormats && nNumFormats) {
      *piFormats = argsp[1];
      *nNumFormats = 1;
  }
  DPRINTF("ChoosePixelFormatARB() fmt %02x", *piFormats);
  return ret;
}

/* WGL_ARB_create_context */
static int level;
static HGLRC WINAPI COMPACT
wglCreateContextAttribsARB(HDC hDC,
                           HGLRC hShareContext,
                           const int *attribList)
{
  uint32_t i, ret;
  WGL_FUNCP("wglCreateContextAttribsARB");
  argsp[0] = (uint32_t) hShareContext;
  for (i = 0; attribList[i]; i+=2) {
      argsp[i+2] = attribList[i];
      argsp[i+3] = attribList[i+1];
  }
  argsp[i+2] = 0; argsp[i+3] = 0;
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  if (ret) {
      if (currGLRC && hShareContext) {
          level++;
          level = (level % MAX_LVLCNTX)? (level % MAX_LVLCNTX):1;
      }
      else {
          currDC = (uint32_t)hDC;
          currGLRC = 0;
          level = 0;
          InitClientStates();
          GLwnd = WindowFromDC(hDC);
      }
  }
  return (ret)? (HGLRC)(MESAGL_MAGIC - level):0;
}

/* WGL_ARB_render_texture */
typedef void *HPBUFFERARB;
static BOOL WINAPI
wglBindTexImageARB (HPBUFFERARB hPbuffer, int iBuffer)
{
  uint32_t ret;
  WGL_FUNCP("wglBindTexImageARB");
  argsp[0] = (uint32_t)hPbuffer; argsp[1] = iBuffer;
  //DPRINTF("BindTexImageARB %x", iBuffer);
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  return ret;
}

static BOOL WINAPI
wglReleaseTexImageARB (HPBUFFERARB hPbuffer, int iBuffer)
{
  uint32_t ret;
  WGL_FUNCP("wglReleaseTexImageARB");
  argsp[0] = (uint32_t)hPbuffer; argsp[1] = iBuffer;
  //DPRINTF("ReleaseTexImageARB %x", iBuffer);
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  return ret;
}

static BOOL WINAPI
wglSetPbufferAttribARB (HPBUFFERARB hPbuffer,
			const int *piAttribList)
{
  uint32_t i, ret;
  WGL_FUNCP("wglSetPbufferAttribARB");
  argsp[0] = (uint32_t)hPbuffer;
  for (i = 0; piAttribList[i]; i+=2) {
      argsp[i+2] = piAttribList[i];
      argsp[i+3] = piAttribList[i+1];
  }
  argsp[i+2] = 0; argsp[i+3] = 0;
  //DPRINTF("SetPbufferAttribARB");
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  return ret;
}

/* WGL_ARB_pbuffer */
static HPBUFFERARB WINAPI
wglCreatePbufferARB (HDC hDC,
		     int iPixelFormat,
		     int iWidth,
		     int iHeight,
		     const int *piAttribList)
{
  uint32_t i, ret;
  WGL_FUNCP("wglCreatePbufferARB");
  argsp[0] = iPixelFormat; argsp[1] = iWidth; argsp[2] = iHeight;
  for (i = 0; piAttribList[i]; i+=2) {
      argsp[i+4] = piAttribList[i];
      argsp[i+5] = piAttribList[i+1];
  }
  argsp[i+4] = 0; argsp[i+5] = 0;
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  i = argsp[1] & (MAX_PBUFFER - 1);
  //DPRINTF("CreatePbufferARB %p %02x %d %d pbuf %02x", hDC, iPixelFormat, iWidth, iHeight, argsp[1]);
  currPB[i] = (ret)? (((MESAGL_MAGIC & 0x0FFFFFFFU) << 4) | argsp[1]):0;
  ret = currPB[i];
  return (HPBUFFERARB)ret;
}

static HDC WINAPI
wglGetPbufferDCARB (HPBUFFERARB hPbuffer)
{
  uint32_t ret = 0;
  if (((MESAGL_MAGIC & 0x0FFFFFFFU) << 4) == ((uint32_t)hPbuffer & 0xFFFFFFF0U))
      ret = ((MESAGL_HPBDC & 0xFFFFFFF0U) | ((uint32_t)hPbuffer & (MAX_PBUFFER - 1)));
  //DPRINTF("GetPbufferDCARB %p %x", hPbuffer, ret);
  return (HDC)ret;
}

static int WINAPI
wglReleasePbufferDCARB (HPBUFFERARB hPbuffer, HDC hDC)
{
  //DPRINTF("ReleasePbufferDCARB %p %p", hPbuffer, hDC);
  SetLastError(0);
  return (TRUE);
}

static BOOL WINAPI
wglDestroyPbufferARB (HPBUFFERARB hPbuffer)
{
  uint32_t ret;
  WGL_FUNCP("wglDestroyPbufferARB");
  argsp[0] = (uint32_t)hPbuffer;
  //DPRINTF("DestroyPbufferARB %p", hPbuffer);
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  if (ret)
      currPB[((uint32_t)hPbuffer & (MAX_PBUFFER - 1))] = 0;
  return ret;
}

static BOOL WINAPI
wglQueryPbufferARB (HPBUFFERARB hPbuffer,
		    int iAttribute,
		    int *piValue)
{
  uint32_t ret;
  WGL_FUNCP("wglQueryPbufferARB");
  //DPRINTF("QueryPbufferARB %p %x", hPbuffer, iAttribute);
  argsp[0] = (uint32_t)hPbuffer; argsp[1] = iAttribute;
  ptm[0xFDC >> 2] = MESAGL_MAGIC;
  WGL_FUNCP_RET(ret);
  if (ret && piValue)
      *piValue = argsp[2];
  return ret;
}

/* WGL_3DFX_gamma_control */
static BOOL WINAPI
wglGetDeviceGammaRamp3DFX(HDC hdc, LPVOID arrays)
{
    uint32_t ret;
    WGL_FUNCP("wglGetDeviceGammaRamp3DFX");
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    WGL_FUNCP_RET(ret);
    if (ret && arrays)
        memcpy(arrays, &argsp[2], 3*256*sizeof(uint16_t));
    return ret;
}

static BOOL WINAPI
wglSetDeviceGammaRamp3DFX(HDC hdc, LPVOID arrays)
{
    uint32_t ret;
    WGL_FUNCP("wglSetDeviceGammaRamp3DFX");
    memcpy(&argsp[0], arrays, 3*256*sizeof(uint16_t));
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    WGL_FUNCP_RET(ret);
    return ret;
}

/* WGL_ARB_make_current_read */
uint32_t PT_CALL mglCreateContext (uint32_t arg0);
uint32_t PT_CALL mglMakeCurrent (uint32_t arg0, uint32_t arg1);
static uint32_t WINAPI wglMakeContextCurrentARB(uint32_t arg0,
                              uint32_t arg1,
                              uint32_t arg2)
{
    uint32_t currRC, ret = 0;
    if (arg0 == arg1) {
        uint32_t i = arg0 & (MAX_PBUFFER - 1);
        if (arg0 == currDC)
            ret = mglMakeCurrent(arg0, arg2);
        else if (arg0 == ((MESAGL_HPBDC & 0xFFFFFFF0U) | i)) {
            currRC = mglCreateContext(arg0);
            ret = mglMakeCurrent(arg0, currRC);
        }
    }
    return ret;
}
static HDC WINAPI wglGetCurrentReadDCARB(VOID)
{
    return NULL;
}

/* WGL_NV_allocate_memory */
static void * WINAPI wglAllocateMemoryNV(int size,
                         float readFrequency,
                         float writeFrequency,
                         float priority)
{
    return 0;
}
static void wglFreeMemoryNV(void *pointer) { }

static void WINAPI
wglSetDeviceCursor3DFX(HCURSOR hCursor)
{
    static HCURSOR last_cur;
    ICONINFO ic;

    if (!swapCur || (last_cur == hCursor))
        return;

    BOOL (WINAPI *p_DeleteObject)(HANDLE) = (BOOL (WINAPI *)(HANDLE))
        GetProcAddress(GetModuleHandle("gdi32.dll"), "DeleteObject");
    int (WINAPI *p_GetDIBits)(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT) =
        (int (WINAPI *)(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT))
        GetProcAddress(GetModuleHandle("gdi32.dll"), "GetDIBits");
    memset(&ic, 0, sizeof(ICONINFO));
    if (p_GetDIBits && hCursor && GetIconInfo(hCursor, &ic) && !ic.fIcon) {
#define SIZE_BMPINFO (sizeof(BITMAPINFOHEADER) + sizeof(RGBQUAD[3]))
        unsigned char binfo[SIZE_BMPINFO];
        BITMAPINFO *pbmi = (BITMAPINFO *)binfo;
        HDC hdc = GetDC(GLwnd);
        HBITMAP hBmp = (ic.hbmColor)? ic.hbmColor:ic.hbmMask;
        memset(pbmi, 0, SIZE_BMPINFO);
        pbmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        pbmi->bmiHeader.biPlanes = 1;
        if (hdc && hBmp && p_GetDIBits(hdc, hBmp, 0, 0, NULL, pbmi, DIB_RGB_COLORS) &&
            (pbmi->bmiHeader.biWidth > ic.xHotspot) &&
            (pbmi->bmiHeader.biHeight > ic.yHotspot)) {
            WGL_FUNCP("wglSetDeviceCursor3DFX");
            uint32_t *data = &fbtm[(MGLFBT_SIZE - pbmi->bmiHeader.biSizeImage) >> 2];
            argsp[0] = ic.xHotspot;
            argsp[1] = ic.yHotspot;
            argsp[2] = pbmi->bmiHeader.biWidth;
            argsp[3] = pbmi->bmiHeader.biHeight;
            pbmi->bmiHeader.biHeight = 0 - pbmi->bmiHeader.biHeight;
            p_GetDIBits(hdc, hBmp, 0, argsp[3], data, pbmi, DIB_RGB_COLORS);
            ReleaseDC(GLwnd, hdc);
            if (pbmi->bmiHeader.biBitCount == 1)
                argsp[3] |= 1;
            if (pbmi->bmiHeader.biBitCount > 16) {
                int h, i;
#define ALPHA_MASK 0xFF000000U
                for (h = 0; h < (pbmi->bmiHeader.biSizeImage >> 2); h++)
                    if (data[h] & ALPHA_MASK) break;
#define COLOR_MASK 0x00FFFFFFU
                for (i = 0; h == (pbmi->bmiHeader.biSizeImage >> 2) && i < h; i++)
                    data[i] = (data[i] && (data[i] ^ COLOR_MASK))?
                        (data[i] | ALPHA_MASK):COLOR_MASK;
            }
            ptm[0xFDC >> 2] = MESAGL_MAGIC;
            last_cur = hCursor;
        }
    }
    if (p_DeleteObject) {
        if (ic.hbmColor)
            p_DeleteObject(ic.hbmColor);
        if (ic.hbmMask)
            p_DeleteObject(ic.hbmMask);
    }
}

static void HookPatchGamma(const uint32_t start, const uint32_t *iat, const DWORD range)
{
    DWORD oldProt;
    uint32_t addr = start, *patch = (uint32_t *)iat;
    const char fnGetGamma[] = "GetDeviceGammaRamp", fnSetGamma[] = "SetDeviceGammaRamp";

    if ((addr == (uint32_t)patch) &&
        VirtualProtect(patch, sizeof(intptr_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
        DWORD hkGet = (DWORD)GetProcAddress(GetModuleHandle("gdi32.dll"), fnGetGamma),
              hkSet = (DWORD)GetProcAddress(GetModuleHandle("gdi32.dll"), fnSetGamma);
        for (int i = 0; i < (PAGE_SIZE >> 2); i++) {
            if (hkGet && (hkGet == patch[i])) {
                HookEntryHook(&patch[i], patch[i]);
                patch[i] = (uint32_t)&wglGetDeviceGammaRamp3DFX;
                hkGet = 0;
                OHST_DMESG("..hooked %s", fnGetGamma);
            }
            if (hkSet && (hkSet == patch[i])) {
                HookEntryHook(&patch[i], patch[i]);
                patch[i] = (uint32_t)&wglSetDeviceGammaRamp3DFX;
                hkSet = 0;
                OHST_DMESG("..hooked %s", fnSetGamma);
            }
            if (!hkGet && !hkSet)
                break;
        }
        VirtualProtect(patch, sizeof(intptr_t), oldProt, &oldProt);
    }
}

void HookDeviceGammaRamp(const uint32_t caddr)
{
    uint32_t addr, *patch, range;

    if (caddr && !IsBadReadPtr((void *)(caddr - 0x06), 0x06)) {
        uint16_t *callOp = (uint16_t *)(caddr - 0x06);
        if (0x15ff == (*callOp)) {
            addr = *(uint32_t *)(caddr - 0x04);
            addr &= ~(PAGE_SIZE - 1);
            patch = (uint32_t *)addr;
            HookPatchGamma(addr, patch, PAGE_SIZE);
        }
    }
#define GLGAMMA_HOOK(mod) \
    addr = (uint32_t)GetModuleHandle(mod); \
    for (int i = 0; addr && (i < PAGE_SIZE); i+=0x04) { \
        if (0x4550U == *(uint32_t *)addr) break; \
        addr += 0x04; \
    } \
    addr = (addr && (0x4550U == *(uint32_t *)addr))? addr:0; \
    patch = (uint32_t *)(addr & ~(PAGE_SIZE - 1)); \
    range = PAGE_SIZE; \
    HookParseRange(&addr, &patch, &range); \
    HookPatchGamma(addr, patch, range - (((uint32_t)patch) & (PAGE_SIZE - 1)));
    GLGAMMA_HOOK("opengldrv.dll");
    GLGAMMA_HOOK(0);
#undef GLGAMMA_HOOK
}

BOOL WINAPI
mglDescribeLayerPlane(HDC hdc, int iPixelFormat, int iLayerPlane,
                      UINT nBytes, LPLAYERPLANEDESCRIPTOR ppfd)
{
  SetLastError(0);
  return (FALSE);
}

int WINAPI
mglGetLayerPaletteEntries(HDC hdc, int iLayerPlane, int iStart,
                          int cEntries, COLORREF *pcr)
{
  SetLastError(0);
  return (FALSE);
}

BOOL WINAPI
mglRealizeLayerPalette(HDC hdc,int iLayerPlane,BOOL bRealize)
{
  SetLastError(0);
  return(FALSE);
}

int WINAPI
mglSetLayerPaletteEntries(HDC hdc,int iLayerPlane, int iStart,
                          int cEntries, CONST COLORREF *pcr)
{
  SetLastError(0);
  return(FALSE);
}

uint32_t PT_CALL COMPACT
mglGetProcAddress (uint32_t arg0)
{
    uint32_t ret;
    uint32_t *proc = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2];
    void *fptr = 0;
    //DPRINTF("  query wglext %s", (char *)arg0);

#define FUNC_WGL_EXT(a) \
    if (!memcmp((const void *)arg0, ""#a"", sizeof(""#a""))) fptr = &a
    /* WGL_ARB_create_context */
    FUNC_WGL_EXT(wglCreateContextAttribsARB);
    FUNC_WGL_EXT(wglSwapIntervalEXT);
    FUNC_WGL_EXT(wglGetSwapIntervalEXT);
    FUNC_WGL_EXT(wglGetExtensionsStringARB);
    FUNC_WGL_EXT(wglGetExtensionsStringEXT);
    /* WGL_ARB_pixel_format */
    FUNC_WGL_EXT(wglGetPixelFormatAttribivARB);
    FUNC_WGL_EXT(wglGetPixelFormatAttribfvARB);
    FUNC_WGL_EXT(wglChoosePixelFormatARB);
    /* WGL_ARB_render_texture */
    FUNC_WGL_EXT(wglBindTexImageARB);
    FUNC_WGL_EXT(wglReleaseTexImageARB);
    FUNC_WGL_EXT(wglSetPbufferAttribARB);
    /* WGL_ARB_pbuffer */
    FUNC_WGL_EXT(wglCreatePbufferARB);
    FUNC_WGL_EXT(wglGetPbufferDCARB);
    FUNC_WGL_EXT(wglReleasePbufferDCARB);
    FUNC_WGL_EXT(wglDestroyPbufferARB);
    FUNC_WGL_EXT(wglQueryPbufferARB);
    /* WGL_3DFX_gamma_control */
    FUNC_WGL_EXT(wglGetDeviceGammaRamp3DFX);
    FUNC_WGL_EXT(wglSetDeviceGammaRamp3DFX);
    FUNC_WGL_EXT(wglSetDeviceCursor3DFX);
    /* WGL_ARB_make_current_read */
    FUNC_WGL_EXT(wglMakeContextCurrentARB);
    FUNC_WGL_EXT(wglGetCurrentReadDCARB);
    /* WGL_NV_allocate_memory */
    FUNC_WGL_EXT(wglAllocateMemoryNV);
    FUNC_WGL_EXT(wglFreeMemoryNV);
    /* GL_NV_register_combiners */
    FUNC_WGL_EXT(glCombinerInputNV);
    FUNC_WGL_EXT(glCombinerOutputNV);
    FUNC_WGL_EXT(glCombinerParameterfNV);
    FUNC_WGL_EXT(glCombinerParameterfvNV);
    FUNC_WGL_EXT(glCombinerParameteriNV);
    FUNC_WGL_EXT(glCombinerParameterivNV);
    FUNC_WGL_EXT(glFinalCombinerInputNV);
    /* GL_NV_register_combiners2 */
    FUNC_WGL_EXT(glCombinerStageParameterfvNV);
    /* GL_ARB_debug_output */
    FUNC_WGL_EXT(glDebugMessageCallbackARB);
    FUNC_WGL_EXT(glDebugMessageControlARB);
    FUNC_WGL_EXT(glDebugMessageInsertARB);
#undef FUNC_WGL_EXT

    ret = (uint32_t)fptr;
    do {
        static int once;
        if (!once) {
            once = !once;
            glGetString(GL_RENDERER);
        }
    } while(0);
    if (ret == 0) {
        strncpy((char *)proc, (char *)arg0, 64);
        ptm[0xFE0U >> 2] = MESAGL_MAGIC;
        ret = (ptm[0xFE0U >> 2] == MESAGL_MAGIC)? getExtNameEntry((char *)arg0):0;
    }
    return ret;
}

uint32_t PT_CALL COMPACT
mglCreateContext (uint32_t arg0)
{
    uint32_t i, currRC = 0;
    uint32_t *cntxDC = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2];
    if (currPixFmt == 0)
        return 0;
    i = arg0 & (MAX_PBUFFER - 1);
    cntxDC[0] = arg0;
    ptm[0xFFC >> 2] = MESAGL_MAGIC;
    if (arg0 == ((MESAGL_HPBDC & 0xFFFFFFF0U) | i)) {
        currRC = (((MESAGL_MAGIC & 0x0FFFFFFFU) << 4) | i);
    }
    if (currGLRC == 0) {
        DPRINTF("CreateContext %x", arg0);
        currDC = arg0;
        currRC = MESAGL_MAGIC;
        InitClientStates();
        GLwnd = WindowFromDC((HDC)arg0);
    }
    return currRC;
}

uint32_t PT_CALL COMPACT
mglMakeCurrent (uint32_t arg0, uint32_t arg1)
{
    static const char icdBuild[] __attribute__((aligned(16),used)) =
        __TIME__" "__DATE__" build ";
    uint32_t *ptVer = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2];
    if (!currDC && !mglCreateContext(arg0))
        return 0;
    //DPRINTF("MakeCurrent %x %x", arg0, arg1);
    ptVer[0] = arg1;
    memcpy((char *)&ptVer[1], rev_, 8);
    memcpy(((char *)&ptVer[1] + 8), icdBuild, sizeof(icdBuild));
    ptm[0xFF8 >> 2] = MESAGL_MAGIC;
    if (!currGLRC) {
        struct mglOptions cfg;
        parse_options(&cfg);
        if (cfg.useSRGB && !glIsEnabled(GL_FRAMEBUFFER_SRGB))
            glEnable(GL_FRAMEBUFFER_SRGB);
        if (cfg.vsyncOff) {
            if (wglGetSwapIntervalEXT())
                wglSwapIntervalEXT(0);
        }
        else if (cfg.swapInt && (cfg.swapInt != wglGetSwapIntervalEXT()))
            wglSwapIntervalEXT(cfg.swapInt);
        if (logpname)
            HeapFree(GetProcessHeap(), 0, logpname);
        logpname = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x2000);
        DPRINTF("%s", icdBuild);
    }
    currGLRC = (level && ((arg1 + level) == MESAGL_MAGIC))?
        (arg1 + level):((level)? MESAGL_MAGIC:arg1);
    return TRUE;
}

uint32_t PT_CALL COMPACT
mglDeleteContext (uint32_t arg0)
{
    if (level && ((arg0 + level) == MESAGL_MAGIC)) { }
    else if (!currGLRC && (arg0 == MESAGL_MAGIC)) {
        for (int i = 0; i < MAX_PBUFFER; i++) {
            if (currPB[i])
                wglDestroyPbufferARB((HPBUFFERARB)currPB[i]);
        }
        currDC = 0;
        DPRINTF("DeleteContext %x", arg0);
    }
    else
        return TRUE;

    ptm[0xFF4 >> 2] = arg0;
    return TRUE;
}

uint32_t PT_CALL mglGetCurrentDC(void)
{
    //DPRINTF("wglGetCurrentDC() %x", currDC);
    return currDC;
}

uint32_t PT_CALL mglGetCurrentContext(void)
{
    //DPRINTF("wglGetCurrentContext() %x", currGLRC);
    return ((currGLRC + level) == MESAGL_MAGIC)? (currGLRC - level):currGLRC;
}

uint32_t PT_CALL mglCopyContext(uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    WGL_FUNCP("wglCopyContext");
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    return TRUE;
}

uint32_t PT_CALL mglCreateLayerContext(uint32_t arg0, uint32_t arg1)
{
    WGL_FUNCP("wglCreateLayerContext");
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    return TRUE;
}

uint32_t PT_CALL mglShareLists(uint32_t arg0, uint32_t arg1)
{
    uint32_t ret;
    WGL_FUNCP("wglShareLists");
    argsp[0] = arg0; argsp[1] = arg1;
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    WGL_FUNCP_RET(ret);
    DPRINTF("wglShareLists %x %x ret %x", arg0, arg1, ret);
    return ret;
}
uint32_t PT_CALL mglUseFontBitmapsA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    uint32_t ret;
    WGL_FUNCP("wglUseFontBitmapsA");
    argsp[0] = arg0; argsp[1] = arg1; argsp[2] = arg2; argsp[3] = arg3; 
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    WGL_FUNCP_RET(ret);
    return ret;
}
uint32_t PT_CALL mglUseFontBitmapsW(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
    uint32_t ret;
    WGL_FUNCP("wglUseFontBitmapsW");
    argsp[0] = arg0; argsp[1] = arg1; argsp[2] = arg2; argsp[3] = arg3; 
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    WGL_FUNCP_RET(ret);
    return ret;
}
uint32_t PT_CALL mglUseFontOutlinesA(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
        uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7)
{
    WGL_FUNCP("wglUseFontOutlinesA");
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    return TRUE;
}
uint32_t PT_CALL mglUseFontOutlinesW(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3,
        uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7)
{
    WGL_FUNCP("wglUseFontOutlinesW");
    ptm[0xFDC >> 2] = MESAGL_MAGIC;
    return TRUE;
}

int WINAPI wglSwapBuffers (HDC hdc)
{
    static POINT last_pos;
    static DWORD timestamp;
    uint32_t ret, *swapRet = &mfifo[(MGLSHM_SIZE - ALIGNED(1)) >> 2];
    DWORD t = GetTickCount();
    CURSORINFO ci = { .cbSize = sizeof(CURSORINFO) };
    if (((t - timestamp) >= 16) &&
            display_device_supported() && GetCursorInfo(&ci)) {
        if (ci.flags != CURSOR_SHOWING)
            memset(&last_pos, 0, sizeof(POINT));
        else {
            RECT cr;
            LONG x_adj = ci.ptScreenPos.x, y_adj = ci.ptScreenPos.y;
            timestamp = t;
            GetClientRect(WindowFromDC(hdc), &cr);
            ScreenToClient(WindowFromDC(hdc), &ci.ptScreenPos);
            ci.ptScreenPos.x = max(0, ci.ptScreenPos.x);
            ci.ptScreenPos.y = max(0, ci.ptScreenPos.y);
            x_adj -= ci.ptScreenPos.x;
            x_adj = max(0, x_adj);
            ci.ptScreenPos.x += x_adj;
            y_adj -= ci.ptScreenPos.y;
            y_adj = max(0, y_adj);
            ci.ptScreenPos.y += y_adj;
            ci.ptScreenPos.x = MulDiv(ci.ptScreenPos.x, GetSystemMetrics(SM_CXSCREEN) - 1,
                    (cr.right - cr.left + x_adj + x_adj - 1));
            ci.ptScreenPos.y = MulDiv(ci.ptScreenPos.y, GetSystemMetrics(SM_CYSCREEN) - 1,
                    (cr.bottom - cr.top + x_adj + y_adj - 1));
            memcpy(&last_pos, &ci.ptScreenPos, sizeof(POINT));
            wglSetDeviceCursor3DFX(ci.hCursor);
        }
    }
#define CURSOR_DWORD(d,p) \
    d = ((p.x & 0x7FFFU) << 16) | (p.y & 0x7FFFU)
    CURSOR_DWORD(swapRet[1], last_pos);
    swapRet[0] = swapFps;
    ptm[0xFF0 >> 2] = MESAGL_MAGIC;
    ret = swapRet[0];
    if (ret & 0x1FEU) {
        static uint32_t nexttick;
        const uint32_t maxFPS = (ret & 0x1FEU) >> 1;
        while (GetTickCount() < nexttick)
            Sleep(0);
        nexttick = GetTickCount();
        while (nexttick >= (UINT32_MAX - (1000 / maxFPS)))
            nexttick = GetTickCount();
        nexttick += (1000 / maxFPS);
    }
    return (ret & 0x01U);
}
int WINAPI COMPACT
wgdSwapBuffers(HDC hdc) { return wglSwapBuffers(hdc); }

int WINAPI mglSwapLayerBuffers(HDC hdc, UINT arg1) { return wgdSwapBuffers(hdc); }

#define PPFD_CONFIG() \
    struct mglOptions cfg; \
    parse_options(&cfg); \
    xppfd[0] = cfg.useZERO | cfg.bltFlip | cfg.useMSAA | cfg.scalerOff | cfg.bufoAcc; \
    xppfd[1] = (cfg.dispTimerMS & 0x8000U)? (cfg.dispTimerMS & 0x7FFFU):DISPTMR_DEFAULT

int WINAPI wglChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd)
{
    uint32_t ret, ready = 0;
    uint32_t *xppfd = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2];
    PPFD_CONFIG();
    memcpy(&xppfd[2], ppfd, sizeof(PIXELFORMATDESCRIPTOR));
    ptm[0xFEC >> 2] = MESAGL_MAGIC;
    while (!ready)
        ready = ptm[0xFB8 >> 2];
    ret = ptm[0xFEC >> 2];
    return ret;
}
int WINAPI COMPACT
wgdChoosePixelFormat(HDC hdc, const PIXELFORMATDESCRIPTOR *ppfd)
{ return wglChoosePixelFormat(hdc, ppfd); }

int WINAPI wglDescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes, LPPIXELFORMATDESCRIPTOR ppfd)
{
    uint32_t ret;
    uint32_t *xppfd = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2];
    PPFD_CONFIG();
    xppfd[2] = iPixelFormat;
    xppfd[3] = nBytes;
    ptm[0xFE8 >> 2] = MESAGL_MAGIC;
    ret = ptm[0xFE8 >> 2];
    if (ret && ppfd) {
        memcpy(ppfd, xppfd, nBytes);
    }
    if ((nBytes == sizeof(int)) && ppfd)
        *(int *)ppfd = ret;
    return (ret)? 1:0;
}
int WINAPI COMPACT
wgdDescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes, LPPIXELFORMATDESCRIPTOR ppfd)
{ return wglDescribePixelFormat(hdc, iPixelFormat, nBytes, ppfd); }

int WINAPI wglGetPixelFormat(HDC hdc)
{
    static int fmt;
    if (currPixFmt) {
        if (!fmt)
            wgdDescribePixelFormat(hdc, 1, sizeof(int), (LPPIXELFORMATDESCRIPTOR)&fmt);
    }
    else
        fmt = 0;
    return fmt;
}
int WINAPI COMPACT
wgdGetPixelFormat(HDC hdc) { return wglGetPixelFormat(hdc); }

BOOL WINAPI COMPACT_FRAME
wglSetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd)
{
    uint32_t ret, *rsp, *xppfd;
    asm volatile("lea 0x04(%%ebp), %0;":"=rm"(rsp));
    ret = ((rsp[5] == rsp[1]) && (rsp[6] == rsp[2]))? rsp[4]:
          ((rsp[9] == rsp[1])? rsp[8]:rsp[0]);
    HookDeviceGammaRamp(ret);
    HookTimeGetTime(ret);
    if (!hdc && !format)
        return 0;
    if (currGLRC) {
        mglMakeCurrent(0, 0);
        mglDeleteContext(MESAGL_MAGIC);
    }
    xppfd = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2];
    xppfd[0] = format;
    xppfd[1] = (uint32_t)ptm;
    memset(&xppfd[2], 0, sizeof(PIXELFORMATDESCRIPTOR));
    if (ppfd)
        memcpy(&xppfd[2], ppfd, sizeof(PIXELFORMATDESCRIPTOR));
    ptm[0xFE4 >> 2] = MESAGL_MAGIC;
    ret = (ptm[0xFE4 >> 2] == MESAGL_MAGIC)? 1:0;
    currPixFmt = (ret)? format:0;
    return ret;
}
BOOL WINAPI COMPACT
wgdSetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd)
{ return wglSetPixelFormat(hdc, format, ppfd); }

static void mglSetAffinity(void)
{
    const char *ThreadAffinity[] = {
        "Unigine_x86",
        0,
    };
    int i;
    for (i = 0; ThreadAffinity[i]; i++) {
        if (GetModuleHandle(ThreadAffinity[i]))
            break;
    }
    DWORD affinityMask[2];
    GetProcessAffinityMask(GetCurrentProcess(), &affinityMask[0], &affinityMask[1]);
    if (ThreadAffinity[i])
        SetThreadAffinityMask(GetCurrentThread(), (1 << ((GetCurrentThreadId() >> 2) &
                        ((sizeof(DWORD) << 3) - __builtin_clz(affinityMask[0]) - 1))));
    else
        SetProcessAffinityMask(GetCurrentProcess(), (1 << ((GetCurrentProcessId() >> 2) &
                        ((sizeof(DWORD) << 3) - __builtin_clz(affinityMask[0]) - 1))));
}

LRESULT CALLBACK CallWndProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0)
        return CallNextHookEx(hHook, nCode, wParam, lParam);
    if (nCode == HC_ACTION) {
        PCWPSTRUCT cwp = (PCWPSTRUCT)lParam;
        if ((cwp->hwnd == GLwnd) && (cwp->message == WM_ACTIVATE)) {
            uint32_t *i = &mfifo[(MGLSHM_SIZE - PAGE_SIZE) >> 2];
            //DPRINTF("WM_ACTIVATE %04x", cwp->wParam);
            i[0] = (uint32_t)(cwp->wParam & 0xFFFFU);
            ptm[0xFD8 >> 2] = MESAGL_MAGIC;
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

BOOL APIENTRY DllMain( HINSTANCE hModule,
        DWORD dwReason,
        LPVOID lpReserved
        )
{
    static char cbref, *refcnt;
    uint32_t HostRet;
    TCHAR procName[2048];
    OSVERSIONINFO osInfo;
    DRVFUNC drv;
    osInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&osInfo);
    HookPatchfxCompat(osInfo.dwPlatformId);
    if (osInfo.dwPlatformId == VER_PLATFORM_WIN32_NT) {
        mglSetAffinity();
        kmdDrvInit(&drv);
    }
    else
        vxdDrvInit(&drv);

    switch(dwReason) {
        case DLL_THREAD_ATTACH:
            break;
        case DLL_THREAD_DETACH:
            break;
        case DLL_PROCESS_ATTACH:
            if (drv.Init()) {
                if (InitMesaPTMMBase(&drv)) {
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
#ifdef DEBUG_GLSTUB
            logfp = fopen(LOG_NAME, "w");
#endif
            currGLRC = 0;
            currPixFmt = 0;
            memset(procName, 0, sizeof(procName));
            hHook = SetWindowsHookEx(WH_CALLWNDPROC, (HOOKPROC)CallWndProc, NULL, GetCurrentThreadId());
            GetModuleFileName(NULL, procName, sizeof(procName) - 1);
            DPRINTF("MesaGL Init ( %s )", procName);
	    DPRINTF("ptm 0x%08x fbtm 0x%08x", (uint32_t)ptm, (uint32_t)fbtm);
            memcpy(&fbtm[(MGLFBT_SIZE - ALIGNBO(1)) >> 2], rev_, ALIGNED(1));
	    ptm[(0xFBCU >> 2)] = (0xA0UL << 12) | MESAVER;
	    HostRet = ptm[(0xFBCU >> 2)];
	    if (HostRet != ((MESAVER << 8) | 0xa0UL)) {
		DPRINTF("Error - MesaGL init failed 0x%08x", HostRet);
		return FALSE;
	    }
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            if (--(*refcnt))
                break;
            DPRINTF("MesaGL Fini %x", currGLRC);
            HookEntryHook(0, 0);
            if (currGLRC) {
                mglMakeCurrent(0, 0);
                mglDeleteContext(MESAGL_MAGIC);
            }
            if (hHook)
                UnhookWindowsHookEx(hHook);
	    ptm[(0xFBCU >> 2)] = (0xD0UL << 12) | MESAVER;
            memset(&fbtm[(MGLFBT_SIZE - ALIGNBO(1)) >> 2], 0, ALIGNED(1));
            mfifo[1] = 0;
            if (drv.Init()) {
                FiniMesaPTMMBase(&drv);
                drv.Fini();
            }
#ifdef DEBUG_GLSTUB
            fclose(logfp);
#endif
            break;
    }

    return TRUE;
}
