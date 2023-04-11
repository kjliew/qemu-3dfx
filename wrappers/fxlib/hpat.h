#ifndef __HPAT_H
#define __HPAT_H

enum {
    HP_NONE,
    HP_98ME,
    HP_2KXP,
    HP_ANYO,
    HP_DONE,
};
struct E_PATCH {
    int offs, len;
    char *cb;
};
#define E_PATCH_END() { 0, 0 }

typedef struct fxCompatTbl {
    char *modName, *md5;
    int op_mask;
    struct E_PATCH *ptr;
} COMPATFX, * PCOMPATFX;

const PCOMPATFX fxCompatTblPtr(void);
#endif //__HPAT_H
