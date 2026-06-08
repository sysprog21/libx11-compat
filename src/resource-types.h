#ifndef _RESOURCE_TYPES_H_
#define _RESOURCE_TYPES_H_

typedef enum {
    WINDOW = 1,
    DRAWABLE = 2,
    PIXMAP = 3,
    GRAPHICS_CONTEXT = 4,
    FONT = 5,
    CURSOR = 6,
    COLORMAP = 7,
    CLOSED_FONT = 8
} XResourceType;

typedef struct {
    XResourceType type;
    void *dataPointer;
} XID_Struct;

#include "X11/Xlib.h"
#include "errors.h"
#include "window.h"

#define ALLOC_XID() ((XID) malloc(sizeof(XID_Struct)))
#define FREE_XID(id) free((XID_Struct *) (id))
#define SET_XID_TYPE(id, typeId) ((XID_Struct *) (id))->type = typeId
#define SET_XID_VALUE(id, value) ((XID_Struct *) (id))->dataPointer = value
#define GET_XID_TYPE(id) (((XID_Struct *) (id))->type)
#define GET_XID_VALUE(id) (((XID_Struct *) (id))->dataPointer)

/* GET_WINDOW_STRUCT dereferences the XID's data pointer with no NULL
 * guard. Debug builds trip an abort on misuse so the offending call
 * site shows up in the test log instead of a SIGSEGV in unrelated
 * frames. Release builds keep the bare deref to avoid any overhead on
 * the hot path. Uses a GCC/Clang statement expression to evaluate the
 * argument once. */
#ifdef DEBUG_LIBX11_COMPAT
#include <stdio.h>
#include <stdlib.h>
#define GET_WINDOW_STRUCT(window)                                              \
    (__extension__({                                                           \
        Window _gws_w = (window);                                              \
        if (_gws_w == None) {                                                  \
            fprintf(stderr, "%s:%d: GET_WINDOW_STRUCT(None) in debug build\n", \
                    __FILE__, __LINE__);                                       \
            abort();                                                           \
        }                                                                      \
        WindowStruct *_gws_p = (WindowStruct *) GET_XID_VALUE(_gws_w);         \
        if (!_gws_p) {                                                         \
            fprintf(stderr, "%s:%d: GET_WINDOW_STRUCT freed resource %lu\n",   \
                    __FILE__, __LINE__, (unsigned long) _gws_w);               \
            abort();                                                           \
        }                                                                      \
        _gws_p;                                                                \
    }))
#else
#define GET_WINDOW_STRUCT(window) ((WindowStruct *) GET_XID_VALUE(window))
#endif

#define IS_TYPE(resource, typeID)           \
    ((resource) != None &&                  \
     (GET_XID_TYPE(resource) == (typeID) || \
      ((typeID) == DRAWABLE &&              \
       (GET_XID_TYPE(resource) == PIXMAP || \
        (GET_XID_TYPE(resource) == WINDOW && !IS_INPUT_ONLY(resource))))))


#define TYPE_CHECK(resource, typeID, display, returnCode...)                  \
    if (!IS_TYPE(resource, typeID)) {                                         \
        unsigned char errorCode = resourceTypeToErrorCode(typeID);            \
        LOG("Type error: Expected '%s' to be a %s, but was %d!\n", #resource, \
            #typeID, resource != None ? GET_XID_TYPE(resource) : -1);         \
        handleError(0, display, resource, 0, errorCode, 0);                   \
        return returnCode;                                                    \
    }

#endif /* _RESOURCE_TYPES_H_ */
