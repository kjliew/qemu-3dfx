#include <windows.h>
#include "fxlib.h"

#ifdef __FXSYS__

static FxU32 pciErrorCode = 0;
static HANDLE hMemmapFile;

FxBool fxlibFini(void)
{
  if (hMemmapFile != INVALID_HANDLE_VALUE) CloseHandle(hMemmapFile);

  return FXTRUE;
}

FxBool fxlibInit(void)
{
  hMemmapFile = INVALID_HANDLE_VALUE;

  hMemmapFile = CreateFile("\\\\.\\MAPMEM", 
                           GENERIC_READ | GENERIC_WRITE,
                           0, 
                           NULL, 
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, 
                           NULL);
  if (hMemmapFile == INVALID_HANDLE_VALUE) {
    pciErrorCode = PCI_ERR_MAPMEMDRV;
    return FXFALSE;
  }  
  
  return FXTRUE;
}

FxBool
fxMapLinear(FxU32 busNumber, FxU32 physical_addr,
               FxU32 *linear_addr, FxU32 *length)
{
  FxU32 cbReturned;
  PHYSICAL_MEMORY_INFO pmi;
  
  pmi.InterfaceType       = PCIBus;
  pmi.BusNumber           = busNumber;
  pmi.BusAddress.HighPart = 0x00000000;
  pmi.BusAddress.LowPart  = physical_addr;
  pmi.AddressSpace        = 0;
  pmi.Length              = *length;
  
  if(!DeviceIoControl(hMemmapFile,
                      (FxU32)IOCTL_MAPMEM_MAP_USER_PHYSICAL_MEMORY,
                      &pmi, sizeof(PHYSICAL_MEMORY_INFO),
                      linear_addr, sizeof(PVOID),
                      &cbReturned, NULL)) {
    pciErrorCode = PCI_ERR_MAPMEM;
    return FXFALSE;
  }

  return FXTRUE;
}

FxBool
fxUnmapLinear(FxU32 linear_addr, FxU32 length)
{
  FxU32                cbReturned;
  
  return DeviceIoControl(hMemmapFile,
                         (FxU32)IOCTL_MAPMEM_UNMAP_USER_PHYSICAL_MEMORY,
                         &linear_addr, sizeof(PVOID),
                         NULL, 0,
                         &cbReturned, NULL);
}

FxBool
fxSetPermission(const FxU32 addrBase, const FxU32 addrLen,
                   const FxBool writePermP)
{
  return FXFALSE;
}

#endif // __FXSYS__
