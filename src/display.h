#ifndef _DISPLAY_H
#define _DISPLAY_H

#include "resource-types.h"

#ifdef DEBUG_LIBX11_COMPAT
#include <stdio.h>
#include <stdlib.h>
#define GET_DISPLAY(display)                                             \
    (__extension__({                                                     \
        Display *_gd_d = (display);                                      \
        if (!_gd_d) {                                                    \
            fprintf(stderr, "%s:%d: GET_DISPLAY(NULL) in debug build\n", \
                    __FILE__, __LINE__);                                 \
            abort();                                                     \
        }                                                                \
        (_XPrivDisplay) _gd_d;                                           \
    }))
#else
#define GET_DISPLAY(display) ((_XPrivDisplay) (display))
#endif

void setLastRequestCode(Display *display, unsigned char requestCode);
unsigned char getLastRequestCode(Display *display);

/* Display-global HiDPI factor (physical pixels per logical point), probed once
 * at XOpenDisplay from a hidden throwaway SDL window. macOS reports the Retina
 * backing scale only via a live window, and clients such as xwpe load their
 * fixed-pixel fonts before any window exists, so the factor has to be known up
 * front. It scales the font pixel size and core metrics (src/font.c) to match
 * the physical-pixel window geometry promoted in realizeTopLevelWindow, keeping
 * the terminal grid identical while glyphs render at native resolution. 1.0 on
 * non-HiDPI hosts and under the CI dummy driver, so the scaling is a no-op.
 */
double compatGlobalHiDpiScale(void);
void compatSetGlobalHiDpiScale(double scale);

/* Display-lock handoff support (see src/display.c). A thread about to block on
 * a main-thread handoff (runOnMainThread) releases every level of display lock
 * it holds so the main thread can take the same global lock to run the work,
 * then reacquires the shed depth. Both are no-ops for a thread holding no lock.
 */
int displayLockReleaseForHandoff(void);
Bool displayLockHandoffPending(void);
void displayLockReacquire(int depth);

/* True once a client has called XInitThreads (upstream locking.c installs the
 * lock hooks then, and never for a single-threaded client). Callers use it to
 * skip in-place mutation that would be unsafe under concurrent same-display
 * access, since the compat font/draw paths do not take LockDisplay themselves.
 */
Bool compatDisplayThreadsInitialized(void);

/* Publish the effective HiDPI client scale as a CARDINAL property
 * (_LIBX11_COMPAT_HIDPI_SCALE, scale * 1000 rounded) on the root window, and
 * post the PropertyNotify. Clients that render at a fixed point size (e.g. the
 * xwpe Cairo/Pango backend) read this to match this shim's geometry contract.
 * Promotion-disabled builds publish 1.0 even on a HiDPI host; on a real X
 * server the property is simply absent and clients fall back to 1.0. Called at
 * XOpenDisplay once the root window exists and again whenever a window moves to
 * a display with a different backing scale. No-op when the root window is not
 * yet initialized.
 */
void compatPublishHiDpiScaleProperty(Display *display);

/* Property name and fixed-point encoding shared by the publisher above and any
 * client that reads it. The value is scale * HIDPI_SCALE_PROPERTY_FIXED_POINT
 * so a fractional scale survives the integer CARDINAL transport.
 */
#define HIDPI_SCALE_PROPERTY_NAME "_LIBX11_COMPAT_HIDPI_SCALE"
#define HIDPI_SCALE_PROPERTY_FIXED_POINT 1000

/* True when the SDL library loaded at runtime provides
 * SDL_GetWindowSizeInPixels (added in SDL 2.26.0). The wrapper thunk for that
 * symbol is compiled in whenever the build headers are >= 2.26, but invoking it
 * against an older runtime library resolves a NULL dlsym and aborts, so every
 * caller must gate on this and fall back to the renderer-output-size path.
 * SDL_GetVersion reports the real loaded version and exists in every SDL2. The
 * LIBX11_COMPAT_NO_SIZE_IN_PIXELS env var forces the fallback so the older-SDL
 * path is exercisable on a new host.
 */
Bool compatSdlHasWindowSizeInPixels(void);

/* request is bumped from every Xlib entry point and read for event serials, so
 * the increment is a relaxed atomic to stay data-race free when multiple
 * threads issue requests under XInitThreads. setLastRequestCode is already
 * thread-safe (its own side-table mutex). Read request back with
 * atomicLoadRequest.
 */
#define SET_X_SERVER_REQUEST(display, requestId)                \
    do {                                                        \
        __atomic_add_fetch(&GET_DISPLAY(display)->request, 1,   \
                           __ATOMIC_RELAXED);                   \
        setLastRequestCode(display, (unsigned char) requestId); \
    } while (0)

#define atomicLoadRequest(display) \
    __atomic_load_n(&GET_DISPLAY(display)->request, __ATOMIC_RELAXED)


/* Called when a Display is about to be torn down, so a sibling shim holding a
 * handle on it (libxcb-compat) can invalidate that handle first.
 */
void libx11CompatSetDisplayCloseHook(void (*hook)(Display *display));

#endif  //_DISPLAY_H
