/* A FONT_IS_XFT render table must load a usable font. Before the fix libXm was
 * built --without-xft, so an XmFONT_IS_XFT rendition fell through to the
 * INVALID_TYPE path and loaded no font; text then measured to a degenerate
 * near-zero size, the "tiny crushed block" in the report. With libXm built
 * --with-xft against the in-tree Xft the rendition resolves and measures at a
 * legible height.
 */
#include <Xm/Xm.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    XtAppContext app;
    Widget top = XtVaAppInitialize(&app, "MotifXftRendition", NULL, 0, &argc,
                                   argv, NULL, (char *) NULL);
    if (!top) {
        fprintf(stderr, "no toplevel\n");
        return 2;
    }

    Arg args[8];
    int n = 0;
    XtSetArg(args[n], XmNfontName, (XtArgVal) "DejaVu Sans");
    n++;
    XtSetArg(args[n], XmNfontType, XmFONT_IS_XFT);
    n++;
    XtSetArg(args[n], XmNfontSize, 12);
    n++;
    XtSetArg(args[n], XmNfontStyle, (XtArgVal) "Bold");
    n++;
    XmRendition rend = XmRenditionCreate(top, "rt", args, n);
    XmRenderTable table =
        XmRenderTableAddRenditions(NULL, &rend, 1, XmMERGE_NEW);
    XmString str =
        XmStringGenerate((XtPointer) "Filesave", NULL, XmCHARSET_TEXT, "rt");

    Dimension w = 0, h = 0;
    XmStringExtent(table, str, &w, &h);
    printf("XmStringExtent w=%u h=%u\n", (unsigned) w, (unsigned) h);

    /* A 12pt bold face at ~96 DPI is ~16px tall; anything >= 10px proves a real
     * scalable font loaded rather than the zero-metric no-font fallback.
     */
    if (h >= 10 && w >= 20) {
        printf("XFT_FONT_OK\n");
        return 0;
    }
    printf("XFT_FONT_TINY_OR_MISSING\n");
    return 1;
}
