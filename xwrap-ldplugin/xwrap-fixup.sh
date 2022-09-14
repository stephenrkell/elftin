#!/bin/sh

echo "$0" "$@" 1>&2
tmpname="$1"
shift
origname="$1"
shift
# now we have symnames in $@

NORMRELOCS="${NORMRELOCS:-`dirname "$0"`/../normrelocs/normrelocs}"

cat "$origname" > "$tmpname"
for sym in $@; do
    ${NORMRELOCS} "$tmpname" "$sym"
done
newtmp=`mktemp --suffix=.o`
ld -r `for sym in $@; do echo --defsym __real_$sym=$sym; done` -o "$newtmp" "$tmpname"
mv "$newtmp" "$tmpname"

