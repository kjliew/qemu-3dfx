QEMU_SRC_DIR=../../qemu-0
GIT=$(shell git rev-parse HEAD | sed "s/\(.......\).*/\1\-/")
CROSS=i686-pc-msdosdjgpp
CC=$(CROSS)-gcc
CFLAGS=-I$(QEMU_SRC_DIR)/hw/3dfx -D__REV__=\"$(GIT)\" -Werror -O3 -fomit-frame-pointer
DXEGEN=dxe3gen

TARGET2=glide2x.dxe
TARGET3=glide3x.dxe

all: $(TARGET2) $(TARGET3)

$(TARGET3): gl301dxe.o
	@echo "  CFLAGS $(CFLAGS)"
	@echo "  DXE $@"
	@DXE_LD_LIBRARY_PATH=$(shell dirname `which $(CC)`)/../$(CROSS)/lib \
	    $(DXEGEN) -o $@ -Y libfxgl3.a -E _gr -E _gu $+ -lc -Map glide3x.map
	@if [ -d ../build ]; then cp -v libfxgl3.a $(TARGET3) ../build; fi

$(TARGET2): glidedxe.o
	@echo "  CFLAGS $(CFLAGS)"
	@echo "  DXE $@"
	@DXE_LD_LIBRARY_PATH=$(shell dirname `which $(CC)`)/../$(CROSS)/lib \
	    $(DXEGEN) -o $@ -Y libfxgl2.a -E _Convert -E _gr -E _gu $+ -lc -Map glide2x.map
	@if [ -d ../build ]; then cp -v libfxgl2.a $(TARGET2) ../build; fi

%.o: %.c clib.h
	@echo "  CC  $@"
	@$(CC) -I../src $(CFLAGS) -c -o $@ $<

clean:
	@rm -f *.o *.a *.map $(TARGET2) $(TARGET3)

distclean: clean
	@rm -f ../build/*.dxe
