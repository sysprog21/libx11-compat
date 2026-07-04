/*
 * Expose event synthesis
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 *
 * Recursive helpers that fan damaged-area lists out to mapped descendants and
 * emit synthetic ConfigureNotify / Expose pairs after host resizes. The
 * implementation is purely Xlib-side bookkeeping; SDL event-pump coordination
 * stays in events.c so the pipe wake / pump-thread accounting has a single
 * owner.
 */

#include <X11/Xlib.h>
#include "sdl-compat.h"
#include <stdlib.h>

#include "events.h"
#include "events-expose.h"
#include "window-internal.h"

void clearWindowTreeWithoutExpose(Display *display, Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW) || IS_INPUT_ONLY(window) ||
        GET_WINDOW_STRUCT(window)->mapState == UnMapped) {
        return;
    }
    XClearArea(display, window, 0, 0, 0, 0, False);
    Window *children = GET_CHILDREN(window);
    size_t childCount = GET_WINDOW_STRUCT(window)->children.length;
    for (size_t i = 0; i < childCount; i++)
        clearWindowTreeWithoutExpose(display, children[i]);
}

void postFullWindowExpose(Display *display, Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW) || IS_INPUT_ONLY(window) ||
        GET_WINDOW_STRUCT(window)->mapState != Mapped) {
        return;
    }
    SDL_Rect exposeRect = {0, 0, 0, 0};
    GET_WINDOW_DIMS(window, exposeRect.w, exposeRect.h);
    postExposeEvent(display, window, &exposeRect, 1);
}

void postSyntheticWindowResize(Display *display,
                               Window eventWindow,
                               int width,
                               int height)
{
    if (eventWindow == None || !IS_TYPE(eventWindow, WINDOW) || width <= 0 ||
        height <= 0) {
        return;
    }
    GET_WINDOW_STRUCT(eventWindow)->w = (unsigned int) width;
    GET_WINDOW_STRUCT(eventWindow)->h = (unsigned int) height;
    resizeWindowTexture(eventWindow);
    clearWindowTreeWithoutExpose(display, eventWindow);
    postEvent(display, eventWindow, ConfigureNotify);
    postFullWindowExpose(display, eventWindow);
}

static Bool getRectIntersection(const SDL_Rect *rect1,
                                const SDL_Rect *rect2,
                                SDL_Rect *rectOut)
{
    if (!SDL_IntersectRect(rect1, rect2, rectOut))
        return False;

    rectOut->x -= rect2->x;
    rectOut->y -= rect2->y;
    return True;
}

void postExposeEvent(Display *display,
                     Window window,
                     const SDL_Rect *damagedAreaList,
                     size_t numAreas)
{
    size_t i = numAreas, j;
    while (i-- > 0)
        postEvent(display, window, Expose, &damagedAreaList[i], i);

    SDL_Rect *childDamagedAreaList = malloc(sizeof(SDL_Rect) * numAreas);
    if (!childDamagedAreaList)
        return;

    Window *children = GET_CHILDREN(window);
    for (i = 0; i < GET_WINDOW_STRUCT(window)->children.length; i++) {
        if (!IS_INPUT_ONLY(children[i]) &&
            GET_WINDOW_STRUCT(children[i])->mapState == Mapped) {
            WindowStruct *childWindowStruct = GET_WINDOW_STRUCT(children[i]);
            SDL_Rect childWindowRect = {
                childWindowStruct->x,
                childWindowStruct->y,
                childWindowStruct->w,
                childWindowStruct->h,
            };
            size_t numChildAreas = 0;
            for (j = 0; j < numAreas; j++) {
                if (getRectIntersection(&damagedAreaList[j], &childWindowRect,
                                        &childDamagedAreaList[numChildAreas])) {
                    numChildAreas++;
                }
            }
            if (numChildAreas > 0) {
                postExposeEvent(display, children[i], childDamagedAreaList,
                                numChildAreas);
            }
        }
    }
    free(childDamagedAreaList);
}

void postExposeEventsForMappedChildren(Display *display,
                                       Window window,
                                       const SDL_Rect *damagedAreaList,
                                       size_t numAreas)
{
    if (numAreas == 0 || !damagedAreaList || !IS_TYPE(window, WINDOW))
        return;

    SDL_Rect *childDamagedAreaList = malloc(sizeof(SDL_Rect) * numAreas);
    if (!childDamagedAreaList)
        return;

    Window *children = GET_CHILDREN(window);
    for (size_t i = 0; i < GET_WINDOW_STRUCT(window)->children.length; i++) {
        if (IS_INPUT_ONLY(children[i]) ||
            GET_WINDOW_STRUCT(children[i])->mapState != Mapped) {
            continue;
        }

        WindowStruct *childWindowStruct = GET_WINDOW_STRUCT(children[i]);
        SDL_Rect childWindowRect = {
            childWindowStruct->x,
            childWindowStruct->y,
            childWindowStruct->w,
            childWindowStruct->h,
        };
        size_t numChildAreas = 0;
        for (size_t j = 0; j < numAreas; j++) {
            if (getRectIntersection(&damagedAreaList[j], &childWindowRect,
                                    &childDamagedAreaList[numChildAreas])) {
                numChildAreas++;
            }
        }
        if (numChildAreas > 0) {
            postExposeEvent(display, children[i], childDamagedAreaList,
                            numChildAreas);
        }
    }
    free(childDamagedAreaList);
}
