#!/usr/bin/env python3
"""Populate a cross-compilation sysroot from Debian .deb packages.

The devices run Debian trixie/armhf, so the system libraries the cross build
links against are just that suite's packages. This tool resolves a dependency
closure from the archive index, downloads the .debs into a cache, and unpacks
their payloads into a sysroot. Nothing runs package maintainer scripts: a
sysroot only needs headers, static archives and shared objects.

    tools/debian-sysroot.py                       # SDL2 and its dev dependencies
    tools/debian-sysroot.py --print-pkgconfig
    tools/debian-sysroot.py --packages libsdl2-dev,libdrm-dev --keep-doc

Then point CMake at it:

    cmake --preset arm-rel -DVITA2NATIVE_HEADLESS=OFF \\
          -DVITA2NATIVE_SYSROOT=$PWD/build/sysroot-armhf

The sysroot is idempotent: packages already unpacked at the same version are
skipped, so re-running only picks up changes.
"""

import argparse
import fnmatch
import io
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request

MIRROR = "https://deb.debian.org/debian"

# SDL2 and the libraries its headers reference. libsdl2-dev's own Depends pull
# in the rest (asound, dbus, drm, egl, gbm, gl, gles, ibus, pulse, samplerate),
# which is the point: the closure is resolved from the archive, not hardcoded.
DEFAULT_PACKAGES = [
    "libsdl2-dev",
    # SDL_syswm.h includes the windowing headers of every backend the target's
    # SDL was built with, so compiling against it needs these even though the
    # emulator itself only uses the window/KMSDRM paths.
    "libx11-dev",
    "libwayland-dev",
]

# Not build inputs; their payloads only bloat the sysroot.
PRUNE_PATHS = ["usr/share/doc", "usr/share/man", "usr/share/locale", "usr/share/lintian"]
PRUNE_KEEP_PATHS = ["usr/share/doc/pkg-config", "usr/share/doc/libc6-dev"]


def log(message):
    print(message, flush=True)


def fetch(url, dest):
    """Download url to dest (skipping it when already cached)."""
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    partial = dest + ".part"

    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                with open(partial, "wb") as out:
                    shutil.copyfileobj(response, out)
            os.replace(partial, dest)
            return dest
        except (urllib.error.URLError, TimeoutError) as error:
            if attempt == 2:
                raise SystemExit("%s: %s" % (url, error))
            log("  retrying %s (%s)" % (url, error))
    return dest


def load_index(suite, component, arch):
    """Return {package: {field: value}} for one component of a suite."""
    url = "%s/dists/%s/%s/binary-%s/Packages.xz" % (MIRROR, suite, component, arch)
    log("index %s" % url)
    with urllib.request.urlopen(url, timeout=120) as response:
        raw = lzma.decompress(response.read()).decode("utf-8", "replace")

    packages = {}
    for stanza in raw.split("\n\n"):
        entry = {}
        for line in stanza.splitlines():
            if line[:1] in (" ", "\t") or ":" not in line:
                continue
            key, _, value = line.partition(":")
            entry[key.strip()] = value.strip()
        if "Package" in entry and "Filename" in entry:
            packages[entry["Package"]] = entry
    return packages


def dependency_groups(entry):
    """Yield the alternatives of each dependency, in preference order."""
    for field in ("Pre-Depends", "Depends"):
        value = entry.get(field)
        if not value:
            continue
        for group in value.split(","):
            alternatives = []
            for item in group.split("|"):
                name = item.split("(")[0].strip()
                # "libc6:armhf" and "libc6-dev:any" name the same package.
                name = name.split(":")[0].strip()
                if name:
                    alternatives.append(name)
            if alternatives:
                yield alternatives


