#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/Xutil.h>
#include <Xm/Label.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/Xm.h>

#include "window.h"

static void exit_cb(XtPointer client_data, XtIntervalId *id)
{
    (void) id;
    XtAppSetExitFlag((XtAppContext) client_data);
}

static int image_has_visible_pixels(XImage *image)
{
    if (!image || image->bits_per_pixel != 32)
        return 0;

    for (int y = 0; y < image->height; y++) {
        const uint32_t *row =
            (const uint32_t *) (image->data + y * image->bytes_per_line);
        for (int x = 0; x < image->width; x++) {
            if (row[x] != 0)
                return 1;
        }
    }
    return 0;
}

static int surface_has_visible_pixels(SDL_Surface *surface)
{
    if (!surface || surface->format->BytesPerPixel != 4)
        return 0;

    if (SDL_LockSurface(surface) != 0)
        return 0;
    int visible = 0;
    for (int y = 0; y < surface->h && !visible; y++) {
        const uint32_t *row =
            (const uint32_t *) ((const char *) surface->pixels +
                                y * surface->pitch);
        for (int x = 0; x < surface->w; x++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(row[x], surface->format, &r, &g, &b, &a);
            if (r != 0 || g != 0 || b != 0 || a != 0) {
                visible = 1;
                break;
            }
        }
    }
    SDL_UnlockSurface(surface);
    return visible;
}

static int button_activations = 0;

static int looks_like_allocated_xid(Window window)
{
    return (uintptr_t) window > 4096;
}

static void activate_cb(Widget widget,
                        XtPointer client_data,
                        XtPointer call_data)
{
    (void) widget;
    (void) client_data;
    (void) call_data;
    button_activations++;
}

static int dispatch_pending(XtAppContext app)
{
    int dispatched = 0;
    while (XtAppPending(app)) {
        XEvent event;
        XtAppNextEvent(app, &event);
        XtDispatchEvent(&event);
        dispatched++;
    }
    return dispatched;
}

static Widget sdl_shell_for_widget(Widget widget)
{
    for (Widget current = widget; current; current = XtParent(current)) {
        if (!XtIsRealized(current))
            continue;
        Window window = XtWindow(current);
        if (looks_like_allocated_xid(window) && IS_TYPE(window, WINDOW) &&
            GET_WINDOW_STRUCT(window)->sdlWindow) {
            return current;
        }
    }
    return NULL;
}

