/*
 * QEMU MESA GL Pass-Through
 *
 *  Copyright (c) 2020
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library;
 * if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "cpu.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/sysbus.h"
#include "exec/address-spaces.h"

#include "mesagl_impl.h"

#define DEBUG_MESAPT

#define MESAPT(obj) \
    OBJECT_CHECK(MesaPTState, (obj), TYPE_MESAPT)

#ifdef DEBUG_MESAPT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "mesapt: " fmt "\n" , ## __VA_ARGS__); } while(0)
#else
#define DPRINTF(fmt, ...)
#endif


typedef struct MesaPTState
{
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    MemoryRegion fifo_ram;
    uint8_t *fifo_ptr;
    uint32_t *arg, *hshm;
    int datacb, fifoMax, dataMax;

    MemoryRegion fbtm_ram;
    uint8_t *fbtm_ptr;

    uint32_t FEnum;
    uintptr_t FRet;
    uint32_t reg[4];
    uintptr_t parg[4];
    int mglContext, mglCntxCurrent;
    uint32_t MesaVer;
    uint32_t procRet;
    int pixfmt, pixfmtMax;
    uint8_t *logpname;
    uint16_t extnYear;
    size_t extnLength;
    vtxarry_t Color, EdgeFlag, Normal, Index, TexCoord[MAX_TEXUNIT], Vertex,
              Interleaved, SecondaryColor, FogCoord, Weight, GenAttrib[2];
    uint32_t elemMax;
    int szVertCache;
    int texUnit;
    int pixPackBuf, pixUnpackBuf;
    int szPackRow, szUnpackRow;
    int arrayBuf;
    int elemArryBuf;
    void *BufObj;
    int BufAcc;
    uint32_t BufRange;
    int KickFrame;
    PERFSTAT perfs;

} MesaPTState;

static void vtxarry_init(vtxarry_t *varry, int size, int type, int stride, void *ptr)
{
    varry->size = size;
    varry->type = type;
    varry->stride = stride;
    varry->ptr = ptr;
}

static void vtxarry_state(MesaPTState *s, uint32_t arry, int st)
{
#define GENERIC_ATTRIB6 0x06
#define GENERIC_ATTRIB7 0x07
    switch (arry) {
        case GL_COLOR_ARRAY:
            s->Color.enable = st;
            break;
        case GL_EDGE_FLAG_ARRAY:
            s->EdgeFlag.enable = st;
            break;
        case GL_INDEX_ARRAY:
            s->Index.enable = st;
            break;
        case GL_NORMAL_ARRAY:
            s->Normal.enable = st;
            break;
        case GL_TEXTURE_COORD_ARRAY:
            s->TexCoord[s->texUnit].enable = st;
            break;
        case GL_VERTEX_ARRAY:
            s->Vertex.enable = st;
            break;
        case GL_SECONDARY_COLOR_ARRAY:
            s->SecondaryColor.enable = st;
            break;
        case GL_FOG_COORDINATE_ARRAY:
            s->FogCoord.enable = st;
            break;
        case GL_WEIGHT_ARRAY_ARB:
            s->Weight.enable = st;
            break;
        case GENERIC_ATTRIB6:
            s->GenAttrib[0].enable = st;
            break;
        case GENERIC_ATTRIB7:
            s->GenAttrib[1].enable = st;
            break;
        default:
            break;
    }
}

static uint32_t vattr2arry_state(MesaPTState *s, int attr)
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
        s->texUnit = attr & 0x07U;
        st = GL_TEXTURE_COORD_ARRAY;
    }
    return st;
}

static vtxarry_t *vattr2arry(MesaPTState *s, int attr)
{
    vtxarry_t *attr2arry[] = {
        &s->Vertex,
        &s->Weight,
        &s->Normal,
        &s->Color,
        &s->SecondaryColor,
        &s->FogCoord,
        &s->GenAttrib[0],
        &s->GenAttrib[1],
    };
    vtxarry_t *arry = attr2arry[attr & 0x07U];
    if (attr & 0x08U) {
        int i = (attr & 0x07U);
        arry = &s->TexCoord[i];
    }
    return arry;
}

static void PushVertexArray(MesaPTState *s, const void *pshm, int start, int end)
{
    uint8_t *varry_ptr = (uint8_t *)pshm;
    int i, cbElem, n, ovfl;
    if (s->Interleaved.enable && s->Interleaved.ptr) {
        cbElem = (s->Interleaved.stride)? s->Interleaved.stride:s->Interleaved.size;
        n = (cbElem*(end - start) + s->Interleaved.size);
        n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
        ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
        memcpy(s->Interleaved.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
        varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
        s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
        if (ovfl)
            DPRINTF(" *WARN* Interleaved Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        s->Interleaved.enable = 0;
    }
    else {
        if (s->Color.enable && s->Color.ptr) {
            cbElem = (s->Color.stride)? s->Color.stride:s->Color.size*szgldata(0,s->Color.type);
            n = (cbElem*(end - start) + (s->Color.size*szgldata(0,s->Color.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->Color.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* Color Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        if (s->EdgeFlag.enable && s->EdgeFlag.ptr) {
            cbElem = (s->EdgeFlag.stride)? s->EdgeFlag.stride:s->EdgeFlag.size*szgldata(0,s->EdgeFlag.type);
            n = (cbElem*(end - start) + (s->EdgeFlag.size*szgldata(0,s->EdgeFlag.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->EdgeFlag.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* EdgeFlag Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        if (s->Index.enable && s->Index.ptr) {
            cbElem = (s->Index.stride)? s->Index.stride:s->Index.size*szgldata(0,s->Index.type);
            n = (cbElem*(end - start) + (s->Index.size*szgldata(0,s->Index.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->Index.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* Index Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        if (s->Normal.enable && s->Normal.ptr) {
            cbElem = (s->Normal.stride)? s->Normal.stride:s->Normal.size*szgldata(0,s->Normal.type);
            n = (cbElem*(end - start) + (s->Normal.size*szgldata(0,s->Normal.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->Normal.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* Normal Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        for (i = 0; i < MAX_TEXUNIT; i++) {
            if (s->TexCoord[i].enable && s->TexCoord[i].ptr) {
                cbElem = (s->TexCoord[i].stride)? s->TexCoord[i].stride:s->TexCoord[i].size*szgldata(0,s->TexCoord[i].type);
                n = (cbElem*(end - start) + (s->TexCoord[i].size*szgldata(0,s->TexCoord[i].type)));
                n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
                ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
                memcpy(s->TexCoord[i].ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
                varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
                s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
                if (ovfl)
                    DPRINTF(" *WARN* TexCoord%d Array overflowed, cbElem %04x maxElem %04x", i, cbElem, s->elemMax);
            }
        }
        if (s->Vertex.enable && s->Vertex.ptr) {
            cbElem = (s->Vertex.stride)? s->Vertex.stride:s->Vertex.size*szgldata(0,s->Vertex.type);
            n = (cbElem*(end - start) + (s->Vertex.size*szgldata(0,s->Vertex.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->Vertex.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* Vertex Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        if (s->SecondaryColor.enable && s->SecondaryColor.ptr) {
            cbElem = (s->SecondaryColor.stride)? s->SecondaryColor.stride:s->SecondaryColor.size*szgldata(0,s->SecondaryColor.type);
            n = (cbElem*(end - start) + (s->SecondaryColor.size*szgldata(0,s->SecondaryColor.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->SecondaryColor.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* SecondaryColor Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        if (s->FogCoord.enable && s->FogCoord.ptr) {
            cbElem = (s->FogCoord.stride)? s->FogCoord.stride:s->FogCoord.size*szgldata(0,s->FogCoord.type);
            n = (cbElem*(end - start) + (s->FogCoord.size*szgldata(0,s->FogCoord.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->FogCoord.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* FogCoord Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        if (s->Weight.enable && s->Weight.ptr) {
            cbElem = (s->Weight.stride)? s->Weight.stride:s->Weight.size*szgldata(0,s->Weight.type);
            n = (cbElem*(end - start) + (s->Weight.size*szgldata(0,s->Weight.type)));
            n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
            ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
            memcpy(s->Weight.ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
            varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
            s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
            if (ovfl)
                DPRINTF(" *WARN* Weight Array overflowed, cbElem %04x maxElem %04x", cbElem, s->elemMax);
        }
        for (i = 0; i < 2; i++) {
            if (s->GenAttrib[i].enable && s->GenAttrib[i].ptr) {
                cbElem = (s->GenAttrib[i].stride)? s->GenAttrib[i].stride:s->GenAttrib[i].size*szgldata(0,s->GenAttrib[i].type);
                n = (cbElem*(end - start) + (s->GenAttrib[i].size*szgldata(0,s->GenAttrib[i].type)));
                n = (n & 0x03)? ((n >> 2) + 1):(n >> 2);
                ovfl = ((n << 2) > (s->szVertCache >> 1))? 1:0;
                memcpy(s->GenAttrib[i].ptr + (cbElem*start), varry_ptr, ((ovfl)? (s->szVertCache >> 1):(n << 2)));
                varry_ptr += (n & 0x01)? ((n + 1) << 2):(n << 2);
                s->datacb += (n & 0x01)? ((n + 1) << 2):(n << 2);
                if (ovfl)
                    DPRINTF(" *WARN* GenAttrib%d Array overflowed, cbElem %04x maxElem %04x", i, cbElem, s->elemMax);
            }
        }
    }
}
static void InitClientStates(MesaPTState *s)
{
    memset(&s->Color, 0, sizeof(vtxarry_t));
    memset(&s->EdgeFlag, 0, sizeof(vtxarry_t));
    memset(&s->Index, 0, sizeof(vtxarry_t));
    memset(&s->Normal, 0, sizeof(vtxarry_t));
    memset(&s->Vertex, 0, sizeof(vtxarry_t));
    memset(&s->Interleaved, 0, sizeof(vtxarry_t));
    memset(&s->SecondaryColor, 0, sizeof(vtxarry_t));
    memset(&s->FogCoord, 0, sizeof(vtxarry_t));
    memset(&s->Weight, 0, sizeof(vtxarry_t));
    memset(s->TexCoord, 0, sizeof(vtxarry_t[MAX_TEXUNIT]));
    memset(s->GenAttrib, 0, sizeof(vtxarry_t[2]));
    s->elemMax = 0;
    s->texUnit = 0;
    s->arrayBuf = 0;
    s->elemArryBuf = 0;
    s->pixPackBuf = 0; s->pixUnpackBuf = 0;
    s->szPackRow = 0; s->szUnpackRow = 0;
}

static uint64_t mesapt_read(void *opaque, hwaddr addr, unsigned size)
{
    MesaPTState *s = opaque;
    uint32_t val = 0;

    switch (addr) {
        case 0xFBC:
            val = s->MesaVer;
            break;
        case 0xFC0:
            val = s->FRet;
            break;
        case 0xFEC:
            val = s->pixfmt;
            break;
        case 0xFE8:
            val = s->pixfmtMax;
            break;
        case 0xFE4:
        case 0xFE0:
            val = s->procRet;
            s->procRet = 0;
            break;
        default:
            break;
    }

    return val;
}

static int PArgsShouldAligned(MesaPTState *s)
{

    switch (s->FEnum)
    {
        case FEnum_glGetCompressedTexImage:
        case FEnum_glGetCompressedTexImageARB:
        case FEnum_glGetTexImage:
        case FEnum_glReadPixels:
            if (s->pixPackBuf) return 0;
            break;
        case FEnum_glBitmap:
        case FEnum_glCompressedTexImage1D:
        case FEnum_glCompressedTexImage1DARB:
        case FEnum_glCompressedTexImage2D:
        case FEnum_glCompressedTexImage2DARB:
        case FEnum_glCompressedTexImage3D:
        case FEnum_glCompressedTexImage3DARB:
        case FEnum_glCompressedTexSubImage1D:
        case FEnum_glCompressedTexSubImage1DARB:
        case FEnum_glCompressedTexSubImage2D:
        case FEnum_glCompressedTexSubImage2DARB:
        case FEnum_glCompressedTexSubImage3D:
        case FEnum_glCompressedTexSubImage3DARB:
        case FEnum_glDrawPixels:
        case FEnum_glPolygonStipple:
        case FEnum_glTexImage1D:
        case FEnum_glTexImage2D:
        case FEnum_glTexImage3D:
        case FEnum_glTexImage3DEXT:
        case FEnum_glTexSubImage1D:
        case FEnum_glTexSubImage1DEXT:
        case FEnum_glTexSubImage2D:
        case FEnum_glTexSubImage2DEXT:
        case FEnum_glTexSubImage3D:
        case FEnum_glTexSubImage3DEXT:
            if (s->pixUnpackBuf) return 0;
            break;
        case FEnum_glDrawElements:
        case FEnum_glDrawElementsBaseVertex:
        case FEnum_glDrawRangeElements:
        case FEnum_glDrawRangeElementsEXT:
            if (s->elemArryBuf) return 0;
            break;
        case FEnum_glBufferData:
        case FEnum_glBufferDataARB:
        case FEnum_glBufferSubData:
        case FEnum_glBufferSubDataARB:
        case FEnum_glGetBufferSubData:
        case FEnum_glGetBufferSubDataARB:
        case FEnum_glMapBufferRange:
        case FEnum_glNamedBufferSubData:
        case FEnum_glNamedBufferSubDataEXT:
            /* return 0; */
        case FEnum_glColorPointer:
        case FEnum_glColorPointerEXT:
        case FEnum_glEdgeFlagPointer:
        case FEnum_glEdgeFlagPointerEXT:
        case FEnum_glFogCoordPointer:
        case FEnum_glFogCoordPointerEXT:
        case FEnum_glIndexPointer:
        case FEnum_glIndexPointerEXT:
        case FEnum_glInterleavedArrays:
        case FEnum_glNormalPointer:
        case FEnum_glNormalPointerEXT:
        case FEnum_glSecondaryColorPointer:
        case FEnum_glSecondaryColorPointerEXT:
        case FEnum_glTexCoordPointer:
        case FEnum_glTexCoordPointerEXT:
        case FEnum_glVertexAttribPointer:
        case FEnum_glVertexAttribPointerARB:
        case FEnum_glVertexPointer:
        case FEnum_glVertexPointerEXT:
        case FEnum_glVertexWeightPointerEXT:
        case FEnum_glWeightPointerARB:
            return 0;
    }
    return 1;
}

