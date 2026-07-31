# C program calling the converted X16 assembly library.
#
# Produces ctest.bin, ready to load at $01:0000.
#
# The glue is compiled WITH the library, not against it: runtime/x816_glue.s
# #includes x16.s and x16_code.s, so the whole thing is one translation unit.
# That is not tidiness -- x16lib's labels are local to their unit, so an
# .extern would miss them and `atan2` would silently resolve to libm's
# double-precision version and pull in the entire 64-bit float library. Pick
# modules with -DX16_USE_* on the assembler command line.

CALYPSI ?= /c/calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
CC       = $(CALYPSI)/bin/cc65816
LN       = $(CALYPSI)/bin/ln65816
SRC      = ../../src
RT       = ../../runtime
LIB      = $(CALYPSI)/lib/clib-lc-sd.a
MODULES  = -DX16_USE_MATH=1
CFLAGS   = --core=65816 --code-model=large --data-model=small -O2 -I $(RT)

all: ctest.bin

ctest.o: ctest.c $(RT)/x816.h
	$(CC) $(CFLAGS) ctest.c -o $@

x816_glue.o: $(RT)/x816_glue.s
	$(AS) --core=65816 -I $(SRC) $(MODULES) $< -o $@

x816hdr.o: $(RT)/x816hdr.s
	$(AS) --core=65816 $< -o $@

ctest.bin: ctest.o x816_glue.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o ctest.o x816_glue.o $(LIB) \
	  -o CTEST.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified \
	  --list-file CTEST.map
	cp CTEST.raw ctest.bin

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
