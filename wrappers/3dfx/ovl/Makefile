WATCOM_PATH=$(shell dirname `which wcc386`)
WCC=$(WATCOM_PATH)/wcc386
WLD=$(WATCOM_PATH)/wlink
QEMU_SRC_DIR=../../../qemu-0
INCLUDES=-I$(QEMU_SRC_DIR)/hw/3dfx -I../src
CFLAGS=-zq -we -6s -ohtx -bd -fpi87
OUTDIR?=build

all: glide2x.ovl

glide2x.ovl: glideovl.obj
	@echo "  CFLAGS  $(CFLAGS)"
	@echo "  WLD $@"
	@$(WLD) @$(<:obj=lnk)
	@if [ -d ../$(OUTDIR) ]; then cp -v $@ ../$(OUTDIR); fi

glideovl.obj: glideovl.c
	@git rev-parse HEAD | sed "s/\(.......\).*/\#define\ __REV__\ \"\1\-\"/" > stamp.h
	@echo "  WCC $@"
	@if [ x`uname | sed "s/_.*//"` == xMINGW32 ]; then \
		INCLUDES=`echo $(INCLUDES) | sed "s/\//\\\\\/g"`; \
		$(WCC) `echo $$INCLUDES` $(CFLAGS) $<; else \
		$(WCC) $(INCLUDES) $(CFLAGS) -fo=$@ $<; fi
	@rm stamp.h

clean:
	@rm -f *.map *.obj *.err *.ovl
	
distclean: clean
	@rm -f ../$(OUTDIR)/*.ovl
