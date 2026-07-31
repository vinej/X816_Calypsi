# Minimal X816 C program. Produces X816BLANK.raw -- load it at $01:0000.
# ln65816 names the raw image after the ELF, so there is no rename step.
CALYPSI ?= /c/calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
CC       = $(CALYPSI)/bin/cc65816
LN       = $(CALYPSI)/bin/ln65816
CFLAGS   = --core=65816 --code-model=large --data-model=small -O2
LIB      = $(CALYPSI)/lib/clib-lc-sd.a

X816BLANK.raw: blank.o x816hdr.o
	$(LN) ../runtime/x816-plain.scm x816hdr.o blank.o $(LIB) \
	  -o X816BLANK.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified

x816hdr.o: ../runtime/x816hdr.s
	$(AS) --core=65816 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	-rm -f *.o *.elf *.raw *.map
