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
#include "hw/hw.h"

#include "mesagl_impl.h"

//#define DEBUG_MESAGL

#ifdef DEBUG_MESAGL
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "mgl_trace: " fmt "\n" , ## __VA_ARGS__); } while(0)
#else
#define DPRINTF(fmt, ...)
#endif

#if defined(CONFIG_LINUX) && CONFIG_LINUX
#include <dlfcn.h>
  #if defined(HOST_X86_64) && HOST_X86_64
  #define __stdcall
  #endif
#endif

#define MESAGLCFG "mesagl.cfg"
#include "mglfptbl.h"

static int getNumArgs(const char *sym)
{
    char *p = (char *)sym;
    while (*p != '@') p++;
    return (atoi(++p) >> 2);
}

int GLFEnumArgsCnt(int FEnum)
{
    int val = getNumArgs(tblMesaGL[FEnum].sym);
    return val;
}

int ExtFuncIsValid(char *name)
{
    int i;
    for (i = 0; i < FEnum_zzMGLFuncEnum_max; i++) {
        char func[64];
        strncpy(func, tblMesaGL[i].sym + 1, sizeof(func));
        for (int j = 0; j < sizeof(func); j++) {
            if (func[j] == '@') {
                func[j] = 0;
                break;
            }
        }
        if (!strncmp(func, name, sizeof(func)))
            break;
    }
    return (i == FEnum_zzMGLFuncEnum_max)? 0:((tblMesaGL[i].ptr)? 1:0);
}

int wrMapOrderPoints(uint32_t target)
{
    int v[2] = {1, 1};
    void (__stdcall *fpa1p2)(uint32_t, uint32_t, int *);
    fpa1p2 = tblMesaGL[FEnum_glGetMapiv].ptr;
    fpa1p2(target, GL_ORDER, v);
    return (v[0]*v[1]);
}

int wrTexTextureWxD(uint32_t target, uint32_t level, int compressed)
{
    int w, h, csize;
    void (__stdcall *fpra2p3)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uintptr_t arg3);
    fpra2p3 = tblMesaGL[FEnum_glGetTexLevelParameteriv].ptr;
    if (compressed)
        fpra2p3(target, level, GL_TEXTURE_COMPRESSED_IMAGE_SIZE, (uintptr_t)&csize);
    else {
        fpra2p3(target, level,  GL_TEXTURE_WIDTH, (uintptr_t)&w);
        fpra2p3(target, level,  GL_TEXTURE_HEIGHT, (uintptr_t)&h);
        csize = w * h;
    }
    return csize;
}

int wrGetParamIa1p2(uint32_t FEnum, uint32_t arg0, uint32_t arg1)
{
    int ret;
    void (__stdcall *fpa1p2)(uint32_t arg0, uint32_t arg1, uintptr_t arg2);
    fpa1p2 = tblMesaGL[FEnum].ptr;
    fpa1p2(arg0, arg1, (uintptr_t)&ret);
    return ret;
}

void wrFillBufObj(uint32_t target, void *dst, uint32_t offset, uint32_t range)
{
    void *src;
    void *(__stdcall *wrMapRange)(uint32_t arg0, uintptr_t arg1, uintptr_t arg2, uint32_t arg3);
    void *(__stdcall *wrMap)(uint32_t arg0, uint32_t arg1);
    uint32_t (__stdcall *wrUnmap)(uint32_t arg0);

    switch (target) {
        case GL_PIXEL_UNPACK_BUFFER:
            break;
        default:
            wrMapRange = tblMesaGL[FEnum_glMapBufferRange].ptr;
            wrMap = tblMesaGL[FEnum_glMapBuffer].ptr;
            wrUnmap = tblMesaGL[FEnum_glUnmapBuffer].ptr;
            src = (range == 0)? wrMap(target, GL_READ_ONLY):wrMapRange(target, offset, range, GL_MAP_READ_BIT);
            if (src) {
                uint32_t szBuf = (range == 0)? wrGetParamIa1p2(FEnum_glGetBufferParameteriv, target, GL_BUFFER_SIZE):range;
                memcpy((dst - ALIGNED(szBuf)), src, szBuf);
                wrUnmap(target);
            }
            break;
    }
}

void wrFlushBufObj(int FEnum, uint32_t target, mapbufo_t *bufo)
{
    uint32_t szBuf = (bufo->range == 0)? wrGetParamIa1p2(FEnum, target, GL_BUFFER_SIZE):bufo->range;
    memcpy(bufo->hptr + bufo->offst, (bufo->shmep - ALIGNED(bufo->mapsz) + bufo->offst), szBuf);
}

const char *getGLFuncStr(int FEnum)
{
    if (tblMesaGL[FEnum].impl == 0) {
        tblMesaGL[FEnum].impl = 1;
        return tblMesaGL[FEnum].sym;
    }
    return 0;
}

