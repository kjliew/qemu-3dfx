QEMU_SRC_DIR=../../../qemu-0
CC=gcc
CFLAGS=-I$(QEMU_SRC_DIR)/hw/3dfx -Werror -O3 -fomit-frame-pointer
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

