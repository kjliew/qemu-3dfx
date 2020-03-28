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
#include "mglcntx.h"
#include "mglvarry.h"

int GLFEnumArgsCnt(int);
int ExtFuncIsValid(char *);
int wrMapOrderPoints(uint32_t);
int wrTexTextureWxD(uint32_t, uint32_t, int);
int wrGetParamIa1p2(uint32_t, uint32_t, uint32_t);
const char *getGLFuncStr(int);
void doMesaFunc(int, uint32_t *, uintptr_t *, uintptr_t *);
uint16_t GetGLExtYear(void);
size_t GetGLExtLength(void);
int GetVertCacheMB(void);
int GetCreateWindow(void);
int GetKickFrame(void);
void FiniMesaGL(void);
int InitMesaGL(void);
void InitMesaGLExt(void);

#endif //MESAGL_IMPL_H