void doMesaFunc(int FEnum, uint32_t *arg, uintptr_t *parg, uintptr_t *ret)
{
    int numArgs = getNumArgs(tblMesaGL[FEnum].sym);

#ifdef DEBUG_MESAGL
    const char *fstr = getGLFuncStr(FEnum);
    if (fstr) {
        DPRINTF("%-64s", tblMesaGL[FEnum].sym);
    }
#endif

    /* Handle special GL funcs */
#define GLDONE() \
    numArgs = -1; break

    typedef union {
        uintptr_t (__stdcall *rpfpa0)(uint32_t);
        uintptr_t (__stdcall *rpfpa1)(uint32_t, uint32_t);
        uintptr_t (__stdcall *rpfpa0p2a3)(uint32_t, uintptr_t, uintptr_t, uint32_t);
        uint32_t (__stdcall *fpp0)(uintptr_t);
        uint32_t (__stdcall *fpp1)(uintptr_t, uintptr_t);
        uint32_t (__stdcall *fpa0p1)(uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa0p2)(uint32_t, uintptr_t, uintptr_t);
        uint32_t (__stdcall *fpa0p3)(uint32_t, uintptr_t, uintptr_t, uintptr_t);
        uint32_t (__stdcall *fpa1p2)(uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa1p3)(uint32_t, uint32_t, uintptr_t, uintptr_t);
        uint32_t (__stdcall *fpa0p2a3)(uint32_t, uintptr_t, uintptr_t, uint32_t);
        uint32_t (__stdcall *fpa2p3)(uint32_t, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa2p3a4)(uint32_t, uint32_t, uint32_t, uintptr_t, uint32_t);
        uint32_t (__stdcall *fpa3p4)(uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa4p5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa5p6)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa6p7)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa7p8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa8p9)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa9p10)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
        /* float func proto */
        uint32_t (__stdcall *fpa0f1)(uint32_t, float);
        uint32_t (__stdcall *fpa0f2)(uint32_t, float, float);
        uint32_t (__stdcall *fpa0f3)(uint32_t, float, float, float);
        uint32_t (__stdcall *fpa0f4)(uint32_t, float, float, float, float);
        uint32_t (__stdcall *fpa0f2a3f5)(uint32_t, float, float, uint32_t, float, float);
        uint32_t (__stdcall *fpa1f2)(uint32_t, uint32_t, float);
        uint32_t (__stdcall *fpa1f5)(uint32_t, uint32_t, float, float, float, float);
        uint32_t (__stdcall *fpa0f2p6)(uint32_t, uint32_t, float, float, float, float, uintptr_t);
        uint32_t (__stdcall *fpa0f2a4p5)(uint32_t, float, float, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa0f2a4f6a8p9)(uint32_t, float, float, uint32_t, uint32_t, float, float, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpf0)(float);
        uint32_t (__stdcall *fpf1)(float, float);
        uint32_t (__stdcall *fpf2)(float, float, float);
        uint32_t (__stdcall *fpf3)(float, float, float, float);
        uint32_t (__stdcall *fpf5)(float, float, float, float, float, float);
        uint32_t (__stdcall *fpf7)(float, float, float, float, float, float, float, float);
        /* double func proto */
        uint32_t (__stdcall *fpd0)(double);
        uint32_t (__stdcall *fpd1)(double, double);
        uint32_t (__stdcall *fpd2)(double, double, double);
        uint32_t (__stdcall *fpd3)(double, double, double, double);
        uint32_t (__stdcall *fpd5)(double, double, double, double, double, double);
        uint32_t (__stdcall *fpa0d1)(uint32_t, double);
        uint32_t (__stdcall *fpa0d2)(uint32_t, double, double);
        uint32_t (__stdcall *fpa0d3)(uint32_t, double, double, double);
        uint32_t (__stdcall *fpa0d4)(uint32_t, double, double, double, double);
        uint32_t (__stdcall *fpa1d2)(uint32_t, uint32_t, double);
        uint32_t (__stdcall *fpa1d5)(uint32_t, uint32_t, double, double, double, double);
        uint32_t (__stdcall *fpa0d2a3d5)(uint32_t, double, double, uint32_t, double, double);
        uint32_t (__stdcall *fpa0d2a4p5)(uint32_t, double, double, uint32_t, uint32_t, uintptr_t);
        uint32_t (__stdcall *fpa0d2a4d6a8p9)(uint32_t, double, double, uint32_t, uint32_t, double, double, uint32_t, uint32_t, uintptr_t);
    } USFP;
    USFP usfp;

    switch(FEnum) {
        case FEnum_glAreTexturesResident:
        case FEnum_glAreTexturesResidentEXT:
        case FEnum_glFlushMappedBufferRange:
        case FEnum_glFlushMappedNamedBufferRange:
        case FEnum_glPrioritizeTextures:
        case FEnum_glPrioritizeTexturesEXT:
            usfp.fpa0p2 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa0p2)(arg[0], parg[1], parg[2]);
            GLDONE();
        case FEnum_glBufferSubData:
        case FEnum_glBufferSubDataARB:
        case FEnum_glGetBufferSubData:
        case FEnum_glGetBufferSubDataARB:
        case FEnum_glNamedBufferSubData:
        case FEnum_glNamedBufferSubDataEXT:
            usfp.fpa0p3 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa0p3)(arg[0], parg[1], parg[2], parg[3]);
            GLDONE();
        case FEnum_glBindFragDataLocationIndexed:
        case FEnum_glColorPointer:
        case FEnum_glDrawElements:
        case FEnum_glGetTexLevelParameterfv:
        case FEnum_glGetTexLevelParameteriv:
        case FEnum_glIndexPointerEXT:
        case FEnum_glNormalPointerEXT:
        case FEnum_glProgramEnvParameters4fvEXT:
        case FEnum_glProgramLocalParameters4fvEXT:
        case FEnum_glProgramStringARB:
        case FEnum_glSecondaryColorPointer:
        case FEnum_glSecondaryColorPointerEXT:
        case FEnum_glTexCoordPointer:
        case FEnum_glUniformMatrix2dv:
        case FEnum_glUniformMatrix2fv:
        case FEnum_glUniformMatrix2fvARB:
        case FEnum_glUniformMatrix2x3dv:
        case FEnum_glUniformMatrix2x3fv:
        case FEnum_glUniformMatrix2x4dv:
        case FEnum_glUniformMatrix2x4fv:
        case FEnum_glUniformMatrix3dv:
        case FEnum_glUniformMatrix3fv:
        case FEnum_glUniformMatrix3fvARB:
        case FEnum_glUniformMatrix3x2dv:
        case FEnum_glUniformMatrix3x2fv:
        case FEnum_glUniformMatrix3x4dv:
        case FEnum_glUniformMatrix3x4fv:
        case FEnum_glUniformMatrix4dv:
        case FEnum_glUniformMatrix4fv:
        case FEnum_glUniformMatrix4fvARB:
        case FEnum_glUniformMatrix4x2dv:
        case FEnum_glUniformMatrix4x2fv:
        case FEnum_glUniformMatrix4x3dv:
        case FEnum_glUniformMatrix4x3fv:
        case FEnum_glVertexPointer:
        case FEnum_glVertexWeightPointerEXT:
        case FEnum_glWeightPointerARB:
            usfp.fpa2p3 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa2p3)(arg[0], arg[1], arg[2], parg[3]);
            GLDONE();
        case FEnum_glDrawElementsBaseVertex:
            usfp.fpa2p3a4 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa2p3a4)(arg[0], arg[1], arg[2], parg[3], arg[4]);
            GLDONE();
        case FEnum_glColorPointerEXT:
        case FEnum_glDrawPixels:
        case FEnum_glGetInternalformativ:
        case FEnum_glGetTexImage:
        case FEnum_glTexCoordPointerEXT:
        case FEnum_glVertexPointerEXT:
            usfp.fpa3p4 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa3p4)(arg[0], arg[1], arg[2], arg[3], parg[0]);
            GLDONE();
        case FEnum_glColorSubTable:
        case FEnum_glColorSubTableEXT:
        case FEnum_glColorTable:
        case FEnum_glColorTableEXT:
        case FEnum_glDrawRangeElements:
        case FEnum_glDrawRangeElementsEXT:
        case FEnum_glVertexAttribPointer:
        case FEnum_glVertexAttribPointerARB:
            usfp.fpa4p5 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa4p5)(arg[0], arg[1], arg[2], arg[3], arg[4], parg[1]);
            GLDONE();
        case FEnum_glGetString:
            usfp.rpfpa0 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.rpfpa0)(arg[0]);
            GLDONE();
        case FEnum_glGetStringi:
        case FEnum_glMapBuffer:
        case FEnum_glMapBufferARB:
            usfp.rpfpa1 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.rpfpa1)(arg[0], arg[1]);
            GLDONE();
        case FEnum_glMapBufferRange:
            usfp.rpfpa0p2a3 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.rpfpa0p2a3)(arg[0], parg[1], parg[2], arg[3]);
            GLDONE();
        case FEnum_glClipPlane:
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
        case FEnum_glEdgeFlagPointer:
        case FEnum_glFogfv:
        case FEnum_glFogiv:
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
        case FEnum_glGetAttribLocation:
        case FEnum_glGetAttribLocationARB:
        case FEnum_glGetBooleanv:
        case FEnum_glGetClipPlane:
        case FEnum_glGetDoublev:
        case FEnum_glGetFloatv:
        case FEnum_glGetIntegerv:
        case FEnum_glGetUniformLocation:
        case FEnum_glGetUniformLocationARB:
        case FEnum_glLightModelfv:
        case FEnum_glLightModeliv:
        case FEnum_glMultiTexCoord1dv:
        case FEnum_glMultiTexCoord1dvARB:
        case FEnum_glMultiTexCoord1fv:
        case FEnum_glMultiTexCoord1fvARB:
        case FEnum_glMultiTexCoord1iv:
        case FEnum_glMultiTexCoord1ivARB:
        case FEnum_glMultiTexCoord1sv:
        case FEnum_glMultiTexCoord1svARB:
        case FEnum_glMultiTexCoord2dv:
        case FEnum_glMultiTexCoord2dvARB:
        case FEnum_glMultiTexCoord2fv:
        case FEnum_glMultiTexCoord2fvARB:
        case FEnum_glMultiTexCoord2iv:
        case FEnum_glMultiTexCoord2ivARB:
        case FEnum_glMultiTexCoord2sv:
        case FEnum_glMultiTexCoord2svARB:
        case FEnum_glMultiTexCoord3dv:
        case FEnum_glMultiTexCoord3dvARB:
        case FEnum_glMultiTexCoord3fv:
        case FEnum_glMultiTexCoord3fvARB:
        case FEnum_glMultiTexCoord3iv:
        case FEnum_glMultiTexCoord3ivARB:
        case FEnum_glMultiTexCoord3sv:
        case FEnum_glMultiTexCoord3svARB:
        case FEnum_glMultiTexCoord4dv:
        case FEnum_glMultiTexCoord4dvARB:
        case FEnum_glMultiTexCoord4fv:
        case FEnum_glMultiTexCoord4fvARB:
        case FEnum_glMultiTexCoord4iv:
        case FEnum_glMultiTexCoord4ivARB:
        case FEnum_glMultiTexCoord4sv:
        case FEnum_glMultiTexCoord4svARB:
        case FEnum_glPointParameterfv:
        case FEnum_glPointParameterfvARB:
        case FEnum_glPointParameterfvEXT:
        case FEnum_glPointParameteriv:
        case FEnum_glScissorIndexedv:
        case FEnum_glSelectBuffer:
        case FEnum_glVertexAttrib1dv:
        case FEnum_glVertexAttrib1dvARB:
        case FEnum_glVertexAttrib1fv:
        case FEnum_glVertexAttrib1fvARB:
        case FEnum_glVertexAttrib1sv:
        case FEnum_glVertexAttrib1svARB:
        case FEnum_glVertexAttrib2dv:
        case FEnum_glVertexAttrib2dvARB:
        case FEnum_glVertexAttrib2fv:
        case FEnum_glVertexAttrib2fvARB:
        case FEnum_glVertexAttrib2sv:
        case FEnum_glVertexAttrib2svARB:
        case FEnum_glVertexAttrib3dv:
        case FEnum_glVertexAttrib3dvARB:
        case FEnum_glVertexAttrib3fv:
        case FEnum_glVertexAttrib3fvARB:
        case FEnum_glVertexAttrib3sv:
        case FEnum_glVertexAttrib3svARB:
        case FEnum_glVertexAttrib4Nbv:
        case FEnum_glVertexAttrib4NbvARB:
        case FEnum_glVertexAttrib4Niv:
        case FEnum_glVertexAttrib4NivARB:
        case FEnum_glVertexAttrib4Nsv:
        case FEnum_glVertexAttrib4NsvARB:
        case FEnum_glVertexAttrib4Nubv:
        case FEnum_glVertexAttrib4NubvARB:
        case FEnum_glVertexAttrib4Nuiv:
        case FEnum_glVertexAttrib4NuivARB:
        case FEnum_glVertexAttrib4Nusv:
        case FEnum_glVertexAttrib4NusvARB:
        case FEnum_glVertexAttrib4bv:
        case FEnum_glVertexAttrib4bvARB:
        case FEnum_glVertexAttrib4dv:
        case FEnum_glVertexAttrib4dvARB:
        case FEnum_glVertexAttrib4fv:
        case FEnum_glVertexAttrib4fvARB:
        case FEnum_glVertexAttrib4iv:
        case FEnum_glVertexAttrib4ivARB:
        case FEnum_glVertexAttrib4sv:
        case FEnum_glVertexAttrib4svARB:
        case FEnum_glVertexAttrib4ubv:
        case FEnum_glVertexAttrib4ubvARB:
        case FEnum_glVertexAttrib4uiv:
        case FEnum_glVertexAttrib4uivARB:
        case FEnum_glVertexAttrib4usv:
        case FEnum_glVertexAttrib4usvARB:
        case FEnum_glViewportIndexedfv:
        case FEnum_glWeightbvARB:
        case FEnum_glWeightdvARB:
        case FEnum_glWeightfvARB:
        case FEnum_glWeightivARB:
        case FEnum_glWeightsvARB:
        case FEnum_glWeightubvARB:
        case FEnum_glWeightuivARB:
        case FEnum_glWeightusvARB:
            usfp.fpa0p1 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa0p1)(arg[0], parg[1]);
            GLDONE();
        case FEnum_glColor3bv:
        case FEnum_glColor3dv:
        case FEnum_glColor3fv:
        case FEnum_glColor3iv:
        case FEnum_glColor3sv:
        case FEnum_glColor3ubv:
        case FEnum_glColor3uiv:
        case FEnum_glColor3usv:
        case FEnum_glColor4bv:
        case FEnum_glColor4dv:
        case FEnum_glColor4fv:
        case FEnum_glColor4iv:
        case FEnum_glColor4sv:
        case FEnum_glColor4ubv:
        case FEnum_glColor4uiv:
        case FEnum_glColor4usv:
        case FEnum_glEdgeFlagv:
        case FEnum_glEvalCoord1dv:
        case FEnum_glEvalCoord1fv:
        case FEnum_glEvalCoord2dv:
        case FEnum_glEvalCoord2fv:
        case FEnum_glFogCoorddv:
        case FEnum_glFogCoorddvEXT:
        case FEnum_glFogCoordfv:
        case FEnum_glFogCoordfvEXT:
        case FEnum_glIndexdv:
        case FEnum_glIndexfv:
        case FEnum_glIndexiv:
        case FEnum_glIndexsv:
        case FEnum_glIndexubv:
        case FEnum_glLoadMatrixd:
        case FEnum_glLoadMatrixf:
        case FEnum_glMultMatrixd:
        case FEnum_glMultMatrixf:
        case FEnum_glNormal3bv:
        case FEnum_glNormal3dv:
        case FEnum_glNormal3fv:
        case FEnum_glNormal3iv:
        case FEnum_glNormal3sv:
        case FEnum_glPolygonStipple:
        case FEnum_glRasterPos2dv:
        case FEnum_glRasterPos2fv:
        case FEnum_glRasterPos2iv:
        case FEnum_glRasterPos2sv:
        case FEnum_glRasterPos3dv:
        case FEnum_glRasterPos3fv:
        case FEnum_glRasterPos3iv:
        case FEnum_glRasterPos3sv:
        case FEnum_glRasterPos4dv:
        case FEnum_glRasterPos4fv:
        case FEnum_glRasterPos4iv:
        case FEnum_glRasterPos4sv:
        case FEnum_glSecondaryColor3bv:
        case FEnum_glSecondaryColor3bvEXT:
        case FEnum_glSecondaryColor3dv:
        case FEnum_glSecondaryColor3dvEXT:
        case FEnum_glSecondaryColor3fv:
        case FEnum_glSecondaryColor3fvEXT:
        case FEnum_glSecondaryColor3iv:
        case FEnum_glSecondaryColor3ivEXT:
        case FEnum_glSecondaryColor3sv:
        case FEnum_glSecondaryColor3svEXT:
        case FEnum_glSecondaryColor3ubv:
        case FEnum_glSecondaryColor3ubvEXT:
        case FEnum_glSecondaryColor3uiv:
        case FEnum_glSecondaryColor3uivEXT:
        case FEnum_glSecondaryColor3usv:
        case FEnum_glSecondaryColor3usvEXT:
        case FEnum_glTexCoord2dv:
        case FEnum_glTexCoord2fv:
        case FEnum_glTexCoord2iv:
        case FEnum_glTexCoord2sv:
        case FEnum_glTexCoord3dv:
        case FEnum_glTexCoord3fv:
        case FEnum_glTexCoord3iv:
        case FEnum_glTexCoord3sv:
        case FEnum_glTexCoord4dv:
        case FEnum_glTexCoord4fv:
        case FEnum_glTexCoord4iv:
        case FEnum_glTexCoord4sv:
        case FEnum_glVertex2dv:
        case FEnum_glVertex2fv:
        case FEnum_glVertex2iv:
        case FEnum_glVertex2sv:
        case FEnum_glVertex3dv:
        case FEnum_glVertex3fv:
        case FEnum_glVertex3iv:
        case FEnum_glVertex3sv:
        case FEnum_glVertex4dv:
        case FEnum_glVertex4fv:
        case FEnum_glVertex4iv:
        case FEnum_glVertex4sv:
        case FEnum_glVertexWeightfvEXT:
            usfp.fpp0 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpp0)(parg[0]);
            GLDONE();
        case FEnum_glRectdv:
        case FEnum_glRectfv:
        case FEnum_glRectiv:
        case FEnum_glRectsv:
            usfp.fpp1 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpp1)(parg[0], parg[1]);
            GLDONE();
        case FEnum_glCompressedTexImage1D:
        case FEnum_glCompressedTexImage1DARB:
        case FEnum_glCompressedTexSubImage1D:
        case FEnum_glCompressedTexSubImage1DARB:
        case FEnum_glReadPixels:
        case FEnum_glTexSubImage1D:
        case FEnum_glTexSubImage1DEXT:
            usfp.fpa5p6 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa5p6)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], parg[2]);
            GLDONE();
        case FEnum_glBindAttribLocation:
        case FEnum_glBindAttribLocationARB:
        case FEnum_glBindFragDataLocation:
        case FEnum_glBindFragDataLocationEXT:
        case FEnum_glCallLists:
        case FEnum_glDepthRangeArrayv:
        case FEnum_glEdgeFlagPointerEXT:
        case FEnum_glFeedbackBuffer:
        case FEnum_glFogCoordPointer:
        case FEnum_glFogCoordPointerEXT:
        case FEnum_glGetBufferParameteriv:
        case FEnum_glGetBufferParameterivARB:
        case FEnum_glGetCompressedTexImage:
        case FEnum_glGetCompressedTexImageARB:
        case FEnum_glGetLightfv:
        case FEnum_glGetLightiv:
        case FEnum_glGetMapdv:
        case FEnum_glGetMapfv:
        case FEnum_glGetMapiv:
        case FEnum_glGetMaterialfv:
        case FEnum_glGetMaterialiv:
        case FEnum_glGetProgramiv:
        case FEnum_glGetProgramivARB:
        case FEnum_glGetQueryObjectiv:
        case FEnum_glGetQueryObjectivARB:
        case FEnum_glGetQueryObjectuiv:
        case FEnum_glGetQueryObjectuivARB:
        case FEnum_glGetQueryiv:
        case FEnum_glGetQueryivARB:
        case FEnum_glGetShaderiv:
        case FEnum_glGetTexEnvfv:
        case FEnum_glGetTexEnviv:
        case FEnum_glGetTexGendv:
        case FEnum_glGetTexGenfv:
        case FEnum_glGetTexGeniv:
        case FEnum_glGetTexParameterfv:
        case FEnum_glGetTexParameteriv:
        case FEnum_glIndexPointer:
        case FEnum_glInterleavedArrays:
        case FEnum_glLightfv:
        case FEnum_glLightiv:
        case FEnum_glMaterialfv:
        case FEnum_glMaterialiv:
        case FEnum_glNormalPointer:
        case FEnum_glPixelMapfv:
        case FEnum_glPixelMapuiv:
        case FEnum_glPixelMapusv:
        case FEnum_glProgramEnvParameter4dvARB:
        case FEnum_glProgramEnvParameter4fvARB:
        case FEnum_glProgramLocalParameter4dvARB:
        case FEnum_glProgramLocalParameter4fvARB:
        case FEnum_glSamplerParameterIiv:
        case FEnum_glSamplerParameterIuiv:
        case FEnum_glSamplerParameterfv:
        case FEnum_glSamplerParameteriv:
        case FEnum_glScissorArrayv:
        case FEnum_glTexEnvfv:
        case FEnum_glTexEnviv:
        case FEnum_glTexGendv:
        case FEnum_glTexGenfv:
        case FEnum_glTexGeniv:
        case FEnum_glTexParameterfv:
        case FEnum_glTexParameteriv:
        case FEnum_glUniform1dv:
        case FEnum_glUniform1fv:
        case FEnum_glUniform1fvARB:
        case FEnum_glUniform1iv:
        case FEnum_glUniform1ivARB:
        case FEnum_glUniform1uiv:
        case FEnum_glUniform1uivEXT:
        case FEnum_glUniform2dv:
        case FEnum_glUniform2fv:
        case FEnum_glUniform2fvARB:
        case FEnum_glUniform2iv:
        case FEnum_glUniform2ivARB:
        case FEnum_glUniform2uiv:
        case FEnum_glUniform2uivEXT:
        case FEnum_glUniform3dv:
        case FEnum_glUniform3fv:
        case FEnum_glUniform3fvARB:
        case FEnum_glUniform3iv:
        case FEnum_glUniform3ivARB:
        case FEnum_glUniform3uiv:
        case FEnum_glUniform3uivEXT:
        case FEnum_glUniform4dv:
        case FEnum_glUniform4fv:
        case FEnum_glUniform4fvARB:
        case FEnum_glUniform4iv:
        case FEnum_glUniform4ivARB:
        case FEnum_glUniform4uiv:
        case FEnum_glUniform4uivEXT:
        case FEnum_glViewportArrayv:
            usfp.fpa1p2 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa1p2)(arg[0], arg[1], parg[2]);
            GLDONE();
        case FEnum_glShaderSource:
        case FEnum_glShaderSourceARB:
            usfp.fpa1p3 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa1p3)(arg[0], arg[1], parg[2], parg[3]);
            GLDONE();
        case FEnum_glBufferData:
        case FEnum_glBufferDataARB:
            usfp.fpa0p2a3 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa0p2a3)(arg[0], parg[1], parg[2], arg[3]);
            GLDONE();
        case FEnum_glCompressedTexImage2D:
        case FEnum_glCompressedTexImage2DARB:
        case FEnum_glTexImage1D:
            usfp.fpa6p7 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa6p7)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], parg[3]);
            GLDONE();
        case FEnum_glCompressedTexImage3D:
        case FEnum_glCompressedTexImage3DARB:
        case FEnum_glCompressedTexSubImage2D:
        case FEnum_glCompressedTexSubImage2DARB:
        case FEnum_glTexImage2D:
        case FEnum_glTexSubImage2D:
        case FEnum_glTexSubImage2DEXT:
            usfp.fpa7p8 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa7p8)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], parg[0]);
            GLDONE();
        case FEnum_glTexImage3D:
        case FEnum_glTexImage3DEXT:
            usfp.fpa8p9 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa8p9)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], parg[1]);
            GLDONE();
        case FEnum_glCompressedTexSubImage3D:
        case FEnum_glCompressedTexSubImage3DARB:
        case FEnum_glTexSubImage3D:
        case FEnum_glTexSubImage3DEXT:
            usfp.fpa9p10 = tblMesaGL[FEnum].ptr;
            *ret = (*usfp.fpa9p10)(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], parg[2]);
            GLDONE();
        case FEnum_glDebugMessageCallback:
        case FEnum_glDebugMessageCallbackARB:
        case FEnum_glDebugMessageControl:
        case FEnum_glDebugMessageControlARB:
        case FEnum_glDebugMessageInsert:
        case FEnum_glDebugMessageInsertARB:
            GLDONE();

        /* GLFuncs with float args */