# Packages that only exist to build software: compilers, script runtimes,
# packaging helpers. A -dev package depends on them, but their payloads are of
# no use when compiling against its headers, and unpacking a python or gcc into
# the sysroot only invites picking up the wrong headers.
TOOLING = (
    "python3*", "perl*", "ruby*", "gcc*", "g++*", "cpp*", "make",
    "dpkg*", "debconf*", "debhelper*", "dh-*", "cmake*", "ninja-*", "meson*",
    "autoconf*", "automake*", "autotools-*", "libtool", "pkg-config", "pkgconf*",
    "sed", "tar", "m4", "patch", "quilt", "gettext*", "intltool", "po-debconf",
    "xml-core", "sgml-base", "docbook*", "xsltproc*", "libxml2-utils",
    "openjdk-*", "default-jre*", "lsb-base", "sensible-utils", "ca-certificates",
)


def is_tooling(name):
    return any(fnmatch.fnmatch(name, pattern) for pattern in TOOLING)


def resolve(seeds, index, full_deps=False):
    """Dependency closure of seeds, first-satisfiable alternative per group.

    Without full_deps the closure stops at packages whose only purpose is to
    build software (see TOOLING); headers and libraries are followed, because
    compiling against SDL's headers needs the X11 and Wayland ones they name.
    """
    wanted = []
    seen = set()
    missing = set()
    queue = list(seeds)

    while queue:
        name = queue.pop(0)
        if name in seen:
            continue
        seen.add(name)
        entry = index.get(name)
        if not entry:
            # Virtual packages ("awk", "debconf-2.0") have no files of their
            # own; a build input never depends on one for headers.
            missing.add(name)
            continue
        wanted.append(entry)
        for alternatives in dependency_groups(entry):
            for candidate in alternatives:
                if candidate in seen:
                    break
                if full_deps or not is_tooling(candidate):
                    if candidate in index:
                        queue.append(candidate)
                        break
    return wanted, sorted(missing - seen)


def ar_members(data):
    """Yield (name, payload) from an ar archive (.deb, no symbol table)."""
    if data[:8] != b"!<arch>\n":
        raise SystemExit("not an ar archive")
    offset = 8
    while offset + 60 <= len(data):
        header = data[offset : offset + 60]
        name = header[:16].decode("ascii", "replace").strip()
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError:
            break
        offset += 60
        yield name.rstrip("/"), data[offset : offset + size]
        offset += size + (size & 1)


def unpack(deb, dest):
    """Extract the data.tar.* payload of a .deb into dest."""
    with open(deb, "rb") as handle:
        archive = handle.read()

    for name, payload in ar_members(archive):
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".zst"):
            # libarchive handles zstd when Python's tarfile cannot.
            result = subprocess.run(["bsdtar", "-xf", "-", "-C", dest], input=payload)
            if result.returncode != 0:
                raise SystemExit("%s: bsdtar failed" % deb)
            return
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:*") as tar:
            try:
                tar.extractall(dest, filter="fully_trusted")
            except TypeError:
                tar.extractall(dest)
        return
    raise SystemExit("%s: no data.tar payload" % deb)


def stamp_path(dest, entry):
    return os.path.join(dest, ".debs", "%s_%s.stamp" % (entry["Package"], entry["Version"]))


def prune(dest):
    for relative in PRUNE_PATHS:
        path = os.path.join(dest, relative)
        if os.path.isdir(path):
            for keep in PRUNE_KEEP_PATHS:
                if keep.startswith(relative + "/"):
                    shutil.rmtree(
                        os.path.join(path, keep.split("/", len(relative.split("/")))[-1]),
                        ignore_errors=True,
                    )
            shutil.rmtree(path, ignore_errors=True)


def multiarch_dir(dest, arch):
    """The triplet directory Debian installs libraries into (arm-linux-gnueabihf)."""
    libraries = os.path.join(dest, "usr", "lib")
    if os.path.isdir(libraries):
        for name in sorted(os.listdir(libraries)):
            if name.endswith("-linux-gnu") or "-linux-gnu" in name:
                return name
    return arch


