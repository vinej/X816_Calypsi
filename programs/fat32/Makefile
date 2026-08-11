# FAT32 read conformance test.
#
# The recipe comes from runtime/calypsi.mk, including the -O0 that is
# MANDATORY FOR ANYTHING THAT TOUCHES A MEMORY-MAPPED REGISTER -- and this is
# the code that proved it.
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
# optimiser cannot see them, and then build the rest at -O2. Until then: -O0,
# and calypsi.mk now refuses anything else rather than trusting this comment.
#
# ./run-write.sh is what actually runs the write half of this; make cannot
# spawn the toolchain on the development machine -- see calypsi.mk.

include ../../runtime/calypsi.mk

LNFLAGS = --list-file FSTEST.map

all: fstest.bin

fstest.o: fstest.c $(RT)/fat32.h $(RT)/x816_sd.h
fat32.o: $(RT)/fat32.c $(RT)/fat32.h $(RT)/x816_sd.h
x816hdr.o: $(RT)/x816hdr.s

fstest.bin: x816hdr.o fstest.o fat32.o $(LDSCRIPT)
	$(call X816_LINK,FSTEST)

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
