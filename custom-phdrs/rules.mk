THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

srcdir ?= $(dir $(THIS_MAKEFILE))

PHDR_NAME ?= heap

# We use "redirect I/O to output file" in some of the rules below, so
# make sure that if the rule fails, we delete any partial output
# that might be bogus.
.DELETE_ON_ERROR:

# To make the program headers, we need to
# build a fake binary, snarf its program headers,
# encode the snarfed section-segment mapping as an m4 if-else chain macro,
# rewrite the linker script to invoke this macro for each output section,
# and rebuild.
# GAH. Can't use an if-else chain because some sections are in more than one segment.
# What can we do instead?
# Need to invert the mapping: given a section, get a list of phdrs.
# Can do this either in a bash/ksh script (use associative arrays) or in m4

%.phdrs.inc: %
	($(srcdir)/phdrs-to-lds.sh $< && $(srcdir)/phdrs-to-m4.sh $<) > "$@"

# We rewrite the ldscript so that every output section
# is m4-macroised. This is easy using regular expressions
# because section definitions don't have a recursive structure.
%.lds.phdrify.m4: %.lds phdrs_extra.inc
	cat "$<" | sed 's#SECTIONS#PHDRS\n{\n\tinclude($*.phdrs.inc)\n\tinclude($(filter %phdrs_extra.inc,$+))\n}\nSECTIONS#' | \
	tr '\n' '\f' | sed -r \
	's@(\.[-a-zA-Z0-9\._]+)[[:space:]\f]+(([^\{]*[[:space:]\f]*)?\{[^\}]*\})@expand_phdr([\1], [\2])@g' | \
	tr '\f' '\n' > "$@"

%.with-phdrs.lds: %.lds.phdrify.m4 %.phdrs.inc phdrs_extra.inc
	m4 < "$<" > "$@"
	# HACK around strange linker behaviour (placing orphan .note.ABI-tag at weirdly high offset but low address)
	sed -i '/^ *\.note\.gnu\.build-id : { \*(\.note\.gnu\.build-id) }.*/ s/^\( *\.note\)\(\.gnu\.build-id\)\( : { \*(\.note\)\(\.gnu\.build-id\)\() }.*\)/\1.ABI-tag\3.ABI-tag\5\n&/' "$@"

%.with-$(PHDR_NAME)-phdr.lds: %.with-phdrs.lds $(PHDR_NAME)ify.patch
	patch -o "$@" $+

$(OBJ_NAME).lds: $(OBJ_NAME)
$(OBJ_NAME).with-phdrs.lds: $(OBJ_NAME)
$(OBJ_NAME).phdrs.inc: $(OBJ_NAME)
.SECONDARY: $(OBJ_NAME).lds $(OBJ_NAME).with-phdrs.lds $(OBJ_NAME).phdrs.inc $(OBJ_NAME)

$(OBJ_NAME): LDLIBS += -Wl,--verbose 2>&1 |\
          LC_ALL=C\
          sed -e '/^=========/,/^=========/!d;/^=========/d'\
          > $(OBJ_NAME).lds || (rm -f "$@"; false)
$(OBJ_NAME).with-$(PHDR_NAME)-phdr: $(OBJ_NAME).with-$(PHDR_NAME)-phdr.lds
# put the LDLIBS at the end. And we might get two copies of %.o, so use sort.
%:
%: %.o
	$(CC) -o "$@" $(CFLAGS) $(CPPFLAGS) $(sort $+) $(LDFLAGS) $(LDLIBS)


clean::
	rm -f *.o *.lds *.lds.phdrify.m4
	rm -f *.phdrs.inc
