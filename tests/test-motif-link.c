#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/Xutil.h>
#include <Xm/CascadeB.h>
#include <Xm/Label.h>
#include <Xm/MainW.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
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
static int menu_item_presses = 0;
static int menu_item_releases = 0;
static int menu_item_motions = 0;
static int menu_item_enters = 0;
static int menu_item_arms = 0;
static int menu_item_disarms = 0;
static unsigned int menu_item_last_press_state = 0;
static unsigned int menu_item_last_release_state = 0;
static unsigned int menu_item_last_button = 0;
static Time menu_item_last_press_time = 0;
static Time menu_item_last_release_time = 0;
static int menu_item_last_x = 0;
static int menu_item_last_y = 0;

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

static void menu_arm_cb(Widget widget,
                        XtPointer client_data,
                        XtPointer call_data)
{
    (void) widget;
    (void) client_data;
    (void) call_data;
    menu_item_arms++;
}

static void menu_disarm_cb(Widget widget,
                           XtPointer client_data,
                           XtPointer call_data)
{
    (void) widget;
    (void) client_data;
    (void) call_data;
    menu_item_disarms++;
}

static void menu_item_event_cb(Widget widget,
                               XtPointer client_data,
                               XEvent *event,
                               Boolean *continue_to_dispatch)
{
    (void) widget;
    (void) client_data;
    (void) continue_to_dispatch;
    if (event->type == ButtonPress)
        menu_item_presses++;
    else if (event->type == ButtonRelease)
        menu_item_releases++;
    else if (event->type == MotionNotify)
        menu_item_motions++;
    else if (event->type == EnterNotify)
        menu_item_enters++;
    if (event->type == ButtonPress || event->type == ButtonRelease) {
        menu_item_last_button = event->xbutton.button;
        menu_item_last_x = event->xbutton.x;
        menu_item_last_y = event->xbutton.y;
        if (event->type == ButtonPress) {
            menu_item_last_press_state = event->xbutton.state;
            menu_item_last_press_time = event->xbutton.time;
        } else {
            menu_item_last_release_state = event->xbutton.state;
            menu_item_last_release_time = event->xbutton.time;
        }
    }
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

static int widget_center_in_sdl_window(Widget target,
                                       Uint32 *windowID,
                                       int *shellX,
                                       int *shellY)
{
    if (!target)
        return 0;

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
    Window child = None;
    if (!XTranslateCoordinates(
            display, targetWindow, shellWindow, localX + (int) width / 2,
            localY + (int) height / 2, shellX, shellY, &child)) {
        return 0;
    }

    *windowID = SDL_GetWindowID(GET_WINDOW_STRUCT(shellWindow)->sdlWindow);
    return 1;
}

static int click_widget_until(XtAppContext app, Widget target, int *counter)
{
    /* Generous wait windows: tests run under CI alongside the differential
     * screenshot harness, where Xt realization can take 50-100ms under load.
     * Tighter 20-50ms loops occasionally flaked.
     */
    for (int i = 0; i < 200 && target && !XtIsRealized(target); i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }

    Uint32 windowID = 0;
    int shellX = 0, shellY = 0;
    if (!widget_center_in_sdl_window(target, &windowID, &shellX, &shellY))
        return 0;

    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.windowID = windowID;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = shellX;
    event.button.y = shellY;
    SDL_PushEvent(&event);

    for (int i = 0; i < 10; i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.windowID = windowID;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = shellX;
    event.button.y = shellY;
    SDL_PushEvent(&event);

    if (counter) {
        for (int i = 0; i < 200 && *counter == 0; i++) {
            dispatch_pending(app);
            SDL_Delay(1);
        }
        return *counter > 0;
    }
    /* No counter to wait on: drain just enough cycles to deliver the button-up
     * and let the caller assert the real post-condition (e.g. XtIsRealized on
     * the popped menu).
     */
    for (int i = 0; i < 10; i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    return 1;
}

static int click_widget(XtAppContext app, Widget target)
{
    return click_widget_until(app, target, &button_activations);
}

static int move_pointer_to_widget(XtAppContext app, Widget target)
{
    Uint32 windowID = 0;
    int shellX = 0, shellY = 0;
    if (!widget_center_in_sdl_window(target, &windowID, &shellX, &shellY))
        return 0;

    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.windowID = windowID;
    event.motion.state = 0;
    event.motion.x = shellX;
    event.motion.y = shellY;
    SDL_PushEvent(&event);

    for (int i = 0; i < 100; i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    return 1;
}

static int menu_click_post_select(XtAppContext app, Widget cascade, Widget item)
{
    /* The cascade has no observable Xt callback the test can wait on; the
     * XtIsRealized(item) check below serves as the real post-condition.
     */
    if (!click_widget_until(app, cascade, NULL)) {
        fprintf(stderr, "Motif Help cascade had no SDL-backed center point\n");
        return 0;
    }
    for (int i = 0; i < 200 && !XtIsRealized(item); i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    if (!XtIsRealized(item)) {
        fprintf(stderr, "Motif Help menu item was not realized after click\n");
        return 0;
    }
    SDL_Delay(500);
    dispatch_pending(app);
    if (!move_pointer_to_widget(app, item)) {
        fprintf(stderr, "Motif Help menu item did not accept pointer motion\n");
        return 0;
    }
    return click_widget(app, item);
}

static int menu_drag_select(XtAppContext app, Widget cascade, Widget item)
{
    Uint32 cascadeWindowID = 0;
    int cascadeX = 0, cascadeY = 0;
    if (!widget_center_in_sdl_window(cascade, &cascadeWindowID, &cascadeX,
                                     &cascadeY)) {
        fprintf(stderr, "Motif Help cascade has no SDL-backed center point\n");
        return 0;
    }

    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.windowID = cascadeWindowID;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = cascadeX;
    event.button.y = cascadeY;
    SDL_PushEvent(&event);

    for (int i = 0; i < 200 && !XtIsRealized(item); i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    dispatch_pending(app);
    if (!XtIsRealized(item)) {
        fprintf(stderr, "Motif Help menu item was not realized after press\n");
        return 0;
    }

    Uint32 itemWindowID = 0;
    int itemX = 0, itemY = 0;
    if (!widget_center_in_sdl_window(item, &itemWindowID, &itemX, &itemY)) {
        fprintf(stderr,
                "Motif Help menu item has no SDL-backed center point\n");
        return 0;
    }

    SDL_zero(event);
    event.type = SDL_MOUSEMOTION;
    event.motion.windowID = itemWindowID;
    event.motion.state = SDL_BUTTON_LMASK;
    event.motion.x = itemX;
    event.motion.y = itemY;
    SDL_PushEvent(&event);

    for (int i = 0; i < 100 && menu_item_arms == 0; i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }

    SDL_zero(event);
    event.type = SDL_MOUSEBUTTONUP;
    event.button.windowID = itemWindowID;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = itemX;
    event.button.y = itemY;
    SDL_PushEvent(&event);

    for (int i = 0; i < 200 && button_activations == 0; i++) {
        dispatch_pending(app);
        SDL_Delay(1);
    }
    if (button_activations == 0) {
        XWindowAttributes attrs;
        memset(&attrs, 0, sizeof(attrs));
        int mapped =
            XGetWindowAttributes(XtDisplay(item), XtWindow(item), &attrs)
                ? attrs.map_state
                : -1;
        fprintf(stderr,
                "Motif Help menu item realized=%d mapped=%d activation=%d "
                "press=%d release=%d motion=%d enter=%d\n",
                XtIsRealized(item), mapped, button_activations,
                menu_item_presses, menu_item_releases, menu_item_motions,
                menu_item_enters);
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

    XtUnmanageChild(button);

    Widget mainWindow =
        XmCreateMainWindow(shell, (char *) "mainWindow", NULL, 0);
    Widget menuBar =
        mainWindow ? XmCreateMenuBar(mainWindow, (char *) "menuBar", NULL, 0)
                   : NULL;
    Widget helpMenu =
        menuBar ? XmCreatePulldownMenu(menuBar, (char *) "helpMenu", NULL, 0)
                : NULL;
    Widget helpCascade =
        menuBar ? XmCreateCascadeButton(menuBar, (char *) "Help", NULL, 0)
                : NULL;
    Widget helpItem =
        helpMenu ? XmCreatePushButton(helpMenu, (char *) "Help Item", NULL, 0)
                 : NULL;
    XmString helpLabel = XmStringCreateLocalized((char *) "Help");
    XmString helpItemLabel = XmStringCreateLocalized((char *) "Help Item");
    if (!mainWindow || !menuBar || !helpMenu || !helpCascade || !helpItem ||
        !helpLabel || !helpItemLabel) {
        fprintf(stderr, "failed to create Motif Help menu resources\n");
        if (helpLabel)
            XmStringFree(helpLabel);
        if (helpItemLabel)
            XmStringFree(helpItemLabel);
        XtDestroyWidget(shell);
        return 1;
    }

    XtVaSetValues(helpCascade, XmNlabelString, helpLabel, XmNsubMenuId,
                  helpMenu, NULL);
    XtVaSetValues(helpItem, XmNlabelString, helpItemLabel, NULL);
    XtVaSetValues(menuBar, XmNmenuHelpWidget, helpCascade, NULL);
    XmStringFree(helpLabel);
    XmStringFree(helpItemLabel);
    XtAddCallback(helpItem, XmNactivateCallback, activate_cb, NULL);
    XtAddCallback(helpItem, XmNarmCallback, menu_arm_cb, NULL);
    XtAddCallback(helpItem, XmNdisarmCallback, menu_disarm_cb, NULL);
    XtAddEventHandler(helpItem,
                      ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                          EnterWindowMask,
                      False, menu_item_event_cb, NULL);
    XtManageChild(helpItem);
    XtManageChild(helpCascade);
    XtManageChild(menuBar);
    XmMainWindowSetAreas(mainWindow, menuBar, NULL, NULL, NULL, NULL);
    XtManageChild(mainWindow);
    dispatch_pending(app);
    XSync(display, False);

    button_activations = 0;
    menu_item_presses = 0;
    menu_item_releases = 0;
    menu_item_motions = 0;
    menu_item_enters = 0;
    menu_item_arms = 0;
    menu_item_disarms = 0;
    menu_item_last_press_time = 0;
    menu_item_last_release_time = 0;
    if (!menu_click_post_select(app, helpCascade, helpItem)) {
        fprintf(stderr,
                "Motif Help menu item did not activate after click-post "
                "press=%d release=%d motion=%d enter=%d arm=%d disarm=%d "
                "button=%u "
                "press_state=0x%x release_state=0x%x time=(%lu,%lu) "
                "xy=(%d,%d)\n",
                menu_item_presses, menu_item_releases, menu_item_motions,
                menu_item_enters, menu_item_arms, menu_item_disarms,
                menu_item_last_button, menu_item_last_press_state,
                menu_item_last_release_state,
                (unsigned long) menu_item_last_press_time,
                (unsigned long) menu_item_last_release_time, menu_item_last_x,
                menu_item_last_y);
        XtDestroyWidget(shell);
        return 1;
    }

    button_activations = 0;
    menu_item_presses = 0;
    menu_item_releases = 0;
    menu_item_motions = 0;
    menu_item_enters = 0;
    menu_item_arms = 0;
    menu_item_disarms = 0;
    if (!menu_drag_select(app, helpCascade, helpItem)) {
        fprintf(stderr,
                "Motif Help menu item did not activate from menu flow "
                "press=%d release=%d motion=%d enter=%d arm=%d disarm=%d "
                "button=%u press_state=0x%x release_state=0x%x "
                "time=(%lu,%lu) xy=(%d,%d)\n",
                menu_item_presses, menu_item_releases, menu_item_motions,
                menu_item_enters, menu_item_arms, menu_item_disarms,
                menu_item_last_button, menu_item_last_press_state,
                menu_item_last_release_state,
                (unsigned long) menu_item_last_press_time,
                (unsigned long) menu_item_last_release_time, menu_item_last_x,
                menu_item_last_y);
        XtDestroyWidget(shell);
        return 1;
    }

    XtUnmanageChild(mainWindow);

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
    for (int i = 0; i < 200 && !XtIsRealized(ok); i++) {
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
