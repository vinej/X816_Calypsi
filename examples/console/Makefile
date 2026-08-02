# Console conformance test.
#
# The recipe -- toolchain, memory model, memory map and the mandatory -O0 --
# comes from runtime/calypsi.mk. The -O0 is not a style choice here: every
# function in the console reaches VERA or the VIA through volatile registers,
# and Calypsi 5.18 eliminates volatile reads at -O1 and above.
#
# ./run-emu.sh is what actually runs this test, and make cannot spawn the
# toolchain on the development machine -- see calypsi.mk.

include ../../runtime/calypsi.mk

LNFLAGS = --list-file CONTEST.map

all: contest.bin

contest.o: contest.c $(RT)/console.h
console.o: $(RT)/console.c $(RT)/console.h
font8x8.o: $(RT)/font8x8.c
x816hdr.o: $(RT)/x816hdr.s

contest.bin: x816hdr.o contest.o console.o font8x8.o $(LDSCRIPT)
	$(call X816_LINK,CONTEST)

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