#define PTR(x,y) (((uint8_t *)x)+y)
#define VAL(x) (uintptr_t)x
static void processArgs(MesaPTState *s)
{
    uint8_t *outshm = s->fifo_ptr + (MGLSHM_SIZE - (3*TARGET_PAGE_SIZE));

    switch (s->FEnum) {
        case FEnum_glAreTexturesResident:
        case FEnum_glAreTexturesResidentEXT:
            s->datacb = ALIGNED(s->arg[0] * sizeof(uint32_t));
            s->parg[1] = VAL(s->hshm);
            s->parg[2] = VAL(outshm);
            break;
        case FEnum_glPrioritizeTextures:
        case FEnum_glPrioritizeTexturesEXT:
            s->datacb = ALIGNED(s->arg[0] * sizeof(int)) + ALIGNED(s->arg[0] * sizeof(float));
            s->parg[1] = VAL(s->hshm);
            s->parg[2] = VAL(PTR(s->hshm, ALIGNED(s->arg[0] * sizeof(int))));
            break;
        case FEnum_glArrayElement:
        case FEnum_glArrayElementEXT:
            s->elemMax = (s->arg[0] > s->elemMax)? s->arg[0]:s->elemMax;
            PushVertexArray(s, s->hshm, s->arg[0], s->arg[0]);
            break;
        case FEnum_glCallLists:
            s->datacb = ALIGNED(s->arg[0] * szgldata(0, s->arg[1]));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glClipPlane:
            s->datacb = 4*sizeof(double);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColorSubTable:
        case FEnum_glColorSubTableEXT:
        case FEnum_glColorTable:
        case FEnum_glColorTableEXT:
            s->datacb = ALIGNED(s->arg[2] * szgldata(s->arg[3], s->arg[4]));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColorPointer:
        case FEnum_glColorPointerEXT:
            vtxarry_init(&s->Color, s->arg[0], s->arg[1], s->arg[2], (s->arrayBuf == 0)?
                LookupVertex((s->FEnum == FEnum_glColorPointer)? s->arg[3]:s->arg[4], s->szVertCache):
                (void *)(uintptr_t)((s->FEnum == FEnum_glColorPointer)? s->arg[3]:s->arg[4]));
            s->parg[3] = VAL(s->Color.ptr);
            s->parg[0] = VAL(s->Color.ptr);
            break;
        case FEnum_glEdgeFlagPointer:
        case FEnum_glEdgeFlagPointerEXT:
            vtxarry_init(&s->EdgeFlag, 1, GL_BYTE, s->arg[0], (s->arrayBuf == 0)?
                LookupVertex((s->FEnum == FEnum_glEdgeFlagPointer)? s->arg[1]:s->arg[2], s->szVertCache):
                (void *)(uintptr_t)((s->FEnum == FEnum_glEdgeFlagPointer)? s->arg[1]:s->arg[2]));
            s->parg[1] = VAL(s->EdgeFlag.ptr);
            s->parg[2] = VAL(s->EdgeFlag.ptr);
            break;
        case FEnum_glIndexPointer:
        case FEnum_glIndexPointerEXT:
            vtxarry_init(&s->Index, 1, s->arg[0], s->arg[1], (s->arrayBuf == 0)?
                LookupVertex((s->FEnum == FEnum_glIndexPointer)? s->arg[2]:s->arg[3], s->szVertCache):
                (void *)(uintptr_t)((s->FEnum == FEnum_glIndexPointer)? s->arg[2]:s->arg[3]));
            s->parg[2] = VAL(s->Index.ptr);
            s->parg[3] = VAL(s->Index.ptr);
            break;
        case FEnum_glNormalPointer:
        case FEnum_glNormalPointerEXT:
            vtxarry_init(&s->Normal, 3, s->arg[0], s->arg[1], (s->arrayBuf == 0)?
                LookupVertex((s->FEnum == FEnum_glNormalPointer)? s->arg[2]:s->arg[3], s->szVertCache):
                (void *)(uintptr_t)((s->FEnum == FEnum_glNormalPointer)? s->arg[2]:s->arg[3]));
            s->parg[2] = VAL(s->Normal.ptr);
            s->parg[3] = VAL(s->Normal.ptr);
            break;
        case FEnum_glTexCoordPointer:
        case FEnum_glTexCoordPointerEXT:
            vtxarry_init(&s->TexCoord[s->texUnit], s->arg[0], s->arg[1], s->arg[2], (s->arrayBuf == 0)?
                LookupVertex((s->FEnum == FEnum_glTexCoordPointer)? s->arg[3]:s->arg[4], s->szVertCache):
                (void *)(uintptr_t)((s->FEnum == FEnum_glTexCoordPointer)? s->arg[3]:s->arg[4]));
            s->parg[3] = VAL(s->TexCoord[s->texUnit].ptr);
            s->parg[0] = VAL(s->TexCoord[s->texUnit].ptr);
            break;
        case FEnum_glVertexPointer:
        case FEnum_glVertexPointerEXT:
            vtxarry_init(&s->Vertex, s->arg[0], s->arg[1], s->arg[2], (s->arrayBuf == 0)?
                LookupVertex((s->FEnum == FEnum_glVertexPointer)? s->arg[3]:s->arg[4], s->szVertCache):
                (void *)(uintptr_t)((s->FEnum == FEnum_glVertexPointer)? s->arg[3]:s->arg[4]));
            s->parg[3] = VAL(s->Vertex.ptr);
            s->parg[0] = VAL(s->Vertex.ptr);
            break;
        case FEnum_glSecondaryColorPointer:
        case FEnum_glSecondaryColorPointerEXT:
            vtxarry_init(&s->SecondaryColor, s->arg[0], s->arg[1], s->arg[2], (s->arrayBuf == 0)?
                LookupVertex(s->arg[3], s->szVertCache):(void *)(uintptr_t)s->arg[3]);
            s->parg[3] = VAL(s->SecondaryColor.ptr);
            break;
        case FEnum_glFogCoordPointer:
        case FEnum_glFogCoordPointerEXT:
            vtxarry_init(&s->FogCoord, 1, s->arg[0], s->arg[1], (s->arrayBuf == 0)?
                LookupVertex(s->arg[2], s->szVertCache):(void *)(uintptr_t)s->arg[2]);
            s->parg[2] = VAL(s->FogCoord.ptr);
            break;
        case FEnum_glVertexWeightPointerEXT:
        case FEnum_glWeightPointerARB:
            vtxarry_init(&s->Weight, s->arg[0], s->arg[1], s->arg[2], (s->arrayBuf == 0)?
                LookupVertex(s->arg[3], s->szVertCache):(void *)(uintptr_t)s->arg[3]);
            s->parg[3] = VAL(s->Weight.ptr);
            break;
        case FEnum_glVertexAttribPointer:
        case FEnum_glVertexAttribPointerARB:
            {
                vtxarry_t *arry = vattr2arry(s, s->arg[0]);
                vtxarry_init(arry, s->arg[1], s->arg[2], s->arg[4], (s->arrayBuf == 0)?
                        LookupVertex(s->arg[5], s->szVertCache):(void *)(uintptr_t)s->arg[5]);
                s->parg[1] = VAL(arry->ptr);
            }
            break;
        case FEnum_glInterleavedArrays:
            vtxarry_init(&s->Interleaved, szgldata(s->arg[0], 0), 0, s->arg[1], LookupVertex(s->arg[2], s->szVertCache));
            s->Interleaved.enable = 1;
            s->parg[2] = VAL(s->Interleaved.ptr);
            break;
        case FEnum_glIndexubv:
            s->datacb = ALIGNED(sizeof(uint8_t));
            s->parg[0] = VAL(s->hshm);
            break;
        case FEnum_glColor3bv:
        case FEnum_glColor3ubv:
        case FEnum_glNormal3bv:
        case FEnum_glSecondaryColor3bv:
        case FEnum_glSecondaryColor3bvEXT:
        case FEnum_glSecondaryColor3ubv:
        case FEnum_glSecondaryColor3ubvEXT:            
            s->datacb = ALIGNED(3*sizeof(uint8_t));
            s->parg[0] = VAL(s->hshm);
            break;
        case FEnum_glColor4bv:
        case FEnum_glColor4ubv:
        case FEnum_glVertexAttrib4Nbv:
        case FEnum_glVertexAttrib4NbvARB:
        case FEnum_glVertexAttrib4Nubv:
        case FEnum_glVertexAttrib4NubvARB:
        case FEnum_glVertexAttrib4bv:
        case FEnum_glVertexAttrib4bvARB:
        case FEnum_glVertexAttrib4ubv:
        case FEnum_glVertexAttrib4ubvARB:            
            s->datacb = ALIGNED(4*sizeof(uint8_t));
            s->parg[0] = VAL(s->hshm);
            break;
        case FEnum_glIndexsv:
        case FEnum_glMultiTexCoord1sv:
        case FEnum_glMultiTexCoord1svARB:
        case FEnum_glVertexAttrib1sv:
        case FEnum_glVertexAttrib1svARB:            
            s->datacb = ALIGNED(sizeof(uint16_t));
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glRasterPos2sv:
        case FEnum_glTexCoord2sv:
        case FEnum_glVertex2sv:
        case FEnum_glMultiTexCoord2sv:
        case FEnum_glMultiTexCoord2svARB:
        case FEnum_glVertexAttrib2sv:
        case FEnum_glVertexAttrib2svARB:            
            s->datacb = ALIGNED(2*sizeof(uint16_t));
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColor3sv:
        case FEnum_glColor3usv:
        case FEnum_glMultiTexCoord3sv:
        case FEnum_glMultiTexCoord3svARB:
        case FEnum_glNormal3sv:
        case FEnum_glRasterPos3sv:
        case FEnum_glSecondaryColor3sv:
        case FEnum_glSecondaryColor3svEXT:
        case FEnum_glSecondaryColor3usv:
        case FEnum_glSecondaryColor3usvEXT:            
        case FEnum_glTexCoord3sv:
        case FEnum_glVertex3sv:
        case FEnum_glVertexAttrib3sv:
        case FEnum_glVertexAttrib3svARB:            
            s->datacb = ALIGNED(3*sizeof(uint16_t));
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColor4sv:
        case FEnum_glColor4usv:
        case FEnum_glRasterPos4sv:
        case FEnum_glTexCoord4sv:
        case FEnum_glVertex4sv:
        case FEnum_glMultiTexCoord4sv:
        case FEnum_glMultiTexCoord4svARB:
        case FEnum_glVertexAttrib4Nsv:
        case FEnum_glVertexAttrib4NsvARB:
        case FEnum_glVertexAttrib4Nusv:
        case FEnum_glVertexAttrib4NusvARB:
        case FEnum_glVertexAttrib4sv:
        case FEnum_glVertexAttrib4svARB:
        case FEnum_glVertexAttrib4usv:
        case FEnum_glVertexAttrib4usvARB:
            s->datacb = 4*sizeof(uint16_t);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glEdgeFlagv:
        case FEnum_glEvalCoord1fv:
        case FEnum_glFogCoorddv:
        case FEnum_glFogCoorddvEXT:
        case FEnum_glFogCoordfv:
        case FEnum_glFogCoordfvEXT:
        case FEnum_glIndexfv:
        case FEnum_glIndexiv:
        case FEnum_glMultiTexCoord1fv:
        case FEnum_glMultiTexCoord1fvARB:
        case FEnum_glMultiTexCoord1iv:
        case FEnum_glMultiTexCoord1ivARB:
        case FEnum_glVertexAttrib1fv:
        case FEnum_glVertexAttrib1fvARB:
        case FEnum_glVertexWeightfvEXT:
            s->datacb = ALIGNED(sizeof(uint32_t));
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glEvalCoord2fv:
        case FEnum_glRasterPos2fv:
        case FEnum_glRasterPos2iv:
        case FEnum_glTexCoord2fv:
        case FEnum_glTexCoord2iv:
        case FEnum_glVertex2fv:
        case FEnum_glVertex2iv:
        case FEnum_glMultiTexCoord2fv:
        case FEnum_glMultiTexCoord2fvARB:
        case FEnum_glMultiTexCoord2iv:
        case FEnum_glMultiTexCoord2ivARB:
        case FEnum_glVertexAttrib2fv:
        case FEnum_glVertexAttrib2fvARB:            
            s->datacb = 2*sizeof(uint32_t);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColor3fv:
        case FEnum_glColor3iv:
        case FEnum_glColor3uiv:
        case FEnum_glMultiTexCoord3fv:
        case FEnum_glMultiTexCoord3fvARB:
        case FEnum_glMultiTexCoord3iv:
        case FEnum_glMultiTexCoord3ivARB:
        case FEnum_glNormal3fv:
        case FEnum_glNormal3iv:
        case FEnum_glRasterPos3fv:
        case FEnum_glRasterPos3iv:
        case FEnum_glSecondaryColor3fv:
        case FEnum_glSecondaryColor3fvEXT:
        case FEnum_glSecondaryColor3iv:
        case FEnum_glSecondaryColor3ivEXT:
        case FEnum_glSecondaryColor3uiv:
        case FEnum_glSecondaryColor3uivEXT:            
        case FEnum_glTexCoord3fv:
        case FEnum_glTexCoord3iv:
        case FEnum_glVertex3fv:
        case FEnum_glVertex3iv:
        case FEnum_glVertexAttrib3fv:
        case FEnum_glVertexAttrib3fvARB:            
            s->datacb = ALIGNED(3*sizeof(uint32_t));
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColor4fv:
        case FEnum_glColor4iv:
        case FEnum_glColor4uiv:
        case FEnum_glRasterPos4fv:
        case FEnum_glRasterPos4iv:
        case FEnum_glTexCoord4fv:
        case FEnum_glTexCoord4iv:
        case FEnum_glVertex4fv:
        case FEnum_glVertex4iv:
        case FEnum_glMultiTexCoord4fv:
        case FEnum_glMultiTexCoord4fvARB:
        case FEnum_glMultiTexCoord4iv:
        case FEnum_glMultiTexCoord4ivARB:
        case FEnum_glVertexAttrib4fv:
        case FEnum_glVertexAttrib4fvARB:
        case FEnum_glVertexAttrib4iv:
        case FEnum_glVertexAttrib4ivARB:
        case FEnum_glVertexAttrib4Niv:
        case FEnum_glVertexAttrib4NivARB:
        case FEnum_glVertexAttrib4Nuiv:
        case FEnum_glVertexAttrib4NuivARB:
        case FEnum_glVertexAttrib4uiv:
        case FEnum_glVertexAttrib4uivARB:            
            s->datacb = 4*sizeof(uint32_t);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glEvalCoord1dv:
        case FEnum_glIndexdv:
        case FEnum_glMultiTexCoord1dv:
        case FEnum_glMultiTexCoord1dvARB:
        case FEnum_glVertexAttrib1dv:
        case FEnum_glVertexAttrib1dvARB:            
            s->datacb = sizeof(double);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glEvalCoord2dv:
        case FEnum_glRasterPos2dv:
        case FEnum_glTexCoord2dv:
        case FEnum_glVertex2dv:
        case FEnum_glMultiTexCoord2dv:
        case FEnum_glMultiTexCoord2dvARB:
        case FEnum_glVertexAttrib2dv:
        case FEnum_glVertexAttrib2dvARB:
            s->datacb = 2*sizeof(double);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColor3dv:
        case FEnum_glMultiTexCoord3dv:
        case FEnum_glMultiTexCoord3dvARB:
        case FEnum_glNormal3dv:
        case FEnum_glRasterPos3dv:
        case FEnum_glSecondaryColor3dv:
        case FEnum_glSecondaryColor3dvEXT:            
        case FEnum_glTexCoord3dv:
        case FEnum_glVertex3dv:
        case FEnum_glVertexAttrib3dv:
        case FEnum_glVertexAttrib3dvARB:
            s->datacb = 3*sizeof(double);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glColor4dv:
        case FEnum_glRasterPos4dv:
        case FEnum_glTexCoord4dv:
        case FEnum_glVertex4dv:
        case FEnum_glMultiTexCoord4dv:
        case FEnum_glMultiTexCoord4dvARB:
        case FEnum_glVertexAttrib4dv:
        case FEnum_glVertexAttrib4dvARB:            
            s->datacb = 4*sizeof(double);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glDeleteBuffers:
        case FEnum_glDeleteBuffersARB:
        case FEnum_glDeleteFramebuffers:
        case FEnum_glDeleteFramebuffersEXT:
        case FEnum_glDeleteProgramsARB:
        case FEnum_glDeleteQueries:
        case FEnum_glDeleteQueriesARB:
        case FEnum_glDeleteRenderbuffers:
        case FEnum_glDeleteRenderbuffersEXT:
        case FEnum_glDeleteSamplers:
        case FEnum_glDeleteTextures:
        case FEnum_glDeleteTexturesEXT:
        case FEnum_glDeleteVertexArrays:
        case FEnum_glDrawBuffers:
        case FEnum_glDrawBuffersARB:
            s->datacb = ALIGNED(s->arg[0] * sizeof(uint32_t));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glDrawArrays:
        case FEnum_glDrawArraysEXT:
            s->elemMax = ((s->arg[1] + s->arg[2] - 1) > s->elemMax)? (s->arg[1] + s->arg[2] - 1):s->elemMax;
            if (s->arrayBuf == 0)
                PushVertexArray(s, s->hshm, s->arg[1], s->arg[1] + s->arg[2] - 1);
            break;
        case FEnum_glDrawElements:
            s->parg[3] = s->arg[3];
            if (s->elemArryBuf == 0) {
                s->datacb = ALIGNED(s->arg[1] * szgldata(0, s->arg[2]));
                s->parg[3] = VAL(s->hshm);
                int start, end = 0;
                for (int i = 0; i < s->arg[1]; i++) {
                    if (szgldata(0, s->arg[2]) == 1) {
                        uint8_t *p = (uint8_t *)s->hshm;
                        end = (p[i] > end)? p[i]:end;
                    }
                    if (szgldata(0, s->arg[2]) == 2) {
                        uint16_t *p = (uint16_t *)s->hshm;
                        end = (p[i] > end)? p[i]:end;
                    }
                    if (szgldata(0, s->arg[2]) == 4) {
                        uint32_t *p = s->hshm;
                        end = (p[i] > end)? p[i]:end;
                    }
                }
                start = end;
                for (int i = 0; i < s->arg[1]; i++) {
                    if (szgldata(0, s->arg[2]) == 1) {
                        uint8_t *p = (uint8_t *)s->hshm;
                        start = (p[i] < start)? p[i]:start;
                    }
                    if (szgldata(0, s->arg[2]) == 2) {
                        uint16_t *p = (uint16_t *)s->hshm;
                        start = (p[i] < start)? p[i]:start;
                    }
                    if (szgldata(0, s->arg[2]) == 4) {
                        uint32_t *p = s->hshm;
                        start = (p[i] < start)? p[i]:start;
                    }
                }
                //DPRINTF("DrawElements() %04x %04x", start, end);
                s->elemMax = (end > s->elemMax)? end:s->elemMax;
                if (s->arrayBuf == 0)
                    PushVertexArray(s, PTR(s->hshm, s->datacb), start, end);
            }
            break;
        case FEnum_glDrawElementsBaseVertex:
            s->parg[3] = s->arg[3];
            if (s->elemArryBuf == 0) {
                s->datacb = ALIGNED(s->arg[1] * szgldata(0, s->arg[2]));
                s->parg[3] = VAL(s->hshm);
            }
            break;
        case FEnum_glDrawPixels:
        case FEnum_glPolygonStipple:
            s->parg[0] = (s->FEnum == FEnum_glDrawPixels)? s->arg[4]:s->arg[0];
            if (s->pixUnpackBuf == 0) {
                s->datacb = (s->FEnum == FEnum_glDrawPixels)?
                    ALIGNED(((s->szUnpackRow == 0)? s->arg[0]:s->szUnpackRow) * s->arg[1] * szgldata(s->arg[2], s->arg[3])):
                    ALIGNED(((s->szUnpackRow == 0)? 32:s->szUnpackRow) * 32);
                s->parg[0] = VAL(s->hshm);
            }
            break;
        case FEnum_glDrawRangeElements:
        case FEnum_glDrawRangeElementsEXT:
            s->parg[1] = s->arg[5];
            if (s->elemArryBuf == 0) {
                s->datacb = ALIGNED(s->arg[3] * szgldata(0, s->arg[4]));
                s->parg[1] = VAL(s->hshm);
                s->elemMax = (s->arg[2] > s->elemMax)? s->arg[2]:s->elemMax;
                if (s->arrayBuf == 0)
                    PushVertexArray(s, PTR(s->hshm, s->datacb), s->arg[1], s->arg[2]);
            }
            break;
        case FEnum_glGetInternalformativ:
            s->parg[0] = VAL(outshm);
            break;
        case FEnum_glGenBuffers:
        case FEnum_glGenBuffersARB:
        case FEnum_glGenFramebuffers:
        case FEnum_glGenFramebuffersEXT:
        case FEnum_glGenProgramsARB:
        case FEnum_glGenQueries:
        case FEnum_glGenQueriesARB:
        case FEnum_glGenRenderbuffers:
        case FEnum_glGenRenderbuffersEXT:
        case FEnum_glGenSamplers:
        case FEnum_glGenTextures:
        case FEnum_glGenTexturesEXT:
        case FEnum_glGenVertexArrays:
        case FEnum_glGetClipPlane:
        case FEnum_glSelectBuffer:
            s->parg[1] = VAL(outshm);
            break;
        case FEnum_glFeedbackBuffer:
            s->parg[2] = VAL(outshm);
            break;
        case FEnum_glFogfv:
        case FEnum_glFogiv:
        case FEnum_glLightModelfv:
        case FEnum_glLightModeliv:
        case FEnum_glPointParameterfv:
        case FEnum_glPointParameterfvARB:
        case FEnum_glPointParameterfvEXT:
        case FEnum_glPointParameteriv:
            s->datacb = ALIGNED(szglname(s->arg[0])*sizeof(uint32_t));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glGetBooleanv:
        case FEnum_glGetDoublev:
        case FEnum_glGetFloatv:
        case FEnum_glGetIntegerv:
        case FEnum_glGetPixelMapfv:
        case FEnum_glGetPixelMapuiv:
        case FEnum_glGetPixelMapusv:
            *(int *)outshm = szglname(s->arg[0]);
            s->parg[1] = VAL(PTR(outshm, ALIGNED(sizeof(uint32_t))));
            break;
        case FEnum_glGetBufferParameteriv:
        case FEnum_glGetBufferParameterivARB:
        case FEnum_glGetLightfv:
        case FEnum_glGetLightiv:
        case FEnum_glGetMaterialfv:
        case FEnum_glGetMaterialiv:
        case FEnum_glGetProgramiv:
        case FEnum_glGetProgramivARB:
        case FEnum_glGetQueryiv:
        case FEnum_glGetQueryivARB:
        case FEnum_glGetQueryObjectiv:
        case FEnum_glGetQueryObjectivARB:
        case FEnum_glGetQueryObjectuiv:
        case FEnum_glGetQueryObjectuivARB:
        case FEnum_glGetShaderiv:
        case FEnum_glGetTexEnvfv:
        case FEnum_glGetTexEnviv:
        case FEnum_glGetTexGendv:
        case FEnum_glGetTexGenfv:
        case FEnum_glGetTexGeniv:
        case FEnum_glGetTexParameterfv:
        case FEnum_glGetTexParameteriv:
            *(int *)outshm = szglname(s->arg[1]);
            s->parg[2] = VAL(PTR(outshm, ALIGNED(sizeof(uint32_t))));
            break;
        case FEnum_glGetMapdv:
        case FEnum_glGetMapfv:
        case FEnum_glGetMapiv:
            {
                int n;
                switch (s->arg[0]) {
                    case GL_MAP2_COLOR_4:
                    case GL_MAP2_INDEX:
                    case GL_MAP2_NORMAL:
                    case GL_MAP2_TEXTURE_COORD_1:
                    case GL_MAP2_TEXTURE_COORD_2:
                    case GL_MAP2_TEXTURE_COORD_3:
                    case GL_MAP2_TEXTURE_COORD_4:
                    case GL_MAP2_VERTEX_3:
                    case GL_MAP2_VERTEX_4:
                        n = 2;
                        break;
                    default:
                        n = 1;
                        break;
                }
                switch (s->arg[1]) {
                    case GL_COEFF:
                        n = (szglname(s->arg[0]) * wrMapOrderPoints(s->arg[0]));
                        break;
                    case GL_ORDER:
                        break;
                    case GL_DOMAIN:
                        n <<= 1;
                        break;
                }
                *(int *)outshm = n;
            }
            s->parg[2] = VAL(PTR(outshm, ALIGNED(sizeof(uint32_t))));
            break;
        case FEnum_glGetTexLevelParameterfv:
        case FEnum_glGetTexLevelParameteriv:
            *(int *)outshm = szglname(s->arg[2]);
            s->parg[3] = VAL(PTR(outshm, ALIGNED(sizeof(uint32_t))));
            break;
        case FEnum_glLoadMatrixd:
        case FEnum_glMultMatrixd:
            s->datacb = 16*sizeof(double);
            s->parg[0] = VAL(s->hshm);
            break;
        case FEnum_glLoadMatrixf:
        case FEnum_glMultMatrixf:
            s->datacb = 16*sizeof(float);
            s->parg[0] = VAL(s->hshm);
            break;
        case FEnum_glLockArraysEXT:
            //DPRINTF("LockArraysEXT() %04x %04x", s->arg[0], s->arg[1]);
            PushVertexArray(s, s->hshm, s->arg[0], s->arg[0] + s->arg[1] - 1);
            break;
        case FEnum_glProgramEnvParameter4dvARB:
        case FEnum_glProgramLocalParameter4dvARB:
            s->datacb = 4*sizeof(double);
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glProgramEnvParameter4fvARB:
        case FEnum_glProgramLocalParameter4fvARB:
            s->datacb = 4*sizeof(float);
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glProgramEnvParameters4fvEXT:
        case FEnum_glProgramLocalParameters4fvEXT:
            s->datacb = 4*s->arg[2]*sizeof(float);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glProgramStringARB:
            s->datacb = ALIGNED(s->arg[2]);
            s->parg[3] = VAL(s->hshm);
            //DPRINTF("Program size 0x%04x\n%s", s->arg[2], (unsigned char *)s->hshm);
            break;
        case FEnum_glReadPixels:
            s->parg[2] = (s->pixPackBuf == 0)? VAL(s->fbtm_ptr):s->arg[6];
            break;
        case FEnum_glRectdv:
            s->datacb = 2*sizeof(double);
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(PTR(s->hshm, sizeof(double)));
            break;
        case FEnum_glRectfv:
        case FEnum_glRectiv:
            s->datacb = 2*ALIGNED(sizeof(uint32_t));
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(PTR(s->hshm, ALIGNED(sizeof(uint32_t))));
            break;
        case FEnum_glRectsv:
            s->datacb = 2*ALIGNED(sizeof(uint16_t));
            s->parg[0] = VAL(s->hshm);
            s->parg[1] = VAL(PTR(s->hshm, ALIGNED(sizeof(uint16_t))));
            break;
        case FEnum_glLightfv:
        case FEnum_glLightiv:
        case FEnum_glMaterialfv:
        case FEnum_glMaterialiv:
        case FEnum_glSamplerParameterIiv:
        case FEnum_glSamplerParameterIuiv:
        case FEnum_glSamplerParameterfv:
        case FEnum_glSamplerParameteriv:
        case FEnum_glTexEnvfv:
        case FEnum_glTexEnviv:
        case FEnum_glTexGenfv:
        case FEnum_glTexGeniv:
        case FEnum_glTexParameterfv:
        case FEnum_glTexParameteriv:
            s->datacb = ALIGNED(szglname(s->arg[1])*sizeof(uint32_t));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glTexGendv:
            s->datacb = ALIGNED(szglname(s->arg[1])*sizeof(double));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glPixelMapfv:
        case FEnum_glPixelMapuiv:
        case FEnum_glPixelMapusv:
            s->datacb = (s->FEnum == FEnum_glPixelMapusv)? 
                ALIGNED(s->arg[1]*sizeof(uint16_t)):
                ALIGNED(s->arg[1]*sizeof(uint32_t));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glWeightbvARB:
        case FEnum_glWeightubvARB:
            s->datacb = ALIGNED(s->arg[0] * sizeof(uint8_t));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glWeightsvARB:
        case FEnum_glWeightusvARB:
            s->datacb = ALIGNED(s->arg[0] * sizeof(uint16_t));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glWeightivARB:
        case FEnum_glWeightuivARB:            
        case FEnum_glWeightfvARB:
            s->datacb = ALIGNED(s->arg[0] * sizeof(uint32_t));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glWeightdvARB:
            s->datacb = s->arg[0] * sizeof(double);
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glBitmap:
            s->parg[2] = s->arg[6];
            if (s->pixUnpackBuf == 0) {
                uint32_t szBmp = ((s->szUnpackRow == 0)? s->arg[0]:s->szUnpackRow) * s->arg[1];
                uintptr_t bmpPtr = ((uintptr_t)s->fbtm_ptr) + (MGLFBT_SIZE - ALIGNED(szBmp));
                s->parg[2] = (s->arg[6])? bmpPtr:0;
            }
            break;
        case FEnum_glBufferSubData:
        case FEnum_glBufferSubDataARB:
        case FEnum_glGetBufferSubData:
        case FEnum_glGetBufferSubDataARB:
        case FEnum_glNamedBufferSubData:
        case FEnum_glNamedBufferSubDataEXT:
            s->parg[1] = s->arg[1];
            s->parg[2] = s->arg[2];
            s->parg[3] = VAL(s->fbtm_ptr + MGLFBT_SIZE - ALIGNED(s->arg[2]));
            break;
        case FEnum_glBufferData:
        case FEnum_glBufferDataARB:
            s->parg[1] = s->arg[1];
            s->parg[2] = (s->arg[2])? VAL(s->fbtm_ptr + MGLFBT_SIZE - ALIGNED(s->arg[1])):0;
            break;
        case FEnum_glMapBufferRange:
            s->parg[1] = s->arg[1];
            s->parg[2] = s->arg[2];
            wrFillBufObj(s->arg[0], (s->fbtm_ptr + MGLFBT_SIZE), s->arg[1], s->arg[2]);
            break;
        case FEnum_glMapBuffer:
        case FEnum_glMapBufferARB:
            wrFillBufObj(s->arg[0], (s->fbtm_ptr + MGLFBT_SIZE), 0, 0);
            break;
        case FEnum_glUnmapBuffer:
        case FEnum_glUnmapBufferARB:
            if (s->BufObj && (s->BufAcc & GL_MAP_WRITE_BIT)) {
                wrFlushBufObj((s->FEnum == FEnum_glUnmapBuffer)? FEnum_glGetBufferParameteriv:FEnum_glGetBufferParameterivARB,
                    s->arg[0], (s->fbtm_ptr + MGLFBT_SIZE), s->BufObj, s->BufRange);
                s->BufObj = 0;
            }
            break;
        case FEnum_glGetTexImage:
            s->parg[0] = s->arg[4];
            if (s->pixPackBuf == 0) {
                uint32_t szTex, *texPtr;
                szTex = wrTexTextureWxD(s->arg[0], s->arg[1], 0)*szgldata(s->arg[2], s->arg[3]);
                texPtr = (uint32_t *)s->fbtm_ptr;
                texPtr[0] = szTex;
                s->parg[0] = VAL(&texPtr[ALIGNED(1) >> 2]);
            }
            break;
        case FEnum_glTexImage1D:
        case FEnum_glTexSubImage1D:
        case FEnum_glTexSubImage1DEXT:
            s->parg[3] = s->arg[7];
            s->parg[2] = s->arg[6];
            if (s->pixUnpackBuf == 0) {
                uint32_t szTex, *texPtr;
                szTex = (s->FEnum == FEnum_glTexImage1D)?
                    (((s->szUnpackRow == 0)? s->arg[3]:s->szUnpackRow) * szgldata(s->arg[5], s->arg[6])):
                    (((s->szUnpackRow == 0)? s->arg[3]:s->szUnpackRow) * szgldata(s->arg[4], s->arg[5]));
                texPtr = (uint32_t *)(s->fbtm_ptr + (MGLFBT_SIZE - ALIGNED(szTex)));
                s->parg[3] = (s->arg[7])? VAL(texPtr):0;
                s->parg[2] = (s->arg[6])? VAL(texPtr):0;
            }
            break;
        case FEnum_glTexImage2D:
        case FEnum_glTexSubImage2D:
        case FEnum_glTexSubImage2DEXT:
            s->parg[0] = s->arg[8];
            if (s->pixUnpackBuf == 0) {
                uint32_t szTex, *texPtr;
                szTex = (s->FEnum == FEnum_glTexImage2D)?
                    (((s->szUnpackRow == 0)? s->arg[3]:s->szUnpackRow) * s->arg[4] * szgldata(s->arg[6], s->arg[7])):
                    (((s->szUnpackRow == 0)? s->arg[4]:s->szUnpackRow) * s->arg[5] * szgldata(s->arg[6], s->arg[7]));
                texPtr = (uint32_t *)(s->fbtm_ptr + (MGLFBT_SIZE - ALIGNED(szTex)));
                s->parg[0] = (s->arg[8])? VAL(texPtr):0;
                //DPRINTF("Tex*Image2D() %x,%x,%x,%x,%x,%x,%x,%x,%08x",s->arg[0],s->arg[1],s->arg[2],s->arg[3],s->arg[4],s->arg[5],s->arg[6],s->arg[7],szTex);
            }
            break;
        case FEnum_glTexImage3D:
        case FEnum_glTexImage3DEXT:
            s->parg[1] = s->arg[9];
            if (s->pixUnpackBuf == 0) {
                uint32_t szTex, *texPtr;
                szTex = s->arg[3] * s->arg[4] * s->arg[5] * szgldata(s->arg[7], s->arg[8]);
                texPtr = (uint32_t *)(s->fbtm_ptr + (MGLFBT_SIZE - ALIGNED(szTex)));
                s->parg[1] = (s->arg[9])? VAL(texPtr):0;
            }
            break;
        case FEnum_glGetCompressedTexImage:
        case FEnum_glGetCompressedTexImageARB:
            s->parg[2] = s->arg[2];
            if (s->pixPackBuf == 0) {
                uint32_t *texPtr;
                texPtr = (uint32_t *)s->fbtm_ptr;
                texPtr[0] = wrTexTextureWxD(s->arg[0], s->arg[1], 1);
                s->parg[2] = VAL(&texPtr[ALIGNED(1) >> 2]);
            }
            break;
        case FEnum_glCompressedTexImage1D:
        case FEnum_glCompressedTexImage1DARB:
        case FEnum_glCompressedTexSubImage1D:
        case FEnum_glCompressedTexSubImage1DARB:
            s->parg[2] = s->arg[6];
            if (s->pixUnpackBuf == 0) {
                uint32_t *texPtr;
                texPtr = (uint32_t *)(s->fbtm_ptr + (MGLFBT_SIZE - ALIGNED(s->arg[5])));
                s->parg[2] = VAL(texPtr);
            }
            break;
        case FEnum_glCompressedTexImage2D:
        case FEnum_glCompressedTexImage2DARB:
            s->parg[3] = s->arg[7];
            if (s->pixUnpackBuf == 0) {
                uint32_t *texPtr;
                texPtr = (uint32_t *)(s->fbtm_ptr + (MGLFBT_SIZE - ALIGNED(s->arg[6])));
                s->parg[3] = VAL(texPtr);
            }
            break;
        case FEnum_glCompressedTexImage3D:
        case FEnum_glCompressedTexImage3DARB:
        case FEnum_glCompressedTexSubImage2D:
        case FEnum_glCompressedTexSubImage2DARB:
            s->parg[0] = s->arg[8];
            if (s->pixUnpackBuf == 0) {
                uint32_t *texPtr;
                texPtr = (uint32_t *)(s->fbtm_ptr + (MGLFBT_SIZE - ALIGNED(s->arg[7])));
                s->parg[0] = VAL(texPtr);
            }
            break;
        case FEnum_glCompressedTexSubImage3D:
        case FEnum_glCompressedTexSubImage3DARB:
            s->parg[2] = s->arg[10];
            if (s->pixUnpackBuf == 0) {
                uint32_t *texPtr;
                texPtr = (uint32_t *)(s->fbtm_ptr + (MGLFBT_SIZE - ALIGNED(s->arg[9])));
                s->parg[2] = VAL(texPtr);
            }
            break;
        case FEnum_glMap1d:
        case FEnum_glMap1f:
            s->datacb = (s->FEnum == FEnum_glMap1d)?
                ALIGNED(szglname(s->arg[0])*s->arg[5]*s->arg[6]*sizeof(double)):
                ALIGNED(szglname(s->arg[0])*s->arg[3]*s->arg[4]*sizeof(float));
            s->parg[1] = VAL(s->hshm);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glMap2d:
        case FEnum_glMap2f:
            s->datacb = (s->FEnum == FEnum_glMap2d)?
                ALIGNED(szglname(s->arg[0])*s->arg[5]*s->arg[6]*s->arg[11]*s->arg[12]*sizeof(double)):
                ALIGNED(szglname(s->arg[0])*s->arg[3]*s->arg[4]*s->arg[7]*s->arg[8]*sizeof(float));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glBindAttribLocation:
        case FEnum_glBindAttribLocationARB:
            s->datacb = ALIGNED((strlen((char *)s->hshm) + 1));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glGetAttribLocation:
        case FEnum_glGetAttribLocationARB:
        case FEnum_glGetUniformLocation:
        case FEnum_glGetUniformLocationARB:
            s->datacb = ALIGNED((strlen((char *)s->hshm) + 1));
            s->parg[1] = VAL(s->hshm);
            break;
        case FEnum_glShaderSource:
        case FEnum_glShaderSourceARB:
            {
                char **str;
                int i, offs = 0, *len = (int *)(s->hshm);
                if (s->arg[3]) {
                    for (i = 0; i < s->arg[1]; i++)
                        offs += ALIGNED(len[i]) + ALIGNED(1);
                    str = (char **)PTR(s->hshm, offs + ALIGNED(s->arg[1]*sizeof(int)));
                    str[0] = (char *)PTR(s->hshm, ALIGNED(s->arg[1]*sizeof(int)));
                    for (i = 1; i < s->arg[1]; i++)
                        str[i] = str[i-1] + ALIGNED(len[i-1]) + ALIGNED(1);
                    /* for (i = 0; i < s->arg[1]; i++)
                        DPRINTF("\n%s", str[i]); */
                    s->datacb = ALIGNED(s->arg[1]*sizeof(int)) + offs;
                    s->parg[3] = VAL(len);
                }
                else {
                    char *send = (char *)(s->hshm);
                    for (i = 0; i < s->arg[1]; i++) {
                        offs = ALIGNED((strlen(send) + 1));
                        send += offs;
                    }
                    s->datacb = VAL(send) - VAL(s->hshm);
                    s->parg[3] = 0;
                    str = (char **)send;
                    send = (char *)(s->hshm);
                    offs = 0;
                    str[0] = send;
                    for (i = 1; i < s->arg[1]; i++) {
                        offs = ALIGNED((strlen(send) + 1));
                        send += offs;
                        str[i] = send;
                    }
                    /* for (i = 0; i < s->arg[1]; i++)
                        DPRINTF("\n%s", str[i]); */
                }
                s->parg[2] = VAL(str);
            }
            break;
        case FEnum_glUniform1fv:
        case FEnum_glUniform1fvARB:
        case FEnum_glUniform1iv:
        case FEnum_glUniform1ivARB:
        case FEnum_glUniform1uiv:
        case FEnum_glUniform1uivEXT:
            s->datacb = ALIGNED(s->arg[1]*sizeof(uint32_t));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glUniform2fv:
        case FEnum_glUniform2fvARB:
        case FEnum_glUniform2iv:
        case FEnum_glUniform2ivARB:
        case FEnum_glUniform2uiv:
        case FEnum_glUniform2uivEXT:
            s->datacb = ALIGNED(2*s->arg[1]*sizeof(uint32_t));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glUniform3fv:
        case FEnum_glUniform3fvARB:
        case FEnum_glUniform3iv:
        case FEnum_glUniform3ivARB:
        case FEnum_glUniform3uiv:
        case FEnum_glUniform3uivEXT:
            s->datacb = ALIGNED(3*s->arg[1]*sizeof(uint32_t));
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glUniform4fv:
        case FEnum_glUniform4fvARB:
        case FEnum_glUniform4iv:
        case FEnum_glUniform4ivARB:
        case FEnum_glUniform4uiv:
        case FEnum_glUniform4uivEXT:
        case FEnum_glUniformMatrix2fv:
        case FEnum_glUniformMatrix2fvARB:
            s->datacb = ALIGNED(4*s->arg[1]*sizeof(uint32_t));
            s->parg[2] = VAL(s->hshm);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix2x3fv:
        case FEnum_glUniformMatrix3x2fv:
            s->datacb = ALIGNED(6*s->arg[1]*sizeof(uint32_t));
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix2x4fv:
        case FEnum_glUniformMatrix4x2fv:
            s->datacb = ALIGNED(8*s->arg[1]*sizeof(uint32_t));
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix3fv:
        case FEnum_glUniformMatrix3fvARB:
            s->datacb = ALIGNED(9*s->arg[1]*sizeof(uint32_t));
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix3x4fv:
        case FEnum_glUniformMatrix4x3fv:
            s->datacb = ALIGNED(12*s->arg[1]*sizeof(uint32_t));
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix4fv:
        case FEnum_glUniformMatrix4fvARB:
            s->datacb = ALIGNED(16*s->arg[1]*sizeof(uint32_t));
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniform1dv:
            s->datacb = s->arg[1]*sizeof(double);
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glUniform2dv:
            s->datacb = 2*s->arg[1]*sizeof(double);
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glUniform3dv:
            s->datacb = 3*s->arg[1]*sizeof(double);
            s->parg[2] = VAL(s->hshm);
            break;
        case FEnum_glUniform4dv:
        case FEnum_glUniformMatrix2dv:
            s->datacb = 4*s->arg[1]*sizeof(double);
            s->parg[2] = VAL(s->hshm);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix2x3dv:
        case FEnum_glUniformMatrix3x2dv:
            s->datacb = 6*s->arg[1]*sizeof(double);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix2x4dv:
        case FEnum_glUniformMatrix4x2dv:
            s->datacb = 8*s->arg[1]*sizeof(double);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix3dv:
            s->datacb = 9*s->arg[1]*sizeof(double);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix3x4dv:
        case FEnum_glUniformMatrix4x3dv:
            s->datacb = 12*s->arg[1]*sizeof(double);
            s->parg[3] = VAL(s->hshm);
            break;
        case FEnum_glUniformMatrix4dv:
            s->datacb = 16*s->arg[1]*sizeof(double);
            s->parg[3] = VAL(s->hshm);
            break;
        default:
            break;
    }
    if (PArgsShouldAligned(s)) {
        for (int i = 0; i < 4; i++) {
            if (s->parg[i] & (sizeof(uintptr_t) - 1))
                DPRINTF("WARN: FEnum 0x%02X Unaligned parg[%d]\n", s->FEnum, i);
        }
    }
}

