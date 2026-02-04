/*
 * QEMU MESA GL Pass-Through
 *
 *  Copyright (c) ... in a Galaxy far, far away ...
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

#include <GL/glcorearb.h>
#define MESA_PFN(p,f) \
    p p_##f = (p)GLFEnumFuncPtr(FEnum_##f)
#define PFN_CALL(f) \
    p_##f
#include "crypto/hash.h"
#define ASSERT_ATTEST(x) \
    do { Error *errp = NULL; char *div; \
        struct iovec iv = { .iov_base = x, .iov_len = strnlen(x, 48) }; \
        qcrypto_hash_digestv(QCRYPTO_HASH_ALGO_SHA1, &iv, 1, &div, &errp); \
        MesaContextAttest(div, &s->mglCntxAtt); g_free(div); \
    } while(0)
#define ATTEST_IV \
    "fc35584449182ef9965c04f436f9d076046513f5", \
    "39ece786a71bbacb296403bd8a8e614e4577b6f3", \
    "73038aa5f5097e56144520afe60e479cc63ad986", \
    "92c71e59f94fd963f9de76cce14e6b659b2613d3", \
    "2b7bc2db7b4bb878ba3addf54392346f7568fb87", \
    "9c2d2de9e4c69860d0050c0bb4cdd203c8c72975", \
    NULL
typedef GLboolean (APIENTRYP PFNGLISENABLEDPROC) (GLenum cap);
typedef GLenum (APIENTRYP PFNGLGETERRORPROC) (void);
typedef const GLubyte *(APIENTRYP PFNGLGETSTRINGPROC) (GLenum name);
typedef void (APIENTRYP PFNGLBINDTEXTUREPROC) (GLenum target, GLuint texture);
typedef void (APIENTRYP PFNGLBITMAPPROC) (GLsizei width,GLsizei height,GLfloat xorig,GLfloat yorig,GLfloat xmove,GLfloat ymove,const GLubyte *bitmap);
typedef void (APIENTRYP PFNGLCOPYTEXIMAGE2DPROC) (GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
typedef void (APIENTRYP PFNGLDELETETEXTURESPROC) (GLsizei n, const GLuint *textures);
typedef void (APIENTRYP PFNGLDISABLEPROC) (GLenum cap);
typedef void (APIENTRYP PFNGLDRAWARRAYSPROC) (GLenum mode, GLint first, GLsizei count);
typedef void (APIENTRYP PFNGLENABLEPROC) (GLenum cap);
typedef void (APIENTRYP PFNGLENDLISTPROC) (void);
typedef void (APIENTRYP PFNGLGENTEXTURESPROC) (GLsizei n, GLuint *textures);
typedef void (APIENTRYP PFNGLGETINTEGERVPROC) (GLenum pname, GLint *data);
typedef void (APIENTRYP PFNGLGETMAPIVPROC) (GLenum target,GLenum query,GLint *v);
typedef void (APIENTRYP PFNGLGETTEXLEVELPARAMETERIVPROC) (GLenum target, GLint level, GLenum pname, GLint *params);
typedef void (APIENTRYP PFNGLNEWLISTPROC) (GLuint list,GLenum mode);
typedef void (APIENTRYP PFNGLPIXELSTOREIPROC) (GLenum pname, GLint param);
typedef void (APIENTRYP PFNGLTEXPARAMETERIPROC) (GLenum target, GLenum pname, GLint param);
typedef void (APIENTRYP PFNGLVIEWPORTPROC) (GLint x, GLint y, GLsizei width, GLsizei height);
