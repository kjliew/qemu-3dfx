WATCOM_PATH=/opt/watcom11/bin
WCC=$(WATCOM_PATH)/wcc386
WLD=$(WATCOM_PATH)/wlink
QEMU_SRC_DIR=..\..\..\qemu-0
INCLUDES=-I$(QEMU_SRC_DIR)\hw\3dfx -I..\src

all: glide2x.ovl

glide2x.ovl: glideovl.obj
	@$(WLD) @$(<:obj=lnk)
	@if [ -d ../build ]; then cp -v $@ ../build; fi

glideovl.obj: glideovl.c
	@git rev-parse HEAD | sed "s/\(.......\).*/\#define\ __REV__\ \"\1\-\"/" > stamp.h
	@$(WCC) $(INCLUDES) -we -6s -ohtx -bd -fpi87 $<
	@rm stamp.h

clean:
	@rm -f *.map *.obj *.err *.ovl
	
distclean: clean
	@rm -f ../build/*.ovl
