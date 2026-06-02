/* libXt Tier 1 micro-tests for libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* Exercise the chokepoints Motif depends on, one per Xt subsystem. Each
 * subtest is independent and stops the binary at the first failure with
 * a specific diagnostic so make check fails loudly. The order matters:
 * subsystems with no Display dependency run first (keysyms), then init
 * happens once, then everything that needs an app context or widget runs
 * against the shared shell.
 *
 *   keysyms     - XConvertCase across ASCII, Latin-1, and function keys.
 *   events      - XtAppMainLoop drains an XtAppAddTimeOut without spin.
 *   callbacks   - XtAddCallback/XtCallCallbacks/XtRemoveCallback round-trip.
 *   gc          - XtAllocateGC returns a non-NULL GC and tolerates release.
 *   resources   - XtVaGetValues against an XtAppSetFallbackResources entry.
 *
 * The resources path drives the full XrmQGetSearchResource ->
 * XrmGetResource bridge introduced in src/xrm.c. If that bridge ever
 * regresses, the resources subtest is what catches it first.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>
#include <X11/keysym.h>

#define OK(name)          \
    do {                  \
        puts("ok " name); \
        fflush(stdout);   \
    } while (0)
#define FAIL(name, ...)                     \
    do {                                    \
        fprintf(stderr, "FAIL " name ": "); \
        fprintf(stderr, __VA_ARGS__);       \
        fputc('\n', stderr);                \
        exit(1);                            \
    } while (0)
#define MUST(cond, name, ...)        \
    do {                             \
        if (!(cond))                 \
            FAIL(name, __VA_ARGS__); \
    } while (0)

/* --------------------------------------------------------------- keysyms */

static void test_keysyms(void)
{
    KeySym lo, up;

    /* ASCII A: lower-cases to a, upper-cases to itself. */
    XConvertCase(XK_A, &lo, &up);
    MUST(lo == XK_a && up == XK_A, "keysyms",
         "XK_A -> lo=0x%lx up=0x%lx expected (a, A)", lo, up);

    /* ASCII z: upper-cases to Z. */
    XConvertCase(XK_z, &lo, &up);
    MUST(lo == XK_z && up == XK_Z, "keysyms",
         "XK_z -> lo=0x%lx up=0x%lx expected (z, Z)", lo, up);

    /* Function key has no case form. */
    XConvertCase(XK_F1, &lo, &up);
    MUST(lo == XK_F1 && up == XK_F1, "keysyms",
         "XK_F1 changed: lo=0x%lx up=0x%lx", lo, up);

    /* Latin-1: e-acute pairs with E-acute. */
    XConvertCase(XK_eacute, &lo, &up);
    MUST(lo == XK_eacute && up == XK_Eacute, "keysyms",
         "Latin-1 eacute -> lo=0x%lx up=0x%lx", lo, up);

    /* Greek capital alpha pairs with lower alpha. */
    XConvertCase(XK_Greek_ALPHA, &lo, &up);
    MUST(lo == XK_Greek_alpha && up == XK_Greek_ALPHA, "keysyms",
         "Greek_ALPHA -> lo=0x%lx up=0x%lx", lo, up);

    OK("keysyms");
}

/* ---------------------------------------------------------------- events */

static int tick_count = 0;
static XtAppContext tick_app = NULL;

static void tick_proc(XtPointer baton, XtIntervalId *id)
{
    (void) baton;
    (void) id;
    tick_count++;
    /* Setting the exit flag is checked by XtAppMainLoop after the current
     * callback returns, so this drops us out cleanly with no further
     * event processing. */
    XtAppSetExitFlag(tick_app);
}

static void test_events(XtAppContext app)
{
    tick_count = 0;
    tick_app = app;
    /* 10ms is short enough to keep make check fast but long enough that
     * the SDL dummy backend has a real interval to wait through. */
    XtAppAddTimeOut(app, 10, tick_proc, NULL);
    XtAppMainLoop(app);
    MUST(tick_count == 1, "events",
         "timeout fired %d times, expected exactly 1", tick_count);
    /* The exit flag is sticky inside XtAppMainLoop but does not reset; a
     * follow-up loop would return immediately. Confirm by asking. */
    MUST(XtAppGetExitFlag(app), "events",
         "XtAppGetExitFlag is False after XtAppSetExitFlag in callback");
    OK("events");
}

/* ------------------------------------------------------------- callbacks */

static int cb_calls = 0;
static XtPointer cb_last_call_data = (XtPointer) (-1);

static void cb_proc(Widget w, XtPointer client_data, XtPointer call_data)
{
    (void) w;
    (void) client_data;
    cb_calls++;
    cb_last_call_data = call_data;
}

