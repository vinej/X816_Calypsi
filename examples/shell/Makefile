# The X816 boot prompt, and its conformance tests.
#
#   shell.bin    the interactive prompt -- load it and type
#   shtest.bin   the conformance test: tokeniser, dispatch, hex, far memory
#   kbdprobe.bin shows the RAW byte the SMC returns, for when the keyboard is
#                dead on hardware and the conformance test cannot see it
#
# *** build.sh IS THE BUILD THAT SHIPS, NOT THIS FILE. ***
#
# make cannot spawn the MSYS-style toolchain paths on the development machine
# ("CreateProcess failed"), and mkrelease.sh calls build.sh -- which also
# builds the resident kernel, the VERA tests and the kernel-table tests, none
# of which have targets here. This Makefile covers the shell subset only and
# is kept correct (checkable with `make -n`) rather than authoritative.
#
# Its object list was stale for months before doc/AUDIT.md caught it: COMMON
# was missing kfs.o and kerntab.o and named exec.o twice. The recipe now comes
# from runtime/calypsi.mk and COMMON below is build.sh's COMMON, character for
# character, so the two can be diffed by eye.
#
# -O0 comes from calypsi.mk and is load-bearing: the shell reaches memory and
# device registers through volatile pointers, and Calypsi 5.18 eliminates
# volatile reads above -O0.

include ../../runtime/calypsi.mk

# kerntab.o calls into kfs.o, which calls into fat32.o: the kernel table is
# not linkable without the filesystem behind it, and kexec.o is K_EXEC's
# backend.
COMMON = shell.o fat32.o kfs.o console.o font8x8.o fontcp.o smc.o exec.o \
         kerntab.o kexec.o

all: shell.bin shtest.bin kbdprobe.bin kbdstat.bin kbdecho.bin

shell.o:   $(RT)/shell.c   $(RT)/shell.h
console.o: $(RT)/console.c $(RT)/console.h
fat32.o:   $(RT)/fat32.c   $(RT)/fat32.h
kfs.o:     $(RT)/kfs.c     $(RT)/kfs.h
kexec.o:   $(RT)/kexec.c
font8x8.o: $(RT)/font8x8.c
x816hdr.o: $(RT)/x816hdr.s
smc.o:     $(RT)/smc.s
exec.o:    $(RT)/exec.s     $(RT)/x816_contract.inc
kerntab.o: $(RT)/kerntab.s  $(RT)/x816_contract.inc $(RT)/x816_kerntab.inc

# font_cp437.s does not follow the %.o: $(RT)/%.s stem, so it keeps a rule.
fontcp.o:  $(RT)/font_cp437.s
	$(AS) $(ASFLAGS) $< -o $@

# main.o is this directory's shell.c, not the runtime's -- same base name,
# different file, so it cannot use the implicit rule either.
main.o:    shell.c
	$(CC) $(CFLAGS) $< -o $@

shell.bin: x816hdr.o main.o $(COMMON) $(LDSCRIPT)
	$(call X816_LINK,SHELL)

shtest.bin: x816hdr.o shtest.o $(COMMON) $(LDSCRIPT)
	$(call X816_LINK,SHTEST)

# The probes do not need shell.o -- they are console and I2C only,
# deliberately, so that nothing above the console layer can be blamed for what
# they show.
PROBE = console.o font8x8.o fontcp.o smc.o exec.o

kbdprobe.bin: x816hdr.o kbdprobe.o $(PROBE) $(LDSCRIPT)
	$(call X816_LINK,KBDPROBE)

# Measures the keyboard path on hardware: cycles per poll from VIA Timer 1,
# and how many keystrokes actually arrive.
kbdstat.bin: x816hdr.o kbdstat.o $(PROBE) $(LDSCRIPT)
	$(call X816_LINK,KBDSTAT)

# Same tight loop as kbdstat, but it decodes and echoes as the shell does,
# counting each stage separately so one run names which one loses a key.
kbdecho.bin: x816hdr.o kbdecho.o $(PROBE) $(LDSCRIPT)
	$(call X816_LINK,KBDECHO)

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
