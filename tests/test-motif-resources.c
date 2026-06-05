#include <stdio.h>
#include <stdlib.h>

#include <X11/Intrinsic.h>
#include <Xm/Label.h>
#include <Xm/Xm.h>

static String fallback_resources[] = {
    "*XmLabel.fontList: fixed",
    NULL,
};

int main(int argc, char **argv)
{
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    XtAppContext app = NULL;
    int local_argc = argc;
    Widget shell =
        XtVaAppInitialize(&app, "MotifResources", NULL, 0, &local_argc, argv,
                          fallback_resources, NULL);
    if (!shell) {
        fprintf(stderr, "XtVaAppInitialize returned NULL\n");
        return 1;
    }

    Widget label = XmCreateLabel(shell, (char *) "label", NULL, 0);
    if (!label) {
        fprintf(stderr, "XmCreateLabel returned NULL\n");
        XtDestroyWidget(shell);
        return 1;
    }

    XtManageChild(label);
    XtRealizeWidget(shell);

    XmFontList font_list = NULL;
    XtVaGetValues(label, XmNfontList, &font_list, NULL);
    if (!font_list) {
        fprintf(stderr,
                "XmNfontList stayed NULL despite *XmLabel.fontList fallback\n");
        XtDestroyWidget(shell);
        return 1;
    }

    Pixel background = 0;
    XtVaGetValues(label, XmNbackground, &background, NULL);
    if (background != 0xffc4c4c4ul) {
        fprintf(stderr,
                "XmNbackground default was 0x%08lx, expected 0xffc4c4c4\n",
                (unsigned long) background);
        XtDestroyWidget(shell);
        return 1;
    }

    XtDestroyWidget(shell);
    puts("test_motif_resources: ok");
    return 0;
}
