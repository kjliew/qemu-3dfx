/**
 * ICD interface for QEMU-3dfx
 *
 **/
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include <GL/gl.h>
#include "mglfuncs.h"
/***
  Few implementations notes:
  
  Mostly this is direct mapping Drv* to wgl* but here are few notes:
  0) Opengl32.dll calls driver with extEscape, code 8, data 0x1101 (DWORD) to
     determine if driver supports ICD feature. If yes, calls extEscape, code 0x1101,
     out buffer 532 bytes length. First DWORD is OpengGL version and second is
     driver version (near all real driver returns 0x1 and 0x2). If follows
     ICD name terminated with zero (on 9x is CHAR, on NT wchar, opengl32.dll
     is same for 9x and NT system version is determined in runtime.
  1) Opengl32.dll look to:
     HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\OpenGLdrivers
     for driver name is key and value as DLL name. Opengl32.dll try load this
     library by LoadLibrary.
  2) Opengl32.dll tries to get following function from DLL:
  	  DrvCopyContext
  	  DrvCreateContext
  	  DrvCreateLayerContext
  	  DrvDeleteContext
  	  DrvDescribeLayerPlane
  	  DrvDescribePixelFormat
  	  DrvGetLayerPaletteEntries
  	  DrvGetProcAddress
  	  DrvRealizeLayerPalette
  	  DrvReleaseContext
  	  DrvSetCallbackProcs
  	  DrvSetContext
  	  DrvSetLayerPaletteEntries
  	  DrvSetPixelFormat
  	  DrvShareLists
  	  DrvSwapBuffers
  	  DrvSwapLayerBuffers
  	  DrvValidateVersion
  3) If someone is missig, driver will not be used and MS software opengl opengl is used.
     If DLL fails to load, MS software opengl will be used.
     Opengl32.dll calls DrvValidateVersion, and if return FALSE, SW GL is used.
  4) Opengl32.dll call DrvSetContext, very useful is 'GetDhglrc' function because:
  5) HGLRC returned by driver functions ARE NOT same as HGLRC finaly returned by openg32.dll!
     There is internal mapping between driver HGLRC and user HGLRC. This is problem is user user
     call wglGetProcAddress (DrvGetProcAddress), but as parameters to these function user
     will puts system HGLRC and we must translate to driver HGLRC.
  6) wglCreateContextAttribsARB returns user HGLRC, there is HACK to obtain user HGLRC.
  7) Because passthrough driver use as HGLRC some magic contants, I'm using own ID series and
     using simple list to store them and translating them to driver's HGLRC.
  8) DrvSetPixelFormat has ONLY TWO parameters! (wglSetPixelFormat has 3)
  9) DescribePixelFormat returns maximum iPixelFormat number on success.
     If ppfd is NULL, it just returns maximum iPixelFormat.
 10) DrvSetContext points to table of OpenGL functions if success
 11) DrvCreateContext is not used, instead of it opengl32.dll calls
     CreateLayerContext with iLayerPlane=0
 12) Since rendering to window isn't supported, ICD refuse to load for screensavers,
     see DrvValidateVersion to remove/update this limitation.

***/

extern HINSTANCE DLLModule;

#ifdef DEBUG_ICD
static void icdlog(const char *fmt, ...)
{
	FILE *fa = fopen("C:\\icd.log", "at");
  va_list args;

	if(fa)
	{
		va_start(args, fmt);
		vfprintf(fa, fmt, args);
		va_end(args);
		fclose(fa);
	}
}
#else
#define icdlog(...)
#endif

#define PT_CALL __stdcall

/**
 * From Mesa implemenations:
 *   src/gallium/frontends/wgl/gldrv.h
 *   src/gallium/frontends/wgl/stw_context.c
 **/

#define PIXEL_FORMAT_COUNT 220 /* guessed value from NVIDIA driver */

// Number of entries expected for various versions of OpenGL
#define OPENGL_VERSION_100_ENTRIES      306
#define OPENGL_VERSION_110_ENTRIES      336

