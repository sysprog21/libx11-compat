/*
 * Interactive Motif menu app for validating menu input routing (PR-73 S2-S5)
 * under a real X server (Xvfb + a window manager). Unlike motif-pulldown-menu.c
 * this app installs no replay hook: it just builds the widget tree and runs the
 * event loop, so the SAME binary can be driven by xdotool against both system
 * libX11 (ground truth) and libx11-compat, and the two outcomes compared.
 *
 * Two top-levels, each with a File/Edit menu bar. File holds a few items plus a
 * cascade submenu ("Recent") with its own items, so a test can exercise arrow
 * traversal (S2), cross-window menu-bar activation (S3), and drag-through
 * cascade posting (S4/S5). Window ids are printed so a driver can track which
 * override-redirect menu shells map.
 */
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/PushB.h>
#include <stdio.h>
#include <string.h>

static void buildFileMenu(Widget menubar)
{
    Widget pulldown = XmCreatePulldownMenu(menubar, "filemenu", NULL, 0);
    Widget file =
        XtVaCreateManagedWidget("File", xmCascadeButtonWidgetClass, menubar,
                                XmNsubMenuId, pulldown, XmNmnemonic, 'F', NULL);
    XtVaCreateManagedWidget("New", xmPushButtonWidgetClass, pulldown, NULL);
    XtVaCreateManagedWidget("Open", xmPushButtonWidgetClass, pulldown, NULL);

    /* Cascade submenu for drag-through / sub-submenu tests. */
    Widget recent = XmCreatePulldownMenu(pulldown, "recentmenu", NULL, 0);
    XtVaCreateManagedWidget("Recent", xmCascadeButtonWidgetClass, pulldown,
                            XmNsubMenuId, recent, NULL);
    XtVaCreateManagedWidget("recent-1", xmPushButtonWidgetClass, recent, NULL);
    XtVaCreateManagedWidget("recent-2", xmPushButtonWidgetClass, recent, NULL);
    XtVaCreateManagedWidget("Quit", xmPushButtonWidgetClass, pulldown, NULL);
}

int main(int argc, char **argv)
{
    XtAppContext app;
    int oneTop = argc > 1 && strcmp(argv[1], "--one") == 0;
    Widget top1 = XtVaAppInitialize(&app, "MenuNav", NULL, 0, &argc, argv, NULL,
                                    XmNx, 40, XmNy, 40, XmNwidth, 360,
                                    XmNheight, 260, (char *) NULL);
    Widget main1 = XmCreateMainWindow(top1, "main1", NULL, 0);
    XtManageChild(main1);
    Widget bar1 = XmCreateMenuBar(main1, "bar1", NULL, 0);
    XtManageChild(bar1);
    buildFileMenu(bar1);
    Widget editpd = XmCreatePulldownMenu(bar1, "editmenu", NULL, 0);
    XtVaCreateManagedWidget("Edit", xmCascadeButtonWidgetClass, bar1,
                            XmNsubMenuId, editpd, XmNmnemonic, 'E', NULL);
    XtVaCreateManagedWidget("Cut", xmPushButtonWidgetClass, editpd, NULL);

    Widget top2 = NULL;
    if (!oneTop) {
        /* Second, independent top-level for the cross-window jump test (S3). */
        top2 = XtVaCreatePopupShell("MenuNav2", applicationShellWidgetClass,
                                    top1, XmNx, 460, XmNy, 40, XmNwidth, 360,
                                    XmNheight, 260, (char *) NULL);
        Widget main2 = XmCreateMainWindow(top2, "main2", NULL, 0);
        XtManageChild(main2);
        Widget bar2 = XmCreateMenuBar(main2, "bar2", NULL, 0);
        XtManageChild(bar2);
        buildFileMenu(bar2);
    }

    XtRealizeWidget(top1);
    if (top2)
        XtPopup(top2, XtGrabNone);

    printf("TOP1_WINDOW=0x%lx\n", (unsigned long) XtWindow(top1));
    if (top2)
        printf("TOP2_WINDOW=0x%lx\n", (unsigned long) XtWindow(top2));
    printf("MENU_NAV_READY\n");
    fflush(stdout);

    XtAppMainLoop(app);
    return 0;
}
