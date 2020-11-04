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
#include "qemu/timer.h"
#include "qemu-common.h"
#include "cpu.h"
#include "ui/console.h"

#include "mesagl_impl.h"

#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "glcntx: " fmt "\n" , ## __VA_ARGS__); } while(0)


#if defined(CONFIG_WIN32) && CONFIG_WIN32
#include "sysemu/whpx.h"
#include <winhvplatformdefs.h>
#include <GL/gl.h>
#include <GL/wglext.h>

static LONG WINAPI MGLWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch(uMsg) {
	case WM_MOUSEACTIVATE:
	    return MA_NOACTIVATEANDEAT;
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
	case WM_NCLBUTTONDOWN:
	    return 0;
	default:
	    break;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

static HWND CreateMesaWindow(const char *title, int w, int h, int show)
{
    HWND 	hWnd;
    WNDCLASS 	wc;
    static HINSTANCE hInstance = 0;

    if (!hInstance) {
	memset(&wc, 0, sizeof(WNDCLASS));
	hInstance = GetModuleHandle(NULL);
        wc.hInstance = hInstance;
	wc.style	= CS_OWNDC;
	wc.lpfnWndProc	= (WNDPROC)MGLWndProc;
	wc.lpszClassName = "MGLWnd";

	if (!RegisterClass(&wc)) {
	    DPRINTF("RegisterClass() faled, Error %08lx", GetLastError());
	    return NULL;
	}
    }
    
    RECT rect;
    rect.top = 0; rect.left = 0;
    rect.right = w; rect.bottom = h;
    AdjustWindowRectEx(&rect, WS_CAPTION, FALSE, 0);
    rect.right  -= rect.left;
    rect.bottom -= rect.top;
    hWnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_NOACTIVATE,
	    "MGLWnd", title, 
	    WS_CAPTION | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
	    CW_USEDEFAULT, CW_USEDEFAULT, rect.right, rect.bottom,
	    NULL, NULL, hInstance, NULL);
    if (show) {
        GetClientRect(hWnd, &rect);
        DPRINTF("    window %lux%lu", rect.right, rect.bottom);
        ShowCursor(FALSE);
        ShowWindow(hWnd, SW_SHOW);
    }

    return hWnd;
}

static int *iattribs_fb(const int do_msaa)
{
    static int ia[] = {
        WGL_DRAW_TO_WINDOW_ARB, 1,
        WGL_SUPPORT_OPENGL_ARB, 1,
        WGL_DOUBLE_BUFFER_ARB, 1,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
        WGL_DEPTH_BITS_ARB,  24,
        WGL_STENCIL_BITS_ARB, 8,
        WGL_RED_BITS_ARB,   8,
        WGL_GREEN_BITS_ARB, 8,
        WGL_BLUE_BITS_ARB,  8,
        WGL_ALPHA_BITS_ARB, 8,
        WGL_SAMPLE_BUFFERS_ARB, 0,
        WGL_SAMPLES_ARB, 0,
        0,0,
    };
    for (int i = 0; ia[i]; i+=2) {
        switch(ia[i]) {
            case WGL_SAMPLE_BUFFERS_ARB:
                ia[i+1] = (do_msaa && GetContextMSAA())? 1:0;
                break;
            case WGL_SAMPLES_ARB:
                ia[i+1] = (do_msaa)? GetContextMSAA():0;
                break;
            default:
                break;
        }
    }
    return ia;
}


static HWND hwnd;
static HDC hDC, hPBDC[MAX_PBUFFER];
static HGLRC hRC[MAX_LVLCNTX], hPBRC[MAX_PBUFFER];
static HPBUFFERARB hPbuffer[MAX_PBUFFER];

