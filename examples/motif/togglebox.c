#include <stdio.h>
#include <stdlib.h>

#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/RowColumn.h>
#include <Xm/ToggleB.h>
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
    Widget shell = XtVaAppInitialize(&app, "MotifToggleBox", NULL, 0, &argc,
                                     argv, NULL, XmNallowShellResize, True,
                                     NULL);
    XtAppSetWarningHandler(app, warning_handler);
    XtAppSetErrorHandler(app, error_handler);

    Widget form = XmCreateForm(shell, (char *) "form", NULL, 0);
    Widget frame = XmCreateFrame(form, (char *) "frame", NULL, 0);
    XtVaSetValues(frame, XmNtopAttachment, XmATTACH_FORM, XmNleftAttachment,
                  XmATTACH_FORM, XmNrightAttachment, XmATTACH_FORM, NULL);

    Widget row = XmCreateRowColumn(frame, (char *) "choices", NULL, 0);
    XtVaSetValues(row, XmNorientation, XmHORIZONTAL, XmNpacking,
                  XmPACK_COLUMN, NULL);

    XtVaCreateManagedWidget("alpha", xmToggleButtonWidgetClass, row,
                            XmNset, True, NULL);
    XtVaCreateManagedWidget("beta", xmToggleButtonWidgetClass, row, NULL);
    XtVaCreateManagedWidget("gamma", xmToggleButtonWidgetClass, row, NULL);

    XmString caption = XmStringCreateLocalized((char *) "ToggleBox");
    Widget label = XtVaCreateManagedWidget("caption", xmLabelWidgetClass, form,
                                           XmNlabelString, caption,
                                           XmNtopAttachment, XmATTACH_WIDGET,
                                           XmNtopWidget, frame,
                                           XmNleftAttachment, XmATTACH_FORM,
                                           NULL);
    (void) label;
    XmStringFree(caption);

    XtManageChild(row);
    XtManageChild(frame);
    XtManageChild(form);
    XtRealizeWidget(shell);
    XtAppAddTimeOut(app, 100, exit_cb, app);
    XtAppMainLoop(app);
    XtDestroyWidget(shell);

    if (saw_xt_diagnostic)
        return 1;
    printf("motif-togglebox: ok\n");
    return 0;
}