def pkgconfig_paths(dest, arch):
    return [
        os.path.join(dest, "usr/lib/%s/pkgconfig" % multiarch_dir(dest, arch)),
        os.path.join(dest, "usr/lib/pkgconfig"),
        os.path.join(dest, "usr/share/pkgconfig"),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--suite", default="trixie", help="Debian suite (default: trixie)")
    parser.add_argument("--arch", default="armhf", help="Debian architecture (default: armhf)")
    parser.add_argument("--components", default="main", help="comma-separated archive components")
    parser.add_argument(
        "--dest", default=None, help="sysroot directory (default: build/sysroot-<arch>)"
    )
    parser.add_argument(
        "--cache", default=None, help=".deb cache directory (default: build/debs-<arch>)"
    )
    parser.add_argument(
        "--packages",
        default=",".join(DEFAULT_PACKAGES),
        help="comma-separated seed packages (default: %s)" % ",".join(DEFAULT_PACKAGES),
    )
    parser.add_argument("--keep-doc", action="store_true", help="keep man pages and locale data")
    parser.add_argument(
        "--full-deps",
        action="store_true",
        help="also unpack compilers and script runtimes (a full dpkg-equivalent "
        "closure; not needed to compile against the libraries)",
    )
    parser.add_argument("--dry-run", action="store_true", help="resolve and list, download nothing")
    parser.add_argument(
        "--print-pkgconfig", action="store_true", help="print the pkg-config environment and exit"
    )
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dest = os.path.abspath(args.dest or os.path.join(root, "build", "sysroot-" + args.arch))
    cache = os.path.abspath(args.cache or os.path.join(root, "build", "debs-" + args.arch))
    seeds = [name.strip() for name in args.packages.split(",") if name.strip()]

    if args.print_pkgconfig:
        print("PKG_CONFIG_LIBDIR=%s" % ":".join(pkgconfig_paths(dest, args.arch)))
        return 0

    index = {}
    for component in args.components.split(","):
        index.update(load_index(args.suite, component.strip(), args.arch))

    unknown = [name for name in seeds if name not in index]
    if unknown:
        raise SystemExit("not in the index: %s" % ", ".join(unknown))

    wanted, missing = resolve(seeds, index, args.full_deps)
    log("%d package(s) needed, %d virtual name(s) skipped" % (len(wanted), len(missing)))
    if args.dry_run:
        for entry in sorted(wanted, key=lambda item: item["Package"]):
            log("  %-32s %s" % (entry["Package"], entry["Version"]))
        return 0

    os.makedirs(dest, exist_ok=True)
    os.makedirs(os.path.join(dest, ".debs"), exist_ok=True)
    os.makedirs(cache, exist_ok=True)

    downloaded = 0
    unpacked = 0
    for entry in sorted(wanted, key=lambda item: item["Package"]):
        name = entry["Package"]
        deb = os.path.join(cache, os.path.basename(entry["Filename"]))
        stamp = stamp_path(dest, entry)

        if os.path.exists(stamp):
            continue
        fetch("%s/%s" % (MIRROR, entry["Filename"]), deb)
        downloaded += 1
        unpack(deb, dest)
        unpacked += 1
        os.makedirs(os.path.dirname(stamp), exist_ok=True)
        with open(stamp, "w") as handle:
            handle.write(entry["Filename"] + "\n")
        if unpacked % 10 == 0:
            log("  %d/%d unpacked" % (unpacked, len(wanted)))

    if not args.keep_doc:
        prune(dest)

    log("sysroot %s (%d unpacked this run, %d new download(s))" % (dest, unpacked, downloaded))
    print()
    print("configure with:")
    print(
        "  cmake --preset arm-rel -DVITA2NATIVE_HEADLESS=OFF "
        "-DVITA2NATIVE_SYSROOT=%s" % dest
    )
    print()
    print("pkg-config:")
    print("  PKG_CONFIG_LIBDIR=%s" % ":".join(pkgconfig_paths(dest, args.arch)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