static struct {
    HGLRC (WINAPI *CreateContext)(HDC);
    BOOL  (WINAPI *MakeCurrent)(HDC, HGLRC);
    BOOL  (WINAPI *DeleteContext)(HGLRC);
    BOOL  (WINAPI *UseFontBitmapsA)(HDC, DWORD, DWORD, DWORD);
    BOOL  (WINAPI *ShareLists)(HGLRC, HGLRC);
    PROC  (WINAPI *GetProcAddress)(LPCSTR);
    /* WGL extensions */
    BOOL (WINAPI *GetPixelFormatAttribivARB)(HDC, int, int, UINT, const int *, int *);
    BOOL (WINAPI *ChoosePixelFormatARB)(HDC, const int *, const float *, UINT, int *, UINT *);
    const char * (WINAPI *GetExtensionsStringARB)(HDC);
    HGLRC (WINAPI *CreateContextAttribsARB)(HDC, HGLRC, const int *);
    BOOL (WINAPI *SwapIntervalEXT)(int);
    int (WINAPI *GetSwapIntervalEXT)(void);
} wglFuncs;

static void MesaInitGammaRamp(void)
{
    struct {
        uint16_t r[256];
        uint16_t g[256];
        uint16_t b[256];
    } GammaRamp;

    for (int i = 0; i < 256; i++) {
        GammaRamp.r[i] = (uint16_t)(((i << 8) | i) & 0xFFFFU);
        GammaRamp.g[i] = (uint16_t)(((i << 8) | i) & 0xFFFFU);
        GammaRamp.b[i] = (uint16_t)(((i << 8) | i) & 0xFFFFU);
    }
    SetDeviceGammaRamp(hDC, &GammaRamp);
}

void SetMesaFuncPtr(void *p)
{
    HINSTANCE hDLL = (HINSTANCE)p;
    wglFuncs.GetProcAddress = (PROC (WINAPI *)(LPCSTR))GetProcAddress(hDLL, "wglGetProcAddress");
    wglFuncs.CreateContext = (HGLRC (WINAPI *)(HDC))GetProcAddress(hDLL, "wglCreateContext");
    wglFuncs.MakeCurrent   = (BOOL (WINAPI *)(HDC, HGLRC))GetProcAddress(hDLL, "wglMakeCurrent");
    wglFuncs.DeleteContext = (BOOL (WINAPI *)(HGLRC))GetProcAddress(hDLL, "wglDeleteContext");
    wglFuncs.UseFontBitmapsA = (BOOL (WINAPI *)(HDC, DWORD, DWORD, DWORD))GetProcAddress(hDLL, "wglUseFontBitmapsA");
    wglFuncs.ShareLists = (BOOL (WINAPI *)(HGLRC, HGLRC))GetProcAddress(hDLL, "wglShareLists");
}

void *MesaGLGetProc(const char *proc)
{
    return (void *)wglFuncs.GetProcAddress(proc);
}

int MGLBOUseAccel(void)
{
    return GetBufOAccelEN()? whpx_enabled():0;
}
void MGLBOMap(mapbufo_t *bufo)
{
    whpx_update_guest_pa_range(bufo->gpa - ALIGNPG((bufo->mapsz + (bufo->hva & 0xFFFU))),
        ALIGNPG((bufo->mapsz + (bufo->hva & 0xFFFU))),
        (void *)(bufo->hva - (bufo->hva & 0xFFFU)),
        (WHvMapGpaRangeFlagRead |
        ((bufo->acc & GL_MAP_WRITE_BIT)? WHvMapGpaRangeFlagWrite:0)), 1);
}
void MGLBOUnmap(mapbufo_t *bufo)
{
    whpx_update_guest_pa_range(bufo->gpa - ALIGNPG((bufo->mapsz + (bufo->hva & 0xFFFU))),
        ALIGNPG((bufo->mapsz + (bufo->hva & 0xFFFU))),
        (void *)(bufo->hva - (bufo->hva & 0xFFFU)), 0, 0);
}

