#!/bin/sh
# The game is 32-bit and hard-codes /lib/ld-linux.so.2 as its ELF interpreter.
# That path does not exist in the sandbox: the loader ships with the i386 compat
# extension, which mounts somewhere else entirely. execve() then fails with
# ENOENT for a file that is plainly there, which reads as a missing binary
# rather than a missing loader. Invoking the loader by path is the fix.
#
# The search path is set through the environment rather than the loader's
# --library-path for two reasons: --library-path replaces the search rather than
# adding to it, which loses the graphics driver, and it does not reach dlopen(),
# which is how SDL finds libdecor and how GLVND finds the driver behind it.
#
# See spike 0011.
i386=/app/lib/i386-linux-gnu

path=$i386
# The graphics driver lands in a subdirectory named for the driver in use, so it
# is discovered rather than named: on this machine nvidia-610-57-04, on the next
# one Mesa's `default`.
for dir in "$i386"/GL/*/lib; do
    [ -d "$dir" ] && path="$path:$dir"
done

export LD_LIBRARY_PATH="$path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# GLVND reads its vendor configuration from here; merge-dirs collects the 32-bit
# copies under the extension rather than the runtime's 64-bit ones.
if [ -d "$i386/GL/glvnd/egl_vendor.d" ]; then
    export __EGL_VENDOR_LIBRARY_DIRS="$i386/GL/glvnd/egl_vendor.d"
fi

exec "$i386/ld-linux.so.2" /app/lib/frlg/frlg-native "$@"