#define GLARGSF_N(a,i) \
            memcpy(&a, &arg[i], sizeof(float))
#define GLARGS2F(a,b) \
            memcpy(&a, &arg[0], sizeof(float)); \
            memcpy(&b, &arg[1], sizeof(float))
#define GLARGS3F(a,b,c) \
            memcpy(&a, &arg[0], sizeof(float)); \
            memcpy(&b, &arg[1], sizeof(float)); \
            memcpy(&c, &arg[2], sizeof(float))
#define GLARGS4F(a,b,c,d) \
            memcpy(&a, &arg[0], sizeof(float)); \
            memcpy(&b, &arg[1], sizeof(float)); \
            memcpy(&c, &arg[2], sizeof(float)); \
            memcpy(&d, &arg[3], sizeof(float))
#define GLARGS6F(a,b,c,d,e,f) \
            memcpy(&a, &arg[0], sizeof(float)); \
            memcpy(&b, &arg[1], sizeof(float)); \
            memcpy(&c, &arg[2], sizeof(float)); \
            memcpy(&d, &arg[3], sizeof(float)); \
            memcpy(&e, &arg[4], sizeof(float)); \
            memcpy(&f, &arg[5], sizeof(float))
#define GLARGS8F(a,b,c,d,e,f,g,h) \
            memcpy(&a, &arg[0], sizeof(float)); \
            memcpy(&b, &arg[1], sizeof(float)); \
            memcpy(&c, &arg[2], sizeof(float)); \
            memcpy(&d, &arg[3], sizeof(float)); \
            memcpy(&e, &arg[4], sizeof(float)); \
            memcpy(&f, &arg[5], sizeof(float)); \
            memcpy(&g, &arg[6], sizeof(float)); \
            memcpy(&h, &arg[7], sizeof(float))
        case FEnum_glClearIndex:
        case FEnum_glLineWidth:
        case FEnum_glMinSampleShading:
        case FEnum_glMinSampleShadingARB:
        case FEnum_glPassThrough:
        case FEnum_glPointSize:
        case FEnum_glClearDepthf:
        case FEnum_glEvalCoord1f:
        case FEnum_glFogCoordf:
        case FEnum_glFogCoordfEXT:
        case FEnum_glIndexf:
        case FEnum_glTexCoord1f:
        case FEnum_glVertexWeightfEXT:
            {
                float a0;
                GLARGSF_N(a0,0);
                usfp.fpf0 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpf0)(a0);
            }
            GLDONE();
        case FEnum_glDepthRangef:
        case FEnum_glPathStencilDepthOffsetNV:
        case FEnum_glPixelZoom:
        case FEnum_glPolygonOffset:
        case FEnum_glPolygonOffsetEXT:
        case FEnum_glEvalCoord2f:
        case FEnum_glRasterPos2f:
        case FEnum_glTexCoord2f:
        case FEnum_glVertex2f:
            {
                float a0, a1;
                GLARGS2F(a0,a1);
                usfp.fpf1 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpf1)(a0,a1);
            }
            GLDONE();
        case FEnum_glColor3f:
        case FEnum_glNormal3f:
        case FEnum_glPolygonOffsetClamp:
        case FEnum_glPolygonOffsetClampEXT:
        case FEnum_glRasterPos3f:
        case FEnum_glScalef:
        case FEnum_glSecondaryColor3f:
        case FEnum_glSecondaryColor3fEXT:
        case FEnum_glTexCoord3f:
        case FEnum_glTranslatef:
        case FEnum_glVertex3f:
            {
                float a0, a1, a2;
                GLARGS3F(a0,a1,a2);
                usfp.fpf3 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpf2)(a0,a1,a2);
            }
            GLDONE();
        case FEnum_glBlendColor:
        case FEnum_glBlendColorEXT:
        case FEnum_glClearColor:
        case FEnum_glClearAccum:
        case FEnum_glRectf:
        case FEnum_glRotatef:
        case FEnum_glColor4f:
        case FEnum_glRasterPos4f:
        case FEnum_glTexCoord4f:
        case FEnum_glVertex4f:
            {
                float a0, a1, a2, a3;
                GLARGS4F(a0,a1,a2,a3);
                usfp.fpf3 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpf3)(a0,a1,a2,a3);
            }
            GLDONE();
        case FEnum_glFrustumfOES:
        case FEnum_glOrthofOES:
            {
                float a0, a1, a2, a3, a4, a5;
                GLARGS6F(a0,a1,a2,a3,a4,a5);
                usfp.fpf5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpf5)(a0,a1,a2,a3,a4,a5);
            }
            GLDONE();
        case FEnum_glPrimitiveBoundingBoxARB:
            {
                float a0, a1, a2, a3, a4, a5, a6, a7;
                GLARGS8F(a0,a1,a2,a3,a4,a5,a6,a7);
                usfp.fpf7 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpf7)(a0,a1,a2,a3,a4,a5,a6,a7);
            }
            GLDONE();
        case FEnum_glAccum:
        case FEnum_glAlphaFunc:
        case FEnum_glFogf:
        case FEnum_glLightModelf:
        case FEnum_glMultiTexCoord1f:
        case FEnum_glMultiTexCoord1fARB:
        case FEnum_glPixelStoref:
        case FEnum_glPixelTransferf:
        case FEnum_glPointParameterf:
        case FEnum_glPointParameterfARB:
        case FEnum_glPointParameterfEXT:
        case FEnum_glUniform1f:
        case FEnum_glUniform1fARB:
        case FEnum_glVertexAttrib1f:
        case FEnum_glVertexAttrib1fARB:
            {
                float a1;
                GLARGSF_N(a1,1);
                usfp.fpa0f1 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f1)(arg[0], a1);
            }
            GLDONE();
        case FEnum_glMapGrid1f:
        case FEnum_glMultiTexCoord2f:
        case FEnum_glMultiTexCoord2fARB:
        case FEnum_glUniform2f:
        case FEnum_glUniform2fARB:
        case FEnum_glVertexAttrib2f:
        case FEnum_glVertexAttrib2fARB:
            {
                float a1, a2;
                GLARGSF_N(a1,1);
                GLARGSF_N(a2,2);
                usfp.fpa0f2 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f2)(arg[0], a1, a2);
            }
            GLDONE();
        case FEnum_glMultiTexCoord3f:
        case FEnum_glMultiTexCoord3fARB:
        case FEnum_glUniform3f:
        case FEnum_glUniform3fARB:
        case FEnum_glVertexAttrib3f:
        case FEnum_glVertexAttrib3fARB:
            {
                float a1, a2, a3;
                GLARGSF_N(a1,1);
                GLARGSF_N(a2,2);
                GLARGSF_N(a3,3);
                usfp.fpa0f3 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f3)(arg[0], a1, a2, a3);
            }
            GLDONE();
        case FEnum_glMultiTexCoord4f:
        case FEnum_glMultiTexCoord4fARB:
        case FEnum_glUniform4f:
        case FEnum_glUniform4fARB:
        case FEnum_glVertexAttrib4f:
        case FEnum_glVertexAttrib4fARB:
        case FEnum_glViewportIndexedf:
            {
                float a1, a2, a3, a4;
                GLARGSF_N(a1,1);
                GLARGSF_N(a2,2);
                GLARGSF_N(a3,3);
                GLARGSF_N(a4,4);
                usfp.fpa0f4 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f4)(arg[0], a1, a2, a3, a4);
            }
            GLDONE();
        case FEnum_glMapGrid2f:
            {
                float a1, a2, a4, a5;
                GLARGSF_N(a1, 1);
                GLARGSF_N(a2, 2);
                GLARGSF_N(a4, 4);
                GLARGSF_N(a5, 5);
                usfp.fpa0f2a3f5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f2a3f5)(arg[0], a1, a2, arg[3], a4, a5);
            }
            GLDONE();
        case FEnum_glMap1f:
            {
                float a1, a2;
                GLARGSF_N(a1, 1);
                GLARGSF_N(a2, 2);
                usfp.fpa0f2a4p5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f2a4p5)(arg[0], a1, a2, arg[3], arg[4], parg[1]);
            }
            GLDONE();
        case FEnum_glMap2f:
            {
                float a1, a2, a5, a6;
                GLARGSF_N(a1, 1);
                GLARGSF_N(a2, 2);
                GLARGSF_N(a5, 5);
                GLARGSF_N(a6, 6);
                usfp.fpa0f2a4f6a8p9 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f2a4f6a8p9)(arg[0], a1, a2, arg[3], arg[4], a5, a6, arg[7], arg[8], parg[1]);
            }
            GLDONE();
        case FEnum_glLightf:
        case FEnum_glMaterialf:
        case FEnum_glSamplerParameterf:
        case FEnum_glTexEnvf:
        case FEnum_glTexGenf:
        case FEnum_glTexParameterf:
            {
                float a2;
                GLARGSF_N(a2,2);
                usfp.fpa1f2 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa1f2)(arg[0], arg[1], a2);
            }
            GLDONE();
        case FEnum_glProgramEnvParameter4fARB:
        case FEnum_glProgramLocalParameter4fARB:
            {
                float a2, a3, a4, a5;
                GLARGSF_N(a2,2);
                GLARGSF_N(a3,3);
                GLARGSF_N(a4,4);
                GLARGSF_N(a5,5);
                usfp.fpa1f5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa1f5)(arg[0], arg[1], a2, a3, a4, a5);
            }
            GLDONE();
        case FEnum_glBitmap:
            {
                float a2, a3, a4, a5;
                GLARGSF_N(a2,2);
                GLARGSF_N(a3,3);
                GLARGSF_N(a4,4);
                GLARGSF_N(a5,5);
                usfp.fpa0f2p6 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0f2p6)(arg[0], arg[1], a2, a3, a4, a5, parg[2]);
            }
            GLDONE();

        /* GLFuncs with double args */
