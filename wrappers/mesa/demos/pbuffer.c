/*
 * Copyright (c) ... in a Galaxy far, far away ... 
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <windows.h>
#include <GL/gl.h>
#include <GL/wgl.h>
#include <GL/wglext.h>
#include <stdlib.h>
#include <stdio.h>

/* Global vars */
static HDC hDC;
static HGLRC hRC;
static HWND hWnd;
static HINSTANCE hInst;
static RECT winrect;

static const char *ProgramName;      /* program name (from argv[0]) */

static HPBUFFERARB pBuffer;
static HDC         pBufferHDC, screenHDC;
static HGLRC       pBufferCtx, screenCtx;
static GLuint      pBufferTex, screenTex;
static GLuint      list;
static int         winWidth, winHeight;
static GLboolean use_srgb = GL_FALSE;

static void initPbuffer()
{
    PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB_func =
        (PFNWGLCHOOSEPIXELFORMATARBPROC) wglGetProcAddress("wglChoosePixelFormatARB");
    assert(wglChoosePixelFormatARB_func);
    PFNWGLCREATEPBUFFERARBPROC wglCreatePbufferARB_func =
        (PFNWGLCREATEPBUFFERARBPROC) wglGetProcAddress("wglCreatePbufferARB");
    assert(wglCreatePbufferARB_func);
    PFNWGLGETPBUFFERDCARBPROC wglGetPbufferDCARB_func =
        (PFNWGLGETPBUFFERDCARBPROC) wglGetProcAddress("wglGetPbufferDCARB");
    assert(wglGetPbufferDCARB_func);
    PFNWGLQUERYPBUFFERARBPROC wglQueryPbufferARB_func =
        (PFNWGLQUERYPBUFFERARBPROC) wglGetProcAddress("wglQueryPbufferARB");
    assert(wglQueryPbufferARB_func);

    // Create the pbuffer with the help of glew and glut
    int ia[] = {
        WGL_DRAW_TO_PBUFFER_ARB, 1,
        WGL_BIND_TO_TEXTURE_RGBA_ARB, 1,
        WGL_DEPTH_BITS_ARB, 24,
        WGL_RED_BITS_ARB,   8,
        WGL_GREEN_BITS_EXT, 8,
        WGL_BLUE_BITS_EXT,  8,
        0, 0
    };
    float fa[] = {
        0, 0
    };
    int fmts[64];
    unsigned nfmts = 0;
    if (!wglChoosePixelFormatARB_func(wglGetCurrentDC(), ia, fa, _countof(fmts), fmts, &nfmts) || !nfmts) {
        printf("wglChoosePixelFormat FAILED -- nfmts %d,  GetLastError 0x%08X\n", nfmts, GetLastError());
        getchar();
        exit(0);
    }

    int pb[] = {
        WGL_TEXTURE_FORMAT_ARB, WGL_TEXTURE_RGBA_ARB,
        WGL_TEXTURE_TARGET_ARB, WGL_TEXTURE_2D_ARB,
        WGL_PBUFFER_LARGEST_ARB, 1,
        0, 0
    };
    if (!(pBuffer = wglCreatePbufferARB_func(wglGetCurrentDC(), fmts[0], winWidth, winHeight, pb)))    __debugbreak();
    if (!(pBufferHDC = wglGetPbufferDCARB_func(pBuffer)))                                        __debugbreak();
    if (!(pBufferCtx = wglCreateContext(pBufferHDC)))                                       __debugbreak();

    // Get it's actual size
    int w;
    if (!wglQueryPbufferARB_func(pBuffer, WGL_PBUFFER_WIDTH_ARB, &w))                    __debugbreak();
    int h;
    if (!wglQueryPbufferARB_func(pBuffer, WGL_PBUFFER_HEIGHT_ARB, &h))                   __debugbreak();

    // Initialize it's projection matrix
    if (!wglMakeCurrent(pBufferHDC, pBufferCtx))                                            __debugbreak();
    void reshape(int w, int h);
    reshape(w, h);
    if (!wglMakeCurrent(screenHDC, screenCtx))                                              __debugbreak();
    if (!wglShareLists(screenCtx, pBufferCtx))                                              __debugbreak();
}