static int click_widget(XtAppContext app, Widget target)
{
    if (!target)
        return 0;

    for (int i = 0; i < 50 && !XtIsRealized(target); i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    if (!XtIsRealized(target))
        return 0;

    Widget eventTarget = target;
    Window targetWindow = XtWindow(eventTarget);
    int localX = 0, localY = 0;
    while ((!looks_like_allocated_xid(targetWindow) ||
            !IS_TYPE(targetWindow, WINDOW)) &&
           XtParent(eventTarget)) {
        Position x = 0, y = 0;
        XtVaGetValues(eventTarget, XmNx, &x, XmNy, &y, NULL);
        localX += x;
        localY += y;
        eventTarget = XtParent(eventTarget);
        targetWindow = XtWindow(eventTarget);
    }

    Widget shell = sdl_shell_for_widget(eventTarget);
    if (!shell)
        return 0;

    Display *display = XtDisplay(shell);
    Window shellWindow = XtWindow(shell);
    if (shellWindow == None || targetWindow == None ||
        !looks_like_allocated_xid(shellWindow) ||
        !looks_like_allocated_xid(targetWindow) ||
        !IS_TYPE(shellWindow, WINDOW) || !IS_TYPE(targetWindow, WINDOW))
        return 0;

    Dimension width = 0, height = 0;
    XtVaGetValues(target, XmNwidth, &width, XmNheight, &height, NULL);
    int shellX = 0, shellY = 0;
    Window child = None;
    if (!XTranslateCoordinates(
            display, targetWindow, shellWindow, localX + (int) width / 2,
            localY + (int) height / 2, &shellX, &shellY, &child)) {
        return 0;
    }

    Uint32 windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(shellWindow)->sdlWindow);
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.windowID = windowID;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = shellX;
    event.button.y = shellY;
    SDL_PushEvent(&event);

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.windowID = windowID;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = shellX;
    event.button.y = shellY;
    SDL_PushEvent(&event);

    for (int i = 0; i < 20 && button_activations == 0; i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    return button_activations > 0;
}

int main(int argc, char **argv)
{
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    XtAppContext app;
    int local_argc = argc;
    Widget shell = XtAppInitialize(&app, "MotifSmoke", NULL, 0, &local_argc,
                                   argv, NULL, NULL, 0);
    if (!shell) {
        fprintf(stderr, "XtAppInitialize returned NULL\n");
        return 1;
    }

    XmString text = XmStringCreateLocalized((char *) "hello");
    Widget label = XmCreateLabel(shell, (char *) "label", NULL, 0);
    if (!text || !label) {
        fprintf(stderr, "failed to create Motif label resources\n");
        if (text)
            XmStringFree(text);
        XtDestroyWidget(shell);
        return 1;
    }

    XtVaSetValues(label, XmNlabelString, text, NULL);
    XmStringFree(text);
    XtManageChild(label);
    XtRealizeWidget(shell);

    XtAppAddTimeOut(app, 100, exit_cb, app);
    XtAppMainLoop(app);

    Display *display = XtDisplay(shell);
    Window window = XtWindow(shell);
    XSync(display, False);
    XImage *image =
        XGetImage(display, window, 0, 0, 32, 32, AllPlanes, ZPixmap);
    if (!image_has_visible_pixels(image)) {
        fprintf(stderr, "Motif label rendered an all-zero framebuffer\n");
        if (image)
            XDestroyImage(image);
        XtDestroyWidget(shell);
        return 1;
    }
    XDestroyImage(image);

    SDL_Surface *surface =
        SDL_GetWindowSurface(GET_WINDOW_STRUCT(window)->sdlWindow);
    if (!surface_has_visible_pixels(surface)) {
        fprintf(stderr,
                "Motif label presented an all-zero SDL window surface\n");
        XtDestroyWidget(shell);
        return 1;
    }

    XmString buttonText = XmStringCreateLocalized((char *) "Done");
    Widget button = XmCreatePushButton(shell, (char *) "done", NULL, 0);
    if (!button || !buttonText) {
        fprintf(stderr, "failed to create Motif push button resources\n");
        if (buttonText)
            XmStringFree(buttonText);
        XtDestroyWidget(shell);
        return 1;
    }
    XtVaSetValues(button, XmNlabelString, buttonText, NULL);
    XmStringFree(buttonText);
    XtAddCallback(button, XmNactivateCallback, activate_cb, NULL);
    XtUnmanageChild(label);
    XtManageChild(button);
    dispatch_pending(app);
    XSync(display, False);
    if (!click_widget(app, button)) {
        fprintf(stderr, "Motif push button did not activate from SDL click\n");
        XtDestroyWidget(shell);
        return 1;
    }

    button_activations = 0;
    XmString message =
        XmStringCreateLocalized((char *) "Select a language, Verify your OS");
    XmString done = XmStringCreateLocalized((char *) "Done");
    Arg args[4];
    int n = 0;
    XtSetArg(args[n], XmNmessageString, message);
    n++;
    XtSetArg(args[n], XmNokLabelString, done);
    n++;
    Widget dialog =
        XmCreateTemplateDialog(shell, (char *) "search_box", args, n);
    if (message)
        XmStringFree(message);
    if (done)
        XmStringFree(done);
    if (!dialog) {
        fprintf(stderr, "failed to create Motif template dialog\n");
        XtDestroyWidget(shell);
        return 1;
    }
    XtAddCallback(dialog, XmNokCallback, activate_cb, NULL);
    Widget ok = XtNameToWidget(dialog, (char *) "OK");
    XtManageChild(dialog);
    XtPopup(XtParent(dialog), XtGrabNone);
    for (int i = 0; i < 20 && !XtIsRealized(ok); i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    XSync(display, False);
    if (!click_widget(app, ok)) {
        fprintf(stderr,
                "Motif template dialog OK button did not activate from SDL "
                "click\n");
        XtDestroyWidget(shell);
        return 1;
    }

    XtDestroyWidget(shell);
    printf("test_motif_link: ok\n");
    return 0;
}
