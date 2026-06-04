#include <stdio.h>
#include <stdlib.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <Xm/Label.h>
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
    Widget shell = XtVaAppInitialize(&app, "MotifHello", NULL, 0, &argc,
                                     argv, NULL, XmNallowShellResize, True,
                                     NULL);
    XtAppSetWarningHandler(app, warning_handler);
    XtAppSetErrorHandler(app, error_handler);

    Widget box = XmCreateRowColumn(shell, (char *) "box", NULL, 0);
    XmString label_text = XmStringCreateLocalized((char *) "Hello Motif");
    Widget label = XtVaCreateManagedWidget("label", xmLabelWidgetClass, box,
                                           XmNlabelString, label_text, NULL);
    Widget button = XtVaCreateManagedWidget("button", xmPushButtonWidgetClass,
                                            box, NULL);
    (void) label;
    (void) button;
    XmStringFree(label_text);

    XtManageChild(box);
    XtRealizeWidget(shell);
    XtAppAddTimeOut(app, 100, exit_cb, app);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);

    if (saw_xt_diagnostic)
        return 1;
    printf("motif-hello: ok\n");
    return 0;
}
