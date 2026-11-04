# Loader fixtures

`build_guest_elf.py` emits two minimal `ET_SCE_RELEXEC` executables used to
exercise the loader without a VitaSDK toolchain or a real game:

| File | What it proves |
| --- | --- |
| `guest_return.elf` | `PT_LOAD` mapping, `R_ARM_ABS32` relocations against both segments, module-info lookup, guest `module_start` returning normally (the host must exit). Returns `42` when the relocated pointer is non-zero, `1` otherwise. |
| `guest_exit.elf` | The same loader path plus import resolution: the `SceLibKernel` / `sceKernelExitProcess` NID (`0x7595D9AA`) is patched into a `movw`/`movt`/`bx` stub and called from guest ARM code. |
| `guest_io.elf` | Import resolution for `SceLibKernel` / `sceIoMkdir` (`0x9670D39F`) through the VFS. Returns 42 if `ux0:/fix` was created, 1 otherwise. |

Layout (RX segment):

| Offset | Contents |
| --- | --- |
| `0x000` | `module_start` ARM code |
| `0x040` | import stub, patched by the loader |
| `0x060` | `"SceLibKernel"` |
| `0x080` | `sce_module_info_raw` |
| `0x0E0` | `sce_module_imports_short_raw` |
| `0x108` | function NID table |
| `0x10C` | function entry table |

The data segment holds one word that is relocated to an RX address, and the
relocation segment holds long-form `SCE_Rel` entries.

Generate and run:

```sh
python3 tests/fixtures/build_guest_elf.py build/guest
mkdir -p /tmp/v2n-test && cd /tmp/v2n-test
qemu-arm-static -L /usr/arm-linux-gnueabihf /path/to/vita2native /path/to/guest_return.elf
```

These files are build artifacts; do not commit the generated ELFs.