static void test_callbacks(Widget shell)
{
    cb_calls = 0;
    cb_last_call_data = (XtPointer) (-1);

    XtAddCallback(shell, XtNdestroyCallback, cb_proc, (XtPointer) 0x1234);
    XtCallCallbacks(shell, XtNdestroyCallback, (XtPointer) 0xBEEF);
    MUST(cb_calls == 1, "callbacks", "first call fired %d times, expected 1",
         cb_calls);
    MUST(cb_last_call_data == (XtPointer) 0xBEEF, "callbacks",
         "call_data passthrough: got %p expected 0xBEEF", cb_last_call_data);

    /* Removing the same (proc, client_data) pair makes a follow-up
     * CallCallbacks a no-op. */
    XtRemoveCallback(shell, XtNdestroyCallback, cb_proc, (XtPointer) 0x1234);
    XtCallCallbacks(shell, XtNdestroyCallback, NULL);
    MUST(cb_calls == 1, "callbacks", "callback fired after remove (now %d)",
         cb_calls);

    OK("callbacks");
}

/* -------------------------------------------------------------------- gc */

static void test_gc(Widget shell)
{
    XGCValues values;
    memset(&values, 0, sizeof(values));
    values.foreground = 0x000000;
    values.background = 0xFFFFFF;

    GC gc1 = XtAllocateGC(shell, 0, GCForeground | GCBackground, &values, 0, 0);
    MUST(gc1 != NULL, "gc", "XtAllocateGC returned NULL");

    /* Same widget + same value mask + same values should hit the shared
     * GC cache. We do not strictly require pointer equality (libXt is
     * allowed to return a fresh GC), but the allocation must succeed. */
    GC gc2 = XtAllocateGC(shell, 0, GCForeground | GCBackground, &values, 0, 0);
    MUST(gc2 != NULL, "gc", "second XtAllocateGC returned NULL");

    /* XtReleaseGC must accept GCs allocated via XtAllocateGC without
     * crashing the GC cache bookkeeping. */
    XtReleaseGC(shell, gc1);
    XtReleaseGC(shell, gc2);

    OK("gc");
}

/* --------------------------------------------------------- resources */

/* Fallback resources are merged into the per-screen database the first
 * time XtScreenDatabase runs, then cached. Set them before XtOpenDisplay
 * in main() (not here) so the database we query is the one that includes
 * them. The class name "MicroLibxtTier1" is unique enough that no real
 * X11 app-defaults file is going to shadow it. */
static String fallback_resources[] = {
    "*microLibxtTier1.smokeProbeValue: ResourceBridgeOk",
    NULL,
};

static void test_resources(Widget shell)
{
    /* Look up the fallback through the screen-level database the same way
     * any Xt widget initialization would. This is the path Motif uses for
     * every widget resource and the one that crashes hardest when the
     * XrmQGetSearchResource bridge regresses. */
    XrmDatabase db = XtScreenDatabase(XtScreen(shell));
    MUST(db != NULL, "resources", "XtScreenDatabase NULL");

    char *type = NULL;
    XrmValue value;
    memset(&value, 0, sizeof(value));
    Bool found =
        XrmGetResource(db, "microLibxtTier1.smokeProbeValue",
                       "MicroLibxtTier1.SmokeProbeValue", &type, &value);
    MUST(found, "resources",
         "fallback resource not found through XtScreenDatabase");
    MUST(value.addr != NULL && value.size > 0, "resources",
         "fallback value pointer/size empty");
    MUST(
        strncmp((const char *) value.addr, "ResourceBridgeOk", value.size) == 0,
        "resources", "fallback value mismatch: got %.*s", (int) value.size,
        (const char *) value.addr);

    OK("resources");
}

/* ----------------------------------------------------------------- main */

int main(int argc, char *argv[])
{
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    /* Subsystems below this point all need a live application context. */
    XtAppContext app;
    int local_argc = 1;
    char *local_argv[] = {(char *) "test_libxt_micro", NULL};

    XtToolkitInitialize();
    app = XtCreateApplicationContext();
    MUST(app != NULL, "init", "XtCreateApplicationContext returned NULL");

    /* Fallbacks must be set before the first XtScreenDatabase build, which
     * happens transitively through XtAppCreateShell. Otherwise the cached
     * per-screen database skips the fallback merge entirely. */
    XtAppSetFallbackResources(app, fallback_resources);

    Display *dpy =
        XtOpenDisplay(app, NULL, "microLibxtTier1", "MicroLibxtTier1", NULL, 0,
                      &local_argc, local_argv);
    MUST(dpy != NULL, "init", "XtOpenDisplay returned NULL");

    Widget shell = XtAppCreateShell("microLibxtTier1", "MicroLibxtTier1",
                                    applicationShellWidgetClass, dpy, NULL, 0);
    MUST(shell != NULL, "init", "XtAppCreateShell returned NULL");

    XtVaSetValues(shell, XtNwidth, 64, XtNheight, 64, NULL);
    XtRealizeWidget(shell);
    MUST(XtIsRealized(shell), "init", "shell did not realize");

    test_keysyms();
    test_events(app);
    test_callbacks(shell);
    test_gc(shell);
    test_resources(shell);

    /* Hold off destroy until the shell-using tests finish so a callback
     * accidentally triggered during teardown does not corrupt cb_calls. */
    XtDestroyWidget(shell);
    XtCloseDisplay(dpy);
    XtDestroyApplicationContext(app);

    puts("test_libxt_micro: all subtests ok");
    (void) argc;
    (void) argv;
    return 0;
}
