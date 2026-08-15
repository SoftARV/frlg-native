#!/bin/sh
# List the symbols a link genuinely cannot resolve.
#
# Deriving this from `nm` over the archives does not work: the ROM build's .sym
# contains the ARM libc, so memset/memcpy/strcmp look like game code and get
# bound to the stub -- silently hijacking the host libc. A trial link is the
# only thing that knows what the host C library already provides.
#
# A failing trial link is the *expected* case: it fails because symbols are
# missing, and naming them is the point. What must not pass silently is a link
# that failed for some other reason -- no space, no /tmp, a toolchain that is
# not there. That produces an empty list, every cart symbol goes undefined, and
# the build dies hundreds of lines later somewhere unrelated. Ask me how I know.
set -u

out=$1
shift

"$@" > "$out.log" 2>&1
status=$?

grep -oE "undefined reference to \`[^']+'" "$out.log" \
    | sed "s/.*\`//; s/'\$//" \
    | sort -u > "$out"

count=$(wc -l < "$out")

if [ "$status" -ne 0 ] && [ "$count" -eq 0 ]; then
    echo "find_unbound: the trial link failed without naming a single undefined" >&2
    echo "symbol, so it did not fail for the reason this script exists to find:" >&2
    sed 's/^/    /' "$out.log" | tail -20 >&2
    exit 1
fi

echo "unbound symbols: $count" >&2
