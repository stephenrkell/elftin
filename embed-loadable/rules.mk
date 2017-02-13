THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ELFTIN ?= $(realpath $(dir $(THIS_MAKEFILE))/..)
$(warning ELFTIN is $(ELFTIN))

include $(ELFTIN)/embed-file/rules.mk
THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

vpath %.patch $(ELFTIN)/embed-file
vpath %.inc $(ELFTIN)/embed-file
export M4PATH += $(ELFTIN)/embed-file

SHIFT_ELF ?= $(ELFTIN)/embed-loadable/shift-elf
$(ELFTIN)/embed-loadable/shift-elf:
	$(MAKE) -C $(dir $@) shift-elf

$(warning "You will need a special/hacked ld.so for the embedded file to be loadable, until I rewrite ldso-helper.c to reflect on the ld.so structures correctly. Ask me (srk31@cl.cam.ac.uk).")

%-shifted.so: %.so $(OBJ_NAME) $(SHIFT_ELF)
	offset=$$( printf "%d" "$$( $(ELFTIN)/custom-phdrs/predict-meta-offset.sh $(OBJ_NAME) )" ); \
	cp "$<" "$@" && $(SHIFT_ELF) "$@" $$offset || (rm -f "$@"; false)
%-shifted: % $(OBJ_NAME) $(SHIFT_ELF)
	offset=$$( printf "%d" "$$( $(ELFTIN)/custom-phdrs/predict-meta-offset.sh $(OBJ_NAME) )" ); \
	cp "$<" "$@" && $(SHIFT_ELF) "$@" $$offset || (rm -f "$@"; false)

clean::
	rm -f $(OBJ_NAME) $(OBJ_NAME).with-$(PHDR_NAME)-phdr