static void processFRet(MesaPTState *s)
{
    uint8_t *outshm = s->fifo_ptr + (MGLSHM_SIZE - (3*TARGET_PAGE_SIZE));

    if (PArgsShouldAligned(s) == 0) {
        s->parg[0] &= ~(sizeof(uintptr_t) - 1);
        s->parg[1] &= ~(sizeof(uintptr_t) - 1);
        s->parg[2] &= ~(sizeof(uintptr_t) - 1);
        s->parg[3] &= ~(sizeof(uintptr_t) - 1);
    }

    switch (s->FEnum) {
        case FEnum_glBindBuffer:
        case FEnum_glBindBufferARB:
            s->pixPackBuf = (s->arg[0] == GL_PIXEL_PACK_BUFFER)? s->arg[1]:s->pixPackBuf;
            s->pixUnpackBuf = (s->arg[0] == GL_PIXEL_UNPACK_BUFFER)? s->arg[1]:s->pixUnpackBuf;
            s->arrayBuf = (s->arg[0] == GL_ARRAY_BUFFER)? s->arg[1]:s->arrayBuf;
            s->elemArryBuf = (s->arg[0] == GL_ELEMENT_ARRAY_BUFFER)? s->arg[1]:s->elemArryBuf;
            break;
        case FEnum_glDeleteBuffers:
        case FEnum_glDeleteBuffersARB:
            for (int i = 0; i < s->arg[0]; i++) {
                s->pixPackBuf = (((uint32_t *)s->hshm)[i] == s->pixPackBuf)? 0:s->pixPackBuf;
                s->pixUnpackBuf = (((uint32_t *)s->hshm)[i] == s->pixUnpackBuf)? 0:s->pixUnpackBuf;
                s->arrayBuf = (((uint32_t *)s->hshm)[i] == s->arrayBuf)? 0:s->arrayBuf;
                s->elemArryBuf = (((uint32_t *)s->hshm)[i] == s->elemArryBuf)? 0:s->elemArryBuf;
            }
            break;
        case FEnum_glClientActiveTextureARB:
            s->texUnit = ((s->arg[0] & 0xFFF0U) == GL_TEXTURE0_ARB)? (s->arg[0] & 0x0FU):0;
            break;
        case FEnum_glDisable:
        case FEnum_glDisableClientState:
            vtxarry_state(s, s->arg[0], 0);
            break;
        case FEnum_glDisableVertexAttribArray:
        case FEnum_glDisableVertexAttribArrayARB:
            vtxarry_state(s, vattr2arry_state(s, s->arg[0]), 0);
            break;
        case FEnum_glEnable:
        case FEnum_glEnableClientState:
            vtxarry_state(s, s->arg[0], 1);
            break;
        case FEnum_glEnableVertexAttribArray:
        case FEnum_glEnableVertexAttribArrayARB:
            vtxarry_state(s, vattr2arry_state(s, s->arg[0]), 1);
            break;
        case FEnum_glFlush:
            if (s->KickFrame) {
                s->KickFrame = 1;
                MGLKickFrameProc(s->KickFrame);
                s->KickFrame--;
            }
            break;
        case FEnum_glMapBuffer:
        case FEnum_glMapBufferARB:
        case FEnum_glMapBufferRange:
            if (s->BufObj) {
                DPRINTF("  *WARN* GL buffer object contention, target %04x access %04x",
                    s->arg[0], ((s->FEnum == FEnum_glMapBufferRange)? s->arg[3]:s->arg[1]));
            }
            s->BufObj = (void *)(s->FRet);
            s->BufAcc = 0;
            if (s->FEnum == FEnum_glMapBufferRange) {
                s->BufAcc = s->arg[3];
                s->BufRange = s->arg[2];
                s->FRet = s->arg[2];
            }
            else {
                s->BufAcc |= (s->arg[1] == GL_READ_ONLY)? GL_MAP_READ_BIT:0;
                s->BufAcc |= (s->arg[1] == GL_WRITE_ONLY)? GL_MAP_WRITE_BIT:0;
                s->BufAcc |= (s->arg[1] == GL_READ_WRITE)? (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT):0;
                s->BufRange = 0;
                s->FRet = wrGetParamIa1p2((s->FEnum == FEnum_glMapBuffer)? FEnum_glGetBufferParameteriv:FEnum_glGetBufferParameterivARB,
                    s->arg[0], GL_BUFFER_SIZE);
            }
            break;
        case FEnum_glPixelStorei:
            s->szPackRow = (s->arg[0] == GL_PACK_ROW_LENGTH)? s->arg[1]:s->szPackRow;
            s->szUnpackRow = (s->arg[0] == GL_UNPACK_ROW_LENGTH)? s->arg[1]:s->szUnpackRow;
            //DPRINTF("PixelStorei %x %x", s->arg[0], s->arg[1]);
            break;
#define MGL_TRACE 0
#if defined(MGL_TRACE) && MGL_TRACE
        case FEnum_glGetBooleanv:
        case FEnum_glGetDoublev:
        case FEnum_glGetFloatv:
        case FEnum_glGetIntegerv:
            if ((s->logpname[s->arg[0] >> 3] & (1 << (s->arg[0] % 8))) == 0) {
                s->logpname[s->arg[0] >> 3] |= (1 << (s->arg[0] % 8));
                fprintf(stderr, "mgl_trace: Get() %04x ( %04x ): ", s->arg[0], *(int *)outshm);
                for (int i = 0; i < *(int *)outshm; i++) {
                    void *v = outshm + ALIGNED(sizeof(int));
                    if (s->FEnum == FEnum_glGetDoublev)
                        fprintf(stderr, "% .4f ", *(double *)PTR(v, i*sizeof(double)));
                    if (s->FEnum == FEnum_glGetFloatv)
                        fprintf(stderr, "% .2f ", *(float *)PTR(v, i*sizeof(float)));
                    if ((s->FEnum != FEnum_glGetFloatv) && (s->FEnum != FEnum_glGetDoublev))
                        fprintf(stderr, "%08X ", *(uint32_t *)PTR(v, i*sizeof(uint32_t)));
                }
                fprintf(stderr, "\n");
            }
            break;
        case FEnum_glGetTexLevelParameteriv:
            if ((s->logpname[s->arg[2] >> 3] & (1 << (s->arg[2] % 8))) == 0) {
                s->logpname[s->arg[2] >> 3] |= (1 << (s->arg[2] % 8));
                fprintf(stderr, "mgl_trace: GetTexLevelParameteriv() %x %x %04x ( %04x ): ", s->arg[0], s->arg[1], s->arg[2], *(int *)outshm);
                for (int i = 0; i < *(int *)outshm; i++) {
                    void *v = outshm + ALIGNED(sizeof(int));
                    fprintf(stderr, "%08X ", *(uint32_t *)PTR(v, i*sizeof(uint32_t)));
                }
                fprintf(stderr, "\n");
            }
            break;
        case FEnum_glGetAttribLocation:
        case FEnum_glGetAttribLocationARB:
        case FEnum_glGetUniformLocation:
        case FEnum_glGetUniformLocationARB:
            if (-1 != (uint32_t)s->FRet) {
                DPRINTF("%sLocation %s %d", ((s->FEnum == FEnum_glGetAttribLocation) || (s->FEnum == FEnum_glGetAttribLocationARB))?
                        "Attrib":"Uniform", (char *)s->hshm, (uint32_t)s->FRet);
            }
            break;
#endif
        case FEnum_glGetString:
            if (s->FRet) {
                size_t len = strnlen((char *)s->FRet, 3*TARGET_PAGE_SIZE);
                len++; /* '\0' */
                if ((s->arg[0] & 0x03U) != 0x03U) {
                    strncpy((char *)outshm, (char *)s->FRet, len);
                    DPRINTF("%s [ %u ]", (char *)outshm, (uint32_t)len);
                }
                else {
                    char *tmpstr, *stok, *xbuf = (char *)outshm;
                    tmpstr = g_new0(char, len);
                    strncpy(tmpstr, (char *)s->FRet, len);
                    //DPRINTF("Host GL Extensions:\n%s", tmpstr);
                    stok = strtok(tmpstr, " ");
                    while (stok) {
                        size_t extnLength = strnlen(stok, TARGET_PAGE_SIZE);
                        for (int i = 0; i < MESA_EXTENSION_COUNT; i++) {
                            if ((s->extnLength == 0) || (s->extnLength >= extnLength)) {
                                if (!memcmp(_mesa_extension_table[i].name, stok, extnLength)) {
                                    if (((s->extnYear == 0) || (s->extnYear >= _mesa_extension_table[i].year))) {
                                        memcpy(xbuf, stok, extnLength);
                                        xbuf += extnLength;
                                        *xbuf = ' ';
                                        xbuf++;
                                    }
                                }
                            }
                        }
                        //DPRINTF("  %s[ %u ]", stok, (uint32_t)extnLength);
                        stok = strtok(NULL, " ");
                    }
                    memcpy(xbuf, "WGL_EXT_swap_control", sizeof("WGL_EXT_swap_control"));
                    xbuf += sizeof("WGL_EXT_swap_control");
                    *xbuf = '\0';
                    g_free(tmpstr);
                }
            }
            break;
        case FEnum_glGetStringi:
            if (s->FRet) {
                size_t n;
                char str[255];
                //DPRINTF("GetStringi() %04x %s", s->arg[1], s->FRet);
                memset(str, 0, sizeof(str));
                strncpy(str, (char *)s->FRet, sizeof(str));
                n = strnlen(str, sizeof(str) - 1);
                *(int *)outshm = ++n;
                memcpy(PTR(outshm, sizeof(int)), str, n);
            }
            break;
        default:
            break;
    }
}

