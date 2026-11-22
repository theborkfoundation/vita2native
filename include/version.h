#ifndef VITA2NATIVE_VERSION_H
#define VITA2NATIVE_VERSION_H

/*
 * The one place the version is spelled out. VITA2NATIVE_MAJOR/MINOR/PATCH/HASH
 * come from cmake/sources.cmake, so this follows the same value the startup log
 * and `--version` both print - they cannot drift apart.
 */
#define VITA2NATIVE_VERSION_STRING                                                                 \
    "vita2native " VITA2NATIVE_MAJOR "." VITA2NATIVE_MINOR "." VITA2NATIVE_PATCH "-"               \
                   VITA2NATIVE_HASH

#endif
