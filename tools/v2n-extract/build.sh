#!/bin/sh
#
# Build the host-side extractor. It needs OpenSSL 3 and zlib; on macOS with
# Homebrew the openssl@3 keg is not on the default CMake search path, so it is
# passed explicitly when present.
#
#   tools/v2n-extract/build.sh          # -> build/v2n-extract-host/v2n-extract
#   tools/v2n-extract/build.sh --clean

set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build="$root/build/v2n-extract-host"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$build"
fi

openssl_root=
for candidate in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 /usr; do
    if [ -f "$candidate/include/openssl/evp.h" ]; then
        openssl_root=$candidate
        break
    fi
done

if [ -z "$openssl_root" ]; then
    echo "$0: OpenSSL 3 not found; install openssl@3 (brew) or set CMAKE_PREFIX_PATH" >&2
    exit 1
fi

cmake -S "$root/tools/v2n-extract" -B "$build" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DOPENSSL_ROOT_DIR="$openssl_root"
cmake --build "$build" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "built: $build/v2n-extract"
