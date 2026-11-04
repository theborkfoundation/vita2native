#ifndef LOAD_H
#define LOAD_H

#include "jit.h"

int load_file(Jit *jit, const char *filename, void **entry);

#endif
