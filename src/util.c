#include "X11/Xlib.h"
#include "X11/Xutil.h"
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
