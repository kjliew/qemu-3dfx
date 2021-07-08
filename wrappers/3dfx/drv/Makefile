FXLIB=../../fxlib
CROSS=i686-w64-mingw32-
CC=$(CROSS)gcc

all: instdrv.exe fxmemmap.vxd fxptl.sys md5sum

instdrv.exe: instdrv.c
	@if [ ! -d ../build ]; then exit 1; fi
	@echo "  LD  $@"
	@$(CC) -I$(FXLIB) -Wall -Wextra -Werror -s -o ../build/$@ $< ../build/fxlibnt.o

fxmemmap.vxd: fxmemmap.xxd
	@echo "  DRV $@"
	@xxd -r $< ../build/$@

fxptl.sys: fxptl.xxd
	@echo "  DRV $@"
	@xxd -r $< ../build/$@

md5sum: md5sum.txt
	@md5sum ../build/fxmemmap.vxd ../build/fxptl.sys | sed "s/.\.\.\/build\///" | diff - $<
