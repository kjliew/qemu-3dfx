/*
 * Copyright (C) 1999-2001  Brian Paul   All Rights Reserved.
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

/*
 * This is a port of the infamous "gears" demo to straight GLX (i.e. no GLUT)
 * Port by Brian Paul  23 March 2001
 *
 * Command line options:
 *    -info      print GL implementation information
 *
 * Modified from X11/GLX to Win32/WGL by Ben Skeggs
 * 25th October 2004
 */

#include <assert.h>
#include <windows.h>
#include <GL/gl.h>
#include <GL/wglext.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* XXX this probably isn't very portable */
#include <time.h>
#ifndef _MSC_VER
#include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265
#endif /* !M_PI */

#ifndef WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB
#define WGL_FRAMEBUFFER_SRGB_CAPABLE_ARB 0x20A9
#endif

#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8db9
#endif

/* Global vars */
static HDC hDC;
static HGLRC hRC;
static HWND hWnd;
static HINSTANCE hInst;
static RECT winrect;

static const char *ProgramName;      /* program name (from argv[0]) */

static GLfloat view_rotx = 20.0, view_roty = 30.0, view_rotz = 0.0;
static GLint gear1, gear2, gear3;
static GLfloat angle = 0.0;
static GLboolean use_srgb = GL_FALSE;

static
void usage(void)
{
    fprintf (stderr, "usage:\n");
    fprintf (stderr, "-info              display OpenGL renderer info\n");
    fprintf (stderr, "-geometry WxH+X+Y  window geometry\n");
}

/* return current time (in seconds) */
static int
current_time(void)
{
    return (int)time(NULL);
}

/*
 *
 *  Draw a gear wheel.  You'll probably want to call this function when
 *  building a display list since we do a lot of trig here.
 *
 *  Input:  inner_radius - radius of hole at center
 *          outer_radius - radius at center of teeth
 *          width - width of gear
 *          teeth - number of teeth
 *          tooth_depth - depth of tooth
 */
static void
gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
     GLint teeth, GLfloat tooth_depth)
{
    GLint i;
    GLfloat r0, r1, r2;
    GLfloat angle, da;
    GLfloat u, v, len;

    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2.0;
    r2 = outer_radius + tooth_depth / 2.0;

    da = 2.0 * M_PI / teeth / 4.0;

    glShadeModel(GL_FLAT);

    glNormal3f(0.0, 0.0, 1.0);

    /* draw front face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        if (i < teeth) {
            glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
            glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
                       width * 0.5);
        }
    }
    glEnd();

    /* draw front sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0 * M_PI / teeth / 4.0;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
                   width * 0.5);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
                   width * 0.5);
    }
    glEnd();

    glNormal3f(0.0, 0.0, -1.0);

    /* draw back face */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        if (i < teeth) {
            glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
                       -width * 0.5);
            glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        }
    }
    glEnd();

    /* draw back sides of teeth */
    glBegin(GL_QUADS);
    da = 2.0 * M_PI / teeth / 4.0;
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
                   -width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
                   -width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
    }
    glEnd();

    /* draw outward faces of teeth */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i < teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;

        glVertex3f(r1 * cos(angle), r1 * sin(angle), width * 0.5);
        glVertex3f(r1 * cos(angle), r1 * sin(angle), -width * 0.5);
        u = r2 * cos(angle + da) - r1 * cos(angle);
        v = r2 * sin(angle + da) - r1 * sin(angle);
        len = sqrt(u * u + v * v);
        u /= len;
        v /= len;
        glNormal3f(v, -u, 0.0);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), width * 0.5);
        glVertex3f(r2 * cos(angle + da), r2 * sin(angle + da), -width * 0.5);
        glNormal3f(cos(angle), sin(angle), 0.0);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
                   width * 0.5);
        glVertex3f(r2 * cos(angle + 2 * da), r2 * sin(angle + 2 * da),
                   -width * 0.5);
        u = r1 * cos(angle + 3 * da) - r2 * cos(angle + 2 * da);
        v = r1 * sin(angle + 3 * da) - r2 * sin(angle + 2 * da);
        glNormal3f(v, -u, 0.0);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
                   width * 0.5);
        glVertex3f(r1 * cos(angle + 3 * da), r1 * sin(angle + 3 * da),
                   -width * 0.5);
        glNormal3f(cos(angle), sin(angle), 0.0);
    }

    glVertex3f(r1 * cos(0), r1 * sin(0), width * 0.5);
    glVertex3f(r1 * cos(0), r1 * sin(0), -width * 0.5);

    glEnd();

    glShadeModel(GL_SMOOTH);

    /* draw inside radius cylinder */
    glBegin(GL_QUAD_STRIP);
    for (i = 0; i <= teeth; i++) {
        angle = i * 2.0 * M_PI / teeth;
        glNormal3f(-cos(angle), -sin(angle), 0.0);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), -width * 0.5);
        glVertex3f(r0 * cos(angle), r0 * sin(angle), width * 0.5);
    }
    glEnd();
}

