# X816 desktop launcher. Produces x.bin; place it on the card as /DESKTOP/X.BIN.

include ../../runtime/calypsi.mk

all: x.bin

x816hdr.o: $(RT)/x816hdr.s
kcall.o:   $(RT)/kcall.s

x.bin: x816hdr.o desktop.o kcall.o $(LDSCRIPT)
	$(call X816_LINK,X)

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