typedef struct _GLDISPATCHTABLE {
    void      (APIENTRY *glNewList                )( GLuint list, GLenum mode );
    void      (APIENTRY *glEndList                )( void );
    void      (APIENTRY *glCallList               )( GLuint list );
    void      (APIENTRY *glCallLists              )( GLsizei n, GLenum type, const GLvoid *lists );
    void      (APIENTRY *glDeleteLists            )( GLuint list, GLsizei range );
    GLuint    (APIENTRY *glGenLists               )( GLsizei range );
    void      (APIENTRY *glListBase               )( GLuint base );
    void      (APIENTRY *glBegin                  )( GLenum mode );
    void      (APIENTRY *glBitmap                 )( GLsizei width, GLsizei height, GLfloat xorig, GLfloat yorig, GLfloat xmove, GLfloat ymove, const GLubyte *bitmap );
    void      (APIENTRY *glColor3b                )( GLbyte red, GLbyte green, GLbyte blue );
    void      (APIENTRY *glColor3bv               )( const GLbyte *v );
    void      (APIENTRY *glColor3d                )( GLdouble red, GLdouble green, GLdouble blue );
    void      (APIENTRY *glColor3dv               )( const GLdouble *v );
    void      (APIENTRY *glColor3f                )( GLfloat red, GLfloat green, GLfloat blue );
    void      (APIENTRY *glColor3fv               )( const GLfloat *v );
    void      (APIENTRY *glColor3i                )( GLint red, GLint green, GLint blue );
    void      (APIENTRY *glColor3iv               )( const GLint *v );
    void      (APIENTRY *glColor3s                )( GLshort red, GLshort green, GLshort blue );
    void      (APIENTRY *glColor3sv               )( const GLshort *v );
    void      (APIENTRY *glColor3ub               )( GLubyte red, GLubyte green, GLubyte blue );
    void      (APIENTRY *glColor3ubv              )( const GLubyte *v );
    void      (APIENTRY *glColor3ui               )( GLuint red, GLuint green, GLuint blue );
    void      (APIENTRY *glColor3uiv              )( const GLuint *v );
    void      (APIENTRY *glColor3us               )( GLushort red, GLushort green, GLushort blue );
    void      (APIENTRY *glColor3usv              )( const GLushort *v );
    void      (APIENTRY *glColor4b                )( GLbyte red, GLbyte green, GLbyte blue, GLbyte alpha );
    void      (APIENTRY *glColor4bv               )( const GLbyte *v );
    void      (APIENTRY *glColor4d                )( GLdouble red, GLdouble green, GLdouble blue, GLdouble alpha );
    void      (APIENTRY *glColor4dv               )( const GLdouble *v );
    void      (APIENTRY *glColor4f                )( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha );
    void      (APIENTRY *glColor4fv               )( const GLfloat *v );
    void      (APIENTRY *glColor4i                )( GLint red, GLint green, GLint blue, GLint alpha );
    void      (APIENTRY *glColor4iv               )( const GLint *v );
    void      (APIENTRY *glColor4s                )( GLshort red, GLshort green, GLshort blue, GLshort alpha );
    void      (APIENTRY *glColor4sv               )( const GLshort *v );
    void      (APIENTRY *glColor4ub               )( GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha );
    void      (APIENTRY *glColor4ubv              )( const GLubyte *v );
    void      (APIENTRY *glColor4ui               )( GLuint red, GLuint green, GLuint blue, GLuint alpha );
    void      (APIENTRY *glColor4uiv              )( const GLuint *v );
    void      (APIENTRY *glColor4us               )( GLushort red, GLushort green, GLushort blue, GLushort alpha );
    void      (APIENTRY *glColor4usv              )( const GLushort *v );
    void      (APIENTRY *glEdgeFlag               )( GLboolean flag );
    void      (APIENTRY *glEdgeFlagv              )( const GLboolean *flag );
    void      (APIENTRY *glEnd                    )( void );
    void      (APIENTRY *glIndexd                 )( GLdouble c );
    void      (APIENTRY *glIndexdv                )( const GLdouble *c );
    void      (APIENTRY *glIndexf                 )( GLfloat c );
    void      (APIENTRY *glIndexfv                )( const GLfloat *c );
    void      (APIENTRY *glIndexi                 )( GLint c );
    void      (APIENTRY *glIndexiv                )( const GLint *c );
    void      (APIENTRY *glIndexs                 )( GLshort c );
    void      (APIENTRY *glIndexsv                )( const GLshort *c );
    void      (APIENTRY *glNormal3b               )( GLbyte nx, GLbyte ny, GLbyte nz );
    void      (APIENTRY *glNormal3bv              )( const GLbyte *v );
    void      (APIENTRY *glNormal3d               )( GLdouble nx, GLdouble ny, GLdouble nz );
    void      (APIENTRY *glNormal3dv              )( const GLdouble *v );
    void      (APIENTRY *glNormal3f               )( GLfloat nx, GLfloat ny, GLfloat nz );
    void      (APIENTRY *glNormal3fv              )( const GLfloat *v );
    void      (APIENTRY *glNormal3i               )( GLint nx, GLint ny, GLint nz );
    void      (APIENTRY *glNormal3iv              )( const GLint *v );
    void      (APIENTRY *glNormal3s               )( GLshort nx, GLshort ny, GLshort nz );
    void      (APIENTRY *glNormal3sv              )( const GLshort *v );
    void      (APIENTRY *glRasterPos2d            )( GLdouble x, GLdouble y );
    void      (APIENTRY *glRasterPos2dv           )( const GLdouble *v );
    void      (APIENTRY *glRasterPos2f            )( GLfloat x, GLfloat y );
    void      (APIENTRY *glRasterPos2fv           )( const GLfloat *v );
    void      (APIENTRY *glRasterPos2i            )( GLint x, GLint y );
    void      (APIENTRY *glRasterPos2iv           )( const GLint *v );
    void      (APIENTRY *glRasterPos2s            )( GLshort x, GLshort y );
    void      (APIENTRY *glRasterPos2sv           )( const GLshort *v );
    void      (APIENTRY *glRasterPos3d            )( GLdouble x, GLdouble y, GLdouble z );
    void      (APIENTRY *glRasterPos3dv           )( const GLdouble *v );
    void      (APIENTRY *glRasterPos3f            )( GLfloat x, GLfloat y, GLfloat z );
    void      (APIENTRY *glRasterPos3fv           )( const GLfloat *v );
    void      (APIENTRY *glRasterPos3i            )( GLint x, GLint y, GLint z );
    void      (APIENTRY *glRasterPos3iv           )( const GLint *v );
    void      (APIENTRY *glRasterPos3s            )( GLshort x, GLshort y, GLshort z );
    void      (APIENTRY *glRasterPos3sv           )( const GLshort *v );
    void      (APIENTRY *glRasterPos4d            )( GLdouble x, GLdouble y, GLdouble z, GLdouble w );
    void      (APIENTRY *glRasterPos4dv           )( const GLdouble *v );
    void      (APIENTRY *glRasterPos4f            )( GLfloat x, GLfloat y, GLfloat z, GLfloat w );
    void      (APIENTRY *glRasterPos4fv           )( const GLfloat *v );
    void      (APIENTRY *glRasterPos4i            )( GLint x, GLint y, GLint z, GLint w );
    void      (APIENTRY *glRasterPos4iv           )( const GLint *v );
    void      (APIENTRY *glRasterPos4s            )( GLshort x, GLshort y, GLshort z, GLshort w );
    void      (APIENTRY *glRasterPos4sv           )( const GLshort *v );
    void      (APIENTRY *glRectd                  )( GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2 );
    void      (APIENTRY *glRectdv                 )( const GLdouble *v1, const GLdouble *v2 );
    void      (APIENTRY *glRectf                  )( GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2 );
    void      (APIENTRY *glRectfv                 )( const GLfloat *v1, const GLfloat *v2 );
    void      (APIENTRY *glRecti                  )( GLint x1, GLint y1, GLint x2, GLint y2 );
    void      (APIENTRY *glRectiv                 )( const GLint *v1, const GLint *v2 );
    void      (APIENTRY *glRects                  )( GLshort x1, GLshort y1, GLshort x2, GLshort y2 );
    void      (APIENTRY *glRectsv                 )( const GLshort *v1, const GLshort *v2 );
    void      (APIENTRY *glTexCoord1d             )( GLdouble s );
    void      (APIENTRY *glTexCoord1dv            )( const GLdouble *v );
    void      (APIENTRY *glTexCoord1f             )( GLfloat s );
    void      (APIENTRY *glTexCoord1fv            )( const GLfloat *v );
    void      (APIENTRY *glTexCoord1i             )( GLint s );
    void      (APIENTRY *glTexCoord1iv            )( const GLint *v );
    void      (APIENTRY *glTexCoord1s             )( GLshort s );
    void      (APIENTRY *glTexCoord1sv            )( const GLshort *v );
    void      (APIENTRY *glTexCoord2d             )( GLdouble s, GLdouble t );
    void      (APIENTRY *glTexCoord2dv            )( const GLdouble *v );
    void      (APIENTRY *glTexCoord2f             )( GLfloat s, GLfloat t );
    void      (APIENTRY *glTexCoord2fv            )( const GLfloat *v );
    void      (APIENTRY *glTexCoord2i             )( GLint s, GLint t );
    void      (APIENTRY *glTexCoord2iv            )( const GLint *v );
    void      (APIENTRY *glTexCoord2s             )( GLshort s, GLshort t );
    void      (APIENTRY *glTexCoord2sv            )( const GLshort *v );
    void      (APIENTRY *glTexCoord3d             )( GLdouble s, GLdouble t, GLdouble r );
    void      (APIENTRY *glTexCoord3dv            )( const GLdouble *v );
    void      (APIENTRY *glTexCoord3f             )( GLfloat s, GLfloat t, GLfloat r );
    void      (APIENTRY *glTexCoord3fv            )( const GLfloat *v );
    void      (APIENTRY *glTexCoord3i             )( GLint s, GLint t, GLint r );
    void      (APIENTRY *glTexCoord3iv            )( const GLint *v );
    void      (APIENTRY *glTexCoord3s             )( GLshort s, GLshort t, GLshort r );
    void      (APIENTRY *glTexCoord3sv            )( const GLshort *v );
    void      (APIENTRY *glTexCoord4d             )( GLdouble s, GLdouble t, GLdouble r, GLdouble q );
    void      (APIENTRY *glTexCoord4dv            )( const GLdouble *v );
    void      (APIENTRY *glTexCoord4f             )( GLfloat s, GLfloat t, GLfloat r, GLfloat q );
    void      (APIENTRY *glTexCoord4fv            )( const GLfloat *v );
    void      (APIENTRY *glTexCoord4i             )( GLint s, GLint t, GLint r, GLint q );
    void      (APIENTRY *glTexCoord4iv            )( const GLint *v );
    void      (APIENTRY *glTexCoord4s             )( GLshort s, GLshort t, GLshort r, GLshort q );
    void      (APIENTRY *glTexCoord4sv            )( const GLshort *v );
    void      (APIENTRY *glVertex2d               )( GLdouble x, GLdouble y );
    void      (APIENTRY *glVertex2dv              )( const GLdouble *v );
    void      (APIENTRY *glVertex2f               )( GLfloat x, GLfloat y );
    void      (APIENTRY *glVertex2fv              )( const GLfloat *v );
    void      (APIENTRY *glVertex2i               )( GLint x, GLint y );
    void      (APIENTRY *glVertex2iv              )( const GLint *v );
    void      (APIENTRY *glVertex2s               )( GLshort x, GLshort y );
    void      (APIENTRY *glVertex2sv              )( const GLshort *v );
    void      (APIENTRY *glVertex3d               )( GLdouble x, GLdouble y, GLdouble z );
    void      (APIENTRY *glVertex3dv              )( const GLdouble *v );
    void      (APIENTRY *glVertex3f               )( GLfloat x, GLfloat y, GLfloat z );
    void      (APIENTRY *glVertex3fv              )( const GLfloat *v );
    void      (APIENTRY *glVertex3i               )( GLint x, GLint y, GLint z );
    void      (APIENTRY *glVertex3iv              )( const GLint *v );
    void      (APIENTRY *glVertex3s               )( GLshort x, GLshort y, GLshort z );
    void      (APIENTRY *glVertex3sv              )( const GLshort *v );
    void      (APIENTRY *glVertex4d               )( GLdouble x, GLdouble y, GLdouble z, GLdouble w );
    void      (APIENTRY *glVertex4dv              )( const GLdouble *v );
    void      (APIENTRY *glVertex4f               )( GLfloat x, GLfloat y, GLfloat z, GLfloat w );
    void      (APIENTRY *glVertex4fv              )( const GLfloat *v );
    void      (APIENTRY *glVertex4i               )( GLint x, GLint y, GLint z, GLint w );
    void      (APIENTRY *glVertex4iv              )( const GLint *v );
    void      (APIENTRY *glVertex4s               )( GLshort x, GLshort y, GLshort z, GLshort w );
    void      (APIENTRY *glVertex4sv              )( const GLshort *v );
    void      (APIENTRY *glClipPlane              )( GLenum plane, const GLdouble *equation );
    void      (APIENTRY *glColorMaterial          )( GLenum face, GLenum mode );
    void      (APIENTRY *glCullFace               )( GLenum mode );
    void      (APIENTRY *glFogf                   )( GLenum pname, GLfloat param );
    void      (APIENTRY *glFogfv                  )( GLenum pname, const GLfloat *params );
    void      (APIENTRY *glFogi                   )( GLenum pname, GLint param );
    void      (APIENTRY *glFogiv                  )( GLenum pname, const GLint *params );
    void      (APIENTRY *glFrontFace              )( GLenum mode );
    void      (APIENTRY *glHint                   )( GLenum target, GLenum mode );
    void      (APIENTRY *glLightf                 )( GLenum light, GLenum pname, GLfloat param );
    void      (APIENTRY *glLightfv                )( GLenum light, GLenum pname, const GLfloat *params );
    void      (APIENTRY *glLighti                 )( GLenum light, GLenum pname, GLint param );
    void      (APIENTRY *glLightiv                )( GLenum light, GLenum pname, const GLint *params );
    void      (APIENTRY *glLightModelf            )( GLenum pname, GLfloat param );
    void      (APIENTRY *glLightModelfv           )( GLenum pname, const GLfloat *params );
    void      (APIENTRY *glLightModeli            )( GLenum pname, GLint param );
    void      (APIENTRY *glLightModeliv           )( GLenum pname, const GLint *params );
    void      (APIENTRY *glLineStipple            )( GLint factor, GLushort pattern );
    void      (APIENTRY *glLineWidth              )( GLfloat width );
    void      (APIENTRY *glMaterialf              )( GLenum face, GLenum pname, GLfloat param );
    void      (APIENTRY *glMaterialfv             )( GLenum face, GLenum pname, const GLfloat *params );
    void      (APIENTRY *glMateriali              )( GLenum face, GLenum pname, GLint param );
    void      (APIENTRY *glMaterialiv             )( GLenum face, GLenum pname, const GLint *params );
    void      (APIENTRY *glPointSize              )( GLfloat size );
    void      (APIENTRY *glPolygonMode            )( GLenum face, GLenum mode );
    void      (APIENTRY *glPolygonStipple         )( const GLubyte *mask );
    void      (APIENTRY *glScissor                )( GLint x, GLint y, GLsizei width, GLsizei height );
    void      (APIENTRY *glShadeModel             )( GLenum mode );
    void      (APIENTRY *glTexParameterf          )( GLenum target, GLenum pname, GLfloat param );
    void      (APIENTRY *glTexParameterfv         )( GLenum target, GLenum pname, const GLfloat *params );
    void      (APIENTRY *glTexParameteri          )( GLenum target, GLenum pname, GLint param );
    void      (APIENTRY *glTexParameteriv         )( GLenum target, GLenum pname, const GLint *params );
    void      (APIENTRY *glTexImage1D             )( GLenum target, GLint level, GLint components, GLsizei width, GLint border, GLenum format, GLenum type, const GLvoid *pixels );
    void      (APIENTRY *glTexImage2D             )( GLenum target, GLint level, GLint components, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels );
    void      (APIENTRY *glTexEnvf                )( GLenum target, GLenum pname, GLfloat param );
    void      (APIENTRY *glTexEnvfv               )( GLenum target, GLenum pname, const GLfloat *params );
    void      (APIENTRY *glTexEnvi                )( GLenum target, GLenum pname, GLint param );
    void      (APIENTRY *glTexEnviv               )( GLenum target, GLenum pname, const GLint *params );
    void      (APIENTRY *glTexGend                )( GLenum coord, GLenum pname, GLdouble param );
    void      (APIENTRY *glTexGendv               )( GLenum coord, GLenum pname, const GLdouble *params );
    void      (APIENTRY *glTexGenf                )( GLenum coord, GLenum pname, GLfloat param );
    void      (APIENTRY *glTexGenfv               )( GLenum coord, GLenum pname, const GLfloat *params );
    void      (APIENTRY *glTexGeni                )( GLenum coord, GLenum pname, GLint param );
    void      (APIENTRY *glTexGeniv               )( GLenum coord, GLenum pname, const GLint *params );
    void      (APIENTRY *glFeedbackBuffer         )( GLsizei size, GLenum type, GLfloat *buffer );
    void      (APIENTRY *glSelectBuffer           )( GLsizei size, GLuint *buffer );
    GLint     (APIENTRY *glRenderMode             )( GLenum mode );
    void      (APIENTRY *glInitNames              )( void );
    void      (APIENTRY *glLoadName               )( GLuint name );
    void      (APIENTRY *glPassThrough            )( GLfloat token );
    void      (APIENTRY *glPopName                )( void );
    void      (APIENTRY *glPushName               )( GLuint name );
    void      (APIENTRY *glDrawBuffer             )( GLenum mode );
    void      (APIENTRY *glClear                  )( GLbitfield mask );
    void      (APIENTRY *glClearAccum             )( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha );
    void      (APIENTRY *glClearIndex             )( GLfloat c );
    void      (APIENTRY *glClearColor             )( GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha );
    void      (APIENTRY *glClearStencil           )( GLint s );
    void      (APIENTRY *glClearDepth             )( GLclampd depth );
    void      (APIENTRY *glStencilMask            )( GLuint mask );
    void      (APIENTRY *glColorMask              )( GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha );
    void      (APIENTRY *glDepthMask              )( GLboolean flag );
    void      (APIENTRY *glIndexMask              )( GLuint mask );
    void      (APIENTRY *glAccum                  )( GLenum op, GLfloat value );
    void      (APIENTRY *glDisable                )( GLenum cap );
    void      (APIENTRY *glEnable                 )( GLenum cap );
    void      (APIENTRY *glFinish                 )( void );
    void      (APIENTRY *glFlush                  )( void );
    void      (APIENTRY *glPopAttrib              )( void );
    void      (APIENTRY *glPushAttrib             )( GLbitfield mask );
    void      (APIENTRY *glMap1d                  )( GLenum target, GLdouble u1, GLdouble u2, GLint stride, GLint order, const GLdouble *points );
    void      (APIENTRY *glMap1f                  )( GLenum target, GLfloat u1, GLfloat u2, GLint stride, GLint order, const GLfloat *points );
    void      (APIENTRY *glMap2d                  )( GLenum target, GLdouble u1, GLdouble u2, GLint ustride, GLint uorder, GLdouble v1, GLdouble v2, GLint vstride, GLint vorder, const GLdouble *points );
    void      (APIENTRY *glMap2f                  )( GLenum target, GLfloat u1, GLfloat u2, GLint ustride, GLint uorder, GLfloat v1, GLfloat v2, GLint vstride, GLint vorder, const GLfloat *points );
    void      (APIENTRY *glMapGrid1d              )( GLint un, GLdouble u1, GLdouble u2 );
    void      (APIENTRY *glMapGrid1f              )( GLint un, GLfloat u1, GLfloat u2 );
    void      (APIENTRY *glMapGrid2d              )( GLint un, GLdouble u1, GLdouble u2, GLint vn, GLdouble v1, GLdouble v2 );
    void      (APIENTRY *glMapGrid2f              )( GLint un, GLfloat u1, GLfloat u2, GLint vn, GLfloat v1, GLfloat v2 );
    void      (APIENTRY *glEvalCoord1d            )( GLdouble u );
    void      (APIENTRY *glEvalCoord1dv           )( const GLdouble *u );
    void      (APIENTRY *glEvalCoord1f            )( GLfloat u );
    void      (APIENTRY *glEvalCoord1fv           )( const GLfloat *u );
    void      (APIENTRY *glEvalCoord2d            )( GLdouble u, GLdouble v );
    void      (APIENTRY *glEvalCoord2dv           )( const GLdouble *u );
    void      (APIENTRY *glEvalCoord2f            )( GLfloat u, GLfloat v );
    void      (APIENTRY *glEvalCoord2fv           )( const GLfloat *u );
    void      (APIENTRY *glEvalMesh1              )( GLenum mode, GLint i1, GLint i2 );
    void      (APIENTRY *glEvalPoint1             )( GLint i );
    void      (APIENTRY *glEvalMesh2              )( GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2 );
    void      (APIENTRY *glEvalPoint2             )( GLint i, GLint j );
    void      (APIENTRY *glAlphaFunc              )( GLenum func, GLclampf ref );
    void      (APIENTRY *glBlendFunc              )( GLenum sfactor, GLenum dfactor );
    void      (APIENTRY *glLogicOp                )( GLenum opcode );
    void      (APIENTRY *glStencilFunc            )( GLenum func, GLint ref, GLuint mask );
    void      (APIENTRY *glStencilOp              )( GLenum fail, GLenum zfail, GLenum zpass );
    void      (APIENTRY *glDepthFunc              )( GLenum func );
    void      (APIENTRY *glPixelZoom              )( GLfloat xfactor, GLfloat yfactor );
    void      (APIENTRY *glPixelTransferf         )( GLenum pname, GLfloat param );
    void      (APIENTRY *glPixelTransferi         )( GLenum pname, GLint param );
    void      (APIENTRY *glPixelStoref            )( GLenum pname, GLfloat param );
    void      (APIENTRY *glPixelStorei            )( GLenum pname, GLint param );
    void      (APIENTRY *glPixelMapfv             )( GLenum map, GLint mapsize, const GLfloat *values );
    void      (APIENTRY *glPixelMapuiv            )( GLenum map, GLint mapsize, const GLuint *values );
    void      (APIENTRY *glPixelMapusv            )( GLenum map, GLint mapsize, const GLushort *values );
    void      (APIENTRY *glReadBuffer             )( GLenum mode );
    void      (APIENTRY *glCopyPixels             )( GLint x, GLint y, GLsizei width, GLsizei height, GLenum type );
    void      (APIENTRY *glReadPixels             )( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels );
    void      (APIENTRY *glDrawPixels             )( GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels );
    void      (APIENTRY *glGetBooleanv            )( GLenum pname, GLboolean *params );
    void      (APIENTRY *glGetClipPlane           )( GLenum plane, GLdouble *equation );
    void      (APIENTRY *glGetDoublev             )( GLenum pname, GLdouble *params );
    GLenum    (APIENTRY *glGetError               )( void );
    void      (APIENTRY *glGetFloatv              )( GLenum pname, GLfloat *params );
    void      (APIENTRY *glGetIntegerv            )( GLenum pname, GLint *params );
    void      (APIENTRY *glGetLightfv             )( GLenum light, GLenum pname, GLfloat *params );
    void      (APIENTRY *glGetLightiv             )( GLenum light, GLenum pname, GLint *params );
    void      (APIENTRY *glGetMapdv               )( GLenum target, GLenum query, GLdouble *v );
    void      (APIENTRY *glGetMapfv               )( GLenum target, GLenum query, GLfloat *v );
    void      (APIENTRY *glGetMapiv               )( GLenum target, GLenum query, GLint *v );
    void      (APIENTRY *glGetMaterialfv          )( GLenum face, GLenum pname, GLfloat *params );
    void      (APIENTRY *glGetMaterialiv          )( GLenum face, GLenum pname, GLint *params );
    void      (APIENTRY *glGetPixelMapfv          )( GLenum map, GLfloat *values );
    void      (APIENTRY *glGetPixelMapuiv         )( GLenum map, GLuint *values );
    void      (APIENTRY *glGetPixelMapusv         )( GLenum map, GLushort *values );
    void      (APIENTRY *glGetPolygonStipple      )( GLubyte *mask );
    const GLubyte * (APIENTRY *glGetString        )( GLenum name );
    void      (APIENTRY *glGetTexEnvfv            )( GLenum target, GLenum pname, GLfloat *params );
    void      (APIENTRY *glGetTexEnviv            )( GLenum target, GLenum pname, GLint *params );
    void      (APIENTRY *glGetTexGendv            )( GLenum coord, GLenum pname, GLdouble *params );
    void      (APIENTRY *glGetTexGenfv            )( GLenum coord, GLenum pname, GLfloat *params );
    void      (APIENTRY *glGetTexGeniv            )( GLenum coord, GLenum pname, GLint *params );
    void      (APIENTRY *glGetTexImage            )( GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels );
    void      (APIENTRY *glGetTexParameterfv      )( GLenum target, GLenum pname, GLfloat *params );
    void      (APIENTRY *glGetTexParameteriv      )( GLenum target, GLenum pname, GLint *params );
    void      (APIENTRY *glGetTexLevelParameterfv )( GLenum target, GLint level, GLenum pname, GLfloat *params );
    void      (APIENTRY *glGetTexLevelParameteriv )( GLenum target, GLint level, GLenum pname, GLint *params );
    GLboolean (APIENTRY *glIsEnabled              )( GLenum cap );
    GLboolean (APIENTRY *glIsList                 )( GLuint list );
    void      (APIENTRY *glDepthRange             )( GLclampd zNear, GLclampd zFar );
    void      (APIENTRY *glFrustum                )( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar );
    void      (APIENTRY *glLoadIdentity           )( void );
    void      (APIENTRY *glLoadMatrixf            )( const GLfloat *m );
    void      (APIENTRY *glLoadMatrixd            )( const GLdouble *m );
    void      (APIENTRY *glMatrixMode             )( GLenum mode );
    void      (APIENTRY *glMultMatrixf            )( const GLfloat *m );
    void      (APIENTRY *glMultMatrixd            )( const GLdouble *m );
    void      (APIENTRY *glOrtho                  )( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar );
    void      (APIENTRY *glPopMatrix              )( void );
    void      (APIENTRY *glPushMatrix             )( void );
    void      (APIENTRY *glRotated                )( GLdouble angle, GLdouble x, GLdouble y, GLdouble z );
    void      (APIENTRY *glRotatef                )( GLfloat angle, GLfloat x, GLfloat y, GLfloat z );
    void      (APIENTRY *glScaled                 )( GLdouble x, GLdouble y, GLdouble z );
    void      (APIENTRY *glScalef                 )( GLfloat x, GLfloat y, GLfloat z );
    void      (APIENTRY *glTranslated             )( GLdouble x, GLdouble y, GLdouble z );
    void      (APIENTRY *glTranslatef             )( GLfloat x, GLfloat y, GLfloat z );
    void      (APIENTRY *glViewport               )( GLint x, GLint y, GLsizei width, GLsizei height );
    // OpenGL version 1.0 entries end here

    // OpenGL version 1.1 entries begin here
    void      (APIENTRY *glArrayElement           )(GLint i);
    void      (APIENTRY *glBindTexture            )(GLenum target, GLuint texture);
    void      (APIENTRY *glColorPointer           )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
    void      (APIENTRY *glDisableClientState     )(GLenum array);
    void      (APIENTRY *glDrawArrays             )(GLenum mode, GLint first, GLsizei count);
    void      (APIENTRY *glDrawElements           )(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices);
    void      (APIENTRY *glEdgeFlagPointer        )(GLsizei stride, const GLvoid *pointer);
    void      (APIENTRY *glEnableClientState      )(GLenum array);
    void      (APIENTRY *glIndexPointer           )(GLenum type, GLsizei stride, const GLvoid *pointer);
    void      (APIENTRY *glIndexub                )(GLubyte c);
    void      (APIENTRY *glIndexubv               )(const GLubyte *c);
    void      (APIENTRY *glInterleavedArrays      )(GLenum format, GLsizei stride, const GLvoid *pointer);
    void      (APIENTRY *glNormalPointer          )(GLenum type, GLsizei stride, const GLvoid *pointer);
    void      (APIENTRY *glPolygonOffset          )(GLfloat factor, GLfloat units);
    void      (APIENTRY *glTexCoordPointer        )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
    void      (APIENTRY *glVertexPointer          )(GLint size, GLenum type, GLsizei stride, const GLvoid *pointer);
    GLboolean (APIENTRY *glAreTexturesResident    )(GLsizei n, const GLuint *textures, GLboolean *residences);
    void      (APIENTRY *glCopyTexImage1D         )(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width, GLint border);
    void      (APIENTRY *glCopyTexImage2D         )(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border);
    void      (APIENTRY *glCopyTexSubImage1D      )(GLenum target, GLint level, GLint xoffset, GLint x, GLint y, GLsizei width);
    void      (APIENTRY *glCopyTexSubImage2D      )(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height);
    void      (APIENTRY *glDeleteTextures         )(GLsizei n, const GLuint *textures);
    void      (APIENTRY *glGenTextures            )(GLsizei n, GLuint *textures);
    void      (APIENTRY *glGetPointerv            )(GLenum pname, GLvoid* *params);
    GLboolean (APIENTRY *glIsTexture              )(GLuint texture);
    void      (APIENTRY *glPrioritizeTextures     )(GLsizei n, const GLuint *textures, const GLclampf *priorities);
    void      (APIENTRY *glTexSubImage1D          )(GLenum target, GLint level, GLint xoffset, GLsizei width, GLenum format, GLenum type, const GLvoid *pixels);
    void      (APIENTRY *glTexSubImage2D          )(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
    void      (APIENTRY *glPopClientAttrib        )(void);
    void      (APIENTRY *glPushClientAttrib       )(GLbitfield mask);
} GLDISPATCHTABLE, *PGLDISPATCHTABLE;

typedef struct _GLCLTPROCTABLE {
	int cEntries;// Number of function entries in table
	GLDISPATCHTABLE glDispatchTable;    // OpenGL function dispatch table
} GLCLTPROCTABLE, *PGLCLTPROCTABLE;

/**
 * Although WGL allows different dispatch entrypoints per context
 */
static const GLCLTPROCTABLE cpt =
{
   OPENGL_VERSION_110_ENTRIES,
   {
      &glNewList,
      &glEndList,
      &glCallList,
      &glCallLists,
      &glDeleteLists,
      &glGenLists,
      &glListBase,
      &glBegin,
      &glBitmap,
      &glColor3b,
      &glColor3bv,
      &glColor3d,
      &glColor3dv,
      &glColor3f,
      &glColor3fv,
      &glColor3i,
      &glColor3iv,
      &glColor3s,
      &glColor3sv,
      &glColor3ub,
      &glColor3ubv,
      &glColor3ui,
      &glColor3uiv,
      &glColor3us,
      &glColor3usv,
      &glColor4b,
      &glColor4bv,
      &glColor4d,
      &glColor4dv,
      &glColor4f,
      &glColor4fv,
      &glColor4i,
      &glColor4iv,
      &glColor4s,
      &glColor4sv,
      &glColor4ub,
      &glColor4ubv,
      &glColor4ui,
      &glColor4uiv,
      &glColor4us,
      &glColor4usv,
      &glEdgeFlag,
      &glEdgeFlagv,
      &glEnd,
      &glIndexd,
      &glIndexdv,
      &glIndexf,
      &glIndexfv,
      &glIndexi,
      &glIndexiv,
      &glIndexs,
      &glIndexsv,
      &glNormal3b,
      &glNormal3bv,
      &glNormal3d,
      &glNormal3dv,
      &glNormal3f,
      &glNormal3fv,
      &glNormal3i,
      &glNormal3iv,
      &glNormal3s,
      &glNormal3sv,
      &glRasterPos2d,
      &glRasterPos2dv,
      &glRasterPos2f,
      &glRasterPos2fv,
      &glRasterPos2i,
      &glRasterPos2iv,
      &glRasterPos2s,
      &glRasterPos2sv,
      &glRasterPos3d,
      &glRasterPos3dv,
      &glRasterPos3f,
      &glRasterPos3fv,
      &glRasterPos3i,
      &glRasterPos3iv,
      &glRasterPos3s,
      &glRasterPos3sv,
      &glRasterPos4d,
      &glRasterPos4dv,
      &glRasterPos4f,
      &glRasterPos4fv,
      &glRasterPos4i,
      &glRasterPos4iv,
      &glRasterPos4s,
      &glRasterPos4sv,
      &glRectd,
      &glRectdv,
      &glRectf,
      &glRectfv,
      &glRecti,
      &glRectiv,
      &glRects,
      &glRectsv,
      &glTexCoord1d,
      &glTexCoord1dv,
      &glTexCoord1f,
      &glTexCoord1fv,
      &glTexCoord1i,
      &glTexCoord1iv,
      &glTexCoord1s,
      &glTexCoord1sv,
      &glTexCoord2d,
      &glTexCoord2dv,
      &glTexCoord2f,
      &glTexCoord2fv,
      &glTexCoord2i,
      &glTexCoord2iv,
      &glTexCoord2s,
      &glTexCoord2sv,
      &glTexCoord3d,
      &glTexCoord3dv,
      &glTexCoord3f,
      &glTexCoord3fv,
      &glTexCoord3i,
      &glTexCoord3iv,
      &glTexCoord3s,
      &glTexCoord3sv,
      &glTexCoord4d,
      &glTexCoord4dv,
      &glTexCoord4f,
      &glTexCoord4fv,
      &glTexCoord4i,
      &glTexCoord4iv,
      &glTexCoord4s,
      &glTexCoord4sv,
      &glVertex2d,
      &glVertex2dv,
      &glVertex2f,
      &glVertex2fv,
      &glVertex2i,
      &glVertex2iv,
      &glVertex2s,
      &glVertex2sv,
      &glVertex3d,
      &glVertex3dv,
      &glVertex3f,
      &glVertex3fv,
      &glVertex3i,
      &glVertex3iv,
      &glVertex3s,
      &glVertex3sv,
      &glVertex4d,
      &glVertex4dv,
      &glVertex4f,
      &glVertex4fv,
      &glVertex4i,
      &glVertex4iv,
      &glVertex4s,
      &glVertex4sv,
      &glClipPlane,
      &glColorMaterial,
      &glCullFace,
      &glFogf,
      &glFogfv,
      &glFogi,
      &glFogiv,
      &glFrontFace,
      &glHint,
      &glLightf,
      &glLightfv,
      &glLighti,
      &glLightiv,
      &glLightModelf,
      &glLightModelfv,
      &glLightModeli,
      &glLightModeliv,
      &glLineStipple,
      &glLineWidth,
      &glMaterialf,
      &glMaterialfv,
      &glMateriali,
      &glMaterialiv,
      &glPointSize,
      &glPolygonMode,
      &glPolygonStipple,
      &glScissor,
      &glShadeModel,
      &glTexParameterf,
      &glTexParameterfv,
      &glTexParameteri,
      &glTexParameteriv,
      &glTexImage1D,
      &glTexImage2D,
      &glTexEnvf,
      &glTexEnvfv,
      &glTexEnvi,
      &glTexEnviv,
      &glTexGend,
      &glTexGendv,
      &glTexGenf,
      &glTexGenfv,
      &glTexGeni,
      &glTexGeniv,
      &glFeedbackBuffer,
      &glSelectBuffer,
      &glRenderMode,
      &glInitNames,
      &glLoadName,
      &glPassThrough,
      &glPopName,
      &glPushName,
      &glDrawBuffer,
      &glClear,
      &glClearAccum,
      &glClearIndex,
      &glClearColor,
      &glClearStencil,
      &glClearDepth,
      &glStencilMask,
      &glColorMask,
      &glDepthMask,
      &glIndexMask,
      &glAccum,
      &glDisable,
      &glEnable,
      &glFinish,
      &glFlush,
      &glPopAttrib,
      &glPushAttrib,
      &glMap1d,
      &glMap1f,
      &glMap2d,
      &glMap2f,
      &glMapGrid1d,
      &glMapGrid1f,
      &glMapGrid2d,
      &glMapGrid2f,
      &glEvalCoord1d,
      &glEvalCoord1dv,
      &glEvalCoord1f,
      &glEvalCoord1fv,
      &glEvalCoord2d,
      &glEvalCoord2dv,
      &glEvalCoord2f,
      &glEvalCoord2fv,
      &glEvalMesh1,
      &glEvalPoint1,
      &glEvalMesh2,
      &glEvalPoint2,
      &glAlphaFunc,
      &glBlendFunc,
      &glLogicOp,
      &glStencilFunc,
      &glStencilOp,
      &glDepthFunc,
      &glPixelZoom,
      &glPixelTransferf,
      &glPixelTransferi,
      &glPixelStoref,
      &glPixelStorei,
      &glPixelMapfv,
      &glPixelMapuiv,
      &glPixelMapusv,
      &glReadBuffer,
      &glCopyPixels,
      &glReadPixels,
      &glDrawPixels,
      &glGetBooleanv,
      &glGetClipPlane,
      &glGetDoublev,
      &glGetError,
      &glGetFloatv,
      &glGetIntegerv,
      &glGetLightfv,
      &glGetLightiv,
      &glGetMapdv,
      &glGetMapfv,
      &glGetMapiv,
      &glGetMaterialfv,
      &glGetMaterialiv,
      &glGetPixelMapfv,
      &glGetPixelMapuiv,
      &glGetPixelMapusv,
      &glGetPolygonStipple,
      &glGetString,
      &glGetTexEnvfv,
      &glGetTexEnviv,
      &glGetTexGendv,
      &glGetTexGenfv,
      &glGetTexGeniv,
      &glGetTexImage,
      &glGetTexParameterfv,
      &glGetTexParameteriv,
      &glGetTexLevelParameterfv,
      &glGetTexLevelParameteriv,
      &glIsEnabled,
      &glIsList,
      &glDepthRange,
      &glFrustum,
      &glLoadIdentity,
      &glLoadMatrixf,
      &glLoadMatrixd,
      &glMatrixMode,
      &glMultMatrixf,
      &glMultMatrixd,
      &glOrtho,
      &glPopMatrix,
      &glPushMatrix,
      &glRotated,
      &glRotatef,
      &glScaled,
      &glScalef,
      &glTranslated,
      &glTranslatef,
      &glViewport,
      &glArrayElement,
      &glBindTexture,
      &glColorPointer,
      &glDisableClientState,
      &glDrawArrays,
      &glDrawElements,
      &glEdgeFlagPointer,
      &glEnableClientState,
      &glIndexPointer,
      &glIndexub,
      &glIndexubv,
      &glInterleavedArrays,
      &glNormalPointer,
      &glPolygonOffset,
      &glTexCoordPointer,
      &glVertexPointer,
      &glAreTexturesResident,
      &glCopyTexImage1D,
      &glCopyTexImage2D,
      &glCopyTexSubImage1D,
      &glCopyTexSubImage2D,
      &glDeleteTextures,
      &glGenTextures,
      &glGetPointerv,
      &glIsTexture,
      &glPrioritizeTextures,
      &glTexSubImage1D,
      &glTexSubImage2D,
      &glPopClientAttrib,
      &glPushClientAttrib
   }
};

typedef VOID   (APIENTRY *PFN_SETCURRENTVALUE)(VOID *pv);
typedef VOID  *(APIENTRY *PFN_GETCURRENTVALUE)(VOID);
typedef HGLRC (APIENTRY *PFN_GETDHGLRC)(HGLRC hrc);

struct WGLCALLBACKS
{
    PFN_SETCURRENTVALUE pfnSetCurrentValue;
    PFN_GETCURRENTVALUE pfnGetCurrentValue;
    PFN_GETDHGLRC pfnGetDhglrc;
    PROC pfnUnused;
};

static struct WGLCALLBACKS wglCallbacks = {NULL, NULL, NULL, NULL};

/**
 * List to store hglrc conversions
 **/
typedef struct ctx_list {
//	GLCLTPROCTABLE cpt; // use onw table for every context? (Mesa doesn't do it)
	HGLRC dhglrc; // own ID auto
	HGLRC hwrc;   // hwrc from driver
	struct ctx_list *next;
} ctx_list_t;

static ctx_list_t *ctx_list_first = NULL;
static volatile uint32_t dhglrc_next = 0x0000A001;
static HGLRC dhglrc_active = 0; /* active dhglrc context  */

static ctx_list_t *ctx_list_lookup(HGLRC dhglrc)
{
	ctx_list_t *ptr = ctx_list_first;
	
	while(ptr != NULL)
	{
		if(ptr->dhglrc == dhglrc)
		{
			return ptr;
		}
		
		ptr = ptr->next;
	}
	
	return NULL;
}

static size_t ctx_list_count_hwrc(HGLRC hwrc)
{
	ctx_list_t *ptr = ctx_list_first;
	size_t cnt = 0;
	
	while(ptr != NULL)
	{
		if(ptr->hwrc == hwrc)
		{
			cnt++;
		}
		
		ptr = ptr->next;
	}
	
	return cnt;
}

static ctx_list_t *ctx_list_create(HGLRC hwrc)
{
	ctx_list_t *item = HeapAlloc(GetProcessHeap(), 0, sizeof(ctx_list_t));
	item->dhglrc = (HGLRC)dhglrc_next++;
	item->hwrc = hwrc;
	
	item->next = ctx_list_first;
	ctx_list_first = item;
	
	return item;
}

static BOOL ctx_list_destroy(HGLRC dhglrc)
{
	ctx_list_t *ptr = ctx_list_first;
	ctx_list_t *prev = NULL;
	
	while(ptr != NULL)
	{
		if(ptr->dhglrc == dhglrc)
		{
			if(prev != NULL)
			{
				prev->next = ptr->next;
			}
			else
			{
				ctx_list_first = ptr->next;
			}
			
			HeapFree(GetProcessHeap(), 0, ptr);
			return TRUE;
		}
		
		prev = ptr;
		ptr = ptr->next;
	}
	
	return FALSE;
}

/**
 * Prototypes from wrapgl32.c
 *
 **/
int WINAPI wglDescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes, LPPIXELFORMATDESCRIPTOR ppfd);
BOOL WINAPI wglSetPixelFormat(HDC hdc, int format, const PIXELFORMATDESCRIPTOR *ppfd);
int WINAPI wglDescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes, LPPIXELFORMATDESCRIPTOR ppfd);
uint32_t PT_CALL mglCreateContext(uint32_t arg0);
uint32_t PT_CALL mglMakeCurrent(uint32_t arg0, uint32_t arg1);
uint32_t PT_CALL mglDeleteContext(uint32_t arg0);
uint32_t PT_CALL mglGetCurrentContext(void);
uint32_t PT_CALL mglCopyContext(uint32_t arg0, uint32_t arg1, uint32_t arg2);

