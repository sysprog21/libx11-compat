/*
 * WebAssembly idle-yield link smoke.
 *
 * Links a program that blocks in XNextEvent against the core archive plus the
 * yield override (compat/wasm-idle-yield.c) with -sASYNCIFY. A clean emcc link
 * proves the override resolves emscripten_sleep (which needs ASYNCIFY) and
 * supersedes the core's default NULL hook, so an unmodified showcase app that
 * blocks in the compat layer's event waits yields to the browser instead of
 * freezing the tab. Not executed: Emscripten's SDL2 port has no dummy video
 * driver, so XOpenDisplay needs a real browser canvas; this only exercises the
 * link.
 */
#include <X11/Xlib.h>

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (!display)
        return 1;
    XEvent event;
    /* Reaches the blocking idle wait, which calls the installed yield hook. */
    XNextEvent(display, &event);
    XCloseDisplay(display);
    return 0;
}