#define GLARGSD_N(a,i) \
            memcpy((char *)&a, &arg[i], sizeof(uint32_t)); \
            memcpy(((char *)&a)+4, &arg[i+1], sizeof(uint32_t))
        case FEnum_glClearDepth:
        case FEnum_glEvalCoord1d:
        case FEnum_glIndexd:
        case FEnum_glTexCoord1d:
        case FEnum_glFogCoordd:
        case FEnum_glFogCoorddEXT:
        case FEnum_glClearDepthdNV:
        case FEnum_glGlobalAlphaFactordSUN:
            {
                double a0;
                GLARGSD_N(a0,0);
                usfp.fpd0 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpd0)(a0);
            }
            GLDONE();
        case FEnum_glDepthRange:
        case FEnum_glDepthRangedNV:
        case FEnum_glDepthBoundsEXT:
        case FEnum_glDepthBoundsdNV:
        case FEnum_glEvalCoord2d:
        case FEnum_glRasterPos2d:
        case FEnum_glTexCoord2d:
        case FEnum_glVertex2d:
        case FEnum_glWindowPos2d:
        case FEnum_glWindowPos2dARB:
        case FEnum_glWindowPos2dMESA:
            {
                double a0, a1;
                GLARGSD_N(a0,0);
                GLARGSD_N(a1,2);
                usfp.fpd1 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpd1)(a0,a1);
            }
            GLDONE();
        case FEnum_glScaled:
        case FEnum_glTranslated:
        case FEnum_glColor3d:
        case FEnum_glNormal3d:
        case FEnum_glRasterPos3d:
        case FEnum_glTexCoord3d:
        case FEnum_glVertex3d:
        case FEnum_glBinormal3dEXT:
        case FEnum_glSecondaryColor3d:
        case FEnum_glSecondaryColor3dEXT:
        case FEnum_glTangent3dEXT:
        case FEnum_glWindowPos3d:
        case FEnum_glWindowPos3dARB:
        case FEnum_glWindowPos3dMESA:
            {
                double a0, a1, a2;
                GLARGSD_N(a0,0);
                GLARGSD_N(a1,2);
                GLARGSD_N(a2,4);
                usfp.fpd2 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpd2)(a0,a1,a2);
            }
            GLDONE();
        case FEnum_glRectd:
        case FEnum_glRotated:
        case FEnum_glColor4d:
        case FEnum_glRasterPos4d:
        case FEnum_glTexCoord4d:
        case FEnum_glVertex4d:
        case FEnum_glWindowPos4dMESA:
            {
                double a0, a1, a2, a3;
                GLARGSD_N(a0,0);
                GLARGSD_N(a1,2);
                GLARGSD_N(a2,4);
                GLARGSD_N(a3,6);
                usfp.fpd3 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpd3)(a0,a1,a2,a3);
            }
            GLDONE();
        case FEnum_glFrustum:
        case FEnum_glOrtho:
            {
                double a0, a1, a2, a3, a4, a5;
                GLARGSD_N(a0,0);
                GLARGSD_N(a1,2);
                GLARGSD_N(a2,4);
                GLARGSD_N(a3,6);
                GLARGSD_N(a4,8);
                GLARGSD_N(a5,10);
                usfp.fpd5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpd5)(a0,a1,a2,a3,a4,a5);
            }
            GLDONE();
        case FEnum_glMultiTexCoord1d:
        case FEnum_glMultiTexCoord1dARB:
        case FEnum_glUniform1d:
        case FEnum_glVertexAttrib1d:
        case FEnum_glVertexAttrib1dARB:
            {
                double a1;
                GLARGSD_N(a1,1);
                usfp.fpa0d1 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0d1)(arg[0], a1);
            }
            GLDONE();
        case FEnum_glMapGrid1d:
        case FEnum_glMultiTexCoord2d:
        case FEnum_glMultiTexCoord2dARB:
        case FEnum_glUniform2d:
        case FEnum_glVertexAttrib2d:
        case FEnum_glVertexAttrib2dARB:
            {
                double a1, a2;
                GLARGSD_N(a1,1);
                GLARGSD_N(a2,3);
                usfp.fpa0d2 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0d2)(arg[0], a1, a2);
            }
            GLDONE();
        case FEnum_glMultiTexCoord3d:
        case FEnum_glMultiTexCoord3dARB:
        case FEnum_glUniform3d:
        case FEnum_glVertexAttrib3d:
        case FEnum_glVertexAttrib3dARB:
            {
                double a1, a2, a3;
                GLARGSD_N(a1,1);
                GLARGSD_N(a2,3);
                GLARGSD_N(a3,5);
                usfp.fpa0d3 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0d3)(arg[0], a1, a2, a3);
            }
            GLDONE();
        case FEnum_glMultiTexCoord4d:
        case FEnum_glMultiTexCoord4dARB:
        case FEnum_glUniform4d:
        case FEnum_glVertexAttrib4d:
        case FEnum_glVertexAttrib4dARB:
            {
                double a1, a2, a3, a4;
                GLARGSD_N(a1,1);
                GLARGSD_N(a2,3);
                GLARGSD_N(a3,5);
                GLARGSD_N(a4,7);
                usfp.fpa0d4 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0d4)(arg[0], a1, a2, a3, a4);
            }
            GLDONE();
        case FEnum_glTexGend:
            {
                double a2;
                GLARGSD_N(a2,2);
                usfp.fpa1d2 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa1d2)(arg[0], arg[1], a2);
            }
            GLDONE();
        case FEnum_glProgramEnvParameter4dARB:
        case FEnum_glProgramLocalParameter4dARB:
            {
                double a2, a3, a4, a5;
                GLARGSD_N(a2,2);
                GLARGSD_N(a3,4);
                GLARGSD_N(a4,6);
                GLARGSD_N(a5,8);
                usfp.fpa1d5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa1d5)(arg[0], arg[1], a2, a3, a4, a5);
            }
            GLDONE();
        case FEnum_glMapGrid2d:
            {
                double a1, a2, a4, a5;
                GLARGSD_N(a1, 1);
                GLARGSD_N(a2, 3);
                GLARGSD_N(a4, 6);
                GLARGSD_N(a5, 8);
                usfp.fpa0d2a3d5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0d2a3d5)(arg[0], a1, a2, arg[5], a4, a5);
            }
            GLDONE();
        case FEnum_glMap1d:
            {
                double a1, a2;
                GLARGSD_N(a1, 1);
                GLARGSD_N(a2, 3);
                usfp.fpa0d2a4p5 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0d2a4p5)(arg[0], a1, a2, arg[5], arg[6], parg[3]);
            }
            GLDONE();
        case FEnum_glMap2d:
            {
                double a1, a2, a5, a6;
                GLARGSD_N(a1, 1);
                GLARGSD_N(a2, 3);
                GLARGSD_N(a5, 7);
                GLARGSD_N(a6, 9);
                usfp.fpa0d2a4d6a8p9 = tblMesaGL[FEnum].ptr;
                *ret = (*usfp.fpa0d2a4d6a8p9)(arg[0], a1, a2, arg[5], arg[6], a5, a6, arg[11], arg[12], parg[1]);
            }
        default:
            break;
    }

    /* Start - generated by hostgenfuncs */

    typedef union {
    uint32_t __stdcall (*fpra0)(void);
    uint32_t __stdcall (*fpra1)(uint32_t arg0);
    uint32_t __stdcall (*fpra2)(uint32_t arg0, uint32_t arg1);
    uint32_t __stdcall (*fpra3)(uint32_t arg0, uint32_t arg1, uint32_t arg2);
    uint32_t __stdcall (*fpra4)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3);
    uint32_t __stdcall (*fpra5)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4);
    uint32_t __stdcall (*fpra6)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5);
    uint32_t __stdcall (*fpra7)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6);
    uint32_t __stdcall (*fpra8)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7);
    uint32_t __stdcall (*fpra9)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8);
    uint32_t __stdcall (*fpra10)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9);
    uint32_t __stdcall (*fpra11)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10);
    uint32_t __stdcall (*fpra12)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11);
    uint32_t __stdcall (*fpra13)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12);
    uint32_t __stdcall (*fpra14)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13);
    uint32_t __stdcall (*fpra15)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14);
    uint32_t __stdcall (*fpra16)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15);
    uint32_t __stdcall (*fpra17)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15, uint32_t arg16);
    uint32_t __stdcall (*fpra18)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15, uint32_t arg16, uint32_t arg17);
    uint32_t __stdcall (*fpra19)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15, uint32_t arg16, uint32_t arg17, uint32_t arg18);
    uint32_t __stdcall (*fpra20)(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5, uint32_t arg6, uint32_t arg7, uint32_t arg8, uint32_t arg9, uint32_t arg10, uint32_t arg11, uint32_t arg12, uint32_t arg13, uint32_t arg14, uint32_t arg15, uint32_t arg16, uint32_t arg17, uint32_t arg18, uint32_t arg19);
    } UARG_FP;
    UARG_FP ufp;

    switch (numArgs) {
    case 0:
        ufp.fpra0 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra0))();
        break;
    case 1:
        ufp.fpra1 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra1))(arg[0]);
        break;
    case 2:
        ufp.fpra2 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra2))(arg[0], arg[1]);
        break;
    case 3:
        ufp.fpra3 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra3))(arg[0], arg[1], arg[2]);
        break;
    case 4:
        ufp.fpra4 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra4))(arg[0], arg[1], arg[2], arg[3]);
        break;
    case 5:
        ufp.fpra5 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra5))(arg[0], arg[1], arg[2], arg[3], arg[4]);
        break;
    case 6:
        ufp.fpra6 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra6))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5]);
        break;
    case 7:
        ufp.fpra7 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra7))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6]);
        break;
    case 8:
        ufp.fpra8 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra8))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7]);
        break;
    case 9:
        ufp.fpra9 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra9))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8]);
        break;
    case 10:
        ufp.fpra10 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra10))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9]);
        break;
    case 11:
        ufp.fpra11 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra11))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10]);
        break;
    case 12:
        ufp.fpra12 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra12))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11]);
        break;
    case 13:
        ufp.fpra13 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra13))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12]);
        break;
    case 14:
        ufp.fpra14 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra14))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13]);
        break;
    case 15:
        ufp.fpra15 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra15))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13], arg[14]);
        break;
    case 16:
        ufp.fpra16 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra16))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13], arg[14], arg[15]);
        break;
    case 17:
        ufp.fpra17 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra17))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13], arg[14], arg[15], arg[16]);
        break;
    case 18:
        ufp.fpra18 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra18))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13], arg[14], arg[15], arg[16], arg[17]);
        break;
    case 19:
        ufp.fpra19 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra19))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13], arg[14], arg[15], arg[16], arg[17], arg[18]);
        break;
    case 20:
        ufp.fpra20 = tblMesaGL[FEnum].ptr;
        *ret = (*(ufp.fpra20))(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5], arg[6], arg[7], arg[8], arg[9], arg[10], arg[11], arg[12], arg[13], arg[14], arg[15], arg[16], arg[17], arg[18], arg[19]);
        break;
    default:
        break;
    }

    /* End - generated by hostgenfuncs */

}

