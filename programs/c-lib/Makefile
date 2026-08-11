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
#
# THE -O2 IS DELIBERATE, AND THIS IS THE ONE PLACE IT IS SAFE. ctest.c only
# ever WRITES VERA registers; it never reads one back, so Calypsi 5.18's
# elision of volatile READS cannot bite it -- and measuring the x16lib call
# glue under the optimiser is the point of this example. calypsi.mk makes that
# claim explicit rather than letting it be inferred from a missing flag.
#
# ./run-emu.sh is what actually runs this; make cannot spawn the toolchain on
# the development machine -- see calypsi.mk.

CALYPSI_OPT       := -O2
CALYPSI_ALLOW_OPT := 1
include ../../runtime/calypsi.mk

MODULES = -DX16_USE_MATH=1 -DX16_USE_PALETTE=1 -DX16_USE_ZX0=1
LNFLAGS = --list-file CTEST.map

all: ctest.bin

ctest.o: ctest.c $(RT)/x816.h

x816_glue.o: $(RT)/x816_glue.s
	$(AS) $(ASFLAGS) -I $(X16LIB) $(MODULES) $< -o $@

x816hdr.o: $(RT)/x816hdr.s

ctest.bin: x816hdr.o ctest.o x816_glue.o $(LDSCRIPT)
	$(call X816_LINK,CTEST)

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
