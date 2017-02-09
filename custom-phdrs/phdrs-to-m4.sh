#!/bin/bash

# FLAGS ($flags)

# To use extra_phdrs, we want a way to substitute the mapping for a given section
# 

declare -A phdrs
while read num rest; do \
    while read scn; do \
        if ! [[ -z "$scn" ]]; then
            # echo "scn is $scn" 1>&2
            phdrs[$scn]="${phdrs[$scn]}${phdrs[$scn]:+ }:phdr$(( $num ))"; \
        fi
    done<<<"$(echo -n "$rest" | tr '\t' '\n')"; \
done<<<"$( readelf -Wl "$1" | \
sed -ne '/^ Section to Segment mapping/,$ p' | tail -n+3 | \
sed 's/^[[:blank:]]*//' | sed 's/^0//' | sed 's/[[:blank:]]*$//' | tr -s '[:blank:]' '\t' \
)"

# Now we want to allow reassignments, i.e.
# 1. to override which phdr a given section goes in, and
# 2. to specify phdrs for sections that haven't yet been seen.
#
# To do (1), we really want to hack the loop above.
# To do (2), we simply want to add more stuff to phdrs.
#
# NOTE that it's output sections that matter. So the linker
# script probably should already have a custom output section
# stanza.
#
# NOTE also that we only really need number 2. Instead of 
# overriding stuff, just do the dummy link without it. E.g.
# my fakemalloc.so should just omit the heapsegment.o
# containing the heap sections.

echo "changequote([,])dnl"
echo -n "define([expand_phdr], [ifelse("; \
for scn in "${!phdrs[@]}"; do echo -n "[\$1], [$scn], [\$1 \$2 ${phdrs[$scn]}], "; done; \
echo "[\$1 \$2])])"
