# Program built against the converted X16 assembly library.
#
# Produces X816DEMO.raw, ready to load at $01:0000 -- one link, no stub, no
# copy step. runtime/x816-lib.scm puts the library's variables and tables in
# bank $00 alongside the direct page and stack, and the code at $01:0000+ in
# SDRAM, which is what X816_Core doc/MEMORY_MAP.md calls for.
#
# Calypsi's cstartup runs first: it sets native mode, the stack, the direct
# page and DBR = $00, and copies the library's initialised tables out of the
# image into bank $00. Your entry point is main().
#
# ln65816 writes the ELF at -o and the raw image beside it with the same stem,
# so there is no rename step -- `mv` is not dependable across the shells make
# picks up on Windows.

CALYPSI ?= /c/calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
LN       = $(CALYPSI)/bin/ln65816
SRC      = ../../src
RT       = ../../runtime
LIB      = $(CALYPSI)/lib/clib-lc-sd.a

all: X816DEMO.raw LIBTEST.raw

demo.o: demo.s
	$(AS) --core=65816 -I $(SRC) $< -o $@

x816hdr.o: $(RT)/x816hdr.s
	$(AS) --core=65816 $< -o $@

X816DEMO.raw: demo.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o demo.o $(LIB) \
	  -o X816DEMO.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified \
	  --list-file X816DEMO.map

libtest.o: libtest.s
	$(AS) --core=65816 -I $(SRC) $< -o $@

# The runtime conformance test: GREEN screen = pass. ./run-emu.sh builds this
# and checks the result automatically in the emulator.
LIBTEST.raw: libtest.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o libtest.o $(LIB) \
	  -o LIBTEST.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified \
	  --list-file LIBTEST.map

clean:
	-rm -f *.o *.elf *.raw *.map

.PHONY: all clean