static void initLights()
{
    GLfloat light_ambient[]      = { 0.0f, 0.0f, 0.0f, 1.0f };  /* default value */
    GLfloat light_diffuse[]      = { 1.0f, 1.0f, 1.0f, 1.0f };  /* default value */
    GLfloat light_specular[]     = { 1.0f, 1.0f, 1.0f, 1.0f };  /* default value */
    GLfloat light_position[]     = { 0.0f, 1.0f, 1.0f, 0.0f };  /* NOT default value */
    GLfloat lightModel_ambient[] = { 0.2f, 0.2f, 0.2f, 1.0f };  /* default value */
    GLfloat material_specular[]  = { 1.0f, 1.0f, 1.0f, 1.0f };  /* NOT default value */
    GLfloat material_emission[]  = { 0.0f, 0.0f, 0.0f, 1.0f };  /* default value */

    glLightfv(GL_LIGHT0, GL_AMBIENT, light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
    glLightfv(GL_LIGHT0, GL_SPECULAR, light_specular);
    glLightfv(GL_LIGHT0, GL_POSITION, light_position);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lightModel_ambient);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glMaterialfv(GL_FRONT, GL_SPECULAR, material_specular);
    glMaterialfv(GL_FRONT, GL_EMISSION, material_emission);
    glMaterialf(GL_FRONT, GL_SHININESS, 10.0);                  /* NOT default value	*/

    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
}

/*
* Texture copied and modifided modified from:
* https://www.opengl.org/archives/resources/code/samples/mjktips/TexShadowReflectLight.html
*/
static char *circles[] = {
    "................",
    "................",
    "......xxxx......",
    "....xxxxxxxx....",
    "...xxxxxxxxxx...",
    "...xxx....xxx...",
    "..xxx......xxx..",
    "..xxx......xxx..",
    "..xxx......xxx..",
    "..xxx......xxx..",
    "...xxx....xxx...",
    "...xxxxxxxxxx...",
    "....xxxxxxxx....",
    "......xxxx......",
    "................",
    "................",
};

