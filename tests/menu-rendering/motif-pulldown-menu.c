/* A real Motif drop-down menu must appear anchored under its menu-bar item, not
 * as a free-standing window. This builds an actual XmMenuBar with an
 * XmPulldownMenu (the "File" menu), posts it programmatically, and lets the
 * XmMenuShell map. Run under WAYLAND_DEBUG: the posted menu shell must be an
 * xdg_popup parented to the main window's xdg_surface, not a second
 * xdg_toplevel. This exercises Motif's own menu-posting path (XmMenuShell,
 * override-redirect, absolute position), unlike the raw-Xlib popup-anchor
 * proxy.
 */
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/PushB.h>
#include <stdio.h>
#include <unistd.h>

static void pump(XtAppContext app, int ticks)
{
    for (int i = 0; i < ticks; i++) {
        while (XtAppPending(app))
            XtAppProcessEvent(app, XtIMAll);
        usleep(20000);
    }
}

int main(int argc, char **argv)
{
    XtAppContext app;
    Widget top = XtVaAppInitialize(&app, "MotifPulldown", NULL, 0, &argc, argv,
                                   NULL, (char *) NULL);
    XtVaSetValues(top, XmNwidth, 400, XmNheight, 300, NULL);

    Widget mainw = XmCreateMainWindow(top, "mainw", NULL, 0);
    XtManageChild(mainw);
    Widget menubar = XmCreateMenuBar(mainw, "menubar", NULL, 0);
    XtManageChild(menubar);
    Widget pulldown = XmCreatePulldownMenu(menubar, "filemenu", NULL, 0);
    Widget cascade =
        XtVaCreateManagedWidget("File", xmCascadeButtonWidgetClass, menubar,
                                XmNsubMenuId, pulldown, NULL);
    XtVaCreateManagedWidget("Open", xmPushButtonWidgetClass, pulldown, NULL);
    XtVaCreateManagedWidget("Quit", xmPushButtonWidgetClass, pulldown, NULL);

    XtRealizeWidget(top);

    /* The File menu is posted by a real Button1 press over the cascade button.
     * Driving that headlessly needs Motif's menu-bar grab to see genuine
     * pointer events, so the click is injected by the in-process replay engine
     * (LIBX11_COMPAT_REPLAY) that the harness points at this binary, exactly
     * the path the XNEdit smoke uses. When run without a replay this is a no-op
     * (nothing posts), so the harness always drives it through the replay. Pump
     * long enough for the replay's delayed click to land and the XmMenuShell to
     * map as an override-redirect popup.
     */
    (void) cascade;
    pump(app, 250);
    printf("MOTIF_PULLDOWN_DONE\n");
    return 0;
}
