# The X816 boot prompt, and its conformance test.
#
#   shell.bin    the interactive prompt -- load it and type
#   shtest.bin   the conformance test: tokeniser, dispatch, hex, far memory
#   kbdprobe.bin shows the RAW byte the SMC returns, for when the keyboard is
#                dead on hardware and the conformance test cannot see it
#
# -O0 IS MANDATORY: the shell reaches memory and device registers through
# volatile pointers, and Calypsi 5.18 eliminates volatile reads above -O0.

CALYPSI ?= ../../Calypsi/calypsi-65816-5.18
AS       = $(CALYPSI)/bin/as65816
CC       = $(CALYPSI)/bin/cc65816
LN       = $(CALYPSI)/bin/ln65816
RT       = ../../runtime
LIB      = $(CALYPSI)/lib/clib-lc-sd.a
CFLAGS   = --core=65816 --code-model=large --data-model=small -O0 -I $(RT)
COMMON   = shell.o fat32.o console.o font8x8.o fontcp.o smc.o exec.o exec.o x816hdr.o

all: shell.bin shtest.bin kbdprobe.bin kbdstat.bin kbdecho.bin

shell.o:   $(RT)/shell.c   $(RT)/shell.h   ; $(CC) $(CFLAGS) $< -o $@
console.o: $(RT)/console.c $(RT)/console.h ; $(CC) $(CFLAGS) $< -o $@
fat32.o:   $(RT)/fat32.c   $(RT)/fat32.h   ; $(CC) $(CFLAGS) $< -o $@
font8x8.o: $(RT)/font8x8.c                 ; $(CC) $(CFLAGS) $< -o $@
x816hdr.o: $(RT)/x816hdr.s                 ; $(AS) --core=65816 $< -o $@
smc.o:     $(RT)/smc.s                     ; $(AS) --core=65816 $< -o $@
exec.o:    $(RT)/exec.s                    ; $(AS) --core=65816 $< -o $@
fontcp.o:  $(RT)/font_cp437.s               ; $(AS) --core=65816 $< -o $@
main.o:    shell.c                         ; $(CC) $(CFLAGS) $< -o $@
shtest.o:  shtest.c                        ; $(CC) $(CFLAGS) $< -o $@
probe.o:   kbdprobe.c                      ; $(CC) $(CFLAGS) $< -o $@
stat.o:    kbdstat.c                       ; $(CC) $(CFLAGS) $< -o $@
echo.o:    kbdecho.c                       ; $(CC) $(CFLAGS) $< -o $@

shell.bin: main.o $(COMMON) $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o main.o shell.o fat32.o console.o font8x8.o fontcp.o smc.o exec.o \
	  $(LIB) -o SHELL.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified
	cp SHELL.raw shell.bin

shtest.bin: shtest.o $(COMMON) $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o shtest.o shell.o fat32.o console.o font8x8.o fontcp.o smc.o exec.o \
	  $(LIB) -o SHTEST.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified
	cp SHTEST.raw shtest.bin

# The probe does not need shell.o -- it is console and I2C only, deliberately,
# so that nothing above the console layer can be blamed for what it shows.
kbdprobe.bin: probe.o console.o font8x8.o fontcp.o smc.o exec.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o probe.o console.o font8x8.o fontcp.o smc.o exec.o \
	  $(LIB) -o KBDPROBE.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified
	cp KBDPROBE.raw kbdprobe.bin

# Measures the keyboard path on hardware: cycles per poll from VIA Timer 1,
# and how many keystrokes actually arrive. No shell -- nothing above the
# console layer can be blamed for what it reports.
kbdstat.bin: stat.o console.o font8x8.o fontcp.o smc.o exec.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o stat.o console.o font8x8.o fontcp.o smc.o exec.o \
	  $(LIB) -o KBDSTAT.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified
	cp KBDSTAT.raw kbdstat.bin

# Same tight loop as kbdstat, but it decodes and echoes as the shell does,
# counting each stage separately so one run names which one loses a key.
kbdecho.bin: echo.o console.o font8x8.o fontcp.o smc.o exec.o x816hdr.o $(RT)/x816-lib.scm
	$(LN) $(RT)/x816-lib.scm x816hdr.o echo.o console.o font8x8.o fontcp.o smc.o exec.o \
	  $(LIB) -o KBDECHO.elf --output-format raw \
	  --program-root __x816_root_section --rtattr exit=simplified
	cp KBDECHO.raw kbdecho.bin

clean:
	-rm -f *.o *.elf *.raw *.map *.bin

.PHONY: all clean