static void processFifo(MesaPTState *s)
{
    uint32_t *fifoptr = (uint32_t *)s->fifo_ptr;
    uint32_t *dataptr = (uint32_t *)(s->fifo_ptr + (MAX_FIFO << 2));
    int FEnum = s->FEnum, i = FIRST_FIFO, j = ALIGNED(1) >> 2;

    if (fifoptr[0] - FIRST_FIFO) {
#define DEBUG_FIFO 0
#if DEBUG_FIFO
        const char *fstr = getGLFuncStr(s->FEnum);
        if (fstr)
            DPRINTF("FIFO depth %s fifoptr %06x dataptr %06x", fstr, fifoptr[0], dataptr[0]);
        if (dataptr[0] >= MAX_DATA) {
            DPRINTF("  *WARN* Data bound overlapped 0x%02x dataptr %06X", s->FEnum, dataptr[0]);
        }
#endif
        while (i < fifoptr[0]) {
            int numArgs, numData;
            s->FEnum = fifoptr[i++];
            numArgs = GLFEnumArgsCnt(s->FEnum);
#if DEBUG_FIFO
            if (i == (FIRST_FIFO + 1))
                fprintf(stderr, "FIFO { [%02X] fifo %04x data %04x\n%02X ", FEnum, fifoptr[0], dataptr[0], s->FEnum);
            else
                fprintf(stderr, "%02X ", s->FEnum);
#endif
            s->datacb = 0;
            s->arg = &fifoptr[i];
            s->hshm = &dataptr[j];
            processArgs(s);
            doMesaFunc(s->FEnum, s->arg, s->parg, &(s->FRet));
            processFRet(s);
            numData = (s->datacb & 0x03)? ((s->datacb >> 2) + 1):(s->datacb >> 2);
            i += numArgs;
            j += numData;
        }
#if DEBUG_FIFO
        if (i != FIRST_FIFO)
            fprintf(stderr, "\n} [%02X] fifo %04x data %d/%d\n", FEnum, i, j, dataptr[0]);
#endif
        s->fifoMax = (s->fifoMax < i)? i:s->fifoMax;
        fifoptr[0] = FIRST_FIFO;
        s->FEnum = FEnum;
    }
    s->datacb = 0;
    s->arg = &fifoptr[2];
    s->hshm = &dataptr[j];
    if (j > (ALIGNED(1) >> 2)){
        s->dataMax = (s->dataMax < dataptr[0])? dataptr[0]:s->dataMax;
        dataptr[0] -= j;
    }
}