HGLRC WINAPI private_wglCreateContextAttribsARB(HDC hDC, HGLRC hShareContext, const int *attribList);
uint32_t WINAPI private_wglMakeContextCurrentARB(uint32_t arg0, uint32_t arg1, uint32_t arg2);

/**
 * Driver functions
 *
 **/
BOOL WINAPI mgdValidateVersion(ULONG ulVersion)
{
	(void)ulVersion;
	char progname[MAX_PATH];
	
	/* QEMU-3Dfx not supporting rendering to window,
	   so small hack to NOT use HW rendering for all screensavers */
	if(GetModuleFileName(NULL, progname, MAX_PATH))
	{
		size_t len = strlen(progname);
		if(len >= 4)
		{
			if(stricmp(progname+len-4, ".scr") == 0)
			{
				return FALSE;
			}
		}
	}
	
	return TRUE;
}

void WINAPI mgdSetCallbackProcs(INT nProcs, PROC *pProcs)
{
	icdlog("ENTRY: mgdSetCallbackProcs\n");
	
	size_t size = nProcs * sizeof(PROC);
	
	if(sizeof(wglCallbacks) < size)
	{
		size = sizeof(wglCallbacks);
	}

	memcpy(&wglCallbacks, pProcs, size);
}

BOOL WINAPI mgdReleaseContext(HGLRC dhglrc)
{
	icdlog("ENTRY: mgdReleaseContext(%p)\n", dhglrc);
	
	ctx_list_t *item = ctx_list_lookup(dhglrc);
	if(item)
	{
		icdlog("  %p -> %p\n", dhglrc, item->hwrc);
		//if(item->hwrc == (HGLRC)mglGetCurrentContext())
		if(dhglrc_active == dhglrc)
		{
			mglMakeCurrent(0, 0);
			dhglrc_active = 0;
		}
		return TRUE;
	}
	
	return FALSE;
}