void MGLTmpContext(void)
{
    HWND tmpWin = CreateMesaWindow("dummy", 640, 480, 0);
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
    HGLRC tmpGL = wglFuncs.CreateContext(tmpDC);
    wglFuncs.MakeCurrent(tmpDC, tmpGL);

    wglFuncs.GetPixelFormatAttribivARB = (BOOL (WINAPI *)(HDC, int, int, UINT, const int *, int *))
        MesaGLGetProc("wglGetPixelFormatAttribivARB");
    wglFuncs.ChoosePixelFormatARB = (BOOL (WINAPI *)(HDC, const int *, const float *, UINT, int *, UINT *))
        MesaGLGetProc("wglChoosePixelFormatARB");
    wglFuncs.GetExtensionsStringARB =  (const char * (WINAPI *)(HDC))
        MesaGLGetProc("wglGetExtensionsStringARB");
    wglFuncs.CreateContextAttribsARB = (HGLRC (WINAPI *)(HDC, HGLRC, const int *))
        MesaGLGetProc("wglCreateContextAttribsARB");
    wglFuncs.SwapIntervalEXT = (BOOL (WINAPI *)(int))
        MesaGLGetProc("wglSwapIntervalEXT");
    wglFuncs.GetSwapIntervalEXT = (int (WINAPI *)(void))
        MesaGLGetProc("wglGetSwapIntervalEXT");

    wglFuncs.MakeCurrent(NULL, NULL);
    wglFuncs.DeleteContext(tmpGL);
    ReleaseDC(tmpWin, tmpDC);
    DestroyWindow(tmpWin);
}

#define GLWINDOW_INIT() \
    if (hDC == 0) { hwnd = GetCreateWindow()? \
    CreateMesaWindow("MesaGL",640,480,1): \
    ((HWND)mesa_prepare_window()); hDC = GetDC(hwnd); }

#define GLWINDOW_FINI() \
    if (GetCreateWindow()) DestroyWindow(hwnd); \
    else mesa_release_window()

void MGLDeleteContext(int level)
{
    int n = (level >= MAX_LVLCNTX)? (MAX_LVLCNTX - 1):level;
    wglFuncs.MakeCurrent(NULL, NULL);
    if (n == 0) {
        for (int i = MAX_LVLCNTX; i > 1;) {
            if (hRC[--i]) {
                wglFuncs.DeleteContext(hRC[i]);
                hRC[i] = 0;
            }
        }
    }
    wglFuncs.DeleteContext(hRC[n]);
    hRC[n] = 0;
    if (!GetCreateWindow())
        MGLActivateHandler(0);
}

void MGLWndRelease(void)
{
    if (hwnd) {
        MesaInitGammaRamp();
        ReleaseDC(hwnd, hDC);
        GLWINDOW_FINI();
        hDC = 0;
        hwnd = 0;
    }
}

int MGLCreateContext(uint32_t gDC)
{
    int i, ret;
    i = gDC & (MAX_PBUFFER - 1);
    if (gDC == ((MESAGL_HPBDC & 0xFFFFFFF0U) | i)) {
        hPBRC[i] = wglFuncs.CreateContext(hPBDC[i]);
        ret = (hPBRC[i])? 0:1;
    }
    else {
        wglFuncs.MakeCurrent(NULL, NULL);
        for (i = MAX_LVLCNTX; i > 0;) {
            if (hRC[--i]) {
                wglFuncs.DeleteContext(hRC[i]);
                hRC[i] = 0;
            }
        }
        hRC[0] = wglFuncs.CreateContext(hDC);
        ret = (hRC[0])? 0:1;
    }
    return ret;
}

int MGLMakeCurrent(uint32_t cntxRC, int level)
{
    uint32_t i = cntxRC & (MAX_PBUFFER - 1), n = (level >= MAX_LVLCNTX)? (MAX_LVLCNTX - 1):level;
    if (cntxRC == (MESAGL_MAGIC - n)) {
        wglFuncs.MakeCurrent(hDC, hRC[n]);
        InitMesaGLExt();
        int val = GetContextVsync();
        if (val == -1) { }
        else if (wglFuncs.SwapIntervalEXT)
            wglFuncs.SwapIntervalEXT(val);
    }
    if (cntxRC == (((MESAGL_MAGIC & 0xFFFFFFFU) << 4) | i))
        wglFuncs.MakeCurrent(hPBDC[i], hPBRC[i]);

    return 0;
}

int MGLSwapBuffers(void)
{
    MGLActivateHandler(1);
    return SwapBuffers(hDC);
}

