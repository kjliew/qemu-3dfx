FXLIB=../../fxlib
CROSS?=
CC=$(CROSS)gcc
OUTDIR?=build

all: instdrv.exe fxmemmap.vxd fxptl.sys md5sum

instdrv.exe: instdrv.c
	@if [ ! -d ../$(OUTDIR) ]; then exit 1; fi
	@echo "  LD  $@"
	@$(CC) -I$(FXLIB) -Wall -Wextra -Werror -s -o ../$(OUTDIR)/$@ $< ../$(OUTDIR)/fxlibnt.o

fxmemmap.vxd: fxmemmap.xxd
	@echo "  DRV $@"
	@xxd -r $< ../$(OUTDIR)/$@

fxptl.sys: fxptl.xxd
	@echo "  DRV $@"
	@xxd -r $< ../$(OUTDIR)/$@

md5sum: md5sum.txt
	@md5sum ../$(OUTDIR)/fxmemmap.vxd ../$(OUTDIR)/fxptl.sys | sed "s/.\.\.\/$(OUTDIR)\///" | diff - $<
