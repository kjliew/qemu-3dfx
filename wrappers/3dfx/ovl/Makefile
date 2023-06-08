WATCOM_PATH=/opt/watcom/binl
WCC=$(WATCOM_PATH)/wcc386
WLD=$(WATCOM_PATH)/wlink
QEMU_SRC_DIR=../../../qemu-0
INCLUDES=-I$(QEMU_SRC_DIR)/hw/3dfx -I../src
CFLAGS=-zq -we -6s -ohtx -bd -fpi87
OUTDIR?=build

all: glide2x.ovl

glide2x.ovl: glideovl.o
	@echo "  CFLAGS  $(CFLAGS)"
	@echo "  WLD $@"
	@$(WLD) @$(<:o=lnk)
	@if [ -d ../$(OUTDIR) ]; then cp -v $@ ../$(OUTDIR); fi

glideovl.o: glideovl.c
	@git rev-parse HEAD | sed "s/\(.......\).*/\#define\ __REV__\ \"\1\-\"/" > stamp.h
	@echo "  WCC $@"
	@$(WCC) $(INCLUDES) $(CFLAGS) $<
	@rm stamp.h

clean:
	@rm -f *.map *.o *.err *.ovl
	
distclean: clean
	@rm -f ../$(OUTDIR)/*.ovl
