#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xaw/Command.h>
#include <stdlib.h>
#include <unistd.h>

static void quit_callback(XtPointer client_data, XtIntervalId *id)
{
    (void) id;
    int *done = (int *) client_data;
    *done = 1;
}

int main(int argc, char **argv)
{
    XtAppContext app;
    Widget top = XtAppInitialize(&app, "XawLinkTest", NULL, 0, &argc, argv,
                                 NULL, NULL, 0);
    if (!top)
        return 1;
    Widget button = XtVaCreateManagedWidget("ok", commandWidgetClass, top,
                                            XtNlabel, "OK", NULL);
    if (!button)
        return 1;
    XtRealizeWidget(top);
    int done = 0;
    XtAppAddTimeOut(app, 1, quit_callback, &done);
    for (int i = 0; i < 1000 && !done; i++) {
        while (XtAppPending(app))
            XtAppProcessEvent(app, XtIMAll);
        usleep(1000);
    }
    XtDestroyWidget(top);
    return done ? 0 : 1;
}
