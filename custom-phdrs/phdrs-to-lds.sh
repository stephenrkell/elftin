#!/bin/bash

readelf -Wl "$1" | \
sed -ne '/^Program Headers/,/^ Section to Segment mapping/ p' | grep -v '[.*]' | \
tail -n+3 | head -n-2 | sed 's/^[[:blank:]]*//' | tr -s '[:blank:]' '\t' | \
(ctr=0; while read pt_type pt_offset pt_vaddr pt_paddr pt_filesz pt_memsz flags_and_align; do \
	align=$( echo "$flags_and_align" | sed 's/.*\(0x.*\)/\1/' ); \
	flags=$( echo "$flags_and_align" | sed 's/\(.*\)0x.*/\1/' | tr -cd RWE ); \
	if [[ $pt_type == "PHDR" ]] || ( \
	[[ $pt_type == "LOAD" ]] && \
	grep 'E' <<<"$flags" >/dev/null && \
	[[ $(( $pt_offset )) -lt $(( 0x1000 )) ]] ); then \
		phdr_keyword="PHDRS"; else phdr_keyword=""; \
	fi; \
	echo "phdr$ctr PT_$pt_type $phdr_keyword; " \
; ctr=$(( $ctr + 1 )); done) | cpp -P -imacros elf.h | sed '/^[[:blank:]]*$/ d'


# AT ($pt_vaddr)
