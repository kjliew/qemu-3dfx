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

#include "mglfuncs.h"
#include "mglmapbo.h"


typedef struct _bufobj {
    mapbufo_t bo;
    struct _bufobj *next;
} MAPBO, * PMAPBO;

static PMAPBO pbufo = NULL;

void InitBufObj(void)
{
    PMAPBO p = pbufo;
    while (p) {
        PMAPBO next = p->next;
        g_free(p);
        p = next;
    }
    pbufo = p;
}

mapbufo_t *LookupBufObj(const int idx)
{
    PMAPBO p = pbufo;

    while(p) {
        if ((idx == p->bo.idx) || (p->next == NULL))
            break;
        p = p->next;
    }

    if (p == NULL) {
        p = g_new0(MAPBO, 1);
        p->bo.idx = idx;
        pbufo = p;
    }
    else {
        if (idx == p->bo.idx) { }
        else {
            p->next = g_new0(MAPBO, 1);
            p = p->next;
            p->bo.idx = idx;
        }
    }
    return &p->bo;
}

int FreeBufObj(const int idx)
{
    PMAPBO prev = pbufo, curr = pbufo;
    int cnt = 0;
    while (curr) {
        if (idx == curr->bo.idx) {
            if (pbufo == curr)
                pbufo = curr->next;
            else
                prev->next = curr->next;
            g_free(curr);
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    curr = pbufo;
    while (curr) {
        cnt++;
        curr = curr->next;
    }
    return cnt;
}

int MapBufObjGpa(mapbufo_t *bufo)
{
    PMAPBO curr = pbufo;
    int ret = 0;

    bufo->gpa = bufo->hva & (MBUFO_SIZE - 1);
    if (bufo != &curr->bo) {
        uintptr_t addr_lo = MBUFO_SIZE - 1, addr_hi = 0;
        uint32_t bufo_sz = ALIGNBO(bufo->mapsz) + (uint32_t)(bufo->hva & (qemu_real_host_page_size() - 1));
        while (bufo != &curr->bo) {
            uint32_t curr_sz = curr->bo.mapsz + (uint32_t)(curr->bo.hva & (qemu_real_host_page_size() - 1));
            addr_lo = ((curr->bo.gpa & qemu_real_host_page_mask()) < addr_lo)?
                (curr->bo.gpa & qemu_real_host_page_mask()):addr_lo;
            addr_hi = (((curr->bo.gpa + curr_sz) & qemu_real_host_page_mask()) > addr_hi)?
                ((curr->bo.gpa + curr_sz) & qemu_real_host_page_mask()):addr_hi;
            curr = curr->next;
            ret++;
        }
        if (((bufo->gpa + bufo_sz) < addr_lo) || (bufo->gpa >= addr_hi))
            return ret;
        bufo->gpa = 0;
        if (!bufo->gpa && (addr_lo > bufo_sz))
            bufo->gpa = addr_lo - bufo_sz;
        if (!bufo->gpa && ((addr_hi + bufo_sz) < MBUFO_SIZE))
            bufo->gpa = addr_hi;
    }
    return ret;
}

