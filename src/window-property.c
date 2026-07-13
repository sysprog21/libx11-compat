/*
 * Window property storage and retrieval
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 *
 * Owns XChangeProperty / XDeleteProperty / XGetWindowProperty and the
 * synthesized reads (_XSETTINGS_SETTINGS for GTK, default _MOTIF_WM_HINTS for
 * Motif probes) plus the in-line side-effect hooks that route _NET_WM_ICON,
 * _NET_WM_WINDOW_TYPE, _MOTIF_WM_HINTS, WM_TRANSIENT_FOR, _NET_WM_STATE, and
 * WM_NAME / _NET_WM_NAME writes into the SDL window or peer modules
 * (window-wm.c for WM state, window.c for the title detour).
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "atoms.h"
#include "display.h"
#include "errors.h"
#include "events.h"
#include "net-atoms.h"
#include "window.h"

static Bool isNetPropertyWithNoWindowSideEffects(Atom property)
{
    return property == _NET_WM_USER_TIME ||
           property == _NET_WM_USER_TIME_WINDOW ||
           property == _NET_WM_HANDLED_ICONS ||
           property == _NET_WM_ICON_GEOMETRY ||
           property == _NET_WM_ALLOWED_ACTIONS || property == _NET_WM_STRUT ||
           property == _NET_WM_STRUT_PARTIAL;
}

/* Push the stored window icon to SDL, routed to the main event thread when a
 * client worker drives the property change (XChangeProperty / XDeleteProperty).
 * The surface is read from the window struct, so the on-main run always applies
 * the icon current at that point (NULL after a delete).
 */
static void applyWindowIconToSdl(Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        runWindowOpOnMain(applyWindowIconToSdl, window);
        return;
    }
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    SDL_SetWindowIcon(windowStruct->sdlWindow, windowStruct->icon);
}

/* Drop SDL's border for menu / popup / dock window types, routed to the main
 * event thread when a client worker sets _NET_WM_WINDOW_TYPE. Called only after
 * the caller has decided the type wants no border.
 */
static void applyWindowBorderlessToSdl(Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        runWindowOpOnMain(applyWindowBorderlessToSdl, window);
        return;
    }
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;
    SDL_SetWindowBordered(GET_WINDOW_STRUCT(window)->sdlWindow, SDL_FALSE);
}