static void initTextures()
{
    GLubyte floorTexture[16][16][3];
    GLubyte *loc;
    int s, t;

    /* Setup RGB image for the texture. */
    loc = (GLubyte*)floorTexture;
    for (t = 0; t < 16; t++) {
        for (s = 0; s < 16; s++) {
            if (circles[t][s] == 'x') {
                /* Nice green. */
                loc[0] = 0x1f;
                loc[1] = 0x8f;
                loc[2] = 0x1f;
            }
            else {
                /* Light gray. */
                loc[0] = 0xaa;
                loc[1] = 0xaa;
                loc[2] = 0xaa;
            }
            loc += 3;
        }
    }

    // create, configure and initialize the textures
    for (int i = 0; i < 2; ++i) {
        glGenTextures(1, i ? &screenTex : &pBufferTex);
        glBindTexture(GL_TEXTURE_2D, i ? screenTex : pBufferTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, 3, 16, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, floorTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    }
}

void init(void)
{
    screenHDC = wglGetCurrentDC();
    screenCtx = wglGetCurrentContext();
    initPbuffer();
    initTextures();
}

void display(void)
{
    PFNWGLBINDTEXIMAGEARBPROC wglBindTexImageARB_func =
        (PFNWGLBINDTEXIMAGEARBPROC) wglGetProcAddress("wglBindTexImageARB");
    assert(wglBindTexImageARB_func);
    PFNWGLBINDTEXIMAGEARBPROC wglReleaseTexImageARB_func =
        (PFNWGLBINDTEXIMAGEARBPROC) wglGetProcAddress("wglReleaseTexImageARB");
    assert(wglReleaseTexImageARB_func);

    // First time?
    if (!list)
    {
        // Yes, Create the display list
        list = glGenLists(1);
        glNewList(list, GL_COMPILE);

        // Clear back-buffer
        glClearColor(.5f, .5f, .5f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Be sure light doesn't move
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        initLights();
        glPopMatrix();

        // Enable textures and depth testing
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_DEPTH_TEST);

        // Draw the cube with 6 quads
        glBegin(GL_QUADS);
        // Top face (y = 1.0f)
        glColor3f(0.0f, 1.0f, 0.0f);     // Green
        glNormal3f(0.0f, 1.0f, 0.0f);
        glTexCoord2f(0.0, 0.0);
        glVertex3f( 1.0f,  1.0f, -1.0f);
        glTexCoord2f(0.0, 1.0);
        glVertex3f(-1.0f,  1.0f, -1.0f);
        glTexCoord2f(1.0, 1.0);
        glVertex3f(-1.0f,  1.0f,  1.0f);
        glTexCoord2f(1.0, 0.0);
        glVertex3f( 1.0f,  1.0f,  1.0f);

        // Bottom face (y = -1.0f)
        glColor3f(1.0f, 0.5f, 0.0f);     // Orange
        glNormal3f(0.0f, -1.0f, 0.0f);
        glTexCoord2f(0.0, 0.0);
        glVertex3f( 1.0f, -1.0f,  1.0f);
        glTexCoord2f(0.0, 1.0);
        glVertex3f(-1.0f, -1.0f,  1.0f);
        glTexCoord2f(1.0, 1.0);
        glVertex3f(-1.0f, -1.0f, -1.0f);
        glTexCoord2f(1.0, 0.0);
        glVertex3f( 1.0f, -1.0f, -1.0f);

        // Front face  (z = 1.0f)
        glColor3f(1.0f, 0.0f, 0.0f);     // Red
        glNormal3f(0.0f, 0.0f, 1.0f);
        glTexCoord2f(0.0, 0.0);
        glVertex3f( 1.0f,  1.0f, 1.0f);
        glTexCoord2f(0.0, 1.0);
        glVertex3f(-1.0f,  1.0f, 1.0f);
        glTexCoord2f(1.0, 1.0);
        glVertex3f(-1.0f, -1.0f, 1.0f);
        glTexCoord2f(1.0, 0.0);
        glVertex3f( 1.0f, -1.0f, 1.0f);

        // Back face (z = -1.0f)
        glColor3f(1.0f, 1.0f, 0.0f);     // Yellow
        glNormal3f(0.0f, 0.0f, -1.0f);
        glTexCoord2f(0.0, 0.0);
        glVertex3f( 1.0f, -1.0f, -1.0f);
        glTexCoord2f(0.0, 1.0);
        glVertex3f(-1.0f, -1.0f, -1.0f);
        glTexCoord2f(1.0, 1.0);
        glVertex3f(-1.0f,  1.0f, -1.0f);
        glTexCoord2f(1.0, 0.0);
        glVertex3f( 1.0f,  1.0f, -1.0f);

        // Left face (x = -1.0f)
        glColor3f(0.0f, 0.0f, 1.0f);     // Blue
        glNormal3f(-1.0f, 0.0f, 0.0f);
        glTexCoord2f(0.0, 0.0);
        glVertex3f(-1.0f,  1.0f,  1.0f);
        glTexCoord2f(0.0, 1.0);
        glVertex3f(-1.0f,  1.0f, -1.0f);
        glTexCoord2f(1.0, 1.0);
        glVertex3f(-1.0f, -1.0f, -1.0f);
        glTexCoord2f(1.0, 0.0);
        glVertex3f(-1.0f, -1.0f,  1.0f);

        // Right face (x = 1.0f)
        glColor3f(1.0f, 0.0f, 1.0f);     // Magenta
        glNormal3f(1.0f, 0.0f, 0.0f);
        glTexCoord2f(0.0, 0.0);
        glVertex3f( 1.0f,  1.0f, -1.0f);
        glTexCoord2f(0.0, 1.0);
        glVertex3f( 1.0f,  1.0f,  1.0f);
        glTexCoord2f(1.0, 1.0);
        glVertex3f( 1.0f, -1.0f,  1.0f);
        glTexCoord2f(1.0, 0.0);
        glVertex3f( 1.0f, -1.0f, -1.0f);
        glEnd();

        glEndList();
    }

    // The frame counter
    static unsigned cnt;

    for (int i = 0; i < 2; ++i)
    {
        // first-pass, draw into pbuffer; second-pass, draw into backbuffer
        if (i)  {
            if (!wglMakeCurrent(screenHDC, screenCtx))                                      __debugbreak();
            glBindTexture(GL_TEXTURE_2D, screenTex);
        }
        else {
            if (!wglMakeCurrent(pBufferHDC, pBufferCtx))                                    __debugbreak();
            glBindTexture(GL_TEXTURE_2D, pBufferTex);
        }

        // Set absolute rotation
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glRotatef((float)(cnt % 360), 1, 0, 0);
        glRotatef(45.f, 0, 0, 1);

        // Draw; on the second-pass use pbuffer as texture image
        if (i && !wglBindTexImageARB_func(pBuffer, WGL_FRONT_LEFT_ARB))                           __debugbreak();
        glCallList(list);
        if (i && !wglReleaseTexImageARB_func(pBuffer, WGL_FRONT_LEFT_ARB))                        __debugbreak();
    }

    // Count
    ++cnt;

}

void reshape(int w, int h)
{
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    if (w <= h)
        glOrtho(-2.f, 2.f, -2.f*h / w, 2.f*h / w, -10.f, 10.f);
    else
        glOrtho(-2.f*w / h, 2.f*w / h, -2.f, 2.f, -10.f, 10.f);
}

static
void usage(void)
{
    fprintf (stderr, "usage:\n");
    fprintf (stderr, "-geometry WxH+X+Y  window geometry\n");
}

static LRESULT CALLBACK
WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        reshape(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

/*
 * Create an RGB, double-buffered window.
 * Return the window and context handles.
 */
static void
make_window(const char *name, int x, int y, int width, int height)
{
    int pixelFormat;
    WNDCLASS wc;
    DWORD dwExStyle, dwStyle;
    static const PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR),
        1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA,
        24,
        0, 0, 0, 0, 0, 0,
        0,
        0,
        0,
        0, 0, 0, 0,
        16,
        0,
        0,
        PFD_MAIN_PLANE,
        0,
        0, 0, 0
    };

    winrect.left = (long)0;
    winrect.right = (long)width;
    winrect.top = (long) 0;
    winrect.bottom = (long)height;

    hInst = GetModuleHandle(NULL);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = (WNDPROC)WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(NULL, IDI_WINLOGO);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = name;
    if (!RegisterClass(&wc)) {
        printf("failed to register class\n");
        exit(0);
    }

    dwExStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE;
    dwStyle = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectEx(&winrect, dwStyle, FALSE, dwExStyle);

    if (!(hWnd = CreateWindowEx(dwExStyle, name, name,
                                WS_CLIPSIBLINGS | WS_CLIPCHILDREN | dwStyle,
                                x, y,
                                winrect.right - winrect.left,
                                winrect.bottom - winrect.top,
                                NULL, NULL, hInst, NULL))) {
        printf("failed to create window\n");
        exit(0);
    }

    if (!(hDC = GetDC(hWnd)) ||
            !(pixelFormat = ChoosePixelFormat(hDC, &pfd)) ||
            !(SetPixelFormat(hDC, pixelFormat, &pfd)) ||
            !(hRC = wglCreateContext(hDC)) ||
            !(wglMakeCurrent(hDC, hRC))) {
        printf("failed to initialise opengl\n");
        exit(0);
    }

    if (use_srgb) {
        /* We can't query/use extension functions until after we've
         * created and bound a rendering context (done above).
         *
         * We can only set the pixel format of the window once, so we need to
         * create a new device context in order to use the pixel format returned
         * from wglChoosePixelFormatARB, and then create a new window.
         */
        PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB_func =
            (PFNWGLCHOOSEPIXELFORMATARBPROC)
            wglGetProcAddress("wglChoosePixelFormatARB");
        assert(wglChoosePixelFormatARB_func);

        static const int int_attribs[] = {
            WGL_SUPPORT_OPENGL_ARB, TRUE,
            WGL_DRAW_TO_WINDOW_ARB, TRUE,
            WGL_COLOR_BITS_ARB, 24,  // at least 24-bits of RGB
            WGL_DEPTH_BITS_ARB, 24,
            WGL_DOUBLE_BUFFER_ARB, TRUE,
            WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB, TRUE,
            0
        };
        static const float float_attribs[] = { 0 };
        UINT numFormats;

        pixelFormat = 0;
        if (!wglChoosePixelFormatARB_func(hDC, int_attribs, float_attribs, 1,
                                          &pixelFormat, &numFormats)) {
            printf("wglChoosePixelFormatARB failed\n");
            exit(0);
        }
        assert(numFormats > 0);
        printf("Chose sRGB pixel format %d (0x%x)\n", pixelFormat, pixelFormat);
        fflush(stdout);

        PIXELFORMATDESCRIPTOR newPfd;
        DescribePixelFormat(hDC, pixelFormat, sizeof(pfd), &newPfd);

        /* now, create new context with new pixel format */
        wglMakeCurrent(hDC, NULL);
        wglDeleteContext(hRC);
        DeleteDC(hDC);

        if (!(hWnd = CreateWindowEx(dwExStyle, name, name,
                                    WS_CLIPSIBLINGS | WS_CLIPCHILDREN | dwStyle,
                                    0, 0,
                                    winrect.right - winrect.left,
                                    winrect.bottom - winrect.top,
                                    NULL, NULL, hInst, NULL))) {
            printf("failed to create window\n");
            exit(0);
        }

        if (!(hDC = GetDC(hWnd))) {
            printf("GetDC() failed.\n");
            exit(0);
        }
        if (!SetPixelFormat(hDC, pixelFormat, &pfd)) {
            printf("SetPixelFormat failed %d\n", (int) GetLastError());
            exit(0);
        }
        if (!(hRC = wglCreateContext(hDC))) {
            printf("wglCreateContext() failed\n");
            exit(0);
        }
        if (!wglMakeCurrent(hDC, hRC)) {
            printf("wglMakeCurrent() failed\n");
            exit(0);
        }
    }

    ShowWindow(hWnd, SW_SHOW);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
}

