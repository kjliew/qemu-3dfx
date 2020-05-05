/*
 * QEMU 3Dfx Glide Pass-Through
 *
 *  Copyright (c) 2018-2020
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

#ifndef GLIDEWND_H
#define GLIDEWND_H

int glide_lfbmerge(void);
int glide_lfbdirty(void);
int glide_lfbnoaux(void);
int glide_lfbmode(void);
void glide_winres(const int, uint32_t *, uint32_t *);
int stat_window(const int, const int);
uint32_t init_window(const int, const char *);
void fini_window(void);

typedef struct _perfstat {
    void (*stat)(void);
    void (*last)(void);
} PERFSTAT, *PPERFSTAT;

void glidestat(PPERFSTAT);

#endif // GLIDEWND_H
