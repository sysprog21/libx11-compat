#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>
#include <X11/Xatom.h>
#include <X11/Xmu/Atoms.h>
#include <X11/Xmu/CharSet.h>
#include <X11/Xmu/Converters.h>
#include <X11/Xmu/StdCmap.h>
#include <X11/Xmu/StdSel.h>
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

static int atom_list_contains(const Atom *atoms, unsigned long count, Atom atom)
{
    for (unsigned long i = 0; i < count; i++) {
        if (atoms[i] == atom)
            return 1;
    }
    return 0;
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

    /* Pin every Xmu converter declared in Converters.h so dropping any symbol
     * from compat/xmu-compat.c fails at link rather than at the first
     * downstream demo that registers it.
     */
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
        (void *) XmuConvertStandardSelection,
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

    int local_argc = 1;
    char *local_argv[] = {(char *) "test_xmu_link", NULL};
    XtToolkitInitialize();
    XtAppContext app = XtCreateApplicationContext();
    CHECK(app != NULL, "XtCreateApplicationContext failed");

    Display *display = XtOpenDisplay(app, NULL, "xmuSmoke", "XmuSmoke", NULL, 0,
                                     &local_argc, local_argv);
    CHECK(display != NULL, "XtOpenDisplay failed");

    Arg shell_args[1];
    XtSetArg(shell_args[0], XtNtitle, "Xmu Smoke Title");
    Widget shell =
        XtAppCreateShell("xmuSmoke", "XmuSmoke", applicationShellWidgetClass,
                         display, shell_args, 1);
    CHECK(shell != NULL, "XtAppCreateShell failed");
    Widget child =
        XtCreateManagedWidget("selectionChild", widgetClass, shell, NULL, 0);
    CHECK(child != NULL, "XtCreateManagedWidget failed");

    Atom atom = XA_CLIENT_WINDOW(display);
    CHECK(atom != None, "XA_CLIENT_WINDOW returned None");

    Atom selection = XA_PRIMARY;
    Atom target = XA_CLASS(display);
    Atom type = None;
    XPointer value = NULL;
    unsigned long length = 0;
    int format = 0;
    CHECK(XmuConvertStandardSelection(shell, CurrentTime, &selection, &target,
                                      &type, &value, &length, &format),
          "XmuConvertStandardSelection CLASS failed");
    CHECK(type == XA_STRING && format == 8,
          "XmuConvertStandardSelection CLASS returned wrong type or format");
    CHECK(length == strlen("xmuSmoke") + strlen("XmuSmoke") + 2,
          "XmuConvertStandardSelection CLASS returned wrong length");
    CHECK(!strcmp((char *) value, "xmuSmoke"),
          "XmuConvertStandardSelection CLASS returned wrong instance");
    CHECK(!strcmp((char *) value + strlen("xmuSmoke") + 1, "XmuSmoke"),
          "XmuConvertStandardSelection CLASS returned wrong class");
    XtFree(value);

    Widget second_shell =
        XtAppCreateShell("secondarySmoke", "SecondaryClass",
                         applicationShellWidgetClass, display, NULL, 0);
    CHECK(second_shell != NULL, "secondary XtAppCreateShell failed");
    Widget second_child = XtCreateManagedWidget("secondaryChild", widgetClass,
                                                second_shell, NULL, 0);
    CHECK(second_child != NULL, "secondary child creation failed");

    value = NULL;
    length = 0;
    CHECK(XmuConvertStandardSelection(second_child, CurrentTime, &selection,
                                      &target, &type, &value, &length, &format),
          "XmuConvertStandardSelection secondary CLASS failed");
    CHECK(type == XA_STRING && format == 8,
          "secondary CLASS returned wrong type or format");
    CHECK(length == strlen("secondarySmoke") + strlen("SecondaryClass") + 2,
          "secondary CLASS returned wrong length");
    CHECK(!strcmp((char *) value, "secondarySmoke"),
          "secondary CLASS returned wrong instance");
    CHECK(!strcmp((char *) value + strlen("secondarySmoke") + 1,
                  "SecondaryClass"),
          "secondary CLASS returned Display-wide class instead of shell class");
    XtFree(value);

    target = XA_NAME(display);
    value = NULL;
    length = 0;
    CHECK(XmuConvertStandardSelection(child, CurrentTime, &selection, &target,
                                      &type, &value, &length, &format),
          "XmuConvertStandardSelection NAME failed");
    CHECK(type == XA_STRING && format == 8,
          "XmuConvertStandardSelection NAME returned wrong type or format");
    CHECK(length == strlen("Xmu Smoke Title"),
          "XmuConvertStandardSelection NAME returned wrong length");
    CHECK(!strcmp((char *) value, "Xmu Smoke Title"),
          "XmuConvertStandardSelection NAME returned wrong title");
    XtFree(value);

    target = XA_TARGETS(display);
    value = NULL;
    length = 0;
    CHECK(XmuConvertStandardSelection(shell, CurrentTime, &selection, &target,
                                      &type, &value, &length, &format),
          "XmuConvertStandardSelection TARGETS failed");
    CHECK(type == XA_ATOM && format == 32 && length == 8,
          "XmuConvertStandardSelection TARGETS returned wrong shape");
    CHECK(atom_list_contains((Atom *) value, length, XA_TARGETS(display)),
          "XmuConvertStandardSelection TARGETS did not advertise TARGETS");
    CHECK(atom_list_contains((Atom *) value, length, XA_CLASS(display)),
          "XmuConvertStandardSelection TARGETS did not advertise CLASS");
    XtFree(value);

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
    XtDestroyWidget(second_shell);
    XtDestroyWidget(shell);
    XtCloseDisplay(display);
    XtDestroyApplicationContext(app);

    puts("test_xmu_link: ok");
    return 0;
}