static int MGLPresetPixelFormat(void)
{
    int ipixfmt = 0;
    ImplMesaGLReset();

    if (wglFuncs.ChoosePixelFormatARB) {
        static const float fa[] = {0, 0};
        int *ia = iattribs_fb(GetContextMSAA());
        int pi[64]; UINT nFmts = 0;
        BOOL status = wglFuncs.ChoosePixelFormatARB(hDC, ia, fa, 64, pi, &nFmts);
        if (GetContextMSAA() && !nFmts) {
            ia = iattribs_fb(0);
            status = wglFuncs.ChoosePixelFormatARB(hDC, ia, fa, 64, pi, &nFmts);
        }
        if (status && nFmts)
            ipixfmt = (nFmts)? pi[0]:0;

    }

    if (ipixfmt == 0) {
        DPRINTF("Fallback to legacy OpenGL context creation");
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
        ipixfmt = ChoosePixelFormat(hDC, &pfd);
    }

    MesaInitGammaRamp();
    return ipixfmt;
}

int MGLChoosePixelFormat(void)
{
    int fmt, curr;
    GLWINDOW_INIT();
    curr = GetPixelFormat(hDC);
    if (curr == 0)
        curr = MGLPresetPixelFormat();
    fmt = curr;
    DPRINTF("ChoosePixelFormat() fmt 0x%02x", fmt);
    return fmt;
}

int MGLSetPixelFormat(int fmt, const void *p)
{
    const PIXELFORMATDESCRIPTOR *ppfd = (const PIXELFORMATDESCRIPTOR *)p;
    int curr, ret;
    GLWINDOW_INIT();
    curr = GetPixelFormat(hDC);
    if (curr == 0)
        curr = MGLPresetPixelFormat();
    if (wglFuncs.GetPixelFormatAttribivARB) {
        static const int iattr[] = {
            WGL_AUX_BUFFERS_ARB,
            WGL_SAMPLE_BUFFERS_ARB,
            WGL_SAMPLES_ARB,
        };
        int cattr[3];
        wglFuncs.GetPixelFormatAttribivARB(hDC, curr, 0, 3, iattr, cattr);
        DPRINTF("PixFmt 0x%02x nAux %d nSamples %d %d", curr,
            cattr[0], cattr[1], cattr[2]);
    }
    ret = SetPixelFormat(hDC, curr, (ppfd->nSize)? ppfd:0);
    DPRINTF("SetPixelFormat() fmt 0x%02x ret %d", curr, (ret)? 1:0);
    return ret;
}

int MGLDescribePixelFormat(int fmt, unsigned int sz, void *p)
{
    LPPIXELFORMATDESCRIPTOR ppfd = (LPPIXELFORMATDESCRIPTOR)p;
    int curr;
    GLWINDOW_INIT();
    curr = GetPixelFormat(hDC);
    if (curr == 0)
        curr = MGLPresetPixelFormat();
    if (sz == sizeof(PIXELFORMATDESCRIPTOR)) {
        int cattr[2];
        if (wglFuncs.GetPixelFormatAttribivARB) {
            static const int iattr[] = {
                WGL_SUPPORT_OPENGL_ARB,
                WGL_ACCELERATION_ARB,
            };
            wglFuncs.GetPixelFormatAttribivARB(hDC, curr, 0, 2, iattr, cattr);
        }
        DescribePixelFormat(hDC, curr, sizeof(PIXELFORMATDESCRIPTOR), ppfd);
        ppfd->dwFlags |= (cattr[0] && (cattr[1] == WGL_FULL_ACCELERATION_ARB))? PFD_SUPPORT_OPENGL:0;
        DPRINTF("DescribePixelFormat() dwFlags:%08lx\n"
            "  cColorbits:%02d cDepthBits:%02d cStencilBits:%02d ARGB%d%d%d%d\n"
            "  cAlphaShift:%02d cRedShift:%02d cGreenShift:%02d cBlueShift:%02d",
            ppfd->dwFlags,
            ppfd->cColorBits, ppfd->cDepthBits, ppfd->cStencilBits,
            ppfd->cRedBits, ppfd->cGreenBits, ppfd->cBlueBits, ppfd->cAlphaBits,
            ppfd->cAlphaShift, ppfd->cRedShift, ppfd->cGreenShift, ppfd->cBlueShift);
    }
    return 1;
}

void MGLActivateHandler(int i)
{
    static int last = 0;

    if (i != last) {
        last = i;
        if (GLFuncTrace())
            DPRINTF("wm_activate %d%-32s", i," ");
        switch (i) {
            case WA_ACTIVE:
                mesa_enabled_set();
                break;
            case WA_INACTIVE:
                mesa_enabled_reset();
                break;
        }
    }
}

