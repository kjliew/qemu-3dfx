#ifndef CLIB_H
#define CLIB_H

#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef unsigned long FxU32;
typedef int FxBool;

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

static int fd = -1;
static int init_fd(void)
{
    if (fd == -1)
        fd = open("/dev/mem", O_RDWR|O_SYNC);
    if (fd == -1) {
        fprintf(stderr, "Error: /dev/mem open failed\n");
        return 1;
    }
    return 0;
}

static void *MapPhysToLin(void *physaddr, unsigned int size)
{
    off_t offset = (off_t)physaddr;
    size_t len = size;

    // Truncate offset to a multiple of the page size, or mmap will fail.
    size_t pagesize = sysconf(_SC_PAGE_SIZE);
    off_t page_base = (offset / pagesize) * pagesize;
    off_t page_offset = offset - page_base;

    unsigned char *mem = mmap(NULL, page_offset + len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_base);
    if (mem == MAP_FAILED) {
        DPRINTF("Error: mmap() failed\n");
        return 0;
    }

    return mem;
}

static FxBool fxMapLinear(FxU32 busNumber, FxU32 physical_addr, FxU32 *linear_addr, FxU32 *length)
{
    *linear_addr = (FxU32)MapPhysToLin((void *)physical_addr, *length);
    if (*linear_addr == 0)
        return 0;
    return 1;
}

#endif //CLIB_H