static void mesapt_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MesaPTState *s = opaque;

    if (addr == 0xFBC) {
        switch (val) {
            case 0xA0320:
                if (InitMesaGL() == 0) {
                    s->MesaVer = (uint32_t)((val >> 12) & 0xFFU) | ((val & 0xFFFU) << 8);
                    MGLTmpContext();
                    DPRINTF("DLL loaded");
                }
                break;
            case 0xD0320:
                MGLWndRelease();
                FiniMesaGL();
                DPRINTF("DLL unloaded");
                break;
        }
    }

    if (addr == 0xFC0) {
        if (s->mglContext && s->mglCntxCurrent) {
            s->FEnum = val;
            processFifo(s);
            processArgs(s);
            doMesaFunc(s->FEnum, s->arg, s->parg, &(s->FRet));
            processFRet(s);
            do {
                uint32_t *dataptr = (uint32_t *)(s->fifo_ptr + (MAX_FIFO << 2));
                uint32_t numData = (s->datacb & 0x03)? ((s->datacb >> 2) + 1):(s->datacb >> 2);
                if ((dataptr[0] - numData) > (ALIGNED(1) >> 2))
                    DPRINTF("WARN: FIFO data leak 0x%02x %06x %06x", s->FEnum, dataptr[0], numData);
                dataptr[0] = ALIGNED(1) >> 2;
            } while (0);
        }
        else
            DPRINTF("WARN: No GL context for func %04x", (uint32_t)val);
    }

    if (val == MESAGL_MAGIC) {
        if (s->mglContext && s->mglCntxCurrent) {
            processFifo(s);
            do {
                uint32_t *dataptr = (uint32_t *)(s->fifo_ptr + (MAX_FIFO << 2));
                if (dataptr[0] > (ALIGNED(1) >> 2))
                    DPRINTF("WARN: FIFO data leak 0x%02x %d", s->FEnum, dataptr[0]);
                dataptr[0] = ALIGNED(1) >> 2;
            } while (0);
        }
        switch(addr) {
            case 0xFFC:
                do {
                    uint32_t *cntxRC = (uint32_t *)(s->fifo_ptr + (MGLSHM_SIZE - TARGET_PAGE_SIZE));
                    if (!s->mglContext) {
                        DPRINTF("wglCreateContext cntx %d curr %d", s->mglContext, s->mglCntxCurrent);
                        s->mglContext = MGLCreateContext(cntxRC[0])? 0:1;
                        s->fifoMax = 0; s->dataMax = 0;
                        s->BufObj = 0;
                        InitClientStates(s);
                        ImplMesaGLReset();
                    }
                    else {
                        //DPRINTF("wglCreateContext cntx %d curr %d %x", s->mglContext, s->mglCntxCurrent, cntxRC[0]);
                        MGLCreateContext(cntxRC[0]);
                    }
                } while(0);
                break;
            case 0xFF8:
                do {
                    char xYear[8], xLen[8];
                    uint32_t *ptVer = (uint32_t *)(s->fifo_ptr + (MGLSHM_SIZE - TARGET_PAGE_SIZE));
                    if (s->mglContext && !s->mglCntxCurrent) {
                        DPRINTF("wglMakeCurrent cntx %d curr %d", s->mglContext, s->mglCntxCurrent);
                        DPRINTF("======== %s ========", (char *)&ptVer[1]);
                        s->mglCntxCurrent = MGLMakeCurrent(ptVer[0])? 0:1;
                        s->logpname = g_new0(uint8_t, 0x2000);
                        s->KickFrame = GetKickFrame();
                        s->extnYear = GetGLExtYear();
                        s->extnLength = GetGLExtLength();
                        s->szVertCache = GetVertCacheMB() << 19;
                        snprintf(xLen, 8, "%u", (uint32_t)s->extnLength);
                        snprintf(xYear, 8, "%d", s->extnYear);
                        DPRINTF("VertexArrayCache %dMB", GetVertCacheMB());
                        DPRINTF("Guest GL Extensions pass-through for Year %s Length %s",
                                (s->extnYear)? xYear:"ALL", (s->extnLength)? xLen:"ANY");
                        if (s->KickFrame)
                            MGLKickFrameProc(s->KickFrame);
                    }
                    else {
                        //DPRINTF("wglMakeCurrent cntx %d curr %d %x", s->mglContext, s->mglCntxCurrent, ptVer[0]);
                        MGLMakeCurrent(ptVer[0]);
                    }
                } while(0);
                break;
            case 0xFF4:
                DPRINTF("wglDeleteContext cntx %d curr %d", s->mglContext, s->mglCntxCurrent);
                if (s->mglContext && s->mglCntxCurrent) {
                    s->perfs.last();
                    MGLDeleteContext();
                    g_free(s->logpname);
                    DPRINTF("VertexArrayStats: elemMax %06x vertexCache %04x", s->elemMax, FreeVertex());
                    DPRINTF("MGLStats: fifo 0x%07x data 0x%07x", s->fifoMax, s->dataMax);
                }
                s->mglContext = 0;
                s->mglCntxCurrent = 0;
                break;
            case 0xFF0:
                //DPRINTF(">>>>>>>> wglSwapBuffers <<<<<<<<");
                s->perfs.stat();
                do {
                    uint32_t *swapRet = (uint32_t *)(s->fifo_ptr + (MGLSHM_SIZE - ALIGNED(1)));
                    if (s->KickFrame) {
                        MGLKickFrameProc(s->KickFrame);
                        s->KickFrame--;
                    }
                    swapRet[0] = MGLSwapBuffers();
                } while(0);
                break;
            case 0xFEC:
                s->pixfmt = MGLChoosePixelFormat();
                break;
            case 0xFE8:
                do {
                    uint8_t *ppfd = s->fifo_ptr + (MGLSHM_SIZE - TARGET_PAGE_SIZE);
                    int pixfmt = *(int *)ppfd;
                    unsigned int nbytes = *(uint32_t *)PTR(ppfd, sizeof(int));
                    s->pixfmtMax = MGLDescribePixelFormat(pixfmt, nbytes, ppfd);
                } while(0);
                break;
            case 0xFE4:
                do {
                    uint8_t *ppfd = s->fifo_ptr + (MGLSHM_SIZE - TARGET_PAGE_SIZE);
                    int pixfmt = *(int *)ppfd;
                    s->procRet = MGLSetPixelFormat(pixfmt, PTR(ppfd, ALIGNED(sizeof(int))))? MESAGL_MAGIC:0;
                } while(0);
                break;
            case 0xFE0:
                do {
                    uint8_t *name = s->fifo_ptr + (MGLSHM_SIZE - TARGET_PAGE_SIZE);
                    s->procRet = (ExtFuncIsValid((char *)name))? MESAGL_MAGIC:0;
                    if (s->procRet == 0)
                        DPRINTF("  query_ext: %s -- %s", name, (s->procRet)? "OK":"Missing");
                } while (0);
                break;
            case 0xFDC:
                do {
                    uint8_t *func = s->fifo_ptr + (MGLSHM_SIZE - TARGET_PAGE_SIZE);
                    MGLFuncHandler((const char *)func);
                } while(0);
                break;
            case 0xFD8:
                do {
                    int *i = (int *)(s->fifo_ptr + (MGLSHM_SIZE - TARGET_PAGE_SIZE));
                    MGLActivateHandler(i[0]);
                } while(0);
            default:
                break;
        }
    }
}

