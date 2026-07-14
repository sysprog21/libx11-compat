#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

#ifndef MOTIF_LIB_PATH
#define MOTIF_LIB_PATH "build/libXm.so"
#endif

int main(void)
{
    void *motif = dlopen(MOTIF_LIB_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!motif) {
        fprintf(stderr, "dlopen(%s) failed: %s\n", MOTIF_LIB_PATH, dlerror());
        return 1;
    }
    if (!compatSelfScalingToolkitLoaded()) {
        fprintf(stderr, "RTLD_LOCAL Motif library was not detected\n");
        return 1;
    }
    return 0;
}