int XChangeProperty(Display *display,
                    Window window,
                    Atom property,
                    Atom type,
                    int format,
                    int mode,
                    _Xconst unsigned char *data,
                    int numberOfElements)
{
    // https://tronche.com/gui/x/xlib/window-information/XChangeProperty.html
    SET_X_SERVER_REQUEST(display, X_ChangeProperty);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (numberOfElements < 0) {
        LOG("Bad parameter: XChangeProperty got negative element count %d "
            "for property %lu (%s), type %lu (%s), format %d, mode %d.\n",
            numberOfElements, property, getAtomName(display, property), type,
            getAtomName(display, type), format, mode);
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (format != 8 && format != 16 && format != 32) {
        LOG("Bad parameter: XChangeProperty got invalid format %d for "
            "property %lu (%s), type %lu (%s), elements %d, mode %d.\n",
            format, property, getAtomName(display, property), type,
            getAtomName(display, type), numberOfElements, mode);
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (!isNetPropertyWithNoWindowSideEffects(property)) {
        LOG("Changing window property %lu (%s).\n", property,
            getAtomName(display, property));
    }
    if (!isValidAtom(property)) {
        handleError(0, display, property, 0, BadAtom, 0);
        return 0;
    }
    if (!isValidAtom(type)) {
        handleError(0, display, type, 0, BadAtom, 0);
        return 0;
    }
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *windowProperty =
        findProperty(&windowStruct->properties, property, NULL);
    unsigned char *combinedData = NULL, *previousData = NULL;
    unsigned int previousDataLength = 0;
    Bool propertyIsNew = !windowProperty;
    size_t dataTypeSize = format == 8
                              ? sizeof(char)
                              : (format == 16 ? sizeof(short) : sizeof(long));
    if (!propertyIsNew) {
        previousDataLength = windowProperty->dataLength;
        previousData = windowProperty->data;
    } else {
        windowProperty = malloc(sizeof(WindowProperty));
        if (!windowProperty) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        if (!insertArray(&windowStruct->properties, windowProperty)) {
            free(windowProperty);
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        windowProperty->dataFormat = format;
        windowProperty->data = NULL;
        windowProperty->dataLength = 0;
        windowProperty->property = property;
        windowProperty->type = type;
    }
    switch (mode) {
    case PropModeAppend:
    case PropModePrepend:
        if (format != windowProperty->dataFormat ||
            type != windowProperty->type) {
            if (propertyIsNew) {
                removeArray(&windowStruct->properties,
                            windowStruct->properties.length - 1, False);
                free(windowProperty);
            }
            handleError(0, display, None, 0, BadMatch, 0);
            return 0;
        }

        /* Checked add then checked multiply: a malicious caller can craft
         * previousDataLength + numberOfElements to wrap u32, undersizing the
         * allocation before the memcpy of dataTypeSize bytes per element would
         * write past the buffer. The element count is then stored back into an
         * unsigned int dataLength field, so it must fit in UINT_MAX as well.
         */
        if ((size_t) numberOfElements >
                SIZE_MAX - (size_t) previousDataLength ||
            (size_t) previousDataLength + (size_t) numberOfElements >
                UINT_MAX ||
            (size_t) previousDataLength + (size_t) numberOfElements >
                SIZE_MAX / dataTypeSize) {
            if (propertyIsNew) {
                removeArray(&windowStruct->properties,
                            windowStruct->properties.length - 1, False);
                free(windowProperty);
            }
            handleError(0, display, None, 0, BadAlloc, 0);
            return 0;
        }
        combinedData = malloc(dataTypeSize * ((size_t) previousDataLength +
                                              (size_t) numberOfElements));
        if (!combinedData) {
            if (propertyIsNew) {
                removeArray(&windowStruct->properties,
                            windowStruct->properties.length - 1, False);
                free(windowProperty);
            }
            LOG("Out of memory: Failed to allocate space for combined data "
                "in XChangeProperty!\n");
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        if (mode == PropModeAppend) {
            if (previousData) {
                memcpy(combinedData, previousData,
                       dataTypeSize * previousDataLength);
            }
            memcpy(combinedData + dataTypeSize * previousDataLength, data,
                   dataTypeSize * numberOfElements);
        } else {
            memcpy(combinedData, data, dataTypeSize * numberOfElements);
            if (previousData) {
                memcpy(combinedData + dataTypeSize * numberOfElements,
                       previousData, dataTypeSize * previousDataLength);
            }
        }
        windowProperty->data = combinedData;
        windowProperty->dataLength = numberOfElements + previousDataLength;
        break;
    case PropModeReplace: {
        /* Same overflow surface as the append/prepend path: a large
         * numberOfElements times dataTypeSize (up to 8 for format == 32) can
         * wrap size_t before malloc, and would then drive memcpy past the
         * undersized buffer.
         *
         * Allocate into a temporary so an OOM here leaves the original
         * windowProperty->data intact instead of orphaning it with the pointer
         * overwritten to NULL.
         */
        unsigned char *newData = NULL;
        if (numberOfElements > 0) {
            if ((size_t) numberOfElements > SIZE_MAX / dataTypeSize) {
                if (propertyIsNew) {
                    removeArray(&windowStruct->properties,
                                windowStruct->properties.length - 1, False);
                    free(windowProperty);
                }
                handleError(0, display, None, 0, BadAlloc, 0);
                return 0;
            }
            newData = malloc(dataTypeSize * (size_t) numberOfElements);
            if (!newData) {
                if (propertyIsNew) {
                    removeArray(&windowStruct->properties,
                                windowStruct->properties.length - 1, False);
                    free(windowProperty);
                }
                LOG("Out of memory: Failed to allocate space for data in "
                    "XChangeProperty!\n");
                handleOutOfMemory(0, display, 0, 0);
                return 0;
            }
            memcpy(newData, data, dataTypeSize * (size_t) numberOfElements);
        }
        windowProperty->data = newData;
        windowProperty->dataLength = (unsigned int) numberOfElements;
        windowProperty->property = property;
        windowProperty->type = type;
        windowProperty->dataFormat = format;
        break;
    }
    default:
        if (propertyIsNew) {
            removeArray(&windowStruct->properties,
                        windowStruct->properties.length - 1, False);
            free(windowProperty);
        }
        LOG("Bad parameter: Got unknown mode %d in XChangeProperty!\n", mode);
        handleError(0, display, None, 0, BadMatch, 0);
        return 0;
    }
    if (previousData)
        free(previousData);
    if (property == _NET_WM_ICON && format == 32 && numberOfElements >= 2) {
        // Find the icon with the highest resolution
        unsigned long *pixelData = (unsigned long *) windowProperty->data;
        unsigned long *icons[20];
        int i = 0, bestIcon = 0;
        size_t offset = 0;
        unsigned long w, h;
        while (i < 20 && offset + 2 <= (size_t) numberOfElements) {
            w = pixelData[0];
            h = pixelData[1];
            /* _NET_WM_ICON pixel data is CARD32 width, height, then w*h BGRA
             * pixels. Both w and h come straight from the client, so w*h plus
             * the two-word header must be guarded against size_t wrap before it
             * feeds the bounds check below.
             */
            if (w > 0 && h > (SIZE_MAX - 2) / w)
                break;
            size_t iconLength = 2 + (size_t) w * (size_t) h;
            if (offset + iconLength > (size_t) numberOfElements)
                break;
            /* SDL_CreateRGBSurfaceFrom takes width, height and pitch as signed
             * int. Reject candidates that would not fit before they can win the
             * bestIcon selection: otherwise an oversized icon followed by a
             * smaller valid icon would leave no usable surface at all.
             */
            Bool fitsSdl = w != 0 && h != 0 && w <= (unsigned long) INT_MAX &&
                           h <= (unsigned long) INT_MAX &&
                           w <= (unsigned long) INT_MAX / 4;
            if (fitsSdl) {
                icons[i] = pixelData;
                if (i == 0 || w > icons[bestIcon][0] || h > icons[bestIcon][1])
                    bestIcon = i;
                i++;
            }
            pixelData += iconLength;
            offset += iconLength;
        }
        if (i > 0) {
            w = icons[bestIcon][0];
            h = icons[bestIcon][1];
            SDL_Surface *icon = SDL_CreateRGBSurfaceFrom(
                &icons[bestIcon][2], (int) w, (int) h, 32, (int) (w * 4),
                0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
            if (windowStruct->icon)
                SDL_FreeSurface(windowStruct->icon);
            windowStruct->icon = icon;
            applyWindowIconToSdl(window);
        }
    }
    /* React to _NET_WM_WINDOW_TYPE: menu/popup/tooltip/splash/dock types
     * normally come from a borderless window-manager decoration request.
     * libx11-compat has no WM, so the closest equivalent is dropping SDL's
     * border.
     */
    if (property == _NET_WM_WINDOW_TYPE && format == 32 &&
        IS_MAPPED_TOP_LEVEL_WINDOW(window) && windowProperty->dataLength > 0) {
        Atom *types = (Atom *) windowProperty->data;
        Bool wantBorderless = False;
        for (unsigned int t = 0; t < windowProperty->dataLength; t++) {
            Atom ty = types[t];
            if (ty == _NET_WM_WINDOW_TYPE_MENU ||
                ty == _NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
                ty == _NET_WM_WINDOW_TYPE_POPUP_MENU ||
                ty == _NET_WM_WINDOW_TYPE_TOOLTIP ||
                ty == _NET_WM_WINDOW_TYPE_SPLASH ||
                ty == _NET_WM_WINDOW_TYPE_DOCK ||
                ty == _NET_WM_WINDOW_TYPE_NOTIFICATION ||
                ty == _NET_WM_WINDOW_TYPE_COMBO) {
                wantBorderless = True;
                break;
            }
        }
        if (wantBorderless)
            applyWindowBorderlessToSdl(window);
    }
    /* A Motif XmDialogShell writes _MOTIF_WM_HINTS during widget realization to
     * clear MWM_DECOR_BORDER or MWM_FUNC_RESIZE. The write handler forwards the
     * decoded bits to SDL so dialogs are not resizable when their hints forbid
     * it and so tooltips and menus arrive borderless. realizeTopLevelWindow
     * creates the SDL window; map/reparent paths replay the write once the
     * top-level is mapped.
     */
    if (property == _MOTIF_WM_HINTS && format == 32)
        applyMotifWmHintsFromProperty(window);
    /* WM_NORMAL_HINTS (XSizeHints) decides resizability the way a real WM does
     * for clients that never speak Motif: resizable unless the client pinned a
     * fixed size. Clients that write it directly via XChangeProperty (rather
     * than XSetWMNormalHints) still get the resizable mirror here; the apply
     * defers to _MOTIF_WM_HINTS when that governs functions.
     */
    if (property == XA_WM_NORMAL_HINTS && format == 32) {
        applyNormalHintsResizableFromProperty(window);
        /* XSetWMSizeHints caches the resize increments straight from the
         * XSizeHints struct, but a client that writes WM_NORMAL_HINTS through a
         * raw XChangeProperty (as here) bypasses that path; decode the stored
         * property so the live-resize drag-end snap still sees the increments.
         * Unlike the resizable mirror above this runs regardless of mapped
         * state, since the snap reads the cache lazily at drag end.
         */
        cacheResizeIncrementsFromNormalHintsProperty(window);
    }
    /* WM_TRANSIENT_FOR establishes a parent / popup link (ICCCM 4.1.2.6). Plain
     * transient_for does NOT make the child modal; SDL's modal hook is only
     * safe when the child is actually marked modal via _NET_WM_STATE_MODAL or
     * MWM_INPUT_FULL_APPLICATION_MODAL. Defer until realize when either window
     * has no SDL backing yet.
     */
    if (property == XA_WM_TRANSIENT_FOR && format == 32 &&
        windowProperty->dataLength >= 1 && windowProperty->data) {
        windowStruct->deferredTransientParent =
            *((Window *) windowProperty->data);
        windowStruct->deferredTransientApplied = False;
        applyTransientForRelationship(window);
    }
    /* A direct write to _NET_WM_STATE drives both the SDL flag mirror
     * (fullscreen / maximized / above) and the modal transient_for pairing,
     * since the modal decision depends on this exact state set. Top-level
     * map/reparent paths run the same apply for writes that arrived before the
     * SDL window existed.
     */
    if (property == _NET_WM_STATE && format == 32) {
        applyNetWmStateFromProperty(window);
        applyTransientForRelationship(window);
    }

    /* Motif, GTK, and Qt set their window titles via XChangeProperty on WM_NAME
     * / _NET_WM_NAME rather than calling XStoreName. The handler routes any
     * format-8 string-encoded write through XStoreName so SDL's window title
     * reflects what the client just asked for. Non-string property types (e.g.
     * atom lists) pass through untouched.
     */
    if (format == 8 && (property == XA_WM_NAME || property == _NET_WM_NAME)) {
        /* Re-resolve per write. freeAtomStorage drops dynamic atoms on the last
         * XCloseDisplay without resetting lastUsedAtom, so a static cache would
         * dangle to a stale id across a close/open cycle and silently stop
         * matching modern toolkit writes.
         */
        Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
        Atom compound = XInternAtom(display, "COMPOUND_TEXT", False);
        if (type == XA_STRING || type == utf8 || type == compound) {
            /* Property bytes are not required to be NUL-terminated. */
            size_t copyLen = (size_t) numberOfElements;
            char *titleBuf = malloc(copyLen + 1);
            if (titleBuf) {
                if (copyLen > 0 && windowProperty->data)
                    memcpy(titleBuf, windowProperty->data, copyLen);
                titleBuf[copyLen] = '\0';
                XStoreName(display, window, titleBuf);
                free(titleBuf);
            }
        }
    }
    postEvent(display, window, PropertyNotify, property, PropertyNewValue);
    return 1;
}

int XDeleteProperty(Display *display, Window window, Atom property)
{
    // https://tronche.com/gui/x/xlib/window-information/XDeleteProperty.html
    SET_X_SERVER_REQUEST(display, X_DeleteProperty);
    TYPE_CHECK(window, WINDOW, display, 0);
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!isValidAtom(property)) {
        handleError(0, display, property, 0, BadAtom, 0);
        return 0;
    }

    if (property == _NET_WM_ICON) {
        if (windowStruct->icon) {
            SDL_FreeSurface(windowStruct->icon);
            windowStruct->icon = NULL;
            applyWindowIconToSdl(window);
        }
    }
    if (property == XA_WM_TRANSIENT_FOR)
        clearTransientForRelationship(window);
    size_t index;
    WindowProperty *windowProperty =
        findProperty(&windowStruct->properties, property, &index);
    if (windowProperty) {
        removeArray(&windowStruct->properties, index, False);
        freeWindowProperty(windowProperty);
        postEvent(display, window, PropertyNotify, property, PropertyDelete);
    }

    /* Re-resolve SDL flag mirror and modal pairing after the property is
     * actually gone: applyNetWmStateFromProperty and
     * applyTransientForRelationship both read _NET_WM_STATE via findProperty,
     * so the read must see the post-delete state.
     */
    if (property == _NET_WM_STATE) {
        applyNetWmStateFromProperty(window);
        applyTransientForRelationship(window);
    }
    return 1;
}

int XGetWindowProperty(Display *display,
                       Window window,
                       Atom property,
                       long long_offset,
                       long long_length,
                       Bool delete,
                       Atom req_type,
                       Atom *actual_type_return,
                       int *actual_format_return,
                       unsigned long *numberOfItems_return,
                       unsigned long *bytes_after_return,
                       unsigned char **prop_return)
{
    // https://tronche.com/gui/x/xlib/window-information/XGetWindowProperty.html
    SET_X_SERVER_REQUEST(display, X_GetProperty);
    TYPE_CHECK(window, WINDOW, display, BadWindow);
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!isValidAtom(property)) {
        handleError(0, display, property, 0, BadAtom, 0);
        return BadAtom;
    }
    *actual_type_return = None;
    *actual_format_return = 0;
    *numberOfItems_return = 0;
    *bytes_after_return = 0;
    *prop_return = NULL;
    WindowProperty *windowProperty =
        findProperty(&windowStruct->properties, property, NULL);
    if (windowProperty) {
        *actual_type_return = windowProperty->type;
        *actual_format_return = windowProperty->dataFormat;
        size_t storageTypeSize =
            windowProperty->dataFormat == 8
                ? sizeof(char)
                : (windowProperty->dataFormat == 16 ? sizeof(short)
                                                    : sizeof(long));
        if (req_type == AnyPropertyType || req_type == windowProperty->type) {
            /* Xlib specifies long_offset and long_length in 32-bit units,
             * independent of the property format.
             */
            size_t wireTypeSize = (size_t) windowProperty->dataFormat / 8;
            size_t itemsPerLong = 4 / wireTypeSize;
            size_t totalItems = windowProperty->dataLength;
            if (long_offset < 0 || long_length < 0 ||
                (size_t) long_offset > SIZE_MAX / itemsPerLong ||
                (size_t) long_length > SIZE_MAX / itemsPerLong) {
                handleError(0, display, None, 0, BadValue, 0);
                return BadValue;
            }
            size_t offsetItems = (size_t) long_offset * itemsPerLong;
            if (offsetItems > totalItems) {
                handleError(0, display, None, 0, BadValue, 0);
                return BadValue;
            }
            size_t remainingItems = totalItems - offsetItems;
            size_t requestItems = (size_t) long_length * itemsPerLong;
            size_t returnItems = MIN(remainingItems, requestItems);
            /* dataReturnSize then dataReturnSize+1 must both fit in size_t. */
            if (returnItems > (SIZE_MAX - 1) / storageTypeSize) {
                handleError(0, display, None, 0, BadAlloc, 0);
                return BadAlloc;
            }
            size_t dataReturnSize = returnItems * storageTypeSize;
            *prop_return = malloc(dataReturnSize + 1);
            if (!*prop_return) {
                LOG("Out of memory: Failed to allocate space for "
                    "the return value in XGetWindowProperty!\n");
                handleOutOfMemory(0, display, 0, 0);
                return BadAlloc;
            }
            /* dataReturnSize == 0 hits two UBSan traps: NULL + 0 pointer
             * arithmetic on an empty property (windowProperty->data is NULL
             * when dataLength is 0), and memcpy(dst, NULL, 0) which the
             * standard leaves undefined. Skip the copy and just NUL-terminate.
             */
            if (dataReturnSize > 0) {
                memcpy(*prop_return,
                       windowProperty->data + offsetItems * storageTypeSize,
                       dataReturnSize);
            }
            (*prop_return)[dataReturnSize] = '\0';
            *numberOfItems_return = returnItems;
            *bytes_after_return = (remainingItems - returnItems) * wireTypeSize;
            if (delete && *bytes_after_return == 0)
                XDeleteProperty(display, window, property);
        } else {
            *bytes_after_return = (unsigned long) windowProperty->dataLength *
                                  ((size_t) windowProperty->dataFormat / 8);
            *numberOfItems_return = 0;
        }
    } else if (property == _XSETTINGS_SETTINGS_ATOM &&
               window == SCREEN_WINDOW &&
               (req_type == AnyPropertyType ||
                req_type == _XSETTINGS_SETTINGS_ATOM)) {
        /* Publish the minimum settings GTK reads: a font name, the DPI in
         * fixed-point 1024ths of a point, and an icon theme name.
         */
        static const char *fontName = "Sans 10";
        static const char *iconTheme = "default";
        /* Each setting:
         *   CARD8 type, CARD8 unused, CARD16 name_len, char name[name_len],
         *   pad to 4, CARD32 last_change_serial, type-specific body.
         * Header: CARD8 byte_order, 3*CARD8 unused, CARD32 serial,
         *         CARD32 n_settings.
         */
        size_t headerBytes = 12;
        struct {
            const char *name;
            int type; /* 0=int, 1=string */
            int intValue;
            const char *strValue;
        } items[] = {
            {
                .name = "Gtk/FontName",
                .type = 1,
                .intValue = 0,
                .strValue = fontName,
            },
            {
                .name = "Xft/DPI",
                .type = 0,
                .intValue = 96 * 1024,
                .strValue = NULL,
            },
            {
                .name = "Net/IconThemeName",
                .type = 1,
                .intValue = 0,
                .strValue = iconTheme,
            },
        };
        const size_t nItems = sizeof(items) / sizeof(items[0]);
        size_t total = headerBytes;
        for (size_t i = 0; i < nItems; i++) {
            size_t nameLen = strlen(items[i].name);
            size_t entry = 1 + 1 + 2 + nameLen;
            entry = (entry + 3) & ~3u;
            entry += 4; /* serial */
            if (items[i].type == 0) {
                entry += 4;
            } else {
                size_t valLen = strlen(items[i].strValue);
                size_t v = 4 + valLen;
                v = (v + 3) & ~3u;
                entry += v;
            }
            total += entry;
        }
        unsigned char *out = malloc(total + 1);
        if (!out) {
            handleOutOfMemory(0, display, 0, 0);
            return BadAlloc;
        }
        unsigned char *p = out;
        *p++ = 0; /* LSBFirst */
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        uint32_t serial = 1;
        memcpy(p, &serial, 4);
        p += 4;
        uint32_t nset = (uint32_t) nItems;
        memcpy(p, &nset, 4);
        p += 4;
        for (size_t i = 0; i < nItems; i++) {
            *p++ = (unsigned char) items[i].type;
            *p++ = 0;
            uint16_t nameLen = (uint16_t) strlen(items[i].name);
            memcpy(p, &nameLen, 2);
            p += 2;
            memcpy(p, items[i].name, nameLen);
            p += nameLen;
            size_t pad = (4 - (nameLen & 3)) & 3;
            memset(p, 0, pad);
            p += pad;
            uint32_t lastChange = 0;
            memcpy(p, &lastChange, 4);
            p += 4;
            if (items[i].type == 0) {
                int32_t iv = (int32_t) items[i].intValue;
                memcpy(p, &iv, 4);
                p += 4;
            } else {
                uint32_t valLen = (uint32_t) strlen(items[i].strValue);
                memcpy(p, &valLen, 4);
                p += 4;
                memcpy(p, items[i].strValue, valLen);
                p += valLen;
                size_t spad = (4 - (valLen & 3)) & 3;
                memset(p, 0, spad);
                p += spad;
            }
        }
        out[total] = '\0';
        *actual_type_return = _XSETTINGS_SETTINGS_ATOM;
        *actual_format_return = 8;
        *numberOfItems_return = total;
        *bytes_after_return = 0;
        *prop_return = out;
    } else if (property == _MOTIF_WM_HINTS &&
               (req_type == AnyPropertyType || req_type == _MOTIF_WM_HINTS)) {
        /* Synthesize a default hints reply when the app has not set one, so
         * Motif/Tk/AWT clients that probe this property do not abort. X11
         * format=32 uses native long-sized client-side storage even though the
         * wire encoding is 32 bits. Reuse the shared MWM_* flag layout from
         * window-internal.h to keep the synthesized values and the
         * write-routing decoder in sync.
         */
        size_t bytes = 5 * sizeof(long);
        long *values = malloc(bytes + 1);
        if (!values) {
            handleOutOfMemory(0, display, 0, 0);
            return BadAlloc;
        }
        values[0] =
            MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS | MWM_HINTS_INPUT_MODE;
        values[1] = MWM_FUNC_ALL;
        values[2] = MWM_DECOR_ALL;
        values[3] = MWM_INPUT_MODELESS;
        values[4] = 0;
        *actual_type_return = _MOTIF_WM_HINTS;
        *actual_format_return = 32;
        *numberOfItems_return = 5;
        *bytes_after_return = 0;
        *prop_return = (unsigned char *) values;
    } else {
        *prop_return = NULL;
    }
    return Success;
}
