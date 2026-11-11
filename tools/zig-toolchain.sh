#!/bin/sh
#
# Writes the compiler wrappers that the CMake toolchain files in
# cmake/toolchains expect. Zig ships its own clang, LLD and libc, so it can
# cross-build the 32-bit ARM target from a 64-bit host (macOS or Linux) without
# a system cross toolchain.
#
#   tools/zig-toolchain.sh arm          # armhf wrappers in build/
#   tools/zig-toolchain.sh x64          # x86-64 wrappers in build/
#   tools/zig-toolchain.sh arm /opt/x   # ... in /opt/x
#
# Configure with:
#   cmake --preset arm-rel              # or arm-debug, arm-minsize,
#                                       #    x64-syntax
# The Zig binary is taken from $ZIG, or `zig` on PATH.

set -eu

arch=${1:-}
out=${2:-build}

case "$arch" in
    arm)
        target=arm-linux-gnueabihf
        prefix=zig-arm
        ;;
    x64)
        target=x86_64-linux-gnu
        prefix=zig-x64
        ;;
    *)
        echo "usage: $0 arm|x64 [output-dir]" >&2
        exit 2
        ;;
esac

zig=${ZIG:-$(command -v zig || true)}
if [ -z "$zig" ]; then
    echo "$0: zig not found; install it or set ZIG=/path/to/zig" >&2
    exit 1
fi

mkdir -p "$out"

write() {
    name=$1
    shift
    printf '#!/bin/sh\nexec %s %s -target %s "$@"\n' "$zig" "$*" "$target" > "$out/$name"
    chmod +x "$out/$name"
}

write "$prefix-cc" cc
write "$prefix-cxx" c++
write "$prefix-ar" ar
write "$prefix-ranlib" ranlib

echo "wrote $out/$prefix-{cc,cxx,ar,ranlib} ($target via $zig)"