PGLCLTPROCTABLE *WINAPI mgdSetContext(HDC hdc, HGLRC dhglrc, void *pfnSetProcTable)
{
	icdlog("ENTRY: mgdSetContext(%p, %p, %p)\n", hdc, dhglrc, pfnSetProcTable);
	
	ctx_list_t *item = ctx_list_lookup(dhglrc);
	BOOL result = FALSE;
	
	if(item == NULL)
	{
		mglMakeCurrent((uint32_t)hdc, 0);
		dhglrc_active = 0;
		result = TRUE;
	}
	else
	{
		result = mglMakeCurrent((uint32_t)hdc, (uint32_t)item->hwrc);
		if(result)
		{
			dhglrc_active = dhglrc;
		}
	}
	
	if(result)
	{
		return (PGLCLTPROCTABLE*)&cpt;
	}
	
	return NULL;
}

BOOL WINAPI mgdSetPixelFormat(HDC hdc, LONG iPixelFormat)
{
	icdlog("ENTRY: mgdSetPixelFormat\n");
	
  return wglSetPixelFormat(hdc, iPixelFormat, NULL) == 0 ? FALSE : TRUE;;
}

HGLRC WINAPI mgdCreateContext(HDC hdc)
{
	icdlog("ENTRY: mgdCreateContext\n");
	
	HGLRC hwrc = (HGLRC)mglCreateContext((uint32_t)hdc);
	if(hwrc)
	{
		ctx_list_t *item = ctx_list_create(hwrc);
		if(item)
		{
			return item->dhglrc;
		}
	}
	
	icdlog("  mglCreateContext failure\n");
	
	return NULL;
}