static uint16_t cfg_xYear;
static size_t cfg_xLength;
static int cfg_vertCacheMB;
static int cfg_createWnd;
static void conf_MGLOptions(void)
{
    cfg_xYear = 2003;
    cfg_xLength = 0;
    cfg_vertCacheMB = 32;
    cfg_createWnd = 0;
    FILE *fp = fopen(MESAGLCFG, "r");
    if (fp != NULL) {
        char line[32];
        int i, y, n, v, w;
        while (fgets(line, 32, fp)) {
            i = sscanf(line, "ExtensionsYear,%d", &y);
            cfg_xYear = (i == 1)? y:cfg_xYear;
            i = sscanf(line, "ExtensionsLength,%d", &n);
            cfg_xLength = (i == 1)? n:cfg_xLength;
            i = sscanf(line, "VertexCacheMB,%d", &v);
            cfg_vertCacheMB = (i == 1)? v:cfg_vertCacheMB;
            i = sscanf(line, "CreateWindow,%d", &w);
#if defined(CONFIG_WIN32) && CONFIG_WIN32
            cfg_createWnd = (i == 1)? w:cfg_createWnd;
#endif
        }
        fclose(fp);
    }
}

uint16_t GetGLExtYear(void) { return cfg_xYear; }
size_t GetGLExtLength(void) { return cfg_xLength; }
int GetVertCacheMB(void) { return cfg_vertCacheMB; }
int GetCreateWindow(void) { return cfg_createWnd; }

