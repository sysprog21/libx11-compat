/* libXt smoke test for libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* Exercise the path that proves libXt sits cleanly on libx11-compat:
 *
 *   1. XtToolkitInitialize     - one-shot Xt init, exits dirty if XtMalloc /
 *                                quark table machinery is broken.
 *   2. XtCreateApplicationContext + XtOpenDisplay - prove the Xt application
 *      context can drive XOpenDisplay under SDL_VIDEODRIVER=dummy.
 *   3. XtAppCreateShell                          - top-level widget creation
 *      hits the widget class machinery, resource manager, and translation
 *      table compiler. If any of those see the wrong Display layout, this
 *      either segfaults or returns NULL.
 *   4. XtRealizeWidget + XtUnrealizeWidget       - drives the realize path
 *      (XCreateWindow + class methods) without entering the event loop.
 *   5. Teardown is XtDestroyApplicationContext.
 *
 * The test does not enter XtAppMainLoop; the goal is link/init coverage, not
 * a steady-state UI session. Anything that returns NULL or 0 where success
 * is expected is fatal so make check fails loudly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/StringDefs.h>

int main(int argc, char *argv[])
{
    XtAppContext app;
    Widget shell;
    Display *display;
    int local_argc = 1;
    char *local_argv[] = {(char *) "test_libxt_link", NULL};

    if (!getenv("SDL_VIDEODRIVER")) {
        /* tests/check defaults to dummy via the make target. When a developer
         * runs the binary directly without that wrapper, force dummy so we do
         * not pop a real window. */
        setenv("SDL_VIDEODRIVER", "dummy", 1);
    }

    XtToolkitInitialize();
    app = XtCreateApplicationContext();
    if (!app) {
        fprintf(stderr, "XtCreateApplicationContext returned NULL\n");
        return 1;
    }

    display = XtOpenDisplay(app, NULL, "smoke", "Smoke", NULL, 0, &local_argc,
                            local_argv);
    if (!display) {
        fprintf(stderr, "XtOpenDisplay returned NULL\n");
        XtDestroyApplicationContext(app);
        return 1;
    }
    (void) argc;
    (void) argv;

    shell = XtAppCreateShell("smoke", "Smoke", applicationShellWidgetClass,
                             display, NULL, 0);
    if (!shell) {
        fprintf(stderr, "XtAppCreateShell returned NULL\n");
        XtCloseDisplay(display);
        XtDestroyApplicationContext(app);
        return 1;
    }

    /* Pin a size before realize so the shell does not refuse to map.
     * The sentinel for XtVaSetValues is a literal NULL, not a cast: the
     * cast hides the sentinel attribute from -Wsentinel and the va_list
     * walker reads past the end. */
    XtVaSetValues(shell, XtNwidth, 64, XtNheight, 64, NULL);

    XtRealizeWidget(shell);
    if (!XtIsRealized(shell)) {
        fprintf(stderr, "Shell did not realize\n");
        XtDestroyWidget(shell);
        XtCloseDisplay(display);
        XtDestroyApplicationContext(app);
        return 1;
    }

    XtUnrealizeWidget(shell);
    XtDestroyWidget(shell);
    XtCloseDisplay(display);
    XtDestroyApplicationContext(app);

    printf("test_libxt_link: ok\n");
    return 0;
}