static void
event_loop(void)
{
    MSG msg;
    int t, t0 = timeGetTime();

    while(1) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        display();
        SwapBuffers(hDC);

        /* calc framerate */
        t = timeGetTime();
        if ((1000 / 60) > (t - t0))
            Sleep((1000 / 60) - (t - t0));
        t0 = timeGetTime();
    }
}

static void
parse_geometry(const char *str, int *x, int *y, unsigned int *w, unsigned int *h)
{
    char *end;
    if (*str == '=')
        str++;

    long tw = LONG_MAX;
    if (isdigit(*str)) {
        tw = strtol(str, &end, 10);
        if (str == end)
            return;
        str = end;
    }

    long th = LONG_MAX;
    if (tolower(*str) == 'x') {
        str++;
        th = strtol(str, &end, 10);
        if (str== end)
            return;
        str = end;
    }

    long tx = LONG_MAX;
    if (*str == '+' || *str == '-') {
        tx = strtol(str, &end, 10);
        if (str == end)
            return;
        str = end;
    }

    long ty = LONG_MAX;
    if (*str == '+' || *str == '-') {
        ty = strtol(str, &end, 10);
        if (str == end)
            return;
        str = end;
    }

    if (tw < LONG_MAX)
        *w = tw;
    if (th < LONG_MAX)
        *h = th;
    if (tx < INT_MAX)
        *x = tx;
    if (ty < INT_MAX)
        *y = ty;
}

