#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdint.h>
#include "visual.h"
#include "util.h"
#include "sdl-compat.h"
#include "errors.h"

static Visual *VISUAL_LIST = NULL;
static size_t NUM_VISUALS = 0;
static int VISUAL_DEPTH = 24;
/* The shared Visual table is screen-0 visuals; a per-screen variant would need
 * a separate entry per screen. Keeping this a constant avoids global mutation
 * racing with concurrent visual lookups.
 */
static const int VISUAL_SCREEN = 0;


Bool initVisuals()
{
    if (VISUAL_LIST) {
        LOG("Warn: Visual memory already allocated!\n");
        return True;
    }
    /* Defer NUM_VISUALS so a malloc failure leaves the two globals consistent.
     * freeVisuals() walks NUM_VISUALS entries of VISUAL_LIST, and would
     * otherwise dereference NULL during failure cleanup.
     */
    VISUAL_LIST = malloc(sizeof(Visual));
    if (!VISUAL_LIST) {
        LOG("Out of memory: Failed to allocate memory for the visuals!\n");
        return False;
    }
    NUM_VISUALS = 1;
    Uint32 pixelFormat = SDL_PIXELFORMAT_RGBA8888;
    SDL_DisplayMode displayMode;
    if (SDL_GetCurrentDisplayMode(0, &displayMode) == 0 &&
        displayMode.format != SDL_PIXELFORMAT_UNKNOWN) {
        pixelFormat = displayMode.format;
    }
    int bitsPerPixel = 32;
    Uint32 redMask = 0x00FF0000;
    Uint32 greenMask = 0x0000FF00;
    Uint32 blueMask = 0x000000FF;
    Uint32 alphaMask = 0xFF000000;
    if (!SDL_PixelFormatEnumToMasks(pixelFormat, &bitsPerPixel, &redMask,
                                    &greenMask, &blueMask, &alphaMask)) {
        bitsPerPixel = 32;
    }
    VISUAL_DEPTH = bitsPerPixel == 32 ? 24 : bitsPerPixel;
    VISUAL_LIST->ext_data = NULL;
    VISUAL_LIST->visualid = 0; /* visual id of this visual */
    VISUAL_LIST->CLASS_ATTRIBUTE = TrueColor;
    VISUAL_LIST->red_mask = redMask;
    VISUAL_LIST->green_mask = greenMask;
    VISUAL_LIST->blue_mask = blueMask;
    VISUAL_LIST->bits_per_rgb = 8;  // sizeof(SDL_Color);
    VISUAL_LIST->map_entries = 256;
    return True;
}

void freeVisuals()
{
    if (!VISUAL_LIST) {
        NUM_VISUALS = 0;
        return;
    }
    for (size_t i = 0; i < NUM_VISUALS; i++)
        free(VISUAL_LIST[i].ext_data);
    free(VISUAL_LIST);
    VISUAL_LIST = NULL;
    NUM_VISUALS = 0;
}

Visual *getDefaultVisual(int screenIndex)
{
    (void) screenIndex;
    return &VISUAL_LIST[0];
}

VisualID XVisualIDFromVisual(Visual *visual)
{
    // https://tronche.com/gui/x/xlib/window/XVisualIDFromVisual.html
    return visual->visualid;
}

void fillVisualInfo(XVisualInfo *info, Visual *visual)
{
    info->visual = visual;
    info->visualid = visual->visualid;
    info->CLASS_ATTRIBUTE = visual->CLASS_ATTRIBUTE;
    info->bits_per_rgb = visual->bits_per_rgb;
    info->colormap_size = visual->map_entries;
    info->red_mask = visual->red_mask;
    info->green_mask = visual->green_mask;
    info->blue_mask = visual->blue_mask;
    info->depth = VISUAL_DEPTH;
    info->screen = VISUAL_SCREEN;
}

XVisualInfo *XGetVisualInfo(Display *display,
                            long vinfo_mask,
                            XVisualInfo *vinfo_template,
                            int *nitems_return)
{
    // https://tronche.com/gui/x/xlib/utilities/XGetVisualInfo.html
    Array visualIds;
    if (!initArray(&visualIds, vinfo_mask == VisualNoMask ? NUM_VISUALS : 1)) {
        handleOutOfMemory(0, display, 0, 0);
        *nitems_return = 0;
        return NULL;
    }
    for (unsigned int i = 0; i < NUM_VISUALS; i++) {
        Visual *visual = &VISUAL_LIST[i];
        if (HAS_VALUE(vinfo_mask, VisualIDMask) &&
            visual->visualid != vinfo_template->visualid)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualClassMask) &&
            visual->CLASS_ATTRIBUTE != vinfo_template->CLASS_ATTRIBUTE)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualRedMaskMask) &&
            visual->red_mask != vinfo_template->red_mask)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualGreenMaskMask) &&
            visual->green_mask != vinfo_template->green_mask)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualBlueMaskMask) &&
            visual->blue_mask != vinfo_template->blue_mask)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualColormapSizeMask) &&
            visual->map_entries != vinfo_template->colormap_size)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualBitsPerRGBMask) &&
            visual->bits_per_rgb != vinfo_template->bits_per_rgb)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualScreenMask) &&
            VISUAL_SCREEN != vinfo_template->screen)
            continue;
        if (HAS_VALUE(vinfo_mask, VisualDepthMask) &&
            VISUAL_DEPTH != vinfo_template->depth)
            continue;
        insertArray(&visualIds, (void *) (uintptr_t) i);
    }
    if (visualIds.length == 0) {
        freeArray(&visualIds);
        *nitems_return = 0;
        return NULL;
    }
    XVisualInfo *visualInfo = malloc(sizeof(XVisualInfo) * visualIds.length);
    if (!visualInfo) {
        freeArray(&visualIds);
        handleOutOfMemory(0, display, 0, 0);
        *nitems_return = 0;
        return NULL;
    }
    for (size_t i = 0; i < visualIds.length; i++) {
        Visual *visual =
            &VISUAL_LIST[(unsigned int) (uintptr_t) visualIds.array[i]];
        fillVisualInfo(&visualInfo[i], visual);
    }
    *nitems_return = (int) visualIds.length;
    freeArray(&visualIds);
    return visualInfo;
}

Status XMatchVisualInfo(Display *display,
                        int screen,
                        int depth,
                        int clazz,
                        XVisualInfo *vinfo_return)
{
    // https://tronche.com/gui/x/xlib/utilities/XMatchVisualInfo.html
    if (!VISUAL_LIST) {
        LOG("Visuals memory is not initialized in %s!\n", __func__);
        return 0;
    }
    /* Only the default screen has visuals registered; for any other screen
     * report no match rather than returning a visual whose `screen` field would
     * silently be rewritten to 0 by fillVisualInfo.
     */
    if (screen != VISUAL_SCREEN || screen >= ScreenCount(display) ||
        depth != VISUAL_DEPTH)
        return 0;
    for (size_t i = 0; i < NUM_VISUALS; i++) {
        if (VISUAL_LIST[i].CLASS_ATTRIBUTE == clazz) {
            fillVisualInfo(vinfo_return, &VISUAL_LIST[i]);
            return 1;
        }
    }
    return 0;
}
