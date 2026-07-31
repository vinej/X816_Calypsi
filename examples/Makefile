# Minimal X816 C program. Produces prog.raw -- load it at $01:0000.
CALYPSI ?= /c/calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
CC       = $(CALYPSI)/bin/cc65816
LN       = $(CALYPSI)/bin/ln65816
CFLAGS   = --core=65816 --code-model=large --data-model=small -O2
LIB      = $(CALYPSI)/lib/clib-lc-sd.a

prog.raw: blank.o x816hdr.o
	$(LN) ../runtime/x816-plain.scm x816hdr.o blank.o $(LIB) \
	  -o prog.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified

x816hdr.o: ../runtime/x816hdr.s
	$(AS) --core=65816 $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	-rm -f *.o prog.elf prog.raw
