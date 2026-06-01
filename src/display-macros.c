#include "X11/Xlib.h"
#include "X11/Xlibint.h"
#include "reallocarray.h"

int XDisplayWidth(Display *dpy, int scr)
{
    return DisplayWidth(dpy, scr);
}

int XDisplayHeight(Display *dpy, int scr)
{
    return DisplayHeight(dpy, scr);
}

int XDisplayWidthMM(Display *dpy, int scr)
{
    return DisplayWidthMM(dpy, scr);
}

int XDisplayHeightMM(Display *dpy, int scr)
{
    return DisplayHeightMM(dpy, scr);
}

int XBitmapUnit(Display *dpy)
{
    return BitmapUnit(dpy);
}

int XBitmapBitOrder(Display *dpy)
{
    return BitmapBitOrder(dpy);
}

int XBitmapPad(Display *dpy)
{
    return BitmapPad(dpy);
}

unsigned long XAllPlanes()
{
    return AllPlanes;
}

unsigned long XBlackPixel(Display *display, int screen_number)
{
    return BlackPixel(display, screen_number);
}

unsigned long XWhitePixel(Display *display, int screen_number)
{
    return WhitePixel(display, screen_number);
}

int XConnectionNumber(Display *display)
{
    return ConnectionNumber(display);
}

Colormap XDefaultColormap(Display *display, int screen_number)
{
    return DefaultColormap(display, screen_number);
}

int XDefaultDepth(Display *display, int screen_number)
{
    return DefaultDepth(display, screen_number);
}

GC XDefaultGC(Display *display, int screen_number)
{
    return DefaultGC(display, screen_number);
}

Window XDefaultRootWindow(Display *display)
{
    return DefaultRootWindow(display);
}

Screen *XDefaultScreenOfDisplay(Display *display)
{
    return DefaultScreenOfDisplay(display);
}

Screen *XScreenOfDisplay(Display *display, int screen_number)
{
    return ScreenOfDisplay(display, screen_number);
};

Display *XDisplayOfScreen(Screen *screen)
{
    return DisplayOfScreen(screen);
}

int XDefaultScreen(Display *display)
{
    return DefaultScreen(display);
}

Visual *XDefaultVisual(Display *display, int screen_number)
{
    return DefaultVisual(display, screen_number);
}

Visual *XDefaultVisualOfScreen(Screen *screen)
{
    return DefaultVisualOfScreen(screen);
}

Window XRootWindowOfScreen(Screen *screen)
{
    return RootWindowOfScreen(screen);
}

unsigned long XBlackPixelOfScreen(Screen *screen)
{
    return BlackPixelOfScreen(screen);
}

unsigned long XWhitePixelOfScreen(Screen *screen)
{
    return WhitePixelOfScreen(screen);
}

Colormap XDefaultColormapOfScreen(Screen *screen)
{
    return DefaultColormapOfScreen(screen);
}

int XDefaultDepthOfScreen(Screen *screen)
{
    return DefaultDepthOfScreen(screen);
}

GC XDefaultGCOfScreen(Screen *screen)
{
    return DefaultGCOfScreen(screen);
}

int XWidthOfScreen(Screen *screen)
{
    return WidthOfScreen(screen);
}

int XHeightOfScreen(Screen *screen)
{
    return HeightOfScreen(screen);
}

int XWidthMMOfScreen(Screen *screen)
{
    return WidthMMOfScreen(screen);
}

int XHeightMMOfScreen(Screen *screen)
{
    return HeightMMOfScreen(screen);
}

int XPlanesOfScreen(Screen *screen)
{
    return PlanesOfScreen(screen);
}

int XCellsOfScreen(Screen *screen)
{
    return CellsOfScreen(screen);
}

int XMinCmapsOfScreen(Screen *screen)
{
    return MinCmapsOfScreen(screen);
}

int XMaxCmapsOfScreen(Screen *screen)
{
    return MaxCmapsOfScreen(screen);
}

Bool XDoesSaveUnders(Screen *screen)
{
    return DoesSaveUnders(screen);
}

int XDoesBackingStore(Screen *screen)
{
    return DoesBackingStore(screen);
}

long XEventMaskOfScreen(Screen *screen)
{
    return EventMaskOfScreen(screen);
}

int XScreenNumberOfScreen(Screen *screen)
{
    Display *display = DisplayOfScreen(screen);
    for (int i = 0; i < ScreenCount(display); i++) {
        if (ScreenOfDisplay(display, i) == screen) {
            return i;
        }
    }
    return -1;
}

int XDisplayCells(Display *display, int screen_number)
{
    return DisplayCells(display, screen_number);
}

int XDisplayPlanes(Display *display, int screen_number)
{
    return DisplayPlanes(display, screen_number);
}

char *XDisplayString(Display *display)
{
    return DisplayString(display);
}

unsigned long XLastKnownRequestProcessed(Display *display)
{
    return LastKnownRequestProcessed(display);
}

unsigned long XNextRequest(Display *display)
{
    return NextRequest(display);
}

int XProtocolVersion(Display *display)
{
    return ProtocolVersion(display);
}

int XProtocolRevision(Display *display)
{
    return ProtocolRevision(display);
}

int XQLength(Display *display)
{
    return QLength(display);
}

Window XRootWindow(Display *display, int screen_number)
{
    return RootWindow(display, screen_number);
}

int XScreenCount(Display *display)
{
    return ScreenCount(display);
}

char *XServerVendor(Display *display)
{
    return ServerVendor(display);
}

int XVendorRelease(Display *display)
{
    return VendorRelease(display);
}

/*
 * XListDepths - return info from connection setup
 */
int *XListDepths(Display *dpy, int scrnum, int *countp)
{
    Screen *scr;
    int count;
    int *depths;

    if (scrnum < 0 || scrnum >= dpy->nscreens)
        return NULL;

    scr = &dpy->screens[scrnum];
    if ((count = scr->ndepths) > 0) {
        register Depth *dp;
        register int i;

        depths = Xmallocarray(count, sizeof(int));
        if (!depths)
            return NULL;
        for (i = 0, dp = scr->depths; i < count; i++, dp++)
            depths[i] = dp->depth;
    } else {
        /* a screen must have a depth */
        return NULL;
    }
    *countp = count;
    return depths;
}
