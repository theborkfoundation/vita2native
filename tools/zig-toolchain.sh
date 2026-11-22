#!/bin/sh
#
# Writes the compiler wrappers that cmake/toolchains/zig-arm.cmake expects. Zig
# ships its own clang, LLD and libc, so it can cross-build the 32-bit ARM target
# from a 64-bit host (macOS or Linux) without a system cross toolchain.
#
#   tools/zig-toolchain.sh             # armhf wrappers in build/
#   tools/zig-toolchain.sh /opt/x      # ... in /opt/x
#
# Configure with:
#   cmake --preset release              # or debug, minsize
# The Zig binary is taken from $ZIG, or `zig` on PATH.

set -eu

out=${1:-build}

target=arm-linux-gnueabihf
prefix=zig-arm

zig=${ZIG:-$(command -v zig || true)}
if [ -z "$zig" ]; then
    echo "$0: zig not found; install it or set ZIG=/path/to/zig" >&2
    exit 1
fi

mkdir -p "$out"

# Only the compilers take -target. ar and ranlib archive whatever objects they
# are handed, and `zig ar -target` is worse than useless: the LLVM archiver reads
# a single-dash argument as bundled options, so "-target" becomes -t -a -r -g -e
# -t and fails with "unknown option g".
write_compiler() {
    name=$1
    shift
    printf '#!/bin/sh\nexec %s %s -target %s "$@"\n' "$zig" "$*" "$target" > "$out/$name"
    chmod +x "$out/$name"
}

write_tool() {
    name=$1
    shift
    printf '#!/bin/sh\nexec %s %s "$@"\n' "$zig" "$*" > "$out/$name"
    chmod +x "$out/$name"
}

write_compiler "$prefix-cc" cc
write_compiler "$prefix-cxx" c++
write_tool "$prefix-ar" ar
write_tool "$prefix-ranlib" ranlib

echo "wrote $out/$prefix-{cc,cxx,ar,ranlib} ($target via $zig)"
