# FAT32 read conformance test.
#
# -O0 IS MANDATORY FOR ANYTHING THAT TOUCHES A MEMORY-MAPPED REGISTER.
#
# Calypsi 5.18 ELIMINATES VOLATILE READS at -O1 and above. Reduced to a
# listing diff, buf_u32() -- four consecutive reads of the block device's data
# window -- emits four `lda 0x9f8c` at -O0 and -O1 out of line, and ZERO once
# inlined at -O1. The reader then walked the root directory correctly and
# failed on the first subdirectory, because the cluster number it read back
# was never actually fetched.
#
# The same bug bites a single read after a write to the same address, which is
# why the device has separate CMD and STATUS registers; see runtime/x816_sd.h.
#
# The proper fix is to move the window accessors into assembly, where the C
# optimiser cannot see them, and then build the rest at -O2. Until then: -O0.

CALYPSI ?= ../../Calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
CC       = $(CALYPSI)/bin/cc65816
LN       = $(CALYPSI)/bin/ln65816
RT       = ../../runtime
LIB      = $(CALYPSI)/lib/clib-lc-sd.a
CFLAGS   = --core=65816 --code-model=large --data-model=small -O0 -I $(RT)

all: fstest.bin

fstest.o: fstest.c $(RT)/fat32.h $(RT)/x816_sd.h
	$(CC) $(CFLAGS) fstest.c -o $@

fat32.o: $(RT)/fat32.c $(RT)/fat32.h $(RT)/x816_sd.h
	$(CC) $(CFLAGS) $(RT)/fat32.c -o $@

x816hdr.o: $(RT)/x816hdr.s
	$(AS) --core=65816 $< -o $@

fstest.bin: fstest.o fat32.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o fstest.o fat32.o $(LIB) \
	  -o FSTEST.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified \
	  --list-file FSTEST.map
	cp FSTEST.raw fstest.bin

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
