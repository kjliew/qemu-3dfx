#ifndef CLIB_H
#define CLIB_H

#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned uint32_t;
typedef unsigned long FxU32;
typedef int FxBool;

void _dxe_putc(int c)
{
    __asm
    {
        mov dx, 0x3F8;
        mov eax, c;
        out dx, al;
    }
}

static unsigned getTickAcpi(void)
{
    static unsigned tick;
    unsigned tickAcpi;

    __asm
    {
        mov dx, 0x608;
        in eax, dx;
        mov tickAcpi, eax;
    }

    if ((tick & 0x00FFFFFFU) > tickAcpi)
        tick = (((tick >> 24) + 1) << 24) | tickAcpi;
    else
        tick = (tick & 0xFF000000U) | tickAcpi;
#define TICK_ACPI 0x369E99U /* 3.579545 MHz */
    return tick;
}

static unsigned getDosPSPSeg(void)
{
    unsigned segPSP;

    __asm
    {
	mov ah, 0x62; /* Get PSP selector */
	int 0x21;
	mov ax, 0x06; /* Get selector base */
	int 0x31;
	movzx eax, cx;
	shl eax, 0x10;
	mov ax, dx;
	mov segPSP, eax;
    }

    return segPSP;
}

static void memcpy(const void *s1, const void *s2, unsigned n)
{
    int i = 0;
    char *dst = (char *)s1, *src = (char *)s2;
    for (; i < n; i++)
        dst[i] = src[i];
}

static void memset(const void *s1, const char c, unsigned n)
{
    int i = 0;
    char *dst = (char *)s1;
    for (; i < n; i++)
        dst[i] = c;
}

static int strncmp(const char *s1, const char *s2, unsigned n)
{
    if (n == 0)
	return (0);
    do {
	if (*s1 != *s2++)
	    return (*(unsigned char *)s1 - *(unsigned char *)--s2);
	if (*s1++ == 0)
	    break;
    } while (--n != 0);
    return (0);
}

static int strnlen(const char *s, unsigned n)
{
    int i;
    if (n == 0)
        return (0);
    for (i = 0; i < n; i++) {
        if (s[i] == 0)
            break;
    }
    return i;
}

static char *basename(const char *name)
{
    int i = 0;
    char *p = (char *)name;
    while (name[i++]);
    for (--i; i > 0; i--) {
        if ((name[i] == '/') || (name[i] == '\\'))
            break;
    }
    return (i)? (p + i + 1):p;
}

static int open(const char *path)
{
    int retval, err;

    __asm
    {
	mov eax, 0x3d00;
	mov edx, path;
	int 0x21;
	mov retval, eax;
	setc al;
	movzx eax, al;
	mov err, eax
    }
    
    if (err)
	retval = -1;

    return retval;
}

static int close(int handle)
{
    int err;

    __asm
    {
	mov eax, 0x3e00;
	mov ebx, handle;
	int 0x21;
	setc al;
	movzx eax, al;
	mov err, eax;
    }

    if (err)
        return -1;
    return 0;
}

static int fsize(int handle)
{
    int retval;

    __asm
    {
	mov eax, 0x4202;
	mov ebx, handle;
	xor ecx, ecx;
	xor edx, edx;
	int 0x21;
	shl edx, 0x10;
	add eax, edx
	mov retval, eax;
	mov eax, 0x4200;
	xor ecx, ecx
	xor edx, edx;
	int 0x21;
    }

    return retval;
}

static int read(int handle, void *buf, unsigned size)
{
    int retval;

    __asm
    {
	mov eax, 0x3f00;
	mov ebx, handle;
	mov ecx, size;
	mov edx, buf;
	int 0x21;
	mov retval, eax;
    }

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

static void *MapPhysToLin(void *physaddr, unsigned int size)
{
	void *linaddr;
	unsigned int l = size;

	__asm
	{
		push ebx;
		push esi;
		push edi;
		mov bx, WORD PTR [physaddr + 2]
		mov cx, WORD PTR [physaddr]
		mov si, WORD PTR [l + 2]
		mov di, WORD PTR [l]

		// Call DPMI function MapPhysicalToLinear (0x800)
		mov ax, 800h
		int 31h

		jnc success
		xor bx, bx
		xor cx, cx
success:	
		mov WORD PTR [linaddr + 2], bx
		mov WORD PTR [linaddr], cx
		pop edi;
		pop esi;
		pop ebx;
	}

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

