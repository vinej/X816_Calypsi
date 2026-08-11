# Minimal X816 C program. Produces X816BLANK.raw -- load it at $01:0000.
#
# The template program, so it links with x816-plain.scm (no x16lib, no kernel
# stack) rather than the x816-lib.scm map everything else here uses.
#
# -O2 is safe in this one: blank.c writes a single VERA register and never
# reads one back, so Calypsi 5.18's elision of volatile READS has nothing to
# elide. calypsi.mk requires that to be said, not assumed.

CALYPSI_OPT       := -O2
CALYPSI_ALLOW_OPT := 1
LDSCRIPT          := ../runtime/x816-plain.scm
include ../runtime/calypsi.mk

# X816_LINK_ONLY, not X816_LINK: the target IS the .raw here, so there is no
# copy step -- which is the one thing this example wanted to show.
X816BLANK.raw: x816hdr.o blank.o $(LDSCRIPT)
	$(call X816_LINK_ONLY,X816BLANK)

x816hdr.o: $(RT)/x816hdr.s

clean:
	-rm -f *.o *.elf *.raw *.map
