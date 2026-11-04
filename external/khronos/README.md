# Khronos EGL / OpenGL ES headers

Vendored so the tree builds without a system `libgles2-mesa-dev` / `libegl-dev`
package and so the cross-build from macOS uses the same headers as the target.

- `EGL/egl.h`, `EGL/eglext.h`, `EGL/eglplatform.h` — KhronosGroup/EGL-Registry
  (`api/EGL/`)
- `KHR/khrplatform.h` — KhronosGroup/EGL-Registry (`api/KHR/`)
- `GLES3/gl3.h`, `GLES3/gl3platform.h` — KhronosGroup/OpenGL-Registry
  (`api/GLES3/`)

All files are Copyright The Khronos Group Inc. and licensed under Apache-2.0
(the SPDX identifier and license text are in each file).

The GLES backend does not link against EGL/GLESv2: it loads them with `dlopen`
at runtime, so these headers are only needed at compile time.
