# Console conformance test.
#
# -O0 IS MANDATORY. Every function in the console reaches VERA or the VIA
# through volatile registers, and Calypsi 5.18 eliminates volatile reads at
# -O1 and above -- see the README.

CALYPSI ?= /c/calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
CC       = $(CALYPSI)/bin/cc65816
LN       = $(CALYPSI)/bin/ln65816
RT       = ../../runtime
LIB      = $(CALYPSI)/lib/clib-lc-sd.a
CFLAGS   = --core=65816 --code-model=large --data-model=small -O0 -I $(RT)

all: contest.bin

contest.o: contest.c $(RT)/console.h
	$(CC) $(CFLAGS) contest.c -o $@

console.o: $(RT)/console.c $(RT)/console.h
	$(CC) $(CFLAGS) $(RT)/console.c -o $@

font8x8.o: $(RT)/font8x8.c
	$(CC) $(CFLAGS) $(RT)/font8x8.c -o $@

x816hdr.o: $(RT)/x816hdr.s
	$(AS) --core=65816 $< -o $@

contest.bin: contest.o console.o font8x8.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o contest.o console.o font8x8.o $(LIB) \
	  -o CONTEST.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified \
	  --list-file CONTEST.map
	cp CONTEST.raw contest.bin

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
