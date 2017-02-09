#!/bin/bash

# Where will the new phdr get placed in the file?
# Although we could just "try it and see", 
# running ld three times per link is beyond the pale.
# So guess from the original binary.
# It's the offset of the end of the data segment, rounded up to commonpagesize
# PROBLEM: if the extra phdr tips us over a page boundary. Hope it doesn't.
# WHY do we have to add the memsz, not the filesz?
# It's because the start address gets calculated first,
# and the offset must be congruent modulo the *maximum* page size

data_segment_line="$( readelf -Wl "$1" | grep 'LOAD.*RW' | tail -n1 )"

read pt offset vaddr paddr filesz memsz rest <<<"$( echo "$data_segment_line" | tr -s '[:blank:]' '\t' | cut -f2- )"

end_offset_of_data_segment="$(( $offset + $filesz ))"
end_address_of_data_segment="$(( $vaddr + $memsz ))"
printf "data segment end offset: %lx\n" $end_offset_of_data_segment 1>&2
printf "data segment end address: %lx\n" $end_address_of_data_segment 1>&2

commonpagesize=4096
maxpagesize=2097152

remainder="$(( $end_address_of_data_segment % $commonpagesize ))"

# To save space, we only align the meta section to the common page size
if [[ $remainder -eq 0 ]]; then
    rounded_up_addr=$end_address_of_data_segment
else
    rounded_up_addr=$(( $commonpagesize * (1 + $end_address_of_data_segment / $commonpagesize) ))
fi
printf "data segment end address rounded up to meta slignment: %lx\n" $rounded_up_addr 1>&2

# now we have the meta address; what offset
# can it begin at? It's the lowest offset that is
# congruent to the address, modulo the maxpagesize.
# Because we're lazy and inefficient, do it by linear search.

address_modulo_maxpagesize="$(( $rounded_up_addr % $maxpagesize ))"

test_offset="$end_offset_of_data_segment"
while ! [[ $(( $test_offset % $maxpagesize )) == $address_modulo_maxpagesize ]]; do
    test_offset=$(( $test_offset + 1 ))
done

printf "0x%lx\n" $test_offset
