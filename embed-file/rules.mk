$(warning MAKEFILE_LIST is $(MAKEFILE_LIST))
THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
# We build main with a custom phdr
# whose contents are
# the doctored ELF file we want to embed.
# This phdr should NOT be a LOAD phdr; give it some custom number.
# PROBLEM: we need to know the file offset that the phdr will go at,
# so that we can shift all the file offsets in the embedded ELF file.

ELFTIN ?= $(realpath $(dir $(THIS_MAKEFILE))/..)
$(warning ELFTIN is $(ELFTIN))
LIBALLOCS ?= $(realpath $(dir $(THIS_MAKEFILE))/../../liballocs.hg)
$(warning LIBALLOCS is $(LIBALLOCS))

PHDR_NAME ?= meta
SECTION_NAME ?= .meta

include $(ELFTIN)/custom-phdrs/rules.mk
THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

vpath %.patch $(ELFTIN)/custom-phdrs
vpath %.inc $(ELFTIN)/custom-phdrs
export M4PATH += $(ELFTIN)/custom-phdrs

$(PHDR_NAME).o: $(FILE_TO_EMBED)
	objcopy -I binary -O elf64-x86-64 -B i386:x86-64 --rename-section .data=$(SECTION_NAME) "$<" "$@"
#--set-section-flags .data=noload "$<" "$@"

clean::
	rm -f $(PHDR_NAME).o
