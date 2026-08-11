# Program built against the converted X16 assembly library.
#
# Produces libtest.bin and x816demo.bin, ready to load at $01:0000 -- one link,
# no stub, no copy step. runtime/x816-lib.scm puts the library's variables and
# tables in bank $00 alongside the direct page and stack, and the code at
# $01:0000+ in SDRAM, which is what X816_Core doc/MEMORY_MAP.md calls for.
#
# Calypsi's cstartup runs first: it sets native mode, the stack, the direct
# page and DBR = $00, and copies the library's initialised tables out of the
# image into bank $00. Your entry point is main().
#
# ln65816 always names the raw image <stem>.raw, whatever -o says, so a copy
# step is needed: the core's OSD entry is "F1,BIN,Load Image", so it only
# offers .bin files. X816_LINK in calypsi.mk does that copy -- with cp, not
# mv, because mv is not found by the shell make picks up here.
#
# ./run-emu.sh is what actually runs the conformance test; make cannot spawn
# the toolchain on the development machine -- see calypsi.mk.

include ../../runtime/calypsi.mk

all: x816demo.bin libtest.bin

demo.o: demo.s
	$(AS) $(ASFLAGS) -I $(X16LIB) $< -o $@

libtest.o: libtest.s
	$(AS) $(ASFLAGS) -I $(X16LIB) $< -o $@

x816hdr.o: $(RT)/x816hdr.s

x816demo.bin: LNFLAGS = --list-file X816DEMO.map
x816demo.bin: x816hdr.o demo.o $(LDSCRIPT)
	$(call X816_LINK,X816DEMO)

# The runtime conformance test: GREEN screen = pass. ./run-emu.sh builds this
# and checks the result automatically in the emulator.
libtest.bin: LNFLAGS = --list-file LIBTEST.map
libtest.bin: x816hdr.o libtest.o $(LDSCRIPT)
	$(call X816_LINK,LIBTEST)

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