#if defined(CONFIG_WIN32) && CONFIG_WIN32
static HINSTANCE hDll = 0;
#endif
#if defined(CONFIG_LINUX) && CONFIG_LINUX
static void *hDll = 0;
#endif

void FiniMesaGL(void)
{
    if (hDll) {
#if defined(CONFIG_WIN32) && CONFIG_WIN32
        FreeLibrary(hDll);
#endif
#if defined(CONFIG_LINUX) && CONFIG_LINUX
        dlclose(hDll);
#endif
    }
    hDll = 0;
    for (int i = 0 ; i < FEnum_zzMGLFuncEnum_max; i++)
        tblMesaGL[i].ptr = 0;
}

void ImplMesaGLReset(void)
{
    for (int i = 0 ; i < FEnum_zzMGLFuncEnum_max; i++)
        tblMesaGL[i].impl = 0;
}

int InitMesaGL(void)
{
#if defined(CONFIG_WIN32) && CONFIG_WIN32
    const char dllname[] = "opengl32.dll";
    hDll = LoadLibrary(dllname);
#endif
#if defined(CONFIG_LINUX) && CONFIG_LINUX
    const char dllname[] = "libGL.so";
    hDll = dlopen(dllname, RTLD_NOW);
#endif
    if (!hDll) {
        return 1;
    }

    for (int i = 0; i < FEnum_zzMGLFuncEnum_max; i++) {
        char func[64];
        strncpy(func, tblMesaGL[i].sym + 1, sizeof(func));
        for (int j = 0; j < sizeof(func); j++) {
            if (func[j] == '@') {
                func[j] = 0;
                break;
            }
        }
#if defined(CONFIG_WIN32) && CONFIG_WIN32        
        tblMesaGL[i].ptr = (void *)GetProcAddress(hDll, func);
#endif
#if defined(CONFIG_LINUX) && CONFIG_LINUX
        tblMesaGL[i].ptr = (void *)dlsym(hDll, func);
#endif
    }
    SetMesaFuncPtr(hDll);
    conf_MGLOptions();
    return 0;
}

void InitMesaGLExt(void)
{
    for (int i = 0; i < FEnum_zzMGLFuncEnum_max; i++) {
        char func[64];
        if (tblMesaGL[i].ptr == NULL) {
            strncpy(func, tblMesaGL[i].sym + 1, sizeof(func));
            for (int j = 0; j < sizeof(func); j++) {
                if (func[j] == '@') {
                    func[j] = 0;
                    break;
                }
            }
            tblMesaGL[i].ptr = (void *)MesaGLGetProc(func);
        }
    }
}
