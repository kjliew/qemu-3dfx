#include <windows.h>
#include "fxlib.h"

#ifdef __FXVXD__

static FxU32 pciErrorCode = 0;
static HANDLE hMemmapFile;

FxBool fxlibFini(void)
{
  FxBool
    retVal = (hMemmapFile != INVALID_HANDLE_VALUE);

  if (retVal) CloseHandle( hMemmapFile );
  return retVal;
}

FxBool fxlibInit(void)
{
  hMemmapFile = CreateFile("\\\\.\\FXMEMMAP.VXD", 0, 0, NULL, 0,
                           FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if ( hMemmapFile == INVALID_HANDLE_VALUE ) {
    pciErrorCode = PCI_ERR_MEMMAPVXD;
    return FXFALSE;
  }

  return FXTRUE;
}

FxBool
fxMapLinear(FxU32 busNumber, FxU32 physical_addr,
               FxU32 *linear_addr, FxU32 *length)
{
  FxU32 nret;
  FxU32 Physical [2];         /* Physical address[0] & size[1] */
  FxU32 Linear [2];           /* Linear address[0] & size[1] */
  LPDWORD pPhysical = Physical;
  LPDWORD pLinear = Linear;
  
  Physical[0] = physical_addr;
  Physical[1] = *length;
  
    DeviceIoControl(hMemmapFile, GETLINEARADDR, 
                    &pPhysical, sizeof(pPhysical), 
                    &pLinear, sizeof(pLinear), 
                    &nret, NULL);
  
  *linear_addr = Linear[0];
  
  if ( nret == 0 ) {
    pciErrorCode = PCI_ERR_MEMMAP;
    return FXFALSE;
  }

  return FXTRUE;
}

FxBool
fxUnmapLinear(FxU32 linear_addr, FxU32 length)
{
    return FXTRUE;
}

/* Ganked from vmm.h */
#define PC_USER         0x00040000  /* make the pages ring 3 accessible */

FxBool
fxSetPermission(const FxU32 addrBase, const FxU32 addrLen,
                   const FxBool writePermP)
{
  FxU32 vxdParamArray[] = {
    addrBase,
    addrLen,
    0
  };
  FxU32 nRet = 0;
  
  /* Set the user accessable bit. We don't dork w/ the
   * rest of the bits.
   */
  vxdParamArray[2] = (writePermP ? PC_USER : 0);
  
  return DeviceIoControl(hMemmapFile, SETADDRPERM,
                         vxdParamArray, sizeof(vxdParamArray),
                         NULL, 0,
                         &nRet, NULL);
}

#endif // __FXVXD__
