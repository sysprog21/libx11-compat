#include "window-debug.h"

#ifdef DEBUG_WINDOWS

#include <unistd.h>
#include "window.h"
#include "drawing.h"
#include "util.h"


void printWindowHierarchyOfChild(Window window, char *prepend, int prependLen)
{
    Window *children = GET_CHILDREN(window);
    /* +2: one byte for the per-depth indent character, one for the NUL. */
    char *childPrepend = malloc((size_t) prependLen + 2);
    size_t numChildren = GET_WINDOW_STRUCT(window)->children.length;
    memcpy(childPrepend, prepend, (size_t) prependLen);
    char *charPointer = childPrepend + prependLen;
    *(charPointer + 1) = '\0';
    for (size_t i = 0; i < numChildren; i++) {
        WindowStruct *childStruct = GET_WINDOW_STRUCT(children[i]);
        int x, y, w, h;
        GET_WINDOW_POS(children[i], x, y);
        GET_WINDOW_DIMS(children[i], w, h);
        const char *mapState;
        switch (childStruct->mapState) {
        case UnMapped:
            mapState = "UnMapped";
            break;
        case Mapped:
            mapState = "Mapped";
            break;
        case MapRequested:
            mapState = "MapRequested";
            break;
        default:
            mapState = "Unknown";
        }
        printf(
            "%s+- Window (address: %lu, id: 0x%08lx, x: %d, y: %d, %dx%d, "
            "state: %s)",
            prepend, children[i], childStruct->debugId, x, y, w, h, mapState);

        if (childStruct->sdlRenderer) {
            printf(", sdlRenderer = %p", childStruct->sdlRenderer);
        }
        if (childStruct->sdlWindow) {
            printf(", sdlWindow = %d", SDL_GetWindowID(childStruct->sdlWindow));
        }
        if (childStruct->sdlTexture) {
            int tw, th;
            SDL_QueryTexture(childStruct->sdlTexture, NULL, NULL, &tw, &th);
            printf(", sdlTexture = %p (%d x %d)", childStruct->sdlTexture, tw,
                   th);
        }
        printf("\n");
        *charPointer = (char) (i != numChildren - 1 ? ' ' : '|');
        printWindowHierarchyOfChild(children[i], childPrepend, prependLen + 1);
    }
    free(childPrepend);
}

void printWindowsHierarchy()
{
    printf("- SCREEN_WINDOW (address: %lu, id = 0x%08lx)\n", SCREEN_WINDOW,
           GET_WINDOW_STRUCT(SCREEN_WINDOW)->debugId);
    printWindowHierarchyOfChild(SCREEN_WINDOW, "", 0);
    fflush(stdout);
}

void drawWindowDebugViewForChild(SDL_Renderer *renderer,
                                 Window window,
                                 int absParentX,
                                 int absParentY)
{
    int i;
    SDL_Rect windowRect;
    long drawColor = GET_WINDOW_STRUCT(window)->debugId;
    GET_WINDOW_POS(window, windowRect.x, windowRect.y);
    GET_WINDOW_DIMS(window, windowRect.w, windowRect.h);
    windowRect.x += absParentX;
    windowRect.y += absParentY;
    SDL_SetRenderDrawColor(renderer, ((drawColor >> 24) & 0xFF) * 0.9,
                           ((drawColor >> 16) & 0xFF) * 0.9,
                           ((drawColor >> 8) & 0xFF) * 0.9, 0xFF);
    invalidateSdlDrawStateCache();
    SDL_RenderDrawRect(renderer, &windowRect);
    Window *children = GET_CHILDREN(window);
    for (i = 0; i < GET_WINDOW_STRUCT(window)->children.length; i++) {
        drawWindowDebugViewForChild(renderer, children[i], windowRect.x,
                                    windowRect.y);
    }
}

void drawWindowDebugView()
{
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    int i, j;
    long windowColor;
    for (i = 0; i < GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length; i++) {
        if (GET_WINDOW_STRUCT(children[i])->sdlRenderer) {
            windowColor = GET_WINDOW_STRUCT(children[i])->debugId;
            WindowStruct *windowStruct = GET_WINDOW_STRUCT(children[i]);
            SDL_RenderSetViewport(windowStruct->sdlRenderer, NULL);
            SDL_Rect windowRect;
            GET_WINDOW_POS(children[i], windowRect.x, windowRect.y);
            GET_WINDOW_DIMS(children[i], windowRect.w, windowRect.h);
            SDL_SetRenderDrawColor(
                windowStruct->sdlRenderer, (windowColor >> 24) & 0xFF,
                (windowColor >> 16) & 0xFF, (windowColor >> 8) & 0xFF, 0xFF);
            invalidateSdlDrawStateCache();
            SDL_RenderDrawRect(windowStruct->sdlRenderer, &windowRect);
            Window *topLevelWindowChildren = GET_CHILDREN(children[i]);
            for (j = 0; j < windowStruct->children.length; j++) {
                drawWindowDebugViewForChild(windowStruct->sdlRenderer,
                                            topLevelWindowChildren[j], 0, 0);
            }
            SDL_RenderPresent(windowStruct->sdlRenderer);
        }
    }
}

void drawDebugWindowChildSurfacePlanes(Window child)
{
    int i;
    long windowColor = GET_WINDOW_STRUCT(child)->debugId;
    SDL_Renderer *renderer = getWindowRenderer(child);
    SDL_Rect windowRect;
    SDL_RenderGetViewport(renderer, &windowRect);
    windowRect.x = 0;
    windowRect.y = 0;
    SDL_SetRenderDrawColor(renderer, (windowColor >> 24) & 0xFF,
                           (windowColor >> 16) & 0xFF,
                           (windowColor >> 8) & 0xFF, 0x55);
    invalidateSdlDrawStateCache();
    SDL_RenderFillRect(renderer, &windowRect);
    Window *children = GET_CHILDREN(child);
    for (i = 0; i < GET_WINDOW_STRUCT(child)->children.length; i++) {
        drawDebugWindowChildSurfacePlanes(children[i]);
    }
}

void drawDebugWindowSurfacePlanes()
{
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    int i;
    for (i = 0; i < GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length; i++) {
        if (GET_WINDOW_STRUCT(children[i])->sdlRenderer) {
            SDL_Renderer *renderer =
                GET_WINDOW_STRUCT(children[i])->sdlRenderer;
            SDL_BlendMode oldBlendMode;
            SDL_GetRenderDrawBlendMode(renderer, &oldBlendMode);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            drawDebugWindowChildSurfacePlanes(children[i]);
            SDL_RenderPresent(renderer);
            SDL_SetRenderDrawBlendMode(renderer, oldBlendMode);
            invalidateSdlDrawStateCache();
        }
    }
}

#endif /* DEBUG_WINDOWS */