static void
enum_display_setting_current(int *w, int *h)
{
    DEVMODE m = { .dmSize = sizeof(DEVMODE) };
    if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, &m)) {
        *w = m.dmPelsWidth;
        *h = m.dmPelsHeight;
    }
}

int
main(int argc, char *argv[])
{
    int x = 0, y = 0;
    int i;

    ProgramName = argv[0];

    enum_display_setting_current(&winWidth, &winHeight);

    for (i = 1; i < argc; i++) {
        if (0) {
        }
        else if (strcmp(argv[i], "-geometry") == 0) {
            parse_geometry(argv[i+1], &x, &y, &winWidth, &winHeight);
            i++;
        }
        else {
            usage();
            return -1;
        }
    }

    make_window("pbuffer", x, y, winWidth, winHeight);
    reshape(winWidth, winHeight);
    
    printf("OpenGL vendor  : %s\n", glGetString(GL_VENDOR));
    printf("OpenGL renderer: %s\n", glGetString(GL_RENDERER));
    printf("OpenGL version : %s\n", glGetString(GL_VERSION));
    
    PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB_func =
        (PFNWGLGETEXTENSIONSSTRINGARBPROC)
        wglGetProcAddress("wglGetExtensionsStringARB");
    assert(wglGetExtensionsStringARB_func);
    const char *wglstr = wglGetExtensionsStringARB_func(wglGetCurrentDC());

    if (strstr(wglstr, "WGL_ARB_pbuffer") &&
        strstr(wglstr, "WGL_ARB_render_texture"))
    {
        init();
        event_loop();
    }
    else
        fprintf(stderr, "\n%sunsupported\n",
                "WGL_ARB_pbuffer WGL_ARB_render_texture ");

    /* cleanup */
    wglMakeCurrent (NULL, NULL);
    wglDeleteContext (hRC);
    ReleaseDC (hWnd, hDC);

    return EXIT_SUCCESS;
}