HGLRC WINAPI mgdCreateLayerContext(HDC hdc, int iLayerPlane)
{
	icdlog("ENTRY: mgdCreateLayerContext(%p, %d)\n", hdc, iLayerPlane);
	
	if(iLayerPlane == 0)
	{
		return mgdCreateContext(hdc);
	}
	/*
	  if we need empty hgtrc, we call CreateLayerContext
	  with concrete nonsence iLayerPlane
	*/
	else if(iLayerPlane == 255)
	{
		ctx_list_t *item = ctx_list_create(0);
		if(item)
		{
			return item->dhglrc;
		}
	}
	
	SetLastError(ERROR_INVALID_PARAMETER);
	return NULL;
}

BOOL WINAPI mgdDeleteContext(HGLRC dhglrc)
{
	icdlog("ENTRY: mgdDeleteContext\n");
	
	ctx_list_t *item;
	
	item = ctx_list_lookup(dhglrc);
	if(item)
	{
		if(dhglrc_active == dhglrc)
		{
			mglMakeCurrent(0, 0);
			dhglrc_active = 0;
		}
		
		if(ctx_list_count_hwrc(item->hwrc) == 1)
		{
			/* delete ctx if we haven't anyone with same HW id */
			mglDeleteContext((uint32_t)item->hwrc);
		}
		
		ctx_list_destroy(dhglrc);
		
		return TRUE;
	}
	return FALSE;
}

