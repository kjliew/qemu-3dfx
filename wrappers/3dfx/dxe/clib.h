#ifndef CLIB_H
#define CLIB_H

#include <crt0.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/exceptn.h>
#include <sys/nearptr.h>
#include <sys/segments.h>
#include <sys/farptr.h>

typedef unsigned char uint8_t;
typedef unsigned uint32_t;
typedef unsigned long FxU32;
typedef int FxBool;

void _dxe_putc(int c) { outportb(0x3f8, (c & 0xFF)); }

static int open(const char *path)
{
    int retval;

    int dos_segment, dos_selector;

    if ((dos_segment=__dpmi_allocate_dos_memory((0x40 >> 4), &dos_selector)) == -1)
        return -1;
    movedata(_my_ds(), (unsigned int)path, dos_selector, 0, 0x40); 

    __dpmi_regs r;

    r.x.ax = 0x3D00;
    r.x.dx = 0;
    r.x.ds = dos_segment;
    __dpmi_int(0x21, &r);
    if ( r.x.flags & 1 )
        retval = -1;
    retval = r.x.ax;

    __dpmi_free_dos_memory(dos_selector);
    DPRINTF("%s opened handle %d\r\n", path, retval);

    return retval;
}

static int close(int handle)
{
    __dpmi_regs r;

    r.h.ah = 0x3E;
    r.x.bx = handle;
    __dpmi_int(0x21, &r);
    if ( r.x.flags & 1 )
        return -1;
    return 0;
}

static int fsize(int handle)
{
    int retval;
    __dpmi_regs r;

    r.x.ax = 0x4202;
    r.x.bx = handle;
    r.x.cx = 0;
    r.x.dx = 0;
    __dpmi_int(0x21, &r);

    retval = ((int)r.x.dx << 0x10) + r.x.ax;

    r.x.ax = 0x4200;
    r.x.bx = handle;
    r.x.cx = 0;
    r.x.dx = 0;
    __dpmi_int(0x21, &r);

    DPRINTF("handle %d fsize %x\r\n", handle, retval);

    return retval;
}

static unsigned int _dxe_read(int handle, void *buffer, unsigned int count, unsigned int *result)
{
  __dpmi_regs r;
  int dos_segment;
  int dos_selector;
  unsigned int dos_buffer_size, read_size;
  unsigned char *p_buffer;

  /* Allocate ~64K or less transfer buffer from DOS */
  dos_buffer_size = ( count < 0xFFE0 ) ? count : 0xFFE0;
  if ( (dos_segment=__dpmi_allocate_dos_memory((dos_buffer_size + 15) >> 4, &dos_selector)) == -1 )
  {
    return 8;
  }

  /* Reads blocks of file and transfers these into user buffer. */
  p_buffer = buffer;
  *result  = 0;
  while( count )
  {
    read_size = ( count < dos_buffer_size ) ? count : dos_buffer_size;
    r.h.ah = 0x3F;
    r.x.bx = handle;
    r.x.cx = read_size;
    r.x.ds = dos_segment;
    r.x.dx = 0;
    __dpmi_int(0x21, &r);
    if ( r.x.flags & 1 )
    {
      __dpmi_free_dos_memory(dos_selector);
      return r.x.ax;
    }
    if ( r.x.ax )
      movedata(dos_selector, 0, _my_ds(), (unsigned int)p_buffer, r.x.ax);
    count    -= read_size;
    p_buffer += r.x.ax;
    *result  += r.x.ax;
    DPRINTF("read chunk %x\r\n", read_size);
  }

  /* Frees allocated DOS transfer buffer. */
  __dpmi_free_dos_memory(dos_selector);
  return 0;
}


static int read(int handle, void *buf, unsigned size)
{
    int retval, err;
    (void)err;

    err = _dxe_read(handle, buf, size, (unsigned int *)&retval);
    DPRINTF("_dxe_read %d\r\n", err);

    return retval;
}

static INLINE uint32_t f2u32(const float f)
{
    uint32_t u32;
    char *s = (char *)&f;
    char *d = (char *)&u32;
    
    *d++ = *s++;
    *d++ = *s++;
    *d++ = *s++;
    *d++ = *s++;

    return u32;
}

static int *_ptr_djgpp_base_address;
static int *_ptr_djgpp_selector_limit;
static unsigned short *_ptr_djgpp_ds_alias;
static int *_ptr_crt0_startup_flags;

static void _dxe_sync(void)
{
    *_ptr_crt0_startup_flags = _crt0_startup_flags;
}

static int _dxe_crt0(void)
{
    unsigned long p;

    for (p = 0x1000; p < 0x4000; p+=4) {
        if ((0xd189cb8c == _farpeekl(_my_cs(), p)) &&
            (0xcd10e9c1 == _farpeekl(_my_cs(), (p + 0x04))))
            break;
    }

    if (0x4000 == p)
        return 1;

    _ptr_djgpp_base_address   = (int *)_farpeekl(_my_cs(), (p + 0x33));
    _ptr_djgpp_selector_limit = (int *)_farpeekl(_my_cs(), (p + 0x42));
    _ptr_djgpp_ds_alias       = (unsigned short *)_farpeekl(_my_cs(), (p + 0x21));
    _ptr_crt0_startup_flags   = (int *)_farpeekl(_my_cs(), (p + 0x0b));

    __djgpp_base_address   = *_ptr_djgpp_base_address;
    __djgpp_selector_limit = *_ptr_djgpp_selector_limit;
    __djgpp_ds_alias       = *_ptr_djgpp_ds_alias;
    _crt0_startup_flags    = *_ptr_crt0_startup_flags;

    return 0;
}

static void *MapPhysToLin(void *physaddr, unsigned int size)
{
    void *linaddr;

    __dpmi_meminfo mi;
    mi.address = (unsigned long)physaddr;
    mi.size = size;

    if (_dxe_crt0())
        return NULL;
    if ((_crt0_startup_flags & _CRT0_FLAG_NEARPTR) == 0) {
        if (__djgpp_nearptr_enable() == 0)
            return NULL;
    }
    _dxe_sync();
    
    if (__dpmi_physical_address_mapping(&mi))
        DPRINTF("Map PA 0x%x failed\r\n", mi.address);

    if (__dpmi_lock_linear_region(&mi))
        DPRINTF("Lock region failed VA 0x%x\r\n", mi.address);

    linaddr = (void *)(mi.address + __djgpp_conventional_base);

    return linaddr;
}

static FxBool fxMapLinear(FxU32 busNumber, FxU32 physical_addr, FxU32 *linear_addr, FxU32 *length)
{
    *linear_addr = (FxU32)MapPhysToLin((void *)physical_addr, *length);
    if (*linear_addr == 0)
        return 0;
    return 1;
}

#endif //CLIB_H

