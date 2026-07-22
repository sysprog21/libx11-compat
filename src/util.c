#include "X11/Xlib.h"
#include "X11/Xutil.h"
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "drawing.h"
#include "util.h"

void compatTrace(const char *fmt, ...)
{
    const char *path = getenv("LIBX11_COMPAT_TRACE");
    if (!path || !path[0])
        return;
    FILE *out = fopen(path, "a");
    if (!out)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fflush(out);
    fclose(out);
}

int libx11CompatWarnUnimplemented(void)
{
    /* No cache: this runs only on cold unimplemented-call paths, so re-reading
     * the env each time costs nothing meaningful and avoids a static-init data
     * race under threads.
     */
    const char *value = getenv("LIBX11_COMPAT_WARN_UNIMPLEMENTED");
    return value && value[0] && strcmp(value, "0") != 0;
}

static Bool loadedLibraryHasSymbol(const char *soname, const char *symbol)
{
#ifdef RTLD_NOLOAD
    void *handle = dlopen(soname, RTLD_LAZY | RTLD_NOLOAD);
    if (!handle)
        return False;

    /* RTLD_NOLOAD still bumps the library refcount, and these probes run
     * repeatedly on cold font/realize paths, so drop the reference once the
     * presence check is done. Only the boolean matters, not keeping it mapped.
     */
    Bool present = dlsym(handle, symbol) != NULL;
    dlclose(handle);
    return present;
#else
    (void) soname;
    (void) symbol;
    return False;
#endif
}

static Bool defaultSymbolPresent(const char *const *symbols, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (dlsym(RTLD_DEFAULT, symbols[i]))
            return True;
    return False;
}

static Bool loadedLibraryHasAnySymbol(const char *soname,
                                      const char *const *symbols,
                                      size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (loadedLibraryHasSymbol(soname, symbols[i]))
            return True;
    }
    return False;
}

Bool compatSelfScalingToolkitLoaded(void)
{
    static const char *tkSymbols[] = {
        "Tk_MainWindow",
        "Tk_CreateMainWindow",
        "Tk_CreateWindow",
    };
    if (defaultSymbolPresent(tkSymbols, ARRAY_LENGTH(tkSymbols)) ||
        dlsym(RTLD_DEFAULT, "XmStringCreateLocalized"))
        return True;

    static const char *tkLibs[] = {
        "libtk8.6.dylib", "libtk8.6.so", "libtk8.5.dylib", "libtk8.5.so",
        "libtk4.2.dylib", "libtk4.2.so", "libtk.dylib",    "libtk.so",
    };
    static const char *xmLibs[] = {
        "libXm.5.dylib",
        "libXm.so.5",
        "libXm.dylib",
        "libXm.so",
    };
    for (size_t i = 0; i < ARRAY_LENGTH(tkLibs); i++)
        if (loadedLibraryHasAnySymbol(tkLibs[i], tkSymbols,
                                      ARRAY_LENGTH(tkSymbols)))
            return True;
    for (size_t i = 0; i < ARRAY_LENGTH(xmLibs); i++)
        if (loadedLibraryHasSymbol(xmLibs[i], "XmStringCreateLocalized"))
            return True;
    return False;
}

Bool compatHiDpiPromoteToolkits(void)
{
    /* Promoting a client's X11 geometry from logical points to physical pixels
     * is disabled by default: fixed-size raw Xlib clients keep painting only
     * their requested logical area and leave the rest of a Retina backing
     * blank. LIBX11_COMPAT_HIDPI_PROMOTE opts back into the promotion path for
     * debugging or a future workload that lays out in physical pixels.
     */
    static int promote = -1;
    if (promote < 0) {
        const char *env = getenv("LIBX11_COMPAT_HIDPI_PROMOTE");
        promote = env && env[0] && strcmp(env, "0") != 0;
    }
    return promote ? True : False;
}

Bool initArray(Array *a, size_t initialSize)
{
    if (initialSize != 0) {
        if (initialSize > SIZE_MAX / sizeof(void *))
            return False;
        a->array = malloc(initialSize * sizeof(void *));
        if (!a->array)
            return False;
    } else {
        a->array = NULL;
    }
    a->length = 0;
    a->capacity = initialSize;
    return True;
}

