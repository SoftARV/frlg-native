#!/bin/sh
# The game is 32-bit and hard-codes /lib/ld-linux.so.2 as its ELF interpreter.
# That path does not exist in the sandbox: the loader ships with the i386 compat
# extension, which mounts somewhere else entirely. execve() then fails with
# ENOENT for a file that is plainly there, which reads as a missing binary
# rather than a missing loader.
#
# Invoking the loader explicitly is what makes it runnable. See spike 0011.
exec /app/lib/i386-linux-gnu/ld-linux.so.2 \
     --library-path /app/lib/i386-linux-gnu \
     /app/lib/frlg/frlg-native "$@"
