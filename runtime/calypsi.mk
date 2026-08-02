# ============================================================================
# calypsi.mk -- the make half of the one X816 build recipe.
#
#     include ../../runtime/calypsi.mk
#
# The shell half is calypsi.sh, and the two exist for the same reason
# (doc/AUDIT.md M-4): the compile line carrying the load-bearing -O0 was
# copy-pasted across a build script, ten run scripts and six Makefiles, so a
# recipe that forgot it produced a clean-linking broken binary. Here the
# forgetting is an $(error), not a miscompile.
#
# HEALTH WARNING, and it is why build.sh exists at all: `make` cannot spawn
# the MSYS-style toolchain paths on the development machine ("CreateProcess
# failed"). These Makefiles are kept correct and are checkable with `make -n`,
# but examples/shell/build.sh is the build that actually runs and the one
# mkrelease.sh calls. If you are adding a target, add it to build.sh too.
#
# Provides:  CC AS LN LIB CFLAGS ASFLAGS LDSCRIPT RT X16LIB
#            $(call X816_LINK,<STEM>)   link $^ into <STEM>.elf, copy .raw to $@
# ============================================================================

# This file's own directory, as the includer spelled it, so RT resolves
# whether the caller is in examples/shell or examples/vera.
CALYPSI_MK := $(lastword $(MAKEFILE_LIST))
RT         := $(patsubst %/,%,$(dir $(CALYPSI_MK)))
REPO       := $(RT)/..

CALYPSI ?= $(REPO)/Calypsi/calypsi-65816-5.18
X16LIB  ?= $(REPO)/src

AS  := $(CALYPSI)/bin/as65816
CC  := $(CALYPSI)/bin/cc65816
LN  := $(CALYPSI)/bin/ln65816
LIB := $(CALYPSI)/lib/clib-lc-sd.a

LDSCRIPT ?= $(RT)/x816-lib.scm

# Large code / small data is not a preference: `far` code and a bank-0 data
# world are what the linker scripts' memory map assumes.
CALYPSI_MODEL := --core=65816 --code-model=large --data-model=small

# -O0 IS THE DEFAULT AND IS LOAD-BEARING. Calypsi 5.18 eliminates volatile
# READS above -O0, so anything that reads a hardware register back -- the
# console's cursor, FAT32's SD status, the SMC's I2C byte -- miscompiles
# silently. A Makefile whose code provably only WRITES registers may set
#     CALYPSI_OPT := -O2
#     CALYPSI_ALLOW_OPT := 1
# and should say in a comment why that is true of it.
CALYPSI_OPT       ?= -O0
CALYPSI_ALLOW_OPT ?= 0

CFLAGS  ?= $(CALYPSI_MODEL) $(CALYPSI_OPT) -I $(RT)
ASFLAGS ?= --core=65816
LNFLAGS ?=

ifeq (,$(findstring -O0,$(CFLAGS)))
ifneq (1,$(CALYPSI_ALLOW_OPT))
$(error calypsi.mk: CFLAGS has no -O0 ($(CFLAGS)). Calypsi 5.18 eliminates \
volatile READS above -O0, so anything that reads a hardware register back \
miscompiles silently. If this code provably never does, set CALYPSI_ALLOW_OPT \
:= 1 and say why in a comment)
endif
endif

# --program-root keeps the x816hdr section (the magic and the entry jmp) from
# being dropped as unreferenced; --rtattr picks the exit path that does not
# drag in the full C runtime teardown. ln65816 always names the raw image
# <stem>.raw whatever -o says, hence the copy -- and cp, not mv: mv is not
# found by the shell make picks up here.
define X816_LINK_ONLY
$(LN) $(LDSCRIPT) $(filter %.o,$^) $(LIB) \
  -o $(1).elf --output-format raw \
  --program-root __x816_root_section --rtattr exit=simplified $(LNFLAGS)
endef

define X816_LINK
$(call X816_LINK_ONLY,$(1))
cp $(1).raw $@
endef

# The runtime sources every example shares. A Makefile that wants one of these
# objects just names it as a prerequisite.
%.o: $(RT)/%.c
	$(CC) $(CFLAGS) $< -o $@

%.o: $(RT)/%.s
	$(AS) $(ASFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@
