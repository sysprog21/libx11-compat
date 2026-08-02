/*
 * Browser idle-yield override for the compat layer's blocking event waits.
 *
 * An unmodified showcase app links this object, built with -sASYNCIFY. Its
 * constructor installs a hook (compatBrowserIdleYieldHook, defined in
 * src/events.c) that the blocking waits (XNextEvent, XWindowEvent, XIfEvent,
 * XMaskEvent, and everything funneling through them such as XtAppMainLoop) call
 * instead of SDL_Delay. emscripten_sleep(0) suspends to the browser event loop
 * and resumes one turn later, so the tab stays responsive without touching the
 * app source. It works only because ASYNCIFY (or JSPI) instrumented the stack,
 * which is why this lives in a separate object the app links rather than in the
 * core archive: the in-tree examples link the core without ASYNCIFY and must
 * not pull in emscripten_sleep.
 */
#include <emscripten.h>

extern void (*compatBrowserIdleYieldHook)(void);

static void browserIdleYield(void)
{
    emscripten_sleep(0);
}

__attribute__((constructor)) static void installBrowserIdleYield(void)
{
    compatBrowserIdleYieldHook = browserIdleYield;
}
