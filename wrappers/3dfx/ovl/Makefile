WCC=$(WATCOM_PATH)/wcc386
WLD=$(WATCOM_PATH)/wlink
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
	@$(WCC) $(INCLUDES) $(CFLAGS) -fo=$@ $<
	@rm stamp.h

clean:
	@rm -f *.map *.obj *.err *.ovl
	
distclean: clean
	@rm -f ../$(OUTDIR)/*.ovl
