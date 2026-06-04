#include <stdio.h>
#include <stdlib.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <Xm/CascadeB.h>
#include <Xm/Label.h>
#include <Xm/MainW.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/Xm.h>

static int saw_xt_diagnostic = 0;

static void warning_handler(String message)
{
    saw_xt_diagnostic = 1;
    fprintf(stderr, "XtWarning: %s\n", message ? message : "(null)");
}

static void error_handler(String message) __attribute__((noreturn));
static void error_handler(String message)
{
    saw_xt_diagnostic = 1;
    fprintf(stderr, "XtError: %s\n", message ? message : "(null)");
    exit(2);
}

static void exit_cb(XtPointer client_data, XtIntervalId *id)
{
    (void) id;
    XtAppSetExitFlag((XtAppContext) client_data);
}

int main(int argc, char **argv)
{
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    XtAppContext app;
    Widget shell = XtVaAppInitialize(&app, "MotifSimpleApp", NULL, 0, &argc,
                                     argv, NULL, XmNallowShellResize, True,
                                     NULL);
    XtAppSetWarningHandler(app, warning_handler);
    XtAppSetErrorHandler(app, error_handler);

    Widget main_window = XmCreateMainWindow(shell, (char *) "main", NULL, 0);
    Widget menu_bar = XmCreateMenuBar(main_window, (char *) "menuBar", NULL, 0);
    Widget menu = XmCreatePulldownMenu(menu_bar, (char *) "fileMenu", NULL, 0);
    Widget cascade = XtVaCreateManagedWidget("File", xmCascadeButtonWidgetClass,
                                             menu_bar, XmNsubMenuId, menu, NULL);
    Widget quit = XtVaCreateManagedWidget("Quit", xmPushButtonWidgetClass,
                                          menu, NULL);
    (void) cascade;
    (void) quit;

    XmString text = XmStringCreateLocalized((char *) "Simple Motif App");
    Widget label = XtVaCreateManagedWidget("content", xmLabelWidgetClass,
                                           main_window, XmNlabelString, text,
                                           NULL);
    XmStringFree(text);

    XtManageChild(menu_bar);
    XtManageChild(label);
    XtManageChild(main_window);
    XmMainWindowSetAreas(main_window, menu_bar, NULL, NULL, NULL, label);

    XtRealizeWidget(shell);
    XtAppAddTimeOut(app, 100, exit_cb, app);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);

    if (saw_xt_diagnostic)
        return 1;
    printf("motif-simpleapp: ok\n");
    return 0;
}
