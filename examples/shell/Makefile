# The X816 boot prompt, and its conformance test.
#
#   shell.bin   the interactive prompt -- load it and type
#   shtest.bin  the conformance test: tokeniser, dispatch, hex, far memory
#
# -O0 IS MANDATORY: the shell reaches memory and device registers through
# volatile pointers, and Calypsi 5.18 eliminates volatile reads above -O0.

CALYPSI ?= /c/calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
CC       = $(CALYPSI)/bin/cc65816
LN       = $(CALYPSI)/bin/ln65816
RT       = ../../runtime
LIB      = $(CALYPSI)/lib/clib-lc-sd.a
CFLAGS   = --core=65816 --code-model=large --data-model=small -O0 -I $(RT)
COMMON   = shell.o console.o font8x8.o x816hdr.o

all: shell.bin shtest.bin

shell.o:   $(RT)/shell.c   $(RT)/shell.h   ; $(CC) $(CFLAGS) $< -o $@
console.o: $(RT)/console.c $(RT)/console.h ; $(CC) $(CFLAGS) $< -o $@
font8x8.o: $(RT)/font8x8.c                 ; $(CC) $(CFLAGS) $< -o $@
x816hdr.o: $(RT)/x816hdr.s                 ; $(AS) --core=65816 $< -o $@
main.o:    shell.c                         ; $(CC) $(CFLAGS) $< -o $@
shtest.o:  shtest.c                        ; $(CC) $(CFLAGS) $< -o $@

shell.bin: main.o $(COMMON) $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o main.o shell.o console.o font8x8.o \
	  $(LIB) -o SHELL.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified
	cp SHELL.raw shell.bin

shtest.bin: shtest.o $(COMMON) $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o shtest.o shell.o console.o font8x8.o \
	  $(LIB) -o SHTEST.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified
	cp SHTEST.raw shtest.bin

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
