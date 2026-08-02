/*
 * WebAssembly Motif link smoke.
 *
 * Links a minimal Motif program against the wasm libXm.a and libMrm.a on top of
 * the Xt + Xpm + Xmu + core archives. A clean emcc link proves the wasm Motif
 * archives are symbol-complete on the compat stack; a missing symbol fails the
 * link loudly. It is not executed: Emscripten's SDL2 port has no dummy video
 * driver, so a functional run needs a browser canvas (covered per app later).
 * MrmInitialize is referenced so libMrm is actually pulled in and validated
 * rather than dropped as an unreferenced archive.
 */
#include <X11/Intrinsic.h>
#include <Xm/Xm.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Mrm/MrmPublic.h>

int main(int argc, char **argv)
{
    XtAppContext app;
    Widget top = XtAppInitialize(&app, "WasmMotifSmoke", NULL, 0, &argc, argv,
                                 NULL, NULL, 0);
    Widget rc = XmCreateRowColumn(top, "rc", NULL, 0);
    Widget button = XmCreatePushButton(rc, "button", NULL, 0);
    XtManageChild(button);
    XtManageChild(rc);
    XtRealizeWidget(top);
    MrmInitialize();
    return 0;
}
