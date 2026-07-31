# Program built against the converted X16 assembly library.
#
# Produces libtest.bin and x816demo.bin, ready to load at $01:0000 -- one link, no stub, no
# copy step. runtime/x816-lib.scm puts the library's variables and tables in
# bank $00 alongside the direct page and stack, and the code at $01:0000+ in
# SDRAM, which is what X816_Core doc/MEMORY_MAP.md calls for.
#
# Calypsi's cstartup runs first: it sets native mode, the stack, the direct
# page and DBR = $00, and copies the library's initialised tables out of the
# image into bank $00. Your entry point is main().
#
# ln65816 always names the raw image <stem>.raw, whatever -o says, so a copy
# step is needed: the core's OSD entry is "F1,BIN,Load Image", so it only
# offers .bin files. Use cp, not mv -- mv is not found by the shell make picks
# up here, cp is.

CALYPSI ?= ../../Calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
LN       = $(CALYPSI)/bin/ln65816
SRC      = ../../src
RT       = ../../runtime
LIB      = $(CALYPSI)/lib/clib-lc-sd.a

all: x816demo.bin libtest.bin

demo.o: demo.s
	$(AS) --core=65816 -I $(SRC) $< -o $@

x816hdr.o: $(RT)/x816hdr.s
	$(AS) --core=65816 $< -o $@

x816demo.bin: demo.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o demo.o $(LIB) \
	  -o X816DEMO.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified \
	  --list-file X816DEMO.map
	cp X816DEMO.raw x816demo.bin

libtest.o: libtest.s
	$(AS) --core=65816 -I $(SRC) $< -o $@

# The runtime conformance test: GREEN screen = pass. ./run-emu.sh builds this
# and checks the result automatically in the emulator.
libtest.bin: libtest.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o libtest.o $(LIB) \
	  -o LIBTEST.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified \
	  --list-file LIBTEST.map
	cp LIBTEST.raw libtest.bin

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