int WINAPI mgdDescribePixelFormat(HDC hdc, int iPixelFormat, UINT nBytes, LPPIXELFORMATDESCRIPTOR ppfd)
{
	icdlog("ENTRY: mgdDescribePixelFormat(%p, %d, %u, %p)\n", hdc, iPixelFormat, nBytes, ppfd);
	
	if(ppfd == NULL)
	{
		return PIXEL_FORMAT_COUNT;
	}
	
	if(wglDescribePixelFormat(hdc, iPixelFormat, nBytes, ppfd))
	{
		return PIXEL_FORMAT_COUNT;
	}
	
	return 0;
}

/*
 * For wglCreateContextAttribsARB we need alloc system HGLRC,
 * so we calls dynamicaly opengl32.dll, call wglCreateLayerContext(hDC, 255)
 * and remap result to wglCreateContextAttribsARB return.
 */
typedef HGLRC (WINAPI *wglCreateLayerContext_t)(HDC hdc, int iLayer);
static HGLRC systemAllocateHGLRC(HDC hdc)
{
	HMODULE libgl32 = GetModuleHandleA("opengl32.dll");
	if(libgl32 != NULL)
	{
		if(libgl32 == DLLModule)
		{
			/* this library IS opengl32.dll, prevent loops */
			return NULL;
		}
		
		wglCreateLayerContext_t ctxProc = (wglCreateLayerContext_t)GetProcAddress(libgl32, "wglCreateLayerContext");
		
		if(ctxProc)
		{
			return ctxProc(hdc, 255);
		}
	}
	
	return NULL;
}

