#include <windows.h>
#include "fxlib.h"


static FxU32 pciErrorCode;
static HANDLE hMemmapFile;

static FxBool fxlibFini(void)
{
  if (hMemmapFile != INVALID_HANDLE_VALUE) CloseHandle(hMemmapFile);

  return FXTRUE;
}

static FxBool fxlibInit(void)
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

static FxBool
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

static FxBool
fxUnmapLinear(FxU32 linear_addr, FxU32 length)
{
  FxU32                cbReturned;
  
  return DeviceIoControl(hMemmapFile,
                         (FxU32)IOCTL_MAPMEM_UNMAP_USER_PHYSICAL_MEMORY,
                         &linear_addr, sizeof(PVOID),
                         NULL, 0,
                         &cbReturned, NULL);
}

static FxBool
fxGetMSR(MSRInfo* in, MSRInfo* out)
{
  ULONG retLen;

  return DeviceIoControl(hMemmapFile, (FxU32)IOCTL_MAPMEM_GET_MSR,
                         in, sizeof(*in),
                         out, sizeof(*out),
                         &retLen, NULL);
}

static FxBool
fxSetMSR(MSRInfo* in, MSRInfo* out)
{
  ULONG retLen;

  return DeviceIoControl(hMemmapFile, (FxU32)IOCTL_MAPMEM_SET_MSR,
                         in, sizeof(*in),
                         out, sizeof(*out),
                         &retLen, NULL);
}

static FxBool
fxSetPermission(const FxU32 addrBase, const FxU32 addrLen,
                   const FxBool writePermP)
{
  return FXFALSE;
}

void kmdDrvInit(PDRVFUNC drv)
{
    drv->Init = &fxlibInit;
    drv->Fini = &fxlibFini;
    drv->MapLinear = &fxMapLinear;
    drv->UnmapLinear = &fxUnmapLinear;
    drv->GetMSR = &fxGetMSR;
    drv->SetMSR = &fxSetMSR;
    drv->SetPermission = &fxSetPermission;
}
