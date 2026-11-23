#define GL_SILENCE_DEPRECATION
#include "nanovg.h"
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#define NANOVG_GL3_IMPLEMENTATION
#else
#include <GLES2/gl2.h>
#define NANOVG_GLES2_IMPLEMENTATION
#endif
#include "nanovg_gl.h"
