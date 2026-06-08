#ifndef WINDOWINTERNAL_H
#define WINDOWINTERNAL_H

#include "window.h"

typedef struct _WindowSdlIdMapper {
    Window window;
    Uint32 sdlWindowId;
    struct _WindowSdlIdMapper *next;
} WindowSdlIdMapper;

void initWindowStruct(WindowStruct *windowStruct,
                      int x,
                      int y,
                      unsigned int width,
                      unsigned int height,
                      Visual *visual,
                      Colormap colormap,
                      Bool inputOnly,
                      unsigned long backgroundColor,
                      Pixmap backgroundPixmap);
Bool initScreenWindow(Display *display);
Window getWindowFromId(Uint32 sdlWindowId);
void destroyScreenWindow(Display *display);
void destroyWindow(Display *display, Window window, Bool freeParentData);
Window getContainingWindow(Window window, int x, int y);
Bool addChildToWindow(Window parent, Window child);
Bool insertChildIntoWindow(Window parent, Window child, size_t index);
Bool moveChildToIndex(Window window, size_t targetIndex);
void removeChildFromParent(Window child);
Bool windowsOverlap(Window a, Window b);
void resizeWindowTexture(Window window);
void deleteWindowMapping(Window window);
void registerWindowMapping(Window window, Uint32 sdlWindowId);
Bool isParent(Window window1, Window window2);
Bool isWindowEffectivelyViewable(Window window);
WindowProperty *findProperty(Array *properties, Atom property, size_t *index);
void freeWindowProperty(WindowProperty *property);
Bool mergeWindowDrawables(Window parent, Window child);
void mapRequestedChildren(Display *display, Window window);
Bool configureWindow(Display *display,
                     Window window,
                     unsigned long value_mask,
                     XWindowChanges *values);

#endif /* WINDOWINTERNAL_H */
