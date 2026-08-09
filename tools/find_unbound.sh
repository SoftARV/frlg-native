#!/bin/sh
# List the symbols a link genuinely cannot resolve.
#
# Deriving this from `nm` over the archives does not work: the ROM build's .sym
# contains the ARM libc, so memset/memcpy/strcmp look like game code and get
# bound to the stub -- silently hijacking the host libc. A trial link is the
# only thing that knows what the host C library already provides.
#
# usage: find_unbound.sh OUTFILE LINK_COMMAND...
set -u

out=$1
shift

"$@" > "$out.log" 2>&1 || true

grep -oE "undefined reference to \`[^']+'" "$out.log" \
    | sed "s/.*\`//; s/'\$//" \
    | sort -u > "$out"

echo "unbound symbols: $(wc -l < "$out")" >&2
