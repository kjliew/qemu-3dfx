QEMU_SRC_DIR=../../../qemu-0
GIT=$(shell git rev-parse --short HEAD | sed "s/\(.*\)/\1\-/")
CROSS=
CC=$(CROSS)gcc
CFLAGS=-I$(QEMU_SRC_DIR)/hw/3dfx -D__REV__=\"$(GIT)\" -Wall -Werror -O3 -fomit-frame-pointer
LDFLAGS=

TARGET2=libglide.so.2

all: $(TARGET2)

$(TARGET2): glidedso.o
	@echo "  CFLAGS $(CFLAGS)"
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) $(CFLAGS) -shared -Wl,-soname,$@ -o $@ $+

%.o: %.c clib.h
	@echo "  CC  $@"
	@$(CC) -I ../src $(CFLAGS) -c -o $@ $<

clean:
	@rm -f *.o *.a *.map 

distclean: clean
	@rm -f $(TARGET2)