static void
draw(void)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glPushMatrix();
    glRotatef(view_rotx, 1.0, 0.0, 0.0);
    glRotatef(view_roty, 0.0, 1.0, 0.0);
    glRotatef(view_rotz, 0.0, 0.0, 1.0);

    glPushMatrix();
    glTranslatef(-3.0, -2.0, 0.0);
    glRotatef(angle, 0.0, 0.0, 1.0);
    glCallList(gear1);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(3.1, -2.0, 0.0);
    glRotatef(-2.0 * angle - 9.0, 0.0, 0.0, 1.0);
    glCallList(gear2);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(-3.1, 4.2, 0.0);
    glRotatef(-2.0 * angle - 25.0, 0.0, 0.0, 1.0);
    glCallList(gear3);
    glPopMatrix();

    glPopMatrix();
}

/* new window size or exposure */
static void
reshape(int width, int height)
{
    GLfloat h = (GLfloat) height / (GLfloat) width;

    glViewport(0, 0, (GLint) width, (GLint) height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -h, h, 5.0, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0, 0.0, -40.0);
}

static void
init(void)
{
    static GLfloat pos[4] = { 5.0, 5.0, 10.0, 0.0 };
    static GLfloat red[4] = { 0.8, 0.1, 0.0, 1.0 };
    static GLfloat green[4] = { 0.0, 0.8, 0.2, 1.0 };
    static GLfloat blue[4] = { 0.2, 0.2, 1.0, 1.0 };

    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glEnable(GL_CULL_FACE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_DEPTH_TEST);
    if (use_srgb) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    /* make the gears */
    gear1 = glGenLists(1);
    glNewList(gear1, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
    gear(1.0, 4.0, 1.0, 20, 0.7);
    glEndList();

    gear2 = glGenLists(1);
    glNewList(gear2, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, green);
    gear(0.5, 2.0, 2.0, 10, 0.7);
    glEndList();

    gear3 = glGenLists(1);
    glNewList(gear3, GL_COMPILE);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, blue);
    gear(1.3, 2.0, 0.5, 10, 0.7);
    glEndList();

    glEnable(GL_NORMALIZE);
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
        if (wParam == VK_LEFT)
            view_roty += 5.0;
        else if (wParam == VK_RIGHT)
            view_roty -= 5.0;
        else if (wParam == VK_UP)
            view_rotx += 5.0;
        else if (wParam == VK_DOWN)
            view_rotx -= 5.0;
        else if (wParam == VK_ESCAPE)
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
    int t, t0 = current_time();
    int frames = 0;

    while(1) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        angle += 2.0;
        draw();
        SwapBuffers(hDC);

        /* calc framerate */
        t = current_time();
        frames++;
        if (t - t0 >= 5.0) {
            GLfloat s = t - t0;
            GLfloat fps = frames / s;
            printf("%d frames in %3.1f seconds = %6.3f FPS\n", frames, s, fps);
            t0 = t;
            frames = 0;
        }
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
    unsigned int winWidth = 300, winHeight = 300;
    int x = 0, y = 0;
    int i;
    GLboolean printInfo = GL_FALSE;

    ProgramName = argv[0];

    enum_display_setting_current(&winWidth, &winHeight);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-info") == 0) {
            printInfo = GL_TRUE;
        }
        else if (strcmp(argv[i], "-srgb") == 0) {
            use_srgb = GL_TRUE;
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

    make_window("wglgears", x, y, winWidth, winHeight);
    reshape(winWidth, winHeight);

    if (printInfo) {
        printf("GL_RENDERER   = %s\n", (char *) glGetString(GL_RENDERER));
        printf("GL_VERSION    = %s\n", (char *) glGetString(GL_VERSION));
        printf("GL_VENDOR     = %s\n", (char *) glGetString(GL_VENDOR));
        printf("GL_EXTENSIONS = %s\n", (char *) glGetString(GL_EXTENSIONS));
    }

    init();

    event_loop();

    /* cleanup */
    wglMakeCurrent (NULL, NULL);
    wglDeleteContext (hRC);
    ReleaseDC (hWnd, hDC);

    return EXIT_SUCCESS;
}
