#ifndef SELECTION_H
#define SELECTION_H

#include <X11/Xlib.h>

void freeSelectionStorage(Display *display);
void resetSelectionAtomCache(void);

#endif /* SELECTION_H */