int NumPbuffer(void)
{
    int i, c;
    for (i = 0, c = 0; i < MAX_PBUFFER;)
        if (hPbuffer[i++]) c++;
    return c;
}

static int LookupAttribArray(const int *attrib, const int attr)
{
    int ret = 0;
    for (int i = 0; (attrib[i] && attrib[i+1]); i+=2) {
        if (attrib[i] == attr) {
            ret = attrib[i+1];
            break;
        }
    }
    return ret;
}

void MGLFuncHandler(const char *name)
{
    char fname[64];
    uint32_t *argsp = (uint32_t *)(name + ALIGNED(strnlen(name, sizeof(fname))));
    strncpy(fname, name, sizeof(fname));

#define FUNCP_HANDLER(a) \
    if (!memcmp(fname, a, sizeof(a)))

    FUNCP_HANDLER("wglShareLists") {
        uint32_t i, ret = 0;
        i = argsp[1] & (MAX_PBUFFER - 1);
        if (((argsp[0] == MESAGL_MAGIC) && (argsp[1] == ((MESAGL_MAGIC & 0xFFFFFFFU) << 4 | i))) &&
            (hRC[0] && hPBRC[i]))
            ret = wglFuncs.ShareLists(hRC[0], hPBRC[i]);
        else {
            DPRINTF("  *WARN* ShareLists called with unknown contexts, %x %x", argsp[0], argsp[1]);
        }
        argsp[0] = ret;
        return;
    }
    FUNCP_HANDLER("wglUseFontBitmapsA") {
        uint32_t ret;
        ret = wglFuncs.UseFontBitmapsA(hDC, argsp[1], argsp[2], argsp[3]);
        argsp[0] = ret;
        return;
    }
    FUNCP_HANDLER("wglSwapIntervalEXT") {
        if (wglFuncs.SwapIntervalEXT) {
            uint32_t ret, err;
            int curr = wglFuncs.GetSwapIntervalEXT();
            if (curr != argsp[0]) {
                ret =  wglFuncs.SwapIntervalEXT(argsp[0]);
                err = (ret)? 0:GetLastError();
                DPRINTF("wglSwapIntervalEXT(%u) %s %-24u", argsp[0], ((ret)? "ret":"err"), ((ret)? ret:err));
            }
            else {
                ret = 1;
                DPRINTF("wglSwapIntervalEXT(%u) curr %d ret %-24u", argsp[0], curr, ret);
            }
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglGetSwapIntervalEXT") {
        if (wglFuncs.GetSwapIntervalEXT) {
            uint32_t ret;
            ret = wglFuncs.GetSwapIntervalEXT();
            DPRINTF("wglGetSwapIntervalEXT() ret %-24u", ret);
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglGetExtensionsStringARB") {
        if (1 /* wglFuncs.GetExtensionsStringARB */) {
            //const char *str = wglFuncs.GetExtensionsStringARB(hDC);
            const char *tmp = "WGL_3DFX_gamma_control "
                "WGL_ARB_create_context "
                "WGL_ARB_create_context_profile "
                "WGL_ARB_extensions_string "
                "WGL_ARB_multisample "
                "WGL_ARB_pbuffer "
                "WGL_ARB_pixel_format "
                "WGL_ARB_render_texture "
                "WGL_EXT_extensions_string "
                "WGL_EXT_swap_control";
            strncpy((char *)name, tmp, TARGET_PAGE_SIZE);
            //DPRINTF("WGL extensions\nHost: %s [ %d ]\nGuest: %s [ %d ]", str, (uint32_t)strlen(str), name, (uint32_t)strlen(name));
            return;
        }
    }
    FUNCP_HANDLER("wglCreateContextAttribsARB") {
        if (wglFuncs.CreateContextAttribsARB) {
            uint32_t i, ret;
            for (i = 0; ((i < MAX_LVLCNTX) && hRC[i]); i++);
            argsp[1] = (argsp[0])? i:0;
            if (argsp[1] == 0) {
                wglFuncs.MakeCurrent(NULL, NULL);
                for (i = MAX_LVLCNTX; i > 0;) {
                    if (hRC[--i]) {
                        wglFuncs.DeleteContext(hRC[i]);
                        hRC[i] = 0;
                    }
                }
                hRC[0] = wglFuncs.CreateContextAttribsARB(hDC, 0, (const int *)&argsp[2]);
                ret = (hRC[0])? 1:0;
            }
            else {
                if (i == MAX_LVLCNTX) {
                    wglFuncs.DeleteContext(hRC[1]);
                    for (i = 1; i < (MAX_LVLCNTX - 1); i++)
                        hRC[i] = hRC[i + 1];
                }
                hRC[i] = wglFuncs.CreateContextAttribsARB(hDC, hRC[i-1], (const int *)&argsp[2]);
                ret = (hRC[i])? 1:0;
            }
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglGetPixelFormatAttribfvARB") {
        BOOL (__stdcall *fp)(HDC, int, int, UINT, const int *, float *) =
            (BOOL (__stdcall *)(HDC, int, int, UINT, const int *, float *)) MesaGLGetProc(fname);
        if (fp) {
            uint32_t ret;
            float pf[64], n = argsp[2];
            ret = fp(hDC, argsp[0], argsp[1], argsp[2], (const int *)&argsp[4], pf);
            if (ret)
                memcpy(&argsp[2], pf, n*sizeof(float));
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglGetPixelFormatAttribivARB") {
        if (wglFuncs.GetPixelFormatAttribivARB) {
            uint32_t ret;
            int pi[64], n = argsp[2];
            ret = wglFuncs.GetPixelFormatAttribivARB(hDC, argsp[0], argsp[1], argsp[2], (const int *)&argsp[4], pi);
            if (ret)
                memcpy(&argsp[2], pi, n*sizeof(int));
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglChoosePixelFormatARB") {
        if (wglFuncs.ChoosePixelFormatARB) {
            const int *ia = (const int *)argsp;
            if (LookupAttribArray(ia, WGL_DRAW_TO_PBUFFER_ARB)) {
                int piFormats[64]; UINT nNumFormats;
                float fa[] = {0,0};
                wglFuncs.ChoosePixelFormatARB(hDC, ia, fa, 64, piFormats, &nNumFormats);
                argsp[1] = (nNumFormats)? piFormats[0]:0;
            }
            else {
                DPRINTF("wglChoosePixelFormatARB()");
                argsp[1] = MGLChoosePixelFormat();
            }
            argsp[0] = 1;
            return;
        }
    }
    FUNCP_HANDLER("wglBindTexImageARB") {
        BOOL (__stdcall *fp)(HPBUFFERARB, int) =
            (BOOL (__stdcall *)(HPBUFFERARB, int)) MesaGLGetProc(fname);
        if (fp) {
            uint32_t i, ret;
            i = argsp[0] & (MAX_PBUFFER - 1);
            ret = (hPbuffer[i])? fp(hPbuffer[i], argsp[1]):0;
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglReleaseTexImageARB") {
        BOOL (__stdcall *fp)(HPBUFFERARB, int) =
            (BOOL (__stdcall *)(HPBUFFERARB, int)) MesaGLGetProc(fname);
        if (fp) {
            uint32_t i, ret;
            i = argsp[0] & (MAX_PBUFFER - 1);
            ret = (hPbuffer[i])? fp(hPbuffer[i], argsp[1]):0;
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglSetPbufferAttribARB") {
        BOOL (__stdcall *fp)(HPBUFFERARB, const int *) =
            (BOOL (__stdcall *)(HPBUFFERARB, const int *)) MesaGLGetProc(fname);
        if (fp) {
            uint32_t i, ret;
            i = argsp[0] & (MAX_PBUFFER - 1);
            ret = (hPbuffer[i])? fp(hPbuffer[i], (const int *)&argsp[2]):0;
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglCreatePbufferARB") {
        HPBUFFERARB (__stdcall *fp)(HDC, int, int, int, const int *) =
            (HPBUFFERARB (__stdcall *)(HDC, int, int, int, const int *)) MesaGLGetProc(fname);
        HDC (__stdcall *fpDC)(HPBUFFERARB) =
            (HDC (__stdcall *)(HPBUFFERARB)) MesaGLGetProc("wglGetPbufferDCARB");
        if (fp && fpDC) {
            uint32_t i;
            i = 0; while(hPbuffer[i]) i++;
            if (i == MAX_PBUFFER) {
                DPRINTF("MAX_PBUFFER reached %d", i);
                argsp[0] = 0;
                return;
            }
            hPbuffer[i] = fp(hDC, argsp[0], argsp[1], argsp[2], (const int *)&argsp[4]);
            hPBDC[i] = fpDC(hPbuffer[i]);
            argsp[0] = (hPbuffer[i] && hPBDC[i])? 1:0;
            argsp[1] = i;
            return;
        }
    }
    FUNCP_HANDLER("wglDestroyPbufferARB") {
        BOOL (__stdcall *fp)(HPBUFFERARB) =
            (BOOL (__stdcall *)(HPBUFFERARB)) MesaGLGetProc(fname);
        int (__stdcall *fpDC)(HPBUFFERARB, HDC) =
            (int (__stdcall *)(HPBUFFERARB, HDC)) MesaGLGetProc("wglReleasePbufferDCARB");
        if (fp && fpDC) {
            uint32_t i, ret;
            i = argsp[0] & (MAX_PBUFFER - 1);
            wglFuncs.DeleteContext(hPBRC[i]);
            fpDC(hPbuffer[i], hPBDC[i]);
            ret = fp(hPbuffer[i]);
            hPbuffer[i] = 0; hPBDC[i] = 0; hPBRC[i] = 0;
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglQueryPbufferARB") {
        BOOL (__stdcall *fp)(HPBUFFERARB, int, int *) =
            (BOOL (__stdcall *)(HPBUFFERARB, int, int *)) MesaGLGetProc(fname);
        if (fp) {
            uint32_t i, ret;
            i = argsp[0] & (MAX_PBUFFER - 1);
            ret = (hPbuffer[i])? fp(hPbuffer[i], argsp[1], (int *)&argsp[2]):0;
            argsp[0] = ret;
            return;
        }
    }
    FUNCP_HANDLER("wglGetDeviceGammaRamp3DFX") {
        uint32_t ret;
        ret = GetDeviceGammaRamp(hDC, &argsp[2]);
        argsp[0] = ret;
        return;
    }
    FUNCP_HANDLER("wglSetDeviceGammaRamp3DFX") {
        uint32_t ret;
        ret = SetDeviceGammaRamp(hDC, &argsp[0]);
        argsp[0] = ret;
        return;
    }

    DPRINTF("  *WARN* Unhandled GLFunc %s", name);
    argsp[0] = 0;
}

#endif //CONFIG_WIN32

typedef struct {
    uint64_t last;
    uint32_t fcount;
    float ftime;
} STATSFX, * PSTATSFX;

static STATSFX fxstats = { .last = 0 };

static void profile_dump(void)
{
    PSTATSFX p = &fxstats;
    if (p->last) {
	p->last = 0;
	fprintf(stderr, "%-4u frames in %-4.1f seconds, %-4.1f FPS%-8s\r", p->fcount, p->ftime, (p->fcount / p->ftime), " ");
        fflush(stderr);
    }
}

static void profile_last(void)
{
    PSTATSFX p = &fxstats;
    if (p->last) {
	p->last = 0;
	fprintf(stderr, "%-64s\r", " ");
    }
}

#ifndef NANOSECONDS_PER_SECOND
#define NANOSECONDS_PER_SECOND get_ticks_per_sec()
#endif

static void profile_stat(void)
{
    uint64_t curr;
    int i;

    PSTATSFX p = &fxstats;

    if (p->last == 0) {
	p->fcount = 0;
	p->ftime = 0;
	p->last = get_clock();
	return;
    }

    curr = get_clock();
    p->fcount++;
    p->ftime += (curr - p->last) * (1.0f /  NANOSECONDS_PER_SECOND);
    p->last = curr;

    i = (GLFifoTrace() || GLFuncTrace())? 0:((int) p->ftime);
    if (i && ((i % 5) == 0))
	profile_dump();
}

void mesastat(PPERFSTAT s)
{
    s->stat = &profile_stat;
    s->last = &profile_last;
}