HGLRC WINAPI wglCreateContextAttribsARB(HDC hDC, HGLRC hShareContext, const int *attribList)
{
	icdlog("ENTRY: wglCreateContextAttribsARB(%p, %p, %p)\n", hDC, hShareContext, attribList);
	
	HGLRC hwrc; // driver context
	HGLRC hdcrc;  // system context
	HGLRC dhdcrc_replacement; // extra context to replace;
	
	HGLRC shc_hwrc = NULL;
	ctx_list_t *item;
	
	if(wglCallbacks.pfnGetDhglrc == NULL) /* library is in place replacement */
	{
		return private_wglCreateContextAttribsARB(hDC, hShareContext, attribList);
	}
	
	/* convert ShareContext ID */
	if(hShareContext != NULL)
	{
		HGLRC dhShareContext = wglCallbacks.pfnGetDhglrc(hShareContext);
		
		if(dhShareContext)
		{
			ctx_list_t *item_shc = ctx_list_lookup(dhShareContext);
			if(item_shc)
			{
				shc_hwrc = item_shc->hwrc;
			}
		}
	}
	
	hwrc = private_wglCreateContextAttribsARB(hDC, shc_hwrc, attribList);
	
	if(hwrc == NULL)
	{
		icdlog("  private_wglCreateContextAttribsARB FAIL\n");
		return NULL;
	}
	
	hdcrc = systemAllocateHGLRC(hDC);
	if(hdcrc)
	{
		dhdcrc_replacement = wglCallbacks.pfnGetDhglrc(hdcrc);
		item = ctx_list_lookup(dhdcrc_replacement);
		if(item)
		{
			item->hwrc = hwrc;
			icdlog("  maped hShareContext: %p => %p\n", hShareContext, shc_hwrc);
			icdlog("  maped hdcrc: %p => %p => %p\n", hdcrc, dhdcrc_replacement, hwrc);
			return hdcrc;
		}
		else
		{
			icdlog("  ctx_list_lookup: FAIL\n");
		}
	}
	else
	{
		icdlog("  systemCreateContext: FAIL - %lu\n", GetLastError());
	}
	
	return NULL;
}

