#include <X11/Xlib.h>
#define XUTIL_DEFINE_FUNCTIONS
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/Xresource.h>
#include <X11/Xlibint.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>

/* This test deliberately probes the deprecated public API to ensure the library
 * still exports it. Silence the corresponding compiler warning.
 */
#ifdef __clang__
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#undef XGetPixel
#undef XPutPixel
#undef XSubImage
#undef XDestroyImage

#define REF(fn)                                            \
    do {                                                   \
        void (*volatile ref)(void) = (void (*)(void))(fn); \
        (void) ref;                                        \
    } while (0)

int main(void)
{
    REF(XOpenDisplay);
    REF(XCloseDisplay);
    REF(XCreateSimpleWindow);
    REF(XDestroyWindow);
    REF(XDestroySubwindows);
    REF(XMapWindow);
    REF(XMapSubwindows);
    REF(XUnmapWindow);
    REF(XUnmapSubwindows);
    REF(XGetWindowAttributes);
    REF(XGetGeometry);
    REF(XRaiseWindow);
    REF(XLowerWindow);
    REF(XRestackWindows);
    REF(XCreateGC);
    REF(XFreeGC);
    REF(XChangeGC);
    REF(XGetGCValues);
    REF(XSetState);
    REF(XSetGraphicsExposures);
    REF(XSetArcMode);
    REF(XSetFillStyle);
    REF(XSetFillRule);
    REF(XSetClipMask);
    REF(XSetTile);
    REF(XSetStipple);
    REF(XSetClipRectangles);
    REF(XSetRegion);
    REF(XDrawPoint);
    REF(XDrawImageString);
    REF(XDrawImageString16);
    REF(XClearArea);
    REF(XSetWindowBackgroundPixmap);
    REF(XCreateBitmapFromData);
    REF(XCreatePixmapFromBitmapData);
    REF(XListPixmapFormats);
    REF(XGetKeyboardMapping);
    REF(XKeysymToKeycode);
    REF(XKeycodeToKeysym);
    REF(XLookupKeysym);
    REF(XDisplayKeycodes);
    REF(XGetModifierMapping);
    REF(XFreeModifiermap);
    REF(XkbKeysymToModifiers);
    REF(XkbLookupKeySym);
    REF(XkbUseExtension);
    REF(XkbQueryExtension);
    REF(XkbSetDetectableAutoRepeat);
    REF(XInternAtom);
    REF(XInternAtoms);
    REF(XGetAtomName);
    REF(XGetAtomNames);
    REF(XCheckTypedEvent);
    REF(XCheckTypedWindowEvent);
    REF(XCheckWindowEvent);
    REF(XWindowEvent);
    REF(XPutBackEvent);
    REF(XQueryFont);
    REF(XFreeFontInfo);
    REF(XListFontsWithInfo);
    REF(XSaveContext);
    REF(XFindContext);
    REF(XDeleteContext);
    REF(XrmUniqueQuark);
    REF(XrmStringToQuark);
    REF(XrmPermStringToQuark);
    REF(XrmQuarkToString);
    REF(XrmStringToQuarkList);
    REF(XrmStringToBindingQuarkList);
    REF(XSetWMHints);
    REF(XGetWMHints);
    REF(XSetWMNormalHints);
    REF(XGetWMNormalHints);
    REF(XSetTextProperty);
    REF(XGetTextProperty);
    REF(XStringListToTextProperty);
    REF(XInitExtension);
    REF(XAddExtension);
    REF(XQueryExtension);
    REF(XESetCloseDisplay);
    REF(XGetDefault);
    REF(XImageByteOrder);
    REF(XBitmapUnit);
    REF(XBitmapBitOrder);
    REF(XBitmapPad);
    REF(XRootWindowOfScreen);
    REF(XBlackPixelOfScreen);
    REF(XWhitePixelOfScreen);
    REF(XDefaultColormapOfScreen);
    REF(XDefaultDepthOfScreen);
    REF(XDefaultGCOfScreen);
    REF(XWidthOfScreen);
    REF(XHeightOfScreen);
    REF(XWidthMMOfScreen);
    REF(XHeightMMOfScreen);
    REF(XPlanesOfScreen);
    REF(XCellsOfScreen);
    REF(XMinCmapsOfScreen);
    REF(XMaxCmapsOfScreen);
    REF(XDoesSaveUnders);
    REF(XDoesBackingStore);
    REF(XEventMaskOfScreen);
    REF(XScreenNumberOfScreen);
    REF(XListDepths);
    REF(XDestroyImage);
    REF(XPutPixel);
    REF(XGetPixel);
    REF(XGetImage);
    REF(XGetSubImage);
    REF(XPutImage);
    REF(XSubImage);
    REF(XCreateRegion);
    REF(XUnionRegion);
    REF(XIntersectRegion);
    REF(XSubtractRegion);
    REF(XXorRegion);
    REF(XEqualRegion);
    REF(XOffsetRegion);
    REF(XPointInRegion);
    REF(XPolygonRegion);
    REF(XShrinkRegion);
    REF(XUnionRectWithRegion);
    REF(XRectInRegion);
    REF(XClipBox);
    REF(XEmptyRegion);
    REF(XDestroyRegion);
    REF(XDefineCursor);
    REF(XUndefineCursor);
    REF(XCreateFontCursor);
    REF(XCreatePixmapCursor);
    REF(XCreateGlyphCursor);
    REF(XRecolorCursor);
    REF(XFreeCursor);
    REF(XMaxRequestSize);
    REF(XExtendedMaxRequestSize);
    REF(XQueryBestSize);
    REF(XQueryBestTile);
    REF(XQueryBestStipple);
    REF(XGetErrorText);
    REF(XGetErrorDatabaseText);
    REF(XGetPointerControl);
    REF(XChangePointerControl);
    REF(XGetKeyboardControl);
    REF(XChangeKeyboardControl);
    REF(XSetIOErrorHandler);
    REF(XSyncQueryExtension);
    REF(XSyncInitialize);
    REF(XSyncListSystemCounters);
    REF(XSyncCreateCounter);
    REF(XSyncDestroyCounter);
    REF(XSyncCreateFence);
    REF(XShapeQueryExtension);
    REF(XShapeQueryVersion);
    REF(XShapeCombineRegion);
    REF(XShmQueryExtension);
    REF(XShmGetEventBase);
    REF(XShmQueryVersion);
    REF(XShmGetImage);
    REF(XShmPutImage);
    REF(XRenderQueryExtension);
    REF(XRenderQueryVersion);
    REF(XRenderFindStandardFormat);
    REF(XRenderFindVisualFormat);
    REF(XRenderCreatePicture);
    REF(XRenderFreePicture);
    REF(XRenderComposite);
    REF(XRenderFillRectangle);
    REF(XRenderFillRectangles);
    REF(XRenderCreateGlyphSet);
    REF(XRenderAddGlyphs);
    REF(XRenderCompositeText8);
    REF(XRenderCompositeText16);
    REF(XRenderCompositeText32);
    REF(XFixesQueryExtension);
    REF(XDamageQueryExtension);
    REF(XRRQueryExtension);
    REF(XSetSelectionOwner);
    REF(XGetSelectionOwner);
    REF(XConvertSelection);
    REF(XmbLookupString);
    REF(XmbDrawString);
    REF(Xutf8DrawString);
    REF(XwcDrawString);
    REF(XmbTextEscapement);
    REF(Xutf8TextEscapement);
    REF(Xutf8TextExtents);
    REF(Xutf8TextPerCharExtents);
    REF(Xutf8TextListToTextProperty);
    REF(Xutf8TextPropertyToTextList);
    REF(Xutf8SetWMProperties);
    REF(XReadBitmapFile);
    REF(XReadBitmapFileData);
    REF(XGetOCValues);
    REF(XGetOMValues);
    REF(XOMOfOC);
    REF(XSetOCValues);
    REF(XWriteBitmapFile);
    REF(XParseGeometry);
    REF(XrmGetStringDatabase);
    REF(XrmGetResource);
    REF(XrmParseCommand);
    REF(XrmPutStringResource);
    REF(XrmDestroyDatabase);
    REF(FcPatternCreate);
    REF(FcPatternDestroy);
    REF(FcPatternDuplicate);
    REF(FcPatternReference);
    REF(FcPatternDel);
    REF(FcPatternRemove);
    REF(FcPatternAddMatrix);
    REF(FcUcs4ToUtf8);
    REF(FcUtf8ToUcs4);
    REF(FcNameParse);
    REF(FcConfigSubstitute);
    REF(FcDefaultSubstitute);
    REF(FcFontMatch);
    REF(FcFontList);
    REF(FcFontSort);
    REF(FcFontSetDestroy);
    REF(FcCharSetCreate);
    REF(FcCharSetAddChar);
    REF(FcCharSetHasChar);
    REF(FcCharSetDestroy);
    REF(FcObjectSetCreate);
    REF(FcObjectSetAdd);
    REF(FcObjectSetDestroy);
    REF(FcPatternAdd);
    REF(FcPatternAddString);
    REF(FcPatternAddInteger);
    REF(FcPatternAddCharSet);
    REF(FcPatternAddDouble);
    REF(FcPatternAddBool);
    REF(FcPatternGet);
    REF(FcPatternGetString);
    REF(FcPatternGetInteger);
    REF(FcPatternGetDouble);
    REF(FcPatternGetBool);
    REF(XftInit);
    REF(XftGetVersion);
    REF(XftDefaultHasRender);
    REF(XftDefaultSubstitute);
    REF(XftNameParse);
    REF(XftFontMatch);
    REF(XftFontOpen);
    REF(XftFontOpenName);
    REF(XftFontOpenPattern);
    REF(XftFontOpenXlfd);
    REF(XftFontClose);
    REF(XftDrawCreate);
    REF(XftDrawDestroy);
    REF(XftDrawChange);
    REF(XftDrawSetClipRectangles);
    REF(XftDrawDisplay);
    REF(XftDrawDrawable);
    REF(XftDrawColormap);
    REF(XftDrawVisual);
    REF(XftColorAllocValue);
    REF(XftColorAllocName);
    REF(XftColorFree);
    REF(XftDrawString8);
    REF(XftDrawString16);
    REF(XftDrawString32);
    REF(XftDrawStringUtf8);
    REF(XftDrawStringUtf16);
    REF(XftTextExtents8);
    REF(XftTextExtents16);
    REF(XftTextExtents32);
    REF(XftTextExtentsUtf8);
    REF(XftTextExtentsUtf16);
    REF(XftUtf8Len);
    REF(XftUtf8ToUcs4);
    REF(XftFontCopy);
    REF(XftCharIndex);
    REF(XftCharExists);
    REF(XftGlyphExtents);
    REF(XftDrawGlyphs);
    REF(XftDrawRect);
    return 0;
}
