#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Intrinsic.h>
#include <X11/Xmu/Atoms.h>
#include <X11/Xmu/CharSet.h>
#include <X11/Xmu/Converters.h>
#include <X11/Xmu/StdCmap.h>
#include <X11/Xmu/SysUtil.h>
#include <X11/Xmu/WinUtil.h>

#define FAIL(...)                     \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr);          \
        exit(1);                      \
    } while (0)

#define CHECK(cond, ...)       \
    do {                       \
        if (!(cond))           \
            FAIL(__VA_ARGS__); \
    } while (0)

static int callback_seen = 0;

static void callback_proc(Widget widget, XtPointer closure, XtPointer call_data)
{
    (void) widget;
    (void) closure;
    (void) call_data;
    callback_seen++;
}

int main(void)
{
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    char lowered[32];
    XmuCopyISOLatin1Lowered(lowered, "MotifCLIENT");
    CHECK(!strcmp(lowered, "motifclient"),
          "XmuCopyISOLatin1Lowered returned %s", lowered);
    CHECK(XmuCompareISOLatin1("Motif", "motif") == 0,
          "XmuCompareISOLatin1 should ignore case");

    XtCallbackProc proc = callback_proc;
    XtCallbackList callbacks = NULL;
    XrmValue from = {sizeof(proc), (XPointer) &proc};
    XrmValue to = {sizeof(callbacks), (XPointer) &callbacks};
    XmuCvtFunctionToCallback(NULL, NULL, &from, &to);
    CHECK(callbacks && callbacks[0].callback == callback_proc,
          "XmuCvtFunctionToCallback did not build callback list");
    callbacks[0].callback(NULL, NULL, NULL);
    CHECK(callback_seen == 1, "converted callback did not run");
    free(callbacks);

    /* Pin every Xmu converter declared in Converters.h so dropping any
     * symbol from compat/xmu-compat.c fails at link rather than at the
     * first downstream demo that registers it. */
    void *converter_pins[] = {
        (void *) XmuCvtStringToBackingStore,
        (void *) XmuCvtBackingStoreToString,
        (void *) XmuCvtStringToCursor,
        (void *) XmuCvtStringToColorCursor,
        (void *) XmuCvtStringToGravity,
        (void *) XmuCvtGravityToString,
        (void *) XmuCvtStringToJustify,
        (void *) XmuCvtJustifyToString,
        (void *) XmuCvtStringToLong,
        (void *) XmuCvtLongToString,
        (void *) XmuCvtStringToOrientation,
        (void *) XmuCvtOrientationToString,
        (void *) XmuCvtStringToBitmap,
    };
    CHECK(converter_pins[0] != NULL, "converter pinning produced a NULL entry");

    /* Spot-check the StringToOrientation converter wires through. */
    char hbuf[16] = "horizontal";
    XtOrientation orient = (XtOrientation) -1;
    XrmValue ofrom = {(unsigned int) strlen(hbuf) + 1, (XPointer) hbuf};
    XrmValue oto = {sizeof(orient), (XPointer) &orient};
    XmuCvtStringToOrientation(NULL, NULL, &ofrom, &oto);
    CHECK(orient == XtorientHorizontal, "XmuCvtStringToOrientation produced %d",
          (int) orient);

    int backing = WhenMapped;
    XrmValue bfrom = {sizeof(backing), (XPointer) &backing};
    XrmValue bto = {0, NULL};
    CHECK(XmuCvtBackingStoreToString(NULL, NULL, NULL, &bfrom, &bto, NULL),
          "XmuCvtBackingStoreToString failed");
    CHECK(bto.size == sizeof(char *) && bto.addr != NULL,
          "XmuCvtBackingStoreToString returned invalid storage");
    char *backing_string = *(char **) bto.addr;
    CHECK(backing_string && !strcmp(backing_string, "whenMapped"),
          "XmuCvtBackingStoreToString returned %s",
          backing_string ? backing_string : "(null)");

    long number = 42;
    XrmValue lfrom = {sizeof(number), (XPointer) &number};
    XrmValue lto = {0, NULL};
    CHECK(XmuCvtLongToString(NULL, NULL, NULL, &lfrom, &lto, NULL),
          "XmuCvtLongToString failed");
    CHECK(lto.size == sizeof(char *) && lto.addr != NULL,
          "XmuCvtLongToString returned invalid storage");
    char *long_string = *(char **) lto.addr;
    CHECK(long_string && !strcmp(long_string, "42"),
          "XmuCvtLongToString returned %s",
          long_string ? long_string : "(null)");

    char hostname[256];
    CHECK(XmuGetHostname(hostname, (int) sizeof(hostname)) >= 0,
          "XmuGetHostname failed");

    Display *display = XOpenDisplay(NULL);
    CHECK(display != NULL, "XOpenDisplay failed");
    Atom atom = XA_CLIENT_WINDOW(display);
    CHECK(atom != None, "XA_CLIENT_WINDOW returned None");
    Atom rgb_default_map = XInternAtom(display, "RGB_DEFAULT_MAP", False);
    CHECK(
        XmuLookupStandardColormap(
            display, DefaultScreen(display),
            XVisualIDFromVisual(DefaultVisual(display, DefaultScreen(display))),
            DefaultDepth(display, DefaultScreen(display)), rgb_default_map,
            False, False),
        "XmuLookupStandardColormap returned False");

    Window root = DefaultRootWindow(display);
    CHECK(XmuClientWindow(display, root) == root,
          "XmuClientWindow should fall back to the input window");
    CHECK(XmuScreenOfWindow(display, root) == DefaultScreenOfDisplay(display),
          "XmuScreenOfWindow returned wrong screen");
    XCloseDisplay(display);

    puts("test_xmu_link: ok");
    return 0;
}
