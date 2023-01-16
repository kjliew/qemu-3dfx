#include <windows.h>
#include "fxlib.h"


static FxU32 pciErrorCode;
static HANDLE hMemmapFile;

static FxBool fxlibFini(void)
{
  FxBool
    retVal = (hMemmapFile != INVALID_HANDLE_VALUE);

  if (retVal) CloseHandle( hMemmapFile );
  return retVal;
}

static FxBool fxlibInit(void)
{
  hMemmapFile = CreateFile("\\\\.\\FXMEMMAP.VXD", 0, 0, NULL, 0,
                           FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if ( hMemmapFile == INVALID_HANDLE_VALUE ) {
    pciErrorCode = PCI_ERR_MEMMAPVXD;
    return FXFALSE;
  }

  return FXTRUE;
}

static FxBool
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

static FxBool
fxUnmapLinear(FxU32 linear_addr, FxU32 length)
{
  FxU32 nret;

  return DeviceIoControl(hMemmapFile, DECREMENTMUTEX,
                         NULL, 0,
                         NULL, 0,
                         &nret, NULL);
}

static FxBool
fxGetMSR(MSRInfo* in, MSRInfo* out)
{
  FxU32 nret;

  return DeviceIoControl( hMemmapFile, GETMSR,
                          in, sizeof(*in),
                          out, sizeof(*out),
                          &nret, NULL);
}

static FxBool
fxSetMSR(MSRInfo* in, MSRInfo* out)
{
  FxU32 nret;

  return DeviceIoControl( hMemmapFile, SETMSR,
                          in, sizeof(*in),
                          out, sizeof(*out),
                          &nret, NULL);
}

/* Ganked from vmm.h */
#define PC_USER         0x00040000  /* make the pages ring 3 accessible */

static FxBool
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

void vxdDrvInit(PDRVFUNC drv)
{
    drv->Init = &fxlibInit;
    drv->Fini = &fxlibFini;
    drv->MapLinear = &fxMapLinear;
    drv->UnmapLinear = &fxUnmapLinear;
    drv->GetMSR = &fxGetMSR;
    drv->SetMSR = &fxSetMSR;
    drv->SetPermission = &fxSetPermission;
}