static const MemoryRegionOps mesapt_ops = {
    .read = mesapt_read,
    .write = mesapt_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void mesapt_reset(DeviceState *d)
{
    //MesaPTState *s = MESAPT(d);
}

static void mesapt_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    MesaPTState *s = MESAPT(obj);
    MemoryRegion *sysmem = get_system_memory();

    memory_region_init_ram(&s->fifo_ram, NULL, "mglshm", MGLSHM_SIZE, &error_fatal);
    memory_region_init_ram(&s->fbtm_ram, NULL, "mglfbt", MGLFBT_SIZE, &error_fatal);
    s->fifo_ptr = memory_region_get_ram_ptr(&s->fifo_ram);
    s->fbtm_ptr = memory_region_get_ram_ptr(&s->fbtm_ram);
    memory_region_add_subregion(sysmem, MESA_FIFO_BASE, &s->fifo_ram);
    memory_region_add_subregion(sysmem, MESA_FBTM_BASE, &s->fbtm_ram);

    memory_region_init_io(&s->iomem, obj, &mesapt_ops, s, TYPE_MESAPT, TARGET_PAGE_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void mesapt_realize(DeviceState *dev, Error **errp)
{
    MesaPTState *s = MESAPT(dev);
    mesastat(&s->perfs);
}

static void mesapt_finalize(Object *obj)
{
    //MesaPTState *s = MESAPT(obj);
}

static void mesapt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mesapt_realize;
    dc->reset = mesapt_reset;
}

static const TypeInfo mesapt_info = {
    .name = TYPE_MESAPT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MesaPTState),
    .instance_init = mesapt_init,
    .instance_finalize = mesapt_finalize,
    .class_init = mesapt_class_init,
};

static void mesapt_register_type(void)
{
    type_register_static(&mesapt_info);
}

type_init(mesapt_register_type)