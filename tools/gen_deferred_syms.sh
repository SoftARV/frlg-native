#!/bin/sh
# List every symbol defined by a deferred subsystem's sources.
#
# Prefix matching cannot do this job: sound routines are named m4aSoundInit,
# SetPokemonCryStereo and PlayBGM alike, so a prefix list is an endless guess
# that fails one routine at a time, deep into a run. The ROM build already
# knows exactly which file defines what.
#
# usage: gen_deferred_syms.sh OBJDIR OUTFILE object.o...
set -eu

objdir=$1
out=$2
shift 2

: > "$out"
for obj in "$@"; do
    if [ -f "$objdir/$obj" ]; then
        arm-none-eabi-nm --defined-only "$objdir/$obj" | awk 'NF==3 {print $3}' >> "$out"
    fi
done

sort -u -o "$out" "$out"
echo "deferred symbols: $(wc -l < "$out")" >&2