BOOL WINAPI wglMakeContextCurrentARB(HDC hDrawDC, HDC hReadDC, HGLRC hglrc)
{
	icdlog("ENTRY: wglMakeContextCurrentARB(%p, %p, %p)\n", hDrawDC, hReadDC, hglrc);
	
	if(wglCallbacks.pfnGetDhglrc == NULL) /* library is in place replacement */
	{
		return private_wglMakeContextCurrentARB((uint32_t)hDrawDC, (uint32_t)hReadDC, (uint32_t)hglrc);
	}
	
	HGLRC dhglrc = wglCallbacks.pfnGetDhglrc(hglrc);
	if(dhglrc)
	{
		ctx_list_t *item = ctx_list_lookup(dhglrc);
		if(item)
		{
			if(private_wglMakeContextCurrentARB((uint32_t)hDrawDC, (uint32_t)hReadDC, (uint32_t)item->hwrc))
			{
				dhglrc_active = hglrc;
				return TRUE;
			}
		}
	}
	
	return FALSE;
}

/* aliases */
#define MK_ALIAS(_newname, _oldname, _type, _call, _params_def, _params_call) \
	_type _call _oldname _params_def; \
	_type WINAPI _newname _params_def { \
		icdlog("ENTRY: " #_newname "\n"); \
		return _oldname _params_call; \
	}
	
MK_ALIAS(mgdDescribeLayerPlane, mglDescribeLayerPlane, BOOL, WINAPI,
	(HDC hdc, int iPixelFormat, int iLayerPlane, INT nBytes, LPLAYERPLANEDESCRIPTOR ppfd),
	(hdc, iPixelFormat, iLayerPlane, nBytes, ppfd));

MK_ALIAS(mgdGetLayerPaletteEntries, mglGetLayerPaletteEntries, int, WINAPI,
	(HDC hdc, int iLayerPlane, int iStart, int cEntries, COLORREF *pcr),
	(hdc, iLayerPlane, iStart, cEntries, pcr));

MK_ALIAS(mgdGetProcAddress, mglGetProcAddress, uint32_t, PT_CALL,
	(uint32_t arg0), (arg0));

MK_ALIAS(mgdRealizeLayerPalette, mglRealizeLayerPalette, BOOL, WINAPI,
	(HDC hdc, int iLayerPlane, BOOL bRealize),
	(hdc, iLayerPlane, bRealize));

MK_ALIAS(mgdSetLayerPaletteEntries, mglSetLayerPaletteEntries, int, WINAPI,
	(HDC hdc, int iLayerPlane, int iStart, int cEntries, CONST COLORREF *pcr),
	(hdc, iLayerPlane, iStart, cEntries, pcr));

MK_ALIAS(mgdSwapBuffers, wglSwapBuffers, int, WINAPI,
	(HDC hdc), (hdc));

MK_ALIAS(mgdSwapLayerBuffers, mglSwapLayerBuffers, int, WINAPI,
	(HDC hdc, UINT arg1), (hdc, arg1));

MK_ALIAS(mgdShareLists, mglShareLists, uint32_t, PT_CALL,
	(uint32_t arg0, uint32_t arg1), (arg0, arg1));

/* if somebody ask for unload */
HRESULT WINAPI DllCanUnloadNow()
{
	icdlog("ENTRY: DllCanUnloadNow\n");
	return S_FALSE;
}
