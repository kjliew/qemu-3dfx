#include <stdio.h>
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wgl.h>

static void MGLTmpContext(char **str, char **wstr)
{
    HWND tmpWin = CreateWindow("STATIC", "dummy",
        WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 640, 480, NULL, NULL, NULL, NULL);
    HDC  tmpDC = GetDC(tmpWin);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof(PIXELFORMATDESCRIPTOR));
    pfd.nSize = sizeof(PIXELFORMATDESCRIPTOR);
    pfd.nVersion = 1;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.iLayerType = PFD_MAIN_PLANE,
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cAlphaBits = 8;
    pfd.cStencilBits = 8;
    SetPixelFormat(tmpDC, ChoosePixelFormat(tmpDC, &pfd), &pfd);
    HGLRC tmpGL = wglCreateContext(tmpDC);
    wglMakeCurrent(tmpDC, tmpGL);

    const char * (WINAPI *wglGetString)(HDC hdc) = (const char * (WINAPI *)(HDC))
        wglGetProcAddress("wglGetExtensionsStringARB");
    /*
    BOOL (WINAPI *wglChoosePixelFormatARB)(HDC, const int *, const FLOAT *, UINT, int *, UINT *) =
        (BOOL (WINAPI *)(HDC, const int *, const FLOAT *, UINT, int *, UINT *))
        wglGetProcAddress("wglChoosePixelFormatARB");
    */
    BOOL (WINAPI *wglGetPixelFormatAttribivARB)(HDC, int, int, UINT, const int *, int *) =
        (BOOL (WINAPI *)(HDC, int, int, UINT, const int *, int *))
        wglGetProcAddress("wglGetPixelFormatAttribivARB");

    const int na[] = { WGL_NUMBER_PIXEL_FORMATS_ARB },
          piAttr[] = {
        WGL_DRAW_TO_WINDOW_ARB,
        WGL_DRAW_TO_BITMAP_ARB,
        WGL_ACCELERATION_ARB,
        WGL_NEED_PALETTE_ARB,
        WGL_NEED_SYSTEM_PALETTE_ARB,
        WGL_SWAP_LAYER_BUFFERS_ARB,
        WGL_SWAP_METHOD_ARB,
        WGL_NUMBER_OVERLAYS_ARB,
        WGL_NUMBER_UNDERLAYS_ARB,
        WGL_SUPPORT_GDI_ARB,
        WGL_SUPPORT_OPENGL_ARB,
        WGL_DOUBLE_BUFFER_ARB,
        WGL_STEREO_ARB,
        WGL_PIXEL_TYPE_ARB,
        WGL_COLOR_BITS_ARB,
        WGL_RED_BITS_ARB,
        WGL_GREEN_BITS_ARB,
        WGL_BLUE_BITS_ARB,
        WGL_ALPHA_BITS_ARB,
        WGL_ALPHA_SHIFT_ARB,
        WGL_RED_SHIFT_ARB,
        WGL_GREEN_SHIFT_ARB,
        WGL_BLUE_SHIFT_ARB,
        WGL_ACCUM_BITS_ARB,
        WGL_ACCUM_RED_BITS_ARB,
        WGL_ACCUM_GREEN_BITS_ARB,
        WGL_ACCUM_BLUE_BITS_ARB,
        WGL_ACCUM_ALPHA_BITS_ARB,
        WGL_DEPTH_BITS_ARB,
        WGL_STENCIL_BITS_ARB,
        WGL_AUX_BUFFERS_ARB,
        WGL_SAMPLE_BUFFERS_ARB,
        WGL_SAMPLES_ARB,
    };
    const UINT nAttr = sizeof(piAttr) / sizeof(int);
    int nPix, n[nAttr];

    const char accel[] = "0NGF",
          swap[] = "XCU",
          type[] = "f0rc";
    const struct {
        char *fmt;
        int enm;
    } strname[] = {
        { .fmt = "Renderer : %s [ %d ]\n",   .enm = GL_RENDERER, },
        { .fmt = "Vendor   : %s [ %d ]\n",   .enm = GL_VENDOR,   },
        { .fmt = "Version  : %s [ %d ]\n\n", .enm = GL_VERSION,  },
        { .fmt = "Shading Language Version : %s [ %d ]\n\n", .enm = GL_SHADING_LANGUAGE_VERSION, },
        { .fmt = 0, .enm = 0, },
    };
    for (int i = 0; strname[i].enm; i++) {
        const char *namestr;
        char *namebuf;
        namestr = (const char *)glGetString(strname[i].enm);
        namebuf = HeapAlloc(GetProcessHeap(), 0, 1 + strlen(namestr));
        if (namestr && namebuf) {
            lstrcpy(namebuf, namestr);
            printf(strname[i].fmt, namebuf, 1 + strlen(namestr));
            HeapFree(GetProcessHeap(), 0, namebuf);
        }
    }

    wglGetPixelFormatAttribivARB(tmpDC, 1, 0, 1, na, &nPix);
    printf("Total pixel formats %d\n", nPix);
    printf("\n"
       "      w b a p n s s o u g o d s t                                                    a     \n"
       "      i m c a s l w v n d g b t y                                ac                  u    n\n"
       "  fmt n p c l p y p l l i l f e p  cb   r  g  b  a   a  r  g  b  cm  r  g  b  a  d s x b  s\n"
       "--------------------------------------------------------------------------------------------\n");
    /*
        0x001 1 0 F 0 0 0 U 0 0 0 1 0 0 r  32   8  8  8  0   0 16  8  0  64 16 16 16 16 24 0 4 0  0
        0x002 1 0 F 0 0 0 U 0 0 0 1 0 0 r  32   8  8  8  8  24 16  8  0  64 16 16 16 16 24 0 4 0  0
        0x003 1 0 F 0 0 0 U 0 0 0 1 0 0 r  32   8  8  8  0   0 16  8  0  64 16 16 16 16 24 8 4 0  0
    */
    int curr = GetPixelFormat(tmpDC);
    wglGetPixelFormatAttribivARB(tmpDC, curr, 0, nAttr, piAttr, n);
    printf(
    "0x%03x %-2x%-2x%-2c%-2x%-2x%-2x%-2c%-2x%-2x%-2x%-2x%-2x%-2x%-2c%3d %3d%3d%3d%3d %3d%3d%3d%3d  %2d %2d %2d %2d %2d %2d %d %d %d %2d\n",
     curr, n[0],n[1],accel[n[2]&0x3],n[3],n[4],n[5],swap[n[6]&0x3],n[7],n[8],n[9],n[10],n[11],n[12],type[(n[13]&0xC)>>2],n[14],n[15],
           n[16],n[17],n[18],n[19],n[20],n[21],n[22],n[23],n[24],n[25],n[26],n[27],n[28],n[29],n[30],n[31],n[32]);
    for (int i = 1; i <= nPix; i++) {
        if (curr == i )
            continue;
        wglGetPixelFormatAttribivARB(tmpDC, i, 0, nAttr, piAttr, n);
        printf(
        "0x%03x %-2x%-2x%-2c%-2x%-2x%-2x%-2c%-2x%-2x%-2x%-2x%-2x%-2x%-2c%3d %3d%3d%3d%3d %3d%3d%3d%3d  %2d %2d %2d %2d %2d %2d %d %d %d %2d\n",
            i, n[0],n[1],accel[n[2]&0x3],n[3],n[4],n[5],swap[n[6]&0x3],n[7],n[8],n[9],n[10],n[11],n[12],type[(n[13]&0xC)>>2],n[14],n[15],
               n[16],n[17],n[18],n[19],n[20],n[21],n[22],n[23],n[24],n[25],n[26],n[27],n[28],n[29],n[30],n[31],n[32]);
    }
    printf("\n");

    const char *glstr = (const char *)glGetString(GL_EXTENSIONS),
              *wglstr = (const char *)wglGetString(tmpDC);

    *str = HeapAlloc(GetProcessHeap(), 0, 1 + strlen(glstr));
    *wstr = HeapAlloc(GetProcessHeap(), 0, 1 + strlen(wglstr));
    if (*str)
        lstrcpy(*str, glstr);
    if (*wstr)
        lstrcpy(*wstr, wglstr);

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmpGL);
    ReleaseDC(tmpWin, tmpDC);
    DestroyWindow(tmpWin);
}

int main()
{
    char *str;
    char *wstr;
    MGLTmpContext(&str, &wstr);
    if (str && wstr) {
        printf("%s [ %d ]\n%s [ %d ]\n", str, 1 + strlen(str), wstr, 1 + strlen(wstr));
        HeapFree(GetProcessHeap(), 0, str);
        HeapFree(GetProcessHeap(), 0, wstr);
    }
    return 0;
}

