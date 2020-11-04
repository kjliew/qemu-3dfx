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

#ifndef MESAGL_IMPL_H
#define MESAGL_IMPL_H

#include <stdint.h>
#include "extensions_defs.h"
#include "mglfuncs.h"
#include "mglvarry.h"
#include "mglmapbo.h"
#include "mglcntx.h"

int GLFEnumArgsCnt(int);
int ExtFuncIsValid(char *);
int wrMapOrderPoints(uint32_t);
int wrTexSizeTexture(uint32_t, uint32_t, int);
int wrGetParamIa1p2(uint32_t, uint32_t, uint32_t);
void wrFillBufObj(uint32_t, void *, uint32_t, uint32_t);
void wrFlushBufObj(int, uint32_t, mapbufo_t *);
const char *getGLFuncStr(int);
void doMesaFunc(int, uint32_t *, uintptr_t *, uintptr_t *);
void GLExtUncapped(void);
int GetGLExtYear(void);
int GetGLExtLength(void);
int GetVertCacheMB(void);
int GetDispTimerMS(void);
int GetBufOAccelEN(void);
int GetContextMSAA(void);
int GetContextVsync(void);
int GetCreateWindow(void);
int GLFifoTrace(void);
int GLFuncTrace(void);
void FiniMesaGL(void);
void ImplMesaGLReset(void);
int InitMesaGL(void);
void InitMesaGLExt(void);

#endif //MESAGL_IMPL_H