Bool insertArray(Array *a, void *element)
{
    if (a->length == a->capacity) {
        size_t newCapacity = a->capacity + a->capacity / 2;
        if (newCapacity < 8)
            newCapacity = 8;
        if (newCapacity <= a->capacity ||
            newCapacity > SIZE_MAX / sizeof(void *)) {
            return False;
        }
        void *temp = realloc(a->array, newCapacity * sizeof(void *));
        if (!temp)
            return False;
        a->array = temp;
        a->capacity = newCapacity;
    }
    a->array[a->length++] = element;
    return True;
}

void *removeArray(Array *a, size_t index, Bool preserveOrder)
{
    if (index >= a->length)
        abort();

    void *element = a->array[index];
    if (index + 1 < a->length) {
        if (preserveOrder) {
            memmove(&a->array[index], &a->array[index + 1],
                    sizeof(void *) * (a->length - (index + 1)));
        } else {
            a->array[index] = a->array[a->length - 1];
        }
    } else if (!preserveOrder && a->length > 0) {
        a->array[index] = a->array[a->length - 1];
    }
    a->length--;
    return element;
}

Bool equalCmp(void *element, void *arg)
{
    return element == arg;
}

ssize_t findInArray(Array *a, void *element)
{
    return findInArrayNCmp(a, element, 0, &equalCmp);
}

ssize_t findInArrayN(Array *a, void *element, size_t startIndex)
{
    return findInArrayNCmp(a, element, startIndex, &equalCmp);
}

ssize_t findInArrayCmp(Array *a, void *element, Bool (*cmpFunc)(void *, void *))
{
    return findInArrayNCmp(a, element, 0, cmpFunc);
}

ssize_t findInArrayNCmp(Array *a,
                        void *element,
                        size_t startIndex,
                        Bool (*cmpFunc)(void *, void *))
{
    for (size_t i = startIndex; i < a->length; i++) {
        if (cmpFunc(a->array[i], element))
            return (ssize_t) i;
    }
    return -1;
}

void swapArray(Array *a, size_t index1, size_t index2)
{
    if (index1 >= a->length || index2 >= a->length)
        abort();
    void *tmp = a->array[index1];
    a->array[index1] = a->array[index2];
    a->array[index2] = tmp;
}

void freeArray(Array *a)
{
    free(a->array);
    a->array = NULL;
    a->length = a->capacity = 0;
}

Bool matchWildcard(const char *wildcard, const char *string)
{
    if (!wildcard || !string)
        return False;

    const char *w = wildcard;
    const char *s = string;
    const char *star = NULL;
    const char *retry = NULL;

    while (*s != '\0') {
        if (*w == '?' || *w == *s) {
            w++;
            s++;
        } else if (*w == '*') {
            star = w++;
            retry = s;
        } else if (star) {
            w = star + 1;
            s = ++retry;
        } else {
            return False;
        }
    }
    while (*w == '*')
        w++;
    return *w == '\0';
}

int XFree(void *data)
{
    // https://tronche.com/gui/x/xlib/display/XFree.html
    free(data);
    return 1;
}

Status XGetGeometry(Display *display,
                    Drawable d,
                    Window *root_return,
                    int *x_return,
                    int *y_return,
                    unsigned int *width_return,
                    unsigned int *height_return,
                    unsigned int *border_width_return,
                    unsigned int *depth_return)
{
    // https://tronche.com/gui/x/xlib/window-information/XGetGeometry.html
    SET_X_SERVER_REQUEST(display, X_GetGeometry);
    *root_return = SCREEN_WINDOW;
    if (IS_TYPE(d, WINDOW)) {
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(d);
        *x_return = windowStruct->x;
        *y_return = windowStruct->y;
        *width_return = windowStruct->w;
        *height_return = windowStruct->h;
        *border_width_return = windowStruct->borderWidth;
        *depth_return = windowStruct->depth == CopyFromParent
                            ? DefaultDepth(display, DefaultScreen(display))
                            : windowStruct->depth;
    } else if (IS_TYPE(d, PIXMAP)) {
        PixmapStruct *pixmapStruct = GET_PIXMAP_STRUCT(d);
        *x_return = 0;
        *y_return = 0;
        *width_return = pixmapStruct->width;
        *height_return = pixmapStruct->height;
        *border_width_return = 0;
        *depth_return = pixmapStruct->depth;
    } else {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    return 1;
}

XSizeHints *XAllocSizeHints()
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XAllocSizeHints.html
    return calloc(1, sizeof(XSizeHints));
}

XClassHint *XAllocClassHint()
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XAllocClassHint.html
    XClassHint *classHint = malloc(sizeof(XClassHint));
    if (classHint) {
        classHint->res_name = NULL;
        classHint->res_class = NULL;
    }
    return classHint;
}
