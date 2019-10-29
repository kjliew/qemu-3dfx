#ifndef _SV_TRCKR_H
#define _SV_TRCKR_H

void *LookupGrState(uint32_t handle, int size);
void *LookupVtxLayout(uint32_t handle, int size);
int FreeGrState(void);
int FreeVtxLayout(void);

#endif //_SV_TRCKR_H

