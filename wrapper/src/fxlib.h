#ifndef FXLIB_H
#define FXLIB_H

#define FXTRUE  1
#define FXFALSE 0

#define   GETLINEARADDR         2
#define   SETADDRPERM           10

#define PCI_ERR_MEMMAPVXD       2
#define PCI_ERR_MAPMEMDRV       3
#define PCI_ERR_MEMMAP          16
#define PCI_ERR_MAPMEM          17

typedef int FxBool;
typedef unsigned long FxU32;

#ifdef small
  /* MSYS/MinGW.org headers #define small char */
  #undef small
  #include <ddk/ntddk.h>
#endif

#ifdef __FXSYS__
#ifndef _DDK_NTDDK_H 
typedef enum _INTERFACE_TYPE {
  InterfaceTypeUndefined = -1,
  Internal,
  Isa,
  Eisa,
  MicroChannel,
  TurboChannel,
  PCIBus,
  VMEBus,
  NuBus,
  PCMCIABus,
  CBus,
  MPIBus,
  MPSABus,
  ProcessorInternal,
  InternalPowerBus,
  PNPISABus,
  PNPBus,
  Vmcs,
  MaximumInterfaceType
} INTERFACE_TYPE, *PINTERFACE_TYPE;
#endif //_DDK_NTDDK_H

#define FILE_DEVICE_MAPMEM  0x00008000
//
// Macro definition for defining IOCTL and FSCTL function control codes.  Note
// that function codes 0-2047 are reserved for Microsoft Corporation, and
// 2048-4095 are reserved for customers.
//
#define MAPMEM_IOCTL_INDEX  0x800

//
// Define our own private IOCTL
//
#define IOCTL_MAPMEM_MAP_USER_PHYSICAL_MEMORY   CTL_CODE(FILE_DEVICE_MAPMEM , \
                                                          MAPMEM_IOCTL_INDEX,  \
                                                          METHOD_BUFFERED,     \
                                                          FILE_ANY_ACCESS)

#define IOCTL_MAPMEM_UNMAP_USER_PHYSICAL_MEMORY CTL_CODE(FILE_DEVICE_MAPMEM,  \
                                                          MAPMEM_IOCTL_INDEX+1,\
                                                          METHOD_BUFFERED,     \
                                                          FILE_ANY_ACCESS)

typedef struct
{
    INTERFACE_TYPE InterfaceType; // Isa, Eisa, etc....
    ULONG BusNumber;              // Bus number
    LARGE_INTEGER BusAddress;     // Bus-relative address
    ULONG AddressSpace;           // 0 is memory, 1 is I/O
    ULONG Length;                 // Length of section to map

}
PHYSICAL_MEMORY_INFO, *PPHYSICAL_MEMORY_INFO;
#endif // __FXSYS__

FxBool fxlibInit(void);
FxBool fxlibFini(void);
FxBool fxMapLinear(FxU32 busNumber, FxU32 physical_addr, FxU32 *linear_addr, FxU32 *length);
FxBool fxUnmapLinear(FxU32 linear_addr, FxU32 length);
FxBool fxSetPermission(const FxU32 addrBase, const FxU32 addrLen, const FxBool writePermP);

#endif // FXLIB_H

