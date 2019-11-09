/*
 * QEMU 3Dfx Glide Pass-Through 
 *
 *  Copyright (c) 2018
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu-common.h"
#include "ui/console.h"

#include "glide2x_impl.h"

#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, " " fmt , ## __VA_ARGS__); fflush(stderr); } while(0)

#define GLIDECFG "glide.cfg"

struct tblGlideResolution {
    int w;
    int	h;
};

static struct tblGlideResolution tblRes[] = {
  { .w = 320, .h = 200   }, //0x0
  { .w = 320, .h = 240   }, //0x1
  { .w = 400, .h = 256   }, //0x2
  { .w = 512, .h = 384   }, //0x3
  { .w = 640, .h = 200   }, //0x4
  { .w = 640, .h = 350   }, //0x5
  { .w = 640, .h = 400   }, //0x6
  { .w = 640, .h = 480   }, //0x7
  { .w = 800, .h = 600   }, //0x8
  { .w = 960, .h = 720   }, //0x9
  { .w = 856, .h = 480   }, //0xa
  { .w = 512, .h = 256   }, //0xb
  { .w = 1024, .h = 768  }, //0xC
  { .w = 1280, .h = 1024 }, //0xD
  { .w = 1600, .h = 1200 }, //0xE
  { .w = 400, .h = 300   }, //0xF
  { .w = 0, .h = 0},
};

static uintptr_t hwnd = 0;
static int cfg_createWnd = 0;
static int cfg_scaleX = 0;
static int cfg_lfbHandler = 0;
static int cfg_lfbNoAux = 0;
static int cfg_lfbWriteMerge = 0;

#if defined(CONFIG_WIN32) && CONFIG_WIN32
static LONG WINAPI GlideWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
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

static HWND CreateGlideWindow(const char *title, int w, int h)
{
    HWND 	hWnd;
    WNDCLASS 	wc;
    static HINSTANCE hInstance = 0;

    if (!hInstance) {
	memset(&wc, 0, sizeof(WNDCLASS));
	hInstance = GetModuleHandle(NULL);
	wc.style	= CS_OWNDC;
	wc.lpfnWndProc	= (WNDPROC)GlideWndProc;
	wc.lpszClassName = "GlideWnd";

	if (!RegisterClass(&wc)) {
	    DPRINTF("RegisterClass() faled, Error %08lx\n", GetLastError());
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
	    "GlideWnd", title, 
	    WS_CAPTION | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
	    CW_USEDEFAULT, CW_USEDEFAULT, rect.right, rect.bottom,
	    NULL, NULL, hInstance, NULL);
    GetClientRect(hWnd, &rect);
    DPRINTF("    window %lux%lu\n", rect.right, rect.bottom);
    ShowCursor(FALSE);
    ShowWindow(hWnd, SW_SHOW);

    return hWnd;
}
#endif // defined(CONFIG_WIN32) && CONFIG_WIN32

static int scaledRes(int w, float r)
{
    int i;
    for (i = 0xE; i > 0x7; i--)
	if ((tblRes[i].w == w) && (((float)tblRes[i].h) / tblRes[i].w == r))
	    break;
    if (i == 0x7) {
        i = 0x10;
        tblRes[i].w = w; tblRes[i].h = (w * r);
    }
    return i;
}

int glide_lfbmerge(void) { return cfg_lfbWriteMerge; }
int glide_lfbnoaux(void) { return cfg_lfbNoAux; }
int glide_lfbmode(void) { return cfg_lfbHandler; }
void glide_winres(const int res, uint32_t *w, uint32_t *h)
{
    *w = tblRes[res].w;
    *h = tblRes[res].h;
}

int stat_window(const int res, const int activate)
{
    int stat, sel;
    sel = (cfg_scaleX)? scaledRes(cfg_scaleX, ((float)tblRes[res].h) / tblRes[res].w):res;
    stat = (cfg_createWnd)? 0:1;

    if (stat) {
	uint32_t wndStat = (glide_gui_fullscreen())?
            (((tblRes[sel].h & 0xFFFFU) << 0x10) | tblRes[sel].w) : glide_window_stat(activate);
	if (activate) {
	    if (wndStat == (((tblRes[sel].h & 0xFFFFU) << 0x10) | tblRes[sel].w)) {
		DPRINTF("    window %ux%u %s\n", (wndStat & 0xFFFFU), (wndStat >> 0x10), (cfg_scaleX)? "(scaled)":"");
		stat = 0;
	    }
	}
	else {
	    stat = wndStat;
#if defined(CONFIG_LINUX) && CONFIG_LINUX
            if (stat)
                glide_release_window();
#endif
            if (glide_gui_fullscreen())
                stat = 0;
        }
    }
    return stat;
}

void fini_window(void)
{
    if (hwnd) {
#if defined(CONFIG_WIN32) && CONFIG_WIN32	    
        if (cfg_createWnd)
            DestroyWindow((HWND)hwnd);
        else
            glide_release_window();
#endif
#if defined(CONFIG_LINUX) && CONFIG_LINUX
        glide_release_window();
#endif	    
    }
    hwnd = 0;
}

uint32_t init_window(const int res, const char *wndTitle)
{
    union {
	uint32_t u32;
	uintptr_t uptr;
    } cvHWnd;

    int sel;

    FILE *fp = fopen(GLIDECFG, "r");
    if (fp != NULL) {
        char line[32];
        while (fgets(line, 32, fp)) {
	    sscanf(line, "CreateWindow,%d", &cfg_createWnd);
	    sscanf(line, "ScaleWidth,%d", &cfg_scaleX);
            sscanf(line, "LfbHandler,%d", &cfg_lfbHandler);
            sscanf(line, "LfbNoAux,%d", &cfg_lfbNoAux);
            sscanf(line, "LfbWriteMerge,%d", &cfg_lfbWriteMerge);
	}
        fclose(fp);
    }
    else {
        cfg_createWnd = 0;
	cfg_scaleX = 0;
        cfg_lfbHandler = 0;
        cfg_lfbNoAux = 0;
        cfg_lfbWriteMerge = 0;
    }

    sel = res;
    if (cfg_scaleX) {
        sel = scaledRes(cfg_scaleX, ((float)tblRes[res].h) / tblRes[res].w);
        conf_glide2x(tblRes[sel].w);
    }
#if defined(CONFIG_WIN32) && CONFIG_WIN32	    
    if (cfg_createWnd)
        hwnd = (uintptr_t)CreateGlideWindow(wndTitle, tblRes[sel].w, tblRes[sel].h);
    else
        hwnd = glide_prepare_window(tblRes[sel].w, tblRes[sel].h);
#endif
#if defined(CONFIG_LINUX) && CONFIG_LINUX
    hwnd = glide_prepare_window(tblRes[sel].w, tblRes[sel].h);
    cfg_createWnd = (hwnd)? 0:1;
#endif	

    cvHWnd.uptr = (uintptr_t)hwnd;

    return cvHWnd.u32;
}

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
	DPRINTF("%-3u frames in %-4.1f seconds, %-4.1f FPS\r", p->fcount, p->ftime, (p->fcount / p->ftime));
    }
}

void profile_last(void)
{
    PSTATSFX p = &fxstats;
    if (p->last) {
	p->last = 0;
	DPRINTF("                                         \r");
    }
}

#ifndef NANOSECONDS_PER_SECOND
#define NANOSECONDS_PER_SECOND get_ticks_per_sec()
#endif

void profile_stat(void)
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

    i = (int) p->ftime;
    if (i && ((i % 5) == 0))
	profile_dump();
}

